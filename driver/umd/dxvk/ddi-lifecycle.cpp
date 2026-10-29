// SPDX-License-Identifier: MIT
#include "ddi-lifecycle.h"
#include <mutex>
namespace bc250::umd {
namespace {
struct RetainedHandles {
    std::mutex mutex;
    HANDLE handles[64]{};
    unsigned count=0,total=0;
};
RetainedHandles &retained() { static RetainedHandles r; return r; }
}
void note_retained_runtime_handle(HANDLE runtime) noexcept {
    auto &r=retained();
    std::lock_guard<std::mutex> lock(r.mutex);
    ++r.total;
    if (runtime && r.count<sizeof(r.handles)/sizeof(r.handles[0])) r.handles[r.count++]=runtime;
}
bool retained_runtime_handle(HANDLE runtime) noexcept {
    auto &r=retained();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (unsigned i=0;i<r.count;++i) if (runtime && r.handles[i]==runtime) return true;
    return false;
}
unsigned retained_device_count() noexcept {
    auto &r=retained();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.total;
}
bool kept_device_limit_reached() noexcept { return retained_device_count()>=kKeptDeviceLimit; }
HRESULT retire_device_handle(DdiDeviceHandle &handle) noexcept {
    auto *owner=handle.owner;
    if (!owner) return S_OK;
    HRESULT hr=E_FAIL;
    {
        RuntimeDomain::Scope scope(owner->runtime().domain);
        try { hr=owner->close(); }
        catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
        catch (...) { hr=E_FAIL; }
        if (SUCCEEDED(hr) && owner->has_live_objects()) hr=E_UNEXPECTED;
        if (FAILED(hr)) report_ddi_error(*owner,hr);
    }
    // Scope destructor refers to the owner's domain. Never delete it while
    // that scope is active, even after the last Vulkan/runtime object is gone.
    if (SUCCEEDED(hr)) { handle.owner=nullptr; delete owner; }
    else {
        // BD-253: the owner stays, with its hosted instance and winsys, for the life of the process. One line for
        // each such device, in both flavours: an ordinary error report.
        auto &runtime=owner->runtime();
        note_retained_runtime_handle(runtime.hDevice);
        static std::atomic_uint notes{16};
        device_life_note(notes,"BC250 BD-253: DestroyDevice kept the device: step=%s hr=%08X lost=%u runtime=%p "
            "identity=%p\n",owner->step(),unsigned(hr),unsigned(owner->bridge().device_lost),runtime.hDevice,
            runtime.host_identity());
    }
    return hr;
}
namespace {
void APIENTRY destroy(D3D10DDI_HDEVICE h) {
    auto *handle=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (handle) (void)retire_device_handle(*handle);
    // Runtime may free handle storage on return. Failed retirement deliberately
    // retains the heap owner rather than invalidating live callback userdata.
    // DeviceOwner retains its counted UMD/engine/ICD references on this path.
}
}
void install_lifecycle_ddi(D3D11_1DDI_DEVICEFUNCS &t) { t.pfnDestroyDevice=destroy; }
}
