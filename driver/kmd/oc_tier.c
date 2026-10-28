// The overclock tier of this start (include/bc250_oc.h, docs/design/dpm.md "Overclock tiers"). The policy is
// driver/shim/bc250_oc.c and is host-tested; this file reads the two settings the control application writes after
// its questionnaire, persists the reset of a tier that belongs to another board, and latches the result for the
// clock owner, the governor and the CPU surface. It sends no SMU message.
//
// Settings, all REG_DWORD under Services\bc250kmd\Parameters:
//   OcTier            0 none (absent is the same), 1 air cooling, 2 all-in-one liquid cooling
//   OcTierBoard       the board the tier was chosen on: bc250_oc_board_fingerprint, as the board capability query
//                     reports it to the application
//   OcTierLastReason  what this start did with them (enum bc250_oc_reason)
// It also reads DpmSession (dpm.c), before DpmStart consumes it: a start that finds it knows that the last one went
// down above the floor without a clean stop, and a stored tier goes (BC250_OC_REASON_UNCLEAN_STOP).
#include "bc250kmd.h"
#include "bc250_oc.h"

#define OC_SETTING_TIER L"OcTier"
#define OC_SETTING_BOARD L"OcTierBoard"
#define OC_SETTING_REASON L"OcTierLastReason"
#define OC_SETTING_DPM_SESSION L"DpmSession"

C_ASSERT(BC250_OC_TIER_COUNT == 3);
C_ASSERT(BC250_OC_REASON_COUNT == 7);

static BOOLEAN OcQueryPresent(PCWSTR Name, unsigned int* Value)
{
    ULONG value = 0;
    NTSTATUS status = GuardQuerySetting(Name, &value);
    *Value = NT_SUCCESS(status) ? value : 0;
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND)
        GuardLog("oc: reading %ws failed 0x%08X, the tier none runs", Name, status);
    return NT_SUCCESS(status);
}

// The board this start runs on, 0 when it is not a positively identified BC-250 with a qualified firmware identity.
ULONG OcTierCurrentBoard(BC250_DEVICE* Device)
{
    ULONG i, any = 0;
    if (!bc250_board_supported((unsigned)InterlockedCompareExchange(&Device->BoardMemoryProviderId, 0, 0)) ||
        Device->BoardMemoryBiosId == 0) return 0;
    for (i = 0; i < sizeof(Device->BoardMemoryMachineId); ++i) any |= Device->BoardMemoryMachineId[i];
    if (!any) return 0;
    return bc250_oc_board_fingerprint(Device->BoardMemoryMachineId, Device->BoardMemoryBiosId);
}

// StartDevice, after BoardProviderBind and before DpmStart and CpuStart, which read the latched tier. Never fails the
// start: every doubtful case runs the tier none. PASSIVE_LEVEL.
void OcTierStart(BC250_DEVICE* Device)
{
    struct bc250_oc_request r;
    struct bc250_oc_decision d;
    unsigned int session = 0;
    RtlZeroMemory(&r, sizeof(r));
    Device->OcBoard = OcTierCurrentBoard(Device);
    r.current_board = Device->OcBoard;
    r.supported = r.current_board != 0;
    r.tier_present = OcQueryPresent(OC_SETTING_TIER, &r.tier) ? 1 : 0;
    r.board_present = OcQueryPresent(OC_SETTING_BOARD, &r.board) ? 1 : 0;
    r.unclean_stop = OcQueryPresent(OC_SETTING_DPM_SESSION, &session) ? 1 : 0;
    bc250_oc_decide(&r, &d);
    if (d.clear) {
        // The tier first: a crash between the two deletes leaves a binding without a tier, which the next start
        // also deletes, and never a tier without its check.
        NTSTATUS a = GuardDeleteSetting(OC_SETTING_TIER), b = GuardDeleteSetting(OC_SETTING_BOARD);
        if (!NT_SUCCESS(a) || !NT_SUCCESS(b))
            GuardLog("oc: deleting the stored tier failed 0x%08X 0x%08X; the tier none runs anyway", a, b);
    }
    Device->OcStoredTier = (r.tier_present && !d.clear) ? r.tier : 0;
    Device->OcTier = d.tier;
    Device->OcReason = d.reason;
    InterlockedExchange(&Device->Smu.OcTier, (LONG)d.tier);
    if (r.tier_present || d.reason != BC250_OC_REASON_NONE) {
        NTSTATUS status = GuardStoreSetting(OC_SETTING_REASON, d.reason);
        if (!NT_SUCCESS(status)) GuardLog("oc: writing OcTierLastReason failed 0x%08X", status);
    }
    GuardLog("oc: stored tier %lu/%u board %08X/%u, this %08X, unclean %u -> tier %lu reason %lu deleted %u",
             r.tier, r.tier_present, r.board, r.board_present, r.current_board, r.unclean_stop, d.tier, d.reason,
             d.clear);
}
