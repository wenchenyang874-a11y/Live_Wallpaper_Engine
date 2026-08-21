#include "core/InstanceCoordinator.h"

#include "core/Logger.h"

namespace lwe::core {
namespace {

constexpr wchar_t kLifetimeMutexName[] =
    L"Local\\LiveWallpaperEngine.5D5D0B29-9EEF-4AF1-BC19-DA42BB84E610";
constexpr wchar_t kActivationEventName[] =
    L"Local\\LiveWallpaperEngine.Activate.5D5D0B29-9EEF-4AF1-BC19-DA42BB84E610";

}  // namespace

InstanceCoordinator::~InstanceCoordinator() {
    if (activationEvent_ != nullptr) {
        CloseHandle(activationEvent_);
    }
    if (lifetimeMutex_ != nullptr) {
        CloseHandle(lifetimeMutex_);
    }
}

InstanceStartResult InstanceCoordinator::Initialize() {
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kLifetimeMutexName);
    if (mutex == nullptr) {
        LogError(L"Unable to create the single-instance mutex.",
                 HRESULT_FROM_WIN32(GetLastError()));
        return InstanceStartResult::Failed;
    }

    const bool instanceAlreadyExists = GetLastError() == ERROR_ALREADY_EXISTS;
    if (instanceAlreadyExists) {
        // The first process briefly owns the mutex while creating the activation
        // event. Waiting here closes the tiny startup race between two rapid clicks.
        const DWORD waitResult = WaitForSingleObject(mutex, 2000);
        const bool ownsMutex =
            waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
        if (!ownsMutex) {
            CloseHandle(mutex);
            LogWarning(L"The running instance did not finish initialization in time.");
            return InstanceStartResult::Failed;
        }

        HANDLE activationEvent =
            OpenEventW(EVENT_MODIFY_STATE, FALSE, kActivationEventName);
        bool activated = false;
        DWORD activationError = ERROR_SUCCESS;
        if (activationEvent == nullptr) {
            activationError = GetLastError();
        } else if (!SetEvent(activationEvent)) {
            activationError = GetLastError();
        } else {
            activated = true;
        }
        if (activationEvent != nullptr) {
            CloseHandle(activationEvent);
        }
        ReleaseMutex(mutex);
        CloseHandle(mutex);

        if (!activated) {
            LogError(L"Unable to activate the running instance.",
                     HRESULT_FROM_WIN32(activationError));
            return InstanceStartResult::Failed;
        }

        LogInfo(L"A running instance was activated instead of starting another renderer.");
        return InstanceStartResult::ExistingActivated;
    }

    activationEvent_ = CreateEventW(nullptr, FALSE, FALSE, kActivationEventName);
    if (activationEvent_ == nullptr) {
        const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        LogError(L"Unable to create the existing-instance activation event.", error);
        return InstanceStartResult::Failed;
    }

    lifetimeMutex_ = mutex;
    ReleaseMutex(lifetimeMutex_);
    LogInfo(L"Primary application instance acquired.");
    return InstanceStartResult::Primary;
}

HANDLE InstanceCoordinator::ActivationEvent() const noexcept {
    return activationEvent_;
}

}  // namespace lwe::core
