#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string_view>
#include <windows.h>

namespace lwe::core {
// A worker-local context, scoped to one user operation. Core I/O can cooperate
// without holding UI pointers or adding callbacks to every package helper.
struct OperationProgress final {
    std::stop_token stop;
    std::function<void(int, std::wstring_view)> report;
    static inline thread_local OperationProgress* current = nullptr;
    static bool Cancelled() { return current && current->stop.stop_requested(); }
    static void Report(int percent, std::wstring_view text = {}) {
        if (current && current->report) current->report(std::clamp(percent, -1, 100), text);
    }
    static HRESULT Check() { return Cancelled() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK; }
};
struct OperationScope final {
    OperationProgress* previous;
    explicit OperationScope(OperationProgress& value) : previous(OperationProgress::current) {
        OperationProgress::current = &value;
    }
    ~OperationScope() { OperationProgress::current = previous; }
};
}
