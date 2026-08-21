#include "app/WallpaperApplication.h"

#include <optional>

#include "core/Logger.h"

namespace lwe::app {
namespace {

constexpr wchar_t kWallpaperWindowClass[] = L"LiveWallpaperEngine.Wallpaper";
constexpr UINT_PTR kExplorerReattachTimer = 1;

}  // namespace

WallpaperApplication::WallpaperApplication(const HINSTANCE instance) : instance_(instance) {}

WallpaperApplication::~WallpaperApplication() {
    Shutdown();
}

int WallpaperApplication::Run(const std::chrono::seconds testDuration) {
    if (!RegisterWallpaperClass() || !CreateWallpaperWindow()) {
        Shutdown();
        return 1;
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    const auto startedAt = std::chrono::steady_clock::now();
    running_ = true;

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

        const auto elapsed = std::chrono::steady_clock::now() - startedAt;
        if (testDuration.count() > 0 && elapsed >= testDuration) {
            core::LogInfo(L"Controlled test duration completed.");
            running_ = false;
            break;
        }

        if (!renderer_.Render(elapsed)) {
            running_ = false;
        }
    }

    Shutdown();
    return 0;
}

bool WallpaperApplication::RegisterWallpaperClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WallpaperApplication::WindowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWallpaperWindowClass;

    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }

    if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        return true;
    }

    core::LogError(L"RegisterClassEx failed.", HRESULT_FROM_WIN32(GetLastError()));
    return false;
}

bool WallpaperApplication::CreateWallpaperWindow() {
    const std::optional target = shell::FindDesktopTarget();
    if (!target.has_value()) {
        return false;
    }
    desktopTarget_ = *target;

    constexpr DWORD extendedStyle =
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    constexpr DWORD style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

    window_ = CreateWindowExW(extendedStyle, kWallpaperWindowClass, L"", style, 0, 0, 1, 1,
                              nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        core::LogError(L"CreateWindowEx failed.", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    if (!shell::AttachWallpaperWindow(window_, desktopTarget_)) {
        return false;
    }
    if (!renderer_.Initialize(window_)) {
        return false;
    }

    ShowWindow(window_, SW_SHOWNOACTIVATE);
    core::LogInfo(L"Wallpaper test renderer started.");
    return true;
}

bool WallpaperApplication::ReattachToDesktop() {
    const std::optional target = shell::FindDesktopTarget();
    if (!target.has_value()) {
        core::LogWarning(L"Desktop reattachment postponed because no target is available.");
        return false;
    }

    if (!shell::AttachWallpaperWindow(window_, *target)) {
        return false;
    }

    desktopTarget_ = *target;
    ResizeRendererToWindow();
    core::LogInfo(L"Wallpaper window reattached after desktop topology change.");
    return true;
}

void WallpaperApplication::ResizeRendererToWindow() {
    if (!renderer_.IsInitialized() || window_ == nullptr) {
        return;
    }

    RECT client{};
    if (GetClientRect(window_, &client)) {
        renderer_.Resize(static_cast<UINT>(client.right - client.left),
                         static_cast<UINT>(client.bottom - client.top));
    }
}

void WallpaperApplication::Shutdown() {
    running_ = false;
    renderer_.Shutdown();

    if (window_ != nullptr && IsWindow(window_)) {
        ShowWindow(window_, SW_HIDE);
        DestroyWindow(window_);
    }
    window_ = nullptr;
}

LRESULT CALLBACK WallpaperApplication::WindowProcedure(const HWND window, const UINT message,
                                                        const WPARAM wParam, const LPARAM lParam) {
    WallpaperApplication* application = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        application = static_cast<WallpaperApplication*>(create->lpCreateParams);
        // CreateWindowEx sends creation messages before it returns the HWND.
        // Store it now so early default processing never receives a null handle.
        application->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
    } else {
        application = reinterpret_cast<WallpaperApplication*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (application != nullptr) {
        return application->HandleWindowMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT WallpaperApplication::HandleWindowMessage(const UINT message, const WPARAM wParam,
                                                   const LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        // Explorer may recreate Progman/WorkerW asynchronously. Delay one second
        // instead of repeatedly polling or manipulating the shell during startup.
        SetTimer(window_, kExplorerReattachTimer, 1000, nullptr);
        return 0;
    }

    switch (message) {
        case WM_TIMER:
            if (wParam == kExplorerReattachTimer) {
                KillTimer(window_, kExplorerReattachTimer);
                ReattachToDesktop();
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            ReattachToDesktop();
            return 0;

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
            BeginPaint(window_, &paint);
            EndPaint(window_, &paint);
            return 0;
        }

        case WM_CLOSE:
            running_ = false;
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace lwe::app
