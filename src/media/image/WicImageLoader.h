#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <windows.h>

namespace lwe::media::image {

struct DecodedImage final {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<std::uint8_t> pixels;
};

class WicImageLoader final {
public:
    HRESULT LoadFill(std::wstring_view imagePath, UINT targetWidth, UINT targetHeight,
                     DecodedImage& image) const;
    HRESULT ScaleFillBgra(std::span<const std::uint8_t> sourcePixels,
                          UINT sourceWidth, UINT sourceHeight, UINT sourceStride,
                          UINT targetWidth, UINT targetHeight, DecodedImage& image) const;
};

}  // namespace lwe::media::image
