#include <chrono>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>

#include "app/WallpaperApplication.h"
#include "core/InstanceCoordinator.h"
#include "core/Logger.h"
#include "core/WallpaperLibrarySelfTest.h"

namespace {

struct StartupOptions final {
    std::chrono::seconds testDuration = std::chrono::seconds::zero();
    std::optional<std::wstring> testWallpaper;
    std::optional<std::wstring> libraryTestSource;
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

    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument(arguments[index]);
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
                options.testWallpaper = std::wstring(path);
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
        options.testWallpaper.reset();
    }
    return options;
}

}  // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    lwe::core::InitializeLogging();
    lwe::core::LogInfo(L"Live Wallpaper Engine technical spike starting.");

    const StartupOptions options = ParseStartupOptions();
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

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 1;
    if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE) {
        lwe::app::WallpaperApplication application(instance,
                                                   instanceCoordinator.ActivationEvent());
        exitCode = application.Run(options.testDuration, options.testWallpaper);
    } else {
        lwe::core::LogError(L"COM initialization failed.", comResult);
    }

    lwe::core::LogInfo(L"Live Wallpaper Engine stopped.");
    lwe::core::ShutdownLogging();

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return exitCode;
}
