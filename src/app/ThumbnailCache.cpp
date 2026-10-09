#include "app/ThumbnailCache.h"

#include <algorithm>
#include <shobjidl.h>
#include <wrl/client.h>
#include "core/Logger.h"

namespace lwe::app {
namespace {
bool SameFile(const core::WallpaperItem& left, const core::WallpaperItem& right) {
    return left.fileSize == right.fileSize && left.modifiedAt == right.modifiedAt;
}
}

void ThumbnailCache::Initialize(const HWND owner) {
    Stop();
    workEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!workEvent_) {
        core::LogError(L"Thumbnail wake event could not be created.",
                       HRESULT_FROM_WIN32(GetLastError()));
        return;
    }
    owner_ = owner;
}

ThumbnailCache::Image ThumbnailCache::Get(const core::WallpaperItem& item, SIZE size) {
    const std::scoped_lock lock(mutex_);
    if (!owner_) return {};
    if (size.cx > 512) {
        size.cy = MulDiv(size.cy, 512, size.cx);
        size.cx = 512;
    }
    const auto key = item.path.native();
    auto found = entries_.find(key);
    if (found != entries_.end()) {
        auto& entry = *found->second;
        if (SameFile(entry.item, item) && entry.size.cx == size.cx &&
            entry.size.cy == size.cy) {
            entry.used = ++clock_;
            return entry.image;
        }
        entries_.erase(found);
    }
    if (entries_.size() >= MaximumEntries) {
        const auto oldest = std::ranges::min_element(entries_, {}, [](const auto& pair) {
            return pair.second->used;
        });
        entries_.erase(oldest);
    }
    // Discard cancelled requests so fast scrolling cannot grow the work queue.
    std::erase_if(pending_, [&](const auto& entry) {
        const auto current = entries_.find(entry->item.path.native());
        return current == entries_.end() || current->second != entry;
    });
    auto entry = std::make_shared<Entry>();
    entry->item = item;
    entry->size = size;
    entry->used = ++clock_;
    entries_.emplace(key, entry);
    pending_.push_back(std::move(entry));
    if (!worker_.joinable()) {
        worker_ = std::jthread([this](const std::stop_token stop) { Run(stop); });
    }
    SetEvent(workEvent_);
    return {};
}

void ThumbnailCache::Prune(const std::vector<core::WallpaperItem>& items) {
    const std::scoped_lock lock(mutex_);
    std::unordered_map<std::wstring, const core::WallpaperItem*> current;
    for (const auto& item : items) current.emplace(item.path.native(), &item);
    std::erase_if(entries_, [&](const auto& pair) {
        const auto found = current.find(pair.first);
        return found == current.end() || !SameFile(pair.second->item, *found->second);
    });
    std::erase_if(pending_, [&](const auto& entry) {
        const auto found = entries_.find(entry->item.path.native());
        return found == entries_.end() || found->second != entry;
    });
}

std::vector<std::wstring> ThumbnailCache::TakeReady() {
    const std::scoped_lock lock(mutex_);
    notificationPending_ = false;
    std::vector<std::wstring> paths;
    for (auto& [path, entry] : entries_) {
        if (entry->dirty) {
            paths.push_back(path);
            entry->dirty = false;
        }
    }
    return paths;
}

void ThumbnailCache::Stop() {
    {
        const std::scoped_lock lock(mutex_);
        owner_ = nullptr;
        if (worker_.joinable()) worker_.request_stop();
        pending_.clear();
    }
    if (workEvent_) SetEvent(workEvent_);
    if (worker_.joinable()) worker_.join();
    const std::scoped_lock lock(mutex_);
    entries_.clear();
    notificationPending_ = false;
    if (workEvent_) {
        CloseHandle(workEvent_);
        workEvent_ = nullptr;
    }
}

ThumbnailCache::Image ThumbnailCache::Load(const Entry& entry) {
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(entry.item.path.c_str(), nullptr,
                                          IID_PPV_ARGS(&factory)))) return {};
    auto image = std::make_shared<Bitmap>();
    // Do not request BIGGERSIZEOK: providers may otherwise return large surfaces
    // that are needlessly retained just to display an 82-DIP thumbnail.
    if (FAILED(factory->GetImage(entry.size, SIIGBF_THUMBNAILONLY, &image->handle))) {
        image = std::make_shared<Bitmap>();
        if (FAILED(factory->GetImage(entry.size, SIIGBF_ICONONLY, &image->handle))) {
            return {};
        }
    }
    return image;
}

void ThumbnailCache::Run(const std::stop_token stop) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!stop.stop_requested()) {
        // Shell objects live in an STA. Pump its hidden COM windows even when
        // idle, rather than blocking callbacks/cleanup on a condition variable.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::shared_ptr<Entry> entry;
        {
            const std::scoped_lock lock(mutex_);
            if (!pending_.empty()) {
                entry = pending_.front();
                pending_.pop_front();
            }
        }
        if (!entry) {
            if (MsgWaitForMultipleObjectsEx(1, &workEvent_, INFINITE, QS_ALLINPUT,
                                             MWMO_INPUTAVAILABLE) == WAIT_FAILED) {
                core::LogError(L"Thumbnail message wait failed.",
                               HRESULT_FROM_WIN32(GetLastError()));
                break;
            }
            continue;
        }
        auto image = SUCCEEDED(com) ? Load(*entry) : Image{};
        const std::scoped_lock lock(mutex_);
        const auto current = entries_.find(entry->item.path.native());
        if (!stop.stop_requested() && current != entries_.end() &&
            current->second == entry) {
            entry->image = std::move(image);
            entry->dirty = true;
            if (owner_ && !notificationPending_) {
                notificationPending_ = PostMessageW(owner_, ReadyMessage, 0, 0) != FALSE;
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace lwe::app
