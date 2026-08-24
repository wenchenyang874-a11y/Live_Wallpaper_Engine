#include "app/WallpaperApplication.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wtsapi32.h>
#include <windowsx.h>
#include <wrl/client.h>

#include "core/Logger.h"
#include "media/MediaProbe.h"

namespace lwe::app {
namespace {

constexpr wchar_t kControlWindowClass[] = L"LiveWallpaperEngine.Control";
constexpr wchar_t kWallpaperWindowClass[] = L"LiveWallpaperEngine.Wallpaper";
constexpr wchar_t kApplicationTitle[] = L"Live Wallpaper Engine";
constexpr int kApplicationIconResource = 101;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMediaEngineEventMessage = WM_APP + 2;
constexpr UINT kPlaybackFailureMessage = WM_APP + 3;
constexpr UINT kPlaybackFatalMessage = WM_APP + 4;
constexpr UINT kRevealWallpaperMessage = WM_APP + 5;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kExplorerRecoveryTimer = 1;
constexpr UINT_PTR kPlaybackPolicyTimer = 2;
constexpr UINT_PTR kResourceUsageTimer = 3;
constexpr int kTrayImportCommand = 2100;
constexpr int kTrayShowCommand = 2101;
constexpr int kTraySoundCommand = 2102;
constexpr int kTrayExitCommand = 2103;
constexpr int kTrayCancelCommand = 2104;
constexpr int kTrayPauseCommand = 2105;
constexpr int kControlledLibraryDrawCountCommand = 2195;
constexpr int kControlledFrameCountCommand = 2196;
constexpr int kControlledTestSaveCommand = 2198;
constexpr int kControlledTestExitCommand = 2199;
constexpr int kLibraryPreviewCommand = 2200;
constexpr int kLibraryRenameCommand = 2201;
constexpr int kLibraryApplyCommand = 2202;
constexpr wchar_t kDesktopCompatibilityMutexName[] =
    L"cxWallpaperEngineGlobalMutex";

bool RegisterApplicationClass(const HINSTANCE instance, const wchar_t* className,
                              const WNDPROC procedure, const HBRUSH background,
                              const HICON icon) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = icon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background;
    windowClass.lpszClassName = className;
    windowClass.hIconSm = icon;

    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return true;
    }
    core::LogError(L"RegisterClassEx failed.", HRESULT_FROM_WIN32(GetLastError()));
    return false;
}

DWORD RemainingTestMilliseconds(
    const std::chrono::seconds testDuration,
    const std::chrono::steady_clock::duration elapsed) {
    if (testDuration.count() <= 0) {
        return INFINITE;
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(testDuration - elapsed);
    return static_cast<DWORD>(
        std::clamp<std::int64_t>(remaining.count(), 1, INFINITE - 1LL));
}

core::WallpaperSelectionKind SettingsKind(const media::WallpaperKind kind) {
    switch (kind) {
        case media::WallpaperKind::StaticImage:
            return core::WallpaperSelectionKind::StaticImage;
        case media::WallpaperKind::AnimatedGif:
            return core::WallpaperSelectionKind::AnimatedGif;
        case media::WallpaperKind::Video:
            return core::WallpaperSelectionKind::Video;
    }
    return core::WallpaperSelectionKind::StaticImage;
}

media::WallpaperKind MediaKind(const core::WallpaperSelectionKind kind) {
    switch (kind) {
        case core::WallpaperSelectionKind::StaticImage:
            return media::WallpaperKind::StaticImage;
        case core::WallpaperSelectionKind::AnimatedGif:
            return media::WallpaperKind::AnimatedGif;
        case core::WallpaperSelectionKind::Video:
            return media::WallpaperKind::Video;
        case core::WallpaperSelectionKind::DynamicTest:
            break;
    }
    return media::WallpaperKind::StaticImage;
}

bool SamePath(const std::wstring_view left, const std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool ContainsDisplayId(const std::vector<std::wstring>& identifiers,
    const std::wstring_view identifier) {
    return std::ranges::any_of(identifiers, [&](const std::wstring& existing) {
        return SamePath(existing, identifier);
    });
}

bool IsPackagePath(const std::wstring_view path) {
    return _wcsicmp(std::filesystem::path(path).extension().c_str(), L".lwewall") == 0;
}

std::wstring JoinDisplayIds(const std::vector<std::wstring>& identifiers) {
    std::wstring joined;
    for (const std::wstring& identifier : identifiers) {
        if (!joined.empty()) {
            joined += L'|';
        }
        joined += identifier;
    }
    return joined;
}

std::vector<std::wstring> SplitDisplayIds(const std::wstring_view value) {
    std::vector<std::wstring> identifiers;
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t delimiter = value.find(L'|', start);
        const std::size_t end = delimiter == std::wstring_view::npos
                                    ? value.size()
                                    : delimiter;
        if (end > start) {
            identifiers.emplace_back(value.substr(start, end - start));
        }
        if (delimiter == std::wstring_view::npos) {
            break;
        }
        start = delimiter + 1;
    }
    return identifiers;
}

std::wstring FormatResourceUsage(
    const platform::ProcessResourceUsage& usage) {
    constexpr double mebibyte = 1024.0 * 1024.0;
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << L"CPU " << usage.cpuPercent
         << L"%  GPU ";
    if (usage.gpuAvailable) {
        text << usage.gpuPercent << L'%';
    } else {
        text << L"--";
    }
    text << L"  内存 " << std::setprecision(0)
         << static_cast<double>(usage.workingSetBytes) / mebibyte << L" MB  显存 ";
    if (usage.videoMemoryBytes.has_value()) {
        text << static_cast<double>(*usage.videoMemoryBytes) / mebibyte << L" MB";
    } else {
        text << L"--";
    }
    return text.str();
}

}  // namespace

WallpaperApplication::WallpaperApplication(const HINSTANCE instance,
                                           const HANDLE activationEvent)
    : instance_(instance), activationEvent_(activationEvent) {}

WallpaperApplication::~WallpaperApplication() {
    Shutdown();
}

int WallpaperApplication::Run(const std::chrono::seconds testDuration,
                              const std::vector<std::wstring>& testWallpapers) {
    controlledTestMode_ = testDuration.count() > 0;
    // Tencent DeskGo and other desktop organizers use this established signal
    // to stop painting an opaque copy of the Windows wallpaper above live
    // wallpaper hosts. The handle exists only for our application lifetime.
    SetLastError(ERROR_SUCCESS);
    desktopCompatibilityMutex_ =
        CreateMutexW(nullptr, TRUE, kDesktopCompatibilityMutexName);
    if (desktopCompatibilityMutex_ == nullptr) {
        core::LogWarning(L"Desktop-organizer compatibility signal is unavailable.");
    } else {
        desktopCompatibilityMutexOwned_ = GetLastError() != ERROR_ALREADY_EXISTS;
        core::LogInfo(desktopCompatibilityMutexOwned_
                          ? L"Desktop-organizer live-wallpaper compatibility enabled."
                          : L"Desktop-organizer compatibility is already active.");
    }
    const HRESULT mediaFoundationResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    mediaFoundationStarted_ = SUCCEEDED(mediaFoundationResult);
    if (!mediaFoundationStarted_) {
        core::LogError(L"Media Foundation initialization failed; video is unavailable.",
                       mediaFoundationResult);
    }

    const HRESULT libraryResult = wallpaperLibrary_.Initialize();
    if (FAILED(libraryResult)) {
        core::LogError(L"The local wallpaper library could not be initialized.",
                       libraryResult);
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterWindowClasses() || !CreateControlWindow() ||
        !CreateWallpaperWindow()) {
        Shutdown();
        return 1;
    }
    InitializePlaybackPolicy();
    StartPlaybackRenderThread();
    resourceMonitor_.Initialize();
    SetTimer(controlWindow_, kResourceUsageTimer, 1000, nullptr);
    UpdateResourceUsage();

    bool wallpaperApplied = false;
    if (!testWallpapers.empty()) {
        wallpaperApplied = ApplyWallpaper(testWallpapers.front(), false, false);
        if (!wallpaperApplied) {
            Shutdown();
            return 1;
        }
    } else {
        wallpaperApplied = RestoreSavedWallpaperSelection();
    }
    if (!wallpaperApplied) {
        const std::scoped_lock playbackLock(playbackMutex_);
        playbackMode_ = controlledTestMode_ ? PlaybackMode::TechnicalTest
                                            : PlaybackMode::Stopped;
        if (!controlledTestMode_ && IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
        mainWindow_.SetStatus(
            L"尚未选择壁纸 · 点击“导入壁纸”开始建立你的本地壁纸库");
    }
    RefreshLibrary();

    if (!AddTrayIcon()) {
        core::LogWarning(
            L"The tray icon could not be created; the main window will stay open.");
    }

    const auto startedAt = std::chrono::steady_clock::now();
    auto nextControlledSwitchAt = startedAt + std::chrono::milliseconds(200);
    std::size_t nextControlledWallpaper = 1;
    int exitCode = 0;
    running_ = true;
    ShowControlWindow();

    MSG message{};
    while (running_) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running_) {
            break;
        }

        if (activationEvent_ != nullptr &&
            WaitForSingleObject(activationEvent_, 0) == WAIT_OBJECT_0) {
            ShowControlWindow();
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - startedAt;
        if (testDuration.count() > 0 && elapsed >= testDuration) {
            core::LogInfo(L"Controlled test duration completed.");
            break;
        }
        if (testWallpapers.size() > 1 && now >= nextControlledSwitchAt) {
            const std::wstring& nextPath =
                testWallpapers[nextControlledWallpaper % testWallpapers.size()];
            if (!ApplyWallpaper(nextPath, false, false)) {
                core::LogError(L"Controlled wallpaper switching failed: " + nextPath);
                exitCode = 1;
                break;
            }
            ++nextControlledWallpaper;
            nextControlledSwitchAt = now + std::chrono::milliseconds(200);
            core::LogInfo(L"Controlled wallpaper switch completed; count=" +
                          std::to_wstring(nextControlledWallpaper - 1) + L'.');
        }
        if (playbackMode_ == PlaybackMode::TechnicalTest && renderer_.IsInitialized()) {
            if (!renderer_.Render(elapsed)) {
                exitCode = 1;
                break;
            }
            continue;
        }
        DWORD waitMilliseconds = RemainingTestMilliseconds(testDuration, elapsed);
        if (testWallpapers.size() > 1 && now < nextControlledSwitchAt) {
            waitMilliseconds = std::min(
                waitMilliseconds,
                static_cast<DWORD>(std::clamp<std::int64_t>(
                    std::chrono::ceil<std::chrono::milliseconds>(
                        nextControlledSwitchAt - now).count(),
                    1, INFINITE - 1LL)));
        }

        HANDLE handles[] = {activationEvent_};
        const DWORD handleCount = activationEvent_ == nullptr ? 0UL : 1UL;
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            handleCount, handles, waitMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (handleCount == 1 && waitResult == WAIT_OBJECT_0) {
            ShowControlWindow();
        } else if (waitResult == WAIT_FAILED) {
            core::LogError(L"Idle message wait failed.",
                           HRESULT_FROM_WIN32(GetLastError()));
            exitCode = 1;
            break;
        }
    }

    Shutdown();
    return exitCode != 0 ? exitCode : runtimeExitCode_.load();
}

bool WallpaperApplication::RegisterWindowClasses() {
    const HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(kApplicationIconResource));
    return RegisterApplicationClass(instance_, kControlWindowClass,
                                    &WallpaperApplication::WindowProcedure, nullptr,
                                    icon) &&
           RegisterApplicationClass(instance_, kWallpaperWindowClass,
                                    &WallpaperApplication::WindowProcedure, nullptr,
                                    icon);
}

bool WallpaperApplication::CreateControlWindow() {
    const UINT dpi = GetDpiForSystem();
    constexpr DWORD windowStyle =
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT windowRectangle{0, 0, MulDiv(1040, dpi, 96), MulDiv(700, dpi, 96)};
    AdjustWindowRectExForDpi(&windowRectangle, windowStyle, FALSE,
                             WS_EX_APPWINDOW, dpi);
    controlWindow_ = CreateWindowExW(
        WS_EX_APPWINDOW, kControlWindowClass, kApplicationTitle, windowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, windowRectangle.right - windowRectangle.left,
        windowRectangle.bottom - windowRectangle.top, nullptr, nullptr, instance_, this);
    if (controlWindow_ == nullptr) {
        core::LogError(L"Unable to create the main window.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    if (!mainWindow_.Create(controlWindow_, instance_)) {
        core::LogError(L"Unable to create the modern library controls.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    DragAcceptFiles(controlWindow_, TRUE);
    return true;
}

bool WallpaperApplication::CreateWallpaperWindow() {
    if (wallpaperWindow_ != nullptr && IsWindow(wallpaperWindow_)) {
        return true;
    }

    const std::optional target = shell::FindDesktopTarget();
    if (!target.has_value()) {
        return false;
    }
    desktopTarget_ = *target;

    constexpr DWORD extendedStyle =
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    constexpr DWORD style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    wallpaperWindow_ = CreateWindowExW(
        extendedStyle, kWallpaperWindowClass, L"", style, 0, 0, 1, 1, nullptr, nullptr,
        instance_, this);
    if (wallpaperWindow_ == nullptr) {
        core::LogError(L"CreateWindowEx failed for the wallpaper window.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    if (!shell::AttachWallpaperWindow(wallpaperWindow_, desktopTarget_)) {
        return false;
    }
    RefreshDisplayTargets(true);
    if ((!assignments_.empty() && !ConfigureWallpaperWindowRegion()) ||
        !EnsureRenderer()) {
        return false;
    }
    ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
    core::LogInfo(L"Wallpaper renderer window started.");
    return true;
}

bool WallpaperApplication::EnsureRenderer() {
    if (renderer_.IsInitialized()) {
        return true;
    }
    return wallpaperWindow_ != nullptr && renderer_.Initialize(wallpaperWindow_);
}

bool WallpaperApplication::ReattachToDesktop() {
    if (wallpaperWindow_ == nullptr || !IsWindow(wallpaperWindow_)) {
        return RecoverWallpaperWindow();
    }

    const std::optional target = shell::FindDesktopTarget();
    if (!target.has_value() ||
        !shell::AttachWallpaperWindow(wallpaperWindow_, *target)) {
        return false;
    }

    desktopTarget_ = *target;
    RefreshDisplayTargets(true);
    ConfigureWallpaperWindowRegion();
    ResizeRendererToWindow();
    core::LogInfo(L"Wallpaper window reattached after desktop topology change.");
    return true;
}

bool WallpaperApplication::RecoverWallpaperWindow() {
    if (!CreateWallpaperWindow()) {
        return false;
    }
    if (!assignments_.empty()) {
        return RebuildPlaybackSessions(false);
    }
    return true;
}

bool WallpaperApplication::AddTrayIcon() {
    if (controlWindow_ == nullptr) {
        return false;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = controlWindow_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(kApplicationIconResource));
    wcscpy_s(data.szTip, kApplicationTitle);
    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    if (trayIconAdded_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    return trayIconAdded_;
}

void WallpaperApplication::RemoveTrayIcon() {
    if (!trayIconAdded_ || controlWindow_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = controlWindow_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded_ = false;
}

bool WallpaperApplication::RestoreSavedWallpaperSelection() {
    const std::optional settings = settingsStore_.Load();
    if (!settings.has_value()) {
        return false;
    }
    soundEnabled_ = settings->soundEnabled;
    selectedDisplayIds_ = SplitDisplayIds(settings->displayTargets);
    spanAcrossDisplays_ = settings->spanAcrossDisplays;
    assignments_ = settings->assignments;
    RefreshDisplayTargets(true);
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (assignments_.empty()) {
        return false;
    }
    if (RebuildPlaybackSessions(false)) {
        core::LogInfo(L"Restored saved per-display wallpaper assignments.");
        return true;
    }
    mainWindow_.SetStatus(
        L"上次选择的壁纸无法恢复 · 文件可能已移动或当前系统缺少解码器");
    core::LogWarning(L"Unable to restore the saved wallpaper selection.");
    return false;
}

void WallpaperApplication::RefreshLibrary() {
    std::vector<core::WallpaperItem> items = wallpaperLibrary_.Scan();
    for (const std::wstring& activePath : ActiveWallpaperPaths()) {
        const bool alreadyListed = std::ranges::any_of(items, [&](const auto& item) {
            return SamePath(item.path.native(), activePath);
        });
        std::error_code fileError;
        if (!alreadyListed && std::filesystem::is_regular_file(activePath, fileError) &&
            !fileError) {
            media::MediaInfo info;
            if (SUCCEEDED(media::ProbeMediaFile(activePath, info))) {
                core::WallpaperItem external;
                external.path = activePath;
                external.displayName =
                    std::filesystem::path(activePath).filename().native() +
                    L"（外部）";
                external.kind = info.kind;
                external.formatLabel = info.formatLabel;
                external.width = info.width;
                external.height = info.height;
                external.hasAudio = info.hasAudio;
                external.external = true;
                std::error_code error;
                external.fileSize =
                    std::filesystem::file_size(activePath, error);
                items.insert(items.begin(), std::move(external));
            }
        }
    }
    mainWindow_.SetItems(std::move(items));
    mainWindow_.SetActivePaths(ActiveWallpaperPaths());
    mainWindow_.SetSoundEnabled(soundEnabled_);
}

void WallpaperApplication::ChooseImport() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"无法打开导入窗口。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"支持的壁纸和分享包",
         L"*.lwewall;*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.mp4;*.m4v;*.mov;*.wmv;*.avi"},
        {L"Live Wallpaper 分享包 (*.lwewall)", L"*.lwewall"},
        {L"图片和 GIF", L"*.jpg;*.jpeg;*.png;*.bmp;*.gif"},
        {L"视频", L"*.mp4;*.m4v;*.mov;*.wmv;*.avi"},
    };
    result = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    FILEOPENDIALOGOPTIONS options{};
    if (SUCCEEDED(result)) {
        result = dialog->GetOptions(&options);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                    FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT |
                                    FOS_NOCHANGEDIR);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetTitle(L"导入到我的壁纸");
    }
    if (SUCCEEDED(result)) {
        result = dialog->Show(controlWindow_);
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }
    if (FAILED(result)) {
        MessageBoxW(controlWindow_, L"导入窗口打开失败。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }

    Microsoft::WRL::ComPtr<IShellItemArray> selected;
    result = dialog->GetResults(&selected);
    DWORD count = 0;
    if (SUCCEEDED(result)) {
        result = selected->GetCount(&count);
    }
    std::vector<std::wstring> paths;
    for (DWORD index = 0; SUCCEEDED(result) && index < count; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        result = selected->GetItemAt(index, &item);
        PWSTR path = nullptr;
        if (SUCCEEDED(result)) {
            result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        }
        if (SUCCEEDED(result) && path != nullptr) {
            paths.emplace_back(path);
        }
        CoTaskMemFree(path);
    }
    if (SUCCEEDED(result)) {
        ImportPaths(paths);
    }
}

void WallpaperApplication::ImportPaths(const std::vector<std::wstring>& paths) {
    std::vector<core::WallpaperItem> importedItems;
    std::size_t failedCount = 0;
    for (const std::wstring& path : paths) {
        core::WallpaperItem item;
        const HRESULT result = IsPackagePath(path)
                                   ? wallpaperLibrary_.ImportPackage(path, item)
                                   : wallpaperLibrary_.ImportFile(path, item);
        if (SUCCEEDED(result)) {
            importedItems.push_back(std::move(item));
        } else {
            ++failedCount;
            core::LogError(L"Wallpaper import failed: " + path, result);
        }
    }

    if (!importedItems.empty()) {
        ApplyWallpaperWithTargetPrompt(importedItems.front().path.native(), true,
                                       true);
    }
    RefreshLibrary();

    std::wstring status = L"已导入 " + std::to_wstring(importedItems.size()) + L" 项";
    if (failedCount > 0) {
        status += L" · " + std::to_wstring(failedCount) + L" 项不受支持或校验失败";
        MessageBoxW(controlWindow_, status.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONWARNING);
    }
    mainWindow_.SetStatus(std::move(status));
}

void WallpaperApplication::ChooseExport() {
    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        MessageBoxW(controlWindow_, L"请先在“我的壁纸”中选择一项。",
                    kApplicationTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }

    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return;
    }
    const COMDLG_FILTERSPEC filter{L"Live Wallpaper 分享包 (*.lwewall)",
                                   L"*.lwewall"};
    result = dialog->SetFileTypes(1, &filter);
    if (SUCCEEDED(result)) {
        result = dialog->SetDefaultExtension(L"lwewall");
    }
    const std::wstring suggestedName =
        selected->path.stem().native() + L".lwewall";
    if (SUCCEEDED(result)) {
        result = dialog->SetFileName(suggestedName.c_str());
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetTitle(L"导出可分享壁纸包");
    }
    if (SUCCEEDED(result)) {
        result = dialog->Show(controlWindow_);
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return;
    }

    Microsoft::WRL::ComPtr<IShellItem> destinationItem;
    PWSTR destination = nullptr;
    if (SUCCEEDED(result)) {
        result = dialog->GetResult(&destinationItem);
    }
    if (SUCCEEDED(result)) {
        result = destinationItem->GetDisplayName(SIGDN_FILESYSPATH, &destination);
    }
    if (SUCCEEDED(result) && destination != nullptr) {
        result = wallpaperLibrary_.ExportPackage(*selected, destination);
    }
    CoTaskMemFree(destination);

    if (FAILED(result)) {
        std::wstring message = L"壁纸包导出失败。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    mainWindow_.SetStatus(L"壁纸包已导出 · 可以直接分享给其他用户导入");
}

void WallpaperApplication::ApplySelectedWallpaper() {
    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        MessageBoxW(controlWindow_, L"请先选择一项壁纸。", kApplicationTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    ApplyWallpaperWithTargetPrompt(selected->path.native());
}

bool WallpaperApplication::ApplyWallpaperWithTargetPrompt(
    const std::wstring_view path, const bool persistSelection,
    const bool showErrors) {
    if (!ChooseApplicationTargets()) {
        return false;
    }
    return ApplyWallpaper(path, persistSelection, showErrors);
}

void WallpaperApplication::PreviewSelectedWallpaper() {
    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        return;
    }
    PreviewWallpaper(*selected);
}

void WallpaperApplication::PreviewWallpaper(const core::WallpaperItem& item) {
    // Use the registered Windows preview/player so image, GIF and video
    // previews remain codec-aware without starting a second decoder inside the
    // lightweight wallpaper process.
    const HINSTANCE opened = ShellExecuteW(controlWindow_, L"open",
                                           item.path.c_str(), nullptr,
                                           item.path.parent_path().c_str(),
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) {
        MessageBoxW(controlWindow_, L"Windows 无法打开该壁纸的预览程序。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
    }
}

void WallpaperApplication::CommitWallpaperRename() {
    const auto rename = mainWindow_.FinishRename();
    if (!rename.has_value()) {
        return;
    }
    const core::WallpaperItem source = rename->first;
    const bool wasActive = std::ranges::any_of(
        assignments_, [&](const core::WallpaperAssignmentSetting& assignment) {
            return SamePath(assignment.wallpaperPath, source.path.native());
        });
    if (wasActive) {
        StopAllPlayback();
    }
    core::WallpaperItem renamed;
    const HRESULT result = wallpaperLibrary_.Rename(source, rename->second, renamed);
    if (FAILED(result)) {
        if (wasActive) {
            RebuildPlaybackSessions(false);
        }
        std::wstring message = L"壁纸重命名失败。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    if (wasActive) {
        for (auto& assignment : assignments_) {
            if (SamePath(assignment.wallpaperPath, source.path.native())) {
                assignment.wallpaperPath = renamed.path.native();
            }
        }
        if (!RebuildPlaybackSessions(true) || FAILED(SaveCurrentSelection())) {
            MessageBoxW(controlWindow_,
                        L"壁纸已重命名，但播放状态或本地设置恢复失败。",
                        kApplicationTitle, MB_OK | MB_ICONWARNING);
        }
    }
    RefreshLibrary();
    mainWindow_.SetStatus(L"壁纸已重命名 · " + renamed.displayName);
}

void WallpaperApplication::ShowLibraryContextMenu(POINT screenPoint) {
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        const LRESULT selection =
            SendMessageW(mainWindow_.LibraryControl(), LB_GETCURSEL, 0, 0);
        RECT item{};
        if (selection < 0 ||
            SendMessageW(mainWindow_.LibraryControl(), LB_GETITEMRECT, selection,
                         reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
            return;
        }
        screenPoint = POINT{item.left + 24, item.bottom};
        ClientToScreen(mainWindow_.LibraryControl(), &screenPoint);
    } else if (!mainWindow_.SelectItemAtScreenPoint(screenPoint)) {
        return;
    }

    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kLibraryPreviewCommand, L"预览壁纸");
    AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                kLibraryRenameCommand, L"重命名");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryApplyCommand, L"应用到所选屏幕");
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPoint.x,
        screenPoint.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    if (command == kLibraryPreviewCommand) {
        PreviewSelectedWallpaper();
    } else if (command == kLibraryRenameCommand) {
        mainWindow_.BeginRenameSelected();
    } else if (command == kLibraryApplyCommand) {
        ApplySelectedWallpaper();
    }
}

void WallpaperApplication::ShowActiveWallpaperContextMenu(POINT screenPoint) {
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        const LRESULT selection = SendMessageW(
            mainWindow_.ActiveLibraryControl(), LB_GETCURSEL, 0, 0);
        RECT item{};
        if (selection < 0 ||
            SendMessageW(mainWindow_.ActiveLibraryControl(), LB_GETITEMRECT,
                         selection, reinterpret_cast<LPARAM>(&item)) == LB_ERR) {
            return;
        }
        screenPoint = POINT{item.left + 24, item.bottom};
        ClientToScreen(mainWindow_.ActiveLibraryControl(), &screenPoint);
    } else if (!mainWindow_.SelectActiveItemAtScreenPoint(screenPoint)) {
        return;
    }

    const std::optional selected = mainWindow_.SelectedActiveItem();
    if (!selected.has_value()) {
        return;
    }
    constexpr int kActivePreviewCommand = 2210;
    constexpr int kActiveRenameCommand = 2211;
    constexpr int kActiveCancelCommand = 2212;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kActivePreviewCommand, L"预览壁纸");
    AppendMenuW(menu, MF_STRING | (selected->external ? MF_GRAYED : 0),
                kActiveRenameCommand, L"重命名");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kActiveCancelCommand, L"取消应用此壁纸");
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPoint.x,
        screenPoint.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    if (command == kActivePreviewCommand) {
        PreviewWallpaper(*selected);
    } else if (command == kActiveRenameCommand) {
        mainWindow_.BeginRenameActiveSelected();
    } else if (command == kActiveCancelCommand) {
        CancelWallpaper(*selected);
    }
}

void WallpaperApplication::CancelActiveWallpaper(const bool persistSelection) {
    const std::scoped_lock playbackLock(playbackMutex_);
    StopAllPlayback();
    assignments_.clear();
    playbackMode_ = PlaybackMode::Stopped;
    manualPlaybackPaused_ = false;
    if (IsWindow(wallpaperWindow_)) {
        ShowWindow(wallpaperWindow_, SW_HIDE);
    }
    mainWindow_.SetActivePaths({});
    mainWindow_.SetStatus(L"当前未应用壁纸 · Windows 原壁纸已恢复显示");
    RefreshLibrary();
    if (persistSelection && FAILED(SaveCurrentSelection())) {
        MessageBoxW(controlWindow_, L"壁纸已取消，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
}

void WallpaperApplication::CancelWallpaper(
    const core::WallpaperItem& item, const bool confirmCancellation,
    const bool persistSelection) {
    const bool isActive = std::ranges::any_of(
        assignments_, [&](const core::WallpaperAssignmentSetting& assignment) {
            return SamePath(assignment.wallpaperPath, item.path.native());
        });
    if (!isActive) {
        return;
    }
    if (confirmCancellation) {
        std::wstring prompt = L"确认取消应用“" + item.displayName +
                              L"”？\r\n\r\n该壁纸在所有屏幕上的应用都会取消。";
        if (MessageBoxW(controlWindow_, prompt.c_str(), kApplicationTitle,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    const std::scoped_lock playbackLock(playbackMutex_);

    // Removing one wallpaper must not rebuild unrelated video decoders. Their
    // clocks and transfer surfaces remain alive, so canceling an image on one
    // display cannot introduce a visible hitch on another display's video.
    std::erase_if(playbackSessions_, [&](const auto& session) {
        const bool matches =
            SamePath(session->assignment.wallpaperPath, item.path.native());
        if (matches && session->gifPlayer) {
            session->gifPlayer->Reset();
        }
        if (matches && session->videoPlayer) {
            session->videoPlayer->Shutdown();
        }
        return matches;
    });
    std::erase_if(assignments_, [&](const auto& assignment) {
        return SamePath(assignment.wallpaperPath, item.path.native());
    });

    if (assignments_.empty()) {
        playbackMode_ = PlaybackMode::Stopped;
        pendingWallpaperReveal_ = false;
        dynamicPlaybackPaused_ = false;
        manualPlaybackPaused_ = false;
        if (IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
    } else {
        playbackMode_ = PlaybackMode::Active;
        ConfigureWallpaperWindowRegion();
    }
    mainWindow_.SetActivePaths(ActiveWallpaperPaths());
    mainWindow_.SetStatus(ActivePlaybackStatus());
    RefreshLibrary();
    WakePlaybackRenderThread();
    if (persistSelection && FAILED(SaveCurrentSelection())) {
        MessageBoxW(controlWindow_, L"壁纸已取消，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
}

bool WallpaperApplication::ApplyWallpaper(const std::wstring_view path,
                                          const bool persistSelection,
                                          const bool showErrors) {
    media::MediaInfo info;
    HRESULT result = media::ProbeMediaFile(path, info);
    if (FAILED(result)) {
        if (showErrors) {
            MessageBoxW(controlWindow_,
                        L"无法识别该文件的实际图片容器或视频编码。",
                        kApplicationTitle, MB_OK | MB_ICONERROR);
        }
        return false;
    }
    if ((wallpaperWindow_ == nullptr || !IsWindow(wallpaperWindow_)) &&
        !CreateWallpaperWindow()) {
        return false;
    }
    const auto previousAssignments = assignments_;
    core::WallpaperAssignmentSetting newAssignment{
        SettingsKind(info.kind), std::wstring(path),
        JoinDisplayIds(selectedDisplayIds_), spanAcrossDisplays_};

    if (spanAcrossDisplays_) {
        assignments_.clear();
        assignments_.push_back(std::move(newAssignment));
    } else {
        // Replacing selected screens must leave every other screen untouched.
        // A former span assignment is first expanded to concrete display IDs,
        // then the selected IDs are removed from it.
        for (auto& assignment : assignments_) {
            std::vector<std::wstring> identifiers =
                assignment.spanAcrossDisplays
                    ? std::vector<std::wstring>{}
                    : SplitDisplayIds(assignment.displayTargets);
            if (assignment.spanAcrossDisplays) {
                identifiers.reserve(displayTargets_.size());
                for (const auto& display : displayTargets_) {
                    identifiers.push_back(display.deviceId);
                }
            }
            std::erase_if(identifiers, [&](const std::wstring& identifier) {
                return ContainsDisplayId(selectedDisplayIds_, identifier);
            });
            assignment.displayTargets = JoinDisplayIds(identifiers);
            assignment.spanAcrossDisplays = false;
        }
        std::erase_if(assignments_, [](const auto& assignment) {
            return !assignment.spanAcrossDisplays &&
                   assignment.displayTargets.empty();
        });

        auto matching = std::ranges::find_if(
            assignments_, [&](const auto& assignment) {
                return !assignment.spanAcrossDisplays &&
                       assignment.wallpaperKind == newAssignment.wallpaperKind &&
                       SamePath(assignment.wallpaperPath,
                                newAssignment.wallpaperPath);
            });
        if (matching == assignments_.end()) {
            assignments_.push_back(std::move(newAssignment));
        } else {
            std::vector<std::wstring> identifiers =
                SplitDisplayIds(matching->displayTargets);
            for (const std::wstring& selectedId : selectedDisplayIds_) {
                if (!ContainsDisplayId(identifiers, selectedId)) {
                    identifiers.push_back(selectedId);
                }
            }
            matching->displayTargets = JoinDisplayIds(identifiers);
        }
    }

    if (!RebuildPlaybackSessions(showErrors)) {
        assignments_ = previousAssignments;
        RebuildPlaybackSessions(false);
        if (showErrors) {
            MessageBoxW(controlWindow_, L"壁纸无法加载或播放。",
                        kApplicationTitle,
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }

    HRESULT saveResult = S_OK;
    if (persistSelection) {
        saveResult = SaveCurrentSelection();
    }
    mainWindow_.SetActivePaths(ActiveWallpaperPaths());
    mainWindow_.SetStatus(ActivePlaybackStatus());
    mainWindow_.SetSoundEnabled(soundEnabled_);
    RefreshLibrary();

    if (persistSelection && FAILED(saveResult) && showErrors) {
        MessageBoxW(controlWindow_, L"壁纸已应用，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
    return true;
}

bool WallpaperApplication::RebuildPlaybackSessions(const bool showErrors) {
    const std::scoped_lock playbackLock(playbackMutex_);
    StopAllPlayback();
    if (assignments_.empty()) {
        playbackMode_ = PlaybackMode::Stopped;
        if (IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
        return true;
    }
    if (!EnsureRenderer()) {
        return false;
    }
    if (!ConfigureWallpaperWindowRegion()) {
        return false;
    }

    bool presentedImmediately = false;
    for (const auto& assignment : assignments_) {
        auto session = std::make_unique<WallpaperSession>();
        session->token = nextSessionToken_++;
        if (nextSessionToken_ == 0) {
            nextSessionToken_ = 1;
        }
        session->assignment = assignment;
        session->kind = MediaKind(assignment.wallpaperKind);
        session->destinations = DestinationsForAssignment(assignment);
        if (session->destinations.empty()) {
            continue;
        }
        const HRESULT result = StartWallpaperSession(*session);
        if (FAILED(result)) {
            StopAllPlayback();
            if (showErrors) {
                std::wstring message = L"壁纸会话启动失败。\r\n\r\n";
                message += core::HResultMessage(result);
                MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                            MB_OK | MB_ICONERROR);
            }
            return false;
        }
        std::wstring sessionMessage =
            L"Wallpaper session " + std::to_wstring(session->token) + L" started: ";
        sessionMessage += media::WallpaperKindLabel(session->kind);
        sessionMessage += L", targets=";
        sessionMessage += assignment.spanAcrossDisplays
                              ? std::wstring(L"span_all")
                              : assignment.displayTargets;
        core::LogInfo(sessionMessage);
        presentedImmediately = presentedImmediately ||
                               session->kind != media::WallpaperKind::Video;
        playbackSessions_.push_back(std::move(session));
    }
    if (playbackSessions_.empty()) {
        return false;
    }
    playbackMode_ = PlaybackMode::Active;
    pendingWallpaperReveal_ =
        !presentedImmediately && !IsWindowVisible(wallpaperWindow_);
    if (presentedImmediately) {
        ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
    }
    RefreshPlaybackPolicy();
    WakePlaybackRenderThread();
    return true;
}

HRESULT WallpaperApplication::StartWallpaperSession(WallpaperSession& session) {
    if (session.destinations.empty()) {
        return E_INVALIDARG;
    }
    if (session.kind == media::WallpaperKind::StaticImage) {
        const HRESULT result = RenderStaticImage(
            session.assignment.wallpaperPath, session.destinations);
        if (SUCCEEDED(result)) {
            core::LogInfo(L"Static wallpaper session presented one frame.");
        }
        return result;
    }
    if (session.kind == media::WallpaperKind::AnimatedGif) {
        RECT client{};
        if (!GetClientRect(wallpaperWindow_, &client)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        session.gifPlayer = std::make_unique<media::image::GifPlayer>();
        HRESULT result = session.gifPlayer->Load(
            session.assignment.wallpaperPath, static_cast<UINT>(client.right),
            static_cast<UINT>(client.bottom));
        if (SUCCEEDED(result)) {
            session.gifPlayer->SetTargetRects(session.destinations);
        }
        if (SUCCEEDED(result) && !session.gifPlayer->PresentDue(
                                     renderer_, std::chrono::steady_clock::now())) {
            result = E_FAIL;
        }
        if (SUCCEEDED(result)) {
            core::LogInfo(L"Animated GIF overlay mode activated.");
        }
        return result;
    }
    if (!mediaFoundationStarted_) {
        return MF_E_PLATFORM_NOT_INITIALIZED;
    }
    session.videoPlayer = std::make_unique<media::video::MediaEnginePlayer>();
    return session.videoPlayer->Open(
        renderer_.Device(), controlWindow_, kMediaEngineEventMessage,
        session.assignment.wallpaperPath, soundEnabled_, session.token);
}

HRESULT WallpaperApplication::RenderStaticImage(
    const std::wstring_view path, const std::span<const RECT> destinations) {
    if (!renderer_.IsInitialized() || wallpaperWindow_ == nullptr) {
        return E_UNEXPECTED;
    }
    std::vector<media::image::DecodedImage> images;
    images.reserve(destinations.size());
    HRESULT result = S_OK;
    for (const RECT& destination : destinations) {
        const LONG width = destination.right - destination.left;
        const LONG height = destination.bottom - destination.top;
        if (width <= 0 || height <= 0) {
            return E_INVALIDARG;
        }
        media::image::DecodedImage image;
        result = imageLoader_.LoadFill(path, static_cast<UINT>(width),
                                       static_cast<UINT>(height), image);
        if (FAILED(result)) {
            return result;
        }
        images.push_back(std::move(image));
    }
    std::vector<render::ImageRegion> regions;
    regions.reserve(images.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto& image = images[index];
        const RECT& destination = destinations[index];
        regions.push_back(render::ImageRegion{
            image.pixels, image.width, image.height, image.stride,
            destination.left, destination.top});
    }
    if (regions.empty() || !renderer_.PresentImageRegions(regions)) {
        result = E_FAIL;
    }
    return result;
}

void WallpaperApplication::StopAllPlayback() {
    const std::scoped_lock playbackLock(playbackMutex_);
    for (const auto& session : playbackSessions_) {
        if (session->gifPlayer) {
            session->gifPlayer->Reset();
        }
        if (session->videoPlayer) {
            session->videoPlayer->Shutdown();
        }
    }
    playbackSessions_.clear();
    dynamicPlaybackPaused_ = false;
    pendingWallpaperReveal_ = false;
}

bool WallpaperApplication::RemoveFailedPlaybackSessions() {
    const std::scoped_lock playbackLock(playbackMutex_);
    std::vector<core::WallpaperAssignmentSetting> failedAssignments;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer && session->videoPlayer->HasFailed()) {
            failedAssignments.push_back(session->assignment);
        }
    }
    if (failedAssignments.empty()) {
        return false;
    }

    // A decoder failure belongs to one wallpaper assignment. Keeping the
    // control window and unrelated display sessions alive prevents a bad or
    // transient video event from looking like an application crash.
    std::erase_if(playbackSessions_, [&](const auto& session) {
        const bool failed = session->videoPlayer && session->videoPlayer->HasFailed();
        if (failed) {
            session->videoPlayer->Shutdown();
        }
        return failed;
    });
    const auto assignmentFailed = [&](const auto& assignment) {
        return std::ranges::any_of(
            failedAssignments, [&](const auto& failed) {
                return assignment.wallpaperKind == failed.wallpaperKind &&
                       assignment.spanAcrossDisplays == failed.spanAcrossDisplays &&
                       SamePath(assignment.wallpaperPath, failed.wallpaperPath) &&
                       SamePath(assignment.displayTargets, failed.displayTargets);
            });
    };
    std::erase_if(assignments_, assignmentFailed);

    if (assignments_.empty()) {
        playbackMode_ = PlaybackMode::Stopped;
        pendingWallpaperReveal_ = false;
        if (IsWindow(wallpaperWindow_)) {
            ShowWindow(wallpaperWindow_, SW_HIDE);
        }
    } else {
        playbackMode_ = PlaybackMode::Active;
        ConfigureWallpaperWindowRegion();
    }
    mainWindow_.SetActivePaths(ActiveWallpaperPaths());
    mainWindow_.SetStatus(
        assignments_.empty()
            ? L"视频播放失败，已停止该壁纸 · 程序仍在运行"
            : L"一个视频播放失败并已停止 · 其他屏幕继续运行");
    RefreshLibrary();
    if (!controlledTestMode_ && FAILED(SaveCurrentSelection())) {
        core::LogWarning(
            L"Failed video assignment was removed but settings could not be saved.");
    }
    core::LogWarning(
        L"Contained a video playback failure without exiting the application.");
    WakePlaybackRenderThread();
    return true;
}

void WallpaperApplication::ToggleSound() {
    const std::scoped_lock playbackLock(playbackMutex_);
    soundEnabled_ = !soundEnabled_;
    bool failed = false;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer &&
            FAILED(session->videoPlayer->SetSoundEnabled(soundEnabled_))) {
            failed = true;
        }
    }
    if (failed) {
        soundEnabled_ = !soundEnabled_;
        for (const auto& session : playbackSessions_) {
            if (session->videoPlayer) {
                session->videoPlayer->SetSoundEnabled(soundEnabled_);
            }
        }
        MessageBoxW(controlWindow_, L"无法更改当前视频的声音状态。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
        return;
    }
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (!controlledTestMode_ && !assignments_.empty()) {
        SaveCurrentSelection();
        mainWindow_.SetStatus(ActivePlaybackStatus());
    } else {
        mainWindow_.SetStatus(soundEnabled_ ? L"声音已开启 · 将应用到后续视频壁纸"
                                           : L"声音已关闭 · 视频默认静音");
    }
}

void WallpaperApplication::ToggleManualPlaybackPause() {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (!HasDynamicPlayback()) {
        return;
    }
    manualPlaybackPaused_ = !manualPlaybackPaused_;
    RefreshPlaybackPolicy();
}

HRESULT WallpaperApplication::SaveCurrentSelection() const {
    core::AppSettings settings;
    settings.assignments = assignments_;
    settings.soundEnabled = soundEnabled_;
    settings.displayTargets = JoinDisplayIds(selectedDisplayIds_);
    settings.spanAcrossDisplays = spanAcrossDisplays_;
    return settingsStore_.Save(settings);
}

void WallpaperApplication::RefreshDisplayTargets(const bool preserveSelection) {
    if (!IsWindow(desktopTarget_.parent)) {
        return;
    }
    displayTargets_ = shell::EnumerateDisplayTargets(desktopTarget_.parent);
    if (displayTargets_.empty()) {
        return;
    }

    if (!preserveSelection) {
        selectedDisplayIds_.clear();
    }
    std::erase_if(selectedDisplayIds_, [&](const std::wstring& identifier) {
        return std::ranges::none_of(displayTargets_, [&](const auto& display) {
            return _wcsicmp(display.deviceId.c_str(), identifier.c_str()) == 0;
        });
    });
    if (selectedDisplayIds_.empty()) {
        const auto primary = std::ranges::find_if(
            displayTargets_, [](const shell::DisplayTarget& display) {
                return display.primary;
            });
        selectedDisplayIds_.push_back(
            (primary != displayTargets_.end() ? *primary : displayTargets_.front())
                .deviceId);
    }

    std::vector<ModernMainWindow::DisplayOption> options;
    options.reserve(displayTargets_.size());
    for (std::size_t index = 0; index < displayTargets_.size(); ++index) {
        const auto& display = displayTargets_[index];
        const LONG width = display.clientBounds.right - display.clientBounds.left;
        const LONG height = display.clientBounds.bottom - display.clientBounds.top;
        ModernMainWindow::DisplayOption option;
        option.id = display.deviceId;
        option.label = L"屏幕 " + std::to_wstring(index + 1U) +
                       (display.primary ? L"（主屏）" : L"") + L" · " +
                       std::to_wstring(width) + L"×" + std::to_wstring(height);
        option.selected = std::ranges::any_of(
            selectedDisplayIds_, [&](const std::wstring& identifier) {
                return _wcsicmp(identifier.c_str(), display.deviceId.c_str()) == 0;
            });
        options.push_back(std::move(option));
    }
    mainWindow_.SetDisplayOptions(std::move(options), spanAcrossDisplays_);
}

void WallpaperApplication::ApplyDisplayModeFromUi() {
    spanAcrossDisplays_ = mainWindow_.SpanAcrossDisplays();
    if (FAILED(SaveCurrentSelection())) {
        mainWindow_.SetStatus(L"显示方式已选择，但本地设置保存失败");
        return;
    }
    mainWindow_.SetStatus(
        spanAcrossDisplays_
            ? L"显示方式：跨屏扩展 · 选择壁纸后应用到整个桌面"
            : L"显示方式：分屏显示 · 应用壁纸时选择一个或多个屏幕");
}

bool WallpaperApplication::ChooseApplicationTargets() {
    RefreshDisplayTargets(true);
    spanAcrossDisplays_ = mainWindow_.SpanAcrossDisplays();
    if (spanAcrossDisplays_) {
        return true;
    }
    if (displayTargets_.empty()) {
        MessageBoxW(controlWindow_, L"当前没有可用的显示器。", kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (displayTargets_.size() == 1) {
        selectedDisplayIds_ = {displayTargets_.front().deviceId};
        return true;
    }
    const auto selected = mainWindow_.ChooseDisplayTargets();
    if (!selected.has_value()) {
        mainWindow_.SetStatus(L"已取消应用 · 当前壁纸保持不变");
        return false;
    }
    selectedDisplayIds_ = *selected;
    return !selectedDisplayIds_.empty();
}

std::vector<RECT> WallpaperApplication::DestinationsForAssignment(
    const core::WallpaperAssignmentSetting& assignment) const {
    std::vector<RECT> destinations;
    if (!IsWindow(wallpaperWindow_)) {
        return destinations;
    }
    RECT client{};
    if (!GetClientRect(wallpaperWindow_, &client)) {
        return destinations;
    }
    if (assignment.spanAcrossDisplays) {
        destinations.push_back(client);
        return destinations;
    }

    const std::vector<std::wstring> identifiers =
        SplitDisplayIds(assignment.displayTargets);
    for (const shell::DisplayTarget& display : displayTargets_) {
        const bool selected = ContainsDisplayId(identifiers, display.deviceId);
        RECT clipped{};
        if (selected && IntersectRect(&clipped, &client, &display.clientBounds) &&
            !IsRectEmpty(&clipped)) {
            destinations.push_back(clipped);
        }
    }
    return destinations;
}

bool WallpaperApplication::ConfigureWallpaperWindowRegion() {
    if (!IsWindow(wallpaperWindow_)) {
        return false;
    }
    std::vector<RECT> activeDestinations;
    for (const auto& assignment : assignments_) {
        std::vector<RECT> destinations = DestinationsForAssignment(assignment);
        activeDestinations.insert(activeDestinations.end(), destinations.begin(),
                                  destinations.end());
    }
    if (activeDestinations.empty()) {
        return false;
    }

    HRGN combined = CreateRectRgn(0, 0, 0, 0);
    if (combined == nullptr) {
        return false;
    }
    for (const RECT& destination : activeDestinations) {
        HRGN part = CreateRectRgn(destination.left, destination.top,
                                  destination.right, destination.bottom);
        if (part == nullptr || CombineRgn(combined, combined, part, RGN_OR) == ERROR) {
            if (part != nullptr) {
                DeleteObject(part);
            }
            DeleteObject(combined);
            return false;
        }
        DeleteObject(part);
    }
    // SetWindowRgn takes ownership only on success. The union contains every
    // assigned screen while unassigned displays reveal the Windows wallpaper.
    if (SetWindowRgn(wallpaperWindow_, combined, TRUE) == 0) {
        DeleteObject(combined);
        core::LogError(L"Unable to apply the selected display region.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    return true;
}

std::vector<std::wstring> WallpaperApplication::ActiveWallpaperPaths() const {
    std::vector<std::wstring> paths;
    paths.reserve(assignments_.size());
    for (const auto& assignment : assignments_) {
        if (!std::ranges::any_of(paths, [&](const std::wstring& existing) {
                return SamePath(existing, assignment.wallpaperPath);
            })) {
            paths.push_back(assignment.wallpaperPath);
        }
    }
    return paths;
}

std::wstring WallpaperApplication::ActivePlaybackStatus() const {
    if (assignments_.empty()) {
        return L"当前未应用壁纸 · Windows 原壁纸已恢复显示";
    }
    std::vector<std::wstring> assignedDisplays;
    for (const auto& assignment : assignments_) {
        if (assignment.spanAcrossDisplays) {
            assignedDisplays.clear();
            for (const auto& display : displayTargets_) {
                assignedDisplays.push_back(display.deviceId);
            }
            break;
        }
        for (const std::wstring& identifier :
             SplitDisplayIds(assignment.displayTargets)) {
            if (!ContainsDisplayId(assignedDisplays, identifier)) {
                assignedDisplays.push_back(identifier);
            }
        }
    }
    if (assignments_.size() == 1) {
        const auto& assignment = assignments_.front();
        std::wstring status = L"正在使用 · ";
        status += media::WallpaperKindLabel(MediaKind(assignment.wallpaperKind));
        if (assignment.wallpaperKind == core::WallpaperSelectionKind::Video) {
            status += soundEnabled_ ? L" · 声音已开启" : L" · 默认静音";
        }
        status += L" · ";
        status += std::filesystem::path(assignment.wallpaperPath).filename().native();
        status += L" · ";
        status += assignment.spanAcrossDisplays
                      ? L"跨屏扩展"
                      : std::to_wstring(assignedDisplays.size()) + L" 个屏幕";
        return status;
    }
    return L"正在使用 · " + std::to_wstring(ActiveWallpaperPaths().size()) +
           L" 张壁纸 · " + std::to_wstring(assignedDisplays.size()) +
           L" 个屏幕";
}

bool WallpaperApplication::HasDynamicPlayback() const {
    const std::scoped_lock playbackLock(playbackMutex_);
    return std::ranges::any_of(playbackSessions_, [](const auto& session) {
        return session->gifPlayer || session->videoPlayer;
    });
}

std::uint64_t WallpaperApplication::VideoTransferredFrameCount() const {
    const std::scoped_lock playbackLock(playbackMutex_);
    std::uint64_t count = 0;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer) {
            count += session->videoPlayer->TransferredFrameCount();
        }
    }
    return count;
}

bool WallpaperApplication::RenderPlaybackFrame() {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (dynamicPlaybackPaused_ || playbackMode_ != PlaybackMode::Active) {
        return true;
    }
    if (std::ranges::any_of(playbackSessions_, [](const auto& session) {
            return session->videoPlayer && session->videoPlayer->HasFailed();
        })) {
        if (!playbackFailurePending_.exchange(true)) {
            PostMessageW(controlWindow_, kPlaybackFailureMessage, 0, 0);
        }
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    bool videoFrameComposed = false;
    for (const auto& session : playbackSessions_) {
        if (session->gifPlayer &&
            !session->gifPlayer->PresentDue(renderer_, now)) {
            if (runtimeExitCode_.exchange(1) == 0) {
                PostMessageW(controlWindow_, kPlaybackFatalMessage, 0, 0);
            }
            return false;
        }
        if (!session->videoPlayer || !session->videoPlayer->IsPlaying()) {
            continue;
        }
        const HRESULT frameResult = session->videoPlayer->PresentFrame(
            renderer_, session->destinations, false);
        if (FAILED(frameResult)) {
            if (!playbackFailurePending_.exchange(true)) {
                PostMessageW(controlWindow_, kPlaybackFailureMessage, 0, 0);
            }
            return true;
        }
        videoFrameComposed = videoFrameComposed || frameResult == S_OK;
    }

    if (videoFrameComposed && !renderer_.PresentCurrentFrame(true)) {
        if (runtimeExitCode_.exchange(1) == 0) {
            PostMessageW(controlWindow_, kPlaybackFatalMessage, 0, 0);
        }
        return false;
    }
    if (videoFrameComposed && pendingWallpaperReveal_) {
        pendingWallpaperReveal_ = false;
        PostMessageW(controlWindow_, kRevealWallpaperMessage, 0, 0);
    }
    return true;
}

void WallpaperApplication::StartPlaybackRenderThread() {
    if (playbackRenderThread_.joinable()) {
        return;
    }
    playbackRenderThread_ = std::jthread([this](const std::stop_token stopToken) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        core::LogInfo(L"Playback render thread started; thread_id=" +
                      std::to_wstring(GetCurrentThreadId()) + L'.');
        while (!stopToken.stop_requested()) {
            DWORD waitMilliseconds = 250;
            {
                const std::scoped_lock playbackLock(playbackMutex_);
                if (playbackMode_ == PlaybackMode::Active &&
                    !dynamicPlaybackPaused_) {
                    RenderPlaybackFrame();
                    const auto now = std::chrono::steady_clock::now();
                    waitMilliseconds = INFINITE;
                    for (const auto& session : playbackSessions_) {
                        if (session->gifPlayer) {
                            waitMilliseconds = std::min(
                                waitMilliseconds,
                                session->gifPlayer->WaitMilliseconds(now));
                        }
                        if (session->videoPlayer) {
                            waitMilliseconds = std::min(waitMilliseconds, 8UL);
                        }
                    }
                    if (waitMilliseconds == INFINITE) {
                        waitMilliseconds = 250;
                    }
                }
            }

            std::unique_lock waitLock(playbackMutex_);
            playbackWake_.wait_for(
                waitLock,
                std::chrono::milliseconds(std::max(1UL, waitMilliseconds)));
        }
        core::LogInfo(L"Playback render thread stopped.");
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
    });
}

void WallpaperApplication::StopPlaybackRenderThread() {
    if (!playbackRenderThread_.joinable()) {
        return;
    }
    playbackRenderThread_.request_stop();
    playbackWake_.notify_all();
    playbackRenderThread_.join();
}

void WallpaperApplication::WakePlaybackRenderThread() {
    playbackWake_.notify_all();
}

WallpaperApplication::WallpaperSession* WallpaperApplication::FindSession(
    const std::uint32_t token) {
    const auto found = std::ranges::find_if(
        playbackSessions_, [&](const auto& session) {
            return session->token == token;
        });
    return found == playbackSessions_.end() ? nullptr : found->get();
}

void WallpaperApplication::UpdateResourceUsage() {
    std::optional<std::uint64_t> videoMemoryBytes;
    {
        const std::scoped_lock playbackLock(playbackMutex_);
        videoMemoryBytes = renderer_.VideoMemoryUsage();
    }
    const platform::ProcessResourceUsage usage =
        resourceMonitor_.Sample(videoMemoryBytes);
    mainWindow_.SetResourceUsage(FormatResourceUsage(usage));
}

void WallpaperApplication::InitializePlaybackPolicy() {
    if (!IsWindow(controlWindow_)) {
        return;
    }
    sessionNotificationsRegistered_ =
        WTSRegisterSessionNotification(controlWindow_, NOTIFY_FOR_THIS_SESSION) == TRUE;
    if (!sessionNotificationsRegistered_) {
        core::LogWarning(L"Session lock notifications are unavailable.");
    }
    displayPowerNotification_ = RegisterPowerSettingNotification(
        controlWindow_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (displayPowerNotification_ == nullptr) {
        core::LogWarning(L"Display power notifications are unavailable.");
    }
    SetTimer(controlWindow_, kPlaybackPolicyTimer, 2000, nullptr);
    RefreshPlaybackPolicy();
}

std::wstring WallpaperApplication::PlaybackPauseReason() const {
    if (manualPlaybackPaused_) {
        return L"已从托盘手动暂停";
    }
    if (systemSuspended_) {
        return L"系统正在睡眠";
    }
    if (sessionLocked_) {
        return L"会话已锁定";
    }
    if (displayOff_) {
        return L"显示器已关闭";
    }
    if (fullScreenActive_) {
        return L"检测到全屏应用";
    }
    return {};
}

void WallpaperApplication::RefreshPlaybackPolicy() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    const HRESULT notificationResult = SHQueryUserNotificationState(&state);
    fullScreenActive_ = SUCCEEDED(notificationResult) &&
                        (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
                         state == QUNS_PRESENTATION_MODE);

    const std::scoped_lock playbackLock(playbackMutex_);

    const bool dynamic = HasDynamicPlayback();
    const std::wstring reason = PlaybackPauseReason();
    const bool shouldPause = dynamic && !reason.empty();
    if (shouldPause == dynamicPlaybackPaused_) {
        return;
    }

    bool pauseFailed = false;
    for (const auto& session : playbackSessions_) {
        if (session->videoPlayer &&
            FAILED(session->videoPlayer->SetPaused(shouldPause))) {
            pauseFailed = true;
        }
    }
    if (pauseFailed) {
        core::LogWarning(L"Unable to change video playback for the pause policy.");
        return;
    }
    dynamicPlaybackPaused_ = shouldPause;
    if (shouldPause) {
        mainWindow_.SetStatus(L"动态壁纸已暂停 · " + reason);
        core::LogInfo(L"Dynamic wallpaper paused: " + reason);
    } else {
        mainWindow_.SetStatus(ActivePlaybackStatus());
        core::LogInfo(L"Dynamic wallpaper resumed after pause policy cleared.");
    }
    WakePlaybackRenderThread();
}

void WallpaperApplication::ShowControlWindow() {
    if (controlWindow_ == nullptr || !IsWindow(controlWindow_)) {
        return;
    }
    ShowWindow(controlWindow_, IsIconic(controlWindow_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(controlWindow_);
    FLASHWINFO flash{sizeof(flash), controlWindow_, FLASHW_TRAY, 2, 0};
    FlashWindowEx(&flash);
}

void WallpaperApplication::ShowTrayMenu() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayShowCommand, L"打开我的壁纸");
    AppendMenuW(menu, MF_STRING, kTrayImportCommand, L"导入壁纸...");
    AppendMenuW(menu, MF_STRING | (soundEnabled_ ? MF_CHECKED : 0),
                kTraySoundCommand, L"视频声音");
    AppendMenuW(menu,
                MF_STRING | (HasDynamicPlayback() ? 0 : MF_GRAYED) |
                    (manualPlaybackPaused_ ? MF_CHECKED : 0),
                kTrayPauseCommand,
                manualPlaybackPaused_ ? L"继续动态壁纸" : L"暂停动态壁纸");
    AppendMenuW(menu, MF_STRING | (assignments_.empty() ? MF_GRAYED : 0),
                kTrayCancelCommand, L"取消应用当前壁纸");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出");

    if (controlledTestMode_) {
        DestroyMenu(menu);
        core::LogInfo(
            L"CONTROLLED_TRAY_MENU=show,import,sound,pause,cancel,exit");
        return;
    }

    const bool controlWasVisible = IsWindowVisible(controlWindow_) != FALSE;
    SetForegroundWindow(controlWindow_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
                                                 TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    PostMessageW(controlWindow_, WM_NULL, 0, 0);
    if (!controlWasVisible) {
        ShowWindow(controlWindow_, SW_HIDE);
    }

    if (command == kTrayShowCommand) {
        ShowControlWindow();
    } else if (command == kTrayImportCommand) {
        ShowControlWindow();
        ChooseImport();
    } else if (command == kTraySoundCommand) {
        ToggleSound();
    } else if (command == kTrayPauseCommand) {
        ToggleManualPlaybackPause();
    } else if (command == kTrayCancelCommand) {
        CancelActiveWallpaper();
    } else if (command == kTrayExitCommand) {
        RequestExit();
    }
}

void WallpaperApplication::ResizeRendererToWindow() {
    const std::scoped_lock playbackLock(playbackMutex_);
    if (wallpaperWindow_ == nullptr) {
        return;
    }
    RECT client{};
    if (!GetClientRect(wallpaperWindow_, &client)) {
        return;
    }
    const UINT width = static_cast<UINT>(client.right);
    const UINT height = static_cast<UINT>(client.bottom);
    if (renderer_.IsInitialized() && !renderer_.Resize(width, height)) {
        return;
    }
    if (!assignments_.empty()) {
        ConfigureWallpaperWindowRegion();
    }
    for (const auto& session : playbackSessions_) {
        session->destinations = DestinationsForAssignment(session->assignment);
        if (session->kind == media::WallpaperKind::StaticImage) {
            RenderStaticImage(session->assignment.wallpaperPath,
                              session->destinations);
        } else if (session->gifPlayer) {
            session->gifPlayer->Resize(width, height);
            session->gifPlayer->SetTargetRects(session->destinations);
        }
    }
}

void WallpaperApplication::RequestExit() {
    running_ = false;
    if (controlWindow_ != nullptr) {
        PostMessageW(controlWindow_, WM_NULL, 0, 0);
    }
}

void WallpaperApplication::Shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    running_ = false;
    if (controlWindow_ != nullptr) {
        KillTimer(controlWindow_, kPlaybackPolicyTimer);
        KillTimer(controlWindow_, kExplorerRecoveryTimer);
        KillTimer(controlWindow_, kResourceUsageTimer);
    }
    if (displayPowerNotification_ != nullptr) {
        UnregisterPowerSettingNotification(displayPowerNotification_);
        displayPowerNotification_ = nullptr;
    }
    if (sessionNotificationsRegistered_ && controlWindow_ != nullptr) {
        WTSUnRegisterSessionNotification(controlWindow_);
        sessionNotificationsRegistered_ = false;
    }
    RemoveTrayIcon();
    StopPlaybackRenderThread();
    StopAllPlayback();
    renderer_.Shutdown();

    if (wallpaperWindow_ != nullptr && IsWindow(wallpaperWindow_)) {
        ShowWindow(wallpaperWindow_, SW_HIDE);
        DestroyWindow(wallpaperWindow_);
    }
    wallpaperWindow_ = nullptr;
    if (desktopCompatibilityMutex_ != nullptr) {
        // The mutex must remain owned, not merely present. DeskGo waits on it
        // to detect shutdown; an unowned mutex is immediately signalled and
        // makes its static background repeatedly hide/show, causing flashing.
        if (desktopCompatibilityMutexOwned_) {
            ReleaseMutex(desktopCompatibilityMutex_);
        }
        CloseHandle(desktopCompatibilityMutex_);
        desktopCompatibilityMutex_ = nullptr;
        desktopCompatibilityMutexOwned_ = false;
    }
    if (controlWindow_ != nullptr && IsWindow(controlWindow_)) {
        DragAcceptFiles(controlWindow_, FALSE);
        DestroyWindow(controlWindow_);
    }
    controlWindow_ = nullptr;

    if (mediaFoundationStarted_) {
        MFShutdown();
        mediaFoundationStarted_ = false;
    }
}

LRESULT CALLBACK WallpaperApplication::WindowProcedure(const HWND window,
                                                        const UINT message,
                                                        const WPARAM wParam,
                                                        const LPARAM lParam) {
    WallpaperApplication* application = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        application = static_cast<WallpaperApplication*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(application));
    } else {
        application = reinterpret_cast<WallpaperApplication*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return application != nullptr
               ? application->HandleWindowMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT WallpaperApplication::HandleWindowMessage(const HWND window,
                                                   const UINT message,
                                                   const WPARAM wParam,
                                                   const LPARAM lParam) {
    if (window == controlWindow_ && taskbarCreatedMessage_ != 0 &&
        message == taskbarCreatedMessage_) {
        trayIconAdded_ = false;
        AddTrayIcon();
        SetTimer(controlWindow_, kExplorerRecoveryTimer, 1000, nullptr);
        return 0;
    }

    if (window == controlWindow_) {
        switch (message) {
            case WM_COMMAND: {
                const WORD identifier = LOWORD(wParam);
                const WORD notification = HIWORD(wParam);
                if (identifier == kTrayPauseCommand) {
                    ToggleManualPlaybackPause();
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledTestExitCommand) {
                    RequestExit();
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledLibraryDrawCountCommand) {
                    core::LogInfo(
                        L"CONTROLLED_LIBRARY_DRAW_COUNT=" +
                        std::to_wstring(mainWindow_.LibraryDrawCount()));
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledFrameCountCommand) {
                    core::LogInfo(
                        L"CONTROLLED_VIDEO_TRANSFER_COUNT=" +
                        std::to_wstring(VideoTransferredFrameCount()));
                    return 0;
                }
                if (controlledTestMode_ &&
                    identifier == kControlledTestSaveCommand) {
                    const HRESULT result = SaveCurrentSelection();
                    if (SUCCEEDED(result)) {
                        core::LogInfo(L"CONTROLLED_SETTINGS_SAVE=True");
                    } else {
                        core::LogError(L"Controlled settings save failed.", result);
                    }
                    return 0;
                }
                if (mainWindow_.HandleFilterCommand(identifier, notification)) {
                    return 0;
                }
                if (identifier == ModernMainWindow::DisplayModeChanged &&
                    notification == BN_CLICKED) {
                    ApplyDisplayModeFromUi();
                    return 0;
                }
                if (identifier == ModernMainWindow::CancelSelectedWallpaper &&
                    notification == BN_CLICKED) {
                    const HWND source = reinterpret_cast<HWND>(lParam);
                    const std::optional selected =
                        source == mainWindow_.ActiveLibraryControl()
                            ? mainWindow_.SelectedActiveItem()
                            : mainWindow_.SelectedItem();
                    if (selected.has_value()) {
                        CancelWallpaper(*selected);
                    }
                    return 0;
                }
                mainWindow_.CloseTransientUi();
                if (identifier == ModernMainWindow::Import &&
                    notification == BN_CLICKED) {
                    ChooseImport();
                } else if (identifier == ModernMainWindow::Export &&
                           notification == BN_CLICKED) {
                    ChooseExport();
                } else if (identifier == ModernMainWindow::Library &&
                           notification == LBN_DBLCLK) {
                    ApplySelectedWallpaper();
                } else if (identifier == ModernMainWindow::Sound &&
                           notification == BN_CLICKED) {
                    ToggleSound();
                } else if (identifier == ModernMainWindow::RenameCommit &&
                           notification == BN_CLICKED) {
                    CommitWallpaperRename();
                }
                return 0;
            }

            case WM_CONTEXTMENU:
                if (reinterpret_cast<HWND>(wParam) ==
                    mainWindow_.LibraryControl()) {
                    ShowLibraryContextMenu(
                        POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                    return 0;
                }
                if (reinterpret_cast<HWND>(wParam) ==
                    mainWindow_.ActiveLibraryControl()) {
                    ShowActiveWallpaperContextMenu(
                        POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                    return 0;
                }
                break;

            case WM_DRAWITEM:
                if (mainWindow_.DrawItem(
                        *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                    return TRUE;
                }
                break;

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            case WM_CTLCOLORSTATIC:
                return reinterpret_cast<LRESULT>(mainWindow_.ColorControl(
                    reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));

            case WM_PAINT: {
                PAINTSTRUCT paint{};
                const HDC context = BeginPaint(controlWindow_, &paint);
                RECT client{};
                GetClientRect(controlWindow_, &client);
                const int width = client.right - client.left;
                const int height = client.bottom - client.top;
                const HDC bufferContext = CreateCompatibleDC(context);
                const HBITMAP buffer =
                    width > 0 && height > 0
                        ? CreateCompatibleBitmap(context, width, height)
                        : nullptr;
                if (bufferContext != nullptr && buffer != nullptr) {
                    const HGDIOBJ previous = SelectObject(bufferContext, buffer);
                    mainWindow_.Paint(bufferContext, paint.rcPaint);
                    BitBlt(context, paint.rcPaint.left, paint.rcPaint.top,
                           paint.rcPaint.right - paint.rcPaint.left,
                           paint.rcPaint.bottom - paint.rcPaint.top, bufferContext,
                           paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
                    SelectObject(bufferContext, previous);
                } else {
                    mainWindow_.Paint(context, paint.rcPaint);
                }
                if (buffer != nullptr) {
                    DeleteObject(buffer);
                }
                if (bufferContext != nullptr) {
                    DeleteDC(bufferContext);
                }
                EndPaint(controlWindow_, &paint);
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_CLOSE:
                if (trayIconAdded_) {
                    ShowWindow(controlWindow_, SW_HIDE);
                } else {
                    RequestExit();
                }
                return 0;

            case WM_SIZE:
                mainWindow_.Layout();
                return 0;

            case WM_GETMINMAXINFO: {
                auto* minimum = reinterpret_cast<MINMAXINFO*>(lParam);
                const UINT dpi = GetDpiForWindow(controlWindow_);
                minimum->ptMinTrackSize.x = MulDiv(860, dpi, 96);
                minimum->ptMinTrackSize.y = MulDiv(590, dpi, 96);
                return 0;
            }

            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(controlWindow_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
                mainWindow_.DpiChanged();
                return 0;
            }

            case WM_DROPFILES: {
                const HDROP drop = reinterpret_cast<HDROP>(wParam);
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                std::vector<std::wstring> paths;
                for (UINT index = 0; index < count; ++index) {
                    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                    std::wstring path(length + 1U, L'\0');
                    DragQueryFileW(drop, index, path.data(), length + 1U);
                    path.resize(length);
                    paths.push_back(std::move(path));
                }
                DragFinish(drop);
                ImportPaths(paths);
                return 0;
            }

            case WM_TIMER:
                if (wParam == kExplorerRecoveryTimer) {
                    KillTimer(controlWindow_, kExplorerRecoveryTimer);
                    ReattachToDesktop();
                    return 0;
                }
                if (wParam == kPlaybackPolicyTimer) {
                    RefreshPlaybackPolicy();
                    return 0;
                }
                if (wParam == kResourceUsageTimer) {
                    UpdateResourceUsage();
                    return 0;
                }
                if (wParam == ModernMainWindow::AnimationTimerId) {
                    mainWindow_.HandleAnimationTimer();
                    return 0;
                }
                break;

            case WM_WTSSESSION_CHANGE:
                if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK) {
                    sessionLocked_ = wParam == WTS_SESSION_LOCK;
                    RefreshPlaybackPolicy();
                }
                return 0;

            case WM_POWERBROADCAST:
                if (wParam == PBT_APMSUSPEND) {
                    systemSuspended_ = true;
                    RefreshPlaybackPolicy();
                } else if (wParam == PBT_APMRESUMEAUTOMATIC ||
                           wParam == PBT_APMRESUMESUSPEND) {
                    systemSuspended_ = false;
                    RefreshPlaybackPolicy();
                } else if (wParam == PBT_POWERSETTINGCHANGE && lParam != 0) {
                    const auto* setting =
                        reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                    if (IsEqualGUID(setting->PowerSetting,
                                    GUID_CONSOLE_DISPLAY_STATE) &&
                        setting->DataLength >= sizeof(DWORD)) {
                        const DWORD displayState =
                            *reinterpret_cast<const DWORD*>(setting->Data);
                        displayOff_ = displayState == 0;
                        RefreshPlaybackPolicy();
                    }
                }
                return TRUE;

            case WM_DISPLAYCHANGE:
                ReattachToDesktop();
                return 0;

            case kMediaEngineEventMessage:
                {
                    const std::scoped_lock playbackLock(playbackMutex_);
                    if (WallpaperSession* session = FindSession(
                            static_cast<std::uint32_t>(
                                static_cast<std::uint64_t>(wParam) >> 32U));
                        session != nullptr && session->videoPlayer) {
                        session->videoPlayer->HandleEvent(
                            static_cast<DWORD>(static_cast<std::uint64_t>(wParam) &
                                               0xffffffffULL),
                            static_cast<std::uint32_t>(lParam));
                        if (session->videoPlayer->IsPlaying()) {
                            mainWindow_.SetStatus(ActivePlaybackStatus());
                            WakePlaybackRenderThread();
                        }
                    }
                }
                return 0;

            case kPlaybackFailureMessage:
                playbackFailurePending_ = false;
                RemoveFailedPlaybackSessions();
                WakePlaybackRenderThread();
                return 0;

            case kPlaybackFatalMessage:
                runtimeExitCode_ = 1;
                RequestExit();
                return 0;

            case kRevealWallpaperMessage:
                if (IsWindow(wallpaperWindow_)) {
                    ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
                }
                return 0;

            case kTrayCallbackMessage:
                if (LOWORD(lParam) == WM_LBUTTONUP) {
                    ShowControlWindow();
                } else if (LOWORD(lParam) == WM_RBUTTONUP ||
                           LOWORD(lParam) == WM_CONTEXTMENU) {
                    ShowTrayMenu();
                }
                return 0;

            case WM_DESTROY:
                controlWindow_ = nullptr;
                if (!shuttingDown_) {
                    running_ = false;
                }
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (window == wallpaperWindow_) {
        switch (message) {
            case WM_SIZE:
                if (wParam != SIZE_MINIMIZED) {
                    ResizeRendererToWindow();
                }
                return 0;
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_DESTROY:
                wallpaperWindow_ = nullptr;
                StopAllPlayback();
                renderer_.Shutdown();
                if (!shuttingDown_ && controlWindow_ != nullptr) {
                    core::LogWarning(
                        L"Wallpaper window was destroyed; scheduling one recovery.");
                    SetTimer(controlWindow_, kExplorerRecoveryTimer, 1000, nullptr);
                }
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace lwe::app
