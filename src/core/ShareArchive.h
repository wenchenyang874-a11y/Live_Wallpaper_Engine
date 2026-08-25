#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace lwe::core {

struct ShareArchiveEntry final {
    std::filesystem::path sourcePath;
    std::wstring entryName;
};

HRESULT CreateShareArchive(std::span<const ShareArchiveEntry> entries,
                           std::wstring_view destinationPath);

HRESULT ExtractShareArchive(std::wstring_view archivePath,
                            const std::filesystem::path& outputDirectory,
                            std::vector<std::filesystem::path>& extractedFiles);

}  // namespace lwe::core
