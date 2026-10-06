// SPDX-License-Identifier: MIT
//
// M15.14 increment 1: the DXGI obligation that comes with the D3D11_1 DDI.
//
// At the D3D10.0 DDI the driver fills `DXGI_DDI_BASE_FUNCTIONS`, which has 7 entries, and the hosted Mesa
// frontend fills exactly those 7 (Device.cpp). At D3D11_1 the runtime hands the driver a
// `DXGI1_2_DDI_BASE_FUNCTIONS` instead, which has 15. The union member in `DXGI_DDI_BASE_ARGS` is the same
// pointer, so the hosted driver still writes slots 0 to 6 of the runtime's bigger struct in place, and the
// front fills the rest.
//
// Four of the remaining eight are the front's obligation at build version 0:
//   pfnResolveSharedResource   arrived at DXGI1_1. The Route C report named three entries and left this
//                              one out; a missing entry is a call into address zero inside dwm.exe.
//   pfnBlt1                    a blt with a source rectangle, which the D3D10.0 pfnBlt has no field for.
//   pfnOfferResources          resource offering and reclaiming, which arrived with DXGI1_2.
//   pfnReclaimResources
//
// The last four are the multiplane-overlay entries, and `IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS` needs a
// build version above 0 (d3d10umddi.h:8010). The front offers `D3D11_1_DDI_SUPPORTED` exactly, whose build
// version is 0, so the operating system has no reason to call them. They are filled all the same, with a
// body that refuses and says so once: an entry nobody should call is the entry that costs a crash in
// address zero when the assumption is wrong, and the whole point of the completeness assert is that no
// slot is left null.

#include "front-adapter.h"
#include <cstdio>

namespace bc250front {
namespace {

// The front has no device record on this path: the DXGI entries carry a DXGI_DDI_HDEVICE, which is the
// runtime's own handle and not the D3D device handle the front publishes. Every one of these bodies
// therefore logs through the process-wide channel only.
#define FRONT_DXGI_REFUSE_ONCE(name)                  \
    do {                                              \
        static volatile LONG once = 0;                \
        Refuse(nullptr, name, &once);                 \
    } while (0)

// The hosted pfnBlt, kept per device by the front's CreateDevice. One hosted driver per process and one
// blt entry in it, so a single published pointer is the whole mapping; a second hosted driver in one
// process would be a different route, which the router does not create.
typedef HRESULT(APIENTRY *HostedBltFn)(DXGI_DDI_ARG_BLT *);
HostedBltFn HostedBlt;

HRESULT APIENTRY ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *args)
{
    // The hosted frontend does not separate a shared surface's "resolved" state from its normal one: a
    // shared surface is a gallium resource the moment it is opened, and nothing is deferred to a resolve.
    // S_OK is therefore the honest answer and not a silent refusal. A driver that later keeps a resolve
    // step must fill this entry from its own device.
    (void)args;
    return S_OK;
}

HRESULT APIENTRY Blt1(DXGI_DDI_ARG_BLT1 *args)
{
    if (!args) return E_INVALIDARG;
    if (!HostedBlt) {
        FRONT_DXGI_REFUSE_ONCE("dxgi pfnBlt1 (no hosted pfnBlt)");
        return E_NOTIMPL;
    }
    // The D3D10.0 pfnBlt takes a destination rectangle and the whole source subresource. A source
    // rectangle that is not the whole surface, or a rotation, cannot be expressed: refuse rather than
    // scale the wrong pixels onto the screen. Identity rotation with no source rectangle is the shape
    // DXGI uses for a plain present blt, and that one forwards.
    if (args->Rotate != DXGI_DDI_MODE_ROTATION_IDENTITY) {
        FRONT_DXGI_REFUSE_ONCE("dxgi pfnBlt1 with a rotation");
        return E_NOTIMPL;
    }
    if (args->SrcLeft || args->SrcTop || args->SrcRight || args->SrcBottom) {
        FRONT_DXGI_REFUSE_ONCE("dxgi pfnBlt1 with a source rectangle");
        return E_NOTIMPL;
    }
    DXGI_DDI_ARG_BLT blt;
    blt.hDevice = args->hDevice;
    blt.hDstResource = args->hDstResource;
    blt.DstSubresource = args->DstSubresource;
    blt.DstLeft = args->DstLeft;
    blt.DstTop = args->DstTop;
    blt.DstRight = args->DstRight;
    blt.DstBottom = args->DstBottom;
    blt.hSrcResource = args->hSrcResource;
    blt.SrcSubresource = args->SrcSubresource;
    blt.Flags = args->Flags;
    blt.Rotate = args->Rotate;
    return HostedBlt(&blt);
}

// Offering a resource says its contents may be discarded while the application is not using it. The
// hosted frontend has no discardable residency state and nothing below it can drop a surface's pages on
// request, so the honest answer is that nothing was offered and nothing was discarded. Reclaim therefore
// reports every resource as intact, which is what a driver with no offer support must report: a TRUE in
// pDiscarded would make the application throw away contents that are still there.
HRESULT APIENTRY OfferResources(DXGI_DDI_ARG_OFFERRESOURCES *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnOfferResources");
    return S_OK;
}

HRESULT APIENTRY ReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *args)
{
    if (!args) return E_INVALIDARG;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnReclaimResources");
    if (args->pDiscarded)
        for (UINT i = 0; i < args->Resources; ++i) args->pDiscarded[i] = FALSE;
    return S_OK;
}

// The four multiplane-overlay entries. Build version 0 excludes them; these bodies exist so that no slot
// is null and so that a wrong assumption is a line in the log.
HRESULT APIENTRY GetMultiplaneOverlayCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnGetMultiplaneOverlayCaps at build version 0");
    return E_NOTIMPL;
}

HRESULT APIENTRY GetMultiplaneOverlayFilterRange(void *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnGetMultiplaneOverlayFilterRange at build version 0");
    return E_NOTIMPL;
}

HRESULT APIENTRY CheckMultiplaneOverlaySupport(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYSUPPORT *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnCheckMultiplaneOverlaySupport at build version 0");
    return E_NOTIMPL;
}

HRESULT APIENTRY PresentMultiplaneOverlay(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnPresentMultiplaneOverlay at build version 0");
    return E_NOTIMPL;
}

#undef FRONT_DXGI_REFUSE_ONCE

}  // namespace

void FillDxgiFuncs(DXGI1_2_DDI_BASE_FUNCTIONS *out, const DXGI_DDI_BASE_FUNCTIONS &hosted)
{
    if (!out) return;
    HostedBlt = hosted.pfnBlt;
    // 0 .. 6: the hosted driver's own entries, which it wrote into the first seven slots of this very
    // struct through the union member. They are copied here all the same, so that FillDxgiFuncs is a
    // complete function of its input and the host gate can drive it with a table of its own.
    out->pfnPresent = hosted.pfnPresent;
    out->pfnGetGammaCaps = hosted.pfnGetGammaCaps;
    out->pfnSetDisplayMode = hosted.pfnSetDisplayMode;
    out->pfnSetResourcePriority = hosted.pfnSetResourcePriority;
    out->pfnQueryResourceResidency = hosted.pfnQueryResourceResidency;
    out->pfnRotateResourceIdentities = hosted.pfnRotateResourceIdentities;
    out->pfnBlt = hosted.pfnBlt;
    // 7 .. 10: the four the front owes at DXGI1_2 build version 0.
    out->pfnResolveSharedResource = ResolveSharedResource;
    out->pfnBlt1 = Blt1;
    out->pfnOfferResources = OfferResources;
    out->pfnReclaimResources = ReclaimResources;
    // 11 .. 14: multiplane overlay, excluded by the build version and filled anyway.
    out->pfnGetMultiplaneOverlayCaps = GetMultiplaneOverlayCaps;
    out->pfnGetMultiplaneOverlayFilterRange = GetMultiplaneOverlayFilterRange;
    out->pfnCheckMultiplaneOverlaySupport = CheckMultiplaneOverlaySupport;
    out->pfnPresentMultiplaneOverlay = PresentMultiplaneOverlay;
}

}  // namespace bc250front
