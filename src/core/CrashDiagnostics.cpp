#include "core/CrashDiagnostics.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <dbghelp.h>
#include <shlobj.h>

#include "core/Logger.h"

#ifndef LWE_VERSION_MAJOR
#define LWE_VERSION_MAJOR 0
#endif
#ifndef LWE_VERSION_MINOR
#define LWE_VERSION_MINOR 0
#endif
#ifndef LWE_VERSION_PATCH
#define LWE_VERSION_PATCH 0
#endif

namespace lwe::core {
namespace {

constexpr wchar_t kDiagnosticsDirectoryName[] = L"diagnostics";
constexpr wchar_t kCrashDirectoryName[] = L"crashes";
constexpr wchar_t kActiveSessionFileName[] = L"active-session.v1.json";
constexpr wchar_t kLastSessionFileName[] = L"last-session.v1.json";
constexpr wchar_t kTemporarySuffix[] = L".tmp";
constexpr std::size_t kMaximumSessionBytes = 64U * 1024U;
constexpr std::size_t kMaximumCrashDumps = 10;
constexpr LONG kSelfTestExceptionCode = static_cast<LONG>(0xE0424C57UL);

std::array<wchar_t, 32768> g_crashDirectory{};
std::array<wchar_t, 32768> g_activeSessionPath{};
std::array<wchar_t, 32768> g_lastSessionPath{};
std::array<wchar_t, 32> g_version{};
std::array<wchar_t, 32> g_startedAtUtc{};
DWORD g_processId = 0;
volatile LONG g_handlingCrash = 0;

struct StoredSession final {
    std::wstring status;
    std::wstring version;
    std::wstring startedAtUtc;
    std::wstring endedAtUtc;
    std::wstring dumpFile;
    DWORD processId = 0;
    int exitCode = 0;
    DWORD exceptionCode = 0;
};

std::wstring CurrentVersion() {
    return std::to_wstring(LWE_VERSION_MAJOR) + L"." +
           std::to_wstring(LWE_VERSION_MINOR) + L"." +
           std::to_wstring(LWE_VERSION_PATCH);
}

bool FormatUtcTimestamp(wchar_t* destination,
                        const std::size_t destinationSize) noexcept {
    if (destination == nullptr || destinationSize < 25) {
        return false;
    }
    SYSTEMTIME value{};
    GetSystemTime(&value);
    return swprintf_s(destination, destinationSize,
                      L"%04hu-%02hu-%02huT%02hu:%02hu:%02hu.%03huZ",
                      value.wYear, value.wMonth, value.wDay, value.wHour,
                      value.wMinute, value.wSecond, value.wMilliseconds) > 0;
}

bool FormatCrashFileTimestamp(wchar_t* destination,
                              const std::size_t destinationSize) noexcept {
    if (destination == nullptr || destinationSize < 20) {
        return false;
    }
    SYSTEMTIME value{};
    GetSystemTime(&value);
    return swprintf_s(destination, destinationSize,
                      L"%04hu%02hu%02huT%02hu%02hu%02hu%03huZ", value.wYear,
                      value.wMonth, value.wDay, value.wHour, value.wMinute,
                      value.wSecond, value.wMilliseconds) > 0;
}

std::wstring UtcTimestamp() {
    wchar_t timestamp[32]{};
    return FormatUtcTimestamp(timestamp, std::size(timestamp))
               ? std::wstring(timestamp)
               : std::wstring{};
}

bool CopyPath(const std::filesystem::path& path,
              std::array<wchar_t, 32768>& destination) {
    const std::wstring value = path.native();
    if (value.size() >= destination.size()) {
        return false;
    }
    return wcscpy_s(destination.data(), destination.size(), value.c_str()) == 0;
}

std::optional<std::string> WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), utf8.data(), required,
                            nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return utf8;
}

bool WriteUtf8File(const std::filesystem::path& path,
                   const std::wstring_view contents, const bool atomic) {
    const std::optional utf8 = WideToUtf8(contents);
    if (!utf8.has_value()) {
        return false;
    }

    const std::filesystem::path destination =
        atomic ? std::filesystem::path(path.native() + kTemporarySuffix) : path;
    const HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool success =
        utf8->size() <= MAXDWORD &&
        WriteFile(file, utf8->data(), static_cast<DWORD>(utf8->size()), &written,
                  nullptr) != FALSE &&
        written == static_cast<DWORD>(utf8->size()) &&
        FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!success) {
        DeleteFileW(destination.c_str());
        return false;
    }
    if (!atomic) {
        return true;
    }
    if (MoveFileExW(destination.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(destination.c_str());
        return false;
    }
    return true;
}

std::optional<std::string> ReadUtf8File(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaximumSessionBytes)) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool success =
        contents.empty() ||
        (ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read,
                  nullptr) != FALSE &&
         read == static_cast<DWORD>(contents.size()));
    CloseHandle(file);
    return success ? std::optional<std::string>(std::move(contents)) : std::nullopt;
}

std::optional<std::string> JsonString(const std::string_view json,
                                      const std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    std::size_t position = json.find(token);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = json.find(':', position + token.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = json.find('"', position + 1);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t end = json.find('"', position + 1);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(json.substr(position + 1, end - position - 1));
}

template <typename Integer>
std::optional<Integer> JsonInteger(const std::string_view json,
                                   const std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    std::size_t position = json.find(token);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    position = json.find(':', position + token.size());
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    ++position;
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }
    Integer value{};
    const char* begin = json.data() + position;
    const char* end = json.data() + json.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} ? std::optional<Integer>(value) : std::nullopt;
}

std::wstring Utf8AsciiToWide(const std::optional<std::string>& value) {
    if (!value.has_value()) {
        return {};
    }
    return std::wstring(value->begin(), value->end());
}

std::optional<StoredSession> LoadStoredSession(
    const std::filesystem::path& path) {
    const std::optional json = ReadUtf8File(path);
    if (!json.has_value()) {
        return std::nullopt;
    }
    const std::optional status = JsonString(*json, "status");
    if (!status.has_value()) {
        return std::nullopt;
    }
    StoredSession session;
    session.status = Utf8AsciiToWide(status);
    session.version = Utf8AsciiToWide(JsonString(*json, "version"));
    session.startedAtUtc = Utf8AsciiToWide(JsonString(*json, "startedAtUtc"));
    session.endedAtUtc = Utf8AsciiToWide(JsonString(*json, "endedAtUtc"));
    session.dumpFile = Utf8AsciiToWide(JsonString(*json, "dumpFile"));
    session.processId = JsonInteger<DWORD>(*json, "pid").value_or(0);
    session.exitCode = JsonInteger<int>(*json, "exitCode").value_or(0);
    if (const std::optional code = JsonString(*json, "exceptionCode");
        code.has_value() && code->starts_with("0x")) {
        std::uint32_t parsed = 0;
        const auto result = std::from_chars(code->data() + 2,
                                            code->data() + code->size(), parsed, 16);
        if (result.ec == std::errc{}) {
            session.exceptionCode = parsed;
        }
    }
    return session;
}

std::wstring BuildSessionJson(const StoredSession& session) {
    std::wostringstream json;
    json << L"{\r\n"
         << L"  \"schemaVersion\": 1,\r\n"
         << L"  \"status\": \"" << session.status << L"\",\r\n"
         << L"  \"version\": \"" << session.version << L"\",\r\n"
         << L"  \"pid\": " << session.processId << L",\r\n"
         << L"  \"startedAtUtc\": \"" << session.startedAtUtc << L"\"";
    if (!session.endedAtUtc.empty()) {
        json << L",\r\n  \"endedAtUtc\": \"" << session.endedAtUtc << L"\"";
    }
    if (session.status == L"clean") {
        json << L",\r\n  \"exitCode\": " << session.exitCode;
    }
    if (session.status == L"crashed") {
        wchar_t code[16]{};
        swprintf_s(code, L"0x%08lX", session.exceptionCode);
        json << L",\r\n  \"exceptionCode\": \"" << code << L"\""
             << L",\r\n  \"dumpFile\": \"" << session.dumpFile << L"\"";
    }
    json << L"\r\n}\r\n";
    return json.str();
}

PreviousSessionInfo PublicSession(const StoredSession& stored) {
    PreviousSessionInfo session;
    if (stored.status == L"clean") {
        session.status = PreviousExitStatus::Clean;
    } else if (stored.status == L"crashed") {
        session.status = PreviousExitStatus::Crashed;
    } else if (stored.status == L"unclean" || stored.status == L"running") {
        session.status = PreviousExitStatus::Unclean;
    }
    session.version = stored.version;
    session.startedAtUtc = stored.startedAtUtc;
    session.endedAtUtc = stored.endedAtUtc;
    session.dumpFile = stored.dumpFile;
    session.processId = stored.processId;
    session.exitCode = stored.exitCode;
    session.exceptionCode = stored.exceptionCode;
    return session;
}

std::filesystem::path DefaultApplicationDataRoot() {
    PWSTR localAppData = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    if (SUCCEEDED(result) && localAppData != nullptr) {
        std::filesystem::path root(localAppData);
        CoTaskMemFree(localAppData);
        return root / L"LiveWallpaperEngine";
    }
    CoTaskMemFree(localAppData);

    wchar_t temporaryPath[MAX_PATH]{};
    const DWORD length =
        GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)), temporaryPath);
    if (length > 0 && length < std::size(temporaryPath)) {
        return std::filesystem::path(temporaryPath) / L"LiveWallpaperEngine";
    }
    return std::filesystem::current_path();
}

void PruneCrashDumps(const std::filesystem::path& directory) {
    std::error_code error;
    std::vector<std::filesystem::directory_entry> dumps;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) &&
            iterator->path().extension() == L".dmp" &&
            iterator->path().filename().native().starts_with(
                L"LiveWallpaperEngine-v")) {
            dumps.push_back(*iterator);
        }
    }
    std::ranges::sort(dumps, [](const auto& left, const auto& right) {
        std::error_code leftError;
        std::error_code rightError;
        return left.last_write_time(leftError) > right.last_write_time(rightError);
    });
    // Reserve one slot for a possible crash in the session that is about to
    // start. The exception filter can then stay allocation-free after writing
    // the new dump while the directory still never exceeds the public limit.
    constexpr std::size_t kMaximumExistingDumps = kMaximumCrashDumps - 1;
    for (std::size_t index = kMaximumExistingDumps; index < dumps.size(); ++index) {
        std::filesystem::remove(dumps[index].path(), error);
        error.clear();
    }
}

bool WideAscii(const wchar_t* source, char* destination,
               const std::size_t destinationSize) noexcept {
    if (source == nullptr || destination == nullptr || destinationSize == 0) {
        return false;
    }
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, source, -1, destination, static_cast<int>(destinationSize),
        nullptr, nullptr);
    return converted > 0;
}

bool WriteCrashRecord(const DWORD exceptionCode,
                      const wchar_t* dumpFileName) noexcept {
    char version[32]{};
    char startedAt[32]{};
    char endedAt[32]{};
    char dumpFile[256]{};
    wchar_t ended[32]{};
    if (!FormatUtcTimestamp(ended, std::size(ended))) {
        return false;
    }
    if (!WideAscii(g_version.data(), version, std::size(version)) ||
        !WideAscii(g_startedAtUtc.data(), startedAt, std::size(startedAt)) ||
        !WideAscii(ended, endedAt, std::size(endedAt)) ||
        !WideAscii(dumpFileName, dumpFile, std::size(dumpFile))) {
        return false;
    }

    char json[2048]{};
    const int length = sprintf_s(
        json,
        "{\r\n  \"schemaVersion\": 1,\r\n  \"status\": \"crashed\",\r\n"
        "  \"version\": \"%s\",\r\n  \"pid\": %lu,\r\n"
        "  \"startedAtUtc\": \"%s\",\r\n  \"endedAtUtc\": \"%s\",\r\n"
        "  \"exceptionCode\": \"0x%08lX\",\r\n  \"dumpFile\": \"%s\"\r\n}\r\n",
        version, g_processId, startedAt, endedAt, exceptionCode, dumpFile);
    if (length <= 0 || static_cast<std::size_t>(length) >= std::size(json)) {
        return false;
    }

    const HANDLE file = CreateFileW(g_lastSessionPath.data(), GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool success =
        WriteFile(file, json, static_cast<DWORD>(length), &written, nullptr) !=
            FALSE &&
        written == static_cast<DWORD>(length);
    FlushFileBuffers(file);
    CloseHandle(file);
    return success;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exceptionPointers) noexcept {
    if (InterlockedCompareExchange(&g_handlingCrash, 1, 0) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const DWORD exceptionCode =
        exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
            ? exceptionPointers->ExceptionRecord->ExceptionCode
            : static_cast<DWORD>(kSelfTestExceptionCode);
    wchar_t timestamp[32]{};
    if (!FormatCrashFileTimestamp(timestamp, std::size(timestamp))) {
        timestamp[0] = L'u';
        timestamp[1] = L'n';
        timestamp[2] = L'k';
        timestamp[3] = L'n';
        timestamp[4] = L'o';
        timestamp[5] = L'w';
        timestamp[6] = L'n';
        timestamp[7] = L'\0';
    }
    wchar_t dumpFileName[256]{};
    swprintf_s(dumpFileName, L"LiveWallpaperEngine-v%ls-%ls-pid%lu.dmp",
               g_version.data(), timestamp, g_processId);
    wchar_t dumpPath[32768]{};
    swprintf_s(dumpPath, L"%ls\\%ls", g_crashDirectory.data(), dumpFileName);

    bool dumpWritten = false;
    const HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exceptionInformation{};
        exceptionInformation.ThreadId = GetCurrentThreadId();
        exceptionInformation.ExceptionPointers = exceptionPointers;
        exceptionInformation.ClientPointers = FALSE;
        constexpr MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        dumpWritten =
            MiniDumpWriteDump(GetCurrentProcess(), g_processId, dump, dumpType,
                              exceptionPointers != nullptr ? &exceptionInformation
                                                           : nullptr,
                              nullptr, nullptr) != FALSE;
        FlushFileBuffers(dump);
        CloseHandle(dump);
        if (!dumpWritten) {
            DeleteFileW(dumpPath);
        }
    }

    if (WriteCrashRecord(exceptionCode, dumpWritten ? dumpFileName : L"")) {
        DeleteFileW(g_activeSessionPath.data());
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

CrashDiagnostics::~CrashDiagnostics() {
    if (initialized_ && !completed_) {
        SetUnhandledExceptionFilter(previousFilter_);
    }
}

bool CrashDiagnostics::Initialize() {
    try {
        return InitializeAtRoot(DefaultApplicationDataRoot());
    } catch (...) {
        return false;
    }
}

bool CrashDiagnostics::InitializeForTesting(
    const std::filesystem::path& applicationDataRoot) {
    try {
        return InitializeAtRoot(applicationDataRoot);
    } catch (...) {
        return false;
    }
}

bool CrashDiagnostics::InitializeAtRoot(
    const std::filesystem::path& applicationDataRoot) {
    if (initialized_) {
        return true;
    }

    const std::filesystem::path diagnosticsDirectory =
        applicationDataRoot / kDiagnosticsDirectoryName;
    const std::filesystem::path crashDirectory =
        applicationDataRoot / kCrashDirectoryName;
    const std::filesystem::path activeSessionPath =
        diagnosticsDirectory / kActiveSessionFileName;
    const std::filesystem::path lastSessionPath =
        diagnosticsDirectory / kLastSessionFileName;

    std::error_code error;
    std::filesystem::create_directories(diagnosticsDirectory, error);
    if (error) {
        return false;
    }
    std::filesystem::create_directories(crashDirectory, error);
    const std::wstring crashDirectoryValue = crashDirectory.native();
    if (error || crashDirectoryValue.size() + 1 + 255 >=
                     g_crashDirectory.size() ||
        !CopyPath(crashDirectory, g_crashDirectory) ||
        !CopyPath(activeSessionPath, g_activeSessionPath) ||
        !CopyPath(lastSessionPath, g_lastSessionPath)) {
        return false;
    }

    const std::wstring version = CurrentVersion();
    const std::wstring startedAtUtc = UtcTimestamp();
    if (version.size() >= g_version.size() ||
        startedAtUtc.size() >= g_startedAtUtc.size()) {
        return false;
    }
    wcscpy_s(g_version.data(), g_version.size(), version.c_str());
    wcscpy_s(g_startedAtUtc.data(), g_startedAtUtc.size(), startedAtUtc.c_str());
    g_processId = GetCurrentProcessId();
    InterlockedExchange(&g_handlingCrash, 0);

    bool observedUncleanExit = false;
    if (std::filesystem::exists(activeSessionPath, error)) {
        const std::optional active = LoadStoredSession(activeSessionPath);
        StoredSession unclean;
        unclean.status = L"unclean";
        unclean.version = active.has_value() ? active->version : L"unknown";
        unclean.processId = active.has_value() ? active->processId : 0;
        unclean.startedAtUtc = active.has_value() ? active->startedAtUtc : L"unknown";
        unclean.endedAtUtc = startedAtUtc;
        previousSession_ = PublicSession(unclean);
        observedUncleanExit = true;
        if (WriteUtf8File(lastSessionPath, BuildSessionJson(unclean), true)) {
            DeleteFileW(activeSessionPath.c_str());
        }
    }
    error.clear();

    if (!observedUncleanExit) {
        if (const std::optional last = LoadStoredSession(lastSessionPath);
            last.has_value()) {
            previousSession_ = PublicSession(*last);
        }
    }

    StoredSession active;
    active.status = L"running";
    active.version = version;
    active.processId = g_processId;
    active.startedAtUtc = startedAtUtc;
    if (!WriteUtf8File(activeSessionPath, BuildSessionJson(active), true)) {
        return false;
    }

    PruneCrashDumps(crashDirectory);
    previousFilter_ = SetUnhandledExceptionFilter(&CrashFilter);
    initialized_ = true;
    return true;
}

void CrashDiagnostics::MarkCleanExit(const int exitCode) noexcept {
    if (!initialized_ || completed_) {
        return;
    }
    try {
        StoredSession clean;
        clean.status = L"clean";
        clean.version = g_version.data();
        clean.processId = g_processId;
        clean.startedAtUtc = g_startedAtUtc.data();
        clean.endedAtUtc = UtcTimestamp();
        clean.exitCode = exitCode;
        if (WriteUtf8File(g_lastSessionPath.data(), BuildSessionJson(clean), true)) {
            DeleteFileW(g_activeSessionPath.data());
        }
    } catch (...) {
        // Keep the active marker so the next launch reports an unclean exit.
    }
    SetUnhandledExceptionFilter(previousFilter_);
    completed_ = true;
}

const PreviousSessionInfo& CrashDiagnostics::PreviousSession() const noexcept {
    return previousSession_;
}

void LogPreviousSession(const PreviousSessionInfo& session) {
    const std::wstring identity =
        L" version=" + (session.version.empty() ? L"unknown" : session.version) +
        L", pid=" + std::to_wstring(session.processId) + L'.';
    switch (session.status) {
        case PreviousExitStatus::None:
            LogInfo(L"No previous application session record was found.");
            break;
        case PreviousExitStatus::Clean:
            LogInfo(L"Previous application session exited normally;" + identity);
            break;
        case PreviousExitStatus::Crashed:
            LogWarning(L"Previous application session crashed;" + identity +
                       (session.dumpFile.empty()
                            ? L" No crash dump was recorded."
                            : L" Crash dump: " + session.dumpFile));
            break;
        case PreviousExitStatus::Unclean:
            LogWarning(L"Previous application session did not record a normal exit;" +
                       identity +
                       L" It may have been forcibly terminated or lost power.");
            break;
    }
}

}  // namespace lwe::core
