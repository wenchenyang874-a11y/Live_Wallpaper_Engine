#include "app/UpdateChecker.h"

#include <charconv>
#include <chrono>
#include <limits>
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
constexpr wchar_t kReleaseListUrl[] =
    L"https://github.com/wenchenyang874-a11y/Live_Wallpaper_Engine/releases";
constexpr DWORD kHttpStatusTooManyRequests = 429;
constexpr std::size_t kMaximumResponseBytes = 1024U * 1024U;

struct InternetHandleCloser final {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

using InternetHandle = std::unique_ptr<void, InternetHandleCloser>;

UpdateCheckResult ErrorResult(std::wstring summary, std::wstring message) {
    UpdateCheckResult result;
    result.status = UpdateStatus::Error;
    result.currentTag = CurrentVersionTag();
    result.releaseUrl = kReleaseListUrl;
    result.errorSummary = std::move(summary);
    result.errorMessage = std::move(message);
    return result;
}

std::wstring WinHttpErrorDetail(const DWORD error) {
    return L"WinHTTP 错误代码 " + std::to_wstring(error) + L"。";
}

UpdateCheckResult NetworkErrorResult(const DWORD error) {
    switch (error) {
        case ERROR_WINHTTP_TIMEOUT:
            return ErrorResult(L"连接 GitHub 超时。", WinHttpErrorDetail(error));
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return ErrorResult(L"无法解析 GitHub 的网络地址。",
                               WinHttpErrorDetail(error));
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return ErrorResult(L"无法连接到 GitHub。", WinHttpErrorDetail(error));
        case ERROR_WINHTTP_SECURE_FAILURE:
            return ErrorResult(L"与 GitHub 建立安全连接失败。",
                               WinHttpErrorDetail(error));
        default:
            return ErrorResult(L"请求 GitHub 时发生网络错误。",
                               WinHttpErrorDetail(error));
    }
}

std::optional<std::wstring> ResponseHeader(void* request,
                                           const wchar_t* headerName) {
    DWORD size = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, headerName, nullptr,
                            &size, WINHTTP_NO_HEADER_INDEX) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, headerName,
                             value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    while (!value.empty() &&
           (value.back() == L'\0' || value.back() == L'\r' ||
            value.back() == L'\n' || value.back() == L' ')) {
        value.pop_back();
    }
    const std::size_t first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return std::nullopt;
    }
    value.erase(0, first);
    return value;
}

std::optional<std::wstring> LocalResetTime(const std::wstring_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string ascii;
    ascii.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        ascii.push_back(static_cast<char>(character));
    }
    std::uint64_t seconds = 0;
    const auto parsed = std::from_chars(
        ascii.data(), ascii.data() + ascii.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != ascii.data() + ascii.size()) {
        return std::nullopt;
    }
    constexpr std::uint64_t kUnixEpochFileTimeSeconds = 11644473600ULL;
    constexpr std::uint64_t kTicksPerSecond = 10000000ULL;
    if (seconds > std::numeric_limits<std::uint64_t>::max() / kTicksPerSecond -
                      kUnixEpochFileTimeSeconds) {
        return std::nullopt;
    }
    ULARGE_INTEGER ticks{};
    ticks.QuadPart =
        (seconds + kUnixEpochFileTimeSeconds) * kTicksPerSecond;
    FILETIME fileTime{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&fileTime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return std::nullopt;
    }
    wchar_t formatted[32]{};
    swprintf_s(formatted, L"%04u-%02u-%02u %02u:%02u", local.wYear,
               local.wMonth, local.wDay, local.wHour, local.wMinute);
    return std::wstring(formatted);
}

UpdateCheckResult HttpStatusError(
    const DWORD statusCode, const std::optional<std::wstring>& rateRemaining,
    const std::optional<std::wstring>& rateReset,
    const std::optional<std::wstring>& retryAfter) {
    const std::wstring httpStatus =
        L"HTTP " + std::to_wstring(statusCode) + L"。";
    if (statusCode == HTTP_STATUS_FORBIDDEN && rateRemaining == L"0") {
        const auto reset = rateReset.has_value()
                               ? LocalResetTime(*rateReset)
                               : std::nullopt;
        const std::wstring detail = reset.has_value()
                                        ? L"额度将在本地时间 " + *reset +
                                              L" 重置（HTTP 403）。"
                                        : L"匿名请求额度暂时为 0（HTTP 403）。";
        return ErrorResult(L"GitHub API 匿名请求额度已用完。", detail);
    }
    if (statusCode == kHttpStatusTooManyRequests) {
        const std::wstring detail =
            retryAfter.has_value()
                ? L"请在 " + *retryAfter + L" 秒后重试（HTTP 429）。"
                : L"GitHub 要求稍后重试（HTTP 429）。";
        return ErrorResult(L"检查更新的请求过于频繁。", detail);
    }
    if (statusCode == HTTP_STATUS_NOT_FOUND) {
        return ErrorResult(L"仓库当前没有可供检查的公开 Release。",
                           httpStatus);
    }
    if (statusCode >= 500 && statusCode <= 599) {
        return ErrorResult(L"GitHub 服务暂时不可用。", httpStatus);
    }
    if (statusCode == HTTP_STATUS_FORBIDDEN) {
        return ErrorResult(L"GitHub 拒绝了更新检查请求。", httpStatus);
    }
    return ErrorResult(L"GitHub 返回了意外的响应状态。", httpStatus);
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
        return ErrorResult(L"GitHub 返回的版本信息不完整。",
                           L"响应中缺少版本号或 Release 地址。");
    }
    const auto latestVersion = ParseVersionTag(*tag);
    const auto latestTag = Utf8ToWide(*tag);
    const auto releaseUrl = Utf8ToWide(*url);
    if (!latestVersion.has_value() || !latestTag.has_value() ||
        !releaseUrl.has_value() || !url->starts_with(kAllowedReleasePrefix)) {
        return ErrorResult(L"GitHub 返回的版本信息无法识别。",
                           L"版本号或 Release 地址格式不符合预期。");
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

UpdateCheckResult CheckForLatestRelease(const std::stop_token stopToken,
                                        const UpdateCheckMode mode) {
    if (stopToken.stop_requested()) {
        return ErrorResult(L"更新检查已取消。", L"没有发出更新请求。");
    }
    if (mode == UpdateCheckMode::SimulatedRateLimit) {
        const auto reset = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now()
                                   .time_since_epoch())
                               .count() +
                           300;
        return HttpStatusError(
            HTTP_STATUS_FORBIDDEN, std::optional<std::wstring>{L"0"},
            std::optional<std::wstring>{std::to_wstring(reset)}, std::nullopt);
    }

    const std::wstring userAgent =
        L"LiveWallpaperEngine/" + CurrentVersionTag();
    InternetHandle session(WinHttpOpen(
        userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return NetworkErrorResult(GetLastError());
    }
    WinHttpSetTimeouts(session.get(), 3000, 3000, 5000, 5000);

    InternetHandle connection(
        WinHttpConnect(session.get(), kGitHubHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        return NetworkErrorResult(GetLastError());
    }
    InternetHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", kLatestReleasePath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        return NetworkErrorResult(GetLastError());
    }
    constexpr wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpAddRequestHeaders(request.get(), headers,
                                  static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        return NetworkErrorResult(GetLastError());
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return NetworkErrorResult(GetLastError());
    }
    if (stopToken.stop_requested()) {
        return ErrorResult(L"更新检查已取消。", L"请求结果已忽略。");
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        return NetworkErrorResult(GetLastError());
    }
    if (statusCode != HTTP_STATUS_OK) {
        return HttpStatusError(
            statusCode, ResponseHeader(request.get(), L"X-RateLimit-Remaining"),
            ResponseHeader(request.get(), L"X-RateLimit-Reset"),
            ResponseHeader(request.get(), L"Retry-After"));
    }

    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            return NetworkErrorResult(GetLastError());
        }
        if (available == 0) {
            break;
        }
        if (response.size() + available > kMaximumResponseBytes) {
            return ErrorResult(L"GitHub 返回的版本信息过大。",
                               L"为保护本机资源，已停止读取超过 1 MiB 的响应。");
        }
        const std::size_t offset = response.size();
        response.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), response.data() + offset, available,
                             &received)) {
            return NetworkErrorResult(GetLastError());
        }
        response.resize(offset + received);
        if (stopToken.stop_requested()) {
            return ErrorResult(L"更新检查已取消。", L"请求结果已忽略。");
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

    const UpdateCheckResult rateLimited = HttpStatusError(
        HTTP_STATUS_FORBIDDEN, std::optional<std::wstring>{L"0"},
        std::optional<std::wstring>{L"1787714231"}, std::nullopt);
    if (rateLimited.status != UpdateStatus::Error ||
        rateLimited.errorSummary.find(L"额度已用完") == std::wstring::npos ||
        rateLimited.errorMessage.find(L"HTTP 403") == std::wstring::npos ||
        rateLimited.releaseUrl != kReleaseListUrl) {
        return 6;
    }

    const UpdateCheckResult missingRelease =
        HttpStatusError(HTTP_STATUS_NOT_FOUND, std::nullopt, std::nullopt,
                        std::nullopt);
    if (missingRelease.errorSummary.find(L"没有可供检查") ==
            std::wstring::npos ||
        missingRelease.errorMessage != L"HTTP 404。") {
        return 7;
    }

    const UpdateCheckResult timeout = NetworkErrorResult(ERROR_WINHTTP_TIMEOUT);
    if (timeout.errorSummary.find(L"超时") == std::wstring::npos ||
        timeout.errorMessage.find(std::to_wstring(ERROR_WINHTTP_TIMEOUT)) ==
            std::wstring::npos ||
        timeout.releaseUrl != kReleaseListUrl) {
        return 8;
    }
    return 0;
}

}  // namespace lwe::app::updates
