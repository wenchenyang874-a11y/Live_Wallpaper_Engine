#include <chrono>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>

#include "app/WallpaperApplication.h"
#include "app/UpdateChecker.h"
#include "core/CrashDiagnostics.h"
#include "core/InstanceCoordinator.h"
#include "core/Logger.h"
#include "core/WallpaperLibrarySelfTest.h"

namespace {

struct StartupOptions final {
    std::chrono::seconds testDuration = std::chrono::seconds::zero();
    std::vector<std::wstring> testWallpapers;
    std::optional<std::wstring> libraryTestSource;
    lwe::app::updates::UpdateCheckMode updateCheckMode =
        lwe::app::updates::UpdateCheckMode::Live;
    std::wstring crashDiagnosticsTestMode;
    std::filesystem::path crashDiagnosticsTestDirectory;
    bool updateCheckerSelfTest = false;
};

StartupOptions ParseStartupOptions() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return {};
    }

    StartupOptions options;
    constexpr std::wstring_view durationPrefix = L"--test-seconds=";
    constexpr std::wstring_view wallpaperPrefix = L"--test-wallpaper=";
    constexpr std::wstring_view legacyImagePrefix = L"--test-static-image=";
    constexpr std::wstring_view libraryTestPrefix = L"--test-library-package=";
    constexpr std::wstring_view crashDiagnosticsPrefix =
        L"--test-crash-diagnostics=";
    constexpr std::wstring_view crashDirectoryPrefix =
        L"--test-crash-directory=";

    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--test-update-check") {
            options.updateCheckerSelfTest = true;
            continue;
        }
        if (argument == L"--test-update-result=rate-limit") {
            options.updateCheckMode =
                lwe::app::updates::UpdateCheckMode::SimulatedRateLimit;
            continue;
        }
        if (argument.starts_with(crashDiagnosticsPrefix)) {
            options.crashDiagnosticsTestMode =
                argument.substr(crashDiagnosticsPrefix.size());
            continue;
        }
        if (argument.starts_with(crashDirectoryPrefix)) {
            options.crashDiagnosticsTestDirectory =
                std::wstring(argument.substr(crashDirectoryPrefix.size()));
            continue;
        }
        if (argument.starts_with(libraryTestPrefix)) {
            const std::wstring_view path = argument.substr(libraryTestPrefix.size());
            if (!path.empty()) {
                options.libraryTestSource = std::wstring(path);
            }
            continue;
        }
        if (argument.starts_with(wallpaperPrefix) ||
            argument.starts_with(legacyImagePrefix)) {
            const std::size_t prefixLength = argument.starts_with(wallpaperPrefix)
                                                 ? wallpaperPrefix.size()
                                                 : legacyImagePrefix.size();
            const std::wstring_view path = argument.substr(prefixLength);
            if (!path.empty()) {
                options.testWallpapers.emplace_back(path);
            }
            continue;
        }

        if (!argument.starts_with(durationPrefix)) {
            continue;
        }
        const std::wstring_view number = argument.substr(durationPrefix.size());
        wchar_t* end = nullptr;
        const long value = std::wcstol(number.data(), &end, 10);
        if (end != number.data() && *end == L'\0' && value > 0 && value <= 3600) {
            options.testDuration = std::chrono::seconds(value);
        }
    }

    LocalFree(arguments);
    if (options.testDuration.count() <= 0) {
        options.testWallpapers.clear();
    }
    return options;
}

int RunCrashDiagnosticsSelfTest(const StartupOptions& options) {
    if (options.crashDiagnosticsTestDirectory.empty()) {
        lwe::core::LogError(
            L"Crash diagnostics self-test requires an isolated directory.");
        return 2;
    }

    lwe::core::CrashDiagnostics diagnostics;
    if (!diagnostics.InitializeForTesting(
            options.crashDiagnosticsTestDirectory)) {
        lwe::core::LogError(L"Crash diagnostics self-test initialization failed.");
        return 3;
    }
    lwe::core::LogPreviousSession(diagnostics.PreviousSession());

    if (options.crashDiagnosticsTestMode == L"crash") {
        constexpr DWORD kTestExceptionCode = 0xE0424C57UL;
        RaiseException(kTestExceptionCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        TerminateProcess(GetCurrentProcess(), kTestExceptionCode);
    }
    if (options.crashDiagnosticsTestMode == L"leave-unclean") {
        ExitProcess(77);
    }

    int exitCode = 0;
    const lwe::core::PreviousExitStatus previousStatus =
        diagnostics.PreviousSession().status;
    if (options.crashDiagnosticsTestMode == L"verify-clean" &&
        previousStatus != lwe::core::PreviousExitStatus::Clean) {
        exitCode = 4;
    } else if (options.crashDiagnosticsTestMode == L"verify-crash" &&
               previousStatus != lwe::core::PreviousExitStatus::Crashed) {
        exitCode = 5;
    } else if (options.crashDiagnosticsTestMode == L"verify-unclean" &&
               previousStatus != lwe::core::PreviousExitStatus::Unclean) {
        exitCode = 6;
    } else if (options.crashDiagnosticsTestMode != L"clean" &&
               options.crashDiagnosticsTestMode != L"verify-clean" &&
               options.crashDiagnosticsTestMode != L"verify-crash" &&
               options.crashDiagnosticsTestMode != L"verify-unclean") {
        exitCode = 7;
    }

    diagnostics.MarkCleanExit(exitCode);
    return exitCode;
}

}  // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    lwe::core::InitializeLogging();
    lwe::core::LogInfo(L"Live Wallpaper Engine technical spike starting.");

    const StartupOptions options = ParseStartupOptions();
    if (!options.crashDiagnosticsTestMode.empty()) {
        const int exitCode = RunCrashDiagnosticsSelfTest(options);
        lwe::core::ShutdownLogging();
        return exitCode;
    }
    if (options.updateCheckerSelfTest) {
        const int exitCode = lwe::app::updates::RunUpdateCheckerSelfTest();
        if (exitCode == 0) {
            lwe::core::LogInfo(L"Update checker self-test passed.");
        } else {
            lwe::core::LogError(
                L"Update checker self-test failed with code " +
                std::to_wstring(exitCode) + L'.');
        }
        lwe::core::ShutdownLogging();
        return exitCode;
    }
    if (options.libraryTestSource.has_value()) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int exitCode = 1;
        if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE) {
            const HRESULT mediaFoundationResult =
                MFStartup(MF_VERSION, MFSTARTUP_FULL);
            if (SUCCEEDED(mediaFoundationResult)) {
                exitCode =
                    lwe::core::RunWallpaperLibrarySelfTest(*options.libraryTestSource);
                MFShutdown();
            } else {
                lwe::core::LogError(L"Media Foundation initialization failed.",
                                    mediaFoundationResult);
            }
        } else {
            lwe::core::LogError(L"COM initialization failed.", comResult);
        }
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
        lwe::core::LogInfo(L"Live Wallpaper Engine self-test stopped.");
        lwe::core::ShutdownLogging();
        return exitCode;
    }

    lwe::core::InstanceCoordinator instanceCoordinator;
    const lwe::core::InstanceStartResult instanceResult = instanceCoordinator.Initialize();
    if (instanceResult == lwe::core::InstanceStartResult::ExistingActivated) {
        lwe::core::ShutdownLogging();
        return 0;
    }
    if (instanceResult == lwe::core::InstanceStartResult::Failed) {
        MessageBoxW(nullptr, L"程序无法启动或唤醒已有实例，请查看本地日志。",
                    L"Live Wallpaper Engine", MB_OK | MB_ICONERROR);
        lwe::core::ShutdownLogging();
        return 1;
    }

    lwe::core::CrashDiagnostics crashDiagnostics;
    if (crashDiagnostics.Initialize()) {
        lwe::core::LogPreviousSession(crashDiagnostics.PreviousSession());
    } else {
        lwe::core::LogWarning(
            L"Local crash diagnostics could not be initialized.");
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 1;
    if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE) {
        lwe::app::WallpaperApplication application(instance,
                                                   instanceCoordinator.ActivationEvent());
        exitCode = application.Run(options.testDuration, options.testWallpapers,
                                   options.updateCheckMode);
    } else {
        lwe::core::LogError(L"COM initialization failed.", comResult);
    }

    lwe::core::LogInfo(L"Live Wallpaper Engine stopped.");
    lwe::core::ShutdownLogging();

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    crashDiagnostics.MarkCleanExit(exitCode);
    return exitCode;
}
