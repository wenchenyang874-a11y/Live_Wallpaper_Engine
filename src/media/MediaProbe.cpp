#include "media/MediaProbe.h"

#include <array>
#include <string>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "core/Logger.h"

namespace lwe::media {
namespace {

using Microsoft::WRL::ComPtr;

HRESULT ProbeWicImage(const std::wstring& path, MediaInfo& info) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    }
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(result)) {
        return result;
    }

    GUID container{};
    result = decoder->GetContainerFormat(&container);
    if (FAILED(result)) {
        return result;
    }

    if (IsEqualGUID(container, GUID_ContainerFormatJpeg)) {
        info.kind = WallpaperKind::StaticImage;
        info.formatLabel = L"JPEG";
    } else if (IsEqualGUID(container, GUID_ContainerFormatPng)) {
        info.kind = WallpaperKind::StaticImage;
        info.formatLabel = L"PNG";
    } else if (IsEqualGUID(container, GUID_ContainerFormatBmp)) {
        info.kind = WallpaperKind::StaticImage;
        info.formatLabel = L"BMP";
    } else if (IsEqualGUID(container, GUID_ContainerFormatGif)) {
        info.kind = WallpaperKind::AnimatedGif;
        info.formatLabel = L"GIF";
    } else {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        return result;
    }
    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    if (SUCCEEDED(result)) {
        info.width = width;
        info.height = height;
    }
    return result;
}

bool IsSupportedVideoSubtype(const GUID& subtype) {
    constexpr std::array<const GUID*, 10> supported{
        &MFVideoFormat_H264, &MFVideoFormat_H264_ES, &MFVideoFormat_WMV1,
        &MFVideoFormat_WMV2, &MFVideoFormat_WMV3,    &MFVideoFormat_WVC1,
        &MFVideoFormat_MP4V, &MFVideoFormat_M4S2,    &MFVideoFormat_MJPG,
        &MFVideoFormat_MPEG2,
    };
    for (const GUID* candidate : supported) {
        if (IsEqualGUID(subtype, *candidate)) {
            return true;
        }
    }
    return false;
}

std::wstring VideoSubtypeLabel(const GUID& subtype) {
    if (IsEqualGUID(subtype, MFVideoFormat_H264) ||
        IsEqualGUID(subtype, MFVideoFormat_H264_ES)) {
        return L"H.264 video";
    }
    if (IsEqualGUID(subtype, MFVideoFormat_WMV1) ||
        IsEqualGUID(subtype, MFVideoFormat_WMV2) ||
        IsEqualGUID(subtype, MFVideoFormat_WMV3)) {
        return L"Windows Media Video";
    }
    if (IsEqualGUID(subtype, MFVideoFormat_WVC1)) {
        return L"VC-1 video";
    }
    if (IsEqualGUID(subtype, MFVideoFormat_MP4V) ||
        IsEqualGUID(subtype, MFVideoFormat_M4S2)) {
        return L"MPEG-4 video";
    }
    if (IsEqualGUID(subtype, MFVideoFormat_MJPG)) {
        return L"Motion JPEG video";
    }
    if (IsEqualGUID(subtype, MFVideoFormat_MPEG2)) {
        return L"MPEG-2 video";
    }
    return L"Video";
}

HRESULT ProbeMediaFoundationVideo(const std::wstring& path, MediaInfo& info) {
    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(result)) {
        result = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    }
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IMFMediaType> nativeVideoType;
    result = reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
        &nativeVideoType);
    if (FAILED(result)) {
        return result;
    }

    GUID majorType{};
    GUID subtype{};
    result = nativeVideoType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
    if (SUCCEEDED(result)) {
        result = nativeVideoType->GetGUID(MF_MT_SUBTYPE, &subtype);
    }
    if (FAILED(result) || !IsEqualGUID(majorType, MFMediaType_Video) ||
        !IsSupportedVideoSubtype(subtype)) {
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(MFGetAttributeSize(nativeVideoType.Get(), MF_MT_FRAME_SIZE, &width,
                                     &height))) {
        info.width = width;
        info.height = height;
    }
    UINT32 frameRateNumerator = 0;
    UINT32 frameRateDenominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(nativeVideoType.Get(), MF_MT_FRAME_RATE,
                                      &frameRateNumerator,
                                      &frameRateDenominator))) {
        info.frameRateNumerator = frameRateNumerator;
        info.frameRateDenominator = frameRateDenominator;
    }
    UINT32 averageBitrate = 0;
    if (SUCCEEDED(nativeVideoType->GetUINT32(MF_MT_AVG_BITRATE,
                                             &averageBitrate))) {
        info.averageVideoBitrate = averageBitrate;
    }

    ComPtr<IMFMediaType> nativeAudioType;
    info.hasAudio = SUCCEEDED(reader->GetNativeMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
        &nativeAudioType));
    info.kind = WallpaperKind::Video;
    info.videoSubtype = subtype;
    info.formatLabel = VideoSubtypeLabel(subtype);
    return S_OK;
}

}  // namespace

std::wstring_view WallpaperKindLabel(const WallpaperKind kind) noexcept {
    switch (kind) {
        case WallpaperKind::StaticImage:
            return L"静态图片";
        case WallpaperKind::AnimatedGif:
            return L"动态 GIF";
        case WallpaperKind::Video:
            return L"视频";
    }
    return L"未知";
}

HRESULT ProbeMediaFile(const std::wstring_view path, MediaInfo& info) {
    info = {};
    if (path.empty()) {
        return E_INVALIDARG;
    }

    const std::wstring filePath(path);
    HRESULT imageResult = ProbeWicImage(filePath, info);
    if (SUCCEEDED(imageResult)) {
        return S_OK;
    }

    HRESULT videoResult = ProbeMediaFoundationVideo(filePath, info);
    if (SUCCEEDED(videoResult)) {
        return S_OK;
    }

    core::LogWarning(L"The selected file is not a supported image, GIF, or video: " +
                     filePath);
    return videoResult == MF_E_PLATFORM_NOT_INITIALIZED ? videoResult
                                                        : HRESULT_FROM_WIN32(
                                                              ERROR_NOT_SUPPORTED);
}

}  // namespace lwe::media
