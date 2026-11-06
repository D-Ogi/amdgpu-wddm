/* mpo3_track.h - BD-307: plane 0 of the WDDM 2.1 multiplane-overlay DDIs, without kernel calls.
 *
 * From WDDMVersion 2.1 the graphics kernel can send a flip of plane 0 through DxgkDdiSetVidPnSourceAddressWithMultiPlane-
 * Overlay3 in place of DxgkDdiSetVidPnSourceAddress, also when DRIVERCAPS reports no multiplane overlay: the 2.9 image of
 * 2026-11-05 got the disable-planes flip of CddDisable that way, and the NULL table member was bugcheck 0x139/0xA. Such
 * a flip is retired by its PresentId, not by its address:
 *   - d3dkmddi.h 10.0.26100 line 780: DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO2 { LayerIndex, PresentId, Flags }, the payload
 *     of DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2 (line 709, union member line 1090);
 *   - windows-driver-docs hardware-flip-queue.md:242: that report carries "a single last completed PresentId for each
 *     plane" (the type 18 report of the same page is the hardware flip queue's, which this driver does not implement);
 *   - d3dkmddi.h DXGK_PLANE_SPECIFIC_OUTPUT_FLAGS.HsyncInterruptCompletion: only with that flag is an immediate flip not
 *     complete "upon the return from the DDI"; the docs page of the flag says that the OS completes a pending flip
 *     when a report has "the PresentId greater or equal to pending".
 * So a report names the newest PresentId whose surface plane 0 shows. A flip through SetVidPnSourceAddress ends the
 * mode: the report is CRTC_VSYNC with the address again.
 *
 * The tracker functions run under the interrupt lock L (wddm.c: WddmFlipLocked, WddmMpo3KeepLocked and
 * WddmMpo3Notify). The plane rule runs at PASSIVE_LEVEL in Check and at any level in the flip. Nothing here reads a
 * register. test/mpo3_track_test.c drives every rule, and test/run_mpo3_track.ps1 breaks each one in a copy.
 */
#ifndef BC250_MPO3_TRACK_H
#define BC250_MPO3_TRACK_H

/* ---- the PresentId of plane 0 ---------------------------------------------------------------------------------- */

typedef struct _BC250_MPO3_TRACK {
    unsigned Active;                    /* the last accepted request for plane 0 came through the MPO3 DDI */
    unsigned PendingValid;              /* a next-VSync request waits for the report that shows PendingAddress */
    long long PendingAddress;           /* the card address that request published (wddm.c PrimaryAddress) */
    unsigned long long PendingId;
    unsigned CurrentValid;              /* CurrentId is the newest PresentId that plane 0 shows */
    unsigned long long CurrentId;
} BC250_MPO3_TRACK;

/* A request with no plane (PlaneCount 0): the mode is MPO3 from now on, and there is no PresentId to report. */
static __inline void Bc250Mpo3Enter(BC250_MPO3_TRACK* T)
{
    T->Active = 1u;
}

/* A flip of plane 0 that the flip transaction accepted. Address is what it published. An immediate one is complete
 * on the return of the DDI, so its id is current at once and supersedes a pending one. */
static __inline void Bc250Mpo3Flipped(BC250_MPO3_TRACK* T, long long Address, unsigned long long PresentId,
                                      unsigned Immediate)
{
    T->Active = 1u;
    if (Immediate) {
        T->PendingValid = 0u;
        T->CurrentValid = 1u;
        T->CurrentId = PresentId;
    } else {
        T->PendingValid = 1u;
        T->PendingAddress = Address;
        T->PendingId = PresentId;
    }
}

/* A request for plane 0 that changes no surface: the plane disabled (the disable-planes flip), or enabled with no
 * context data. The plane keeps what it shows, and wddm.c answers the request as complete on the return of the DDI
 * (FlipConvertedToImmediate). With a flip pending, the id waits for that flip's surface: a report of it now would
 * also retire the pending flip ("greater or equal"), and the OS would release the buffer that the plane still reads.
 * The id of the later request is the one kept (the order of the calls, not the size of the number). */
static __inline void Bc250Mpo3Kept(BC250_MPO3_TRACK* T, unsigned long long PresentId)
{
    T->Active = 1u;
    if (T->PendingValid) {
        T->PendingId = PresentId;
        return;
    }
    T->CurrentValid = 1u;
    T->CurrentId = PresentId;
}

/* A flip through SetVidPnSourceAddress (or the lab flavour's MPO2 DDI): the address report is the contract again. */
static __inline void Bc250Mpo3Legacy(BC250_MPO3_TRACK* T)
{
    T->Active = 0u;
    T->PendingValid = 0u;
    T->CurrentValid = 0u;
}

/* The report of one VSync. Shown is the address the CRTC_VSYNC report would carry (wddm.c WddmReadCompletedPrimary).
 * Returns 0: report CRTC_VSYNC as before. Returns 1: report CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2 with *Count planes
 * (0 or 1); for 1, layer 0 with *PresentId. A pending request whose surface is shown becomes current. A report never
 * names a PresentId whose surface the plane has not reached. */
static __inline unsigned Bc250Mpo3Report(BC250_MPO3_TRACK* T, long long Shown, unsigned* Count,
                                         unsigned long long* PresentId)
{
    *Count = 0u;
    *PresentId = 0u;
    if (!T->Active) return 0u;
    if (T->PendingValid && T->PendingAddress == Shown) {
        T->PendingValid = 0u;
        T->CurrentValid = 1u;
        T->CurrentId = T->PendingId;
    }
    if (T->CurrentValid) {
        *Count = 1u;
        *PresentId = T->CurrentId;
    }
    return 1u;
}

/* ---- the plane this adapter scans out through the MPO3 DDIs ------------------------------------------------------ */

/* The verdicts, in the order Bc250Mpo3PlaneVerdict tests them. wddm.c adds SCANOUT (the allocation, by scanout_admit.h)
 * and GATE (the flip gate closed, EnableVidPnFlip) after the geometry, and counts every answer by verdict. SCALED_RULE is
 * the scaled-plane rule of dcn_plane.c (BD-223): a plane 0 that only its geometry kept from OK, refused by that rule
 * (Bc250PlanePrepare or the DML step); wddm.c counts that rule's own verdict too. Append only: the summary lines name
 * the verdicts by their text. */
enum {
    BC250_MPO3_OK = 0,
    BC250_MPO3_COUNT,                   /* not exactly one plane */
    BC250_MPO3_SOURCE,                  /* a source other than 0 */
    BC250_MPO3_LAYER,                   /* a layer other than 0 */
    BC250_MPO3_GEOMETRY,                    /* source, destination or clip not the whole source mode */
    BC250_MPO3_ROTATION,                /* not D3DDDI_ROTATION_IDENTITY */
    BC250_MPO3_FLAGS,                   /* a mirror flag, or a flag bit this rule does not know */
    BC250_MPO3_BLEND,                   /* alpha blend */
    BC250_MPO3_COLOR,                   /* not D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 */
    BC250_MPO3_POST,                    /* a post-composition that is not the identity */
    BC250_MPO3_SCANOUT,
    BC250_MPO3_GATE,
    BC250_MPO3_SCALED_RULE,             /* the scaled-plane rule refused a plane 0 of another geometry */
    BC250_MPO3_VERDICTS
};

/* wddm.c asserts both against d3dukmdt.h. */
#define BC250_MPO3_ROTATION_IDENTITY 1u
#define BC250_MPO3_COLOR_SRGB 0u

typedef struct _BC250_MPO3_RECT { long Left, Top, Right, Bottom; } BC250_MPO3_RECT;

/* One plane of a Check or a flip, and the source mode it is held against. Post* describe the post-composition
 * (PostPresent 0: none). */
typedef struct _BC250_MPO3_PLANE {
    unsigned PlaneCount, SourceId, LayerIndex;
    BC250_MPO3_RECT Src, Dst, Clip;
    unsigned Rotation, Flags, AlphaBlend, ColorSpace;
    unsigned PostPresent, PostFlags, PostRotation;
    BC250_MPO3_RECT PostSrc, PostDst;
    unsigned long ModeWidth, ModeHeight;
} BC250_MPO3_PLANE;

static __inline unsigned Bc250Mpo3WholeMode(const BC250_MPO3_RECT* R, unsigned long Width, unsigned long Height)
{
    return R->Left == 0 && R->Top == 0 && R->Right == (long)Width && R->Bottom == (long)Height;
}

/* The rule of one plane 0. Scaled 0: the full-size flip of plane 0 and nothing else, what SetVidPnSourceAddress
 * programs. Scaled 1: the same rule without the geometry test, for a plane 0 whose rectangles the scaled-plane rule of
 * dcn_plane.c decides (Bc250PlanePrepare: the whole allocation, inside the source mode, the scaler limits). Every other
 * test stays: one plane, source 0, layer 0, identity rotation, no flag, opaque, sRGB, an identity post-composition. */
static __inline unsigned Bc250Mpo3PlaneRule(const BC250_MPO3_PLANE* P, unsigned Scaled)
{
    if (P->PlaneCount != 1u) return BC250_MPO3_COUNT;
    if (P->SourceId != 0u) return BC250_MPO3_SOURCE;
    if (P->LayerIndex != 0u) return BC250_MPO3_LAYER;
    if (!Scaled &&
        (!Bc250Mpo3WholeMode(&P->Src, P->ModeWidth, P->ModeHeight) ||
         !Bc250Mpo3WholeMode(&P->Dst, P->ModeWidth, P->ModeHeight) ||
         !Bc250Mpo3WholeMode(&P->Clip, P->ModeWidth, P->ModeHeight)))
        return BC250_MPO3_GEOMETRY;
    if (P->Rotation != BC250_MPO3_ROTATION_IDENTITY) return BC250_MPO3_ROTATION;
    if (P->Flags != 0u) return BC250_MPO3_FLAGS;
    if (P->AlphaBlend) return BC250_MPO3_BLEND;
    if (P->ColorSpace != BC250_MPO3_COLOR_SRGB) return BC250_MPO3_COLOR;
    if (P->PostPresent &&
        (P->PostFlags != 0u || P->PostRotation != BC250_MPO3_ROTATION_IDENTITY ||
         !Bc250Mpo3WholeMode(&P->PostSrc, P->ModeWidth, P->ModeHeight) ||
         !Bc250Mpo3WholeMode(&P->PostDst, P->ModeWidth, P->ModeHeight)))
        return BC250_MPO3_POST;
    return BC250_MPO3_OK;
}

/* The full-size rule (Scaled 0). */
static __inline unsigned Bc250Mpo3PlaneVerdict(const BC250_MPO3_PLANE* P)
{
    return Bc250Mpo3PlaneRule(P, 0u);
}

static __inline const char* Bc250Mpo3VerdictText(unsigned V)
{
    static const char* const text[BC250_MPO3_VERDICTS] = {
        "ok", "count", "source", "layer", "geometry", "rotation", "flags", "blend", "color", "post", "scanout", "gate",
        "scaled-rule"
    };
    return V < BC250_MPO3_VERDICTS ? text[V] : "?";
}

#endif /* BC250_MPO3_TRACK_H */
