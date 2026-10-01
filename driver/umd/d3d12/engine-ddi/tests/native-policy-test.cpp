// SPDX-License-Identifier: MIT
// Linked against the native engine-ddi.lib (no harness macro), the way the shell's DLL links it. Checks what the
// native build must refuse, and that both tables come out with every slot filled. No engine is loaded: the refusals
// happen before engine-ddi touches the engine device, which is a dummy pointer here.
#include "engine-ddi.h"
#include "internal.h"
#include <cstdio>
#include <cstring>
#include <io.h>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

void APIENTRY device_error(void*, HRESULT) {}
void APIENTRY list_error(void*, D3D12DDI_HRTCOMMANDLIST, HRESULT) {}
BOOL APIENTRY device_lost(void*) { return FALSE; }
HRESULT APIENTRY bind_table(void*, D3D12DDI_HRTCOMMANDLIST, uint32_t) { return S_OK; }
HRESULT APIENTRY create_queue(void*, const BC250_VKD3D_COMMAND_QUEUE_DESC*, void*, REFIID, void**) { return E_FAIL; }
engine_ddi::DeviceContext* APIENTRY resolve(D3D12DDI_HDEVICE) { return nullptr; }

template <class T> bool all_filled(const T& table) {
    void* slots[sizeof(T) / sizeof(void*)];
    std::memcpy(slots, &table, sizeof(T));
    for (void* s : slots)
        if (!s) return false;
    return true;
}
} // namespace

int main() {
    // The refusals below log through engine-ddi's stderr sink by design: expected output, sent to stdout in order.
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));
    BC250_VKD3D_ENGINE_FUNCS funcs{};
    funcs.Size = sizeof(funcs);
    funcs.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    funcs.CreateCommandQueue = create_queue;
    engine_ddi::ContextCreateInfo info{};
    info.size = sizeof(info);
    info.boundary_revision = engine_ddi::kBoundaryRevision;
    info.memory_mode = engine_ddi::MemoryMode::EnginePrivateTest;
    info.engine_device = reinterpret_cast<ID3D12Device*>(uintptr_t{0x1000});   // never dereferenced
    info.engine_funcs = &funcs;
    info.hooks.size = sizeof(info.hooks);
    info.hooks.report_device_error = device_error;
    info.hooks.report_list_error = list_error;
    info.hooks.is_device_lost = device_lost;
    info.hooks.bind_list_table = bind_table;
    engine_ddi::DeviceContext* context = reinterpret_cast<engine_ddi::DeviceContext*>(uintptr_t{1});
    check(engine_ddi::create_device_context(&info, &context) == E_INVALIDARG && !context,
          "native build refuses MemoryMode::EnginePrivateTest");

    engine_ddi::FillInfo fill{sizeof(fill), resolve};
    D3D12DDI_DEVICE_FUNCS_CORE_0088 core{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 lists[2]{};
    check(engine_ddi::fill_device_core(&core, sizeof(core) - 8, &fill) == E_INVALIDARG, "core table: wrong size refused");
    check(engine_ddi::fill_command_list(&lists[0], sizeof(lists[0]), 2, &fill) == E_INVALIDARG,
          "list table: index 2 refused");
    check(engine_ddi::fill_device_core(&core, sizeof(core), &fill) == S_OK, "core table filled");
    check(engine_ddi::fill_command_list(&lists[0], sizeof(lists[0]), 0, &fill) == S_OK &&
              engine_ddi::fill_command_list(&lists[1], sizeof(lists[1]), 1, &fill) == S_OK,
          "compute and graphics list tables filled");
    // The shell fills its own slots over these; engine-ddi leaves them null.
    D3D12DDI_DEVICE_FUNCS_CORE_0088 shell_view = core;
    shell_view.pfnCalcPrivateCommandQueueSize = reinterpret_cast<decltype(shell_view.pfnCalcPrivateCommandQueueSize)>(1);
    shell_view.pfnCreateCommandQueue = reinterpret_cast<decltype(shell_view.pfnCreateCommandQueue)>(1);
    shell_view.pfnDestroyCommandQueue = reinterpret_cast<decltype(shell_view.pfnDestroyCommandQueue)>(1);
    shell_view.pfnCalcPrivateFenceSize = reinterpret_cast<decltype(shell_view.pfnCalcPrivateFenceSize)>(1);
    shell_view.pfnCreateFence = reinterpret_cast<decltype(shell_view.pfnCreateFence)>(1);
    shell_view.pfnDestroyFence = reinterpret_cast<decltype(shell_view.pfnDestroyFence)>(1);
    shell_view.pfnMakeResident = reinterpret_cast<decltype(shell_view.pfnMakeResident)>(1);
    shell_view.pfnEvict = reinterpret_cast<decltype(shell_view.pfnEvict)>(1);
    shell_view.pfnOfferResources = reinterpret_cast<decltype(shell_view.pfnOfferResources)>(1);
    shell_view.pfnReclaimResources = reinterpret_cast<decltype(shell_view.pfnReclaimResources)>(1);
    shell_view.pfnGetImplicitPhysicalAdapterMask = reinterpret_cast<decltype(shell_view.pfnGetImplicitPhysicalAdapterMask)>(1);
    shell_view.pfnGetPresentPrivateDriverDataSize = reinterpret_cast<decltype(shell_view.pfnGetPresentPrivateDriverDataSize)>(1);
    shell_view.pfnQueryNodeMap = reinterpret_cast<decltype(shell_view.pfnQueryNodeMap)>(1);
    shell_view.pfnGetDebugAllocationInfo = reinterpret_cast<decltype(shell_view.pfnGetDebugAllocationInfo)>(1);
    shell_view.pfnCalcPrivateSchedulingGroupSize = reinterpret_cast<decltype(shell_view.pfnCalcPrivateSchedulingGroupSize)>(1);
    shell_view.pfnCreateSchedulingGroup = reinterpret_cast<decltype(shell_view.pfnCreateSchedulingGroup)>(1);
    shell_view.pfnDestroySchedulingGroup = reinterpret_cast<decltype(shell_view.pfnDestroySchedulingGroup)>(1);
    shell_view.pfnSetBackgroundProcessingMode = reinterpret_cast<decltype(shell_view.pfnSetBackgroundProcessingMode)>(1);
    check(all_filled(shell_view), "core table: every engine-ddi slot non-null (the 18 shell slots aside)");
    check(!core.pfnCreateCommandQueue && !core.pfnCreateFence && !core.pfnMakeResident,
          "core table: shell slots left untouched");
    check(all_filled(lists[0]) && all_filled(lists[1]), "list tables: all 70 slots non-null");

    // Retire hand-off (set_retire_policy): the refusals that need no context, and the decision itself.
    engine_ddi::RetirePolicy policy{sizeof(policy), 1, 256, 250};
    check(engine_ddi::set_retire_policy(nullptr, &policy) == E_INVALIDARG, "retire policy: null context refused");
    using engine_ddi::retire_defers;
    check(!retire_defers(false, 5, 256, 1000, 990, 250), "retire policy: off never defers");
    check(!retire_defers(true, 0, 256, 1000, 990, 250), "retire policy: nothing pending, nothing to defer");
    check(retire_defers(true, 5, 256, 1000, 990, 250) && retire_defers(true, 255, 256, 1000, 751, 250),
          "retire policy: under both bounds a submission defers");
    check(!retire_defers(true, 256, 256, 1000, 990, 250) && !retire_defers(true, 4000, 256, 1000, 990, 250),
          "retire policy: the backlog bound makes a submission run the sequence");
    check(!retire_defers(true, 5, 256, 1000, 750, 250) && !retire_defers(true, 5, 256, 100000, 990, 250),
          "retire policy: the age bound makes a submission run the sequence");
    check(!retire_defers(true, 5, 256, 1000, 0, 250), "retire policy: before any resource DDI's pass it never defers");
    check(!retire_defers(true, 5, 256, 990, 1000, 250), "retire policy: a tick ahead of now does not defer");
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
