#include "media/video/MediaEnginePlayer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <propvarutil.h>
#include <shlwapi.h>

#include "core/Logger.h"
#include "render/D3DRenderer.h"

namespace lwe::media::video {
namespace {

HRESULT PathToFileUrl(const std::wstring_view path, std::wstring& url) {
    if (path.empty()) {
        return E_INVALIDARG;
    }

    // UrlCreateFromPath performs the escaping required by IMFMediaEngine while
    // preserving Unicode through its wide-character API.
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const std::wstring filePath(path);
    const HRESULT result =
        UrlCreateFromPathW(filePath.c_str(), buffer.data(), &length, 0);
    if (FAILED(result)) {
        return result;
    }
    url.assign(buffer.data(), length);
    return S_OK;
}

std::optional<double> ReadStatistic(IMFMediaEngine* engine,
                                    const MF_MEDIA_ENGINE_STATISTIC statistic) {
    if (engine == nullptr) {
        return std::nullopt;
    }
    Microsoft::WRL::ComPtr<IMFMediaEngineEx> extended;
    if (FAILED(engine->QueryInterface(IID_PPV_ARGS(&extended)))) {
        return std::nullopt;
    }
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (FAILED(extended->GetStatistics(statistic, &value))) {
        PropVariantClear(&value);
        return std::nullopt;
    }
    std::optional<double> number;
    switch (value.vt) {
        case VT_R8:
            number = value.dblVal;
            break;
        case VT_UI4:
            number = value.ulVal;
            break;
        case VT_UI8:
            number = static_cast<double>(value.uhVal.QuadPart);
            break;
        default:
            break;
    }
    PropVariantClear(&value);
    return number;
}

void LogPlaybackStatistics(IMFMediaEngine* engine,
                           const std::chrono::milliseconds activeTime) {
    const auto rendered =
        ReadStatistic(engine, MF_MEDIA_ENGINE_STATISTIC_FRAMES_RENDERED);
    const auto dropped =
        ReadStatistic(engine, MF_MEDIA_ENGINE_STATISTIC_FRAMES_DROPPED);
    const auto framesPerSecond =
        ReadStatistic(engine, MF_MEDIA_ENGINE_STATISTIC_FRAMES_PER_SECOND);
    if (!rendered.has_value() && !dropped.has_value() &&
        !framesPerSecond.has_value()) {
        return;
    }
    std::wostringstream message;
    const double averageFramesPerSecond =
        rendered.has_value() && activeTime.count() > 0
            ? *rendered * 1000.0 / static_cast<double>(activeTime.count())
            : -1.0;
    message << L"Video playback statistics: rendered="
            << rendered.value_or(-1.0) << L", dropped=" << dropped.value_or(-1.0)
            << L", active_ms=" << activeTime.count() << L", average_fps="
            << std::fixed << std::setprecision(2) << averageFramesPerSecond
            << L", reported_fps=" << framesPerSecond.value_or(-1.0);
    core::LogInfo(message.str());
}

}  // namespace

MediaEnginePlayer::~MediaEnginePlayer() {
    Shutdown();
}

HRESULT MediaEnginePlayer::Open(ID3D11Device* const device,
                                const HWND notificationWindow,
                                const UINT notificationMessage,
                                const std::wstring_view path,
                                const bool soundEnabled,
                                const std::uint32_t notificationToken) {
    Shutdown();
    if (device == nullptr || !IsWindow(notificationWindow) ||
        notificationMessage < WM_APP || path.empty()) {
        return E_INVALIDARG;
    }

    std::wstring sourceUrl;
    HRESULT result = PathToFileUrl(path, sourceUrl);
    if (FAILED(result)) {
        return result;
    }

    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    EventCallback* callback = new (std::nothrow) EventCallback(
        notificationWindow, notificationMessage, generation_, notificationToken);
    if (callback == nullptr) {
        return E_OUTOFMEMORY;
    }
    callback_.Attach(callback);

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    result = MFCreateDXGIDeviceManager(&deviceResetToken_, &deviceManager_);
    if (SUCCEEDED(result)) {
        result = deviceManager_->ResetDevice(device, deviceResetToken_);
    }
    if (SUCCEEDED(result)) {
        result = MFCreateAttributes(&attributes, 3);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, callback_.Get());
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER,
                                        deviceManager_.Get());
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                                       DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    // Omitting MF_MEDIA_ENGINE_PLAYBACK_HWND selects frame-server mode. Frames
    // are copied into our existing desktop swap chain with TransferVideoFrame.
    if (SUCCEEDED(result)) {
        result = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
    }
    if (SUCCEEDED(result)) {
        result = factory_->CreateInstance(0, attributes.Get(), &engine_);
    }
    if (FAILED(result)) {
        Shutdown();
        return result;
    }

    soundEnabled_ = soundEnabled;
    result = engine_->SetLoop(TRUE);
    if (SUCCEEDED(result)) {
        result = engine_->SetMuted(soundEnabled ? FALSE : TRUE);
    }
    if (SUCCEEDED(result)) {
        result = engine_->SetAutoPlay(TRUE);
    }
    if (SUCCEEDED(result)) {
        result = engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    }

    BSTR source = nullptr;
    if (SUCCEEDED(result)) {
        source = SysAllocStringLen(sourceUrl.data(), static_cast<UINT>(sourceUrl.size()));
        if (source == nullptr) {
            result = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(result)) {
        result = engine_->SetSource(source);
    }
    SysFreeString(source);
    if (SUCCEEDED(result)) {
        result = engine_->Load();
    }
    if (FAILED(result)) {
        Shutdown();
        return result;
    }

    core::LogInfo(L"Media Engine frame server opened a looping video; audio is " +
                  std::wstring(soundEnabled ? L"enabled: " : L"muted: ") +
                  std::wstring(path));
    return S_OK;
}

HRESULT MediaEnginePlayer::PresentFrame(
    render::D3DRenderer& renderer, const std::span<const RECT> destinations,
    const bool present) {
    if (!engine_ || !playing_ || destinations.empty()) {
        return S_FALSE;
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr std::uint64_t kFullHdPixels = 1920ULL * 1080ULL;
    if (static_cast<std::uint64_t>(nativeWidth_) * nativeHeight_ >
            kFullHdPixels &&
        now < nextFrameTransferAt_) {
        return S_FALSE;
    }

    LONGLONG presentationTime = 0;
    HRESULT result = engine_->OnVideoStreamTick(&presentationTime);
    if (result == S_FALSE) {
        return S_FALSE;
    }
    if (FAILED(result)) {
        failed_ = true;
        core::LogError(L"Media Engine frame tick failed.", result);
        return result;
    }
    if (hasPresentationTime_ && presentationTime == lastPresentationTime_) {
        return S_FALSE;
    }

    if (nativeWidth_ == 0 || nativeHeight_ == 0) {
        result = engine_->GetNativeVideoSize(&nativeWidth_, &nativeHeight_);
        if (FAILED(result) || nativeWidth_ == 0 || nativeHeight_ == 0) {
            failed_ = true;
            core::LogError(L"Media Engine returned an invalid native video size.",
                           FAILED(result) ? result : MF_E_INVALIDMEDIATYPE);
            return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
        }
    }

    UINT maximumDestinationWidth = 0;
    UINT maximumDestinationHeight = 0;
    for (const RECT& destination : destinations) {
        maximumDestinationWidth = std::max(
            maximumDestinationWidth,
            static_cast<UINT>(std::max(0L, destination.right - destination.left)));
        maximumDestinationHeight = std::max(
            maximumDestinationHeight,
            static_cast<UINT>(std::max(0L, destination.bottom - destination.top)));
    }
    if (maximumDestinationWidth == 0 || maximumDestinationHeight == 0) {
        return E_INVALIDARG;
    }
    const double transferScale = std::min(
        1.0, std::max(static_cast<double>(maximumDestinationWidth) / nativeWidth_,
                      static_cast<double>(maximumDestinationHeight) / nativeHeight_));
    const UINT transferWidth = std::max(
        2U, static_cast<UINT>(std::lround(nativeWidth_ * transferScale)) & ~1U);
    const UINT transferHeight = std::max(
        2U, static_cast<UINT>(std::lround(nativeHeight_ * transferScale)) & ~1U);

    bool recreateSurface = !transferSurface_ || !transferSourceView_;
    if (!recreateSurface) {
        D3D11_TEXTURE2D_DESC existing{};
        transferSurface_->GetDesc(&existing);
        recreateSurface = existing.Width != transferWidth ||
                          existing.Height != transferHeight;
    }
    if (recreateSurface) {
        result = renderer.CreateVideoTransferSurface(
            transferWidth, transferHeight, transferSurface_, transferSourceView_);
        if (FAILED(result)) {
            failed_ = true;
            return result;
        }
    }

    // Transfer once at the largest selected monitor's useful resolution.
    // D3DRenderer performs the aspect-correct, opaque fan-out to every selected
    // monitor in a single GPU composition pass; repeating TransferVideoFrame
    // per monitor doubled GPU utilization and exposed intermediate frames on
    // layered desktop windows.
    const MFVideoNormalizedRect source{0.0F, 0.0F, 1.0F, 1.0F};
    const RECT transferDestination{0, 0, static_cast<LONG>(transferWidth),
                                   static_cast<LONG>(transferHeight)};
    const MFARGB border{0, 0, 0, 255};
    result = engine_->TransferVideoFrame(transferSurface_.Get(), &source,
                                         &transferDestination, &border);
    if (FAILED(result)) {
        failed_ = true;
        core::LogError(L"Media Engine could not transfer a decoded video frame.",
                       result);
        return result;
    }
    if (!renderer.CommitVideoTransferSurface(
            transferSourceView_.Get(), destinations, nativeWidth_, nativeHeight_,
            present)) {
        failed_ = true;
        return E_FAIL;
    }
    lastPresentationTime_ = presentationTime;
    hasPresentationTime_ = true;
    if (static_cast<std::uint64_t>(nativeWidth_) * nativeHeight_ >
        kFullHdPixels) {
        // 4K/greater wallpaper presentation is capped at 30 FPS. The media
        // engine remains hardware-decoded, while halving conversion, shader
        // and layered-window Present work that has little desktop benefit.
        nextFrameTransferAt_ = now + std::chrono::milliseconds(33);
    }
    ++transferredFrameCount_;
    return S_OK;
}

void MediaEnginePlayer::HandleEvent(const DWORD eventCode,
                                    const std::uint32_t generation) {
    if (!engine_ || generation != generation_) {
        if (generation != generation_) {
            core::LogInfo(L"Ignored a delayed event from an older video session.");
        }
        return;
    }

    switch (eventCode) {
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:
            if (!playing_ && !pauseRequested_) {
                const HRESULT result = engine_->Play();
                if (FAILED(result)) {
                    failed_ = true;
                    core::LogError(L"Media Engine could not start video playback.", result);
                }
            }
            break;

        case MF_MEDIA_ENGINE_EVENT_PLAY:
        case MF_MEDIA_ENGINE_EVENT_PLAYING:
            if (pauseRequested_) {
                engine_->Pause();
                playing_ = false;
                break;
            }
            if (statisticsStartedAt_ == std::chrono::steady_clock::time_point{}) {
                statisticsStartedAt_ = std::chrono::steady_clock::now();
            }
            if (failed_) {
                core::LogInfo(
                    L"Media Engine recovered after a transient playback event.");
            }
            // Some sources emit ERROR/ABORT while replacing their source and
            // then immediately reach PLAYING. PLAYING is authoritative proof
            // that this session is usable and must clear the transient flag.
            failed_ = false;
            playing_ = true;
            core::LogInfo(L"Media Engine video playback started.");
            break;

        case MF_MEDIA_ENGINE_EVENT_PAUSE:
        case MF_MEDIA_ENGINE_EVENT_WAITING:
            playing_ = false;
            break;

        case MF_MEDIA_ENGINE_EVENT_ERROR:
        case MF_MEDIA_ENGINE_EVENT_ABORT:
            failed_ = true;
            playing_ = false;
            core::LogError(L"Media Engine reported a video playback failure.");
            break;

        case MF_MEDIA_ENGINE_EVENT_ENDED:
            // SetLoop(TRUE) normally rewinds internally. Play is harmless here
            // and covers media sources that emit ENDED before honoring loop.
            engine_->SetCurrentTime(0.0);
            engine_->Play();
            break;

        default:
            break;
    }
}

HRESULT MediaEnginePlayer::SetSoundEnabled(const bool enabled) {
    soundEnabled_ = enabled;
    if (!engine_) {
        return S_OK;
    }
    const HRESULT result = engine_->SetMuted(enabled ? FALSE : TRUE);
    if (SUCCEEDED(result)) {
        core::LogInfo(enabled ? L"Video audio enabled." : L"Video audio muted.");
    }
    return result;
}

HRESULT MediaEnginePlayer::SetPaused(const bool paused) {
    pauseRequested_ = paused;
    if (!engine_) {
        return S_OK;
    }
    const HRESULT result = paused ? engine_->Pause() : engine_->Play();
    if (SUCCEEDED(result)) {
        const auto now = std::chrono::steady_clock::now();
        if (paused) {
            playing_ = false;
            if (statisticsPauseStartedAt_ ==
                    std::chrono::steady_clock::time_point{} &&
                statisticsStartedAt_ != std::chrono::steady_clock::time_point{}) {
                statisticsPauseStartedAt_ = now;
            }
        } else if (statisticsPauseStartedAt_ !=
                   std::chrono::steady_clock::time_point{}) {
            statisticsPausedDuration_ += now - statisticsPauseStartedAt_;
            statisticsPauseStartedAt_ = {};
        }
        core::LogInfo(paused ? L"Video playback paused by policy."
                             : L"Video playback resumed by policy.");
    }
    return result;
}

void MediaEnginePlayer::Shutdown() {
    playing_ = false;
    failed_ = false;
    pauseRequested_ = false;
    if (callback_) {
        callback_->Detach();
    }
    if (engine_) {
        auto pausedDuration = statisticsPausedDuration_;
        const auto now = std::chrono::steady_clock::now();
        if (statisticsPauseStartedAt_ != std::chrono::steady_clock::time_point{}) {
            pausedDuration += now - statisticsPauseStartedAt_;
        }
        const auto activeDuration =
            statisticsStartedAt_ == std::chrono::steady_clock::time_point{}
                ? std::chrono::milliseconds::zero()
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - statisticsStartedAt_ - pausedDuration);
        LogPlaybackStatistics(engine_.Get(), activeDuration);
        core::LogInfo(L"Video frame-server transfers presented=" +
                      std::to_wstring(transferredFrameCount_) + L'.');
        engine_->Pause();
        engine_->Shutdown();
    }
    engine_.Reset();
    deviceManager_.Reset();
    factory_.Reset();
    callback_.Reset();
    transferSourceView_.Reset();
    transferSurface_.Reset();
    statisticsStartedAt_ = {};
    statisticsPauseStartedAt_ = {};
    statisticsPausedDuration_ = {};
    deviceResetToken_ = 0;
    nativeWidth_ = 0;
    nativeHeight_ = 0;
    transferredFrameCount_ = 0;
    lastPresentationTime_ = 0;
    hasPresentationTime_ = false;
    nextFrameTransferAt_ = {};
}

bool MediaEnginePlayer::IsActive() const noexcept {
    return engine_ != nullptr;
}

bool MediaEnginePlayer::IsPlaying() const noexcept {
    return playing_;
}

bool MediaEnginePlayer::HasFailed() const noexcept {
    return failed_;
}

bool MediaEnginePlayer::SoundEnabled() const noexcept {
    return soundEnabled_;
}

MediaEnginePlayer::EventCallback::EventCallback(
    const HWND window, const UINT message, const std::uint32_t generation,
    const std::uint32_t notificationToken)
    : window_(window),
      message_(message),
      generation_(generation),
      notificationToken_(notificationToken) {}

HRESULT STDMETHODCALLTYPE MediaEnginePlayer::EventCallback::QueryInterface(
    REFIID interfaceId, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    if (interfaceId == __uuidof(IUnknown) ||
        interfaceId == __uuidof(IMFMediaEngineNotify)) {
        *object = static_cast<IMFMediaEngineNotify*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MediaEnginePlayer::EventCallback::AddRef() {
    return ++referenceCount_;
}

ULONG STDMETHODCALLTYPE MediaEnginePlayer::EventCallback::Release() {
    const ULONG remaining = --referenceCount_;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE MediaEnginePlayer::EventCallback::EventNotify(
    const DWORD eventCode, DWORD_PTR, DWORD) {
    if (!detached_.load(std::memory_order_acquire) && IsWindow(window_)) {
        static_assert(sizeof(WPARAM) >= sizeof(std::uint64_t));
        const std::uint64_t packedEvent =
            (static_cast<std::uint64_t>(notificationToken_) << 32U) |
            eventCode;
        PostMessageW(window_, message_, static_cast<WPARAM>(packedEvent),
                     static_cast<LPARAM>(generation_));
    }
    return S_OK;
}

void MediaEnginePlayer::EventCallback::Detach() noexcept {
    detached_.store(true, std::memory_order_release);
}

}  // namespace lwe::media::video
