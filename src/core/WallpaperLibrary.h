#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "media/MediaTypes.h"

namespace lwe::core {

struct WallpaperItem final {
    std::filesystem::path path;
    std::wstring displayName;
    media::WallpaperKind kind = media::WallpaperKind::StaticImage;
    std::wstring formatLabel;
    std::uint64_t fileSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool hasAudio = false;
    bool external = false;
};

class WallpaperLibrary final {
public:
    HRESULT Initialize();
    HRESULT InitializeAt(std::filesystem::path rootDirectory);
    std::vector<WallpaperItem> Scan() const;

    HRESULT ImportFile(std::wstring_view sourcePath, WallpaperItem& imported) const;
    HRESULT ImportPackage(std::wstring_view packagePath, WallpaperItem& imported) const;
    HRESULT ImportArchive(std::wstring_view archivePath,
                          std::vector<WallpaperItem>& imported) const;
    HRESULT ExportPackage(const WallpaperItem& item,
                          std::wstring_view destinationPath) const;
    HRESULT ExportArchive(std::span<const WallpaperItem> items,
                          std::wstring_view destinationPath) const;
    HRESULT Rename(const WallpaperItem& item, std::wstring_view newDisplayName,
                   WallpaperItem& renamed) const;
    HRESULT Remove(const WallpaperItem& item) const;
    HRESULT Reorder(std::span<const WallpaperItem> items) const;

    [[nodiscard]] const std::filesystem::path& RootDirectory() const noexcept;

private:
    HRESULT DescribeFile(const std::filesystem::path& path, bool external,
                         WallpaperItem& item) const;
    std::filesystem::path UniqueDestination(std::wstring_view fileName) const;
    std::vector<std::wstring> LoadOrder() const;
    HRESULT SaveOrder(std::span<const std::wstring> fileNames) const;
    HRESULT AppendOrderEntry(std::wstring_view fileName) const;

    std::filesystem::path rootDirectory_;
};

}  // namespace lwe::core
