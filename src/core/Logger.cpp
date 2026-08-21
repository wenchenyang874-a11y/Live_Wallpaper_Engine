#include "core/Logger.h"

#include <filesystem>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

#include <shlobj.h>

namespace lwe::core {
namespace {

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

std::wstring Timestamp() {
    SYSTEMTIME value{};
    GetLocalTime(&value);

    std::wostringstream stream;
    stream << std::setfill(L'0') << std::setw(4) << value.wYear << L'-'
           << std::setw(2) << value.wMonth << L'-' << std::setw(2) << value.wDay
           << L' ' << std::setw(2) << value.wHour << L':' << std::setw(2)
           << value.wMinute << L':' << std::setw(2) << value.wSecond << L'.'
           << std::setw(3) << value.wMilliseconds;
    return stream.str();
}

void Write(std::wstring_view level, std::wstring_view message) {
    std::wostringstream stream;
    stream << L'[' << Timestamp() << L"] [" << level << L"] " << message << L'\n';
    const std::wstring line = stream.str();

    OutputDebugStringW(line.c_str());

    std::scoped_lock lock(g_logMutex);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }

    // std::wofstream uses the process locale and entered a permanent failed
    // state when a selected path contained Chinese characters. Convert every
    // complete line to UTF-8 and append it in one Win32 write instead.
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
                                             static_cast<int>(line.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
                            static_cast<int>(line.size()), utf8.data(), required, nullptr,
                            nullptr) != required) {
        return;
    }

    DWORD written = 0;
    WriteFile(g_logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(g_logFile);
}

std::filesystem::path LogDirectory() {
    PWSTR localAppData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (SUCCEEDED(result) && localAppData != nullptr) {
        std::filesystem::path path(localAppData);
        CoTaskMemFree(localAppData);
        return path / L"LiveWallpaperEngine" / L"logs";
    }

    wchar_t temporaryPath[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)), temporaryPath);
    if (length > 0 && length < std::size(temporaryPath)) {
        return std::filesystem::path(temporaryPath) / L"LiveWallpaperEngine" / L"logs";
    }

    return std::filesystem::current_path();
}

}  // namespace

bool InitializeLogging() {
    std::scoped_lock lock(g_logMutex);

    std::error_code error;
    const std::filesystem::path directory = LogDirectory();
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    const std::filesystem::path path = directory / L"LiveWallpaperEngine.log";
    g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    return true;
}

void ShutdownLogging() {
    std::scoped_lock lock(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_logFile);
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

void LogInfo(const std::wstring_view message) {
    Write(L"INFO", message);
}

void LogWarning(const std::wstring_view message) {
    Write(L"WARN", message);
}

void LogError(const std::wstring_view message, const HRESULT result) {
    if (result == S_OK) {
        Write(L"ERROR", message);
        return;
    }

    std::wostringstream stream;
    stream << message << L" HRESULT=0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result) << L" (" << HResultMessage(result) << L')';
    Write(L"ERROR", stream.str());
}

std::wstring HResultMessage(const HRESULT result) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(flags, nullptr, static_cast<DWORD>(result), 0,
                                       reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (count == 0 || buffer == nullptr) {
        return L"unknown error";
    }

    std::wstring message(buffer, count);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

}  // namespace lwe::core
