#pragma once
#include <algorithm>
#include <cmath>
#include <string>

namespace lwe::core {
enum class FitMode { Fill, Fit, Stretch, Center };
struct WallpaperOptions final {
    FitMode fit = FitMode::Fill;
    unsigned focusX = 50, focusY = 50;
    unsigned volume = 100;
    bool audioAllowed = true;
};
struct WallpaperPreference final {
    std::wstring path;
    WallpaperOptions options;
};
struct ImagePlacement final {
    unsigned x = 0, y = 0, width = 1, height = 1;
    unsigned left = 0, top = 0, outputWidth = 1, outputHeight = 1;
};
// One pixel-rounded placement definition is shared by WIC, GIF and GPU video.
inline ImagePlacement CalculatePlacement(unsigned sw, unsigned sh, unsigned tw,
                                          unsigned th, const WallpaperOptions& o) {
    sw = std::max(1U, sw); sh = std::max(1U, sh);
    tw = std::max(1U, tw); th = std::max(1U, th);
    ImagePlacement p{0, 0, sw, sh, 0, 0, tw, th};
    if (o.fit == FitMode::Fill) {
        const double scale = std::max(double(tw) / sw, double(th) / sh);
        p.width = std::clamp(unsigned(std::lround(tw / scale)), 1U, sw);
        p.height = std::clamp(unsigned(std::lround(th / scale)), 1U, sh);
    } else if (o.fit == FitMode::Fit) {
        const double scale = std::min(double(tw) / sw, double(th) / sh);
        p.outputWidth = std::clamp(unsigned(std::lround(sw * scale)), 1U, tw);
        p.outputHeight = std::clamp(unsigned(std::lround(sh * scale)), 1U, th);
    } else if (o.fit == FitMode::Center) {
        p.width = p.outputWidth = std::min(sw, tw);
        p.height = p.outputHeight = std::min(sh, th);
    }
    p.x = unsigned(std::lround(double(sw - p.width) * std::min(o.focusX, 100U) / 100.0));
    p.y = unsigned(std::lround(double(sh - p.height) * std::min(o.focusY, 100U) / 100.0));
    p.left = (tw - p.outputWidth) / 2;
    p.top = (th - p.outputHeight) / 2;
    return p;
}
} // namespace lwe::core
