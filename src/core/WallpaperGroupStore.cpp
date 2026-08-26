#include "core/WallpaperGroupStore.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include <shlobj.h>

namespace lwe::core {
namespace {

constexpr wchar_t kMetadataFileName[] = L".wallpaper-groups.v1";
constexpr std::array<std::uint8_t, 8> kMagic{'L', 'W', 'E', 'G', 'R', 'P', '1', 0};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaximumGroups = 256;
constexpr std::uint32_t kMaximumMembers = 65536;
constexpr std::uint64_t kMaximumReferences = 65536;
constexpr std::uint32_t kMaximumStringBytes = 4096;
constexpr std::uint64_t kMaximumFileBytes = 16ULL * 1024ULL * 1024ULL;

struct Header final {
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t favoriteCount = 0;
    std::uint32_t groupCount = 0;
};

HRESULT LastErrorResult() {
    return HRESULT_FROM_WIN32(GetLastError());
}

std::wstring OrdinalKey(std::wstring value) {
    if (!value.empty()) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

bool IsSafeFileName(const std::wstring_view value) {
    if (value.empty() || value.size() > 240 || value == L"." || value == L"..") {
        return false;
    }
    constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
    for (const wchar_t character : value) {
        if (character < 0x20 || forbidden.find(character) != std::wstring_view::npos) {
            return false;
        }
    }
    return std::filesystem::path(value).filename().native() == value;
}

bool IsValidGroupName(const std::wstring_view value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    if (value == L"全部壁纸" || value == L"最爱壁纸") {
        return false;
    }
    return std::ranges::none_of(value, [](const wchar_t character) {
               return character < 0x20;
           }) &&
           std::ranges::any_of(value, [](const wchar_t character) {
               return std::iswspace(character) == 0;
           });
}

std::optional<std::string> WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count,
                            nullptr, nullptr) != count) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> Utf8ToWide(const std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) !=
        count) {
        return std::nullopt;
    }
    return result;
}

HRESULT WriteExact(const HANDLE file, const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, request, &written, nullptr)) {
            return LastErrorResult();
        }
        if (written == 0) {
            return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        offset += written;
    }
    return S_OK;
}

HRESULT ReadExact(const HANDLE file, void* data, const std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(file, bytes + offset, request, &read, nullptr)) {
            return LastErrorResult();
        }
        if (read == 0) {
            return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        }
        offset += read;
    }
    return S_OK;
}

HRESULT WriteString(const HANDLE file, const std::wstring_view value) {
    const std::optional encoded = WideToUtf8(value);
    if (!encoded.has_value() || encoded->size() > kMaximumStringBytes) {
        return E_INVALIDARG;
    }
    const auto size = static_cast<std::uint32_t>(encoded->size());
    HRESULT result = WriteExact(file, &size, sizeof(size));
    if (SUCCEEDED(result) && size > 0) {
        result = WriteExact(file, encoded->data(), size);
    }
    return result;
}

HRESULT ReadString(const HANDLE file, std::wstring& value) {
    std::uint32_t size = 0;
    HRESULT result = ReadExact(file, &size, sizeof(size));
    if (FAILED(result) || size > kMaximumStringBytes) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    std::string encoded(size, '\0');
    if (size > 0) {
        result = ReadExact(file, encoded.data(), size);
    }
    const std::optional decoded = SUCCEEDED(result) ? Utf8ToWide(encoded) : std::nullopt;
    if (!decoded.has_value()) {
        return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    value = *decoded;
    return S_OK;
}

HRESULT ResolveLibraryRoot(std::filesystem::path& root) {
    PWSTR localAppData = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (FAILED(result) || localAppData == nullptr) {
        CoTaskMemFree(localAppData);
        return FAILED(result) ? result : E_UNEXPECTED;
    }
    root = std::filesystem::path(localAppData) / L"LiveWallpaperEngine" / L"library";
    CoTaskMemFree(localAppData);
    return S_OK;
}

std::wstring CreateGroupId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return {};
    }
    std::array<wchar_t, 40> text{};
    if (StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) <= 0) {
        return {};
    }
    return text.data();
}

template <typename Mutation>
HRESULT MutateAndSave(std::vector<WallpaperGroup>& groups,
                      std::vector<std::wstring>& favorites,
                      const WallpaperGroupStore& store, Mutation&& mutation) {
    const auto oldGroups = groups;
    const auto oldFavorites = favorites;
    const HRESULT mutationResult = mutation();
    if (FAILED(mutationResult)) {
        return mutationResult;
    }
    const HRESULT saveResult = store.Save();
    if (FAILED(saveResult)) {
        groups = oldGroups;
        favorites = oldFavorites;
    }
    return saveResult;
}

}  // namespace

HRESULT WallpaperGroupStore::Initialize() {
    std::filesystem::path root;
    const HRESULT result = ResolveLibraryRoot(root);
    return FAILED(result) ? result : InitializeAt(std::move(root));
}

HRESULT WallpaperGroupStore::InitializeAt(std::filesystem::path libraryRoot) {
    if (libraryRoot.empty() || !libraryRoot.is_absolute()) {
        return E_INVALIDARG;
    }
    libraryRoot_ = std::move(libraryRoot);
    std::error_code error;
    std::filesystem::create_directories(libraryRoot_, error);
    if (error) {
        return HRESULT_FROM_WIN32(error.value());
    }
    groups_.clear();
    favorites_.clear();
    return Load();
}

const std::vector<WallpaperGroup>& WallpaperGroupStore::Groups() const noexcept {
    return groups_;
}

const std::vector<std::wstring>& WallpaperGroupStore::Favorites() const noexcept {
    return favorites_;
}

bool WallpaperGroupStore::IsFavorite(const std::wstring_view fileName) const {
    const std::wstring key = OrdinalKey(std::wstring(fileName));
    return std::ranges::any_of(favorites_, [&](const std::wstring& value) {
        return OrdinalKey(value) == key;
    });
}

bool WallpaperGroupStore::IsInGroup(const std::wstring_view groupId,
                                    const std::wstring_view fileName) const {
    const WallpaperGroup* group = FindGroup(groupId);
    if (group == nullptr) {
        return false;
    }
    const std::wstring key = OrdinalKey(std::wstring(fileName));
    return std::ranges::any_of(group->fileNames, [&](const std::wstring& value) {
        return OrdinalKey(value) == key;
    });
}

HRESULT WallpaperGroupStore::CreateGroup(const std::wstring_view name,
                                         std::wstring& createdId) {
    if (!IsValidGroupName(name) || std::ranges::any_of(groups_, [&](const auto& group) {
            return _wcsicmp(group.name.c_str(), std::wstring(name).c_str()) == 0;
        })) {
        return E_INVALIDARG;
    }
    const std::wstring id = CreateGroupId();
    if (id.empty()) {
        return E_FAIL;
    }
    const HRESULT result = MutateAndSave(groups_, favorites_, *this, [&] {
        groups_.push_back(WallpaperGroup{id, std::wstring(name), {}});
        return S_OK;
    });
    if (SUCCEEDED(result)) {
        createdId = id;
    }
    return result;
}

HRESULT WallpaperGroupStore::RenameGroup(const std::wstring_view groupId,
                                         const std::wstring_view name) {
    WallpaperGroup* group = FindGroup(groupId);
    if (group == nullptr || !IsValidGroupName(name) ||
        std::ranges::any_of(groups_, [&](const WallpaperGroup& candidate) {
            return &candidate != group &&
                   _wcsicmp(candidate.name.c_str(), std::wstring(name).c_str()) == 0;
        })) {
        return E_INVALIDARG;
    }
    return MutateAndSave(groups_, favorites_, *this, [&] {
        group->name = name;
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::DeleteGroup(const std::wstring_view groupId) {
    const auto found = std::ranges::find_if(groups_, [&](const WallpaperGroup& group) {
        return _wcsicmp(group.id.c_str(), std::wstring(groupId).c_str()) == 0;
    });
    if (found == groups_.end()) {
        return E_INVALIDARG;
    }
    return MutateAndSave(groups_, favorites_, *this, [&] {
        groups_.erase(found);
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::ReorderGroups(
    const std::span<const std::wstring> groupIds) {
    if (groupIds.size() != groups_.size()) {
        return E_INVALIDARG;
    }
    std::vector<WallpaperGroup> reordered;
    reordered.reserve(groups_.size());
    std::unordered_set<std::wstring> seen;
    for (const std::wstring& id : groupIds) {
        const WallpaperGroup* group = FindGroup(id);
        if (group == nullptr || !seen.insert(OrdinalKey(id)).second) {
            return E_INVALIDARG;
        }
        reordered.push_back(*group);
    }
    return MutateAndSave(groups_, favorites_, *this, [&] {
        groups_ = std::move(reordered);
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::SetFavorite(const std::wstring_view fileName,
                                         const bool favorite) {
    const std::array<std::wstring, 1> fileNames{std::wstring(fileName)};
    return SetFavorites(fileNames, favorite);
}

HRESULT WallpaperGroupStore::SetFavorites(
    const std::span<const std::wstring> fileNames, const bool favorite) {
    if (std::ranges::any_of(fileNames, [](const std::wstring& fileName) {
            return !IsSafeFileName(fileName);
        })) {
        return E_INVALIDARG;
    }
    return MutateAndSave(groups_, favorites_, *this, [&] {
        for (const std::wstring& fileName : fileNames) {
            const std::wstring key = OrdinalKey(fileName);
            std::erase_if(favorites_, [&](const std::wstring& value) {
                return OrdinalKey(value) == key;
            });
            if (favorite) {
                favorites_.push_back(fileName);
            }
        }
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::AddToGroup(
    const std::wstring_view groupId,
    const std::span<const std::wstring> fileNames) {
    WallpaperGroup* group = FindGroup(groupId);
    if (group == nullptr || std::ranges::any_of(fileNames, [](const auto& value) {
            return !IsSafeFileName(value);
        })) {
        return E_INVALIDARG;
    }
    return MutateAndSave(groups_, favorites_, *this, [&] {
        std::unordered_set<std::wstring> seen;
        for (const std::wstring& value : group->fileNames) {
            seen.insert(OrdinalKey(value));
        }
        for (const std::wstring& value : fileNames) {
            if (seen.insert(OrdinalKey(value)).second) {
                group->fileNames.push_back(value);
            }
        }
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::RemoveFromGroup(
    const std::wstring_view groupId,
    const std::span<const std::wstring> fileNames) {
    WallpaperGroup* group = FindGroup(groupId);
    if (group == nullptr) {
        return E_INVALIDARG;
    }
    std::unordered_set<std::wstring> removals;
    for (const std::wstring& value : fileNames) {
        removals.insert(OrdinalKey(value));
    }
    return MutateAndSave(groups_, favorites_, *this, [&] {
        std::erase_if(group->fileNames, [&](const std::wstring& value) {
            return removals.contains(OrdinalKey(value));
        });
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::ReplaceWallpaperKey(const std::wstring_view oldFileName,
                                                 const std::wstring_view newFileName) {
    if (!IsSafeFileName(oldFileName) || !IsSafeFileName(newFileName)) {
        return E_INVALIDARG;
    }
    const std::wstring oldKey = OrdinalKey(std::wstring(oldFileName));
    return MutateAndSave(groups_, favorites_, *this, [&] {
        for (std::wstring& value : favorites_) {
            if (OrdinalKey(value) == oldKey) {
                value = newFileName;
            }
        }
        for (WallpaperGroup& group : groups_) {
            for (std::wstring& value : group.fileNames) {
                if (OrdinalKey(value) == oldKey) {
                    value = newFileName;
                }
            }
        }
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::RemoveWallpaperKey(const std::wstring_view fileName) {
    const std::array<std::wstring, 1> removal{std::wstring(fileName)};
    const std::wstring key = OrdinalKey(removal.front());
    return MutateAndSave(groups_, favorites_, *this, [&] {
        std::erase_if(favorites_, [&](const std::wstring& value) {
            return OrdinalKey(value) == key;
        });
        for (WallpaperGroup& group : groups_) {
            std::erase_if(group.fileNames, [&](const std::wstring& value) {
                return OrdinalKey(value) == key;
            });
        }
        return S_OK;
    });
}

HRESULT WallpaperGroupStore::Prune(
    const std::span<const std::wstring> validFileNames) {
    std::unordered_set<std::wstring> valid;
    for (const std::wstring& value : validFileNames) {
        valid.insert(OrdinalKey(value));
    }
    const auto oldGroups = groups_;
    const auto oldFavorites = favorites_;
    std::erase_if(favorites_, [&](const std::wstring& value) {
        return !valid.contains(OrdinalKey(value));
    });
    for (WallpaperGroup& group : groups_) {
        std::erase_if(group.fileNames, [&](const std::wstring& value) {
            return !valid.contains(OrdinalKey(value));
        });
    }
    if (groups_ == oldGroups && favorites_ == oldFavorites) {
        return S_FALSE;
    }
    const HRESULT result = Save();
    if (FAILED(result)) {
        groups_ = oldGroups;
        favorites_ = oldFavorites;
    }
    return result;
}

WallpaperGroup* WallpaperGroupStore::FindGroup(const std::wstring_view groupId) {
    const std::wstring requested(groupId);
    const auto found = std::ranges::find_if(groups_, [&](const WallpaperGroup& group) {
        return _wcsicmp(group.id.c_str(), requested.c_str()) == 0;
    });
    return found == groups_.end() ? nullptr : &*found;
}

const WallpaperGroup* WallpaperGroupStore::FindGroup(
    const std::wstring_view groupId) const {
    const std::wstring requested(groupId);
    const auto found = std::ranges::find_if(groups_, [&](const WallpaperGroup& group) {
        return _wcsicmp(group.id.c_str(), requested.c_str()) == 0;
    });
    return found == groups_.end() ? nullptr : &*found;
}

HRESULT WallpaperGroupStore::Load() {
    const std::filesystem::path path = libraryRoot_ / kMetadataFileName;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? S_OK : LastErrorResult();
    }
    LARGE_INTEGER size{};
    HRESULT result = GetFileSizeEx(file, &size) ? S_OK : LastErrorResult();
    Header header{};
    if (SUCCEEDED(result) &&
        (size.QuadPart < static_cast<LONGLONG>(sizeof(header)) ||
         size.QuadPart > static_cast<LONGLONG>(kMaximumFileBytes))) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    if (SUCCEEDED(result)) {
        result = ReadExact(file, &header, sizeof(header));
    }
    if (SUCCEEDED(result) &&
        (header.magic != kMagic || header.version != kVersion ||
         header.groupCount > kMaximumGroups ||
         header.favoriteCount > kMaximumMembers)) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::vector<std::wstring> favorites;
    std::vector<WallpaperGroup> groups;
    std::unordered_set<std::wstring> favoriteKeys;
    for (std::uint32_t index = 0; SUCCEEDED(result) &&
                                  index < header.favoriteCount; ++index) {
        std::wstring fileName;
        result = ReadString(file, fileName);
        if (SUCCEEDED(result) &&
            (!IsSafeFileName(fileName) ||
             !favoriteKeys.insert(OrdinalKey(fileName)).second)) {
            result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        if (SUCCEEDED(result)) {
            favorites.push_back(std::move(fileName));
        }
    }
    std::unordered_set<std::wstring> ids;
    std::unordered_set<std::wstring> names;
    std::uint64_t referenceCount = header.favoriteCount;
    for (std::uint32_t index = 0; SUCCEEDED(result) && index < header.groupCount;
         ++index) {
        WallpaperGroup group;
        std::uint32_t memberCount = 0;
        result = ReadString(file, group.id);
        if (SUCCEEDED(result)) {
            result = ReadString(file, group.name);
        }
        if (SUCCEEDED(result)) {
            result = ReadExact(file, &memberCount, sizeof(memberCount));
        }
        if (SUCCEEDED(result) &&
            (group.id.empty() || !IsValidGroupName(group.name) ||
             memberCount > kMaximumMembers ||
             referenceCount + memberCount > kMaximumReferences ||
             !ids.insert(OrdinalKey(group.id)).second ||
             !names.insert(OrdinalKey(group.name)).second)) {
            result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        referenceCount += memberCount;
        std::unordered_set<std::wstring> memberKeys;
        for (std::uint32_t member = 0; SUCCEEDED(result) && member < memberCount;
             ++member) {
            std::wstring fileName;
            result = ReadString(file, fileName);
            if (SUCCEEDED(result) &&
                (!IsSafeFileName(fileName) ||
                 !memberKeys.insert(OrdinalKey(fileName)).second)) {
                result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            if (SUCCEEDED(result)) {
                group.fileNames.push_back(std::move(fileName));
            }
        }
        if (SUCCEEDED(result)) {
            groups.push_back(std::move(group));
        }
    }
    LARGE_INTEGER current{};
    LARGE_INTEGER zero{};
    if (SUCCEEDED(result) &&
        (!SetFilePointerEx(file, zero, &current, FILE_CURRENT) ||
         current.QuadPart != size.QuadPart)) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    CloseHandle(file);
    if (FAILED(result)) {
        return result;
    }
    groups_ = std::move(groups);
    favorites_ = std::move(favorites);
    return S_OK;
}

HRESULT WallpaperGroupStore::Save() const {
    if (libraryRoot_.empty() || groups_.size() > kMaximumGroups ||
        favorites_.size() > kMaximumMembers) {
        return E_UNEXPECTED;
    }
    std::uint64_t referenceCount = favorites_.size();
    for (const WallpaperGroup& group : groups_) {
        referenceCount += group.fileNames.size();
        if (group.fileNames.size() > kMaximumMembers ||
            referenceCount > kMaximumReferences) {
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
    }
    const std::filesystem::path destination = libraryRoot_ / kMetadataFileName;
    const std::filesystem::path temporary = destination.native() + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return LastErrorResult();
    }
    const Header header{kMagic, kVersion,
                        static_cast<std::uint32_t>(favorites_.size()),
                        static_cast<std::uint32_t>(groups_.size())};
    HRESULT result = WriteExact(file, &header, sizeof(header));
    for (const std::wstring& favorite : favorites_) {
        if (SUCCEEDED(result)) {
            result = WriteString(file, favorite);
        }
    }
    for (const WallpaperGroup& group : groups_) {
        if (SUCCEEDED(result)) {
            result = WriteString(file, group.id);
        }
        if (SUCCEEDED(result)) {
            result = WriteString(file, group.name);
        }
        const auto count = static_cast<std::uint32_t>(group.fileNames.size());
        if (SUCCEEDED(result)) {
            result = WriteExact(file, &count, sizeof(count));
        }
        for (const std::wstring& fileName : group.fileNames) {
            if (SUCCEEDED(result)) {
                result = WriteString(file, fileName);
            }
        }
    }
    if (SUCCEEDED(result) && !FlushFileBuffers(file)) {
        result = LastErrorResult();
    }
    CloseHandle(file);
    if (SUCCEEDED(result) &&
        !MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = LastErrorResult();
    }
    if (FAILED(result)) {
        DeleteFileW(temporary.c_str());
    }
    return result;
}

}  // namespace lwe::core
