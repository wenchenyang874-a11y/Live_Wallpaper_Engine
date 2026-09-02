#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>

#include <windows.h>

namespace lwe::media::video {

struct VideoOptimizationPlan final {
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 1;
    std::uint32_t outputBitrate = 0;
    bool hasAudio = false;
    bool needed = false;
};

// Creates an opt-in, display-sized H.264/MP4 playback copy while leaving the
// library original untouched. Source frame rate is preserved; neither output
// dimension is allowed to undershoot the fill size required by the display.
HRESULT PlanVideoOptimization(std::wstring_view sourcePath,
                              std::uint32_t maximumDisplayWidth,
                              std::uint32_t maximumDisplayHeight,
                              VideoOptimizationPlan& plan);
HRESULT OptimizeVideo(std::wstring_view sourcePath,
                      std::wstring_view outputPath,
                      const VideoOptimizationPlan& plan,
                      std::stop_token stopToken = {});

}  // namespace lwe::media::video
