#include "shell/DesktopHost.h"

#include <cstdint>
#include <sstream>

#include "core/Logger.h"

namespace lwe::shell {
namespace {

constexpr UINT kSpawnWorkerMessage = 0x052C;
constexpr LONG_PTR kNoRedirectionBitmap = 0x00200000L;

struct ClassicSearchContext {
    HWND worker = nullptr;
};

std::wstring HandleText(const HWND window) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase
           << reinterpret_cast<std::uintptr_t>(window);
    return stream.str();
}

BOOL CALLBACK FindClassicWorker(const HWND topLevelWindow, const LPARAM parameter) {
    auto* context = reinterpret_cast<ClassicSearchContext*>(parameter);
    const HWND shellView = FindWindowExW(topLevelWindow, nullptr, L"SHELLDLL_DefView", nullptr);
    if (shellView == nullptr) {
        return TRUE;
    }

    // On the classic desktop, the wallpaper WorkerW is the top-level sibling that
    // immediately follows the window containing SHELLDLL_DefView in Z order.
    const HWND worker = FindWindowExW(nullptr, topLevelWindow, L"WorkerW", nullptr);
    if (worker != nullptr) {
        context->worker = worker;
        return FALSE;
    }
    return TRUE;
}

HWND FindClassicWallpaperWorker() {
    ClassicSearchContext context{};
    EnumWindows(&FindClassicWorker, reinterpret_cast<LPARAM>(&context));
    return context.worker;
}

void RequestWallpaperWorker(const HWND progman) {
    DWORD_PTR messageResult = 0;

    // Windows 11 raised desktop requires wParam=0xD and lParam=0x1. We only send
    // the undocumented shell message after confirming that no suitable WorkerW
    // already exists, because re-sending it can visibly rebuild the desktop.
    const LRESULT result = SendMessageTimeoutW(
        progman, kSpawnWorkerMessage, static_cast<WPARAM>(0xD), static_cast<LPARAM>(0x1),
        SMTO_ABORTIFHUNG, 1000, &messageResult);
    if (result == 0) {
        core::LogWarning(L"Explorer did not acknowledge the WorkerW request.");
    }
}

bool SetChildStyle(const HWND window) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR oldStyle = GetWindowLongPtrW(window, GWL_STYLE);
    if (oldStyle == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }

    const LONG_PTR newStyle = (oldStyle & ~static_cast<LONG_PTR>(WS_POPUP)) |
                              static_cast<LONG_PTR>(WS_CHILD) |
                              static_cast<LONG_PTR>(WS_CLIPCHILDREN) |
                              static_cast<LONG_PTR>(WS_CLIPSIBLINGS);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, GWL_STYLE, newStyle);
    return previous != 0 || GetLastError() == ERROR_SUCCESS;
}

}  // namespace

std::optional<DesktopTarget> FindDesktopTarget() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman == nullptr) {
        core::LogError(L"Progman desktop window was not found.");
        return std::nullopt;
    }

    const LONG_PTR progmanExStyle = GetWindowLongPtrW(progman, GWL_EXSTYLE);
    const bool isRaisedDesktop = (progmanExStyle & kNoRedirectionBitmap) != 0;

    if (isRaisedDesktop) {
        HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        if (worker == nullptr) {
            RequestWallpaperWorker(progman);
            worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        }

        if (worker != nullptr) {
            core::LogInfo(L"Raised desktop WorkerW selected: " + HandleText(worker));
            return DesktopTarget{worker, nullptr, DesktopLayout::Raised};
        }
    }

    HWND classicWorker = FindClassicWallpaperWorker();
    if (classicWorker == nullptr) {
        RequestWallpaperWorker(progman);
        classicWorker = FindClassicWallpaperWorker();
    }
    if (classicWorker != nullptr) {
        core::LogInfo(L"Classic desktop WorkerW selected: " + HandleText(classicWorker));
        return DesktopTarget{classicWorker, nullptr, DesktopLayout::Classic};
    }

    // Progman is a last-resort fallback. The icon view is used as a Z-order
    // anchor so our child remains below icons instead of covering the desktop.
    const HWND shellView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    core::LogWarning(L"No WorkerW wallpaper surface was found; using Progman fallback.");
    return DesktopTarget{progman, shellView, DesktopLayout::ProgmanFallback};
}

bool AttachWallpaperWindow(const HWND wallpaperWindow, const DesktopTarget& target) {
    if (!IsWindow(wallpaperWindow) || !IsWindow(target.parent)) {
        core::LogError(L"Wallpaper or desktop parent window is invalid.");
        return false;
    }

    if (!SetChildStyle(wallpaperWindow)) {
        core::LogError(L"Failed to apply child window style.", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const HWND oldParent = SetParent(wallpaperWindow, target.parent);
    if (oldParent == nullptr && GetLastError() != ERROR_SUCCESS) {
        core::LogError(L"SetParent failed while attaching the wallpaper window.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    // A fully opaque layered child is required by Windows 11's raised desktop
    // compositor. Alpha 255 preserves the rendered pixels while allowing DWM to
    // place this child under the layered icon view.
    if (!SetLayeredWindowAttributes(wallpaperWindow, 0, 255, LWA_ALPHA)) {
        core::LogError(L"SetLayeredWindowAttributes failed.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    RECT parentClient{};
    if (!GetClientRect(target.parent, &parentClient)) {
        core::LogError(L"GetClientRect failed for the desktop surface.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    const int width = parentClient.right - parentClient.left;
    const int height = parentClient.bottom - parentClient.top;
    if (width <= 0 || height <= 0) {
        core::LogError(L"Desktop surface has an invalid client size.");
        return false;
    }

    const HWND insertAfter = target.zOrderAnchor != nullptr ? target.zOrderAnchor : HWND_BOTTOM;
    const UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED;
    if (!SetWindowPos(wallpaperWindow, insertAfter, 0, 0, width, height, flags)) {
        core::LogError(L"SetWindowPos failed while sizing the wallpaper window.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    std::wostringstream message;
    message << L"Wallpaper attached using " << LayoutName(target.layout) << L" layout at "
            << width << L'x' << height << L'.';
    core::LogInfo(message.str());
    return true;
}

std::wstring_view LayoutName(const DesktopLayout layout) {
    switch (layout) {
        case DesktopLayout::Raised:
            return L"raised";
        case DesktopLayout::Classic:
            return L"classic";
        case DesktopLayout::ProgmanFallback:
            return L"Progman-fallback";
    }
    return L"unknown";
}

}  // namespace lwe::shell
