#include "core/WallpaperLibrarySelfTest.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <string>

#include <windows.h>

#include "core/Logger.h"
#include "core/WallpaperGroupStore.h"
#include "core/WallpaperLibrary.h"

namespace lwe::core {
namespace {

constexpr std::uint64_t kPackageNameOffset = 60;
constexpr std::uint64_t kZipEntryNameOffset = 30;

HRESULT LastErrorResult() {
    return HRESULT_FROM_WIN32(GetLastError());
}

HRESULT CreateTemporaryDirectory(std::filesystem::path& directory) {
    std::array<wchar_t, MAX_PATH> temporaryRoot{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(temporaryRoot.size()),
                                      temporaryRoot.data());
    if (length == 0 || length >= temporaryRoot.size()) {
        return LastErrorResult();
    }

    std::array<wchar_t, MAX_PATH> temporaryFile{};
    if (GetTempFileNameW(temporaryRoot.data(), L"LWE", 0, temporaryFile.data()) == 0) {
        return LastErrorResult();
    }
    if (!DeleteFileW(temporaryFile.data()) ||
        !CreateDirectoryW(temporaryFile.data(), nullptr)) {
        return LastErrorResult();
    }
    directory = temporaryFile.data();
    return S_OK;
}

bool FilesEqual(const std::filesystem::path& left,
                const std::filesystem::path& right) {
    std::error_code error;
    const auto leftSize = std::filesystem::file_size(left, error);
    if (error) {
        return false;
    }
    const auto rightSize = std::filesystem::file_size(right, error);
    if (error || leftSize != rightSize) {
        return false;
    }

    HANDLE leftFile = CreateFileW(left.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    HANDLE rightFile = CreateFileW(right.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (leftFile == INVALID_HANDLE_VALUE || rightFile == INVALID_HANDLE_VALUE) {
        if (leftFile != INVALID_HANDLE_VALUE) {
            CloseHandle(leftFile);
        }
        if (rightFile != INVALID_HANDLE_VALUE) {
            CloseHandle(rightFile);
        }
        return false;
    }

    std::array<std::uint8_t, 64 * 1024> leftBuffer{};
    std::array<std::uint8_t, 64 * 1024> rightBuffer{};
    bool equal = true;
    while (true) {
        DWORD leftRead = 0;
        DWORD rightRead = 0;
        if (!ReadFile(leftFile, leftBuffer.data(), static_cast<DWORD>(leftBuffer.size()),
                      &leftRead, nullptr) ||
            !ReadFile(rightFile, rightBuffer.data(),
                      static_cast<DWORD>(rightBuffer.size()), &rightRead, nullptr) ||
            leftRead != rightRead ||
            !std::equal(leftBuffer.begin(), leftBuffer.begin() + leftRead,
                        rightBuffer.begin())) {
            equal = false;
            break;
        }
        if (leftRead == 0) {
            break;
        }
    }
    CloseHandle(rightFile);
    CloseHandle(leftFile);
    return equal;
}

HRESULT ModifyByte(const std::filesystem::path& path, const std::uint64_t offset,
                   const bool fromEnd) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return LastErrorResult();
    }
    LARGE_INTEGER position{};
    position.QuadPart = fromEnd ? -1 : static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, fromEnd ? FILE_END : FILE_BEGIN)) {
        const HRESULT result = LastErrorResult();
        CloseHandle(file);
        return result;
    }
    std::uint8_t value = 0;
    DWORD transferred = 0;
    if (!ReadFile(file, &value, 1, &transferred, nullptr) || transferred != 1) {
        const HRESULT result = LastErrorResult();
        CloseHandle(file);
        return result;
    }
    position.QuadPart = -1;
    if (!SetFilePointerEx(file, position, nullptr, FILE_CURRENT)) {
        const HRESULT result = LastErrorResult();
        CloseHandle(file);
        return result;
    }
    value = fromEnd ? static_cast<std::uint8_t>(value ^ 0xffU)
                    : static_cast<std::uint8_t>('/');
    if (!WriteFile(file, &value, 1, &transferred, nullptr) || transferred != 1 ||
        !FlushFileBuffers(file)) {
        const HRESULT result = LastErrorResult();
        CloseHandle(file);
        return result;
    }
    CloseHandle(file);
    return S_OK;
}

}  // namespace

int RunWallpaperLibrarySelfTest(const std::wstring_view sourcePath) {
    std::filesystem::path temporaryRoot;
    HRESULT result = CreateTemporaryDirectory(temporaryRoot);
    if (FAILED(result)) {
        LogError(L"Library self-test could not create its temporary directory.", result);
        return 1;
    }

    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove_all(temporaryRoot, error);
        if (error) {
            LogWarning(L"Library self-test left its temporary directory: " +
                       temporaryRoot.native());
        }
    };

    WallpaperLibrary sourceLibrary;
    WallpaperLibrary destinationLibrary;
    WallpaperLibrary archiveDestinationLibrary;
    WallpaperItem importedSource;
    WallpaperItem importedPackage;
    WallpaperItem secondSource;
    const std::filesystem::path package = temporaryRoot / L"shared.lwewall";
    const std::filesystem::path corrupted = temporaryRoot / L"corrupted.lwewall";
    const std::filesystem::path unsafeName = temporaryRoot / L"unsafe-name.lwewall";
    const std::filesystem::path archive = temporaryRoot / L"shared.zip";
    const std::filesystem::path corruptedArchive =
        temporaryRoot / L"corrupted.zip";
    const std::filesystem::path unsafeArchive =
        temporaryRoot / L"unsafe-entry.zip";

    result = sourceLibrary.InitializeAt(temporaryRoot / L"source-library");
    if (SUCCEEDED(result)) {
        result = destinationLibrary.InitializeAt(temporaryRoot / L"destination-library");
    }
    if (SUCCEEDED(result)) {
        result = archiveDestinationLibrary.InitializeAt(
            temporaryRoot / L"archive-destination-library");
    }
    if (SUCCEEDED(result)) {
        result = sourceLibrary.ImportFile(sourcePath, importedSource);
    }
    if (SUCCEEDED(result)) {
        WallpaperItem renamed;
        result = sourceLibrary.Rename(importedSource, L"renamed wallpaper", renamed);
        if (SUCCEEDED(result) &&
            (renamed.path.stem().native() != L"renamed wallpaper" ||
             std::filesystem::exists(importedSource.path))) {
            result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        if (SUCCEEDED(result)) {
            importedSource = std::move(renamed);
            LogInfo(L"SELF_TEST_LIBRARY_RENAME=True");
        }
    }
    if (SUCCEEDED(result)) {
        result = sourceLibrary.ExportPackage(importedSource, package.native());
    }
    if (SUCCEEDED(result)) {
        result = destinationLibrary.ImportPackage(package.native(), importedPackage);
    }
    if (FAILED(result) || !FilesEqual(importedSource.path, importedPackage.path) ||
        importedSource.kind != importedPackage.kind) {
        LogError(L"Library package round-trip self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_CRC));
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_PACKAGE_ROUNDTRIP=True");

    if (!CopyFileW(package.c_str(), corrupted.c_str(), TRUE) ||
        FAILED(ModifyByte(corrupted, 0, true)) ||
        SUCCEEDED(destinationLibrary.ImportPackage(corrupted.native(), importedPackage))) {
        LogError(L"Library package corruption self-test failed.");
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_CORRUPTION_REJECTED=True");

    if (!CopyFileW(package.c_str(), unsafeName.c_str(), TRUE) ||
        FAILED(ModifyByte(unsafeName, kPackageNameOffset, false)) ||
        SUCCEEDED(destinationLibrary.ImportPackage(unsafeName.native(), importedPackage))) {
        LogError(L"Library unsafe package-name self-test failed.");
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_UNSAFE_NAME_REJECTED=True");

    if (SUCCEEDED(result)) {
        result = sourceLibrary.ImportFile(sourcePath, secondSource);
    }
    std::vector<WallpaperItem> reordered = sourceLibrary.Scan();
    if (SUCCEEDED(result) && reordered.size() == 2) {
        std::ranges::reverse(reordered);
        result = sourceLibrary.Reorder(reordered);
    } else if (SUCCEEDED(result)) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    const std::vector<WallpaperItem> persistedOrder = sourceLibrary.Scan();
    if (FAILED(result) || persistedOrder.size() != 2 ||
        _wcsicmp(persistedOrder.front().path.c_str(),
                 reordered.front().path.c_str()) != 0) {
        LogError(L"Library persisted reorder self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_REORDER_PERSISTED=True");

    WallpaperGroupStore groups;
    std::wstring firstGroupId;
    std::wstring secondGroupId;
    result = groups.InitializeAt(sourceLibrary.RootDirectory());
    if (SUCCEEDED(result)) {
        result = groups.CreateGroup(L"工作", firstGroupId);
    }
    if (SUCCEEDED(result)) {
        result = groups.CreateGroup(L"休闲", secondGroupId);
    }
    if (SUCCEEDED(result) &&
        SUCCEEDED(groups.CreateGroup(L"工作", secondGroupId))) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    const std::array<std::wstring, 2> memberNames{
        persistedOrder[0].path.filename().native(),
        persistedOrder[1].path.filename().native()};
    if (SUCCEEDED(result)) {
        result = groups.AddToGroup(firstGroupId, memberNames);
    }
    const std::array<std::wstring, 1> sharedMember{memberNames.front()};
    if (SUCCEEDED(result)) {
        result = groups.AddToGroup(secondGroupId, sharedMember);
    }
    if (SUCCEEDED(result)) {
        result = groups.SetFavorite(memberNames.front(), true);
    }
    const std::array<std::wstring, 2> groupOrder{secondGroupId, firstGroupId};
    if (SUCCEEDED(result)) {
        result = groups.ReorderGroups(groupOrder);
    }
    WallpaperGroupStore reloadedGroups;
    if (SUCCEEDED(result)) {
        result = reloadedGroups.InitializeAt(sourceLibrary.RootDirectory());
    }
    if (FAILED(result) || reloadedGroups.Groups().size() != 2 ||
        _wcsicmp(reloadedGroups.Groups().front().id.c_str(),
                 secondGroupId.c_str()) != 0 ||
        !reloadedGroups.IsFavorite(memberNames.front()) ||
        !reloadedGroups.IsInGroup(firstGroupId, memberNames.front()) ||
        !reloadedGroups.IsInGroup(secondGroupId, memberNames.front()) ||
        !reloadedGroups.IsInGroup(firstGroupId, memberNames.back())) {
        LogError(L"Wallpaper group persistence self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        cleanup();
        return 1;
    }
    result = reloadedGroups.RemoveFromGroup(firstGroupId, sharedMember);
    if (FAILED(result) ||
        reloadedGroups.IsInGroup(firstGroupId, memberNames.front()) ||
        !reloadedGroups.IsInGroup(secondGroupId, memberNames.front())) {
        LogError(L"Wallpaper multi-group membership self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_WALLPAPER_MULTI_GROUP=True");
    result = reloadedGroups.RemoveWallpaperKey(memberNames.front());
    const std::array<std::wstring, 1> validNames{memberNames.back()};
    if (SUCCEEDED(result)) {
        result = reloadedGroups.Prune(validNames);
    }
    if (FAILED(result) || reloadedGroups.IsFavorite(memberNames.front()) ||
        reloadedGroups.IsInGroup(firstGroupId, memberNames.front()) ||
        reloadedGroups.IsInGroup(secondGroupId, memberNames.front())) {
        LogError(L"Wallpaper group cleanup self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_WALLPAPER_GROUPS=True");

    result = sourceLibrary.ExportArchive(persistedOrder, archive.native());
    std::vector<WallpaperItem> archiveItems;
    if (SUCCEEDED(result)) {
        result = archiveDestinationLibrary.ImportArchive(archive.native(),
                                                          archiveItems);
    }
    if (FAILED(result) || archiveItems.size() != 2 ||
        !std::ranges::all_of(archiveItems, [&](const WallpaperItem& item) {
            return FilesEqual(importedSource.path, item.path);
        })) {
        LogError(L"Library ZIP archive round-trip self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_CRC));
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_ZIP_ROUNDTRIP=True");

    if (!CopyFileW(archive.c_str(), corruptedArchive.c_str(), TRUE) ||
        FAILED(ModifyByte(corruptedArchive, 0, true)) ||
        SUCCEEDED(archiveDestinationLibrary.ImportArchive(
            corruptedArchive.native(), archiveItems))) {
        LogError(L"Library ZIP archive corruption self-test failed.");
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_ZIP_CORRUPTION_REJECTED=True");

    if (!CopyFileW(archive.c_str(), unsafeArchive.c_str(), TRUE) ||
        FAILED(ModifyByte(unsafeArchive, kZipEntryNameOffset, false)) ||
        SUCCEEDED(archiveDestinationLibrary.ImportArchive(
            unsafeArchive.native(), archiveItems))) {
        LogError(L"Library unsafe ZIP entry self-test failed.");
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_ZIP_UNSAFE_ENTRY_REJECTED=True");

    result = sourceLibrary.Remove(persistedOrder.front());
    const auto remainingSourceItems = sourceLibrary.Scan();
    if (FAILED(result) || remainingSourceItems.size() != 1) {
        LogError(L"Library remove self-test failed.",
                 FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_REMOVE=True");

    const auto destinationItems = destinationLibrary.Scan();
    if (destinationItems.size() != 1) {
        LogError(L"Library self-test found an orphaned extraction file.");
        cleanup();
        return 1;
    }
    LogInfo(L"SELF_TEST_LIBRARY_NO_ORPHANS=True");
    cleanup();
    return 0;
}

}  // namespace lwe::core
