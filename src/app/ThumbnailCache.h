#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/WallpaperLibrary.h"

namespace lwe::app {

// Get only queues work or returns a retained bitmap: no Shell/COM calls on UI.
class ThumbnailCache final {
public:
    struct Bitmap final {
        Bitmap() = default;
        Bitmap(const Bitmap&) = delete;
        Bitmap& operator=(const Bitmap&) = delete;
        HBITMAP handle = nullptr;
        ~Bitmap() { if (handle) DeleteObject(handle); }
    };
    using Image = std::shared_ptr<Bitmap>;
    static constexpr UINT ReadyMessage = WM_APP + 80;
    static constexpr std::size_t MaximumEntries = 64;

    ~ThumbnailCache() { Stop(); }
    void Initialize(HWND owner);
    Image Get(const core::WallpaperItem& item, SIZE size);
    void Prune(const std::vector<core::WallpaperItem>& items);
    std::vector<std::wstring> TakeReady();
    void Stop();

private:
    struct Entry final {
        core::WallpaperItem item;
        SIZE size{};
        std::uint64_t used = 0;
        Image image;
        bool dirty = false;
    };
    void Run(std::stop_token stop);
    static Image Load(const Entry& entry);
    std::mutex mutex_;
    HANDLE workEvent_ = nullptr;
    std::unordered_map<std::wstring, std::shared_ptr<Entry>> entries_;
    std::deque<std::shared_ptr<Entry>> pending_;
    std::uint64_t clock_ = 0;
    HWND owner_ = nullptr;
    bool notificationPending_ = false;
    std::jthread worker_;
};

}  // namespace lwe::app
