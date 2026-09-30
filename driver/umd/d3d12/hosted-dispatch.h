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
// Backing of another owner on this device (an imported heap). borrow admits only an allocation that
// is complete, not retired and not in a callback, and holds it against release until give_back.
// One give_back per admitted borrow; the same handle may be borrowed more than once.
using HostedBorrow = bool (*)(void*,D3DKMT_HANDLE) noexcept;
using HostedReturn = void (*)(void*,D3DKMT_HANDLE) noexcept;
struct HostedDispatchHooks {
    void* userdata{};
    HostedOperation paging{};
    HostedOperation queue{};
    HostedBorrow borrow{};
    HostedReturn give_back{};
};

// Device-lifetime owner for internal ICD allocations. Callbacks arrive on every DDI thread of the
// device at once; each one runs in its own thread's entered domain (permission, not a lock). lock_
// guards the lists, their records and the contexts, and is a leaf: it is never held across a runtime
// callback, a hook or the error report. A record that a callback names is marked (busy, borrowed)
// under the lock before the callback and released under it afterwards. A busy record (its own create
// or destroy in flight) refuses every other call, as a re-entrant one always was; a borrowed record
// admits further borrows and refuses its destruction, and the release of an allocation's address,
// until they return. The one lock taken under lock_ is the heap
// imports' own leaf lock, through hooks_.borrow and hooks_.give_back. The owner stops engine workers
// before destruction and calls discard_metadata only after every call has returned.
// No destructor callback, KMT fallback, fabricated runtime queue, or successful
// response for an unsupported operation. Internal KT callbacks take hRTDevice;
// application queue hooks must use their real runtime queue ownership.
// Address ranges have two owners: an allocation's ordinary mapping and a reservation. No extent
// belongs to both. What is mapped inside a reservation is not recorded: the bridge checks extents
// and handles when a call is admitted and holds them only for the length of the runtime callback.
// Update admits MAP, MAP_PROTECT and UNMAP of one reservation per call; COPY and DoNotWait refuse.
class HostedDispatch final {
    struct Allocation;
    struct Reservation;
    struct Sync;
    bc250::umd::RuntimeDomain& domain_;
    D3D12DDI_HRTDEVICE runtime_{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 user_{};
    D3DDDI_DEVICECALLBACKS kernel_{};
    HostedDispatchHooks hooks_{};
    SRWLOCK lock_=SRWLOCK_INIT;
    // Under lock_ from here to paging_busy_.
    Allocation* allocations_{};
    Reservation* reservations_{};
    Sync* syncs_{};
    Sync* find_sync(D3DKMT_HANDLE handle) noexcept;
    // busy: its own create or destroy callback is in flight. borrowed: callbacks that name it, made by
    // user, the thread of the first one still in flight (for the HCONTEXT note in hosted-dispatch.cpp).
    struct Context {HANDLE handle{};uint32_t token{};bool busy{};unsigned borrowed{};DWORD user{};
                    bc250_host_progress progress{};};
    Context contexts_[16]{};
    uint32_t next_context_{};
    bc250_host_paging paging_{};
    bool paging_busy_{};                        // the paging queue's create or destroy is in flight
    Context* context(uint32_t token) noexcept;
    HRESULT internal_queue(uint32_t op,void* argument) noexcept;
    std::atomic<bool> lost_{false};
    std::atomic<bool> active_{true};
    Allocation* find(D3DKMT_HANDLE handle) noexcept;
    Reservation* containing(UINT64 base,UINT64 bytes) noexcept;
    bool owned(UINT64 base,UINT64 bytes,const void* except) const noexcept;
    // Under lock_. An internal allocation is held by its record; another owner's by hooks_.
    bool borrow(D3DKMT_HANDLE handle) noexcept;
    void give_back(D3DKMT_HANDLE handle) noexcept;
    void unlink(Allocation*) noexcept;
    void unlink(Reservation*) noexcept;
    void unlink(Sync*) noexcept;
    HRESULT update(void* argument) noexcept;
    HRESULT operation(uint32_t op,void* argument) noexcept;
    HRESULT remove_device(int site=__builtin_LINE()) noexcept;
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
    // unresolved allocation/reservation/context/paging/sync count; does not claim OS allocation reclamation.
    unsigned discard_metadata() noexcept;
    bool lost() const noexcept {return lost_.load();}
};
}
