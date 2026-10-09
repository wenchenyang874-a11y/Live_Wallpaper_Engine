#pragma once
#include <optional>
#include <vector>
#include <windows.h>
#include "core/WallpaperOptions.h"

namespace lwe::app {
struct WallpaperOptionsResult { core::WallpaperOptions options; bool solo = false; };
std::optional<WallpaperOptionsResult> EditWallpaperOptions(
    HWND owner, const std::wstring& name, const core::WallpaperOptions& options, bool hasAudio);
// IDs follow the application screen selector, not enumeration-dependent Windows labels.
void IdentifyScreens(const std::vector<std::wstring>& deviceIds);
void ShowDiagnostics(HWND owner, int action); // 0: logs, 1: previous session, 2: export
}
