#include "media/image/GifPlayer.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <propvarutil.h>

#include "core/Logger.h"
#include "render/D3DRenderer.h"

namespace lwe::media::image {
namespace {

constexpr std::uint64_t kMaximumCanvasPixels = 64ULL * 1024ULL * 1024ULL;
constexpr UINT kMaximumFrames = 10000;
constexpr UINT kMinimumFrameDelayMilliseconds = 20;
constexpr UINT kDefaultFrameDelayMilliseconds = 100;

std::optional<UINT> MetadataUnsigned(IWICMetadataQueryReader* reader,
                                     const wchar_t* query) {
    if (reader == nullptr) {
        return std::nullopt;
    }

    PROPVARIANT value{};
    PropVariantInit(&value);
    const HRESULT result = reader->GetMetadataByName(query, &value);
    std::optional<UINT> parsed;
    if (SUCCEEDED(result)) {
        switch (value.vt) {
            case VT_UI1:
                parsed = value.bVal;
                break;
            case VT_UI2:
                parsed = value.uiVal;
                break;
            case VT_UI4:
                parsed = value.ulVal;
                break;
            case VT_I2:
                if (value.iVal >= 0) {
                    parsed = static_cast<UINT>(value.iVal);
                }
                break;
            case VT_I4:
                if (value.lVal >= 0) {
                    parsed = static_cast<UINT>(value.lVal);
                }
                break;
            default:
                break;
        }
    }
    PropVariantClear(&value);
    return parsed;
}

GifPlayer::FrameMetadata ReadFrameMetadata(IWICBitmapFrameDecode* frame,
                                           const UINT frameWidth,
                                           const UINT frameHeight) {
    GifPlayer::FrameMetadata metadata;
    metadata.width = frameWidth;
    metadata.height = frameHeight;

    Microsoft::WRL::ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) {
        return metadata;
    }

    metadata.left = MetadataUnsigned(reader.Get(), L"/imgdesc/Left").value_or(0);
    metadata.top = MetadataUnsigned(reader.Get(), L"/imgdesc/Top").value_or(0);
    metadata.width = MetadataUnsigned(reader.Get(), L"/imgdesc/Width").value_or(frameWidth);
    metadata.height =
        MetadataUnsigned(reader.Get(), L"/imgdesc/Height").value_or(frameHeight);
    const UINT delayCentiseconds =
        MetadataUnsigned(reader.Get(), L"/grctlext/Delay").value_or(10);
    metadata.delayMilliseconds = std::max(
        kMinimumFrameDelayMilliseconds,
        delayCentiseconds == 0 ? kDefaultFrameDelayMilliseconds
                              : delayCentiseconds * 10U);
    metadata.disposal =
        MetadataUnsigned(reader.Get(), L"/grctlext/Disposal").value_or(0);
    return metadata;
}

HRESULT CreateWicFactory(Microsoft::WRL::ComPtr<IWICImagingFactory>& factory) {
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    }
    return result;
}

}  // namespace

HRESULT GifPlayer::Load(const std::wstring_view path, const UINT targetWidth,
                        const UINT targetHeight) {
    Reset();
    if (path.empty() || targetWidth == 0 || targetHeight == 0) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT result = CreateWicFactory(factory);
    if (FAILED(result)) {
        return result;
    }

    const std::wstring filePath(path);
    result = factory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder_);
    if (FAILED(result)) {
        return result;
    }

    GUID container{};
    result = decoder_->GetContainerFormat(&container);
    if (FAILED(result) || !IsEqualGUID(container, GUID_ContainerFormatGif)) {
        Reset();
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    result = decoder_->GetFrameCount(&frameCount_);
    if (FAILED(result) || frameCount_ == 0 || frameCount_ > kMaximumFrames) {
        Reset();
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> firstFrame;
    result = decoder_->GetFrame(0, &firstFrame);
    if (FAILED(result)) {
        Reset();
        return result;
    }
    result = firstFrame->GetSize(&canvasWidth_, &canvasHeight_);
    if (FAILED(result) || canvasWidth_ == 0 || canvasHeight_ == 0 ||
        static_cast<std::uint64_t>(canvasWidth_) * canvasHeight_ >
            kMaximumCanvasPixels) {
        Reset();
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    // GIF logical-screen metadata is authoritative when it is available. Some
    // optimized files expose a smaller first frame than their complete canvas.
    Microsoft::WRL::ComPtr<IWICMetadataQueryReader> decoderMetadata;
    if (SUCCEEDED(decoder_->GetMetadataQueryReader(&decoderMetadata))) {
        canvasWidth_ = MetadataUnsigned(decoderMetadata.Get(), L"/logscrdesc/Width")
                           .value_or(canvasWidth_);
        canvasHeight_ = MetadataUnsigned(decoderMetadata.Get(), L"/logscrdesc/Height")
                            .value_or(canvasHeight_);

        const std::optional<UINT> backgroundIndex = MetadataUnsigned(
            decoderMetadata.Get(), L"/logscrdesc/BackgroundColorIndex");
        if (backgroundIndex.has_value()) {
            Microsoft::WRL::ComPtr<IWICPalette> palette;
            if (SUCCEEDED(factory->CreatePalette(&palette)) &&
                SUCCEEDED(decoder_->CopyPalette(palette.Get()))) {
                UINT colorCount = 0;
                if (SUCCEEDED(palette->GetColorCount(&colorCount)) &&
                    *backgroundIndex < colorCount) {
                    std::vector<WICColor> colors(colorCount);
                    UINT copied = 0;
                    if (SUCCEEDED(palette->GetColors(colorCount, colors.data(), &copied)) &&
                        *backgroundIndex < copied) {
                        const WICColor color = colors[*backgroundIndex];
                        backgroundBgra_ = {
                            static_cast<std::uint8_t>(color & 0xffU),
                            static_cast<std::uint8_t>((color >> 8U) & 0xffU),
                            static_cast<std::uint8_t>((color >> 16U) & 0xffU),
                            0xffU};
                    }
                }
            }
        }
    }
    if (canvasWidth_ == 0 || canvasHeight_ == 0 ||
        static_cast<std::uint64_t>(canvasWidth_) * canvasHeight_ >
            kMaximumCanvasPixels) {
        Reset();
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    const std::uint64_t canvasBytes =
        static_cast<std::uint64_t>(canvasWidth_) * canvasHeight_ * 4U;
    if (canvasBytes > std::numeric_limits<std::size_t>::max()) {
        Reset();
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    canvas_.resize(static_cast<std::size_t>(canvasBytes));
    FillBackground(0, 0, canvasWidth_, canvasHeight_);
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;
    nextFrameIndex_ = 0;
    nextFrameAt_ = std::chrono::steady_clock::time_point::min();
    core::LogInfo(L"Loaded an animated GIF with " + std::to_wstring(frameCount_) +
                  L" frames: " + filePath);
    return S_OK;
}

bool GifPlayer::PresentDue(render::D3DRenderer& renderer,
                           const std::chrono::steady_clock::time_point now) {
    if (!IsLoaded() || now < nextFrameAt_) {
        return true;
    }

    UINT delayMilliseconds = kDefaultFrameDelayMilliseconds;
    const HRESULT result = ComposeFrame(nextFrameIndex_, delayMilliseconds);
    if (FAILED(result)) {
        core::LogError(L"Unable to decode the next GIF frame.", result);
        return false;
    }

    const std::vector<RECT> fallback{
        RECT{0, 0, static_cast<LONG>(targetWidth_), static_cast<LONG>(targetHeight_)}};
    const std::vector<RECT>& destinations =
        targetRects_.empty() ? fallback : targetRects_;
    std::vector<DecodedImage> scaledFrames;
    scaledFrames.reserve(destinations.size());
    const UINT canvasStride = canvasWidth_ * 4U;
    HRESULT scaleResult = S_OK;
    for (const RECT& destination : destinations) {
        const LONG width = destination.right - destination.left;
        const LONG height = destination.bottom - destination.top;
        if (width <= 0 || height <= 0) {
            continue;
        }
        DecodedImage scaled;
        scaleResult = scaler_.ScaleFillBgra(
            canvas_, canvasWidth_, canvasHeight_, canvasStride,
            static_cast<UINT>(width), static_cast<UINT>(height), scaled);
        if (FAILED(scaleResult)) {
            break;
        }
        scaledFrames.push_back(std::move(scaled));
    }
    std::vector<render::ImageRegion> regions;
    regions.reserve(scaledFrames.size());
    for (std::size_t index = 0; index < scaledFrames.size(); ++index) {
        const DecodedImage& scaled = scaledFrames[index];
        const RECT& destination = destinations[index];
        regions.push_back(render::ImageRegion{scaled.pixels, scaled.width,
                                              scaled.height, scaled.stride,
                                              destination.left, destination.top});
    }
    if (FAILED(scaleResult) || regions.empty() ||
        !renderer.PresentImageRegions(regions)) {
        core::LogError(L"Unable to scale or present a GIF frame.", scaleResult);
        return false;
    }

    nextFrameIndex_ = (nextFrameIndex_ + 1U) % frameCount_;
    nextFrameAt_ = now + std::chrono::milliseconds(delayMilliseconds);
    if (presentedFrames_ == 0) {
        playbackStartedAt_ = now;
    }
    ++presentedFrames_;
    return true;
}

DWORD GifPlayer::WaitMilliseconds(
    const std::chrono::steady_clock::time_point now) const noexcept {
    if (!IsLoaded()) {
        return INFINITE;
    }
    if (now >= nextFrameAt_) {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(nextFrameAt_ - now);
    return static_cast<DWORD>(std::clamp<std::int64_t>(remaining.count(), 1, 60000));
}

void GifPlayer::Resize(const UINT targetWidth, const UINT targetHeight) {
    if (targetWidth == 0 || targetHeight == 0) {
        return;
    }
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;
    nextFrameAt_ = std::chrono::steady_clock::time_point::min();
}

void GifPlayer::SetTargetRects(std::vector<RECT> targetRects) {
    targetRects_ = std::move(targetRects);
    nextFrameAt_ = std::chrono::steady_clock::time_point::min();
}

void GifPlayer::Reset() {
    if (presentedFrames_ > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - playbackStartedAt_);
        core::LogInfo(L"GIF playback statistics: rendered=" +
                      std::to_wstring(presentedFrames_) + L", elapsed_ms=" +
                      std::to_wstring(elapsed.count()));
    }
    decoder_.Reset();
    canvasWidth_ = 0;
    canvasHeight_ = 0;
    targetWidth_ = 0;
    targetHeight_ = 0;
    targetRects_.clear();
    frameCount_ = 0;
    nextFrameIndex_ = 0;
    previousFrame_ = {};
    backgroundBgra_ = {};
    canvas_.clear();
    previousCanvas_.clear();
    nextFrameAt_ = {};
    playbackStartedAt_ = {};
    presentedFrames_ = 0;
}

bool GifPlayer::IsLoaded() const noexcept {
    return decoder_ && frameCount_ > 0 && !canvas_.empty();
}

std::uint32_t GifPlayer::FrameCount() const noexcept {
    return frameCount_;
}

HRESULT GifPlayer::ComposeFrame(const UINT frameIndex, UINT& delayMilliseconds) {
    if (!IsLoaded() || frameIndex >= frameCount_) {
        return E_INVALIDARG;
    }

    if (frameIndex == 0) {
        FillBackground(0, 0, canvasWidth_, canvasHeight_);
        previousCanvas_.clear();
        previousFrame_ = {};
    } else {
        ApplyPreviousDisposal();
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT result = decoder_->GetFrame(frameIndex, &frame);
    if (FAILED(result)) {
        return result;
    }

    UINT frameWidth = 0;
    UINT frameHeight = 0;
    result = frame->GetSize(&frameWidth, &frameHeight);
    if (FAILED(result) || frameWidth == 0 || frameHeight == 0) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    const FrameMetadata metadata = ReadFrameMetadata(frame.Get(), frameWidth, frameHeight);
    if (metadata.left >= canvasWidth_ || metadata.top >= canvasHeight_) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    result = CreateWicFactory(factory);
    if (FAILED(result)) {
        return result;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
    }
    if (FAILED(result)) {
        return result;
    }

    const std::uint64_t stride = static_cast<std::uint64_t>(frameWidth) * 4U;
    const std::uint64_t byteCount = stride * frameHeight;
    if (byteCount > std::numeric_limits<UINT>::max()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byteCount));
    result = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                   static_cast<UINT>(byteCount), pixels.data());
    if (FAILED(result)) {
        return result;
    }

    if (metadata.disposal == 3) {
        previousCanvas_ = canvas_;
    } else {
        previousCanvas_.clear();
    }
    BlendFrame(pixels, frameWidth, frameHeight, static_cast<UINT>(stride), metadata);
    previousFrame_ = metadata;
    delayMilliseconds = metadata.delayMilliseconds;
    return S_OK;
}

void GifPlayer::ApplyPreviousDisposal() {
    if (previousFrame_.disposal == 2) {
        const UINT right =
            std::min(canvasWidth_, previousFrame_.left + previousFrame_.width);
        const UINT bottom =
            std::min(canvasHeight_, previousFrame_.top + previousFrame_.height);
        FillBackground(previousFrame_.left, previousFrame_.top, right, bottom);
    } else if (previousFrame_.disposal == 3 &&
               previousCanvas_.size() == canvas_.size()) {
        canvas_ = previousCanvas_;
    }
    previousCanvas_.clear();
}

void GifPlayer::FillBackground(const UINT left, const UINT top, const UINT right,
                               const UINT bottom) {
    const UINT clippedRight = std::min(right, canvasWidth_);
    const UINT clippedBottom = std::min(bottom, canvasHeight_);
    for (UINT y = std::min(top, canvasHeight_); y < clippedBottom; ++y) {
        for (UINT x = std::min(left, canvasWidth_); x < clippedRight; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * canvasWidth_ + x) * 4U;
            std::copy(backgroundBgra_.begin(), backgroundBgra_.end(),
                      canvas_.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
}

void GifPlayer::BlendFrame(const std::span<const std::uint8_t> pixels, const UINT width,
                           const UINT height, const UINT stride,
                           const FrameMetadata& metadata) {
    const UINT copyWidth =
        std::min({width, metadata.width, canvasWidth_ - metadata.left});
    const UINT copyHeight =
        std::min({height, metadata.height, canvasHeight_ - metadata.top});
    for (UINT y = 0; y < copyHeight; ++y) {
        for (UINT x = 0; x < copyWidth; ++x) {
            const std::size_t sourceOffset = static_cast<std::size_t>(y) * stride + x * 4U;
            const std::size_t destinationOffset =
                (static_cast<std::size_t>(metadata.top + y) * canvasWidth_ +
                 metadata.left + x) *
                4U;
            const UINT alpha = pixels[sourceOffset + 3];
            if (alpha == 0) {
                continue;
            }
            for (UINT channel = 0; channel < 3; ++channel) {
                const UINT source = pixels[sourceOffset + channel];
                const UINT destination = canvas_[destinationOffset + channel];
                canvas_[destinationOffset + channel] = static_cast<std::uint8_t>(
                    (source * alpha + destination * (255U - alpha) + 127U) / 255U);
            }
            canvas_[destinationOffset + 3] = 255;
        }
    }
}

}  // namespace lwe::media::image
