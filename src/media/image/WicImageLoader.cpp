#include "media/image/WicImageLoader.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include <wincodec.h>
#include <wrl/client.h>

#include "core/Logger.h"

namespace lwe::media::image {
namespace {

HRESULT CreateWicFactory(Microsoft::WRL::ComPtr<IWICImagingFactory>& factory) {
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    }
    return result;
}

bool IsSupportedContainer(const GUID& container) {
    return IsEqualGUID(container, GUID_ContainerFormatJpeg) ||
           IsEqualGUID(container, GUID_ContainerFormatPng) ||
           IsEqualGUID(container, GUID_ContainerFormatBmp);
}

HRESULT ScaleSource(IWICImagingFactory* factory, IWICBitmapSource* source,
                    const UINT sourceWidth, const UINT sourceHeight,
                    const UINT targetWidth, const UINT targetHeight,
                    DecodedImage& image, const core::WallpaperOptions& options) {
    Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
    HRESULT result = factory->CreateBitmapClipper(&clipper);
    if (FAILED(result)) {
        return result;
    }
    const auto placement = core::CalculatePlacement(sourceWidth, sourceHeight, targetWidth, targetHeight, options);
    WICRect crop{static_cast<INT>(placement.x), static_cast<INT>(placement.y),
                 static_cast<INT>(placement.width), static_cast<INT>(placement.height)};
    result = clipper->Initialize(source, &crop);
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    result = factory->CreateBitmapScaler(&scaler);
    if (FAILED(result)) {
        return result;
    }
    result = scaler->Initialize(clipper.Get(), placement.outputWidth, placement.outputHeight,
                                WICBitmapInterpolationModeFant);
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) {
        return result;
    }
    result = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return result;
    }

    const std::uint64_t stride = static_cast<std::uint64_t>(targetWidth) * 4U;
    const std::uint64_t byteCount = stride * targetHeight;
    if (stride > std::numeric_limits<UINT>::max() ||
        byteCount > std::numeric_limits<UINT>::max() ||
        byteCount > std::numeric_limits<std::size_t>::max()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    image.width = targetWidth;
    image.height = targetHeight;
    image.stride = static_cast<UINT>(stride);
    image.pixels.resize(static_cast<std::size_t>(byteCount));
    if (placement.outputWidth != targetWidth || placement.outputHeight != targetHeight) {
        std::fill(image.pixels.begin(), image.pixels.end(), std::uint8_t{0});
        for (std::size_t alpha = 3; alpha < image.pixels.size(); alpha += 4) image.pixels[alpha] = 255;
    }
    const UINT offset = placement.top * image.stride + placement.left * 4;
    result = converter->CopyPixels(nullptr, image.stride, static_cast<UINT>(byteCount) - offset,
                                   image.pixels.data() + offset);
    if (FAILED(result)) {
        image = {};
    }
    return result;
}

}  // namespace

HRESULT WicImageLoader::LoadFill(const std::wstring_view imagePath, const UINT targetWidth,
                                 const UINT targetHeight, DecodedImage& image, const core::WallpaperOptions& options) const {
    image = {};
    if (imagePath.empty() || targetWidth == 0 || targetHeight == 0) {
        return E_INVALIDARG;
    }

    const std::wstring path(imagePath);
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT result = CreateWicFactory(factory);
    if (FAILED(result)) {
        core::LogError(L"Unable to create the WIC imaging factory.", result);
        return result;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(result)) {
        core::LogError(L"WIC could not decode the selected image file.", result);
        return result;
    }

    GUID container{};
    result = decoder->GetContainerFormat(&container);
    if (FAILED(result)) {
        return result;
    }
    if (!IsSupportedContainer(container)) {
        core::LogWarning(L"The selected file is not a supported JPEG, PNG, or BMP container.");
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        return result;
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    result = frame->GetSize(&sourceWidth, &sourceHeight);
    if (FAILED(result)) {
        return result;
    }
    if (sourceWidth == 0 || sourceHeight == 0 ||
        sourceWidth > static_cast<UINT>(std::numeric_limits<INT>::max()) ||
        sourceHeight > static_cast<UINT>(std::numeric_limits<INT>::max())) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    result = ScaleSource(factory.Get(), frame.Get(), sourceWidth, sourceHeight, targetWidth,
                         targetHeight, image, options);
    if (FAILED(result)) {
        return result;
    }

    core::LogInfo(L"Decoded and scaled a static wallpaper through WIC: " + path);
    return S_OK;
}

HRESULT WicImageLoader::ScaleFillBgra(
    const std::span<const std::uint8_t> sourcePixels, const UINT sourceWidth,
    const UINT sourceHeight, const UINT sourceStride, const UINT targetWidth,
    const UINT targetHeight, DecodedImage& image, const core::WallpaperOptions& options) const {
    image = {};
    const std::uint64_t sourceBytes =
        static_cast<std::uint64_t>(sourceStride) * sourceHeight;
    if (sourceWidth == 0 || sourceHeight == 0 || targetWidth == 0 || targetHeight == 0 ||
        sourceStride < static_cast<std::uint64_t>(sourceWidth) * 4U ||
        sourceBytes > sourcePixels.size() || sourceBytes > std::numeric_limits<UINT>::max()) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT result = CreateWicFactory(factory);
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    result = factory->CreateBitmapFromMemory(
        sourceWidth, sourceHeight, GUID_WICPixelFormat32bppBGRA, sourceStride,
        static_cast<UINT>(sourceBytes), const_cast<BYTE*>(sourcePixels.data()), &bitmap);
    if (FAILED(result)) {
        return result;
    }
    return ScaleSource(factory.Get(), bitmap.Get(), sourceWidth, sourceHeight, targetWidth,
                       targetHeight, image, options);
}

}  // namespace lwe::media::image
