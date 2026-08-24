#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "app/ModernMainWindow.h"
#include "core/SettingsStore.h"
#include "core/WallpaperLibrary.h"
#include "media/MediaTypes.h"
#include "media/image/GifPlayer.h"
#include "media/image/WicImageLoader.h"
#include "media/video/MediaEnginePlayer.h"
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
            const std::optional<std::wstring>& testWallpaper);

private:
    enum class PlaybackMode {
        TechnicalTest,
        StaticImage,
        AnimatedGif,
        Video,
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                            LPARAM lParam);
    LRESULT HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClasses();
    bool CreateControlWindow();
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
    void ApplySelectedWallpaper();
    bool ApplyWallpaper(std::wstring_view path, bool persistSelection = true,
                        bool showErrors = true);
    HRESULT ApplyStaticImage(std::wstring_view path);
    HRESULT ApplyAnimatedGif(std::wstring_view path);
    HRESULT ApplyVideo(std::wstring_view path);
    HRESULT RenderStaticImage(std::wstring_view path);
    void StopActivePlayback();
    void ToggleSound();
    HRESULT SaveCurrentSelection() const;
    void InitializePlaybackPolicy();
    void RefreshPlaybackPolicy();
    std::wstring PlaybackPauseReason() const;

    void ShowControlWindow();
    void ShowTrayMenu();
    void ResizeRendererToWindow();
    void RequestExit();
    void Shutdown();

    HINSTANCE instance_ = nullptr;
    HANDLE activationEvent_ = nullptr;
    HANDLE desktopCompatibilityMutex_ = nullptr;
    bool desktopCompatibilityMutexOwned_ = false;
    HWND controlWindow_ = nullptr;
    HWND wallpaperWindow_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool running_ = false;
    bool shuttingDown_ = false;
    bool trayIconAdded_ = false;
    bool mediaFoundationStarted_ = false;
    bool controlledTestMode_ = false;
    bool sessionLocked_ = false;
    bool systemSuspended_ = false;
    bool displayOff_ = false;
    bool fullScreenActive_ = false;
    bool dynamicPlaybackPaused_ = false;
    bool sessionNotificationsRegistered_ = false;
    HPOWERNOTIFY displayPowerNotification_ = nullptr;
    bool soundEnabled_ = false;
    PlaybackMode playbackMode_ = PlaybackMode::TechnicalTest;
    media::WallpaperKind activeKind_ = media::WallpaperKind::StaticImage;
    std::wstring activeWallpaperPath_;
    shell::DesktopTarget desktopTarget_{};
    ModernMainWindow mainWindow_;
    core::SettingsStore settingsStore_;
    core::WallpaperLibrary wallpaperLibrary_;
    media::image::WicImageLoader imageLoader_;
    media::image::GifPlayer gifPlayer_;
    media::video::MediaEnginePlayer videoPlayer_;
    render::D3DRenderer renderer_;
};

}  // namespace lwe::app
