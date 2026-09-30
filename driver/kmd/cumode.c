// CU mode: 24 compute units (stock) or 40, chosen by a service parameter, applied inside the GFX
// constants stage and reported to the UMD through the caps (docs/design/cu-mode.md).
//
// What lives where:
//   driver/shim/bc250_cu_mode.c   the policy (bc250_cu_decide) and the register sequence, host-tested
//   this file                     the registry around them, the boot guard's persistence, the hook's
//                                 installation, the snapshot for the escape, the caps patch
//
// Settings, all REG_DWORD under Services\bc250kmd\Parameters:
//   CuMode            24 or 40, absent = 24. Written by the cumode tool; the driver writes 24 on a fallback.
//   CuDisableWgp      optional with 40: WGPs that stay masked, bit sa * 5 + wgp, stock-inactive ones only
//   CuModePending     the encoded 40 request of a start nobody confirmed yet
//   CuModeConfirmed   the encoded 40 request a healthy start confirmed; a later start is not pending
//   CuModeLastApplied, CuModeLastReason   what the last start did, for the tool when the adapter is gone
// And a volatile subkey CuModeBoot: this boot's firmware stock of both registers, per shader array.
// It dies with the boot, which is the point: a warm device restart finds our own writes in the
// registers and must not take them for stock.
//
// The boot guard: a 40 request is marked pending, durably, before the first register write. Only a
// confirmation (start-health CONFIRM or BC250_CU_MODE_OP_CONFIRM on a READY adapter) clears the mark.
// A start that finds a mark applies 24 and writes CuMode = 24: whatever happened to the start that
// set it (bugcheck, hang, power cut, a reboot before anyone looked), 40 is not tried twice unasked.
// Mądry Polak po szkodzie - a Pole is wise after the damage; the driver tries to be wise before it.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "amdgpu.h"
#include "umd_caps.h"

#define CU_SETTING_MODE L"CuMode"
#define CU_SETTING_DISABLE L"CuDisableWgp"
#define CU_SETTING_PENDING L"CuModePending"
#define CU_SETTING_CONFIRMED L"CuModeConfirmed"
#define CU_SETTING_LAST_APPLIED L"CuModeLastApplied"
#define CU_SETTING_LAST_REASON L"CuModeLastReason"
#define CU_BOOT_KEY L"CuModeBoot"

C_ASSERT(sizeof(BC250_ESCAPE_CU_MODE) == 184);
C_ASSERT(BC250_SHADER_ENGINES * BC250_SH_PER_SE == BC250_CU_MODE_SA_COUNT);
C_ASSERT(BC250_CU_MODE_SA_COUNT <= BC250_CU_SA_MAX);

static const PCWSTR g_StockCcName[BC250_CU_SA_MAX] = { L"StockCc0", L"StockCc1", L"StockCc2", L"StockCc3" };
static const PCWSTR g_StockSpiName[BC250_CU_SA_MAX] = { L"StockSpi0", L"StockSpi1", L"StockSpi2", L"StockSpi3" };

static void CuLock(BC250_CU_MODE_STATE* S)
{
    KeWaitForSingleObject(&S->Lock, Executive, KernelMode, FALSE, NULL);
}
static void CuUnlock(BC250_CU_MODE_STATE* S)
{
    KeReleaseMutex(&S->Lock, FALSE);
}

void CuModeInitialize(BC250_DEVICE* Device)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    KeInitializeMutex(&s->Lock, 0);
    KeInitializeSpinLock(&s->SnapLock);
    s->Reason = BC250_CU_REASON_NOT_RUN;
}

// StartDevice, after StartHealthBegin: nothing of the previous start is reported any more.
void CuModeBegin(BC250_DEVICE* Device)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    KIRQL irql;
    CuLock(s);
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Valid = FALSE;
    s->Requested = 0;
    s->Applied = 0;
    s->Reason = BC250_CU_REASON_NOT_RUN;
    s->Runs = 0;
    RtlZeroMemory(&s->Snap, sizeof(s->Snap));
    RtlZeroMemory(&s->Info, sizeof(s->Info));
    s->Pending = FALSE;
    s->Confirmed = FALSE;
    s->StockRecord = FALSE;
    s->Generation = Device->StartHealth.Generation;
    KeReleaseSpinLock(&s->SnapLock, irql);
    CuUnlock(s);
}

// Runs inside the constants stage, under GartLock (APC_LEVEL) with the gfx sequence as the backend:
// no registry, no waits. A plan executes no write, so there is nothing to verify and nothing to report.
static void CuModeHook(struct amdgpu_device* adev, void* ctx,
                       void (*select)(struct amdgpu_device* adev, u32 se, u32 sh, u32 instance))
{
    BC250_CU_MODE_STATE* s = CONTAINING_RECORD(ctx, BC250_CU_MODE_STATE, Hw);
    BC250_SEQUENCE* sequence = (BC250_SEQUENCE*)adev->backend;
    KIRQL irql;
    if (sequence == NULL || sequence->Plan) return;
    bc250_cu_mode_apply(adev, &s->Hw, (bc250_cu_select_fn)select);
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Snap = s->Hw;
    ++s->Runs;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (s->Hw.reason != BC250_CU_REASON_NONE || s->Hw.wrote)
        GuardLog("cumode: stage run: mode %lu applied %lu reason %lu wrote %u", (ULONG)s->Hw.mode,
                 (ULONG)s->Hw.applied, (ULONG)s->Hw.reason, (ULONG)s->Hw.wrote);
}

static BOOLEAN QueryPresent(PCWSTR Name, unsigned int* Value)
{
    ULONG value = 0;
    NTSTATUS status = GuardQuerySetting(Name, &value);
    *Value = value;
    if (NT_SUCCESS(status)) return TRUE;
    *Value = 0;
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        GuardLog("cumode: reading %ws failed 0x%08X, treated as absent", Name, status);
    return FALSE;
}

static void StoreLogged(PCWSTR Name, ULONG Value)
{
    NTSTATUS status = GuardStoreSetting(Name, Value);
    if (!NT_SUCCESS(status)) GuardLog("cumode: writing %ws = %lu failed 0x%08X", Name, Value, status);
}

static void DeleteLogged(PCWSTR Name)
{
    NTSTATUS status = GuardDeleteSetting(Name);
    if (!NT_SUCCESS(status)) GuardLog("cumode: deleting %ws failed 0x%08X", Name, status);
}

// The automatic fallback: CuMode back to 24, durably, so that the next start does not try again.
static void PersistFallback(ULONG Reason)
{
    StoreLogged(CU_SETTING_MODE, BC250_CU_MODE_STOCK);
    DeleteLogged(CU_SETTING_CONFIRMED);
    DeleteLogged(CU_SETTING_PENDING);
    StoreLogged(CU_SETTING_LAST_REASON, Reason);
}

static BOOLEAN ReadStockRecord(struct bc250_cu_mode_hw* Hw)
{
    ULONG valid = 0, i;
    if (!NT_SUCCESS(GuardVolatileQuery(CU_BOOT_KEY, L"Valid", &valid)) || valid != 1) return FALSE;
    for (i = 0; i < BC250_CU_MODE_SA_COUNT; i++) {
        ULONG cc, spi;
        if (!NT_SUCCESS(GuardVolatileQuery(CU_BOOT_KEY, g_StockCcName[i], &cc)) ||
            !NT_SUCCESS(GuardVolatileQuery(CU_BOOT_KEY, g_StockSpiName[i], &spi))) return FALSE;
        Hw->stock_cc[i] = cc;
        Hw->stock_spi[i] = spi;
    }
    return TRUE;
}

static void StoreStockRecord(const struct bc250_cu_mode_hw* Hw)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    for (i = 0; i < BC250_CU_MODE_SA_COUNT && NT_SUCCESS(status); i++) {
        status = GuardVolatileStore(CU_BOOT_KEY, g_StockCcName[i], Hw->stock_cc[i]);
        if (NT_SUCCESS(status)) status = GuardVolatileStore(CU_BOOT_KEY, g_StockSpiName[i], Hw->stock_spi[i]);
    }
    // Valid last: a half-written record is no record.
    if (NT_SUCCESS(status)) status = GuardVolatileStore(CU_BOOT_KEY, L"Valid", 1);
    if (!NT_SUCCESS(status)) GuardLog("cumode: stock record not stored 0x%08X", status);
}

// PASSIVE_LEVEL, before GfxInitializeHardware: the decision, the durable pending mark, the hook.
void CuModePrepare(BC250_DEVICE* Device)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    struct bc250_cu_request* r = &s->Request;
    struct bc250_cu_decision* d = &s->Decision;
    struct amdgpu_device* adev = NULL;
    BOOLEAN enabled = FALSE;
    ULONG pci = 0, bytes = 0, requested;
    NTSTATUS status;
    KIRQL irql;

    CuLock(s);
    RtlZeroMemory(r, sizeof(*r));
    RtlZeroMemory(d, sizeof(*d));
    RtlZeroMemory(&s->Hw, sizeof(s->Hw));
    s->Installed = FALSE;
    s->Hw.max_cu_per_sh = BC250_MAX_CU_PER_SH;

    r->mode_present = QueryPresent(CU_SETTING_MODE, &r->mode);
    requested = r->mode_present ? r->mode : 0;
    status = Device->Dxgk.DxgkCbReadDeviceSpace(Device->Dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG, &pci, 0,
                                                sizeof(pci), &bytes);
    s->PciId = NT_SUCCESS(status) && bytes == sizeof(pci) ? pci : 0;
    if (s->PciId != BC250_PCI_ID) {
        // Not the part the register values were measured on: no hook, the static caps, nothing written.
        d->mode = BC250_CU_MODE_STOCK;
        d->reason = BC250_CU_REASON_NOT_THIS_DEVICE;
        GuardLog("cumode: PCI id 0x%08X is not 1002:13FE, CU mode left alone", s->PciId);
        goto Out;
    }

    r->disable_present = QueryPresent(CU_SETTING_DISABLE, &r->disable);
    (void)QueryPresent(CU_SETTING_PENDING, &r->pending);
    (void)QueryPresent(CU_SETTING_CONFIRMED, &r->confirmed);
    r->sa_count = BC250_SHADER_ENGINES * BC250_SH_PER_SE;
    r->wgps_per_sa = BC250_MAX_CU_PER_SH / 2;
    bc250_cu_decide(r, d);

    if (d->force_stock) {
        GuardLog("cumode: 40 CU request 0x%08X was pending from an earlier start that was never confirmed: "
                 "falling back to 24 and writing CuMode = 24", r->pending);
        PersistFallback(d->reason);
    } else if (d->clear_pending) DeleteLogged(CU_SETTING_PENDING);
    if (d->mark_pending) {
        // Durable before the first register write, or no 40 at all.
        status = GuardStoreSetting(CU_SETTING_PENDING, d->encoded);
        if (!NT_SUCCESS(status)) {
            GuardLog("cumode: pending mark not durable 0x%08X, applying 24", status);
            d->mode = BC250_CU_MODE_STOCK;
            d->disable = 0;
            d->reason = BC250_CU_REASON_REGISTRY;
            d->mark_pending = 0;
        } else s->Pending = TRUE;
    }
    s->Confirmed = d->confirmed && d->mode == BC250_CU_MODE_FULL;

    s->StockRecord = ReadStockRecord(&s->Hw);
    s->Hw.have_stock = s->StockRecord;
    s->Hw.mode = d->mode;
    s->Hw.disable = d->mode == BC250_CU_MODE_FULL ? d->disable : 0;

    ExAcquireFastMutex(&Device->GartLock);
    status = GartDevice(Device, &adev, &enabled);
    if (NT_SUCCESS(status) && adev != NULL) {
        adev->gfx.cu_mode_ctx = &s->Hw;
        adev->gfx.cu_mode_hook = CuModeHook;
        s->Installed = TRUE;
    }
    ExReleaseFastMutex(&Device->GartLock);
    if (!s->Installed) GuardLog("cumode: no GART device (0x%08X), the constants stage will run without the hook", status);

Out:
    GuardLog("cumode: CuMode %lu%s disable 0x%05X -> mode %lu reason %lu%s%s%s", requested,
             r->mode_present ? "" : " (absent)", r->disable, d->mode, d->reason,
             s->Pending ? ", pending" : "", s->Confirmed ? ", confirmed earlier" : "",
             s->StockRecord ? ", stock from this boot's record" : "");
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Requested = requested;
    s->Reason = d->reason != BC250_CU_REASON_NONE ? d->reason : BC250_CU_REASON_NOT_RUN;
    KeReleaseSpinLock(&s->SnapLock, irql);
    CuUnlock(s);
}

// PASSIVE_LEVEL, after GfxInitializeHardware, whether it succeeded or not: publish what the registers
// read back, keep this boot's stock, turn a failed 40 into a durable 24, arm the hook for resume.
void CuModeFinish(BC250_DEVICE* Device)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    struct bc250_cu_mode_hw hw;
    struct bc250_cu_info info;
    ULONG applied, reason;
    BOOLEAN valid;
    KIRQL irql;

    CuLock(s);
    RtlZeroMemory(&info, sizeof(info));
    if (s->Installed) {
        ExAcquireFastMutex(&Device->GartLock);
        hw = s->Hw;
        ExReleaseFastMutex(&Device->GartLock);
    } else RtlZeroMemory(&hw, sizeof(hw));

    valid = s->Installed && hw.ran;
    if (valid) {
        applied = hw.applied;
        reason = hw.reason != BC250_CU_REASON_NONE ? hw.reason : s->Decision.reason;
        bc250_cu_info_from_wgps(hw.active_wgps, hw.se_count, hw.sh_per_se, hw.max_cu_per_sh, &info);
        if (!hw.have_stock) StoreStockRecord(&hw);
        if (bc250_cu_hardware_fallback(s->Decision.mode, applied)) {
            GuardLog("cumode: 40 CU did not hold (reason %lu), stock restored, writing CuMode = 24", reason);
            PersistFallback(reason);
            s->Pending = FALSE;
            s->Confirmed = FALSE;
        }
        // A retained-power resume re-runs the stage: it re-applies what held, against this boot's stock.
        ExAcquireFastMutex(&Device->GartLock);
        s->Hw.have_stock = 1;
        s->Hw.mode = applied == BC250_CU_MODE_FULL ? BC250_CU_MODE_FULL : BC250_CU_MODE_STOCK;
        ExReleaseFastMutex(&Device->GartLock);
        GuardLog("cumode: applied %lu CUs (%lu counted), reason %lu, consistent %u, wrote %u, RLC_PG_CNTL 0x%08X (PG enables 0x%X)",
                 applied, info.active, reason, (ULONG)hw.consistent, (ULONG)hw.wrote, hw.rlc_pg_cntl,
                 bc250_cu_pg_enables(hw.rlc_pg_cntl));
        GuardLog("cumode: SA0-3 CC 0x%08X 0x%08X 0x%08X 0x%08X SPI 0x%X 0x%X 0x%X 0x%X",
                 hw.cc[0], hw.cc[1], hw.cc[2], hw.cc[3], hw.spi[0], hw.spi[1], hw.spi[2], hw.spi[3]);
    } else {
        applied = 0;
        reason = s->Decision.reason != BC250_CU_REASON_NONE ? s->Decision.reason :
                 (hw.reason != BC250_CU_REASON_NONE ? hw.reason : BC250_CU_REASON_NOT_RUN);
        // The pending mark stays: a start that never reached the stage is no healthy start.
        GuardLog("cumode: the constants stage did not run the hook, reason %lu; the caps stay stock", reason);
    }
    StoreLogged(CU_SETTING_LAST_APPLIED, applied);
    StoreLogged(CU_SETTING_LAST_REASON, reason);

    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Valid = valid && info.active != 0;
    s->Applied = applied;
    s->Reason = reason;
    s->Info = info;
    KeReleaseSpinLock(&s->SnapLock, irql);
    CuUnlock(s);
}

// Clears this start's pending mark. PASSIVE_LEVEL. Pending goes first, Confirmed second: a crash in
// between costs a reconfirmation, never a false fallback. Nothing to do is success.
NTSTATUS CuModeConfirm(BC250_DEVICE* Device, _In_z_ const char* Why)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    NTSTATUS status = STATUS_SUCCESS;
    CuLock(s);
    if (s->Valid && s->Applied == BC250_CU_MODE_FULL && s->Pending) {
        status = GuardDeleteSetting(CU_SETTING_PENDING);
        if (NT_SUCCESS(status)) {
            s->Pending = FALSE;
            status = GuardStoreSetting(CU_SETTING_CONFIRMED, s->Decision.encoded);
            if (NT_SUCCESS(status)) s->Confirmed = TRUE;
        }
        GuardLog("cumode: 40 CU request 0x%08X confirmed by %s: 0x%08X", s->Decision.encoded, Why, status);
    }
    CuUnlock(s);
    return status;
}

static void Snapshot(BC250_CU_MODE_STATE* S, BC250_ESCAPE_CU_MODE* Data)
{
    const struct bc250_cu_mode_hw* h = &S->Snap;
    ULONG i;
    KIRQL irql;
    KeAcquireSpinLock(&S->SnapLock, &irql);
    Data->Flags = (S->Valid ? BC250_CU_MODE_FLAG_VALID : 0) |
                  (S->Pending ? BC250_CU_MODE_FLAG_PENDING : 0) |
                  (S->Confirmed ? BC250_CU_MODE_FLAG_CONFIRMED : 0) |
                  (S->StockRecord ? BC250_CU_MODE_FLAG_STOCK_RECORD : 0) |
                  (h->ran && h->consistent ? BC250_CU_MODE_FLAG_CONSISTENT : 0) |
                  (h->wrote ? BC250_CU_MODE_FLAG_WROTE : 0);
    Data->Requested = S->Requested;
    Data->Applied = S->Applied;
    Data->Reason = S->Reason;
    Data->ActiveCus = S->Info.active;
    Data->DisableMask = S->Decision.mode == BC250_CU_MODE_FULL ? S->Decision.disable : 0;
    Data->PciId = S->PciId;
    Data->RlcPgCntl = h->rlc_pg_cntl;
    Data->RlcAonWgpMask = h->rlc_aon_wgp_mask;
    for (i = 0; i < BC250_CU_MODE_SA_COUNT; i++) {
        Data->StockCc[i] = h->stock_cc[i];
        Data->StockSpi[i] = h->stock_spi[i];
        Data->Cc[i] = h->cc[i];
        Data->User[i] = h->user[i];
        Data->Spi[i] = h->spi[i];
        Data->ActiveWgps[i] = h->active_wgps[i];
    }
    Data->Generation = S->Generation;
    KeReleaseSpinLock(&S->SnapLock, irql);
}

// BC250_ESCAPE_CU_MODE. Software state only, so NoAdapterSynchronization=1 for both operations.
void CuModeRequest(BC250_DEVICE* Device, BC250_ESCAPE_CU_MODE* Data, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONGLONG ready = 0;
    BOOLEAN confirm = Data->Op == BC250_CU_MODE_OP_CONFIRM;
    ULONG i;

    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)status;
    Data->Flags = Data->Requested = Data->Applied = Data->Reason = Data->ActiveCus = 0;
    Data->DisableMask = Data->PciId = Data->RlcPgCntl = Data->RlcAonWgpMask = 0;
    for (i = 0; i < BC250_CU_MODE_SA_COUNT; i++)
        Data->StockCc[i] = Data->StockSpi[i] = Data->Cc[i] = Data->User[i] = Data->Spi[i] = Data->ActiveWgps[i] = 0;
    Data->Generation = 0;
    if (Data->AbiVersion != BC250_CU_MODE_ABI || Data->Reserved[0] || Data->Reserved[1] ||
        (Data->Op != BC250_CU_MODE_OP_READ && !confirm) || EscapeFlags != expectedFlags.Value) return;
    if (confirm && !Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    status = STATUS_SUCCESS;
    if (confirm) {
        // The same healthy milestone a deploy confirms: READY, visible, completions, a minute of it.
        if (Data->ExpectedGeneration != s->Generation) status = STATUS_RETRY;
        else if (!StartHealthIsReady(Device, &ready) || ready != s->Generation) status = STATUS_DEVICE_NOT_READY;
        else status = CuModeConfirm(Device, "escape");
    }
    Snapshot(s, Data);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// DXGKQAITYPE_UMDRIVERPRIVATE: the CU fields of the device block follow the registers of this start.
// Without a valid run the measured stock template stays, which is what the registers hold then.
void CuModePatchCaps(BC250_DEVICE* Device, _Inout_updates_bytes_(Bytes) PVOID Caps, ULONG Bytes)
{
    BC250_CU_MODE_STATE* s = &Device->CuMode;
    PUCHAR blob = (PUCHAR)Caps;
    struct bc250_cu_info info;
    BOOLEAN valid;
    KIRQL irql;
    if (Bytes < UMD_CAPS_BYTES) return;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    valid = s->Valid;
    info = s->Info;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (!valid) return;
    RtlCopyMemory(blob + UMD_CAPS_CU_ACTIVE_OFFSET, &info.active, sizeof(info.active));
    RtlCopyMemory(blob + UMD_CAPS_CU_AO_MASK_OFFSET, &info.ao_mask, sizeof(info.ao_mask));
    RtlCopyMemory(blob + UMD_CAPS_CU_BITMAP_OFFSET, info.bitmap, sizeof(info.bitmap));
    RtlCopyMemory(blob + UMD_CAPS_CU_AO_BITMAP_OFFSET, info.ao_bitmap, sizeof(info.ao_bitmap));
}
