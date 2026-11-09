// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// Included by wddm.c below the flip state machine block: the I/O half of the two-plane Check3 preflight (BD-223
// Part Y). The translation of the two WDDM planes (WddmMpo3GroupDescribe) is in that block, where the flip host
// test compiles it.
// The two-plane Check3 path consumes owned copies, never an allocation pointer held across connector/FPU work.
// A successful preflight still does not admit scan-out: clock and pipe transaction ownership and the Y0 hardware
// witness are pending.
static NTSTATUS WddmMpo3CheckGroup(BC250_DEVICE* Device, BC250_WDDM* Wddm,
    const DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3* Check,
    const DXGK_MULTIPLANE_OVERLAY_POST_COMPOSITION* Post, BC250_LINK_OVERLAY_CHECK* Out)
{
    HANDLE handles[2];
    BC250_MPO_STORAGE storage[2];
    BC250_OVERLAY_DESCRIPTION description;
    NTSTATUS status;

    RtlZeroMemory(Out, sizeof(*Out));
    if (Check->PlaneCount != 2 || !Check->ppPlanes || !Check->ppPlanes[0] || !Check->ppPlanes[1])
        return STATUS_INVALID_PARAMETER;
    handles[0] = Check->ppPlanes[0]->hAllocation;
    handles[1] = Check->ppPlanes[1]->hAllocation;
    if (!WddmSnapshotMpoStorage(Device, Wddm, handles, storage)) return STATUS_INVALID_PARAMETER;
    status = WddmMpo3GroupDescribe(Device, Check, Post, storage, &description);
    if (!NT_SUCCESS(status)) return status;
    return ConnectorLinkOverlayCheck(Device, &description, Out);
}
