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
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMediaEngineEventMessage = WM_APP + 2;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kExplorerRecoveryTimer = 1;
constexpr UINT_PTR kPlaybackPolicyTimer = 2;
constexpr UINT_PTR kResourceUsageTimer = 3;
constexpr int kTrayImportCommand = 2100;
constexpr int kTrayShowCommand = 2101;
constexpr int kTraySoundCommand = 2102;
constexpr int kTrayExitCommand = 2103;
constexpr int kTrayCancelCommand = 2104;
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

std::wstring PlaybackStatus(const media::WallpaperKind kind, const bool soundEnabled,
                            const std::wstring_view path) {
    std::wstring status = L"正在使用 · ";
    status += media::WallpaperKindLabel(kind);
    if (kind == media::WallpaperKind::Video) {
        status += soundEnabled ? L" · 声音已开启" : L" · 默认静音";
    }
    status += L" · ";
    status += std::filesystem::path(path).filename().native();
    return status;
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
                              const std::optional<std::wstring>& testWallpaper) {
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
    resourceMonitor_.Initialize();
    SetTimer(controlWindow_, kResourceUsageTimer, 1000, nullptr);
    UpdateResourceUsage();

    bool wallpaperApplied = false;
    if (testWallpaper.has_value()) {
        wallpaperApplied = ApplyWallpaper(*testWallpaper, false, false);
        if (!wallpaperApplied) {
            Shutdown();
            return 1;
        }
    } else {
        wallpaperApplied = RestoreSavedWallpaperSelection();
    }
    if (!wallpaperApplied) {
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
        if (playbackMode_ == PlaybackMode::Video && videoPlayer_.HasFailed()) {
            exitCode = 1;
            break;
        }

        if (playbackMode_ == PlaybackMode::TechnicalTest && renderer_.IsInitialized()) {
            if (!renderer_.Render(elapsed)) {
                exitCode = 1;
                break;
            }
            continue;
        }
        if (playbackMode_ == PlaybackMode::AnimatedGif &&
            !dynamicPlaybackPaused_) {
            if (!gifPlayer_.PresentDue(renderer_, now)) {
                exitCode = 1;
                break;
            }
        }
        if (playbackMode_ == PlaybackMode::Video &&
            !dynamicPlaybackPaused_ && videoPlayer_.IsPlaying()) {
            const HRESULT frameResult =
                videoPlayer_.PresentFrame(renderer_, renderDestinations_);
            if (FAILED(frameResult)) {
                exitCode = 1;
                break;
            }
            if (frameResult == S_OK && pendingWallpaperReveal_) {
                ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
                pendingWallpaperReveal_ = false;
            }
        }

        DWORD waitMilliseconds = RemainingTestMilliseconds(testDuration, elapsed);
        if (playbackMode_ == PlaybackMode::AnimatedGif &&
            !dynamicPlaybackPaused_) {
            waitMilliseconds =
                std::min(waitMilliseconds, gifPlayer_.WaitMilliseconds(now));
        }
        if (playbackMode_ == PlaybackMode::Video && !dynamicPlaybackPaused_) {
            waitMilliseconds = std::min(waitMilliseconds, 8UL);
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
    return exitCode;
}

bool WallpaperApplication::RegisterWindowClasses() {
    const HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
    return RegisterApplicationClass(instance_, kControlWindowClass,
                                    &WallpaperApplication::WindowProcedure, nullptr,
                                    icon) &&
           RegisterApplicationClass(instance_, kWallpaperWindowClass,
                                    &WallpaperApplication::WindowProcedure, nullptr,
                                    icon);
}

bool WallpaperApplication::CreateControlWindow() {
    const UINT dpi = GetDpiForSystem();
    RECT windowRectangle{0, 0, MulDiv(1040, dpi, 96), MulDiv(700, dpi, 96)};
    AdjustWindowRectExForDpi(&windowRectangle, WS_OVERLAPPEDWINDOW, FALSE,
                             WS_EX_APPWINDOW, dpi);
    controlWindow_ = CreateWindowExW(
        WS_EX_APPWINDOW, kControlWindowClass, kApplicationTitle, WS_OVERLAPPEDWINDOW,
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
    if (!ConfigureWallpaperWindowRegion() || !EnsureRenderer()) {
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
    const std::wstring path = activeWallpaperPath_;
    if (!CreateWallpaperWindow()) {
        return false;
    }
    if (!path.empty()) {
        return ApplyWallpaper(path, false, false);
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
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, kApplicationTitle);
    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
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
    RefreshDisplayTargets(true);
    ConfigureWallpaperWindowRegion();
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (settings->wallpaperKind == core::WallpaperSelectionKind::DynamicTest) {
        return false;
    }
    if (ApplyWallpaper(settings->wallpaperPath, false, false)) {
        core::LogInfo(L"Restored the saved wallpaper selection.");
        return true;
    }
    mainWindow_.SetStatus(
        L"上次选择的壁纸无法恢复 · 文件可能已移动或当前系统缺少解码器");
    core::LogWarning(L"Unable to restore the saved wallpaper selection.");
    return false;
}

void WallpaperApplication::RefreshLibrary() {
    std::vector<core::WallpaperItem> items = wallpaperLibrary_.Scan();
    if (!activeWallpaperPath_.empty()) {
        const bool alreadyListed = std::ranges::any_of(items, [&](const auto& item) {
            return _wcsicmp(item.path.c_str(), activeWallpaperPath_.c_str()) == 0;
        });
        std::error_code fileError;
        if (!alreadyListed && std::filesystem::is_regular_file(activeWallpaperPath_,
                                                                fileError) &&
            !fileError) {
            media::MediaInfo info;
            if (SUCCEEDED(media::ProbeMediaFile(activeWallpaperPath_, info))) {
                core::WallpaperItem external;
                external.path = activeWallpaperPath_;
                external.displayName =
                    std::filesystem::path(activeWallpaperPath_).filename().native() +
                    L"（外部）";
                external.kind = info.kind;
                external.formatLabel = info.formatLabel;
                external.width = info.width;
                external.height = info.height;
                external.hasAudio = info.hasAudio;
                external.external = true;
                std::error_code error;
                external.fileSize =
                    std::filesystem::file_size(activeWallpaperPath_, error);
                items.insert(items.begin(), std::move(external));
            }
        }
    }
    mainWindow_.SetItems(std::move(items));
    mainWindow_.SetActivePath(activeWallpaperPath_);
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
        ApplyWallpaper(importedItems.front().path.native(), true, true);
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
    ApplyWallpaper(selected->path.native());
}

void WallpaperApplication::PreviewSelectedWallpaper() {
    const std::optional selected = mainWindow_.SelectedItem();
    if (!selected.has_value()) {
        return;
    }
    // Use the registered Windows preview/player so image, GIF and video
    // previews remain codec-aware without starting a second decoder inside the
    // lightweight wallpaper process.
    const HINSTANCE opened = ShellExecuteW(controlWindow_, L"open",
                                           selected->path.c_str(), nullptr,
                                           selected->path.parent_path().c_str(),
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
    const bool wasActive = !activeWallpaperPath_.empty() &&
                           _wcsicmp(activeWallpaperPath_.c_str(),
                                    source.path.c_str()) == 0;
    if (wasActive) {
        StopActivePlayback();
    }
    core::WallpaperItem renamed;
    const HRESULT result = wallpaperLibrary_.Rename(source, rename->second, renamed);
    if (FAILED(result)) {
        if (wasActive) {
            ApplyWallpaper(source.path.native(), false, false);
        }
        std::wstring message = L"壁纸重命名失败。\r\n\r\n";
        message += core::HResultMessage(result);
        MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    if (wasActive) {
        activeWallpaperPath_ = renamed.path.native();
        ApplyWallpaper(activeWallpaperPath_, true, true);
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

void WallpaperApplication::CancelActiveWallpaper(const bool persistSelection) {
    StopActivePlayback();
    playbackMode_ = PlaybackMode::Stopped;
    activeWallpaperPath_.clear();
    if (IsWindow(wallpaperWindow_)) {
        ShowWindow(wallpaperWindow_, SW_HIDE);
    }
    mainWindow_.SetActivePath(L"");
    mainWindow_.SetStatus(L"当前未应用壁纸 · Windows 原壁纸已恢复显示");
    RefreshLibrary();
    if (persistSelection && FAILED(SaveClearedSelection())) {
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
    if (!ConfigureWallpaperWindowRegion()) {
        return false;
    }

    StopActivePlayback();
    switch (info.kind) {
        case media::WallpaperKind::StaticImage:
            result = ApplyStaticImage(path);
            break;
        case media::WallpaperKind::AnimatedGif:
            result = ApplyAnimatedGif(path);
            break;
        case media::WallpaperKind::Video:
            result = ApplyVideo(path);
            break;
    }
    if (FAILED(result)) {
        playbackMode_ = PlaybackMode::TechnicalTest;
        EnsureRenderer();
        if (showErrors) {
            std::wstring message = L"壁纸无法加载或播放。\r\n\r\n";
            message += core::HResultMessage(result);
            MessageBoxW(controlWindow_, message.c_str(), kApplicationTitle,
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }

    activeWallpaperPath_.assign(path);
    activeKind_ = info.kind;
    HRESULT saveResult = S_OK;
    if (persistSelection) {
        saveResult = SaveCurrentSelection();
    }
    mainWindow_.SetActivePath(activeWallpaperPath_);
    mainWindow_.SetStatus(PlaybackStatus(activeKind_, soundEnabled_, path));
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (info.kind != media::WallpaperKind::Video) {
        ShowWindow(wallpaperWindow_, SW_SHOWNOACTIVATE);
    }
    RefreshLibrary();

    if (persistSelection && FAILED(saveResult) && showErrors) {
        MessageBoxW(controlWindow_, L"壁纸已应用，但本地设置保存失败。",
                    kApplicationTitle, MB_OK | MB_ICONWARNING);
    }
    return true;
}

HRESULT WallpaperApplication::ApplyStaticImage(const std::wstring_view path) {
    if (!EnsureRenderer()) {
        return E_FAIL;
    }
    const HRESULT result = RenderStaticImage(path);
    if (SUCCEEDED(result)) {
        playbackMode_ = PlaybackMode::StaticImage;
        core::LogInfo(L"Static overlay mode activated after one presented frame.");
    }
    return result;
}

HRESULT WallpaperApplication::ApplyAnimatedGif(const std::wstring_view path) {
    if (!EnsureRenderer()) {
        return E_FAIL;
    }
    RECT client{};
    if (!GetClientRect(wallpaperWindow_, &client)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT result = gifPlayer_.Load(path, static_cast<UINT>(client.right),
                                     static_cast<UINT>(client.bottom));
    if (SUCCEEDED(result)) {
        gifPlayer_.SetTargetRects(renderDestinations_);
    }
    if (SUCCEEDED(result) &&
        !gifPlayer_.PresentDue(renderer_, std::chrono::steady_clock::now())) {
        result = E_FAIL;
    }
    if (SUCCEEDED(result)) {
        playbackMode_ = PlaybackMode::AnimatedGif;
        core::LogInfo(L"Animated GIF overlay mode activated.");
        RefreshPlaybackPolicy();
    }
    return result;
}

HRESULT WallpaperApplication::ApplyVideo(const std::wstring_view path) {
    if (!mediaFoundationStarted_) {
        return MF_E_PLATFORM_NOT_INITIALIZED;
    }
    if (!EnsureRenderer()) {
        return E_FAIL;
    }
    // Keep the existing swap chain alive while media is prepared. Destroying
    // and recreating the HWND renderer on every video switch exposed the system
    // wallpaper between frames and produced the user's continuous flashing.
    const HRESULT result = videoPlayer_.Open(renderer_.Device(), controlWindow_,
                                              kMediaEngineEventMessage, path,
                                              soundEnabled_);
    if (SUCCEEDED(result)) {
        playbackMode_ = PlaybackMode::Video;
        pendingWallpaperReveal_ = !IsWindowVisible(wallpaperWindow_);
        RefreshPlaybackPolicy();
    }
    return result;
}

HRESULT WallpaperApplication::RenderStaticImage(const std::wstring_view path) {
    if (!renderer_.IsInitialized() || wallpaperWindow_ == nullptr) {
        return E_UNEXPECTED;
    }
    std::vector<media::image::DecodedImage> images;
    images.reserve(renderDestinations_.size());
    HRESULT result = S_OK;
    for (const RECT& destination : renderDestinations_) {
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
        const RECT& destination = renderDestinations_[index];
        regions.push_back(render::ImageRegion{
            image.pixels, image.width, image.height, image.stride,
            destination.left, destination.top});
    }
    if (regions.empty() || !renderer_.PresentImageRegions(regions)) {
        result = E_FAIL;
    }
    return result;
}

void WallpaperApplication::StopActivePlayback() {
    gifPlayer_.Reset();
    videoPlayer_.Shutdown();
    dynamicPlaybackPaused_ = false;
    pendingWallpaperReveal_ = false;
}

void WallpaperApplication::ToggleSound() {
    soundEnabled_ = !soundEnabled_;
    const HRESULT result = videoPlayer_.SetSoundEnabled(soundEnabled_);
    if (FAILED(result)) {
        soundEnabled_ = !soundEnabled_;
        MessageBoxW(controlWindow_, L"无法更改当前视频的声音状态。",
                    kApplicationTitle, MB_OK | MB_ICONERROR);
        return;
    }
    mainWindow_.SetSoundEnabled(soundEnabled_);
    if (!controlledTestMode_ && !activeWallpaperPath_.empty()) {
        SaveCurrentSelection();
        mainWindow_.SetStatus(
            PlaybackStatus(activeKind_, soundEnabled_, activeWallpaperPath_));
    } else {
        mainWindow_.SetStatus(soundEnabled_ ? L"声音已开启 · 将应用到后续视频壁纸"
                                           : L"声音已关闭 · 视频默认静音");
    }
}

HRESULT WallpaperApplication::SaveCurrentSelection() const {
    if (activeWallpaperPath_.empty()) {
        return E_UNEXPECTED;
    }
    core::AppSettings settings;
    settings.wallpaperPath = activeWallpaperPath_;
    settings.soundEnabled = soundEnabled_;
    settings.displayTargets = JoinDisplayIds(selectedDisplayIds_);
    settings.spanAcrossDisplays = spanAcrossDisplays_;
    switch (activeKind_) {
        case media::WallpaperKind::StaticImage:
            settings.wallpaperKind = core::WallpaperSelectionKind::StaticImage;
            break;
        case media::WallpaperKind::AnimatedGif:
            settings.wallpaperKind = core::WallpaperSelectionKind::AnimatedGif;
            break;
        case media::WallpaperKind::Video:
            settings.wallpaperKind = core::WallpaperSelectionKind::Video;
            break;
    }
    return settingsStore_.Save(settings);
}

HRESULT WallpaperApplication::SaveClearedSelection() const {
    core::AppSettings settings;
    settings.wallpaperKind = core::WallpaperSelectionKind::DynamicTest;
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
    UpdateRenderDestinations();
}

void WallpaperApplication::ApplyDisplaySelectionFromUi() {
    selectedDisplayIds_ = mainWindow_.SelectedDisplayIds();
    spanAcrossDisplays_ = mainWindow_.SpanAcrossDisplays();
    if (!ConfigureWallpaperWindowRegion()) {
        mainWindow_.SetStatus(L"无法更新显示器布局");
        return;
    }
    if (!activeWallpaperPath_.empty()) {
        const std::wstring path = activeWallpaperPath_;
        if (ApplyWallpaper(path, false, true)) {
            SaveCurrentSelection();
        }
    } else {
        SaveClearedSelection();
    }
}

void WallpaperApplication::UpdateRenderDestinations() {
    renderDestinations_.clear();
    if (!IsWindow(wallpaperWindow_)) {
        return;
    }
    RECT client{};
    if (!GetClientRect(wallpaperWindow_, &client)) {
        return;
    }
    if (spanAcrossDisplays_) {
        renderDestinations_.push_back(client);
        return;
    }

    for (const shell::DisplayTarget& display : displayTargets_) {
        const bool selected = std::ranges::any_of(
            selectedDisplayIds_, [&](const std::wstring& identifier) {
                return _wcsicmp(identifier.c_str(), display.deviceId.c_str()) == 0;
            });
        RECT clipped{};
        if (selected && IntersectRect(&clipped, &client, &display.clientBounds) &&
            !IsRectEmpty(&clipped)) {
            renderDestinations_.push_back(clipped);
        }
    }
    if (renderDestinations_.empty()) {
        renderDestinations_.push_back(client);
        spanAcrossDisplays_ = true;
    }
}

bool WallpaperApplication::ConfigureWallpaperWindowRegion() {
    if (!IsWindow(wallpaperWindow_)) {
        return false;
    }
    UpdateRenderDestinations();
    if (renderDestinations_.empty()) {
        return false;
    }

    HRGN combined = CreateRectRgn(0, 0, 0, 0);
    if (combined == nullptr) {
        return false;
    }
    for (const RECT& destination : renderDestinations_) {
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
    // SetWindowRgn takes ownership only on success. The complex region allows
    // one decoder and one D3D device to target any subset of monitors while the
    // untouched monitors continue showing the original Windows wallpaper.
    if (SetWindowRgn(wallpaperWindow_, combined, TRUE) == 0) {
        DeleteObject(combined);
        core::LogError(L"Unable to apply the selected display region.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    gifPlayer_.SetTargetRects(renderDestinations_);
    return true;
}

void WallpaperApplication::UpdateResourceUsage() {
    const platform::ProcessResourceUsage usage =
        resourceMonitor_.Sample(renderer_.VideoMemoryUsage());
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

    const bool dynamic = playbackMode_ == PlaybackMode::AnimatedGif ||
                         playbackMode_ == PlaybackMode::Video;
    const std::wstring reason = PlaybackPauseReason();
    const bool shouldPause = dynamic && !reason.empty();
    if (shouldPause == dynamicPlaybackPaused_) {
        return;
    }

    if (playbackMode_ == PlaybackMode::Video &&
        FAILED(videoPlayer_.SetPaused(shouldPause))) {
        core::LogWarning(L"Unable to change video playback for the pause policy.");
        return;
    }
    dynamicPlaybackPaused_ = shouldPause;
    if (shouldPause) {
        mainWindow_.SetStatus(L"动态壁纸已暂停 · " + reason);
        core::LogInfo(L"Dynamic wallpaper paused: " + reason);
    } else {
        mainWindow_.SetStatus(
            PlaybackStatus(activeKind_, soundEnabled_, activeWallpaperPath_));
        core::LogInfo(L"Dynamic wallpaper resumed after pause policy cleared.");
    }
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
    AppendMenuW(menu, MF_STRING | (activeWallpaperPath_.empty() ? MF_GRAYED : 0),
                kTrayCancelCommand, L"取消应用当前壁纸");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出");

    SetForegroundWindow(controlWindow_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
                                                 TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, controlWindow_, nullptr);
    DestroyMenu(menu);
    PostMessageW(controlWindow_, WM_NULL, 0, 0);

    if (command == kTrayShowCommand) {
        ShowControlWindow();
    } else if (command == kTrayImportCommand) {
        ShowControlWindow();
        ChooseImport();
    } else if (command == kTraySoundCommand) {
        ToggleSound();
    } else if (command == kTrayCancelCommand) {
        CancelActiveWallpaper();
    } else if (command == kTrayExitCommand) {
        RequestExit();
    }
}

void WallpaperApplication::ResizeRendererToWindow() {
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
    UpdateRenderDestinations();
    if (playbackMode_ == PlaybackMode::StaticImage &&
        !activeWallpaperPath_.empty()) {
        RenderStaticImage(activeWallpaperPath_);
    } else if (playbackMode_ == PlaybackMode::AnimatedGif) {
        gifPlayer_.Resize(width, height);
        gifPlayer_.SetTargetRects(renderDestinations_);
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
    gifPlayer_.Reset();
    videoPlayer_.Shutdown();
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
                if (controlledTestMode_ &&
                    identifier == kControlledTestExitCommand) {
                    RequestExit();
                    return 0;
                }
                if (mainWindow_.HandleFilterCommand(identifier, notification)) {
                    return 0;
                }
                if (identifier == ModernMainWindow::DisplaySelector &&
                    notification == BN_CLICKED) {
                    if (mainWindow_.ShowDisplaySelectorMenu()) {
                        ApplyDisplaySelectionFromUi();
                    }
                    return 0;
                }
                if (identifier == ModernMainWindow::Import) {
                    ChooseImport();
                } else if (identifier == ModernMainWindow::Export) {
                    ChooseExport();
                } else if (identifier == ModernMainWindow::Apply ||
                           (identifier == ModernMainWindow::Library &&
                            notification == LBN_DBLCLK)) {
                    ApplySelectedWallpaper();
                } else if (identifier == ModernMainWindow::Sound) {
                    ToggleSound();
                } else if (identifier == ModernMainWindow::CancelApplication) {
                    CancelActiveWallpaper();
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
                mainWindow_.Paint(context, paint.rcPaint);
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
                videoPlayer_.HandleEvent(static_cast<DWORD>(wParam),
                                         static_cast<std::uint32_t>(lParam));
                if (videoPlayer_.IsPlaying() &&
                    playbackMode_ == PlaybackMode::Video) {
                    mainWindow_.SetStatus(PlaybackStatus(
                        activeKind_, soundEnabled_, activeWallpaperPath_));
                }
                return 0;

            case kTrayCallbackMessage:
                if (lParam == WM_LBUTTONDBLCLK) {
                    ShowControlWindow();
                } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
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
                gifPlayer_.Reset();
                videoPlayer_.Shutdown();
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
