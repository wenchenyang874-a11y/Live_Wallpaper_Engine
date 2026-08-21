#include "core/WallpaperLibrary.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <bcrypt.h>
#include <shlobj.h>

#include "core/Logger.h"
#include "media/MediaProbe.h"

namespace lwe::core {
namespace {

constexpr std::array<std::uint8_t, 8> kPackageMagic{'L', 'W', 'E', 'W', 'A', 'L', 'L', '1'};
constexpr std::uint32_t kPackageVersion = 1;
constexpr std::uint32_t kMaximumNameBytes = 1024;
constexpr std::uint64_t kMaximumPayloadBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr DWORD kIoBufferBytes = 1024 * 1024;

#pragma pack(push, 1)
struct PackageHeader final {
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t wallpaperKind = 0;
    std::uint32_t nameBytes = 0;
    std::uint64_t payloadBytes = 0;
    std::array<std::uint8_t, 32> sha256{};
};
#pragma pack(pop)

static_assert(sizeof(PackageHeader) == 60);

class Sha256 final {
public:
    ~Sha256() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    HRESULT Initialize() {
        NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                                      nullptr, 0);
        if (!BCRYPT_SUCCESS(status)) {
            return HRESULT_FROM_NT(status);
        }

        DWORD objectBytes = 0;
        DWORD copied = 0;
        status = BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectBytes),
                                   sizeof(objectBytes), &copied, 0);
        if (!BCRYPT_SUCCESS(status) || objectBytes == 0) {
            return HRESULT_FROM_NT(status);
        }

        object_.resize(objectBytes);
        status = BCryptCreateHash(algorithm_, &hash_, object_.data(), objectBytes, nullptr, 0,
                                  0);
        return BCRYPT_SUCCESS(status) ? S_OK : HRESULT_FROM_NT(status);
    }

    HRESULT Update(const std::span<const std::uint8_t> bytes) const {
        if (hash_ == nullptr || bytes.size() > std::numeric_limits<ULONG>::max()) {
            return E_INVALIDARG;
        }
        const NTSTATUS status = BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()),
                                               static_cast<ULONG>(bytes.size()), 0);
        return BCRYPT_SUCCESS(status) ? S_OK : HRESULT_FROM_NT(status);
    }

    HRESULT Finish(std::array<std::uint8_t, 32>& digest) const {
        if (hash_ == nullptr) {
            return E_UNEXPECTED;
        }
        const NTSTATUS status =
            BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0);
        return BCRYPT_SUCCESS(status) ? S_OK : HRESULT_FROM_NT(status);
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<std::uint8_t> object_;
};

HRESULT LastErrorResult() {
    return HRESULT_FROM_WIN32(GetLastError());
}

HRESULT ReadExact(const HANDLE file, void* destination, const std::size_t byteCount) {
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < byteCount) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            byteCount - offset, std::numeric_limits<DWORD>::max()));
        DWORD received = 0;
        if (!ReadFile(file, output + offset, request, &received, nullptr)) {
            return LastErrorResult();
        }
        if (received == 0) {
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        offset += received;
    }
    return S_OK;
}

HRESULT WriteExact(const HANDLE file, const void* source, const std::size_t byteCount) {
    const auto* input = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0;
    while (offset < byteCount) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            byteCount - offset, std::numeric_limits<DWORD>::max()));
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

std::optional<std::string> WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 0) {
        return std::nullopt;
    }
    std::string utf8(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), utf8.data(), count, nullptr,
                            nullptr) != count) {
        return std::nullopt;
    }
    return utf8;
}

std::optional<std::wstring> Utf8ToWide(const std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), count) != count) {
        return std::nullopt;
    }
    return wide;
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

HRESULT HashAndCopyPayload(const HANDLE input, const HANDLE output,
                           const std::uint64_t bytesToCopy,
                           std::array<std::uint8_t, 32>& digest) {
    Sha256 hash;
    HRESULT result = hash.Initialize();
    if (FAILED(result)) {
        return result;
    }

    std::vector<std::uint8_t> buffer(kIoBufferBytes);
    std::uint64_t remaining = bytesToCopy;
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

        result = hash.Update(std::span<const std::uint8_t>(buffer.data(), read));
        if (FAILED(result)) {
            return result;
        }
        if (output != INVALID_HANDLE_VALUE) {
            result = WriteExact(output, buffer.data(), read);
            if (FAILED(result)) {
                return result;
            }
        }
        remaining -= read;
    }
    return hash.Finish(digest);
}

bool IsSafePackageFileName(const std::wstring_view name) {
    if (name.empty() || name.size() > 240 || name == L"." || name == L"..") {
        return false;
    }
    constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
    for (const wchar_t character : name) {
        if (character < 0x20 || forbidden.find(character) != std::wstring_view::npos) {
            return false;
        }
    }
    return std::filesystem::path(name).filename().native() == name;
}

HRESULT ResolveLibraryRoot(std::filesystem::path& root) {
    PWSTR localAppData = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (FAILED(result) || localAppData == nullptr) {
        CoTaskMemFree(localAppData);
        return FAILED(result) ? result : E_UNEXPECTED;
    }
    root = std::filesystem::path(localAppData) / L"LiveWallpaperEngine" / L"library";
    CoTaskMemFree(localAppData);
    return S_OK;
}

}  // namespace

HRESULT WallpaperLibrary::Initialize() {
    std::filesystem::path rootDirectory;
    const HRESULT result = ResolveLibraryRoot(rootDirectory);
    if (FAILED(result)) {
        return result;
    }

    return InitializeAt(std::move(rootDirectory));
}

HRESULT WallpaperLibrary::InitializeAt(std::filesystem::path rootDirectory) {
    if (rootDirectory.empty() || !rootDirectory.is_absolute()) {
        return E_INVALIDARG;
    }
    rootDirectory_ = std::move(rootDirectory);

    std::error_code error;
    std::filesystem::create_directories(rootDirectory_, error);
    if (error) {
        return HRESULT_FROM_WIN32(error.value());
    }
    return S_OK;
}

std::vector<WallpaperItem> WallpaperLibrary::Scan() const {
    std::vector<WallpaperItem> items;
    if (rootDirectory_.empty()) {
        return items;
    }

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(rootDirectory_, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error) || error) {
            error.clear();
            continue;
        }

        const std::wstring extension = entry.path().extension().native();
        if (_wcsicmp(extension.c_str(), L".lwewall") == 0 ||
            _wcsicmp(extension.c_str(), L".tmp") == 0 ||
            extension.find(L".importing") != std::wstring::npos) {
            continue;
        }

        WallpaperItem item;
        if (SUCCEEDED(DescribeFile(entry.path(), false, item))) {
            items.push_back(std::move(item));
        }
    }

    std::ranges::sort(items, [](const WallpaperItem& left, const WallpaperItem& right) {
        return CompareStringOrdinal(left.displayName.c_str(), -1, right.displayName.c_str(),
                                    -1, TRUE) == CSTR_LESS_THAN;
    });
    return items;
}

HRESULT WallpaperLibrary::DescribeFile(const std::filesystem::path& path,
                                       const bool external, WallpaperItem& item) const {
    media::MediaInfo mediaInfo;
    HRESULT result = media::ProbeMediaFile(path.native(), mediaInfo);
    if (FAILED(result)) {
        return result;
    }

    std::error_code error;
    const std::uint64_t size = std::filesystem::file_size(path, error);
    if (error) {
        return HRESULT_FROM_WIN32(error.value());
    }

    item.path = path;
    item.displayName = path.filename().native();
    item.kind = mediaInfo.kind;
    item.formatLabel = mediaInfo.formatLabel;
    item.fileSize = size;
    item.width = mediaInfo.width;
    item.height = mediaInfo.height;
    item.hasAudio = mediaInfo.hasAudio;
    item.external = external;
    return S_OK;
}

std::filesystem::path WallpaperLibrary::UniqueDestination(
    const std::wstring_view fileName) const {
    const std::filesystem::path original(fileName);
    std::filesystem::path candidate = rootDirectory_ / original.filename();
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) {
        return candidate;
    }

    const std::wstring stem = original.stem().native();
    const std::wstring extension = original.extension().native();
    for (std::uint32_t index = 2; index < 100000; ++index) {
        candidate = rootDirectory_ /
                    (stem + L" (" + std::to_wstring(index) + L")" + extension);
        error.clear();
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

HRESULT WallpaperLibrary::ImportFile(const std::wstring_view sourcePath,
                                     WallpaperItem& imported) const {
    if (rootDirectory_.empty() || sourcePath.empty()) {
        return E_UNEXPECTED;
    }

    const std::filesystem::path source(sourcePath);
    WallpaperItem sourceItem;
    HRESULT result = DescribeFile(source, true, sourceItem);
    if (FAILED(result)) {
        return result;
    }

    const std::filesystem::path destination = UniqueDestination(source.filename().native());
    if (destination.empty()) {
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    }
    const std::filesystem::path temporary = destination.native() + L".importing";

    if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
        return LastErrorResult();
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        result = LastErrorResult();
        DeleteFileW(temporary.c_str());
        return result;
    }

    result = DescribeFile(destination, false, imported);
    if (FAILED(result)) {
        DeleteFileW(destination.c_str());
        return result;
    }
    LogInfo(L"Imported a wallpaper into the local library: " + destination.native());
    return S_OK;
}

HRESULT WallpaperLibrary::ExportPackage(const WallpaperItem& item,
                                        const std::wstring_view destinationPath) const {
    if (item.path.empty() || destinationPath.empty()) {
        return E_INVALIDARG;
    }

    const std::optional nameUtf8 = WideToUtf8(item.path.filename().native());
    if (!nameUtf8.has_value() || nameUtf8->empty() ||
        nameUtf8->size() > kMaximumNameBytes) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    }

    HANDLE source = CreateFileW(item.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        return LastErrorResult();
    }

    std::uint64_t sourceBytes = 0;
    HRESULT result = FileSize(source, sourceBytes);
    if (FAILED(result) || sourceBytes > kMaximumPayloadBytes) {
        CloseHandle(source);
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    PackageHeader header{};
    header.magic = kPackageMagic;
    header.version = kPackageVersion;
    header.wallpaperKind = static_cast<std::uint32_t>(item.kind);
    header.nameBytes = static_cast<std::uint32_t>(nameUtf8->size());
    header.payloadBytes = sourceBytes;

    result = HashAndCopyPayload(source, INVALID_HANDLE_VALUE, sourceBytes, header.sha256);
    LARGE_INTEGER start{};
    if (SUCCEEDED(result) && !SetFilePointerEx(source, start, nullptr, FILE_BEGIN)) {
        result = LastErrorResult();
    }
    if (FAILED(result)) {
        CloseHandle(source);
        return result;
    }

    const std::filesystem::path destination(destinationPath);
    const std::filesystem::path temporary = destination.native() + L".tmp";
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(source);
        return LastErrorResult();
    }

    result = WriteExact(output, &header, sizeof(header));
    if (SUCCEEDED(result)) {
        result = WriteExact(output, nameUtf8->data(), nameUtf8->size());
    }
    std::array<std::uint8_t, 32> copiedDigest{};
    if (SUCCEEDED(result)) {
        result = HashAndCopyPayload(source, output, sourceBytes, copiedDigest);
    }
    if (SUCCEEDED(result) && copiedDigest != header.sha256) {
        result = HRESULT_FROM_WIN32(ERROR_CRC);
    }
    if (SUCCEEDED(result) && !FlushFileBuffers(output)) {
        result = LastErrorResult();
    }
    CloseHandle(output);
    CloseHandle(source);

    if (SUCCEEDED(result) &&
        !MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = LastErrorResult();
    }
    if (FAILED(result)) {
        DeleteFileW(temporary.c_str());
        return result;
    }

    LogInfo(L"Exported a shareable wallpaper package: " + destination.native());
    return S_OK;
}

HRESULT WallpaperLibrary::ImportPackage(const std::wstring_view packagePath,
                                        WallpaperItem& imported) const {
    if (rootDirectory_.empty() || packagePath.empty()) {
        return E_UNEXPECTED;
    }

    HANDLE input = CreateFileW(std::wstring(packagePath).c_str(), GENERIC_READ,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return LastErrorResult();
    }

    std::uint64_t packageBytes = 0;
    PackageHeader header{};
    HRESULT result = FileSize(input, packageBytes);
    if (SUCCEEDED(result)) {
        result = ReadExact(input, &header, sizeof(header));
    }
    if (FAILED(result) || header.magic != kPackageMagic ||
        header.version != kPackageVersion || header.nameBytes == 0 ||
        header.nameBytes > kMaximumNameBytes || header.payloadBytes == 0 ||
        header.payloadBytes > kMaximumPayloadBytes ||
        packageBytes != sizeof(header) + header.nameBytes + header.payloadBytes ||
        header.wallpaperKind < static_cast<std::uint32_t>(media::WallpaperKind::StaticImage) ||
        header.wallpaperKind > static_cast<std::uint32_t>(media::WallpaperKind::Video)) {
        CloseHandle(input);
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::string nameUtf8(header.nameBytes, '\0');
    result = ReadExact(input, nameUtf8.data(), nameUtf8.size());
    const std::optional fileName = Utf8ToWide(nameUtf8);
    if (FAILED(result) || !fileName.has_value() || !IsSafePackageFileName(*fileName)) {
        CloseHandle(input);
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    }

    const std::filesystem::path destination = UniqueDestination(*fileName);
    if (destination.empty()) {
        CloseHandle(input);
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (L"." + destination.stem().native() + L".importing" +
         destination.extension().native());
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        return LastErrorResult();
    }

    std::array<std::uint8_t, 32> payloadDigest{};
    result = HashAndCopyPayload(input, output, header.payloadBytes, payloadDigest);
    if (SUCCEEDED(result) && payloadDigest != header.sha256) {
        result = HRESULT_FROM_WIN32(ERROR_CRC);
    }
    if (SUCCEEDED(result) && !FlushFileBuffers(output)) {
        result = LastErrorResult();
    }
    CloseHandle(output);
    CloseHandle(input);

    WallpaperItem extracted;
    if (SUCCEEDED(result)) {
        result = DescribeFile(temporary, false, extracted);
    }
    if (SUCCEEDED(result) &&
        extracted.kind != static_cast<media::WallpaperKind>(header.wallpaperKind)) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    if (SUCCEEDED(result) &&
        !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        result = LastErrorResult();
    }
    if (FAILED(result)) {
        DeleteFileW(temporary.c_str());
        return result;
    }

    result = DescribeFile(destination, false, imported);
    if (FAILED(result)) {
        DeleteFileW(destination.c_str());
        return result;
    }
    LogInfo(L"Imported and verified a wallpaper package: " + destination.native());
    return S_OK;
}

const std::filesystem::path& WallpaperLibrary::RootDirectory() const noexcept {
    return rootDirectory_;
}

}  // namespace lwe::core
