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
//   pfnResolveSharedResource   arrived at DXGI1_1, so a hosted driver whose table shape came out of
//                              IS_DXGI1_1_BASE_FUNCTIONS may have filled it already (the macro reads the
//                              runtime's `Version` low half, which the front does not control). The front
//                              keeps a non-null entry and installs its own only where the slot is empty; a
//                              missing entry is a call into address zero inside dwm.exe.
//   pfnBlt1                    a blt with a source rectangle, which the D3D10.0 pfnBlt has no field for.
//   pfnOfferResources          resource offering and reclaiming, which arrived with DXGI1_2.
//   pfnReclaimResources
//
// The last four are the multiplane-overlay entries. They are NOT excluded by the build version: the second
// clause of `IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS` (d3d10umddi.h:8012) is `major == 11 && minor >
// D3D11_1_DDI_MINOR_VERSION_RC`, which is 15 > 14 for `D3D11_1_DDI_INTERFACE_VERSION` whatever the build
// version is - our own DXVK shell relies on exactly that (driver/umd/dxvk/ddi-device-create.cpp:59). What
// keeps the operating system off them is the kernel driver, which implements no multiplane-overlay DDI at
// all (no DxgkDdiGetMultiPlaneOverlayCaps), so DXGI has nothing to build an overlay plan from. They answer
// DXGI_ERROR_UNSUPPORTED, the same refusal our own UMD gives in make_dxgi_device_table(), and say so once:
// an entry nobody should call is the entry that costs a crash in address zero when the assumption is wrong,
// and the whole point of the completeness assert is that no slot is left null.

#include "front-adapter.h"
#include <cstdio>

namespace bc250front {
namespace {

// The front has no device record on this path: the DXGI entries carry a DXGI_DDI_HDEVICE, which is the
// runtime's own handle and not the D3D device handle the front publishes. Every one of these bodies
// therefore logs through the process-wide channel only.
// Refuse() and Dropped() fall back to the front's process-wide log when there is no device record, so these
// lines do reach the file the trial reads (front-adapter.cpp).
#define FRONT_DXGI_REFUSE_ONCE(name)                  \
    do {                                              \
        static volatile LONG once = 0;                \
        Refuse(nullptr, name, &once);                 \
    } while (0)

#define FRONT_DXGI_DROPPED_ONCE(name, what)           \
    do {                                              \
        static volatile LONG once = 0;                \
        Dropped(nullptr, name, what, &once);          \
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

// Installing the front is what moves the presentation blt onto this entry: at the D3D10.0 DDI the runtime
// holds a 7-entry DXGI table with pfnBlt alone, and at D3D11_1 it holds 15 and prefers pfnBlt1 for a
// stretch, a convert or a resolve present. So this entry is on the desktop's present path, and it never
// refuses: a failed present blt is a device error, and in dwm.exe that is the crash loop arm 1a looks for.
//
// What the older entry can express: a destination rectangle and the WHOLE source subresource
// (ref/ddi-display/dxgiddi.md:1065, "Blt1DXGI always uses a whole source subresource"), with its own
// Rotate field (dxgiddi.h:256), which is why a rotation needs no clause at all. A source rectangle is "a
// dirty subrectangle" (dxgiddi.md:1583) and the whole-surface case is (0, 0, width, height), not four
// zeros - the all-zero rect is the degenerate spelling of the same thing. An unstretched copy from the
// source origin is therefore exact; any other shape is named once and forwarded all the same, because the
// wrong pixels in one blt are recoverable and a dead compositor is not.
HRESULT APIENTRY Blt1(DXGI_DDI_ARG_BLT1 *args)
{
    if (!args) return E_INVALIDARG;
    if (!HostedBlt) {
        FRONT_DXGI_REFUSE_ONCE("dxgi pfnBlt1 (no hosted pfnBlt)");
        return E_NOTIMPL;
    }
    const UINT src_width = args->SrcRight > args->SrcLeft ? args->SrcRight - args->SrcLeft : 0;
    const UINT src_height = args->SrcBottom > args->SrcTop ? args->SrcBottom - args->SrcTop : 0;
    const UINT dst_width = args->DstRight > args->DstLeft ? args->DstRight - args->DstLeft : 0;
    const UINT dst_height = args->DstBottom > args->DstTop ? args->DstBottom - args->DstTop : 0;
    const bool whole_source = !args->SrcLeft && !args->SrcTop &&
                              ((!src_width && !src_height) ||
                               (src_width == dst_width && src_height == dst_height));
    if (!whole_source)
        FRONT_DXGI_DROPPED_ONCE("dxgi pfnBlt1",
                                "a source rectangle the D3D10.0 pfnBlt cannot name: the whole source "
                                "subresource is blitted into the destination rectangle instead");
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

// The four multiplane-overlay entries. The build version does not exclude them (the note at the head of this
// file): what keeps the operating system off them is that the kernel driver implements no overlay DDI, so
// DXGI has nothing to plan an overlay from. DXGI_ERROR_UNSUPPORTED is what our own UMD answers for the same
// entries (driver/umd/dxvk/ddi-dxgi-table.h), and it is the one answer a caller can tell from "this driver
// is broken". These bodies exist so that no slot is null and so that a wrong assumption is a line in the log.
HRESULT APIENTRY GetMultiplaneOverlayCaps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnGetMultiplaneOverlayCaps: the kernel driver has no overlay DDI");
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT APIENTRY GetMultiplaneOverlayFilterRange(void *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnGetMultiplaneOverlayFilterRange: the kernel driver has no overlay DDI");
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT APIENTRY CheckMultiplaneOverlaySupport(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYSUPPORT *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnCheckMultiplaneOverlaySupport: the kernel driver has no overlay DDI");
    return DXGI_ERROR_UNSUPPORTED;
}

HRESULT APIENTRY PresentMultiplaneOverlay(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *args)
{
    (void)args;
    FRONT_DXGI_REFUSE_ONCE("dxgi pfnPresentMultiplaneOverlay: the kernel driver has no overlay DDI");
    return DXGI_ERROR_UNSUPPORTED;
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
    // 7 .. 10: the four the front owes at DXGI1_2. Slot 7 arrived at DXGI1_1, so the hosted driver may have
    // filled it already through the union member; whether it did depends on IS_DXGI1_1_BASE_FUNCTIONS over
    // the runtime's own `Version`, which the front neither sets nor can predict. A non-null slot is the
    // hosted driver's own entry and is kept; only an empty one gets the front's stub. This is what makes
    // FillDxgiFuncs a function of the output's prior slot 7 as well as of the hosted table.
    if (!out->pfnResolveSharedResource) out->pfnResolveSharedResource = ResolveSharedResource;
    out->pfnBlt1 = Blt1;
    out->pfnOfferResources = OfferResources;
    out->pfnReclaimResources = ReclaimResources;
    // 11 .. 14: multiplane overlay, reachable at this interface version and answered DXGI_ERROR_UNSUPPORTED.
    out->pfnGetMultiplaneOverlayCaps = GetMultiplaneOverlayCaps;
    out->pfnGetMultiplaneOverlayFilterRange = GetMultiplaneOverlayFilterRange;
    out->pfnCheckMultiplaneOverlaySupport = CheckMultiplaneOverlaySupport;
    out->pfnPresentMultiplaneOverlay = PresentMultiplaneOverlay;
}

}  // namespace bc250front
