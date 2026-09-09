#include "platform/StartupRegistration.h"

#include <algorithm>
#include <vector>

namespace lwe::platform {
namespace {

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"LiveWallpaperEngine";

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

}  // namespace

std::wstring StartupRegistrationCommand() {
    const std::wstring executable = ExecutablePath();
    if (executable.empty()) {
        return {};
    }
    return L"\"" + executable + L"\" --startup";
}

bool IsStartupRegistrationEnabled() {
    DWORD type = 0;
    DWORD size = 0;
    const LSTATUS result = RegGetValueW(
        HKEY_CURRENT_USER, kRunKey, kRunValueName, RRF_RT_REG_SZ, &type,
        nullptr, &size);
    return result == ERROR_SUCCESS && type == REG_SZ && size > sizeof(wchar_t);
}

HRESULT SetStartupRegistrationEnabled(const bool enabled) {
    if (!enabled) {
        HKEY key = nullptr;
        const LSTATUS opened = RegOpenKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
        if (opened == ERROR_FILE_NOT_FOUND) {
            return S_OK;
        }
        if (opened != ERROR_SUCCESS) {
            return HRESULT_FROM_WIN32(opened);
        }
        const LSTATUS removed = RegDeleteValueW(key, kRunValueName);
        RegCloseKey(key);
        return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND
                   ? S_OK
                   : HRESULT_FROM_WIN32(removed);
    }

    const std::wstring command = StartupRegistrationCommand();
    if (command.empty()) {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_BAD_PATHNAME
                                                         : error);
    }
    HKEY key = nullptr;
    const LSTATUS created = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(created);
    }
    const DWORD bytes = static_cast<DWORD>(
        (command.size() + 1) * sizeof(wchar_t));
    const LSTATUS written = RegSetValueExW(
        key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    RegCloseKey(key);
    return written == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(written);
}

}  // namespace lwe::platform
