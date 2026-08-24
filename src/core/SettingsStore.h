#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace lwe::core {

enum class WallpaperSelectionKind {
    DynamicTest,
    StaticImage,
    AnimatedGif,
    Video,
};

struct WallpaperAssignmentSetting final {
    WallpaperSelectionKind wallpaperKind = WallpaperSelectionKind::StaticImage;
    std::wstring wallpaperPath;
    std::wstring displayTargets;
    bool spanAcrossDisplays = false;
};

struct AppSettings final {
    static constexpr std::uint32_t kCurrentSchemaVersion = 4;

    std::uint32_t schemaVersion = kCurrentSchemaVersion;
    std::vector<WallpaperAssignmentSetting> assignments;
    // These fields describe the current UI target selection, not an applied
    // wallpaper. Each applied wallpaper stores its own targets above.
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
