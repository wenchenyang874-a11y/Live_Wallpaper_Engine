#include "app/UpdateChecker.h"

#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <windows.h>
#include <winhttp.h>

#ifndef LWE_VERSION_MAJOR
#define LWE_VERSION_MAJOR 0
#endif
#ifndef LWE_VERSION_MINOR
#define LWE_VERSION_MINOR 0
#endif
#ifndef LWE_VERSION_PATCH
#define LWE_VERSION_PATCH 0
#endif

namespace lwe::app::updates {
namespace {

constexpr wchar_t kGitHubHost[] = L"api.github.com";
constexpr wchar_t kLatestReleasePath[] =
    L"/repos/wenchenyang874-a11y/Live_Wallpaper_Engine/releases/latest";
constexpr std::string_view kAllowedReleasePrefix =
    "https://github.com/wenchenyang874-a11y/Live_Wallpaper_Engine/releases/";
constexpr std::size_t kMaximumResponseBytes = 1024U * 1024U;

struct InternetHandleCloser final {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

using InternetHandle = std::unique_ptr<void, InternetHandleCloser>;

UpdateCheckResult ErrorResult(std::wstring message) {
    UpdateCheckResult result;
    result.status = UpdateStatus::Error;
    result.currentTag = CurrentVersionTag();
    result.errorMessage = std::move(message);
    return result;
}

std::optional<std::uint32_t> ParseVersionPart(
    const std::string_view text) {
    if (text.empty() || (text.size() > 1U && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<SemanticVersion> ParseVersionTag(std::string_view tag) {
    if (!tag.empty() && tag.front() == 'v') {
        tag.remove_prefix(1);
    }
    const std::size_t firstDot = tag.find('.');
    const std::size_t secondDot = firstDot == std::string_view::npos
                                      ? std::string_view::npos
                                      : tag.find('.', firstDot + 1U);
    if (firstDot == std::string_view::npos ||
        secondDot == std::string_view::npos ||
        tag.find('.', secondDot + 1U) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = ParseVersionPart(tag.substr(0, firstDot));
    const auto minor = ParseVersionPart(
        tag.substr(firstDot + 1U, secondDot - firstDot - 1U));
    const auto patch = ParseVersionPart(tag.substr(secondDot + 1U));
    if (!major.has_value() || !minor.has_value() || !patch.has_value()) {
        return std::nullopt;
    }
    return SemanticVersion{*major, *minor, *patch};
}

bool IsNewer(const SemanticVersion candidate,
             const SemanticVersion current) noexcept {
    return std::tie(candidate.major, candidate.minor, candidate.patch) >
           std::tie(current.major, current.minor, current.patch);
}

std::optional<std::string> JsonStringValue(const std::string_view json,
                                           const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t keyPosition = json.find(needle);
    if (keyPosition == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t position = json.find(':', keyPosition + needle.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return std::nullopt;
    }
    ++position;
    std::string value;
    while (position < json.size()) {
        const char character = json[position++];
        if (character == '"') {
            return value;
        }
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        if (position >= json.size()) {
            return std::nullopt;
        }
        const char escaped = json[position++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), value.data(), length) !=
        length) {
        return std::nullopt;
    }
    return value;
}

UpdateCheckResult EvaluateReleaseJson(const std::string_view json,
                                      const SemanticVersion current) {
    const auto tag = JsonStringValue(json, "tag_name");
    const auto url = JsonStringValue(json, "html_url");
    if (!tag.has_value() || !url.has_value()) {
        return ErrorResult(L"GitHub 返回的版本信息不完整，请稍后重试。");
    }
    const auto latestVersion = ParseVersionTag(*tag);
    const auto latestTag = Utf8ToWide(*tag);
    const auto releaseUrl = Utf8ToWide(*url);
    if (!latestVersion.has_value() || !latestTag.has_value() ||
        !releaseUrl.has_value() || !url->starts_with(kAllowedReleasePrefix)) {
        return ErrorResult(L"GitHub 返回的版本信息无法识别，请稍后重试。");
    }

    UpdateCheckResult result;
    result.status = IsNewer(*latestVersion, current)
                        ? UpdateStatus::UpdateAvailable
                        : UpdateStatus::UpToDate;
    result.currentTag = CurrentVersionTag();
    result.latestTag = *latestTag;
    result.releaseUrl = *releaseUrl;
    return result;
}

}  // namespace

SemanticVersion CurrentVersion() noexcept {
    return SemanticVersion{LWE_VERSION_MAJOR, LWE_VERSION_MINOR,
                           LWE_VERSION_PATCH};
}

std::wstring CurrentVersionTag() {
    const SemanticVersion version = CurrentVersion();
    return L"v" + std::to_wstring(version.major) + L"." +
           std::to_wstring(version.minor) + L"." +
           std::to_wstring(version.patch);
}

UpdateCheckResult CheckForLatestRelease(const std::stop_token stopToken) {
    if (stopToken.stop_requested()) {
        return ErrorResult(L"更新检查已取消。");
    }

    const std::wstring userAgent =
        L"LiveWallpaperEngine/" + CurrentVersionTag();
    InternetHandle session(WinHttpOpen(
        userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return ErrorResult(L"无法初始化网络连接，请稍后重试。");
    }
    WinHttpSetTimeouts(session.get(), 3000, 3000, 5000, 5000);

    InternetHandle connection(
        WinHttpConnect(session.get(), kGitHubHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        return ErrorResult(L"无法连接 GitHub，请检查网络后重试。");
    }
    InternetHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", kLatestReleasePath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        return ErrorResult(L"无法创建更新请求，请稍后重试。");
    }
    constexpr wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpAddRequestHeaders(request.get(), headers,
                                  static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return ErrorResult(L"无法连接 GitHub，请检查网络后重试。");
    }
    if (stopToken.stop_requested()) {
        return ErrorResult(L"更新检查已取消。");
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX) ||
        statusCode != HTTP_STATUS_OK) {
        return ErrorResult(L"GitHub 暂时无法提供更新信息，请稍后重试。");
    }

    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            return ErrorResult(L"读取更新信息失败，请稍后重试。");
        }
        if (available == 0) {
            break;
        }
        if (response.size() + available > kMaximumResponseBytes) {
            return ErrorResult(L"GitHub 返回的版本信息过大，已停止读取。");
        }
        const std::size_t offset = response.size();
        response.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), response.data() + offset, available,
                             &received)) {
            return ErrorResult(L"读取更新信息失败，请稍后重试。");
        }
        response.resize(offset + received);
        if (stopToken.stop_requested()) {
            return ErrorResult(L"更新检查已取消。");
        }
    }
    return EvaluateReleaseJson(response, CurrentVersion());
}

int RunUpdateCheckerSelfTest() {
    const auto v100 = ParseVersionTag("v1.0.0");
    const auto v101 = ParseVersionTag("1.0.1");
    if (!v100.has_value() || !v101.has_value() ||
        ParseVersionTag("v1.0").has_value() ||
        ParseVersionTag("v01.0.0").has_value() ||
        !IsNewer(*v101, *v100) || IsNewer(*v100, *v100)) {
        return 1;
    }

    constexpr std::string_view availableJson =
        R"({"tag_name":"v1.1.0","html_url":"https://github.com/wenchenyang874-a11y/Live_Wallpaper_Engine/releases/tag/v1.1.0"})";
    const UpdateCheckResult available =
        EvaluateReleaseJson(availableJson, SemanticVersion{1, 0, 0});
    if (available.status != UpdateStatus::UpdateAvailable ||
        available.latestTag != L"v1.1.0" || available.releaseUrl.empty()) {
        return 2;
    }

    const UpdateCheckResult current =
        EvaluateReleaseJson(availableJson, SemanticVersion{1, 1, 0});
    if (current.status != UpdateStatus::UpToDate) {
        return 3;
    }

    constexpr std::string_view unsafeJson =
        R"({"tag_name":"v9.0.0","html_url":"https://example.com/download"})";
    if (EvaluateReleaseJson(unsafeJson, SemanticVersion{1, 0, 0}).status !=
        UpdateStatus::Error) {
        return 4;
    }
    if (EvaluateReleaseJson("{}", SemanticVersion{1, 0, 0}).status !=
        UpdateStatus::Error) {
        return 5;
    }
    return 0;
}

}  // namespace lwe::app::updates
