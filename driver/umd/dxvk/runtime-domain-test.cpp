// SPDX-License-Identifier: MIT
#include "runtime-domain.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

using bc250::umd::RuntimeDomain;
static void require(bool condition) { if (!condition) std::abort(); }
struct Witness { std::atomic<unsigned> calls{0}; };
static std::int32_t callback(void *user, std::uint32_t op, void *arg) {
    auto *witness = static_cast<Witness *>(user);
    require(op == 34 && arg == witness);
    ++witness->calls;
    return 103; // Preserve success/pending/error values verbatim.
}
int main() {
    static_assert(!std::is_copy_constructible_v<RuntimeDomain>);
    static_assert(!std::is_move_constructible_v<RuntimeDomain::Scope>);
    RuntimeDomain a, b;
    Witness witness;
    auto invoke = [&](const RuntimeDomain &d) {
        return d.dispatch(callback, &witness, 34, &witness);
    };
    require(invoke(a) == RuntimeDomain::wrong_domain);
    {
        RuntimeDomain::Scope outer(a);
        require(invoke(a) == 103);
        require(invoke(b) == RuntimeDomain::wrong_domain);
        std::thread worker([&] {
            require(invoke(a) == RuntimeDomain::wrong_domain);
            require(invoke(b) == RuntimeDomain::wrong_domain);
        });
        worker.join();
        {
            RuntimeDomain::Scope nested(b);
            require(invoke(a) == RuntimeDomain::wrong_domain);
            require(invoke(b) == 103);
            { RuntimeDomain::Scope reentrant(b); require(invoke(b) == 103); }
            require(b.entered());
        }
        require(a.entered() && !b.entered());
        try { RuntimeDomain::Scope nested(b); throw 7; }
        catch (int) { require(a.entered() && !b.entered()); }
        require(a.dispatch(nullptr, nullptr, 0, nullptr) == RuntimeDomain::wrong_domain);
    }
    require(!a.entered() && !b.entered());
    // Runtime entry may migrate threads: creation-thread affinity is incorrect.
    std::thread migrated([&] {
        require(invoke(a) == RuntimeDomain::wrong_domain);
        { RuntimeDomain::Scope entry(a); require(invoke(a) == 103); }
        require(invoke(a) == RuntimeDomain::wrong_domain);
    });
    migrated.join();
    require(witness.calls == 4);
    std::cout << "PASS runtime domain: device/thread isolation, nesting, unwind, migration\n";
}
