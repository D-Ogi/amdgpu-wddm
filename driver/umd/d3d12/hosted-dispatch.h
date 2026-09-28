// SPDX-License-Identifier: MIT
#pragma once
#include "allocation.h"
#include "../dxvk/runtime-domain.h"
#include <bc250_host_bootstrap.h>
#include <atomic>

namespace native12 {
// Exact operation/argument ABI comes from the Mesa header used by this build.
// Hooks return HRESULT (not NTSTATUS), synchronously in the entered DDI scope.
// Optional paging override: 1/2/21/22. Queue fallback: 3/6/7/25/26/31..35/40..44.
// Device-owned internal paging and null-cookie contexts use KT callbacks directly.
// Non-null queue cookies require the root's validated queue-registry hook.
using HostedOperation = HRESULT (*)(void*,uint32_t,void*) noexcept;
struct HostedDispatchHooks {
    void* userdata{};
    HostedOperation paging{};
    HostedOperation queue{};
};

// Device-lifetime owner for internal ICD allocations. The caller serializes
// calls, stops engine workers before destruction, and enters domain on the DDI
// thread around every engine operation. Domain is permission, not a lock.
// No destructor callback, KMT fallback, fabricated runtime queue, or successful
// response for an unsupported operation. Internal KT callbacks take hRTDevice;
// application queue hooks must use their real runtime queue ownership. Ordinary
// allocation mappings only: sparse reservation/update and external imports refuse.
class HostedDispatch final {
    struct Allocation;
    struct Sync;
    bc250::umd::RuntimeDomain& domain_;
    D3D12DDI_HRTDEVICE runtime_{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 user_{};
    D3DDDI_DEVICECALLBACKS kernel_{};
    HostedDispatchHooks hooks_{};
    Allocation* allocations_{};
    Sync* syncs_{};
    Sync* find_sync(D3DKMT_HANDLE handle) noexcept;
    struct Context {HANDLE handle{};uint32_t token{};bool busy{};bc250_host_progress progress{};};
    Context contexts_[16]{};
    uint32_t next_context_{};
    bc250_host_paging paging_{};
    Context* context(uint32_t token) noexcept;
    HRESULT internal_queue(uint32_t op,void* argument) noexcept;
    std::atomic<bool> lost_{false};
    bool active_{true};
    Allocation* find(D3DKMT_HANDLE handle) noexcept;
    HRESULT operation(uint32_t op,void* argument) noexcept;
    HRESULT remove_device() noexcept;
public:
    HostedDispatch(bc250::umd::RuntimeDomain& domain,D3D12DDI_HRTDEVICE runtime,
                   const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& user,
                   const D3DDDI_DEVICECALLBACKS& kernel,HostedDispatchHooks hooks={}) noexcept;
    ~HostedDispatch();
    HostedDispatch(const HostedDispatch&)=delete;
    HostedDispatch& operator=(const HostedDispatch&)=delete;
    static int32_t dispatch(void* userdata,uint32_t op,void* argument) noexcept;
    // Explicit identity must equal the identity used for imports by the embedder.
    bc250_host descriptor(uint64_t adapter_luid,void* identity) noexcept;
    // Terminal metadata release only, after all calls/workers stop. Returns the
    // unresolved allocation/context/paging/sync count; does not claim OS allocation reclamation.
    unsigned discard_metadata() noexcept;
    bool lost() const noexcept {return lost_.load();}
};
}
