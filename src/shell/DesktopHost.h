#pragma once

#include <optional>
#include <string_view>

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

std::optional<DesktopTarget> FindDesktopTarget();
bool AttachWallpaperWindow(HWND wallpaperWindow, const DesktopTarget& target);
std::wstring_view LayoutName(DesktopLayout layout);

}  // namespace lwe::shell
