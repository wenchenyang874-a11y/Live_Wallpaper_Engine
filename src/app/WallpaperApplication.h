#pragma once

#include <chrono>

#include <windows.h>

#include "render/D3DRenderer.h"
#include "shell/DesktopHost.h"

namespace lwe::app {

class WallpaperApplication final {
public:
    explicit WallpaperApplication(HINSTANCE instance);
    ~WallpaperApplication();

    WallpaperApplication(const WallpaperApplication&) = delete;
    WallpaperApplication& operator=(const WallpaperApplication&) = delete;

    int Run(std::chrono::seconds testDuration);

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                            LPARAM lParam);
    LRESULT HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWallpaperClass();
    bool CreateWallpaperWindow();
    bool ReattachToDesktop();
    void ResizeRendererToWindow();
    void Shutdown();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool running_ = false;
    shell::DesktopTarget desktopTarget_{};
    render::D3DRenderer renderer_;
};

}  // namespace lwe::app
