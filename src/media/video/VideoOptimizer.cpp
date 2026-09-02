#include "media/video/VideoOptimizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include "core/Logger.h"
#include "media/MediaProbe.h"

namespace lwe::media::video {
namespace {

using Microsoft::WRL::ComPtr;

constexpr double kMinimumBitsPerPixelFrame = 0.15;

std::uint32_t EvenDimensionAtLeast(const double value,
                                   const std::uint32_t minimum) {
    auto rounded = static_cast<std::uint32_t>(
        std::max(2.0, std::ceil(value)));
    if ((rounded & 1U) != 0) {
        ++rounded;
    }
    auto floor = std::max(2U, minimum);
    if ((floor & 1U) != 0) {
        ++floor;
    }
    return std::max(rounded, floor);
}

HRESULT CreateMediaSource(const std::wstring& path,
                          ComPtr<IMFMediaSource>& source) {
    ComPtr<IMFSourceResolver> resolver;
    HRESULT result = MFCreateSourceResolver(&resolver);
    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    ComPtr<IUnknown> object;
    if (SUCCEEDED(result)) {
        result = resolver->CreateObjectFromURL(
            path.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr, &objectType,
            &object);
    }
    if (SUCCEEDED(result) && objectType != MF_OBJECT_MEDIASOURCE) {
        result = MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
    }
    if (SUCCEEDED(result)) {
        result = object.As(&source);
    }
    return result;
}

HRESULT CreateVideoAttributes(const VideoOptimizationPlan& plan,
                              ComPtr<IMFAttributes>& attributes) {
    HRESULT result = MFCreateAttributes(&attributes, 9);
    if (SUCCEEDED(result)) {
        result = attributes->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeSize(attributes.Get(), MF_MT_FRAME_SIZE,
                                    plan.outputWidth, plan.outputHeight);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeRatio(attributes.Get(), MF_MT_FRAME_RATE,
                                     plan.frameRateNumerator,
                                     plan.frameRateDenominator);
    }
    if (SUCCEEDED(result)) {
        result = MFSetAttributeRatio(attributes.Get(), MF_MT_PIXEL_ASPECT_RATIO,
                                     1, 1);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MT_INTERLACE_MODE,
                                       MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MT_AVG_BITRATE,
                                       plan.outputBitrate);
    }
    return result;
}

HRESULT CreateAudioAttributes(ComPtr<IMFAttributes>& attributes) {
    HRESULT result = MFCreateAttributes(&attributes, 7);
    if (SUCCEEDED(result)) {
        result = attributes->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
    }
    return result;
}

HRESULT WaitForTranscode(IMFMediaSession* session,
                         const std::stop_token stopToken) {
    bool started = false;
    bool closing = false;
    HRESULT completion = E_FAIL;
    for (;;) {
        if (stopToken.stop_requested() && !closing) {
            completion = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            session->Close();
            closing = true;
        }

        ComPtr<IMFMediaEvent> event;
        const HRESULT eventResult = session->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (eventResult == MF_E_NO_EVENTS_AVAILABLE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        if (FAILED(eventResult)) {
            return eventResult;
        }

        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        event->GetType(&type);
        event->GetStatus(&status);
        if (FAILED(status) && !closing) {
            completion = status;
            session->Close();
            closing = true;
            continue;
        }

        if (type == MESessionTopologyStatus && !started && !closing) {
            UINT32 topologyStatus = MF_TOPOSTATUS_INVALID;
            if (SUCCEEDED(event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS,
                                           &topologyStatus)) &&
                topologyStatus == MF_TOPOSTATUS_READY) {
                PROPVARIANT position;
                PropVariantInit(&position);
                const HRESULT startResult =
                    session->Start(&GUID_NULL, &position);
                PropVariantClear(&position);
                if (FAILED(startResult)) {
                    completion = startResult;
                    session->Close();
                    closing = true;
                } else {
                    started = true;
                }
            }
        } else if (type == MESessionEnded && !closing) {
            completion = S_OK;
            session->Close();
            closing = true;
        } else if (type == MESessionClosed) {
            return completion;
        }
    }
}

}  // namespace

HRESULT PlanVideoOptimization(const std::wstring_view sourcePath,
                              const std::uint32_t maximumDisplayWidth,
                              const std::uint32_t maximumDisplayHeight,
                              VideoOptimizationPlan& plan) {
    plan = {};
    if (sourcePath.empty() || maximumDisplayWidth == 0 ||
        maximumDisplayHeight == 0) {
        return E_INVALIDARG;
    }

    media::MediaInfo info;
    const HRESULT result = media::ProbeMediaFile(sourcePath, info);
    if (FAILED(result) || info.kind != media::WallpaperKind::Video ||
        info.width == 0 || info.height == 0) {
        return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
    }

    plan.sourceWidth = info.width;
    plan.sourceHeight = info.height;
    plan.hasAudio = info.hasAudio;
    const double fillScale = std::min(
        1.0, std::max(static_cast<double>(maximumDisplayWidth) / info.width,
                      static_cast<double>(maximumDisplayHeight) / info.height));
    plan.outputWidth = EvenDimensionAtLeast(
        info.width * fillScale, maximumDisplayWidth);
    plan.outputHeight = EvenDimensionAtLeast(
        info.height * fillScale, maximumDisplayHeight);

    if (info.frameRateNumerator == 0 || info.frameRateDenominator == 0) {
        return MF_E_INVALIDMEDIATYPE;
    }
    plan.frameRateNumerator = info.frameRateNumerator;
    plan.frameRateDenominator = info.frameRateDenominator;

    const std::uint64_t sourcePixels =
        static_cast<std::uint64_t>(info.width) * info.height;
    const std::uint64_t outputPixels =
        static_cast<std::uint64_t>(plan.outputWidth) * plan.outputHeight;
    const bool meaningfulResize = outputPixels < sourcePixels;
    const double pixelRatio = static_cast<double>(outputPixels) / sourcePixels;
    const double sourceBasedBitrate =
        info.averageVideoBitrate > 0
            ? static_cast<double>(info.averageVideoBitrate) * pixelRatio *
                  1.50
            : 0.0;
    const double outputFrameRate =
        static_cast<double>(plan.frameRateNumerator) /
        plan.frameRateDenominator;
    const double qualityFloor = static_cast<double>(outputPixels) *
                                outputFrameRate *
                                kMinimumBitsPerPixelFrame;
    const double estimatedBitrate =
        std::max(sourceBasedBitrate, qualityFloor);
    plan.outputBitrate = static_cast<std::uint32_t>(
        std::clamp(estimatedBitrate, 3'000'000.0, 50'000'000.0));
    plan.needed = meaningfulResize;
    return S_OK;
}

HRESULT OptimizeVideo(const std::wstring_view sourcePath,
                      const std::wstring_view outputPath,
                      const VideoOptimizationPlan& plan,
                      const std::stop_token stopToken) {
    if (sourcePath.empty() || outputPath.empty() || !plan.needed ||
        plan.outputWidth == 0 || plan.outputHeight == 0 ||
        plan.frameRateNumerator == 0 || plan.frameRateDenominator == 0 ||
        plan.outputBitrate == 0) {
        return E_INVALIDARG;
    }

    ComPtr<IMFMediaSource> source;
    HRESULT result = CreateMediaSource(std::wstring(sourcePath), source);
    ComPtr<IMFTranscodeProfile> profile;
    if (SUCCEEDED(result)) {
        result = MFCreateTranscodeProfile(&profile);
    }
    ComPtr<IMFAttributes> videoAttributes;
    if (SUCCEEDED(result)) {
        result = CreateVideoAttributes(plan, videoAttributes);
    }
    if (SUCCEEDED(result)) {
        result = profile->SetVideoAttributes(videoAttributes.Get());
    }
    if (SUCCEEDED(result) && plan.hasAudio) {
        ComPtr<IMFAttributes> audioAttributes;
        result = CreateAudioAttributes(audioAttributes);
        if (SUCCEEDED(result)) {
            result = profile->SetAudioAttributes(audioAttributes.Get());
        }
    }
    ComPtr<IMFAttributes> containerAttributes;
    if (SUCCEEDED(result)) {
        result = MFCreateAttributes(&containerAttributes, 4);
    }
    if (SUCCEEDED(result)) {
        result = containerAttributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                                              MFTranscodeContainerType_MPEG4);
    }
    if (SUCCEEDED(result)) {
        result = containerAttributes->SetUINT32(
            MF_TRANSCODE_TOPOLOGYMODE,
            MF_TRANSCODE_TOPOLOGYMODE_HARDWARE_ALLOWED);
    }
    if (SUCCEEDED(result)) {
        result = containerAttributes->SetUINT32(MF_TRANSCODE_QUALITYVSSPEED, 90);
    }
    if (SUCCEEDED(result)) {
        result = profile->SetContainerAttributes(containerAttributes.Get());
    }

    ComPtr<IMFTopology> topology;
    if (SUCCEEDED(result)) {
        result = MFCreateTranscodeTopology(source.Get(),
                                           std::wstring(outputPath).c_str(),
                                           profile.Get(), &topology);
    }
    ComPtr<IMFMediaSession> session;
    if (SUCCEEDED(result)) {
        result = MFCreateMediaSession(nullptr, &session);
    }
    if (SUCCEEDED(result)) {
        result = session->SetTopology(0, topology.Get());
    }
    if (SUCCEEDED(result)) {
        core::LogInfo(L"Started local video optimization: " +
                      std::wstring(sourcePath));
        result = WaitForTranscode(session.Get(), stopToken);
    }
    if (session) {
        session->Shutdown();
    }
    if (source) {
        source->Shutdown();
    }
    if (SUCCEEDED(result)) {
        core::LogInfo(L"Completed local video optimization: " +
                      std::wstring(outputPath));
    } else if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        core::LogError(L"Local video optimization failed.", result);
    }
    return result;
}

}  // namespace lwe::media::video
