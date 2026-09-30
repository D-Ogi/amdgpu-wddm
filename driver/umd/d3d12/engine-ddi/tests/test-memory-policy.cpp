// SPDX-License-Identifier: MIT
// Memory architecture policy of GetCaps 1002 on the real engine: query_adapter_caps with the harness's create info
// (one QueryAdapterCaps batch, a PRIVATE VkInstance of its own), then set_memory_architecture_policy and build_caps.
// Without a policy 1002 is the engine's answer; each field overridden alone changes that field only; a policy whose
// resulting answer the specification calls contradictory is refused and the policy held before stays
// (INTEGRATION.md, "Memory architecture policy"). The engine's own answers are whatever this PC's GPU gives: the
// expectations are derived from them, not assumed.
#include "harness.h"
#include <cstring>

namespace harness {

namespace {
using engine_ddi::MemoryArchitecturePolicy;
using engine_ddi::PolicyBool;
using Mem = D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041;

MemoryArchitecturePolicy policy() {
    MemoryArchitecturePolicy p{};
    p.size = sizeof(p);
    return p;
}

HRESULT get_1002(const engine_ddi::AdapterCaps* caps, Mem& m) {
    UINT node = 0;
    std::memset(&m, 0xEE, sizeof(m));
    D3D12DDIARG_GETCAPS request{D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE, &node, &m, sizeof(m)};
    return engine_ddi::build_caps(caps, D3D12DDI_BUILD_VERSION_0092, &request);
}

bool same(const Mem& a, const Mem& b) { return !std::memcmp(&a, &b, sizeof(Mem)); }

// The two contradictions set_memory_architecture_policy refuses, on a resulting answer.
bool consistent(const Mem& m) {
    if (m.CacheCoherent && !m.UMA) return false;
    return !(m.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1 &&
             m.ResourceSerializationTier != D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2);
}

PolicyBool opposite(BOOL b) { return b ? PolicyBool::False : PolicyBool::True; }
} // namespace

void test_memory_policy(const Env& env, const BC250_VKD3D_DEVICE_CREATE_INFO& create) {
    engine_ddi::AdapterCaps* caps = nullptr;
    HRESULT hr = engine_ddi::query_adapter_caps(&env.funcs, &create, &caps);
    checkf(hr == S_OK && caps, "memory policy: query_adapter_caps on the engine (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (!caps) return;

    Mem base;
    hr = get_1002(caps, base);
    checkf(hr == S_OK && consistent(base),
           "memory policy: 1002 without a policy is free of the refused contradictions, so a policy can keep any "
           "field at Default (UMA %d, CacheCoherent %d, IOCoherent %d, tiers %d %d)",
           base.UMA, base.CacheCoherent, base.IOCoherent, static_cast<int>(base.HeapSerializationTier),
           static_cast<int>(base.ResourceSerializationTier));
    Mem m;
    const MemoryArchitecturePolicy all_default = policy();
    hr = engine_ddi::set_memory_architecture_policy(caps, &all_default);
    checkf(hr == S_OK && get_1002(caps, m) == S_OK && same(m, base),
           "memory policy: an all-Default policy leaves 1002 byte for byte as without a policy");

    // Each field alone, set to the other value than the engine's answer (a tier to its other end).
    Mem held = base;
    for (int field = 0; field < 5; ++field) {
        MemoryArchitecturePolicy p = policy();
        Mem expected = base;
        const char* what = "";
        switch (field) {
        case 0: p.uma = opposite(base.UMA); expected.UMA = !base.UMA; what = "UMA"; break;
        case 1:
            p.cache_coherent = opposite(base.CacheCoherent);
            expected.CacheCoherent = !base.CacheCoherent;
            what = "CacheCoherent";
            break;
        case 2:
            p.io_coherent = opposite(base.IOCoherent);
            expected.IOCoherent = !base.IOCoherent;
            what = "IOCoherent";
            break;
        case 3: {
            const uint32_t v = base.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0 ? 1u : 0u;
            p.heap_serialization_tier = {1, v};
            expected.HeapSerializationTier = static_cast<D3D12DDI_HEAP_SERIALIZATION_TIER_0041>(v);
            what = "HeapSerializationTier";
            break;
        }
        default: {
            const uint32_t v = base.ResourceSerializationTier == D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2 ? 0u : 2u;
            p.resource_serialization_tier = {1, v};
            expected.ResourceSerializationTier = static_cast<D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041>(v);
            what = "ResourceSerializationTier";
            break;
        }
        }
        const bool accept = consistent(expected);
        hr = engine_ddi::set_memory_architecture_policy(caps, &p);
        const HRESULT hr_get = get_1002(caps, m);
        if (accept) held = expected;
        checkf(hr == (accept ? S_OK : E_INVALIDARG) && hr_get == S_OK && same(m, held),
               "memory policy: %s alone %s (UMA %d, CacheCoherent %d, IOCoherent %d, tiers %d %d)", what,
               accept ? "changes that field only" : "gives a contradiction and is refused, the policy before stays",
               m.UMA, m.CacheCoherent, m.IOCoherent, static_cast<int>(m.HeapSerializationTier),
               static_cast<int>(m.ResourceSerializationTier));
    }

    // IOCoherent TRUE alone, whatever the engine's UMA answer: accepted unless the engine's tiers already contradict.
    MemoryArchitecturePolicy io = policy();
    io.io_coherent = PolicyBool::True;
    Mem expected = base;
    expected.IOCoherent = TRUE;
    hr = engine_ddi::set_memory_architecture_policy(caps, &io);
    checkf(hr == S_OK && get_1002(caps, m) == S_OK && same(m, expected),
           "memory policy: IOCoherent True alone is accepted with the engine's UMA %d and CacheCoherent %d", base.UMA,
           base.CacheCoherent);

    // A contradiction keeps the IOCoherent policy just set.
    MemoryArchitecturePolicy bad = policy();
    bad.uma = PolicyBool::False;
    bad.cache_coherent = PolicyBool::True;
    hr = engine_ddi::set_memory_architecture_policy(caps, &bad);
    checkf(hr == E_INVALIDARG && get_1002(caps, m) == S_OK && same(m, expected),
           "memory policy: CacheCoherent True with UMA False is refused, the IOCoherent policy stays (hr %08lx)",
           static_cast<unsigned long>(hr));
    engine_ddi::free_adapter_caps(caps);
}

} // namespace harness
