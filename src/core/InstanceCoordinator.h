#pragma once

#include <windows.h>

namespace lwe::core {

enum class InstanceStartResult {
    Primary,
    ExistingActivated,
    Failed,
};

class InstanceCoordinator final {
public:
    InstanceCoordinator() = default;
    ~InstanceCoordinator();

    InstanceCoordinator(const InstanceCoordinator&) = delete;
    InstanceCoordinator& operator=(const InstanceCoordinator&) = delete;

    InstanceStartResult Initialize();
    [[nodiscard]] HANDLE ActivationEvent() const noexcept;

private:
    HANDLE lifetimeMutex_ = nullptr;
    HANDLE activationEvent_ = nullptr;
};

}  // namespace lwe::core
