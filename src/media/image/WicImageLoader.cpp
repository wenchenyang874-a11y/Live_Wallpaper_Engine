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

WICRect CalculateFillCrop(const UINT sourceWidth, const UINT sourceHeight,
                          const UINT targetWidth, const UINT targetHeight) {
    WICRect crop{0, 0, static_cast<INT>(sourceWidth), static_cast<INT>(sourceHeight)};
    const std::uint64_t sourceScaled =
        static_cast<std::uint64_t>(sourceWidth) * targetHeight;
    const std::uint64_t targetScaled =
        static_cast<std::uint64_t>(targetWidth) * sourceHeight;

    if (sourceScaled > targetScaled) {
        const UINT croppedWidth = std::max<UINT>(
            1, static_cast<UINT>(static_cast<std::uint64_t>(sourceHeight) * targetWidth /
                                 targetHeight));
        crop.X = static_cast<INT>((sourceWidth - croppedWidth) / 2U);
        crop.Width = static_cast<INT>(croppedWidth);
    } else if (sourceScaled < targetScaled) {
        const UINT croppedHeight = std::max<UINT>(
            1, static_cast<UINT>(static_cast<std::uint64_t>(sourceWidth) * targetHeight /
                                 targetWidth));
        crop.Y = static_cast<INT>((sourceHeight - croppedHeight) / 2U);
        crop.Height = static_cast<INT>(croppedHeight);
    }
    return crop;
}

HRESULT ScaleSource(IWICImagingFactory* factory, IWICBitmapSource* source,
                    const UINT sourceWidth, const UINT sourceHeight,
                    const UINT targetWidth, const UINT targetHeight,
                    DecodedImage& image) {
    Microsoft::WRL::ComPtr<IWICBitmapClipper> clipper;
    HRESULT result = factory->CreateBitmapClipper(&clipper);
    if (FAILED(result)) {
        return result;
    }
    WICRect crop = CalculateFillCrop(sourceWidth, sourceHeight, targetWidth, targetHeight);
    result = clipper->Initialize(source, &crop);
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    result = factory->CreateBitmapScaler(&scaler);
    if (FAILED(result)) {
        return result;
    }
    result = scaler->Initialize(clipper.Get(), targetWidth, targetHeight,
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
    result = converter->CopyPixels(nullptr, image.stride, static_cast<UINT>(byteCount),
                                   image.pixels.data());
    if (FAILED(result)) {
        image = {};
    }
    return result;
}

}  // namespace

HRESULT WicImageLoader::LoadFill(const std::wstring_view imagePath, const UINT targetWidth,
                                 const UINT targetHeight, DecodedImage& image) const {
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
                         targetHeight, image);
    if (FAILED(result)) {
        return result;
    }

    core::LogInfo(L"Decoded and scaled a static wallpaper through WIC: " + path);
    return S_OK;
}

HRESULT WicImageLoader::ScaleFillBgra(
    const std::span<const std::uint8_t> sourcePixels, const UINT sourceWidth,
    const UINT sourceHeight, const UINT sourceStride, const UINT targetWidth,
    const UINT targetHeight, DecodedImage& image) const {
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
                       targetHeight, image);
}

}  // namespace lwe::media::image
