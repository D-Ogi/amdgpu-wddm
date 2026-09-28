// SPDX-License-Identifier: MIT
#include "ddi-blt.h"
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
namespace {
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
        return owner.engine()->Blt(&operation);
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
void install_blt_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &t) { t.pfnBlt=blt; }
}
