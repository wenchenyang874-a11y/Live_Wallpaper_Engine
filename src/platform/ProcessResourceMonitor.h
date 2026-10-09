#pragma once

#include <cstdint>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <pdh.h>
#include <windows.h>

namespace lwe::platform {

struct ProcessResourceUsage final {
    double cpuPercent = 0.0;
    double gpuPercent = 0.0;
    std::uint64_t workingSetBytes = 0;
    std::uint64_t dedicatedGpuMemoryBytes = 0;
    std::uint64_t sharedGpuMemoryBytes = 0;
    bool gpuAvailable = false;
    bool gpuMemoryAvailable = false;
};

class ProcessResourceMonitor final {
public:
    ProcessResourceMonitor() = default;
    ~ProcessResourceMonitor();

    ProcessResourceMonitor(const ProcessResourceMonitor&) = delete;
    ProcessResourceMonitor& operator=(const ProcessResourceMonitor&) = delete;

    void Start();
    void Stop();
    std::optional<ProcessResourceUsage> Latest(bool visible);

private:
    bool Initialize();
    ProcessResourceUsage Sample();
    void Shutdown();
    double SampleCpu();
    std::optional<double> SampleGpu();
    std::optional<std::uint64_t> SampleGpuMemory(PDH_HCOUNTER counter) const;

    PDH_HQUERY gpuQuery_ = nullptr;
    PDH_HCOUNTER gpuCounter_ = nullptr;
    PDH_HCOUNTER dedicatedGpuMemoryCounter_ = nullptr;
    PDH_HCOUNTER sharedGpuMemoryCounter_ = nullptr;
    DWORD processId_ = 0;
    DWORD processorCount_ = 1;
    std::uint64_t previousProcessTime_ = 0;
    ULONGLONG previousTickMilliseconds_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<ProcessResourceUsage> latest_;
    bool visible_ = false;
    std::jthread worker_;
};

}  // namespace lwe::platform
