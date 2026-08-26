#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace lwe::core {

struct WallpaperGroup final {
    std::wstring id;
    std::wstring name;
    std::vector<std::wstring> fileNames;

    bool operator==(const WallpaperGroup&) const = default;
};

class WallpaperGroupStore final {
public:
    HRESULT Initialize();
    HRESULT InitializeAt(std::filesystem::path libraryRoot);

    [[nodiscard]] const std::vector<WallpaperGroup>& Groups() const noexcept;
    [[nodiscard]] const std::vector<std::wstring>& Favorites() const noexcept;
    [[nodiscard]] bool IsFavorite(std::wstring_view fileName) const;
    [[nodiscard]] bool IsInGroup(std::wstring_view groupId,
                                 std::wstring_view fileName) const;

    HRESULT CreateGroup(std::wstring_view name, std::wstring& createdId);
    HRESULT RenameGroup(std::wstring_view groupId, std::wstring_view name);
    HRESULT DeleteGroup(std::wstring_view groupId);
    HRESULT ReorderGroups(std::span<const std::wstring> groupIds);
    HRESULT SetFavorite(std::wstring_view fileName, bool favorite);
    HRESULT SetFavorites(std::span<const std::wstring> fileNames, bool favorite);
    HRESULT AddToGroup(std::wstring_view groupId,
                       std::span<const std::wstring> fileNames);
    HRESULT RemoveFromGroup(std::wstring_view groupId,
                            std::span<const std::wstring> fileNames);
    HRESULT ReplaceWallpaperKey(std::wstring_view oldFileName,
                                std::wstring_view newFileName);
    HRESULT RemoveWallpaperKey(std::wstring_view fileName);
    HRESULT Prune(std::span<const std::wstring> validFileNames);

    // Public for the small transactional mutation helper in the implementation;
    // callers should normally use the mutating operations above.
    HRESULT Save() const;

private:
    HRESULT Load();
    WallpaperGroup* FindGroup(std::wstring_view groupId);
    const WallpaperGroup* FindGroup(std::wstring_view groupId) const;

    std::filesystem::path libraryRoot_;
    std::vector<WallpaperGroup> groups_;
    std::vector<std::wstring> favorites_;
};

}  // namespace lwe::core
