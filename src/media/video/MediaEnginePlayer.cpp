#include "media/video/MediaEnginePlayer.h"

#include <cstdint>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <mfapi.h>
#include <propvarutil.h>
#include <shlwapi.h>

#include "core/Logger.h"

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

HRESULT MediaEnginePlayer::Open(const HWND videoWindow, const HWND notificationWindow,
                                const UINT notificationMessage,
                                const std::wstring_view path,
                                const bool soundEnabled) {
    Shutdown();
    if (!IsWindow(videoWindow) || !IsWindow(notificationWindow) ||
        notificationMessage < WM_APP || path.empty()) {
        return E_INVALIDARG;
    }

    std::wstring sourceUrl;
    HRESULT result = PathToFileUrl(path, sourceUrl);
    if (FAILED(result)) {
        return result;
    }

    EventCallback* callback =
        new (std::nothrow) EventCallback(this, notificationWindow, notificationMessage);
    if (callback == nullptr) {
        return E_OUTOFMEMORY;
    }
    callback_.Attach(callback);

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    result = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(result)) {
        result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, callback_.Get());
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT64(
            MF_MEDIA_ENGINE_PLAYBACK_HWND,
            static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(videoWindow)));
    }
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

    core::LogInfo(L"Media Engine opened a looping video; audio is " +
                  std::wstring(soundEnabled ? L"enabled: " : L"muted: ") +
                  std::wstring(path));
    return S_OK;
}

void MediaEnginePlayer::HandleEvent(const DWORD eventCode) {
    if (!engine_) {
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
        engine_->Pause();
        engine_->Shutdown();
    }
    engine_.Reset();
    factory_.Reset();
    callback_.Reset();
    statisticsStartedAt_ = {};
    statisticsPauseStartedAt_ = {};
    statisticsPausedDuration_ = {};
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

MediaEnginePlayer::EventCallback::EventCallback(MediaEnginePlayer* owner, const HWND window,
                                                const UINT message)
    : owner_(owner), window_(window), message_(message) {}

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
    if (owner_ != nullptr && IsWindow(window_)) {
        PostMessageW(window_, message_, eventCode, 0);
    }
    return S_OK;
}

void MediaEnginePlayer::EventCallback::Detach() noexcept {
    owner_ = nullptr;
    window_ = nullptr;
    message_ = 0;
}

}  // namespace lwe::media::video
