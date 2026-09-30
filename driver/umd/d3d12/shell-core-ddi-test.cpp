// SPDX-License-Identifier: MIT
#include "shell-core-ddi.h"
#include "queue-ddi.h"
#include "fence-ddi.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
struct Fixture {
    native12::Device device;
    unsigned errors{};
    Fixture() {
        device.runtime.handle = this;
        device.callbacks.pfnSetErrorCb = [](D3D10DDI_HRTDEVICE runtime, HRESULT hr) {
            auto f = static_cast<Fixture*>(runtime.handle);
            assert(f->device.lost.load()); // Sticky before a possibly reentrant callback.
            assert(hr == D3DDDIERR_DEVICEREMOVED);
            ++f->errors;
        };
    }
    D3D12DDI_HDEVICE handle() { return {&device}; }
};
template<class T> bool equal(const T& a, const T& b) { return !std::memcmp(&a, &b, sizeof(T)); }
} // namespace

int main() {
    D3D12DDI_DEVICE_FUNCS_CORE_0088 table{};
    native12::install_queue_entries(table);
    native12::install_fence_entries(table);
    native12::install_shell_core_entries(table);
    assert(table.pfnGetImplicitPhysicalAdapterMask && table.pfnQueryNodeMap &&
        table.pfnOfferResources && table.pfnReclaimResources && table.pfnCalcPrivateSchedulingGroupSize &&
        table.pfnCreateSchedulingGroup && table.pfnDestroySchedulingGroup &&
        table.pfnGetDebugAllocationInfo && table.pfnSetBackgroundProcessingMode);
    assert(table.pfnCreateCommandQueue == native12::queue_create);
    assert(table.pfnCreateFence == native12::fence_create);
    assert(!table.pfnMakeResident && !table.pfnEvict && !table.pfnGetPresentPrivateDriverDataSize);
    assert(!table.pfnCreateHeapAndResource && !table.pfnCheckFormatSupport);

    {
        Fixture f;
        assert(table.pfnGetImplicitPhysicalAdapterMask(f.handle()) == 1);
        UINT map[] = {91, 92, 93};
        table.pfnQueryNodeMap(f.handle(), 1, &map[1]);
        assert(map[0] == 91 && map[1] == 0 && map[2] == 93);
        assert(!f.device.lost && f.errors == 0);
    }
    {
        Fixture f;
        UINT map[] = {11, 12, 13, 14};
        table.pfnQueryNodeMap(f.handle(), 2, &map[1]);
        assert(map[0] == 11 && map[1] == D3D12DDI_NODE_MAP_HIDE_NODE &&
            map[2] == D3D12DDI_NODE_MAP_HIDE_NODE && map[3] == 14);
        assert(f.device.lost && f.errors == 1);
        assert(table.pfnGetImplicitPhysicalAdapterMask(f.handle()) == 0);
        table.pfnQueryNodeMap(f.handle(), 1, &map[1]);
        assert(map[1] == D3D12DDI_NODE_MAP_HIDE_NODE);
    }
    {
        Fixture f;
        UINT guard = 99;
        table.pfnQueryNodeMap(f.handle(), 0, &guard);
        assert(guard == 99 && f.errors == 1);
        // Direct calls test defensive null handling without violating the WDK
        // non-null annotation on a valid runtime table call.
        native12::query_node_map(f.handle(), 1, nullptr);
        assert(f.errors == 2);
        assert(native12::implicit_physical_adapter_mask({}) == 0);
        native12::query_node_map({}, 1, &guard);
        assert(guard == D3D12DDI_NODE_MAP_HIDE_NODE);
    }
    {
        Fixture f;
        D3D12DDI_HANDLE_AND_TYPE object{};
        D3D12DDIARG_OFFERRESOURCES offer{};
        offer.NumObjects = 1; offer.pObjects = &object;
        const auto offer_before = offer;
        assert(table.pfnOfferResources(f.handle(), &offer) == E_NOTIMPL && equal(offer, offer_before));
        BOOL discarded = TRUE;
        UINT64 paging_fence = 123456;
        D3D12DDI_HRTPAGINGQUEUE paging_queue{};
        D3D12DDIARG_RECLAIMRESOURCES_0001 reclaim{};
        reclaim.NumAdapters = 1; reclaim.pRTPagingQueue = &paging_queue;
        reclaim.NumObjects = 1; reclaim.pObjects = &object;
        reclaim.pDiscarded = &discarded; reclaim.pPagingFenceValue = &paging_fence; reclaim.WaitMask = 13;
        const auto reclaim_before = reclaim;
        assert(table.pfnReclaimResources(f.handle(), &reclaim) == E_NOTIMPL && equal(reclaim, reclaim_before));
        assert(discarded == TRUE && paging_fence == 123456);
        assert(!f.device.lost && !f.errors);
        assert(native12::offer_resources(f.handle(), nullptr) == E_INVALIDARG);
        assert(native12::reclaim_resources({}, &reclaim) == E_INVALIDARG);
        f.device.remove();
        assert(table.pfnOfferResources(f.handle(), &offer) == D3DDDIERR_DEVICEREMOVED);
        assert(table.pfnReclaimResources(f.handle(), &reclaim) == D3DDDIERR_DEVICEREMOVED);
        assert(equal(reclaim, reclaim_before) && discarded == TRUE && paging_fence == 123456);
    }
    {
        Fixture f;
        D3D12DDIARG_CREATESCHEDULINGGROUP_0050 args{};
        alignas(16) unsigned char storage[64];
        std::memset(storage, 0x5a, sizeof(storage));
        assert(table.pfnCreateSchedulingGroup(f.handle(), &args, {storage}, {}) == E_NOTIMPL);
        for (auto byte : storage) assert(byte == 0x5a);
        assert(!f.device.lost && !f.errors);
        assert(table.pfnCalcPrivateSchedulingGroupSize(f.handle(), &args) == 0);
        assert(f.device.lost && f.errors == 1);
        assert(table.pfnCreateSchedulingGroup(f.handle(), &args, {storage}, {}) == D3DDDIERR_DEVICEREMOVED);
        table.pfnDestroySchedulingGroup(f.handle(), {storage});
        assert(f.errors == 2);
        for (auto byte : storage) assert(byte == 0x5a);
    }
    {
        Fixture f;
        UINT va_count = 1, kmt_count = 1;
        D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012 va{};
        D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014 kmt{};
        std::memset(&va, 0x5a, sizeof(va)); std::memset(&kmt, 0x6b, sizeof(kmt));
        const auto va_before = va; const auto kmt_before = kmt;
        table.pfnGetDebugAllocationInfo(f.handle(), {}, &va_count, &va, &kmt_count, &kmt);
        assert(!va_count && !kmt_count && equal(va, va_before) && equal(kmt, kmt_before));
        assert(f.device.lost && f.errors == 1);
        native12::debug_allocation_info(f.handle(), {}, nullptr, nullptr, nullptr, nullptr);
        assert(f.errors == 2);
    }
    {
        Fixture f;
        BOOL more = TRUE;
        table.pfnSetBackgroundProcessingMode(f.handle(), {}, {}, &more);
        assert(more == FALSE && f.device.lost && f.errors == 1);
        native12::background_processing_mode(f.handle(), {}, {}, nullptr);
        assert(f.errors == 2);
    }
    std::puts("PASSED: nine shell slots, single-node identity, typed refusals, guarded outputs and sticky device loss; no runtime/GPU");
}
