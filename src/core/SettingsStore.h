#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <windows.h>

namespace lwe::core {

enum class WallpaperSelectionKind {
    DynamicTest,
    StaticImage,
    AnimatedGif,
    Video,
};

struct AppSettings final {
    static constexpr std::uint32_t kCurrentSchemaVersion = 3;

    std::uint32_t schemaVersion = kCurrentSchemaVersion;
    WallpaperSelectionKind wallpaperKind = WallpaperSelectionKind::DynamicTest;
    std::wstring wallpaperPath;
    std::wstring displayTargets;
    bool soundEnabled = false;
    bool spanAcrossDisplays = true;
};

class SettingsStore final {
public:
    std::optional<AppSettings> Load() const;
    HRESULT Save(const AppSettings& settings) const;
};

}  // namespace lwe::core
