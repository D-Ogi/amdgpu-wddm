// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace bc250::umd {

// A runtime entry grants authority only to this device on the entering thread.
// This is not a lock and does not by itself grant engine workers permission to
// call DDI callbacks. The one worker that enters a domain is the D3D12 shell's
// deferred-replay worker, which enters it together with
// HostedDispatch::WorkerScope; that scope narrows what the worker may call.
// The runtime owns serialization and device lifetime. Keep this
// object alive until engine workers have stopped and all scopes have returned.
// Instantiate and use the guard in the UMD module, not independently in two DLLs.
class RuntimeDomain final {
public:
    using Dispatch = std::int32_t (*)(void *, std::uint32_t, void *);
    static constexpr std::int32_t wrong_domain =
        static_cast<std::int32_t>(0xc000000du); // STATUS_INVALID_PARAMETER

    RuntimeDomain() = default;
    RuntimeDomain(const RuntimeDomain &) = delete;
    RuntimeDomain &operator=(const RuntimeDomain &) = delete;
    RuntimeDomain(RuntimeDomain &&) = delete;
    RuntimeDomain &operator=(RuntimeDomain &&) = delete;

    class Scope final {
    public:
        explicit Scope(const RuntimeDomain &domain) noexcept
            : previous_(current_) { current_ = &domain; }
        ~Scope() { current_ = previous_; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        Scope(Scope &&) = delete;
        Scope &operator=(Scope &&) = delete;
    private:
        const RuntimeDomain *previous_;
    };

    bool entered() const noexcept { return current_ == this; }

    std::int32_t dispatch(Dispatch callback, void *userdata,
                          std::uint32_t operation, void *argument) const {
        if (!entered() || !callback)
            return wrong_domain;
        return callback(userdata, operation, argument);
    }

private:
    inline static thread_local const RuntimeDomain *current_ = nullptr;
};

} // namespace bc250::umd
