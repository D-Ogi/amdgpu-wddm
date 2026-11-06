// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>

namespace bc250::umd {

// BD-327: the pipeline workers of one engine device (engine ABI 1.7, BC250_DXVK_SHELL_SERVICES::WorkerScope).
// A worker thread binds itself to the gate of its device when it starts and unbinds itself before it ends. On a
// bound thread, host_dispatch admits a small set of driver-internal memory calls of hosted RADV (a new shader arena
// of a pipeline the worker creates) while the gate is open, and refuses every other call. A bound thread is not in
// the RuntimeDomain: each check that needs a DDI entry still refuses it.
//
// revoke() closes the gate. It waits until the admitted call in flight returns, and after it no call of a worker
// reaches the runtime. DeviceOwner::close revokes on each return: a failed close keeps the engine device and its
// workers alive after DestroyDevice returned, and the runtime's device handle is not valid then. reopen() is for
// DeviceOwner::initialize only, which runs when no worker of an earlier engine of the owner is alive (a failed close
// refuses a new initialize). Admitted calls are serialized: at most one worker of a device is in a runtime callback
// at a time.
//
// The gate is a member of RuntimeDevice and lives as long as the owner. bind and bound() compare addresses only,
// so a worker that unbinds after the owner went away does not touch freed memory.
class EngineWorkerGate final {
public:
    EngineWorkerGate()=default;
    EngineWorkerGate(const EngineWorkerGate &)=delete;
    EngineWorkerGate &operator=(const EngineWorkerGate &)=delete;

    // The calling thread becomes a worker of gate, or of no device with nullptr.
    static void bind(const EngineWorkerGate *gate) noexcept { current_=gate; }
    bool bound() const noexcept { return current_==this; }
    // True: one admitted call may go to the runtime, and the caller calls leave() when it returned.
    // False: the gate is closed, and the call must be refused.
    bool enter() noexcept {
        AcquireSRWLockExclusive(&lock_);
        if (!revoked_.load(std::memory_order_relaxed)) return true;
        ReleaseSRWLockExclusive(&lock_);
        return false;
    }
    void leave() noexcept { ReleaseSRWLockExclusive(&lock_); }
    void revoke() noexcept {
        AcquireSRWLockExclusive(&lock_);
        revoked_.store(true,std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock_);
    }
    void reopen() noexcept {
        AcquireSRWLockExclusive(&lock_);
        revoked_.store(false,std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lock_);
    }
    bool revoked() const noexcept { return revoked_.load(std::memory_order_relaxed); }

private:
    SRWLOCK lock_=SRWLOCK_INIT;
    std::atomic_bool revoked_{false};
    inline static thread_local const EngineWorkerGate *current_=nullptr;
};

}
