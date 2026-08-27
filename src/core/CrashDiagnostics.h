#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

namespace lwe::core {

enum class PreviousExitStatus {
    None,
    Clean,
    Crashed,
    Unclean,
};

struct PreviousSessionInfo final {
    PreviousExitStatus status = PreviousExitStatus::None;
    std::wstring version;
    std::wstring startedAtUtc;
    std::wstring endedAtUtc;
    std::wstring dumpFile;
    DWORD processId = 0;
    int exitCode = 0;
    DWORD exceptionCode = 0;
};

class CrashDiagnostics final {
public:
    CrashDiagnostics() = default;
    ~CrashDiagnostics();

    CrashDiagnostics(const CrashDiagnostics&) = delete;
    CrashDiagnostics& operator=(const CrashDiagnostics&) = delete;

    [[nodiscard]] bool Initialize();
    [[nodiscard]] bool InitializeForTesting(
        const std::filesystem::path& applicationDataRoot);
    void MarkCleanExit(int exitCode) noexcept;

    [[nodiscard]] const PreviousSessionInfo& PreviousSession() const noexcept;

private:
    [[nodiscard]] bool InitializeAtRoot(
        const std::filesystem::path& applicationDataRoot);

    PreviousSessionInfo previousSession_;
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter_ = nullptr;
    bool initialized_ = false;
    bool completed_ = false;
};

void LogPreviousSession(const PreviousSessionInfo& session);

}  // namespace lwe::core
