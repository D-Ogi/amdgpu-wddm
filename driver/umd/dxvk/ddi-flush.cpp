// SPDX-License-Identifier: MIT
#include "ddi-flush.h"
namespace bc250::umd {
namespace {
void record_failure(DeviceOwner &owner,HRESULT hr) {
    report_ddi_error(owner,hr);
}
BOOL APIENTRY flush(D3D10DDI_HDEVICE h,UINT flags) {
    BOOL success=FALSE;
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        // This table is D3D11.1/WDDM1.2. Later TRIM_MEMORY needs its own
        // residency policy; do not claim trimming through an ordinary Flush.
        if (flags & ~UINT(D3D11_1DDI_FLUSH_UNLESS_NO_COMMANDS)) { record_failure(owner,E_INVALIDARG); return; }
        if (!owner.device()) { record_failure(owner,E_FAIL); return; }
        auto status=[&]() {
            if (owner.bridge().device_lost || owner.bridge().submission_failed) return HRESULT(DXGI_ERROR_DEVICE_REMOVED);
            return owner.device()->GetDeviceRemovedReason();
        };
        HRESULT hr=status();
        if (FAILED(hr)) { record_failure(owner,hr); return; }
        // Engine ABI E3: all pending commands have reached vkQueueSubmit when
        // this returns. Its empty-work guard implements UNLESS_NO_COMMANDS;
        // no CPU wait for GPU completion or artificial Present is needed.
        context.Flush();
        hr=status();
        if (FAILED(hr)) { record_failure(owner,hr); return; }
        success=TRUE;
    });
    return success;
}
}
void install_flush_ddi(D3D11_1DDI_DEVICEFUNCS &t) { t.pfnFlush=flush; }
}
