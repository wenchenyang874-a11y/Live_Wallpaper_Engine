#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>

namespace lwe::app::updates {

struct SemanticVersion final {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

enum class UpdateStatus {
    UpdateAvailable,
    UpToDate,
    Error,
};

enum class UpdateCheckMode {
    Live,
    SimulatedRateLimit,
};

struct UpdateCheckResult final {
    UpdateStatus status = UpdateStatus::Error;
    std::wstring currentTag;
    std::wstring latestTag;
    std::wstring releaseUrl;
    std::wstring errorSummary;
    std::wstring errorMessage;
};

[[nodiscard]] SemanticVersion CurrentVersion() noexcept;
[[nodiscard]] std::wstring CurrentVersionTag();
[[nodiscard]] UpdateCheckResult CheckForLatestRelease(
    std::stop_token stopToken,
    UpdateCheckMode mode = UpdateCheckMode::Live);
[[nodiscard]] int RunUpdateCheckerSelfTest();

}  // namespace lwe::app::updates
