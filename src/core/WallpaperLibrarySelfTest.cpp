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
#include "core/OperationProgress.h"
#include "core/WallpaperOptions.h"
#include "media/image/WicImageLoader.h"

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

bool TestDescriptionCache(const std::filesystem::path& root) {
    WallpaperLibrary library;
    if (FAILED(library.InitializeAt(root))) return false;
    const auto path = root / L"cache.bmp";
    const auto writeBitmap = [&](const LONG width, const LONG height) {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        const std::array<std::uint32_t, 8> pixels{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + sizeof(pixels);
        info.biSize = sizeof(info);
        info.biWidth = width;
        info.biHeight = height;
        info.biPlanes = 1;
        info.biBitCount = 32;
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(handle, &file, sizeof(file), &written, nullptr) &&
                        written == sizeof(file) &&
                        WriteFile(handle, &info, sizeof(info), &written, nullptr) &&
                        written == sizeof(info) &&
                        WriteFile(handle, pixels.data(), sizeof(pixels), &written, nullptr) &&
                        written == sizeof(pixels);
        CloseHandle(handle);
        return ok;
    };
    if (!writeBitmap(2, 4)) return false;
    const auto first = library.Scan();
    if (first.size() != 1 || first.front().width != 2) return false;
    const auto started = GetTickCount64();
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto cached = library.Scan();
        if (cached.size() != 1 || cached.front().height != 4) return false;
    }
    LogInfo(L"LIBRARY_CACHED_SCAN_100_MS=" + std::to_wstring(GetTickCount64() - started));
    if (!writeBitmap(4, 2)) return false;
    std::error_code error;
    std::filesystem::last_write_time(path, first.front().modifiedAt + std::chrono::seconds(2), error);
    if (error) return false;
    const auto changed = library.Scan();
    if (changed.size() != 1 || changed.front().width != 4 ||
        changed.front().height != 2 || changed.front().fileSize != first.front().fileSize) return false;
    library.InvalidateDescriptions();
    WallpaperGroupStore retainedGroups;
    std::wstring retainedGroupId;
    const std::array<std::wstring,1> retainedNames{path.filename().native()};
    if (FAILED(retainedGroups.InitializeAt(root)) ||
        FAILED(retainedGroups.CreateGroup(L"读取失败保留测试",retainedGroupId)) ||
        FAILED(retainedGroups.AddToGroup(retainedGroupId,retainedNames)) ||
        FAILED(retainedGroups.SetFavorites(retainedNames,true))) return false;
    const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return false;
    const auto unavailable = library.Scan();
    CloseHandle(locked);
    if (unavailable.size() != 1 || SUCCEEDED(unavailable.front().readError)) return false;
    if (FAILED(retainedGroups.Prune(retainedNames)) ||
        !retainedGroups.IsFavorite(retainedNames.front()) ||
        !retainedGroups.IsInGroup(retainedGroupId,retainedNames.front())) return false;
    LogInfo(L"SELF_TEST_UNREADABLE_GROUP_MEMBERSHIP=True");
    library.InvalidateDescriptions();
    const auto restored = library.Scan();
    if (restored.size() != 1 || FAILED(restored.front().readError) || restored.front().width != 4) return false;
    LogInfo(L"SELF_TEST_UNREADABLE_ITEM_RETRY=True");
    if (!DeleteFileW(path.c_str()) || !library.Scan().empty()) return false;
    if (FAILED(library.InitializeAt(root / L"other")) || !library.Scan().empty()) return false;
    LogInfo(L"SELF_TEST_LIBRARY_CACHE_INVALIDATION=True");
    return true;
}

}  // namespace

int RunWallpaperLibrarySelfTest(const std::wstring_view sourcePath) {
    WallpaperOptions placementOptions;
    const auto fill = CalculatePlacement(3840,2160,1000,1000,placementOptions);
    if (fill.width != 2160 || fill.x != 840 || fill.outputWidth != 1000) return 1;
    placementOptions.focusX = 100;
    if (CalculatePlacement(3840,2160,1000,1000,placementOptions).x != 1680) return 1;
    placementOptions.fit = FitMode::Fit;
    const auto fit = CalculatePlacement(3840,2160,1920,1200,placementOptions);
    if (fit.outputHeight != 1080 || fit.top != 60 || fit.width != 3840) return 1;
    placementOptions.fit = FitMode::Center;
    const auto center = CalculatePlacement(100,60,200,200,placementOptions);
    if (center.left != 50 || center.top != 70 || center.outputWidth != 100) return 1;
    placementOptions.fit = FitMode::Stretch;
    const auto stretch = CalculatePlacement(100,60,200,200,placementOptions);
    if (stretch.width != 100 || stretch.outputHeight != 200 || stretch.top != 0) return 1;
    const std::array<std::uint8_t,8> red{0,0,255,255,0,0,255,255};
    media::image::WicImageLoader loader;
    media::image::DecodedImage composed;
    placementOptions.fit = FitMode::Fit;
    if (FAILED(loader.ScaleFillBgra(red,2,1,8,4,4,composed,placementOptions)) ||
        composed.pixels[2] != 0 || composed.pixels[3] != 255 || composed.pixels[18] != 255 ||
        composed.pixels[50] != 0) return 1;
    LogInfo(L"SELF_TEST_IMAGE_PLACEMENT=True");
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
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        OperationProgress progress{cancellation.get_token(), {}};
        OperationScope scope(progress);
        const auto cancelledPackage = temporaryRoot / L"cancelled.lwewall";
        const auto cancelledZip = temporaryRoot / L"cancelled.zip";
        WallpaperItem cancelledItem;
        std::vector<WallpaperItem> cancelledItems;
        if (SUCCEEDED(sourceLibrary.ExportArchive(persistedOrder, cancelledZip.native())) ||
            SUCCEEDED(destinationLibrary.ImportFile(sourcePath, cancelledItem)) ||
            SUCCEEDED(archiveDestinationLibrary.ImportArchive(archive.native(), cancelledItems)) ||
            std::filesystem::exists(cancelledZip) || !cancelledItems.empty()) {
            LogError(L"Cancellation transaction self-test failed."); cleanup(); return 1;
        }
    }
    LogInfo(L"SELF_TEST_TASK_CANCELLATION=True");
    {
        std::stop_source cancellation;
        OperationProgress progress{cancellation.get_token(), [&](int, std::wstring_view) { cancellation.request_stop(); }};
        OperationScope scope(progress);
        const auto output = temporaryRoot / L"cancel-in-progress.zip";
        if (SUCCEEDED(sourceLibrary.ExportArchive(persistedOrder, output.native())) ||
            std::filesystem::exists(output)) {cleanup(); return 1;}
    }
    {
        WallpaperLibrary isolated;
        if (FAILED(isolated.InitializeAt(temporaryRoot / L"cancel-copy"))) {cleanup(); return 1;}
        std::stop_source cancellation;
        OperationProgress progress{cancellation.get_token(), [&](int, std::wstring_view) { cancellation.request_stop(); }};
        OperationScope scope(progress);
        WallpaperItem item;
        if (SUCCEEDED(isolated.ImportFile(sourcePath, item)) || !isolated.Scan().empty()) {cleanup(); return 1;}
        for (const auto& entry : std::filesystem::directory_iterator(isolated.RootDirectory())) {
            if (entry.path().extension() == L".importing") {cleanup(); return 1;}
        }
    }
    LogInfo(L"SELF_TEST_TASK_MID_CANCEL_NO_ORPHANS=True");

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
    if (!TestDescriptionCache(temporaryRoot / L"cache-test")) {
        LogError(L"Library metadata cache invalidation self-test failed.");
        cleanup();
        return 1;
    }
    cleanup();
    return 0;
}

}  // namespace lwe::core
