#include "platform/ProcessResourceMonitor.h"

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <string>
#include <vector>

#include <psapi.h>
#include <pdhmsg.h>

#include "core/Logger.h"

namespace lwe::platform {
namespace {

std::uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

std::wstring Lowercase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

}  // namespace

ProcessResourceMonitor::~ProcessResourceMonitor() {
    Shutdown();
}

bool ProcessResourceMonitor::Initialize() {
    Shutdown();
    processId_ = GetCurrentProcessId();
    SYSTEM_INFO systemInformation{};
    GetSystemInfo(&systemInformation);
    processorCount_ = std::max(1UL, systemInformation.dwNumberOfProcessors);

    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        previousProcessTime_ = FileTimeValue(kernel) + FileTimeValue(user);
        previousTickMilliseconds_ = GetTickCount64();
    }

    PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &gpuQuery_);
    if (status == ERROR_SUCCESS) {
        // GPU Engine is an English performance-counter object on supported
        // Windows 10/11 builds. PdhAddEnglishCounter keeps this independent of
        // the display language while the instance filter limits values to this
        // process instead of reporting whole-system GPU utilization.
        status = PdhAddEnglishCounterW(
            gpuQuery_, L"\\GPU Engine(*)\\Utilization Percentage", 0,
            &gpuCounter_);
    }
    if (status == ERROR_SUCCESS) {
        PdhCollectQueryData(gpuQuery_);
    } else {
        if (gpuQuery_ != nullptr) {
            PdhCloseQuery(gpuQuery_);
        }
        gpuQuery_ = nullptr;
        gpuCounter_ = nullptr;
        core::LogWarning(
            L"Per-process GPU counters are unavailable; CPU and memory remain visible.");
    }
    return true;
}

ProcessResourceUsage ProcessResourceMonitor::Sample(
    const std::optional<std::uint64_t> videoMemoryBytes) {
    ProcessResourceUsage usage;
    usage.cpuPercent = SampleCpu();
    usage.videoMemoryBytes = videoMemoryBytes;

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             sizeof(memory))) {
        usage.workingSetBytes = memory.WorkingSetSize;
    }

    const std::optional<double> gpu = SampleGpu();
    if (gpu.has_value()) {
        usage.gpuPercent = *gpu;
        usage.gpuAvailable = true;
    }
    return usage;
}

void ProcessResourceMonitor::Shutdown() {
    if (gpuQuery_ != nullptr) {
        PdhCloseQuery(gpuQuery_);
    }
    gpuQuery_ = nullptr;
    gpuCounter_ = nullptr;
}

double ProcessResourceMonitor::SampleCpu() {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    const std::uint64_t processTime = FileTimeValue(kernel) + FileTimeValue(user);
    const ULONGLONG tickMilliseconds = GetTickCount64();
    const std::uint64_t processDelta = processTime - previousProcessTime_;
    const ULONGLONG tickDelta = tickMilliseconds - previousTickMilliseconds_;
    previousProcessTime_ = processTime;
    previousTickMilliseconds_ = tickMilliseconds;
    if (tickDelta == 0) {
        return 0.0;
    }
    const double availableHundredNanoseconds =
        static_cast<double>(tickDelta) * 10000.0 * processorCount_;
    return std::clamp(static_cast<double>(processDelta) * 100.0 /
                          availableHundredNanoseconds,
                      0.0, 100.0);
}

std::optional<double> ProcessResourceMonitor::SampleGpu() {
    if (gpuQuery_ == nullptr || gpuCounter_ == nullptr ||
        PdhCollectQueryData(gpuQuery_) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD bufferBytes = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(
        gpuCounter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufferBytes,
        &itemCount, nullptr);
    if (status != PDH_MORE_DATA || bufferBytes == 0) {
        return std::nullopt;
    }
    std::vector<std::byte> buffer(bufferBytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(
        gpuCounter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufferBytes,
        &itemCount, items);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const std::wstring processMarker =
        L"pid_" + std::to_wstring(processId_) + L"_";
    double busiestEngine = 0.0;
    for (DWORD index = 0; index < itemCount; ++index) {
        if (items[index].szName == nullptr ||
            items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) {
            continue;
        }
        if (Lowercase(items[index].szName).find(processMarker) !=
            std::wstring::npos) {
            // Task Manager reports GPU utilization using the busiest relevant
            // engine rather than summing 3D, copy and video-decode engines.
            // Summing made a 44% adapter load appear as roughly 98% here.
            busiestEngine = std::max(
                busiestEngine,
                std::max(0.0, items[index].FmtValue.doubleValue));
        }
    }
    return std::clamp(busiestEngine, 0.0, 100.0);
}

}  // namespace lwe::platform
