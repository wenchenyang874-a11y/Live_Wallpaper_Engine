#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "media/image/WicImageLoader.h"

namespace lwe::render {
class D3DRenderer;
}

namespace lwe::media::image {

class GifPlayer final {
public:
    HRESULT Load(std::wstring_view path, UINT targetWidth, UINT targetHeight);
    bool PresentDue(render::D3DRenderer& renderer,
                    std::chrono::steady_clock::time_point now);
    DWORD WaitMilliseconds(std::chrono::steady_clock::time_point now) const noexcept;
    void Resize(UINT targetWidth, UINT targetHeight);
    void SetTargetRects(std::vector<RECT> targetRects);
    void Reset();

    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::uint32_t FrameCount() const noexcept;

public:
    struct FrameMetadata final {
        UINT left = 0;
        UINT top = 0;
        UINT width = 0;
        UINT height = 0;
        UINT delayMilliseconds = 100;
        UINT disposal = 0;
    };

private:
    HRESULT ComposeFrame(UINT frameIndex, UINT& delayMilliseconds);
    void ApplyPreviousDisposal();
    void FillBackground(UINT left, UINT top, UINT right, UINT bottom);
    void BlendFrame(std::span<const std::uint8_t> pixels, UINT width, UINT height,
                    UINT stride, const FrameMetadata& metadata);

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder_;
    UINT canvasWidth_ = 0;
    UINT canvasHeight_ = 0;
    UINT targetWidth_ = 0;
    UINT targetHeight_ = 0;
    std::vector<RECT> targetRects_;
    UINT frameCount_ = 0;
    UINT nextFrameIndex_ = 0;
    FrameMetadata previousFrame_{};
    std::array<std::uint8_t, 4> backgroundBgra_{};
    std::vector<std::uint8_t> canvas_;
    std::vector<std::uint8_t> previousCanvas_;
    std::chrono::steady_clock::time_point nextFrameAt_{};
    std::chrono::steady_clock::time_point playbackStartedAt_{};
    std::uint64_t presentedFrames_ = 0;
    WicImageLoader scaler_;
};

}  // namespace lwe::media::image
