// redirblt_policy.h - the decisions of redirblt-probe as pure functions, so that a host-only test can pin
// them without a window, a DWM or a lab (review 236). No Windows calls, no state: every function maps its
// inputs to one decision and a reason string the transcript prints.
#ifndef REDIRBLT_POLICY_H
#define REDIRBLT_POLICY_H

#include <windows.h>

// What the DWM said in the handshake, as far as the probe's decisions care.
#define REDIRBLT_HR_S_OK 0x00000000L
#define REDIRBLT_HR_GDI_SURFACE 0x00263005L             // DWM_S_GDI_REDIRECTION_SURFACE
#define REDIRBLT_HR_BLT_VIA_GDI 0x00263008L             // DWM_S_GDI_REDIRECTION_SURFACE_BLT_VIA_GDI

typedef enum _REDIRBLT_HANDLE_KIND {
    HandleUnknown = 0,                  // never classified: not ours to close
    HandleGlobal,                       // a D3DKMT global share handle: not a handle of this process
    HandleNt                            // an NT handle in this process: ours to close, once
} REDIRBLT_HANDLE_KIND;

typedef enum _REDIRBLT_VARIANT { VariantV0 = 0, VariantA, VariantB, VariantC, VariantD, VariantCount } REDIRBLT_VARIANT;

typedef enum _REDIRBLT_WAIT_ACTION {
    WaitProceed = 0,                    // the worker returned: use its result
    WaitTerminate                       // it did not: print, flush, terminate without teardown (exit 5)
} REDIRBLT_WAIT_ACTION;

// 1. After WaitForSingleObject on the handshake worker. Only WAIT_OBJECT_0 lets the run go on; a timeout and
//    a failed wait both end the process without teardown, because the worker still holds the window, the
//    dwmapi module and the probe block, and the watchdog is cancelled on a normal exit.
static __inline REDIRBLT_WAIT_ACTION RedirbltAfterWait(DWORD WaitResult, DWORD LastError, const char** Reason)
{
    if (WaitResult == WAIT_OBJECT_0) { *Reason = "worker returned"; return WaitProceed; }
    if (WaitResult == WAIT_TIMEOUT) { *Reason = "worker did not return within the deadline"; return WaitTerminate; }
    if (WaitResult == WAIT_FAILED) { *Reason = LastError != 0 ? "WaitForSingleObject failed (see error)" : "WaitForSingleObject failed"; return WaitTerminate; }
    *Reason = "unexpected wait result";
    return WaitTerminate;
}

// 2. DwmDxUpdateWindowSharedSurface (ordinal 101) belongs to the S_OK branch only (dedicated DX surface,
//    D3DKMTRender route). Never after DWM_S_GDI_REDIRECTION_SURFACE, whatever a present variant returned.
static __inline BOOL RedirbltUpdateAllowed(BOOL HandshakeCompleted, HRESULT HandshakeHr, const char** Reason)
{
    if (!HandshakeCompleted) { *Reason = "no completed handshake"; return FALSE; }
    if (HandshakeHr != REDIRBLT_HR_S_OK) { *Reason = "ordinal 101 is documented only after S_OK"; return FALSE; }
    *Reason = "S_OK handshake";
    return TRUE;
}

// 3. A destination is ready for B/C/D only when everything about it checked out. Any unknown is a rejection:
//    a wrong-adapter surface, a blob that is not the KMD's, a map or residency that failed.
static __inline BOOL RedirbltDestinationReady(BOOL LuidKnown, BOOL LuidMatches, BOOL BlobIsLb7a, BOOL Mapped,
                                              BOOL Resident, const char** Reason)
{
    if (!LuidKnown) { *Reason = "adapter of the shared surface unknown"; return FALSE; }
    if (!LuidMatches) { *Reason = "shared surface lives on another adapter"; return FALSE; }
    if (!BlobIsLb7a) { *Reason = "allocation private data is not the KMD's LB7A block"; return FALSE; }
    if (!Mapped) { *Reason = "GPU virtual address map failed"; return FALSE; }
    if (!Resident) { *Reason = "residency failed"; return FALSE; }
    *Reason = "destination ready";
    return TRUE;
}

// 4. Which variants may run, given the latest handshake and the destination state. V0 needs nothing. A needs
//    DWM_S_GDI_REDIRECTION_SURFACE. B, C, D need that and a ready destination opened from the same handshake
//    handle; --no-open is a diagnostic that skips them, never a pass with hDestination 0.
static __inline BOOL RedirbltVariantAllowed(REDIRBLT_VARIANT Variant, BOOL HandshakeCompleted, HRESULT HandshakeHr,
                                            BOOL NoOpen, BOOL DestinationReady, BOOL DestinationFromThisHandle,
                                            const char** Reason)
{
    if (Variant == VariantV0) { *Reason = "no prerequisite"; return TRUE; }
    if (!HandshakeCompleted) { *Reason = "no completed handshake"; return FALSE; }
    if (HandshakeHr != REDIRBLT_HR_GDI_SURFACE) { *Reason = "handshake did not return DWM_S_GDI_REDIRECTION_SURFACE"; return FALSE; }
    if (Variant == VariantA) { *Reason = "GDI redirection surface offered"; return TRUE; }
    if (NoOpen) { *Reason = "--no-open: no destination by choice"; return FALSE; }
    if (!DestinationReady) { *Reason = "destination not ready"; return FALSE; }
    if (!DestinationFromThisHandle) { *Reason = "destination not bound to this handshake's handle and update id"; return FALSE; }
    *Reason = "destination ready from this handshake";
    return TRUE;
}

// 5. What to do with the opened destination after a fresh handshake. The surface identity is the handle: a
//    different handle means a stale destination (close and reopen, never mix the new update id with the old
//    surface). The same handle with a new update id is the same validated surface with a new token: bind the
//    new id to it, no reopen (review 238). The same handle and id: nothing to do.
typedef enum _REDIRBLT_DESTINATION_ACTION {
    DestinationKeep = 0,                // same handle, same update id
    DestinationRebind,                  // same handle, new update id: adopt the id, keep the opened surface
    DestinationReopen                   // nothing opened, or a different handle
} REDIRBLT_DESTINATION_ACTION;

static __inline REDIRBLT_DESTINATION_ACTION RedirbltDestinationAction(BOOL HaveOpened, HANDLE OpenedFrom, UINT64 OpenedUpdateId,
                                                                      HANDLE Current, UINT64 CurrentUpdateId, const char** Reason)
{
    if (!HaveOpened) { *Reason = "nothing opened yet"; return DestinationReopen; }
    if (OpenedFrom != Current) { *Reason = "handshake handle changed"; return DestinationReopen; }
    if (OpenedUpdateId != CurrentUpdateId) { *Reason = "same surface, new update id"; return DestinationRebind; }
    *Reason = "same surface, same update id";
    return DestinationKeep;
}

// The destination is bound to the current handshake when it is owned, opened from this very handle and carries
// this very update id (after a rebind, it does).
static __inline BOOL RedirbltDestinationBound(BOOL HaveOpened, HANDLE OpenedFrom, UINT64 OpenedUpdateId,
                                              HANDLE Current, UINT64 CurrentUpdateId)
{
    return HaveOpened && OpenedFrom == Current && OpenedUpdateId == CurrentUpdateId;
}

// 6. Which handles the teardown closes: NT handles we classified, each once. Unknown and global never.
typedef struct _REDIRBLT_HANDLE_SET {
    HANDLE Handles[16];
    UINT Count;
} REDIRBLT_HANDLE_SET;

static __inline BOOL RedirbltHandleSetAdd(REDIRBLT_HANDLE_SET* Set, HANDLE Handle, REDIRBLT_HANDLE_KIND Kind)
{
    UINT i;

    if (Kind != HandleNt || Handle == NULL) return FALSE;
    for (i = 0; i < Set->Count; i++) if (Set->Handles[i] == Handle) return FALSE;    // already owned, no double close
    if (Set->Count >= ARRAYSIZE(Set->Handles)) return FALSE;
    Set->Handles[Set->Count++] = Handle;
    return TRUE;
}

// 7. Which calls --handshake-only needs: a device (and paging queue) only when the surface is to be opened.
static __inline BOOL RedirbltNeedDevice(BOOL HandshakeOnly, BOOL NoOpen)
{
    return !(HandshakeOnly && NoOpen);
}

static __inline BOOL RedirbltNeedSourceAndContext(BOOL HandshakeOnly)
{
    return !HandshakeOnly;
}

#endif
