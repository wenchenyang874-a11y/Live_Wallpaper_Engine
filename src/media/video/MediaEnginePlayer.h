#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include <windows.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

namespace lwe::media::video {

class MediaEnginePlayer final {
public:
    MediaEnginePlayer() = default;
    ~MediaEnginePlayer();

    MediaEnginePlayer(const MediaEnginePlayer&) = delete;
    MediaEnginePlayer& operator=(const MediaEnginePlayer&) = delete;

    HRESULT Open(HWND videoWindow, HWND notificationWindow, UINT notificationMessage,
                 std::wstring_view path, bool soundEnabled);
    void HandleEvent(DWORD eventCode);
    HRESULT SetSoundEnabled(bool enabled);
    HRESULT SetPaused(bool paused);
    void Shutdown();

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] bool IsPlaying() const noexcept;
    [[nodiscard]] bool HasFailed() const noexcept;
    [[nodiscard]] bool SoundEnabled() const noexcept;

private:
    class EventCallback final : public IMFMediaEngineNotify {
    public:
        EventCallback(MediaEnginePlayer* owner, HWND window, UINT message);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId,
                                                 void** object) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE EventNotify(DWORD eventCode, DWORD_PTR parameter1,
                                              DWORD parameter2) override;
        void Detach() noexcept;

    private:
        ~EventCallback() = default;

        std::atomic_ulong referenceCount_{1};
        MediaEnginePlayer* owner_ = nullptr;
        HWND window_ = nullptr;
        UINT message_ = 0;
    };

    Microsoft::WRL::ComPtr<IMFMediaEngineClassFactory> factory_;
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<EventCallback> callback_;
    bool playing_ = false;
    bool failed_ = false;
    bool soundEnabled_ = false;
    bool pauseRequested_ = false;
    std::chrono::steady_clock::time_point statisticsStartedAt_{};
    std::chrono::steady_clock::time_point statisticsPauseStartedAt_{};
    std::chrono::steady_clock::duration statisticsPausedDuration_{};
};

}  // namespace lwe::media::video
