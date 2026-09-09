#pragma once

#include <string>

#include <windows.h>

namespace lwe::platform {

[[nodiscard]] bool IsStartupRegistrationEnabled();
[[nodiscard]] HRESULT SetStartupRegistrationEnabled(bool enabled);
[[nodiscard]] std::wstring StartupRegistrationCommand();

}  // namespace lwe::platform
