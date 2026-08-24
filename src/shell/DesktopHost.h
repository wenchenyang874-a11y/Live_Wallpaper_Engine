#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace lwe::shell {

enum class DesktopLayout {
    Raised,
    Classic,
    ProgmanFallback,
};

struct DesktopTarget {
    HWND parent = nullptr;
    HWND zOrderAnchor = nullptr;
    DesktopLayout layout = DesktopLayout::ProgmanFallback;
};

struct DisplayTarget final {
    std::wstring deviceId;
    std::wstring label;
    RECT clientBounds{};
    bool primary = false;
};

std::optional<DesktopTarget> FindDesktopTarget();
bool AttachWallpaperWindow(HWND wallpaperWindow, const DesktopTarget& target);
std::vector<DisplayTarget> EnumerateDisplayTargets(HWND desktopParent);
std::wstring_view LayoutName(DesktopLayout layout);

}  // namespace lwe::shell
