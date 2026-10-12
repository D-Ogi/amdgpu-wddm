// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include "ddi-trace.h"
#include "present-copy-sync.h"
#include <cstdint>

// BD-133: the context the kernel present of a copied source is made on, and the fences that order it.
//
// Why a context of its own. The kernel driver builds the GPU present of a Blt only on a context that is not a UMD
// context (driver\kmd\wddm.c: WddmBuildGpuPresent refuses Context->UmdContext, and Bc250WddmSubmitCommandImpl refuses
// the present DMA of one), and a context is a UMD context when its creation carried private driver data
// (Bc250WddmCreateContext).
// Every queue context of this shell carries the BC2C request (queue-request.h), so a copied present answered with the
// queue's context reached the kernel and was refused there: lab arm B of 2026-10-12 17:26Z counted "GPU Present calls
// 97 records 0 rotate 0 refused 97" and "errors parameter 97" for 97 copied presents. The D3D11 shell makes its
// presents on a context created without private data (dxvk\device-owner.cpp, the present context;
// dxvk\runtime-bridge.cpp, queue_present_wait and signal_present). This is that pattern for the D3D12 shell, for the
// copied present only: a linear primary is still presented on its queue's context, unchanged (native-tables.cpp).
//
// The order (present-copy-sync.h states the whole rule):
//   1. pfnPresent of a copied source (present_copy_answer below):
//      a. begin: the context and its present fence P exist; when a present was answered before, P is signaled behind
//         every kernel present made on the context so far; P is read on the CPU, the witness of retired readers;
//      b. the engine picks a reusable copy surface of the source or makes one within the budget, the surface gets its
//         completion fence C_S (prepare), and the copy is recorded on the list;
//      c. ticket: k = the surface's next C_S value, and its reader = the P value the next signal will carry, which is
//         behind this present's kernel present; this context queues a GPU wait for C_S >= k;
//      d. the list carries the ticket (PresentCopyListMark).
//   2. ExecuteCommandLists of the list: the work up to and including it is submitted, then C_S = k is signaled on the
//      executing queue's own context (copy_signal; queue-engine.cpp).
//   3. The runtime's kernel present on this context, held by the wait of 1c until the copy is finished.
// The signal of P for present N is made at pfnPresent N+1. For one swap chain that follows the kernel present N,
// because DXGI makes the Present calls of one swap chain one after the other.
// retire (the destroy of a copied source) and close (DestroyDevice) prove retirement on the CPU, bounded, or keep
// what they cannot prove (R3 of the review of 2026-10-12: a context that may still be blocked is never destroyed).
//
// One lock of its own, short, never held over the CPU wait. pfnPresent and ExecuteCommandLists also run under the
// device's queue domain (device-state.h, QueueDomainScope); the lock is for the resource destroy and the list reset,
// which are not queue operations. Pure apart from the callbacks it is given, so it is host-tested
// (present-outputs-test.cpp, present-copy-order-test.cpp). Log lines go to the channel of every shell refusal
// (ddi-trace.h, ddi_refusal): the AMDGPU_WDDM_LOG sink and the debugger channel, counts and status codes only, no
// handle and no pointer.

namespace native12 {

class PresentContext final {
public:
    struct Counts {
        UINT64 presents;        // copied presents given a ticket on this context
        UINT64 signals;         // signals of its present fence, each behind the kernel presents before it
        UINT64 copy_fences;     // completion fences of copy surfaces created
        UINT64 copy_signals;    // signals of a copy fence queued on a queue context behind its copy
        UINT64 cancelled;       // tickets whose list was reset or destroyed without being executed (rule D)
        UINT64 retained;        // copy surfaces kept because their retirement could not be proven
    };

    PresentContext() noexcept = default;
    // Host suites only: a context whose present fence was last signaled at signaled, for the near-limit cases.
    explicit PresentContext(UINT64 signaled) noexcept : signaled_(signaled) {}
    PresentContext(const PresentContext&) = delete;
    PresentContext& operator=(const PresentContext&) = delete;

    // Step 1a. device is the runtime device handle that the kernel callbacks take (D3DDDI_DEVICECALLBACKS); both
    // outlive this object, which is a member of the shell's Device. The first call creates the context and its fence:
    // node 0, engine affinity 1 (the GFX engine of every queue context of this shell) and no private driver data,
    // which is what makes it a context the kernel present accepts. A failure leaves nothing created, writes one line
    // and returns the status, which the caller reports as the device error of the present. A later call tries again.
    // *present_cpu is P as the CPU reads it after the signal; UINT64_MAX is the removed device (R4).
    HRESULT begin(HANDLE device, const D3DDDI_DEVICECALLBACKS& callbacks, UINT64* present_cpu) noexcept {
        if (!present_cpu) return E_INVALIDARG;
        *present_cpu = 0;
        Lock held(lock_);
        if (kept_) return DXGI_ERROR_DEVICE_REMOVED;
        if (!context_) {
            const HRESULT made = create(device, callbacks);
            if (made != S_OK) return made;
        }
        if (open_) {
            const HRESULT hr = signal();
            if (hr != S_OK) return hr;
        }
        const UINT64 now = *fence_cpu_;
        if (now == UINT64_MAX) {
            ddi_refusal("present context: the present fence reads UINT64_MAX, the value of a removed device: the "
                        "copied present is not made");
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        open_ = true;
        *present_cpu = now;
        return S_OK;
    }

    // Step 1b, called by the engine half for the surface it chose, before it records the copy: the surface's
    // completion fence, created once and kept with the surface until retire proves it idle.
    HRESULT prepare(PresentCopySlot& slot) noexcept {
        Lock held(lock_);
        if (kept_) return DXGI_ERROR_DEVICE_REMOVED;
        if (!context_) return E_UNEXPECTED;
        if (slot.fence) return S_OK;
        // A surface with a history and no fence was never given one here.
        if (slot.issued || slot.reader || slot.fence_cpu) return E_UNEXPECTED;
        D3DKMT_HANDLE fence = 0;
        const volatile UINT64* cpu = nullptr;
        const HRESULT hr = make_fence(device_, *callbacks_, &fence, &cpu);
        if (hr != S_OK) {
            ddi_refusal("present context: the completion fence of a copy surface could not be created (%08lx): the "
                        "present is not made, before its copy is recorded",
                        static_cast<unsigned long>(hr));
            return hr;
        }
        slot.fence = fence;
        slot.fence_cpu = cpu;
        ++copy_fences_;
        ++live_copy_fences_;
        return S_OK;
    }

    // Step 1c. k and the reader are the surface's next values; both are refused at 0 and UINT64_MAX before any
    // callback (R5). The wait is queued before the present returns, so the runtime's kernel present on this context is
    // behind it whatever happens to the list. A failure changes nothing and is the device error of the present.
    HRESULT ticket(PresentCopySlot& slot, PresentCopyTicket* ticket) noexcept {
        if (!ticket) return E_INVALIDARG;
        *ticket = {};
        Lock held(lock_);
        if (kept_) return DXGI_ERROR_DEVICE_REMOVED;
        if (!context_ || !open_ || !slot.fence) return E_UNEXPECTED;
        const UINT64 k = slot.issued + 1;
        const UINT64 reader = signaled_ + 1;
        if (!present_fence_value_valid(k) || !present_fence_value_valid(reader)) {
            ddi_refusal("present context: the copy's fence value %llu or the present's fence value %llu is at the end "
                        "of its range: the present is not made",
                        static_cast<unsigned long long>(k), static_cast<unsigned long long>(reader));
            return E_FAIL;
        }
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};
        wait.hContext = context_;
        wait.ObjectCount = 1;
        wait.ObjectHandleArray = &slot.fence;
        wait.MonitoredFenceValueArray = &k;
        const HRESULT hr = callbacks_->pfnWaitForSynchronizationObjectFromGpuCb(device_, &wait);
        if (FAILED(hr)) {
            ddi_refusal("present context: its wait for copy %llu could not be queued (%08lx): the present is not made",
                        static_cast<unsigned long long>(k), static_cast<unsigned long>(hr));
            return hr;
        }
        slot.issued = k;
        slot.reader = reader;
        *ticket = {&slot, k};
        const UINT64 n = ++presents_;
        if (!(n & (n - 1)))
            ddi_refusal("present context: %llu copied presents answered with the present context (%llu signals, "
                        "%llu copy fences, %llu copy signals)",
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(signals_),
                        static_cast<unsigned long long>(copy_fences_), static_cast<unsigned long long>(copy_signals_));
        return S_OK;
    }

    // Step 2: C_S = k, on queue_context, after the engine submitted the list that carries the copy. Refused at 0 and
    // UINT64_MAX before the callback (R5). A failure is returned, and the caller removes the device: the present
    // waits for this value and nothing else will ever signal it.
    HRESULT copy_signal(HANDLE queue_context, const PresentCopyTicket& ticket) noexcept {
        if (!queue_context || !ticket.slot) return E_INVALIDARG;
        Lock held(lock_);
        if (!present_fence_value_valid(ticket.value)) {
            ddi_refusal("present copy: copy fence value %llu is no value to signal: the device is removed",
                        static_cast<unsigned long long>(ticket.value));
            return E_FAIL;
        }
        PresentCopySlot& slot = *ticket.slot;
        if (!context_ || !slot.fence || ticket.value > slot.issued) return E_UNEXPECTED;
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 request{};
        request.ObjectCount = 1;
        request.ObjectHandleArray = &slot.fence;
        request.BroadcastContextCount = 1;
        request.BroadcastContextArray = &queue_context;
        request.MonitoredFenceValueArray = &ticket.value;
        const HRESULT hr = callbacks_->pfnSignalSynchronizationObjectFromGpu2Cb(device_, &request);
        if (FAILED(hr)) {
            ddi_refusal("present copy: the signal of copy %llu behind its copy could not be queued (%08lx): its present "
                        "waits for it and nothing else signals it, so the device is removed",
                        static_cast<unsigned long long>(ticket.value), static_cast<unsigned long>(hr));
            return hr;
        }
        if (ticket.value > slot.signaled) slot.signaled = ticket.value;
        ++copy_signals_;
        return S_OK;
    }

    // Rule D: the list that carried ticket was reset or destroyed without being executed. Its present waits for a
    // copy fence value nothing will signal, and a CPU signal would release that present, and through the shared present
    // fence other readers, before any copy finished. So nothing is signaled: the context is kept (terminal), the
    // surface is kept, and the caller removes the device.
    void cancel(const PresentCopyTicket& ticket) noexcept {
        Lock held(lock_);
        kept_ = true;
        ++cancelled_;
        if (ticket.slot && !ticket.slot->retained.exchange(true, std::memory_order_acq_rel)) ++retained_;
        ddi_refusal("present copy: a command list that carried copy %llu was reset or destroyed without being "
                    "executed (%llu such lists): its present waits for that copy, which no CPU signal may stand in "
                    "for, so the device is removed and the copy surface, its fence and the present context are kept "
                    "for the OS teardown",
                    static_cast<unsigned long long>(ticket.value), static_cast<unsigned long long>(cancelled_));
    }

    // The destroy of a copied source: each of its count surfaces is released only when its retirement is proven, its
    // last copy completed and its last reader retired, both read on the CPU (R4: UINT64_MAX is no witness). The open
    // present is signaled when a reader waits for it, and the CPU waits, bounded, once for all of them. A surface
    // proven idle loses its fence here, and the engine then releases it (engine-ddi release_present_copy); any other
    // is marked retained, which the engine keeps. Returns the first failure, which the caller reports.
    HRESULT retire(PresentCopySlot* const* slots, unsigned count, DWORD timeout_ms) noexcept {
        if (count > kPresentCopySurfacesPerSource || (count && !slots)) return E_INVALIDARG;
        bool proving[kPresentCopySurfacesPerSource]{};
        HRESULT first = S_OK;
        UINT64 target = 0;
        bool any = false;
        Waited present{};
        {
            Lock held(lock_);
            for (unsigned i = 0; i < count; ++i) {
                PresentCopySlot* s = slots[i];
                if (!s || (!s->fence && !s->issued)) continue;     // never given a ticket: nothing to prove
                if (s->retained.load(std::memory_order_acquire)) {
                    if (SUCCEEDED(first)) first = DXGI_ERROR_DEVICE_REMOVED;
                    continue;
                }
                if (kept_ || !context_ || s->signaled < s->issued) {
                    // A ticket whose list was never executed: its present waits for it, so neither may go.
                    keep(*s);
                    if (SUCCEEDED(first)) first = DXGI_ERROR_DEVICE_REMOVED;
                    continue;
                }
                proving[i] = true;
                any = true;
                if (s->reader > target) target = s->reader;
            }
            if (any && target > signaled_) {
                const HRESULT hr = open_ && target == signaled_ + 1 ? signal() : E_UNEXPECTED;
                if (hr != S_OK) {
                    for (unsigned i = 0; i < count; ++i)
                        if (proving[i]) keep(*slots[i]);
                    return SUCCEEDED(first) ? hr : first;
                }
            }
            present = {device_, callbacks_, fence_, fence_cpu_};
        }
        if (!any) return first;
        const HRESULT waited = target ? wait_present(present, target, timeout_ms) : S_OK;
        if (FAILED(waited))
            ddi_refusal("present context: the readers of a destroyed copied source were not proven retired (present "
                        "fence value %llu, %08lx): its copy surfaces are kept",
                        static_cast<unsigned long long>(target), static_cast<unsigned long>(waited));
        Lock held(lock_);
        for (unsigned i = 0; i < count; ++i) {
            if (!proving[i]) continue;
            PresentCopySlot& s = *slots[i];
            const UINT64 copied = s.fence_cpu ? *s.fence_cpu : 0;
            if (FAILED(waited) || copied == UINT64_MAX || copied < s.issued) {
                keep(s);
                if (SUCCEEDED(first)) first = FAILED(waited) ? waited : DXGI_ERROR_DEVICE_REMOVED;
                continue;
            }
            if (s.fence) {
                D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT sync{};
                sync.hSyncObject = s.fence;
                const HRESULT destroyed = callbacks_->pfnDestroySynchronizationObjectCb(device_, &sync);
                if (FAILED(destroyed)) {
                    ddi_refusal("present context: the completion fence of an idle copy surface could not be destroyed "
                                "(%08lx): the OS teardown frees it",
                                static_cast<unsigned long>(destroyed));
                    if (SUCCEEDED(first)) first = destroyed;
                }
                if (live_copy_fences_) --live_copy_fences_;
            }
            s.fence = 0;
            s.fence_cpu = nullptr;
            s.issued = s.signaled = s.reader = 0;
        }
        return first;
    }

    // Every kernel present made on the context so far has finished: signal what is open, then wait on the CPU,
    // bounded. S_OK at once when no context exists or its fence already passed the value. A timeout, or a wake that
    // leaves the fence short, is DXGI_ERROR_DEVICE_HUNG; UINT64_MAX before or after the wait is
    // DXGI_ERROR_DEVICE_REMOVED (R4, as the D3D11 shell's wait_present_value).
    HRESULT drain(DWORD timeout_ms) noexcept {
        UINT64 target = 0;
        Waited present{};
        {
            Lock held(lock_);
            if (!context_) return S_OK;
            if (kept_) return DXGI_ERROR_DEVICE_REMOVED;
            if (open_) {
                const HRESULT hr = signal();
                if (hr != S_OK) return hr;
            }
            target = signaled_;
            present = {device_, callbacks_, fence_, fence_cpu_};
        }
        if (!target) return S_OK;
        const HRESULT hr = wait_present(present, target, timeout_ms);
        if (FAILED(hr))
            ddi_refusal("present context: the wait for the kernel presents up to value %llu ended with %08lx",
                        static_cast<unsigned long long>(target), static_cast<unsigned long>(hr));
        return hr;
    }

    // DestroyDevice. The fence and the context are destroyed only after the drain proved every kernel present on the
    // context finished. Otherwise (R3, as QueueEngineRegistry::release_context keeps a queue context it cannot prove
    // retired) the context enters a terminal kept state: neither is destroyed, the OS teardown of the device frees
    // them, and every later call is refused. A context with a cancelled ticket, or with a ticket no queue signaled, is
    // kept without a wait, because its waits can never be satisfied.
    HRESULT close(DWORD timeout_ms) noexcept {
        HRESULT hr = S_OK;
        {
            Lock held(lock_);
            if (!context_) return S_OK;
            if (kept_) hr = DXGI_ERROR_DEVICE_REMOVED;
            else if (copy_signals_ < presents_) hr = DXGI_ERROR_DEVICE_REMOVED;
        }
        if (hr == S_OK) hr = drain(timeout_ms);
        Lock held(lock_);
        if (hr != S_OK) {
            if (!closed_)
                ddi_refusal("present context: kept for the OS teardown after %llu copied presents (%llu signals, %llu "
                            "copy signals, %llu cancelled): %08lx; neither its fence nor the context is destroyed, "
                            "because a context that may still wait for a copy is not proven retired",
                            static_cast<unsigned long long>(presents_), static_cast<unsigned long long>(signals_),
                            static_cast<unsigned long long>(copy_signals_), static_cast<unsigned long long>(cancelled_),
                            static_cast<unsigned long>(hr));
            kept_ = true;
            closed_ = true;
            return FAILED(hr) ? hr : E_UNEXPECTED;
        }
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT sync{};
        sync.hSyncObject = fence_;
        const HRESULT fence_destroyed = callbacks_->pfnDestroySynchronizationObjectCb(device_, &sync);
        D3DDDICB_DESTROYCONTEXT destroy{};
        destroy.hContext = context_;
        const HRESULT context_destroyed = callbacks_->pfnDestroyContextCb(device_, &destroy);
        if (FAILED(fence_destroyed)) hr = fence_destroyed;
        if (SUCCEEDED(hr) && FAILED(context_destroyed)) hr = context_destroyed;
        ddi_refusal("present context: closed after %llu copied presents (%llu signals, %llu copy fences, %llu copy "
                    "signals, %llu copy fences left to the OS teardown): %08lx",
                    static_cast<unsigned long long>(presents_), static_cast<unsigned long long>(signals_),
                    static_cast<unsigned long long>(copy_fences_), static_cast<unsigned long long>(copy_signals_),
                    static_cast<unsigned long long>(live_copy_fences_), static_cast<unsigned long>(hr));
        context_ = nullptr;
        fence_ = 0;
        fence_cpu_ = nullptr;
        signaled_ = 0;
        open_ = false;
        return SUCCEEDED(hr) ? S_OK : hr;
    }

    // For the trace line of pfnPresent and for the host suites.
    HANDLE context() const noexcept { return context_; }
    UINT64 signaled() const noexcept { return signaled_; }
    bool kept() const noexcept { return kept_; }
    Counts counts() const noexcept {
        return {presents_, signals_, copy_fences_, copy_signals_, cancelled_, retained_};
    }

private:
    class Lock final {
        SRWLOCK& lock_;
    public:
        explicit Lock(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
        ~Lock() { ReleaseSRWLockExclusive(&lock_); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };
    // What a CPU wait outside the lock needs.
    struct Waited {
        HANDLE device;
        const D3DDDI_DEVICECALLBACKS* callbacks;
        D3DKMT_HANDLE fence;
        const volatile UINT64* cpu;
    };

    // R4: the removal sentinel is checked before the comparison and again after the wake, and a wake that leaves the
    // fence short of the target proves nothing.
    static HRESULT wait_present(const Waited& w, UINT64 target, DWORD timeout_ms) noexcept {
        if (!w.cpu || !w.callbacks) return E_UNEXPECTED;
        UINT64 now = *w.cpu;
        if (now == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        if (now >= target) return S_OK;
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return HRESULT_FROM_WIN32(GetLastError());
        D3DKMT_HANDLE fence = w.fence;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{};
        wait.ObjectCount = 1;
        wait.ObjectHandleArray = &fence;
        wait.FenceValueArray = &target;
        wait.hAsyncEvent = event;
        HRESULT hr = w.callbacks->pfnWaitForSynchronizationObjectFromCpuCb(w.device, &wait);
        if (SUCCEEDED(hr) && WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) hr = DXGI_ERROR_DEVICE_HUNG;
        CloseHandle(event);
        if (FAILED(hr)) return hr;
        now = *w.cpu;
        if (now == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        return now >= target ? S_OK : DXGI_ERROR_DEVICE_HUNG;
    }

    // Lock held.
    void keep(PresentCopySlot& slot) noexcept {
        if (!slot.retained.exchange(true, std::memory_order_acq_rel)) ++retained_;
    }

    // A monitored fence on node 0, engine affinity 1, starting at 0. Leaves nothing behind on a failure.
    static HRESULT make_fence(HANDLE device, const D3DDDI_DEVICECALLBACKS& cb, D3DKMT_HANDLE* fence,
                              const volatile UINT64** cpu) noexcept {
        *fence = 0;
        *cpu = nullptr;
        D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 sync{};
        sync.Info.Type = D3DDDI_MONITORED_FENCE;
        sync.Info.MonitoredFence.EngineAffinity = 1;
        const HRESULT hr = cb.pfnCreateSynchronizationObject2Cb(device, &sync);
        if (FAILED(hr)) return hr;
        const auto address = static_cast<const volatile UINT64*>(sync.Info.MonitoredFence.FenceValueCPUVirtualAddress);
        if (sync.hSyncObject && address) {
            *fence = sync.hSyncObject;
            *cpu = address;
            return S_OK;
        }
        if (sync.hSyncObject) {
            D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT undo{};
            undo.hSyncObject = sync.hSyncObject;
            const HRESULT undone = cb.pfnDestroySynchronizationObjectCb(device, &undo);
            if (FAILED(undone))
                ddi_refusal("present context: a malformed fence could not be destroyed again (%08lx)",
                            static_cast<unsigned long>(undone));
        }
        return E_UNEXPECTED;
    }

    // Lock held. Leaves nothing behind on a failure.
    HRESULT create(HANDLE device, const D3DDDI_DEVICECALLBACKS& cb) noexcept {
        const char* part = "the context";
        HRESULT hr = S_OK;
        if (!device) hr = E_INVALIDARG;
        else if (!cb.pfnCreateContextVirtualCb || !cb.pfnDestroyContextCb || !cb.pfnCreateSynchronizationObject2Cb ||
                 !cb.pfnDestroySynchronizationObjectCb || !cb.pfnWaitForSynchronizationObjectFromGpuCb ||
                 !cb.pfnSignalSynchronizationObjectFromGpu2Cb || !cb.pfnWaitForSynchronizationObjectFromCpuCb) {
            part = "a kernel callback";
            hr = E_NOTIMPL;
        }
        HANDLE made = nullptr;
        if (hr == S_OK) {
            D3DDDICB_CREATECONTEXTVIRTUAL create{};
            create.NodeOrdinal = 0;
            create.EngineAffinity = 1;
            // No private driver data, no flags: the kernel driver makes such a context a non-UMD context with the
            // allocation list a present needs (Bc250WddmCreateContext, DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT).
            hr = cb.pfnCreateContextVirtualCb(device, &create);
            if (SUCCEEDED(hr) && !create.hContext) hr = E_UNEXPECTED;
            if (SUCCEEDED(hr)) {
                hr = S_OK;
                made = create.hContext;
            }
        }
        D3DKMT_HANDLE fence = 0;
        const volatile UINT64* cpu = nullptr;
        if (hr == S_OK) {
            part = "its monitored fence";
            hr = make_fence(device, cb, &fence, &cpu);
            if (hr != S_OK) {
                D3DDDICB_DESTROYCONTEXT undo{};
                undo.hContext = made;
                const HRESULT undone = cb.pfnDestroyContextCb(device, &undo);
                if (FAILED(undone))
                    ddi_refusal("present context: the context could not be destroyed again after its fence failed "
                                "(%08lx)",
                                static_cast<unsigned long>(undone));
            }
        }
        if (hr != S_OK) {
            const HRESULT failure = FAILED(hr) ? hr : E_UNEXPECTED;
            ddi_refusal("present context: %s of the context for the kernel present of a copied source could not be "
                        "created (%08lx): the present is not made, and because pfnPresent names no status the runtime "
                        "loses the device",
                        part, static_cast<unsigned long>(failure));
            return failure;
        }
        device_ = device;
        callbacks_ = &cb;
        context_ = made;
        fence_ = fence;
        fence_cpu_ = cpu;
        open_ = false;
        ddi_refusal("present context: created for the copied presents of this device (node 0, engine affinity 1, no "
                    "private driver data: not a UMD context)");
        return S_OK;
    }

    // Lock held, context live: the next value, on this context, behind every kernel present made on it so far. The
    // value is refused at 0 and UINT64_MAX before the callback (R5).
    HRESULT signal() noexcept {
        const UINT64 value = signaled_ + 1;
        if (!present_fence_value_valid(value)) {
            ddi_refusal("present context: present fence value %llu is no value to signal",
                        static_cast<unsigned long long>(value));
            return E_FAIL;
        }
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 request{};
        request.ObjectCount = 1;
        request.ObjectHandleArray = &fence_;
        request.BroadcastContextCount = 1;
        request.BroadcastContextArray = &context_;
        request.MonitoredFenceValueArray = &value;
        const HRESULT hr = callbacks_->pfnSignalSynchronizationObjectFromGpu2Cb(device_, &request);
        if (FAILED(hr)) {
            ddi_refusal("present context: the signal of value %llu behind the kernel presents could not be queued "
                        "(%08lx)",
                        static_cast<unsigned long long>(value), static_cast<unsigned long>(hr));
            return hr;
        }
        signaled_ = value;
        open_ = false;
        ++signals_;
        return S_OK;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    HANDLE device_{};
    const D3DDDI_DEVICECALLBACKS* callbacks_{};
    HANDLE context_{};
    D3DKMT_HANDLE fence_{};
    const volatile UINT64* fence_cpu_{};
    UINT64 signaled_{};     // the last value signaled on context_
    bool open_{};           // a present was answered since that signal
    bool kept_{};           // terminal: a ticket was cancelled or close could not prove retirement (R3)
    bool closed_{};         // close has written its kept line
    UINT64 presents_{}, signals_{}, copy_fences_{}, copy_signals_{}, cancelled_{}, retained_{};
    UINT64 live_copy_fences_{};
};

// Step 1 of a copied present as one call, in its order, for native-tables.cpp and the host suites. The engine half is
// two hooks: mark_of finds the list's ticket record, record picks or makes the surface with pick, has it prepared,
// records the copy and names the surface and its allocation. stage is pfnPresent's trace stage: 6 the copy, 7 the
// present context. cancel and owner are what the list's reset or destroy calls for an unexecuted ticket (rule D).
struct PresentCopyEngine {
    void* state{};
    HRESULT (*mark_of)(void* state, PresentCopyListMark** mark) noexcept {};
    HRESULT (*record)(void* state, const PresentCopyPick& pick, PresentCopySlot** slot,
                      D3DKMT_HANDLE* allocation) noexcept {};
};
inline HRESULT present_copy_answer(PresentContext& present, HANDLE device, const D3DDDI_DEVICECALLBACKS& callbacks,
                                   const PresentCopyEngine& engine, PresentCopyCancel cancel, void* owner,
                                   D3DKMT_HANDLE* allocation, HANDLE* context, unsigned& stage) noexcept {
    if (!allocation || !context) return E_INVALIDARG;
    *allocation = 0;
    *context = nullptr;
    if (!engine.mark_of || !engine.record || !cancel) return E_INVALIDARG;
    stage = 6;
    PresentCopyListMark* mark = nullptr;
    HRESULT hr = engine.mark_of(engine.state, &mark);
    if (hr != S_OK || !mark) return FAILED(hr) ? hr : E_UNEXPECTED;
    if (mark->state() != PresentCopyListMark::Empty) {
        // One ticket a list (R1): a second present on a list that still carries one, or on one that was executed and
        // not reset, would make a ticket the list cannot carry. Refused before anything is recorded or waited for.
        ddi_refusal("present copy: the command list handed to this present already carries a copy (%s): the present "
                    "is not made, and because pfnPresent names no status the runtime loses the device",
                    mark->state() == PresentCopyListMark::Pending ? "not executed yet" : "executed, not reset");
        return E_INVALIDARG;
    }
    stage = 7;
    UINT64 present_cpu = 0;
    hr = present.begin(device, callbacks, &present_cpu);
    if (hr != S_OK) return hr;
    stage = 6;
    const PresentCopyPick pick{present_cpu, &present, [](void* state, PresentCopySlot& slot) noexcept {
        return static_cast<PresentContext*>(state)->prepare(slot);
    }};
    PresentCopySlot* slot = nullptr;
    D3DKMT_HANDLE made = 0;
    hr = engine.record(engine.state, pick, &slot, &made);
    if (hr != S_OK) return FAILED(hr) ? hr : E_UNEXPECTED;
    if (!slot || !made) return E_UNEXPECTED;
    stage = 7;
    PresentCopyTicket ticket{};
    hr = present.ticket(*slot, &ticket);
    if (hr != S_OK) return hr;
    if (!mark->mark(ticket, cancel, owner)) {
        // Unreachable under the queue domain (the state was Empty above); its wait is queued, so this is rule D.
        present.cancel(ticket);
        return E_UNEXPECTED;
    }
    *allocation = made;
    *context = present.context();
    return S_OK;
}

} // namespace native12
