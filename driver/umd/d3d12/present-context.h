// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include "ddi-trace.h"
#include <cstdint>

// BD-133: the context the kernel present of a copied source is made on.
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
// The order, and why it is split over three DDIs. The D3D11 shell makes the kernel present itself, so it waits,
// presents and signals in one call. Here the runtime makes the kernel present, after pfnPresent has returned and after
// it has executed the command list that pfnPresent recorded the copy on. So:
//   1. pfnPresent (open_for_present) answers with this context. When a present was answered before, it first signals
//      this context's monitored fence: the signal is queued behind every kernel present made on the context so far.
//   2. ExecuteCommandLists of the list that carries the copy (queue-engine.cpp, execute):
//      a. before the engine submits the list, the queue GPU-waits for the last signal (queue_wait), so the copy cannot
//         overwrite a copy surface that an earlier kernel present still reads;
//      b. after the engine submitted it, this context GPU-waits for the queue's progress value that covers the copy
//         (copy_wait), so the kernel present that the runtime makes next reads the finished copy.
//   3. The runtime's kernel present on this context.
// The signal of step 1 for present N is made at pfnPresent N+1. For one swap chain that follows the kernel present N,
// because DXGI makes the Present calls of one swap chain one after the other. The copy of frame N+1 therefore waits for
// the present of frame N. The wait holds the copy only, not the frame's rendering, which was executed before it.
// drain (the destroy of a copied source) and close (DestroyDevice) signal what is open and wait on the CPU, bounded.
//
// One lock of its own, short, never held over the CPU wait. pfnPresent and ExecuteCommandLists also run under the
// device's queue domain (device-state.h, QueueDomainScope); the lock is for the resource destroy, which is not a queue
// operation. Pure apart from the callbacks it is given, so it is host-tested (present-outputs-test.cpp,
// queue-engine-test.cpp). Log lines go to the channel of every shell refusal (ddi-trace.h, ddi_refusal): the
// AMDGPU_WDDM_LOG sink and the debugger channel, counts and status codes only, no handle and no pointer.

namespace native12 {

class PresentContext final {
public:
    struct Counts {
        UINT64 presents;        // copied presents answered with this context
        UINT64 signals;         // signals of its fence, each behind the kernel presents before it
        UINT64 queue_waits;     // queue GPU waits for a signal (step 2a)
        UINT64 copy_waits;      // waits of this context for a queue's copy (step 2b)
    };

    PresentContext() noexcept = default;
    PresentContext(const PresentContext&) = delete;
    PresentContext& operator=(const PresentContext&) = delete;

    // Step 1. device is the runtime device handle that the kernel callbacks take (D3DDDI_DEVICECALLBACKS); both
    // outlive this object, which is a member of the shell's Device. The first call creates the context and its fence:
    // node 0, engine affinity 1 (the GFX engine of every queue context of this shell) and no private driver data,
    // which is what makes it a context the kernel present accepts. A failure leaves nothing created, writes one line
    // and returns the status, which the caller reports as the device error of the present. A later call tries again.
    HRESULT open_for_present(HANDLE device, const D3DDDI_DEVICECALLBACKS& callbacks, HANDLE* context) noexcept {
        if (!context) return E_INVALIDARG;
        *context = nullptr;
        Lock held(lock_);
        if (!context_) {
            const HRESULT made = create(device, callbacks);
            if (made != S_OK) return made;
        }
        if (open_) {
            const HRESULT hr = signal();
            if (hr != S_OK) return hr;
        }
        open_ = true;
        const UINT64 n = ++presents_;
        *context = context_;
        if (!(n & (n - 1)))
            ddi_refusal("present context: %llu copied presents answered with the present context (%llu signals, "
                        "%llu queue waits, %llu copy waits)",
                        static_cast<unsigned long long>(n), static_cast<unsigned long long>(signals_),
                        static_cast<unsigned long long>(queue_waits_), static_cast<unsigned long long>(copy_waits_));
        return S_OK;
    }

    // Step 2a: queue_context waits on the GPU for the last signal of this context, unless it already waited for that
    // value (waited is the queue's own record, QueueEngineOwner::present_waited). Nothing to wait for: S_OK, no call.
    HRESULT queue_wait(HANDLE queue_context, UINT64& waited) noexcept {
        if (!queue_context) return E_INVALIDARG;
        Lock held(lock_);
        if (!context_ || signaled_ <= waited) return S_OK;
        const UINT64 value = signaled_;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};
        wait.hContext = queue_context;
        wait.ObjectCount = 1;
        wait.ObjectHandleArray = &fence_;
        wait.MonitoredFenceValueArray = &value;
        const HRESULT hr = callbacks_->pfnWaitForSynchronizationObjectFromGpuCb(device_, &wait);
        if (FAILED(hr)) {
            ddi_refusal("present context: the queue's wait for the kernel presents up to value %llu could not be "
                        "queued (%08lx): the copy is not executed",
                        static_cast<unsigned long long>(value), static_cast<unsigned long>(hr));
            return hr;
        }
        waited = value;
        ++queue_waits_;
        return S_OK;
    }

    // Step 2b: this context waits on the GPU until the queue's progress fence reaches value, the progress the queue
    // published for the submission that carries the copy. E_UNEXPECTED when no present was ever answered: a marked
    // list exists only after one was (native-tables.cpp marks it after open_for_present succeeded).
    HRESULT copy_wait(D3DKMT_HANDLE queue_fence, UINT64 value) noexcept {
        if (!queue_fence || !value) return E_INVALIDARG;
        Lock held(lock_);
        if (!context_) return E_UNEXPECTED;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};
        wait.hContext = context_;
        wait.ObjectCount = 1;
        wait.ObjectHandleArray = &queue_fence;
        wait.MonitoredFenceValueArray = &value;
        const HRESULT hr = callbacks_->pfnWaitForSynchronizationObjectFromGpuCb(device_, &wait);
        if (FAILED(hr)) {
            ddi_refusal("present context: its wait for the copy at queue progress %llu could not be queued (%08lx): "
                        "the kernel present would read a copy that may not be finished",
                        static_cast<unsigned long long>(value), static_cast<unsigned long>(hr));
            return hr;
        }
        ++copy_waits_;
        return S_OK;
    }

    // Every kernel present made on the context so far has finished: signal what is open, then wait on the CPU,
    // bounded. S_OK at once when no context exists or its fence already passed the value. A timeout is
    // DXGI_ERROR_DEVICE_HUNG, as in the D3D11 shell's wait_present_value.
    HRESULT drain(DWORD timeout_ms) noexcept {
        UINT64 target = 0;
        D3DKMT_HANDLE fence = 0;
        const volatile UINT64* cpu = nullptr;
        HANDLE device = nullptr;
        const D3DDDI_DEVICECALLBACKS* cb = nullptr;
        {
            Lock held(lock_);
            if (!context_) return S_OK;
            if (open_) {
                const HRESULT hr = signal();
                if (hr != S_OK) return hr;
            }
            target = signaled_;
            fence = fence_;
            cpu = fence_cpu_;
            device = device_;
            cb = callbacks_;
        }
        if (!target || *cpu >= target) return S_OK;
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return HRESULT_FROM_WIN32(GetLastError());
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{};
        wait.ObjectCount = 1;
        wait.ObjectHandleArray = &fence;
        wait.FenceValueArray = &target;
        wait.hAsyncEvent = event;
        HRESULT hr = cb->pfnWaitForSynchronizationObjectFromCpuCb(device, &wait);
        if (SUCCEEDED(hr) && WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) hr = DXGI_ERROR_DEVICE_HUNG;
        CloseHandle(event);
        if (FAILED(hr)) {
            ddi_refusal("present context: the wait for the kernel presents up to value %llu ended with %08lx",
                        static_cast<unsigned long long>(target), static_cast<unsigned long>(hr));
            return hr;
        }
        return S_OK;
    }

    // DestroyDevice: drain, then the fence and the context, in that order (the D3D11 shell's close order). Both are
    // destroyed also after a failed drain: the device is going, and the kernel retires a destroyed context itself.
    // Returns the first failure.
    HRESULT close(DWORD timeout_ms) noexcept {
        const HRESULT drained = drain(timeout_ms);
        Lock held(lock_);
        if (!context_) return drained;
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT sync{};
        sync.hSyncObject = fence_;
        const HRESULT fence_destroyed = callbacks_->pfnDestroySynchronizationObjectCb(device_, &sync);
        D3DDDICB_DESTROYCONTEXT destroy{};
        destroy.hContext = context_;
        const HRESULT context_destroyed = callbacks_->pfnDestroyContextCb(device_, &destroy);
        HRESULT hr = drained;
        if (SUCCEEDED(hr) && FAILED(fence_destroyed)) hr = fence_destroyed;
        if (SUCCEEDED(hr) && FAILED(context_destroyed)) hr = context_destroyed;
        ddi_refusal("present context: closed after %llu copied presents (%llu signals, %llu queue waits, %llu copy "
                    "waits): %08lx",
                    static_cast<unsigned long long>(presents_), static_cast<unsigned long long>(signals_),
                    static_cast<unsigned long long>(queue_waits_), static_cast<unsigned long long>(copy_waits_),
                    static_cast<unsigned long>(hr));
        context_ = nullptr;
        fence_ = 0;
        fence_cpu_ = nullptr;
        signaled_ = 0;
        open_ = false;
        return SUCCEEDED(hr) ? S_OK : hr;
    }

    // For the trace line of pfnPresent and for the host suite.
    HANDLE context() const noexcept { return context_; }
    UINT64 signaled() const noexcept { return signaled_; }
    Counts counts() const noexcept { return {presents_, signals_, queue_waits_, copy_waits_}; }

private:
    class Lock final {
        SRWLOCK& lock_;
    public:
        explicit Lock(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
        ~Lock() { ReleaseSRWLockExclusive(&lock_); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

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
            D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 sync{};
            sync.Info.Type = D3DDDI_MONITORED_FENCE;
            sync.Info.MonitoredFence.EngineAffinity = 1;
            hr = cb.pfnCreateSynchronizationObject2Cb(device, &sync);
            if (SUCCEEDED(hr)) {
                fence = sync.hSyncObject;
                cpu = static_cast<const volatile UINT64*>(sync.Info.MonitoredFence.FenceValueCPUVirtualAddress);
                hr = fence && cpu ? S_OK : E_UNEXPECTED;
                if (hr != S_OK && fence) {
                    D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT undo{};
                    undo.hSyncObject = fence;
                    const HRESULT undone = cb.pfnDestroySynchronizationObjectCb(device, &undo);
                    if (FAILED(undone))
                        ddi_refusal("present context: a malformed fence could not be destroyed again (%08lx)",
                                    static_cast<unsigned long>(undone));
                }
            }
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
        signaled_ = 0;
        open_ = false;
        ddi_refusal("present context: created for the copied presents of this device (node 0, engine affinity 1, no "
                    "private driver data: not a UMD context)");
        return S_OK;
    }

    // Lock held, context live: the next value, on this context, behind every kernel present made on it so far.
    HRESULT signal() noexcept {
        const UINT64 value = signaled_ + 1;
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
    UINT64 presents_{}, signals_{}, queue_waits_{}, copy_waits_{};
};

} // namespace native12
