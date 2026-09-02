#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <guiddef.h>

namespace lwe::media {

enum class WallpaperKind : std::uint32_t {
    StaticImage = 1,
    AnimatedGif = 2,
    Video = 3,
};

struct MediaInfo final {
    WallpaperKind kind = WallpaperKind::StaticImage;
    std::wstring formatLabel;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 0;
    std::uint32_t averageVideoBitrate = 0;
    bool hasAudio = false;
    GUID videoSubtype{};
};

[[nodiscard]] std::wstring_view WallpaperKindLabel(WallpaperKind kind) noexcept;

}  // namespace lwe::media
