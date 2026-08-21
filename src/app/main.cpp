#include <chrono>
#include <cwchar>
#include <optional>
#include <string_view>

#include <windows.h>
#include <shellapi.h>

#include "app/WallpaperApplication.h"
#include "core/Logger.h"

namespace {

std::chrono::seconds ParseTestDuration() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return std::chrono::seconds::zero();
    }

    std::chrono::seconds duration = std::chrono::seconds::zero();
    constexpr std::wstring_view prefix = L"--test-seconds=";

    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (!argument.starts_with(prefix)) {
            continue;
        }

        const std::wstring_view number = argument.substr(prefix.size());
        wchar_t* end = nullptr;
        const long value = std::wcstol(number.data(), &end, 10);
        if (end != number.data() && *end == L'\0' && value > 0 && value <= 3600) {
            duration = std::chrono::seconds(value);
        }
    }

    LocalFree(arguments);
    return duration;
}

}  // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    lwe::core::InitializeLogging();
    lwe::core::LogInfo(L"Live Wallpaper Engine technical spike starting.");

    int exitCode = 1;
    if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE) {
        lwe::app::WallpaperApplication application(instance);
        exitCode = application.Run(ParseTestDuration());
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
