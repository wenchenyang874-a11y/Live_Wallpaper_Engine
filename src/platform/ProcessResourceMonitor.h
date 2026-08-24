#pragma once

#include <cstdint>
#include <optional>

#include <pdh.h>
#include <windows.h>

namespace lwe::platform {

struct ProcessResourceUsage final {
    double cpuPercent = 0.0;
    double gpuPercent = 0.0;
    std::uint64_t workingSetBytes = 0;
    std::optional<std::uint64_t> videoMemoryBytes;
    bool gpuAvailable = false;
};

class ProcessResourceMonitor final {
public:
    ProcessResourceMonitor() = default;
    ~ProcessResourceMonitor();

    ProcessResourceMonitor(const ProcessResourceMonitor&) = delete;
    ProcessResourceMonitor& operator=(const ProcessResourceMonitor&) = delete;

    bool Initialize();
    ProcessResourceUsage Sample(
        std::optional<std::uint64_t> videoMemoryBytes);

private:
    void Shutdown();
    double SampleCpu();
    std::optional<double> SampleGpu();

    PDH_HQUERY gpuQuery_ = nullptr;
    PDH_HCOUNTER gpuCounter_ = nullptr;
    DWORD processId_ = 0;
    DWORD processorCount_ = 1;
    std::uint64_t previousProcessTime_ = 0;
    ULONGLONG previousTickMilliseconds_ = 0;
};

}  // namespace lwe::platform
