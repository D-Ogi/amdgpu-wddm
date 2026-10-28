// SPDX-License-Identifier: MIT
// Atomics with explicit scope and memory semantics, emitted as exact SPIR-V instructions with DXC's inline SPIR-V
// attributes. Edge case from Erfan Ahmadi's "Inline SPIR-V" (https://erfan-ahmadi.github.io/blog/inline-spirv/):
// OpAtomicStore with Release semantics, here at Device scope and with its OpAtomicLoad Acquire partner.
//
// Two routes, one source: without ROUTE_BDA every access goes through the storage buffer of binding 0 (an
// OpAccessChain into the StorageBuffer storage class); with ROUTE_BDA every access goes through a
// PhysicalStorageBuffer pointer made from the buffer device address in pc.a0. Indices are in 32-bit words.
//
// exact_main   results that only the atomics decide (vkshaderedge.c, CheckAtomicExact):
//              per thread t, a Release store of 3t+1 and an Acquire load of it back; one AcquireRelease IAdd on a
//              shared counter (the returned values are a permutation of 0..n-1); one compare-exchange per
//              workgroup slot, equal semantics AcquireRelease and unequal semantics Acquire, which exactly one
//              thread of the workgroup wins.
// mp_main      message passing between workgroups: thread t writes four payload words with plain stores, then
//              sets its flag with a Release store; it then waits, at most pc.spin Acquire loads, for the flag of a
//              thread half the grid away and checks that thread's payload with plain loads. A seen flag with a
//              wrong payload is a broken release/acquire pair: the producer's stores were not available, or the
//              consumer's loads read stale cache lines. The wait has a bound, so the dispatch always ends.
//
// pc.neg = 1 is the negative control: exact_main stores 3t+2, and mp_main checks the payload of the wrong thread.
// Both must then fail on every driver.
#include "inline_spirv.hlsli"

#ifdef ROUTE_BDA
uint64_t WordAddress(uint i) { return pc.a0 + 4ull * (uint64_t)i; }
uint ALoad(uint i, uint sem) { return spvAtomicLoadPtr(spvUintPointer(WordAddress(i)), kScopeDevice, sem); }
void AStore(uint i, uint sem, uint v) { spvAtomicStorePtr(spvUintPointer(WordAddress(i)), kScopeDevice, sem, v); }
uint AAdd(uint i, uint sem, uint v) { return spvAtomicIAddPtr(spvUintPointer(WordAddress(i)), kScopeDevice, sem, v); }
uint ACas(uint i, uint eq, uint ne, uint v, uint cmp)
{
    return spvAtomicCompareExchangePtr(spvUintPointer(WordAddress(i)), kScopeDevice, eq, ne, v, cmp);
}
uint PLoad(uint i) { return vk::RawBufferLoad<uint>(WordAddress(i), 4); }
void PStore(uint i, uint v) { vk::RawBufferStore<uint>(WordAddress(i), v, 4); }
#else
RWStructuredBuffer<uint> mem : register(u0);
uint ALoad(uint i, uint sem) { return spvAtomicLoad(mem[i], kScopeDevice, sem); }
void AStore(uint i, uint sem, uint v) { spvAtomicStore(mem[i], kScopeDevice, sem, v); }
uint AAdd(uint i, uint sem, uint v) { return spvAtomicIAdd(mem[i], kScopeDevice, sem, v); }
uint ACas(uint i, uint eq, uint ne, uint v, uint cmp) { return spvAtomicCompareExchange(mem[i], kScopeDevice, eq, ne, v, cmp); }
uint PLoad(uint i) { return mem[i]; }
void PStore(uint i, uint v) { mem[i] = v; }
#endif

// Word layout of exact_main for n threads; vkshaderedge.c uses the same formulas.
//   [0, n)            slots, one per thread
//   n                 the shared counter
//   [n+1, n+1+n/64)   compare-exchange slots, one per workgroup
//   then 3n words     results: loaded value, counter value before the add, compare-exchange original
[numthreads(64, 1, 1)]
void exact_main(uint t : SV_DispatchThreadID)
{
    const uint n = pc.n;
    const uint counter = n, cas = n + 1, res = n + 1 + n / 64;
    AStore(t, kReleaseUniform, 3 * t + 1 + pc.neg);
    const uint loaded = ALoad(t, kAcquireUniform);
    const uint before = AAdd(counter, kAcqRelUniform, 1);
    const uint original = ACas(cas + t / 64, kAcqRelUniform, kAcquireUniform, t + 1, 0);
    PStore(res + 3 * t + 0, loaded);
    PStore(res + 3 * t + 1, before);
    PStore(res + 3 * t + 2, original);
}

// Payload word j of thread t: never zero (the cleared buffer), different for every thread.
uint Payload(uint t, uint j) { return ((t * 2654435761u) ^ (j * 0x9E3779B9u + 0x01234567u)) | 1u; }

// Word layout of mp_main for n threads:
//   [0, 4n)    payload, four words per thread
//   [4n, 5n)   flags
//   [5n, 6n)   results: bit 0 the partner's flag was seen, bits 8-15 the number of wrong payload words
[numthreads(64, 1, 1)]
void mp_main(uint t : SV_DispatchThreadID)
{
    const uint n = pc.n;
    const uint flags = 4 * n, res = 5 * n;
    for (uint j = 0; j < 4; ++j)
        PStore(4 * t + j, Payload(t, j));
    AStore(flags + t, kReleaseUniform, 1);

    // n / 2 is a multiple of 64, so the partner is always in another workgroup.
    const uint partner = (t + n / 2) % n;
    uint seen = 0;
    for (uint i = 0; i < pc.spin; ++i) {
        if (ALoad(flags + partner, kAcquireUniform) != 0) {
            seen = 1;
            break;
        }
    }
    uint wrong = 0;
    if (seen) {
        const uint read = pc.neg ? (partner + 1) % n : partner;
        for (uint j = 0; j < 4; ++j)
            wrong += PLoad(4 * read + j) != Payload(partner, j) ? 1u : 0u;
    }
    PStore(res + t, seen | (wrong << 8));
}
