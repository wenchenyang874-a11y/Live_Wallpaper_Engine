#pragma once

#include <string_view>

#include <windows.h>

#include "media/MediaTypes.h"

namespace lwe::media {

// Inspects the actual WIC container or Media Foundation stream. A filename
// extension is never sufficient evidence that a file is playable.
HRESULT ProbeMediaFile(std::wstring_view path, MediaInfo& info);

}  // namespace lwe::media
