#include "core/SettingsStore.h"

#include <limits>
#include <string>
#include <string_view>

#include <shlobj.h>

#include "core/Logger.h"

namespace lwe::core {
namespace {

constexpr wchar_t kSettingsFileName[] = L"settings.json";
constexpr wchar_t kLegacySettingsFileName[] = L"settings.v1.json";
constexpr wchar_t kTemporarySuffix[] = L".tmp";
constexpr LONGLONG kMaximumSettingsBytes = 1024 * 1024;

HRESULT ResolveSettingsPaths(std::wstring& directory, std::wstring& settingsPath,
                             std::wstring* legacyPath = nullptr) {
    PWSTR localAppData = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (FAILED(result) || localAppData == nullptr) {
        CoTaskMemFree(localAppData);
        return FAILED(result) ? result : E_UNEXPECTED;
    }

    directory.assign(localAppData);
    CoTaskMemFree(localAppData);
    directory += L"\\LiveWallpaperEngine";
    settingsPath = directory + L"\\" + kSettingsFileName;
    if (legacyPath != nullptr) {
        *legacyPath = directory + L"\\" + kLegacySettingsFileName;
    }
    return S_OK;
}

std::optional<std::wstring> Utf8ToWide(const std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring{};
    }

    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), wide.data(), required) != required) {
        return std::nullopt;
    }
    return wide;
}

std::optional<std::string> WideToUtf8(const std::wstring_view wide) {
    if (wide.empty()) {
        return std::string{};
    }

    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), utf8.data(), required, nullptr,
                            nullptr) != required) {
        return std::nullopt;
    }
    return utf8;
}

std::wstring EscapeJsonString(const std::wstring_view value) {
    constexpr wchar_t hexadecimal[] = L"0123456789ABCDEF";
    std::wstring escaped;
    escaped.reserve(value.size());

    for (const wchar_t character : value) {
        switch (character) {
            case L'"':
                escaped += L"\\\"";
                break;
            case L'\\':
                escaped += L"\\\\";
                break;
            case L'\b':
                escaped += L"\\b";
                break;
            case L'\f':
                escaped += L"\\f";
                break;
            case L'\n':
                escaped += L"\\n";
                break;
            case L'\r':
                escaped += L"\\r";
                break;
            case L'\t':
                escaped += L"\\t";
                break;
            default:
                if (character < 0x20) {
                    escaped += L"\\u";
                    escaped.push_back(hexadecimal[(character >> 12) & 0xF]);
                    escaped.push_back(hexadecimal[(character >> 8) & 0xF]);
                    escaped.push_back(hexadecimal[(character >> 4) & 0xF]);
                    escaped.push_back(hexadecimal[character & 0xF]);
                } else {
                    escaped.push_back(character);
                }
                break;
        }
    }
    return escaped;
}

std::optional<std::size_t> FindValueStart(const std::wstring_view json,
                                          const std::wstring_view key) {
    const std::wstring token = L"\"" + std::wstring(key) + L"\"";
    std::size_t position = json.find(token);
    if (position == std::wstring_view::npos) {
        return std::nullopt;
    }

    position = json.find(L':', position + token.size());
    if (position == std::wstring_view::npos) {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() && iswspace(json[position]) != 0) {
        ++position;
    }
    return position;
}

int HexadecimalValue(const wchar_t character) {
    if (character >= L'0' && character <= L'9') {
        return character - L'0';
    }
    if (character >= L'a' && character <= L'f') {
        return character - L'a' + 10;
    }
    if (character >= L'A' && character <= L'F') {
        return character - L'A' + 10;
    }
    return -1;
}

std::optional<std::wstring> ParseStringField(const std::wstring_view json,
                                             const std::wstring_view key) {
    const std::optional start = FindValueStart(json, key);
    if (!start.has_value() || *start >= json.size() || json[*start] != L'"') {
        return std::nullopt;
    }

    std::wstring value;
    for (std::size_t position = *start + 1; position < json.size(); ++position) {
        const wchar_t character = json[position];
        if (character == L'"') {
            return value;
        }
        if (character != L'\\') {
            if (character < 0x20) {
                return std::nullopt;
            }
            value.push_back(character);
            continue;
        }

        if (++position >= json.size()) {
            return std::nullopt;
        }
        switch (json[position]) {
            case L'"':
            case L'\\':
            case L'/':
                value.push_back(json[position]);
                break;
            case L'b':
                value.push_back(L'\b');
                break;
            case L'f':
                value.push_back(L'\f');
                break;
            case L'n':
                value.push_back(L'\n');
                break;
            case L'r':
                value.push_back(L'\r');
                break;
            case L't':
                value.push_back(L'\t');
                break;
            case L'u': {
                if (position + 4 >= json.size()) {
                    return std::nullopt;
                }
                unsigned int codeUnit = 0;
                for (int offset = 1; offset <= 4; ++offset) {
                    const int digit = HexadecimalValue(json[position + offset]);
                    if (digit < 0) {
                        return std::nullopt;
                    }
                    codeUnit = (codeUnit << 4U) | static_cast<unsigned int>(digit);
                }
                value.push_back(static_cast<wchar_t>(codeUnit));
                position += 4;
                break;
            }
            default:
                return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ParseVersionField(const std::wstring_view json) {
    const std::optional start = FindValueStart(json, L"version");
    if (!start.has_value()) {
        return std::nullopt;
    }

    std::size_t end = *start;
    while (end < json.size() && json[end] >= L'0' && json[end] <= L'9') {
        ++end;
    }
    if (end == *start) {
        return std::nullopt;
    }

    std::uint32_t version = 0;
    for (std::size_t position = *start; position < end; ++position) {
        const std::uint32_t digit = static_cast<std::uint32_t>(json[position] - L'0');
        if (version > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        version = version * 10U + digit;
    }
    return version;
}

std::optional<bool> ParseBooleanField(const std::wstring_view json,
                                      const std::wstring_view key) {
    const std::optional start = FindValueStart(json, key);
    if (!start.has_value()) {
        return std::nullopt;
    }
    if (json.substr(*start, 4) == L"true") {
        return true;
    }
    if (json.substr(*start, 5) == L"false") {
        return false;
    }
    return std::nullopt;
}

HRESULT ReadSettingsFile(const std::wstring& path, std::wstring& json) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(file);
        return result;
    }
    if (size.QuadPart < 0 || size.QuadPart > kMaximumSettingsBytes) {
        CloseHandle(file);
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    std::string utf8(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    const BOOL read = ReadFile(file, utf8.data(), static_cast<DWORD>(utf8.size()),
                               &bytesRead, nullptr);
    const DWORD readError = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!read) {
        return HRESULT_FROM_WIN32(readError);
    }
    utf8.resize(bytesRead);

    const std::optional decoded = Utf8ToWide(utf8);
    if (!decoded.has_value()) {
        return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
    }
    json = *decoded;
    return S_OK;
}

HRESULT WriteSettingsFileAtomically(const std::wstring& directory,
                                    const std::wstring& path,
                                    const std::wstring_view json) {
    const int createResult = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    if (createResult != ERROR_SUCCESS && createResult != ERROR_ALREADY_EXISTS &&
        createResult != ERROR_FILE_EXISTS) {
        return HRESULT_FROM_WIN32(createResult);
    }

    const std::optional utf8 = WideToUtf8(json);
    if (!utf8.has_value()) {
        return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
    }

    const std::wstring temporaryPath = path + kTemporarySuffix;
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    DWORD bytesWritten = 0;
    const BOOL wrote = WriteFile(file, utf8->data(), static_cast<DWORD>(utf8->size()),
                                 &bytesWritten, nullptr);
    HRESULT result = S_OK;
    if (!wrote || bytesWritten != utf8->size()) {
        result = HRESULT_FROM_WIN32(wrote ? ERROR_WRITE_FAULT : GetLastError());
    } else if (!FlushFileBuffers(file)) {
        result = HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(file);

    if (SUCCEEDED(result) &&
        !MoveFileExW(temporaryPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(result)) {
        DeleteFileW(temporaryPath.c_str());
    }
    return result;
}

}  // namespace

std::optional<AppSettings> SettingsStore::Load() const {
    std::wstring directory;
    std::wstring path;
    std::wstring legacyPath;
    HRESULT result = ResolveSettingsPaths(directory, path, &legacyPath);
    if (FAILED(result)) {
        LogError(L"Unable to resolve the settings path.", result);
        return std::nullopt;
    }

    std::wstring json;
    result = ReadSettingsFile(path, json);
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        path = legacyPath;
        result = ReadSettingsFile(path, json);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return std::nullopt;
        }
    }
    if (FAILED(result)) {
        LogError(L"Unable to read the local settings file.", result);
        return std::nullopt;
    }

    const std::optional version = ParseVersionField(json);
    const std::optional wallpaperType = ParseStringField(json, L"wallpaperType");
    if (!version.has_value() || !wallpaperType.has_value() ||
        (*version != 1 && *version != AppSettings::kCurrentSchemaVersion)) {
        LogWarning(L"The local settings file is invalid or uses an unsupported version.");
        return std::nullopt;
    }

    AppSettings settings;
    settings.schemaVersion = AppSettings::kCurrentSchemaVersion;
    if (*version == 1) {
        settings.wallpaperPath =
            ParseStringField(json, L"staticImagePath").value_or(L"");
    } else {
        settings.wallpaperPath =
            ParseStringField(json, L"wallpaperPath").value_or(L"");
        settings.soundEnabled =
            ParseBooleanField(json, L"soundEnabled").value_or(false);
    }

    if (*wallpaperType == L"static_image" && !settings.wallpaperPath.empty()) {
        settings.wallpaperKind = WallpaperSelectionKind::StaticImage;
    } else if (*wallpaperType == L"animated_gif" &&
               !settings.wallpaperPath.empty()) {
        settings.wallpaperKind = WallpaperSelectionKind::AnimatedGif;
    } else if (*wallpaperType == L"video" && !settings.wallpaperPath.empty()) {
        settings.wallpaperKind = WallpaperSelectionKind::Video;
    } else if (*wallpaperType == L"dynamic_test") {
        settings.wallpaperKind = WallpaperSelectionKind::DynamicTest;
    } else {
        LogWarning(L"The local settings file contains an unsupported wallpaper type.");
        return std::nullopt;
    }

    LogInfo(L"Loaded local wallpaper settings.");
    return settings;
}

HRESULT SettingsStore::Save(const AppSettings& settings) const {
    if (settings.schemaVersion != AppSettings::kCurrentSchemaVersion ||
        (settings.wallpaperKind != WallpaperSelectionKind::DynamicTest &&
         settings.wallpaperPath.empty())) {
        return E_INVALIDARG;
    }

    std::wstring_view wallpaperType = L"dynamic_test";
    switch (settings.wallpaperKind) {
        case WallpaperSelectionKind::StaticImage:
            wallpaperType = L"static_image";
            break;
        case WallpaperSelectionKind::AnimatedGif:
            wallpaperType = L"animated_gif";
            break;
        case WallpaperSelectionKind::Video:
            wallpaperType = L"video";
            break;
        case WallpaperSelectionKind::DynamicTest:
            break;
    }
    std::wstring json = L"{\r\n  \"version\": 2,\r\n  \"wallpaperType\": \"";
    json += wallpaperType;
    json += L"\",\r\n  \"wallpaperPath\": \"";
    json += EscapeJsonString(settings.wallpaperPath);
    json += L"\",\r\n  \"soundEnabled\": ";
    json += settings.soundEnabled ? L"true" : L"false";
    json += L"\r\n}\r\n";

    std::wstring directory;
    std::wstring path;
    HRESULT result = ResolveSettingsPaths(directory, path);
    if (FAILED(result)) {
        LogError(L"Unable to resolve the settings path.", result);
        return result;
    }

    result = WriteSettingsFileAtomically(directory, path, json);
    if (FAILED(result)) {
        LogError(L"Unable to save the local settings file atomically.", result);
        return result;
    }

    LogInfo(L"Saved local wallpaper settings atomically.");
    return S_OK;
}

}  // namespace lwe::core
