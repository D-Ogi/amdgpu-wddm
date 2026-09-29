// SPDX-License-Identifier: MIT
// Host-only composition test against the actual native engine-ddi library.
// No engine, runtime device, Vulkan loader or GPU is opened.
#include <windows.h>
#include "device-table.h"
#include "queue-ddi.h"
#include "fence-ddi.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <type_traits>

namespace {
engine_ddi::DeviceContext* APIENTRY resolve(D3D12DDI_HDEVICE) { return nullptr; }
engine_ddi::DeviceContext* APIENTRY other_resolve(D3D12DDI_HDEVICE) { return nullptr; }
void APIENTRY present(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDQUEUE,
    const D3D12DDIARG_PRESENT_0001*, D3D12DDI_PRESENT_0051*, D3D12DDI_PRESENT_CONTEXTS_0051*, D3D12DDI_PRESENT_HWQUEUES_0051*) {}

// Test-only implementations of the 12 shell slots that have no production DDI
// wrapper yet. They are never invoked, and never linked into the driver.
D3D12DDI_DEVICE_FUNCS_CORE_0088 shell_entries() {
    D3D12DDI_DEVICE_FUNCS_CORE_0088 t{};
    native12::install_queue_entries(t);
    native12::install_fence_entries(t);
    t.pfnMakeResident = [](D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001*) -> HRESULT { return E_NOTIMPL; };
    t.pfnEvict = [](D3D12DDI_HDEVICE, const D3D12DDIARG_EVICT*) -> HRESULT { return E_NOTIMPL; };
    t.pfnOfferResources = [](D3D12DDI_HDEVICE, const D3D12DDIARG_OFFERRESOURCES*) -> HRESULT { return E_NOTIMPL; };
    t.pfnReclaimResources = [](D3D12DDI_HDEVICE, D3D12DDIARG_RECLAIMRESOURCES_0001*) -> HRESULT { return E_NOTIMPL; };
    t.pfnGetImplicitPhysicalAdapterMask = [](D3D12DDI_HDEVICE) -> UINT { return 0; };
    t.pfnGetPresentPrivateDriverDataSize = [](D3D12DDI_HDEVICE, const D3D12DDIARG_PRESENT_0001*) -> UINT { return 0; };
    t.pfnQueryNodeMap = [](D3D12DDI_HDEVICE, UINT, UINT*) {};
    t.pfnGetDebugAllocationInfo = [](D3D12DDI_HDEVICE, D3D12DDI_HANDLE_AND_TYPE, UINT*,
        D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012*, UINT*, D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014*) {};
    t.pfnCalcPrivateSchedulingGroupSize = [](D3D12DDI_HDEVICE, const D3D12DDIARG_CREATESCHEDULINGGROUP_0050*) -> SIZE_T { return 0; };
    t.pfnCreateSchedulingGroup = [](D3D12DDI_HDEVICE, const D3D12DDIARG_CREATESCHEDULINGGROUP_0050*,
        D3D12DDI_HSCHEDULINGGROUP_0050, D3D12DDI_HRTSCHEDULINGGROUP_0050) -> HRESULT { return E_NOTIMPL; };
    t.pfnDestroySchedulingGroup = [](D3D12DDI_HDEVICE, D3D12DDI_HSCHEDULINGGROUP_0050) {};
    t.pfnSetBackgroundProcessingMode = [](D3D12DDI_HDEVICE, D3D12DDI_BACKGROUND_PROCESSING_MODE_0062,
        D3D12DDI_MEASUREMENTS_ACTION_0062, BOOL*) {};
    return t;
}
template<class T> bool equal(const T& a, const T& b) {
    static_assert(std::is_trivially_copyable_v<T>);
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
// x64 Windows ABI gate only: inspect pointer representations, never call through
// integer storage. Each slot remains assigned and invoked through its WDK type.
template<class T> bool all_nonzero(const T& table) {
    static_assert(sizeof(void*) == sizeof(uintptr_t));
    static_assert(sizeof(T) % sizeof(uintptr_t) == 0);
    uintptr_t words[sizeof(T) / sizeof(uintptr_t)]{};
    std::memcpy(words, &table, sizeof(table));
    for (auto word : words) if (!word) return false;
    return true;
}
} // namespace

int main() {
    const engine_ddi::FillInfo info{sizeof(info), resolve};
    const auto shell = shell_entries();
    auto core = shell;
    const auto before = core;
    using native12::compose_core_0092;
    using native12::compose_list_0092;
    assert(compose_core_0092(nullptr, sizeof(core), shell, info) == E_INVALIDARG);
    assert(compose_core_0092(&core, sizeof(core) - 8, shell, info) == E_INVALIDARG && equal(core, before));
    auto bad_info = info;
    bad_info.resolve = nullptr;
    assert(compose_core_0092(&core, sizeof(core), shell, bad_info) == E_INVALIDARG && equal(core, before));
    bad_info = info;
    --bad_info.size;
    assert(compose_core_0092(&core, sizeof(core), shell, bad_info) == E_INVALIDARG && equal(core, before));
    unsigned missing_checked = 0;
    std::apply([&](auto... member) {
        const auto check_missing = [&](auto field) {
            auto missing = shell;
            missing.*field = nullptr;
            assert(compose_core_0092(&core, sizeof(core), missing, info) == E_NOTIMPL);
            assert(equal(core, before));
            ++missing_checked;
        };
        (check_missing(member), ...);
    }, native12::table_detail::shell_core_members);
    assert(missing_checked == 18);
    assert(compose_core_0092(&core, sizeof(core), shell, info) == S_OK);
    assert(all_nonzero(core));
    std::apply([&](auto... member) { assert(((core.*member == shell.*member) && ...)); },
        native12::table_detail::shell_core_members);
    assert(core.pfnCalcPrivateCommandQueueSize == native12::queue_size);
    assert(core.pfnCreateFence == native12::fence_create);
    D3D12DDIARG_CREATE_FENCE invalid_fence{};
    assert(core.pfnCalcPrivateFenceSize({}, &invalid_fence) == sizeof(native12::FenceState));
    // Alias-safe: all input reads finish before the output commit.
    auto alias = shell;
    assert(compose_core_0092(&alias, sizeof(alias), alias, info) == S_OK && equal(alias, core));
    // A second process-wide resolver is rejected by the real engine library.
    const auto completed = core;
    bad_info = info;
    bad_info.resolve = other_resolve;
    assert(FAILED(compose_core_0092(&core, sizeof(core), shell, bad_info)) && equal(core, completed));

    for (UINT number = 0; number < 2; ++number) {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 list{};
        list.pfnPresent = present;
        const auto unchanged = list;
        assert(compose_list_0092(nullptr, sizeof(list), number, present, info) == E_INVALIDARG);
        assert(compose_list_0092(&list, sizeof(list) - 8, number, present, info) == E_INVALIDARG && equal(list, unchanged));
        assert(compose_list_0092(&list, sizeof(list), 2, present, info) == E_INVALIDARG && equal(list, unchanged));
        assert(compose_list_0092(&list, sizeof(list), number, nullptr, info) == E_NOTIMPL && equal(list, unchanged));
        assert(FAILED(compose_list_0092(&list, sizeof(list), number, present, bad_info)) && equal(list, unchanged));
        assert(compose_list_0092(&list, sizeof(list), number, present, info) == S_OK);
        assert(all_nonzero(list) && list.pfnPresent == present);
    }
    std::puts("PASSED: core122/list70x2 composition,18 missing shell entries refused, output preserved on failure; no runtime/GPU");
}
