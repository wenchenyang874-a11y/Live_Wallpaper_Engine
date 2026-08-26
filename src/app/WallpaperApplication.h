#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include "app/ModernMainWindow.h"
#include "app/UpdateChecker.h"
#include "core/SettingsStore.h"
#include "core/WallpaperLibrary.h"
#include "media/MediaTypes.h"
#include "media/image/GifPlayer.h"
#include "media/image/WicImageLoader.h"
#include "media/video/MediaEnginePlayer.h"
#include "platform/ProcessResourceMonitor.h"
#include "render/D3DRenderer.h"
#include "shell/DesktopHost.h"

namespace lwe::app {

class WallpaperApplication final {
public:
    WallpaperApplication(HINSTANCE instance, HANDLE activationEvent);
    ~WallpaperApplication();

    WallpaperApplication(const WallpaperApplication&) = delete;
    WallpaperApplication& operator=(const WallpaperApplication&) = delete;

    int Run(std::chrono::seconds testDuration,
            const std::vector<std::wstring>& testWallpapers,
            updates::UpdateCheckMode updateCheckMode =
                updates::UpdateCheckMode::Live);

private:
    enum class PlaybackMode {
        Stopped,
        TechnicalTest,
        Active,
    };

    struct WallpaperSession final {
        std::uint32_t token = 0;
        core::WallpaperAssignmentSetting assignment;
        media::WallpaperKind kind = media::WallpaperKind::StaticImage;
        std::vector<RECT> destinations;
        std::unique_ptr<media::image::GifPlayer> gifPlayer;
        std::unique_ptr<media::video::MediaEnginePlayer> videoPlayer;
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                            LPARAM lParam);
    LRESULT HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClasses();
    bool CreateControlWindow();
    bool CreateUpdateButtonWindow();
    bool CreateWallpaperWindow();
    bool EnsureRenderer();
    bool ReattachToDesktop();
    bool RecoverWallpaperWindow();
    bool AddTrayIcon();
    void RemoveTrayIcon();

    bool RestoreSavedWallpaperSelection();
    void RefreshLibrary();
    void ChooseImport();
    void ImportPaths(const std::vector<std::wstring>& paths);
    void ChooseExport();
    void ExportWallpapers(const std::vector<core::WallpaperItem>& items);
    void OpenWallpaperLocation(const core::WallpaperItem& item);
    void RemoveWallpaperFromLibrary(const core::WallpaperItem& item);
    void CommitLibraryOrder();
    void ApplySelectedWallpaper();
    void PreviewSelectedWallpaper();
    void PreviewWallpaper(const core::WallpaperItem& item);
    void CommitWallpaperRename();
    void ShowLibraryContextMenu(POINT screenPoint);
    void ShowActiveWallpaperContextMenu(POINT screenPoint);
    void CancelActiveWallpaper(bool persistSelection = true);
    void CancelWallpaper(const core::WallpaperItem& item,
                         bool persistSelection = true);
    bool ApplyWallpaperWithTargetPrompt(std::wstring_view path,
                                        bool persistSelection = true,
                                        bool showErrors = true);
    bool ApplyWallpaper(std::wstring_view path, bool persistSelection = true,
                        bool showErrors = true);
    bool NormalizeAssignments();
    bool RebuildPlaybackSessions(bool showErrors);
    HRESULT StartWallpaperSession(WallpaperSession& session);
    HRESULT RenderStaticImage(std::wstring_view path,
                              std::span<const RECT> destinations);
    void StopAllPlayback();
    bool RemoveFailedPlaybackSessions();
    void ToggleSound();
    void ToggleManualPlaybackPause();
    HRESULT SaveCurrentSelection() const;
    void RefreshDisplayTargets(bool preserveSelection);
    void ApplyDisplayModeFromUi();
    bool ChooseApplicationTargets();
    bool ConfigureWallpaperWindowRegion();
    std::vector<RECT> DestinationsForAssignment(
        const core::WallpaperAssignmentSetting& assignment) const;
    std::vector<std::wstring> ActiveWallpaperPaths() const;
    std::vector<ModernMainWindow::ActiveWallpaperInfo> ActiveWallpapers() const;
    std::wstring ActivePlaybackStatus() const;
    bool HasDynamicPlayback() const;
    std::uint64_t VideoTransferredFrameCount() const;
    bool RenderPlaybackFrame();
    void StartPlaybackRenderThread();
    void StopPlaybackRenderThread();
    void WakePlaybackRenderThread();
    WallpaperSession* FindSession(std::uint32_t token);
    void UpdateResourceUsage();
    void InitializePlaybackPolicy();
    void RefreshPlaybackPolicy();
    std::wstring PlaybackPauseReason() const;

    void ShowControlWindow();
    void ShowTrayMenu();
    [[nodiscard]] RECT UpdateButtonRectangle() const;
    void PositionUpdateButtonWindow() const;
    void RedrawUpdateButton() const;
    void BeginUpdateCheck();
    void CompleteUpdateCheck();
    void StopUpdateCheck();
    void ResizeRendererToWindow();
    void RequestExit();
    void Shutdown();

    HINSTANCE instance_ = nullptr;
    HANDLE activationEvent_ = nullptr;
    HANDLE desktopCompatibilityMutex_ = nullptr;
    bool desktopCompatibilityMutexOwned_ = false;
    HWND controlWindow_ = nullptr;
    HWND updateButtonWindow_ = nullptr;
    HWND wallpaperWindow_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool running_ = false;
    bool shuttingDown_ = false;
    bool trayIconAdded_ = false;
    bool updateCheckInProgress_ = false;
    bool mediaFoundationStarted_ = false;
    bool controlledTestMode_ = false;
    bool sessionLocked_ = false;
    bool systemSuspended_ = false;
    bool displayOff_ = false;
    bool fullScreenActive_ = false;
    bool dynamicPlaybackPaused_ = false;
    bool manualPlaybackPaused_ = false;
    bool pendingWallpaperReveal_ = false;
    std::atomic_int runtimeExitCode_{0};
    std::atomic_bool playbackFailurePending_{false};
    bool sessionNotificationsRegistered_ = false;
    HPOWERNOTIFY displayPowerNotification_ = nullptr;
    bool soundEnabled_ = false;
    PlaybackMode playbackMode_ = PlaybackMode::TechnicalTest;
    shell::DesktopTarget desktopTarget_{};
    std::vector<shell::DisplayTarget> displayTargets_;
    std::vector<std::wstring> selectedDisplayIds_;
    std::vector<core::WallpaperAssignmentSetting> assignments_;
    std::vector<std::unique_ptr<WallpaperSession>> playbackSessions_;
    mutable std::recursive_mutex playbackMutex_;
    std::condition_variable_any playbackWake_;
    std::jthread playbackRenderThread_;
    std::jthread updateCheckThread_;
    std::mutex updateCheckMutex_;
    std::optional<updates::UpdateCheckResult> pendingUpdateResult_;
    updates::UpdateCheckMode updateCheckMode_ = updates::UpdateCheckMode::Live;
    std::uint32_t nextSessionToken_ = 1;
    bool spanAcrossDisplays_ = true;
    ModernMainWindow mainWindow_;
    core::SettingsStore settingsStore_;
    core::WallpaperLibrary wallpaperLibrary_;
    media::image::WicImageLoader imageLoader_;
    render::D3DRenderer renderer_;
    platform::ProcessResourceMonitor resourceMonitor_;
};

}  // namespace lwe::app
