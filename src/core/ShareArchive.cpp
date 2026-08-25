#include "core/ShareArchive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lwe::core {
namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50U;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50U;
constexpr std::uint32_t kEndRecordSignature = 0x06054b50U;
constexpr std::uint16_t kUtf8NameFlag = 0x0800U;
constexpr std::uint16_t kStoredMethod = 0;
constexpr std::uint16_t kZipVersion = 20;
constexpr std::uint16_t kMaximumEntries = 4096;
constexpr std::uint64_t kMaximumArchivePayload = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr DWORD kCopyBufferBytes = 1024U * 1024U;

#pragma pack(push, 1)
struct LocalFileHeader final {
    std::uint32_t signature = kLocalHeaderSignature;
    std::uint16_t versionNeeded = kZipVersion;
    std::uint16_t flags = kUtf8NameFlag;
    std::uint16_t compressionMethod = kStoredMethod;
    std::uint16_t modificationTime = 0;
    std::uint16_t modificationDate = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint16_t fileNameLength = 0;
    std::uint16_t extraFieldLength = 0;
};

struct CentralDirectoryHeader final {
    std::uint32_t signature = kCentralHeaderSignature;
    std::uint16_t versionMadeBy = kZipVersion;
    std::uint16_t versionNeeded = kZipVersion;
    std::uint16_t flags = kUtf8NameFlag;
    std::uint16_t compressionMethod = kStoredMethod;
    std::uint16_t modificationTime = 0;
    std::uint16_t modificationDate = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint16_t fileNameLength = 0;
    std::uint16_t extraFieldLength = 0;
    std::uint16_t fileCommentLength = 0;
    std::uint16_t diskNumberStart = 0;
    std::uint16_t internalAttributes = 0;
    std::uint32_t externalAttributes = 0;
    std::uint32_t localHeaderOffset = 0;
};

struct EndOfCentralDirectory final {
    std::uint32_t signature = kEndRecordSignature;
    std::uint16_t diskNumber = 0;
    std::uint16_t centralDirectoryDisk = 0;
    std::uint16_t entriesOnDisk = 0;
    std::uint16_t totalEntries = 0;
    std::uint32_t centralDirectorySize = 0;
    std::uint32_t centralDirectoryOffset = 0;
    std::uint16_t commentLength = 0;
};
#pragma pack(pop)

static_assert(sizeof(LocalFileHeader) == 30);
static_assert(sizeof(CentralDirectoryHeader) == 46);
static_assert(sizeof(EndOfCentralDirectory) == 22);

struct WrittenEntry final {
    std::string nameUtf8;
    std::uint32_t crc32 = 0;
    std::uint32_t size = 0;
    std::uint32_t localHeaderOffset = 0;
    std::uint16_t modificationTime = 0;
    std::uint16_t modificationDate = 0;
};

HRESULT LastErrorResult() {
    return HRESULT_FROM_WIN32(GetLastError());
}

HRESULT ReadExact(const HANDLE file, void* destination, const std::size_t bytes) {
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < bytes) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(file, output + offset, request, &read, nullptr)) {
            return LastErrorResult();
        }
        if (read == 0) {
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        offset += read;
    }
    return S_OK;
}

HRESULT WriteExact(const HANDLE file, const void* source, const std::size_t bytes) {
    const auto* input = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0;
    while (offset < bytes) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, input + offset, request, &written, nullptr)) {
            return LastErrorResult();
        }
        if (written == 0) {
            return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        offset += written;
    }
    return S_OK;
}

HRESULT SeekAbsolute(const HANDLE file, const std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
               ? S_OK
               : LastErrorResult();
}

HRESULT CurrentOffset(const HANDLE file, std::uint64_t& offset) {
    LARGE_INTEGER movement{};
    LARGE_INTEGER position{};
    if (!SetFilePointerEx(file, movement, &position, FILE_CURRENT)) {
        return LastErrorResult();
    }
    if (position.QuadPart < 0) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    offset = static_cast<std::uint64_t>(position.QuadPart);
    return S_OK;
}

HRESULT FileSize(const HANDLE file, std::uint64_t& size) {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(file, &value)) {
        return LastErrorResult();
    }
    if (value.QuadPart < 0) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    size = static_cast<std::uint64_t>(value.QuadPart);
    return S_OK;
}

std::optional<std::string> WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return std::nullopt;
    }
    std::string converted(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), converted.data(), count,
                            nullptr, nullptr) != count) {
        return std::nullopt;
    }
    return converted;
}

std::optional<std::wstring> Utf8ToWide(const std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (count <= 0) {
        return std::nullopt;
    }
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), converted.data(), count) !=
        count) {
        return std::nullopt;
    }
    return converted;
}

bool SafeEntryName(const std::wstring_view name) {
    if (name.empty() || name.size() > 240 || name == L"." || name == L".." ||
        std::filesystem::path(name).filename().native() != name ||
        _wcsicmp(std::filesystem::path(name).extension().c_str(), L".lwewall") != 0) {
        return false;
    }
    constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
    return std::ranges::none_of(name, [&](const wchar_t character) {
        return character < 0x20 ||
               forbidden.find(character) != std::wstring_view::npos;
    });
}

std::wstring OrdinalKey(std::wstring value) {
    if (!value.empty()) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

std::uint32_t UpdateCrc32(std::uint32_t crc,
                          const std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^
                  (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

HRESULT ComputeCrc32(const HANDLE input, const std::uint64_t bytes,
                     std::uint32_t& crc32) {
    std::vector<std::uint8_t> buffer(kCopyBufferBytes);
    std::uint64_t remaining = bytes;
    std::uint32_t crc = 0xffffffffU;
    while (remaining > 0) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), request, &read, nullptr)) {
            return LastErrorResult();
        }
        if (read == 0) {
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        crc = UpdateCrc32(crc,
                          std::span<const std::uint8_t>(buffer.data(), read));
        remaining -= read;
    }
    crc32 = crc ^ 0xffffffffU;
    return S_OK;
}

HRESULT CopyBytes(const HANDLE input, const HANDLE output,
                  const std::uint64_t bytes, std::uint32_t* crc32 = nullptr) {
    std::vector<std::uint8_t> buffer(kCopyBufferBytes);
    std::uint64_t remaining = bytes;
    std::uint32_t crc = 0xffffffffU;
    while (remaining > 0) {
        const DWORD request = static_cast<DWORD>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), request, &read, nullptr)) {
            return LastErrorResult();
        }
        if (read == 0) {
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        HRESULT result = WriteExact(output, buffer.data(), read);
        if (FAILED(result)) {
            return result;
        }
        if (crc32 != nullptr) {
            crc = UpdateCrc32(
                crc, std::span<const std::uint8_t>(buffer.data(), read));
        }
        remaining -= read;
    }
    if (crc32 != nullptr) {
        *crc32 = crc ^ 0xffffffffU;
    }
    return S_OK;
}

void CurrentDosTimestamp(std::uint16_t& time, std::uint16_t& date) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const WORD year = std::clamp<WORD>(now.wYear, 1980, 2107);
    time = static_cast<std::uint16_t>((now.wHour << 11U) |
                                      (now.wMinute << 5U) |
                                      (now.wSecond / 2U));
    date = static_cast<std::uint16_t>(((year - 1980U) << 9U) |
                                      (now.wMonth << 5U) | now.wDay);
}

}  // namespace

HRESULT CreateShareArchive(const std::span<const ShareArchiveEntry> entries,
                           const std::wstring_view destinationPath) {
    if (entries.empty() || entries.size() > kMaximumEntries ||
        destinationPath.empty()) {
        return E_INVALIDARG;
    }

    const std::filesystem::path destination(destinationPath);
    const std::filesystem::path temporary = destination.native() + L".tmp";
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return LastErrorResult();
    }

    std::vector<WrittenEntry> writtenEntries;
    std::unordered_set<std::wstring> names;
    std::uint64_t totalPayload = 0;
    HRESULT result = S_OK;
    for (const ShareArchiveEntry& entry : entries) {
        if (!SafeEntryName(entry.entryName) ||
            !names.insert(OrdinalKey(entry.entryName)).second) {
            result = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
            break;
        }
        const std::optional nameUtf8 = WideToUtf8(entry.entryName);
        if (!nameUtf8.has_value() || nameUtf8->empty() ||
            nameUtf8->size() > std::numeric_limits<std::uint16_t>::max()) {
            result = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
            break;
        }

        HANDLE input = CreateFileW(entry.sourcePath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            result = LastErrorResult();
            break;
        }
        std::uint64_t sourceBytes = 0;
        result = FileSize(input, sourceBytes);
        if (SUCCEEDED(result) &&
            (sourceBytes == 0 ||
             sourceBytes > std::numeric_limits<std::uint32_t>::max() ||
             totalPayload + sourceBytes > kMaximumArchivePayload)) {
            result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
        std::uint32_t crc32 = 0;
        if (SUCCEEDED(result)) {
            result = ComputeCrc32(input, sourceBytes, crc32);
        }
        if (SUCCEEDED(result)) {
            result = SeekAbsolute(input, 0);
        }

        std::uint64_t headerOffset = 0;
        if (SUCCEEDED(result)) {
            result = CurrentOffset(output, headerOffset);
        }
        if (SUCCEEDED(result) &&
            headerOffset > std::numeric_limits<std::uint32_t>::max()) {
            result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }

        LocalFileHeader header{};
        header.crc32 = crc32;
        header.compressedSize = static_cast<std::uint32_t>(sourceBytes);
        header.uncompressedSize = static_cast<std::uint32_t>(sourceBytes);
        header.fileNameLength = static_cast<std::uint16_t>(nameUtf8->size());
        CurrentDosTimestamp(header.modificationTime, header.modificationDate);
        if (SUCCEEDED(result)) {
            result = WriteExact(output, &header, sizeof(header));
        }
        if (SUCCEEDED(result)) {
            result = WriteExact(output, nameUtf8->data(), nameUtf8->size());
        }
        if (SUCCEEDED(result)) {
            result = CopyBytes(input, output, sourceBytes);
        }
        CloseHandle(input);
        if (FAILED(result)) {
            break;
        }

        writtenEntries.push_back(WrittenEntry{
            *nameUtf8, crc32, static_cast<std::uint32_t>(sourceBytes),
            static_cast<std::uint32_t>(headerOffset), header.modificationTime,
            header.modificationDate});
        totalPayload += sourceBytes;
    }

    std::uint64_t centralOffset = 0;
    if (SUCCEEDED(result)) {
        result = CurrentOffset(output, centralOffset);
    }
    if (SUCCEEDED(result) &&
        centralOffset > std::numeric_limits<std::uint32_t>::max()) {
        result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    for (const WrittenEntry& entry : writtenEntries) {
        if (FAILED(result)) {
            break;
        }
        CentralDirectoryHeader central{};
        central.modificationTime = entry.modificationTime;
        central.modificationDate = entry.modificationDate;
        central.crc32 = entry.crc32;
        central.compressedSize = entry.size;
        central.uncompressedSize = entry.size;
        central.fileNameLength =
            static_cast<std::uint16_t>(entry.nameUtf8.size());
        central.localHeaderOffset = entry.localHeaderOffset;
        result = WriteExact(output, &central, sizeof(central));
        if (SUCCEEDED(result)) {
            result = WriteExact(output, entry.nameUtf8.data(),
                                entry.nameUtf8.size());
        }
    }

    std::uint64_t endOffset = 0;
    if (SUCCEEDED(result)) {
        result = CurrentOffset(output, endOffset);
    }
    const std::uint64_t centralSize =
        endOffset >= centralOffset ? endOffset - centralOffset : 0;
    if (SUCCEEDED(result) &&
        centralSize > std::numeric_limits<std::uint32_t>::max()) {
        result = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    if (SUCCEEDED(result)) {
        EndOfCentralDirectory end{};
        end.entriesOnDisk = static_cast<std::uint16_t>(writtenEntries.size());
        end.totalEntries = end.entriesOnDisk;
        end.centralDirectorySize = static_cast<std::uint32_t>(centralSize);
        end.centralDirectoryOffset = static_cast<std::uint32_t>(centralOffset);
        result = WriteExact(output, &end, sizeof(end));
    }
    if (SUCCEEDED(result) && !FlushFileBuffers(output)) {
        result = LastErrorResult();
    }
    CloseHandle(output);

    if (SUCCEEDED(result) &&
        !MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = LastErrorResult();
    }
    if (FAILED(result)) {
        DeleteFileW(temporary.c_str());
    }
    return result;
}

HRESULT ExtractShareArchive(
    const std::wstring_view archivePath,
    const std::filesystem::path& outputDirectory,
    std::vector<std::filesystem::path>& extractedFiles) {
    extractedFiles.clear();
    if (archivePath.empty() || outputDirectory.empty()) {
        return E_INVALIDARG;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);
    if (directoryError) {
        return HRESULT_FROM_WIN32(directoryError.value());
    }

    HANDLE input = CreateFileW(std::wstring(archivePath).c_str(), GENERIC_READ,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return LastErrorResult();
    }
    std::uint64_t archiveBytes = 0;
    HRESULT result = FileSize(input, archiveBytes);
    EndOfCentralDirectory end{};
    if (SUCCEEDED(result) && archiveBytes >= sizeof(end)) {
        result = SeekAbsolute(input, archiveBytes - sizeof(end));
    } else if (SUCCEEDED(result)) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    if (SUCCEEDED(result)) {
        result = ReadExact(input, &end, sizeof(end));
    }
    if (FAILED(result) || end.signature != kEndRecordSignature ||
        end.diskNumber != 0 || end.centralDirectoryDisk != 0 ||
        end.entriesOnDisk == 0 || end.entriesOnDisk != end.totalEntries ||
        end.totalEntries > kMaximumEntries || end.commentLength != 0 ||
        static_cast<std::uint64_t>(end.centralDirectoryOffset) +
                end.centralDirectorySize + sizeof(end) !=
            archiveBytes) {
        CloseHandle(input);
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    result = SeekAbsolute(input, 0);
    std::unordered_set<std::wstring> names;
    std::uint64_t totalPayload = 0;
    for (std::uint16_t index = 0;
         SUCCEEDED(result) && index < end.totalEntries; ++index) {
        std::uint64_t position = 0;
        result = CurrentOffset(input, position);
        if (FAILED(result) || position >= end.centralDirectoryOffset) {
            result = FAILED(result) ? result
                                    : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            break;
        }

        LocalFileHeader header{};
        result = ReadExact(input, &header, sizeof(header));
        if (FAILED(result) || header.signature != kLocalHeaderSignature ||
            header.versionNeeded > kZipVersion ||
            (header.flags & ~kUtf8NameFlag) != 0 ||
            header.compressionMethod != kStoredMethod ||
            header.compressedSize == 0 ||
            header.compressedSize != header.uncompressedSize ||
            header.fileNameLength == 0 || header.extraFieldLength != 0 ||
            totalPayload + header.uncompressedSize > kMaximumArchivePayload) {
            result = FAILED(result) ? result
                                    : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            break;
        }

        std::string nameUtf8(header.fileNameLength, '\0');
        result = ReadExact(input, nameUtf8.data(), nameUtf8.size());
        const std::optional name = Utf8ToWide(nameUtf8);
        if (FAILED(result) || !name.has_value() || !SafeEntryName(*name) ||
            !names.insert(OrdinalKey(*name)).second) {
            result = FAILED(result) ? result
                                    : HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
            break;
        }

        std::uint64_t payloadOffset = 0;
        result = CurrentOffset(input, payloadOffset);
        if (FAILED(result) ||
            payloadOffset + header.compressedSize >
                end.centralDirectoryOffset) {
            result = FAILED(result) ? result
                                    : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            break;
        }

        const std::filesystem::path destination = outputDirectory / *name;
        HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL |
                                        FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
        if (output == INVALID_HANDLE_VALUE) {
            result = LastErrorResult();
            break;
        }
        std::uint32_t crc32 = 0;
        result = CopyBytes(input, output, header.compressedSize, &crc32);
        if (SUCCEEDED(result) && crc32 != header.crc32) {
            result = HRESULT_FROM_WIN32(ERROR_CRC);
        }
        if (SUCCEEDED(result) && !FlushFileBuffers(output)) {
            result = LastErrorResult();
        }
        CloseHandle(output);
        if (FAILED(result)) {
            DeleteFileW(destination.c_str());
            break;
        }
        extractedFiles.push_back(destination);
        totalPayload += header.uncompressedSize;
    }

    std::uint64_t finalPosition = 0;
    if (SUCCEEDED(result)) {
        result = CurrentOffset(input, finalPosition);
    }
    if (SUCCEEDED(result) &&
        (extractedFiles.size() != end.totalEntries ||
         finalPosition != end.centralDirectoryOffset)) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    CloseHandle(input);

    if (FAILED(result)) {
        for (const std::filesystem::path& extracted : extractedFiles) {
            DeleteFileW(extracted.c_str());
        }
        extractedFiles.clear();
    }
    return result;
}

}  // namespace lwe::core
