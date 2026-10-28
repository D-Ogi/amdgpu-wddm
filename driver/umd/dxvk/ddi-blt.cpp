// SPDX-License-Identifier: MIT
#include "ddi-blt.h"
#include "present-probe.h"
#include <climits>
namespace bc250::umd {
HRESULT prepare_blt(const DXGI_DDI_ARG_BLT &a,BC250_DXVK_BLT &out) {
    auto *source=reinterpret_cast<DdiResource *>(a.hSrcResource);
    auto *destination=reinterpret_cast<DdiResource *>(a.hDstResource);
    if (!source || !destination || !source->object || !destination->object || a.Flags.Reserved ||
        a.DstLeft>LONG_MAX || a.DstRight>LONG_MAX || a.DstTop>LONG_MAX || a.DstBottom>LONG_MAX ||
        a.DstLeft>=a.DstRight || a.DstTop>=a.DstBottom) return E_INVALIDARG;
    if (a.Rotate<DXGI_DDI_MODE_ROTATION_UNSPECIFIED || a.Rotate>DXGI_DDI_MODE_ROTATION_ROTATE270) return E_INVALIDARG;
    BC250_DXVK_BLT result{};
    result.Source=source->object; result.SourceSubresource=a.SrcSubresource;
    result.Destination=destination->object; result.DestinationSubresource=a.DstSubresource;
    result.DestinationRect={LONG(a.DstLeft),LONG(a.DstTop),LONG(a.DstRight),LONG(a.DstBottom)};
    if (a.Flags.Resolve) result.Flags|=BC250_DXVK_BLT_RESOLVE;
    if (a.Flags.Convert) result.Flags|=BC250_DXVK_BLT_CONVERT;
    if (a.Flags.Stretch) result.Flags|=BC250_DXVK_BLT_STRETCH;
    if (a.Flags.Present) result.Flags|=BC250_DXVK_BLT_PRESENT;
    result.Rotation=UINT(a.Rotate);
    out=result; return S_OK;
}
HRESULT prepare_blt1(const DXGI_DDI_ARG_BLT1 &a,BC250_DXVK_BLT1 &out) {
    if (a.SrcLeft>LONG_MAX || a.SrcTop>LONG_MAX || a.SrcRight>LONG_MAX || a.SrcBottom>LONG_MAX ||
        a.SrcLeft>=a.SrcRight || a.SrcTop>=a.SrcBottom) return E_INVALIDARG;
    DXGI_DDI_ARG_BLT base{};
    base.hDevice=a.hDevice; base.hSrcResource=a.hSrcResource; base.hDstResource=a.hDstResource;
    base.SrcSubresource=a.SrcSubresource; base.DstSubresource=a.DstSubresource;
    base.DstLeft=a.DstLeft; base.DstTop=a.DstTop; base.DstRight=a.DstRight; base.DstBottom=a.DstBottom;
    base.Flags=a.Flags; base.Rotate=a.Rotate;
    BC250_DXVK_BLT converted{}; HRESULT hr=prepare_blt(base,converted);
    if (FAILED(hr)) return hr;
    BC250_DXVK_BLT1 result{};
    result.Source=converted.Source; result.SourceSubresource=converted.SourceSubresource;
    result.Destination=converted.Destination; result.DestinationSubresource=converted.DestinationSubresource;
    result.DestinationRect=converted.DestinationRect; result.Flags=converted.Flags; result.Rotation=converted.Rotation;
    result.SourceRect={LONG(a.SrcLeft),LONG(a.SrcTop),LONG(a.SrcRight),LONG(a.SrcBottom)};
    out=result; return S_OK;
}namespace {
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
// BD-253, lab flavour: a few DXGI Blts of the process, with the source and destination texels after the Blt. DXGI
// can copy a buffer into a surface it then presents (a stretch, a conversion, a window's redirection surface).
void blt_note(DeviceOwner &owner,const char *kind,const DXGI_DDI_ARG_BLT &a,HRESULT hr) {
    static std::atomic_uint blts{0};
    unsigned index=0;
    if (!probe_due(blts,index)) return;
    auto *source=reinterpret_cast<DdiResource *>(a.hSrcResource);
    auto *destination=reinterpret_cast<DdiResource *>(a.hDstResource);
    char from[64]="-",to[64]="-";
    if (SUCCEEDED(hr)) {
        if (source && source->object) probe_texels(owner,source->object,a.SrcSubresource,from,sizeof(from));
        if (destination && destination->object) probe_texels(owner,destination->object,a.DstSubresource,to,sizeof(to));
    }
    unsigned sf,sw,sh,df,dw,dh;
    probe_shape(source,sf,sw,sh); probe_shape(destination,df,dw,dh);
    char text[640];
    std::snprintf(text,sizeof(text),"BC250 BD-253: %s #%u flags=%08X rotate=%d src=%u fmt=%u %ux%u blt-model=%u "
        "dst=%u fmt=%u %ux%u rect=%u,%u-%u,%u hr=%08X texels: source %s destination %s thread=%lu\n",kind,index,
        unsigned(a.Flags.Value),int(a.Rotate),source ? unsigned(source->present_allocation) : 0u,sf,sw,sh,
        unsigned(source && source->blt_model_buffer),destination ? unsigned(destination->present_allocation) : 0u,
        df,dw,dh,a.DstLeft,a.DstTop,a.DstRight,a.DstBottom,unsigned(hr),from,to,(unsigned long)GetCurrentThreadId());
    OutputDebugStringA(text);
    amdgpu_wddm_log::print("%s",text);
}
#endif
HRESULT APIENTRY blt1(DXGI_DDI_ARG_BLT1 *args) {
    if (!args) return E_INVALIDARG;
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(args->hDevice);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    IBc250DxvkDevice1 *extended=nullptr;
    HRESULT hr=E_FAIL;
    try {
        if (!owner.engine()) return E_FAIL;
        BC250_DXVK_BLT1 operation{};
        hr=prepare_blt1(*args,operation);
        if (FAILED(hr)) return hr;
        hr=owner.engine()->QueryInterface(__uuidof(IBc250DxvkDevice1),reinterpret_cast<void **>(&extended));
        if (SUCCEEDED(hr)) hr=extended ? extended->Blt1(&operation) : E_NOINTERFACE;
    } catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
    catch (...) { hr=E_FAIL; }
    if (extended) extended->Release();
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    {
        DXGI_DDI_ARG_BLT base{};
        base.hDevice=args->hDevice; base.hSrcResource=args->hSrcResource; base.hDstResource=args->hDstResource;
        base.SrcSubresource=args->SrcSubresource; base.DstSubresource=args->DstSubresource;
        base.DstLeft=args->DstLeft; base.DstTop=args->DstTop; base.DstRight=args->DstRight; base.DstBottom=args->DstBottom;
        base.Flags=args->Flags; base.Rotate=args->Rotate;
        try { blt_note(owner,"Blt1",base,hr); } catch (...) {}
    }
#endif
    return ddi_device_status(hr);
}
HRESULT APIENTRY blt(DXGI_DDI_ARG_BLT *args) {
    if (!args) return E_INVALIDARG;
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(args->hDevice);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    try {
        if (!owner.engine()) return E_FAIL;
        BC250_DXVK_BLT operation{};
        HRESULT hr=prepare_blt(*args,operation);
        if (FAILED(hr)) return hr;
        // Engine performs conversion/resolve/stretch and submits for PRESENT.
        // It owns barriers; no CPU map or alternate backing surface is used.
        hr=owner.engine()->Blt(&operation);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
        blt_note(owner,"Blt",*args,hr);
#endif
        return ddi_device_status(hr);
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
void install_blt_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &t) { t.pfnBlt=blt; t.pfnBlt1=blt1; }
}
