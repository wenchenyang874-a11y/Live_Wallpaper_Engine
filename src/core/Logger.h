#pragma once

#include <string>
#include <string_view>

#include <windows.h>

namespace lwe::core {

bool InitializeLogging();
void ShutdownLogging();

void LogInfo(std::wstring_view message);
void LogWarning(std::wstring_view message);
void LogError(std::wstring_view message, HRESULT result = S_OK);

std::wstring HResultMessage(HRESULT result);

}  // namespace lwe::core
