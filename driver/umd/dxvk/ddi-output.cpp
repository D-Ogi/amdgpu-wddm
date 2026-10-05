// SPDX-License-Identifier: MIT
#include "ddi-output.h"
namespace bc250::umd {
HRESULT prepare_output_bindings(const D3D10DDI_HRENDERTARGETVIEW *rtvs,UINT numRtvs,UINT clearSlots,
    D3D10DDI_HDEPTHSTENCILVIEW dsv,const D3D11DDI_HUNORDEREDACCESSVIEW *uavs,const UINT *counters,
    UINT first,UINT count,UINT changedFirst,UINT changedCount,UINT limit,OutputBindings &out) {
    constexpr UINT rtvLimit=D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    if ((limit!=D3D11_PS_CS_UAV_REGISTER_COUNT && limit!=D3D11_1_UAV_SLOT_COUNT) ||
        numRtvs>rtvLimit || clearSlots>rtvLimit-numRtvs || (numRtvs && !rtvs) ||
        first>limit || count>limit-first || (count && (first<numRtvs || !uavs)) ||
        changedFirst>limit || changedCount>limit-changedFirst) return E_INVALIDARG;
    OutputBindings b{}; b.numRtvs=numRtvs;
    auto *depth=static_cast<DdiDepthStencilView *>(dsv.pDrvPrivate);
    if (depth && !depth->object) return E_INVALIDARG;
    b.dsv=depth ? depth->object : nullptr;
    for (UINT i=0;i<numRtvs;++i) {
        auto *s=static_cast<DdiRenderTargetView *>(rtvs[i].pDrvPrivate);
        if (s && !s->object) return E_INVALIDARG;
        b.rtvs[i]=s ? s->object : nullptr;
    }
    // DDI provides the complete OM state. COM can preserve UAVs outside an
    // updated interval, so explicitly cover all UAV slots after the RTVs.
    // changedFirst/changedCount are optimization hints, not a partial state.
    b.uavFirst=numRtvs; b.uavCount=limit-numRtvs;
    b.counters.fill(UINT_MAX);
    for (UINT i=0;i<count;++i) {
        auto *s=static_cast<DdiUnorderedAccessView *>(uavs[i].pDrvPrivate);
        if (s && !s->object) return E_INVALIDARG;
        const UINT at=first-numRtvs+i;
        b.uavs[at]=s ? s->object : nullptr;
        if (counters) b.counters[at]=counters[i];
    }
    out=b; return S_OK;
}
namespace {
void APIENTRY bind(D3D10DDI_HDEVICE h,const D3D10DDI_HRENDERTARGETVIEW *rtvs,UINT numRtvs,UINT clearSlots,
    D3D10DDI_HDEPTHSTENCILVIEW dsv,const D3D11DDI_HUNORDEREDACCESSVIEW *uavs,const UINT *counters,
    UINT first,UINT count,UINT changedFirst,UINT changedCount) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!owner.device()) { report_ddi_error(owner,E_FAIL); return; }
        const UINT limit=owner.device()->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
        OutputBindings b;
        HRESULT hr=prepare_output_bindings(rtvs,numRtvs,clearSlots,dsv,uavs,counters,first,count,changedFirst,changedCount,limit,b);
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        context.OMSetRenderTargetsAndUnorderedAccessViews(b.numRtvs,b.rtvs.data(),b.dsv,
            b.uavFirst,b.uavCount,b.uavs.data(),b.counters.data());
    });
}
}
void install_output_ddi(D3D11_1DDI_DEVICEFUNCS &t) { t.pfnSetRenderTargets=bind; }
}
