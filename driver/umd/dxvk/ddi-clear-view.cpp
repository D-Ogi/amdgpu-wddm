// SPDX-License-Identifier: MIT
#include "ddi-clear-view.h"
#include "ddi-rtv.h"
#include "ddi-uav.h"
namespace bc250::umd {
ID3D11View *clear_view_object(D3D11DDI_HANDLETYPE type,void *handle) {
    if (!handle) return nullptr;
    switch(type) {
    case D3D10DDI_HT_RENDERTARGETVIEW: return static_cast<DdiRenderTargetView *>(handle)->object;
    case D3D11DDI_HT_UNORDEREDACCESSVIEW: return static_cast<DdiUnorderedAccessView *>(handle)->object;
    default: return nullptr; // Video DDI is not exposed by this rendering table.
    }
}
namespace {
void APIENTRY clear(D3D10DDI_HDEVICE h,D3D11DDI_HANDLETYPE type,void *handle,
    const FLOAT color[4],const D3D10_DDI_RECT *rects,UINT count) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *view=clear_view_object(type,handle);
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!view || !color) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED); return; }
        // Engine ABI1.1 candidate includes buffer-RTV clears (dd35ce7c).

        // DDI RECT is the Win32 RECT type. Preserve rectangles and float color
        // values: the engine owns clipping, format conversion and GPU barriers.
        // A NULL rectangle pointer denotes the entire surface in the DDI.
        context.ClearView(view,color,rects,rects ? count : 0);
    });
}
}
void install_clear_view_ddi(D3D11_1DDI_DEVICEFUNCS &t) { t.pfnClearView=clear; }
}
