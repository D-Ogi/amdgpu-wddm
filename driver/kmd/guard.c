// Boot-loop guard, breadcrumbs and log (ADR 0006, points 3 and 4). This lab has no kernel debugger and a
// hang costs a walk to the machine, so the driver leaves its own trail:
//
//   <service key>\Parameters
//     UnconfirmedStarts  REG_DWORD  incremented at every DxgkDdiStartDevice, cleared from user mode
//                                   (bc250mon) once the desktop is up. At BC250_MAX_UNCONFIRMED_STARTS the
//                                   driver refuses to start and Windows falls back to Basic Display. An
//                                   orderly stop gives its start's count back in a boot that was confirmed
//                                   (BD-090, the section at the end of this file).
//     LastStage          REG_DWORD  BC250_STAGE, written and flushed at each step
//     StageHistory       REG_SZ     the last stages of this boot, oldest first
//
// The registry part runs at PASSIVE_LEVEL. DDIs that can run higher (present is not one of them on a
// display-only driver, the pointer and interrupt DDIs are) must not call in. GuardLog is the exception: since
// 0.7.1 it is safe up to DISPATCH_LEVEL, because the M7 submission and DPC paths log.
#include <ntifs.h>
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "wave_snapshot.h"
#include "guard_keep_prune.h"
#include "hang_recovery.h"
#include "state_kernel.h"
#include <ntstrsafe.h>
#include <stdarg.h>

static WCHAR g_History[256];
static BC250_STAGE g_LastStage;
// BD-090: this image's start wrote and flushed one count, and no confirmation has cleared it since. Per image on
// purpose: a stop runs in the image of the start it ends (a restart or an update unloads the image in between).
// Start, confirmation (under StartHealth.Lifecycle, closed by StartHealthClose) and stop never overlap.
static BOOLEAN g_StartCounted;
static HANDLE g_KeepRoot;

// ---- the log ring --------------------------------------------------------------------------------------------
//
// The lab machine is headless over SSH: no kernel debugger, no DebugView, so DbgPrintEx reaches nobody and the
// evidence an experiment is run for would be unreadable. Every line therefore also lands here, and
// bc250kmd_cli log reads it back through BC250_ESCAPE_GET_LOG.
//
// The ring is a static array, so it lives in the driver image. It survives a device stop and start - which is
// exactly what an experiment does between runs - but **not** a driver unload: the image is loaded again with
// its statics zeroed and the sequence number back at 0. That is useful rather than a limitation, and the stage
// A procedure in README.md leans on it: if `log` shows the sequence starting from 0 again after a disable and
// enable, the driver really did unload and DriverEntry really did re-read its gates.
//
// The first BC250_LOG_HEAD_LINES of a load are never overwritten, because the order of the start-up is the
// point; everything after them wraps in the tail and counts up g_LogLost, so a gap is always visible.
#define BC250_LOG_TAIL_LINES (BC250_LOG_RING_LINES - BC250_LOG_HEAD_LINES)

static BC250_LOG_LINE g_Log[BC250_LOG_RING_LINES];
static KSPIN_LOCK g_LogLock;
static ULONG g_LogNext;                 // the sequence number the next line will get
static ULONG g_LogLost;                 // lines the tail has overwritten
static volatile LONG g_LogAbove;        // callers refused because the IRQL was above DISPATCH_LEVEL
static ULONGLONG g_LogStart;            // interrupt time at GuardInit
static BOOLEAN g_LogReady;

static ULONG LogSlot(ULONG Sequence)
{
    if (Sequence < BC250_LOG_HEAD_LINES) return Sequence;
    return BC250_LOG_HEAD_LINES + (Sequence - BC250_LOG_HEAD_LINES) % BC250_LOG_TAIL_LINES;
}

// ---- the summary's own lines, beside the ring (BD-097) ---------------------------------------------------------
//
// A log summary is about 320 GuardLog lines in one escape (wddm.c WddmSummary and what it calls). In the ring
// that is 42 % of the 768 rotating lines per call, and a caller that asks for one every few seconds - the b23
// lab read measured the overlay's old 5 s poll - rotates the whole ring in about 12 s, which defeats everything
// else this file does to keep a trail. So the summary writes into storage of its own: the lines go to
// g_Summary below, the ring gets nothing, and BC250_ESCAPE_GET_LOG hands them back by a sequence number
// above the ring's own space (BC250_LOG_SUMMARY_SEQ). The CLI's paging loop needs no change for that: it asks
// for the sequence the driver answered with, and this space pages like any other.
//
// One summary at a time, claimed by the thread that writes it (g_SummaryThread): the diversion must catch the
// lines of its own escape and nothing else, because another processor logging a device event at the same moment
// must not have its line taken out of the ring. A caller that finds the claim taken writes to the ring, as
// before - two summaries at once is not a case worth a second buffer.
//
// The thread is not enough by itself. A DPC runs in the context of whatever thread its processor was running, so
// KeGetCurrentThread() inside the submit or paging watchdog DPC answers the escape's own thread while the escape
// writes its summary on that processor. Those DPCs log the FENCE TIMEOUT lines, which are the evidence a hang is
// read from, and the diversion would move them out of the ring and into a block that the next summary overwrites.
// A line of a DPC therefore stays in the ring, whoever the current thread is, and the summary thread's own lines
// are diverted whether it holds a lock or not (WddmSummary logs some of its tables at DISPATCH_LEVEL, so the IRQL
// does not tell the two apart; KeIsExecutingDpc does).
//
// Each claim answers a block of its own in the summary space (g_SummaryBlock, BC250_LOG_SUMMARY_BLOCKS of them).
// The escapes that read a block are not serialized against the escape that writes one - GET_LOG pages go without
// adapter synchronization - so a reader can be half way through a block when the next summary starts. With a
// block number inside the sequence that reader's next page is refused and its paging ends; without it the page
// would be half of one summary and half of the next, with nothing to say so.
static BC250_LOG_LINE g_Summary[BC250_LOG_SUMMARY_LINES];
static PVOID g_SummaryThread;           // the thread whose GuardLog calls go to g_Summary; NULL = nobody's
static ULONG g_SummaryWritten;          // lines of the last (or running) summary that are in g_Summary
static ULONG g_SummaryDropped;          // lines of it that did not fit; the escape reports them
static ULONG g_SummaryRingSeq;          // the ring sequence the summary was taken at, for the one ring line
static ULONG g_SummaryBlock;            // the block of the summary space g_Summary answers as
static BOOLEAN g_SummaryTaken;          // a summary has been taken, so the next claim moves to the next block

// The first sequence number of the block in g_Summary. Callers hold g_LogLock.
static ULONG SummaryBase(void)
{
    return BC250_LOG_SUMMARY_SEQ + g_SummaryBlock * BC250_LOG_SUMMARY_LINES;
}

ULONGLONG GuardLogMilliseconds(void)
{
    ULONGLONG now = KeQueryInterruptTime();

    return (now > g_LogStart) ? (now - g_LogStart) / 10000ull : 0ull;
}

static NTSTATUS OpenParameters(_Out_ HANDLE* Key)
{
    // Retained helper name for the record writers. This opens mutable service
    // state through its OS-returned handle, never the installation parameters.
    return DriverStateOpenData(Key);
}

static NTSTATUS ReadDword(HANDLE Key, PCWSTR Name, _Out_ ULONG* Value)
{
    UNICODE_STRING name;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    ULONG length;
    NTSTATUS status;

    *Value = 0;
    RtlInitUnicodeString(&name, Name);
    status = ZwQueryValueKey(Key, &name, KeyValuePartialInformation, info, sizeof(buffer), &length);
    if (!NT_SUCCESS(status)) return status;
    if (info->Type != REG_DWORD || info->DataLength != sizeof(ULONG)) return STATUS_OBJECT_TYPE_MISMATCH;
    RtlCopyMemory(Value, info->Data, sizeof(ULONG));
    return STATUS_SUCCESS;
}

static NTSTATUS WriteDword(HANDLE Key, PCWSTR Name, ULONG Value)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    return ZwSetValueKey(Key, &name, 0, REG_DWORD, &Value, sizeof(Value));
}

NTSTATUS GuardInit(_In_ PDRIVER_OBJECT DriverObject)
{
    NTSTATUS status;
    // The ring first, so that everything DriverEntry does from here on is in it, including a failure below.
    KeInitializeSpinLock(&g_LogLock);
    g_LogStart = KeQueryInterruptTime();
    g_LogReady = TRUE;

    status = IoGetDriverDirectory(DriverObject, DriverDirectorySharedData, 0, &g_KeepRoot);
    if (!NT_SUCCESS(status)) {
        g_KeepRoot = NULL;
        GuardLog("keeplog: shared directory unavailable 0x%08X", (ULONG)status);
    }
    status = DriverStateInit(DriverObject);
    if (!NT_SUCCESS(status)) GuardCleanup();
    return status;
}

void GuardCleanup(void)
{
    if (g_KeepRoot != NULL) { ZwClose(g_KeepRoot); g_KeepRoot = NULL; }
    DriverStateClose();
}

void GuardStage(BC250_STAGE Stage)
{
    HANDLE key;
    WCHAR item[16];
    UNICODE_STRING name;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) return;
    if (Stage == g_LastStage) return;
    g_LastStage = Stage;

    // Keep the tail if the history is full: the end of the trail is what matters after a hang.
    if (NT_SUCCESS(RtlStringCchPrintfW(item, RTL_NUMBER_OF(item), L"%u ", (ULONG)Stage)) &&
        !NT_SUCCESS(RtlStringCchCatW(g_History, RTL_NUMBER_OF(g_History), item)))
    {
        RtlMoveMemory(g_History, g_History + 64, sizeof(g_History) - 64 * sizeof(WCHAR));
        RtlStringCchCatW(g_History, RTL_NUMBER_OF(g_History), item);
    }

    if (!NT_SUCCESS(OpenParameters(&key))) return;
    WriteDword(key, L"LastStage", (ULONG)Stage);
    RtlInitUnicodeString(&name, L"StageHistory");
    ZwSetValueKey(key, &name, 0, REG_SZ, g_History, (ULONG)((wcslen(g_History) + 1) * sizeof(WCHAR)));
    ZwFlushKey(key);        // the point of a breadcrumb is that it is on disk before the next step runs
    ZwClose(key);
    GuardLog("stage %u", (ULONG)Stage);
}

BC250_STAGE GuardLastStage(void)
{
    return g_LastStage;
}

// A failed read is 0, not the default: a name outside the catalog (STATUS_INVALID_PARAMETER), a closed group or a
// state that is not ready all select the old variant of the gate that asked. BD-190: BootLogo and LabDiagnostics
// read 0 this way from tester.31 on, and nothing said so. The line below says it, bounded, so a support file
// shows the name, the status and the value chosen; tools/state/check_settings.py refuses an unknown name at build
// time. Nothing in GuardLog reads a setting, so the line cannot recurse.
#define BC250_SETTING_FAILURE_LINES 32
static LONG g_SettingReadFailures;

ULONG GuardReadSetting(_In_z_ PCWSTR Name, ULONG Default)
{
    ULONG value=0;NTSTATUS status=DriverStateNumber(L"",Name,TRUE,&value);
    if(NT_SUCCESS(status))return value;
    if(status==STATUS_OBJECT_NAME_NOT_FOUND)return Default;
    if(InterlockedIncrement(&g_SettingReadFailures)<=BC250_SETTING_FAILURE_LINES)
        GuardLog("setting %ws: read failed 0x%08X, 0 used instead of the default %lu",Name,status,Default);
    return 0;
}

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
// BD-235 lab experiment, lab flavour only, not a catalog setting: the paging ring's start pointer.
// The seed is PagingRingStartBytes (REG_QWORD) with PagingRingStartNonce (REG_QWORD, nonzero), both in the driver's
// Parameters key. The driver only reads that key: driver package isolation makes Parameters read only for the
// driver (develop/driver-isolation.md, "should only be read at runtime and not modified"), and on this OS
// IoOpenDriverRegistryKey(DriverRegKeyParameters) refuses KEY_SET_VALUE with 0xC0000022 (arm-ringstart1 boot a).
// A seed is consumed once per nonce: the start writes the nonce as LabPagingRingStartNonce into the driver's internal
// persistent state (DriverRegKeyPersistentState, the place isolation gives a driver for its own runtime state) and
// flushes it BEFORE the seed goes to the shim. A later start that finds the same nonce there does not use the seed,
// so a bug check at the bring-up it changes cannot repeat at the next boot, even though the seed stays in
// Parameters until the lab script removes it. The flush is the same ZwFlushKey at the same point of the start as
// the boot-loop guard's own count (GuardCheckAndCountStart), so it holds for a bug check in the boot itself.
static PDRIVER_OBJECT g_LabDriverObject;
static ULONGLONG g_LabSeed, g_LabNonce;
static NTSTATUS g_LabSeedRead = STATUS_OBJECT_NAME_NOT_FOUND;
// BD-235 lab selector, the same way: PagingBoundaryMode (REG_DWORD, Parameters, read only at DriverEntry). 1, and
// any value but 0, and no value at all: the ring boundary pad of both flavours (bc250_ring.c, gfx.c
// GfxSdmaBoundaryLocked). 0: no pad on any ring, the behaviour before the fix, for a lab control run only. Not a
// catalog setting: a lab arm swaps the KMD image only, and the installed catalog, its identity and bc250kmd_cli stay
// those of the installed package.
static ULONG g_LabBoundaryMode = BC250_PAGING_BOUNDARY_RELEASE_MODE;

static NTSTATUS LabQueryDword(HANDLE Key, PCWSTR Name, _Out_ ULONG* Value)
{
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    UNICODE_STRING name;
    ULONG length = 0;
    NTSTATUS status;

    *Value = 0;
    RtlInitUnicodeString(&name, Name);
    status = ZwQueryValueKey(Key, &name, KeyValuePartialInformation, info, sizeof(buffer), &length);
    if (!NT_SUCCESS(status)) return status;
    if (info->Type != REG_DWORD || info->DataLength != sizeof(ULONG)) return STATUS_OBJECT_TYPE_MISMATCH;
    RtlCopyMemory(Value, info->Data, sizeof(ULONG));
    return STATUS_SUCCESS;
}

static NTSTATUS LabQueryQword(HANDLE Key, PCWSTR Name, _Out_ ULONGLONG* Value)
{
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONGLONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    UNICODE_STRING name;
    ULONG length = 0;
    NTSTATUS status;

    *Value = 0;
    RtlInitUnicodeString(&name, Name);
    status = ZwQueryValueKey(Key, &name, KeyValuePartialInformation, info, sizeof(buffer), &length);
    if (!NT_SUCCESS(status)) return status;
    if (info->Type != REG_QWORD || info->DataLength != sizeof(ULONGLONG)) return STATUS_OBJECT_TYPE_MISMATCH;
    RtlCopyMemory(Value, info->Data, sizeof(ULONGLONG));
    return STATUS_SUCCESS;
}

// DriverEntry: the seed and its nonce, read only. Nothing is used yet.
void GuardLabReadPagingRingStart(_In_ PDRIVER_OBJECT DriverObject)
{
    HANDLE key;
    NTSTATUS status, nonce, mode;
    ULONG value;

    g_LabDriverObject = DriverObject;
    status = IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, KEY_QUERY_VALUE, 0, &key);
    if (!NT_SUCCESS(status)) {
        g_LabSeedRead = status;
        GuardLog("lab: paging ring start: Parameters key not opened for reading (0x%08X), nothing used", status);
        return;
    }
    g_LabSeedRead = LabQueryQword(key, L"PagingRingStartBytes", &g_LabSeed);
    nonce = LabQueryQword(key, L"PagingRingStartNonce", &g_LabNonce);
    mode = LabQueryDword(key, L"PagingBoundaryMode", &value);
    ZwClose(key);
    if (mode == STATUS_OBJECT_NAME_NOT_FOUND)
        GuardLog("lab: paging boundary mode %lu: PagingBoundaryMode absent, the default (pad)", g_LabBoundaryMode);
    else {
        g_LabBoundaryMode = (NT_SUCCESS(mode) && value == 0) ? 0 : 1;
        GuardLog("lab: paging boundary mode %lu read (read only; value %lu, 0x%08X): 0 none (control), else pad",
                 g_LabBoundaryMode, value, mode);
    }
    bc250_ring_boundary_pad = g_LabBoundaryMode != 0;
    if (g_LabSeedRead == STATUS_OBJECT_NAME_NOT_FOUND) return;
    if (!NT_SUCCESS(g_LabSeedRead) || !NT_SUCCESS(nonce) || g_LabSeed == 0 || g_LabNonce == 0) {
        GuardLog("lab: paging ring start not used: seed 0x%llX (0x%08X), nonce 0x%llX (0x%08X); two nonzero REG_QWORDs "
                 "expected", g_LabSeed, g_LabSeedRead, g_LabNonce, nonce);
        g_LabSeed = 0;
        g_LabSeedRead = STATUS_OBJECT_TYPE_MISMATCH;
        return;
    }
    GuardLog("lab: paging ring start 0x%llX nonce 0x%llX read (read only), for this start", g_LabSeed, g_LabNonce);
}

// The compute node selector, lab flavour only, not a catalog setting: a lab arm swaps the KMD image only, and the
// installed catalog stays that of the installed package (docs/design/compute-node.md). EnableComputeNode (REG_DWORD,
// Parameters, read only at DriverEntry): 0 closes node 2, for a control run. Absent or any other value opens it.
static BOOLEAN g_LabComputeNode = TRUE;

void GuardLabReadComputeNode(_In_ PDRIVER_OBJECT DriverObject)
{
    HANDLE key;
    NTSTATUS status;
    ULONG value = 0;

    status = IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, KEY_QUERY_VALUE, 0, &key);
    if (!NT_SUCCESS(status)) {
        GuardLog("lab: compute node selector: Parameters key not opened for reading (0x%08X), node 2 open", status);
        return;
    }
    status = LabQueryDword(key, L"EnableComputeNode", &value);
    ZwClose(key);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        GuardLog("lab: compute node selector absent, node 2 open (the default)");
        return;
    }
    g_LabComputeNode = !(NT_SUCCESS(status) && value == 0);
    GuardLog("lab: compute node selector read (read only; value %lu, 0x%08X): node 2 %s", value, status,
             g_LabComputeNode ? "open" : "closed (control)");
}

// The GDI hardware acceleration selector, lab flavour only, not a catalog setting: a lab arm swaps the KMD image
// only, and the installed catalog stays that of the installed package (docs/design/gdi-hw-accel.md). LabGdiHwAccel
// (REG_DWORD, Parameters, read only at DriverEntry): 1 opens the GDI gate whatever EnableGdiHwAccel says. Absent
// or any other value leaves the gate to EnableGdiHwAccel. The selector can open the gate, never close it.
static BOOLEAN g_LabGdiHwAccel = FALSE;

void GuardLabReadGdiHwAccel(_In_ PDRIVER_OBJECT DriverObject)
{
    HANDLE key;
    NTSTATUS status;
    ULONG value = 0;

    status = IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, KEY_QUERY_VALUE, 0, &key);
    if (!NT_SUCCESS(status)) {
        GuardLog("lab: GDI selector: Parameters key not opened for reading (0x%08X), not used", status);
        return;
    }
    status = LabQueryDword(key, L"LabGdiHwAccel", &value);
    ZwClose(key);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        GuardLog("lab: GDI selector absent, the gate is EnableGdiHwAccel (the default)");
        return;
    }
    g_LabGdiHwAccel = (BOOLEAN)(NT_SUCCESS(status) && value == 1);
    GuardLog("lab: GDI selector read (read only; value %lu, 0x%08X): %s", value, status,
             g_LabGdiHwAccel ? "gate open" : "not used, the gate is EnableGdiHwAccel");
}

// StartDevice, after the boot-loop guard passed: the seed for this start's first fresh SDMA0 bring-up, or 0. The
// nonce is written and flushed first; any failure on the way gives 0. Without a seed, a marker of an earlier seed is
// deleted, so the internal state holds nothing once the lab script has removed the seed.
ULONGLONG GuardLabCommitPagingRingStart(void)
{
    UNICODE_STRING name;
    ULONGLONG marker = 0, seed = g_LabSeed, nonce = g_LabNonce;
    HANDLE key;
    NTSTATUS status, query;

    g_LabSeed = 0;      // one look per DriverEntry read; a second start in this boot finds the marker in any case
    if (g_LabDriverObject == NULL) return 0;
    if (seed == 0 && g_LabSeedRead != STATUS_OBJECT_NAME_NOT_FOUND) return 0;   // malformed: already logged
    status = IoOpenDriverRegistryKey(g_LabDriverObject, DriverRegKeyPersistentState, KEY_QUERY_VALUE | KEY_SET_VALUE, 0,
                                     &key);
    if (!NT_SUCCESS(status)) {
        if (seed != 0)
            GuardLog("lab: paging ring start 0x%llX nonce 0x%llX not used: persistent state not opened (0x%08X)", seed,
                     nonce, status);
        return 0;
    }
    RtlInitUnicodeString(&name, L"LabPagingRingStartNonce");
    query = LabQueryQword(key, L"LabPagingRingStartNonce", &marker);
    if (seed == 0) {
        if (query != STATUS_OBJECT_NAME_NOT_FOUND) {
            status = ZwDeleteValueKey(key, &name);
            if (NT_SUCCESS(status)) status = ZwFlushKey(key);
            GuardLog("lab: paging ring start: no seed, marker 0x%llX deleted (0x%08X)", marker, status);
        }
        ZwClose(key);
        return 0;
    }
    if (NT_SUCCESS(query) && marker == nonce) {
        ZwClose(key);
        GuardLog("lab: paging ring start 0x%llX nonce 0x%llX already consumed at an earlier start, not used", seed,
                 nonce);
        return 0;
    }
    status = ZwSetValueKey(key, &name, 0, REG_QWORD, &nonce, sizeof(nonce));
    if (NT_SUCCESS(status)) status = ZwFlushKey(key);
    ZwClose(key);
    if (!NT_SUCCESS(status)) {
        GuardLog("lab: paging ring start 0x%llX nonce 0x%llX not used: marker not written (0x%08X)", seed, nonce,
                 status);
        return 0;
    }
    GuardLog("lab: paging ring start 0x%llX nonce 0x%llX consumed: marker written and flushed, for the first SDMA0 "
             "bring-up", seed, nonce);
    return seed;
}
#endif

// BD-235: the ring boundary rule (bc250_ring.c, gfx.c). The lab flavour takes the arm's selector; the release flavour
// takes BC250_PAGING_BOUNDARY_RELEASE_MODE (bc250kmd.h).
ULONG GuardPagingBoundaryMode(void)
{
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    return g_LabBoundaryMode;
#else
    return BC250_PAGING_BOUNDARY_RELEASE_MODE;
#endif
}

// Node 2: the lab flavour takes the arm's selector; the release flavour always opens it.
BOOLEAN GuardComputeNodeSelector(void)
{
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    return g_LabComputeNode;
#else
    return TRUE;
#endif
}

BOOLEAN GuardLabGdiHwAccel(void)
{
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    return g_LabGdiHwAccel;
#else
    return FALSE;
#endif
}

// A one-shot gate: the value 1 is read and put back to 0 on disk before anything it opens has run, so a bugcheck
// behind the gate cannot repeat itself at the next boot (E16 run 007 did, once). Any other value is left alone.
ULONG GuardConsumeSetting(_In_z_ PCWSTR Name, ULONG Default)
{
    ULONG value=0,closed=0;
    NTSTATUS status=DriverStateNumber(L"",Name,TRUE,&value);
    if(status==STATUS_OBJECT_NAME_NOT_FOUND)return Default;
    if(!NT_SUCCESS(status))return 0;
    if(value==1) {
        // DriverStateSet includes the durable flush. Failure never grants the
        // experiment permission, including when the caller's default is 1.
        status=DriverStateSet(L"",Name,REG_DWORD,&closed,sizeof(closed));
        if(!NT_SUCCESS(status)) {
            GuardLog("gate: one-shot durable closure failed, refused (0x%08X)",status);
            return 0;
        }
    }
    return value;
}

NTSTATUS GuardCheckAndCountStart(BOOLEAN RequireDurable)
{
    HANDLE key;
    ULONG starts;
    BOOLEAN countWritten;
    NTSTATUS status = OpenParameters(&key);

    g_StartCounted=FALSE;   // set below only when this start's count is on the disk
    if (!NT_SUCCESS(status)) {
        GuardLog("guard: open failed 0x%08X, durable required %u",status,RequireDurable);
        return RequireDurable ? status : STATUS_SUCCESS; // retain display-only recovery policy
    }
    status=ReadDword(key, L"UnconfirmedStarts", &starts);
    // A genuinely absent value starts at zero. Unreadable or malformed data is
    // not a fresh budget for the full table. ReadDword initializes starts to zero.
    if (!NT_SUCCESS(status) && status!=STATUS_OBJECT_NAME_NOT_FOUND) {
        GuardLog("guard: read failed 0x%08X, durable required %u",status,RequireDurable);
        if (RequireDurable) { ZwClose(key); return status; }
    }
    if (starts >= BC250_MAX_UNCONFIRMED_STARTS)
    {
        ZwClose(key);
        GuardLog("guard: %u starts nobody confirmed, refusing to start", starts);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    status=WriteDword(key, L"UnconfirmedStarts", starts + 1);
    countWritten=NT_SUCCESS(status);
    if (!NT_SUCCESS(status)) {
        GuardLog("guard: count write failed 0x%08X, durable required %u",status,RequireDurable);
        if (RequireDurable) { ZwClose(key); return status; }
    }
    status=ZwFlushKey(key);
    ZwClose(key);
    if (!NT_SUCCESS(status)) {
        GuardLog("guard: count flush failed 0x%08X, durable required %u",status,RequireDurable);
        if (RequireDurable) return status;
    } else if (countWritten) {
        g_StartCounted=TRUE;
        GuardLog("guard: count %u -> %u flushed, durable required %u",starts,starts+1,RequireDurable);
    }
    return STATUS_SUCCESS;
}

void GuardLog(_In_z_ const char* Format, ...)
{
    va_list arguments;
    char line[BC250_LOG_TEXT];
    BC250_LOG_LINE* entry;
    ULONGLONG now;
    KIRQL irql;

    // Zeroed, not just terminated: whatever is in here is copied to user mode by the log escape, and the bytes
    // past the terminator would otherwise be whatever this stack happened to hold.
    RtlZeroMemory(line, sizeof(line));
    va_start(arguments, Format);
    RtlStringCchVPrintfA(line, sizeof(line), Format, arguments);     // truncates rather than fails; always terminated
    va_end(arguments);

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "bc250kmd: %s\n", line);

    // The ring needs a spin lock and therefore DISPATCH_LEVEL or below. Nothing in this driver logs from higher:
    // the interrupt routine, the DxgkCbSynchronizeExecution routine and the bugcheck display path are all silent
    // by design, and the one DDI the WDK allows above DISPATCH (DxgkDdiSetVidPnSourceAddress, annotated
    // _IRQL_requires_max_(PROFILE_LEVEL - 1)) checks before it logs. A caller that ever gets here from higher is
    // counted instead of taking the machine down, and the count is reported next to the lines.
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) { InterlockedIncrement(&g_LogAbove); return; }
    if (!g_LogReady) return;

    KeAcquireSpinLock(&g_LogLock, &irql);
    now = KeQueryInterruptTime();       // inside the lock, so that the times rise with the sequence numbers
    // BD-097: a summary's own lines go beside the ring, for the thread that claimed the diversion and for no
    // other. Their sequence numbers are the summary space's, so a reader can tell them from a ring line. A DPC
    // of that same thread's processor is not that thread's work: its line stays in the ring (see the claim above).
    if (g_SummaryThread != NULL && g_SummaryThread == (PVOID)KeGetCurrentThread() && !KeIsExecutingDpc())
    {
        if (g_SummaryWritten < BC250_LOG_SUMMARY_LINES)
        {
            entry = &g_Summary[g_SummaryWritten];
            entry->Sequence = SummaryBase() + g_SummaryWritten;
            entry->Milliseconds = (ULONG)((now - g_LogStart) / 10000ull);
            RtlCopyMemory(entry->Text, line, sizeof(entry->Text));
            g_SummaryWritten++;
        }
        else g_SummaryDropped++;
        KeReleaseSpinLock(&g_LogLock, irql);
        return;
    }
    if (g_LogNext >= BC250_LOG_RING_LINES) g_LogLost++;      // this line overwrites one the tail still held
    entry = &g_Log[LogSlot(g_LogNext)];
    entry->Sequence = g_LogNext++;
    entry->Milliseconds = (ULONG)((now - g_LogStart) / 10000ull);
    RtlCopyMemory(entry->Text, line, sizeof(entry->Text));
    KeReleaseSpinLock(&g_LogLock, irql);
}

// BD-097: claim the diversion for this thread, so that the lines it writes next go beside the ring instead of
// into it. FALSE when another thread holds it; the caller then writes to the ring, which is what every build
// before this one did. Call GuardLogSummaryEnd on every path out, including a failure of what it wraps.
BOOLEAN GuardLogSummaryBegin(void)
{
    KIRQL irql;
    BOOLEAN claimed = FALSE;

    if (!g_LogReady) return FALSE;
    KeAcquireSpinLock(&g_LogLock, &irql);
    if (g_SummaryThread == NULL)
    {
        // The next block of the summary space, so that a reader of the last one is refused instead of mixed.
        // The first summary of a driver load keeps block 0, which is BC250_LOG_SUMMARY_SEQ itself.
        if (g_SummaryTaken) g_SummaryBlock = (g_SummaryBlock + 1u) % BC250_LOG_SUMMARY_BLOCKS;
        g_SummaryTaken = TRUE;
        g_SummaryThread = (PVOID)KeGetCurrentThread();
        g_SummaryWritten = 0;
        g_SummaryDropped = 0;
        g_SummaryRingSeq = g_LogNext;
        claimed = TRUE;
    }
    KeReleaseSpinLock(&g_LogLock, irql);
    return claimed;
}

// Release the claim and say what the summary wrote: First is the sequence number of its first line, which is the
// one to read it from, Lines how many lines it holds, Dropped the lines that did not fit the buffer (0 unless the
// summary outgrows BC250_LOG_SUMMARY_LINES), RingSeq the ring sequence the summary was taken at, which is what
// places the block in the ring's own trail.
void GuardLogSummaryEnd(_Out_ ULONG* First, _Out_ ULONG* Lines, _Out_ ULONG* Dropped, _Out_ ULONG* RingSeq)
{
    KIRQL irql;

    KeAcquireSpinLock(&g_LogLock, &irql);
    *First = SummaryBase();
    *Lines = g_SummaryWritten;
    *Dropped = g_SummaryDropped;
    *RingSeq = g_SummaryRingSeq;
    if (g_SummaryThread == (PVOID)KeGetCurrentThread()) g_SummaryThread = NULL;
    KeReleaseSpinLock(&g_LogLock, irql);
}

// The sequence number the block in the summary buffer is read from now. A page read of the summary space answers
// it, so that a caller whose block has been replaced learns that from the same escape (BC250_ESCAPE_LOG.SummaryFrom).
ULONG GuardLogSummaryBase(void)
{
    KIRQL irql;
    ULONG base;

    KeAcquireSpinLock(&g_LogLock, &irql);
    base = SummaryBase();
    KeReleaseSpinLock(&g_LogLock, irql);
    return base;
}

// The sequence number the next line will get. Racy by nature and that is fine: its only use is to stamp "the
// first call of this DDI happened about here" next to a counter.
ULONG GuardLogSequence(void)
{
    return g_LogNext;
}

// For the bugcheck callback's dump pages (hang.c): names storage, reads none of it, so no lock at HIGH_LEVEL.
void GuardLogDumpRegion(_Outptr_ const void** Ring, _Out_ SIZE_T* RingBytes, _Outptr_ const void** Cursor)
{
    *Ring = g_Log;
    *RingBytes = sizeof(g_Log);
    *Cursor = &g_LogNext;
}

// Under the lock, so that Total and Lost are the same moment: a reader that saw a total from before a wrap and a
// lost count from after it would compute a gap that never existed.
void GuardLogStats(_Out_ ULONG* Total, _Out_ ULONG* Lost, _Out_ ULONG* Above)
{
    KIRQL irql;

    KeAcquireSpinLock(&g_LogLock, &irql);
    *Total = g_LogNext;
    *Lost = g_LogLost;
    KeReleaseSpinLock(&g_LogLock, irql);
    *Above = (ULONG)g_LogAbove;         // its own counter, never taken under the lock: nothing pairs with it
}

// Lines with a sequence number of at least From that the ring still holds, oldest first. Next is where to carry
// on; a return of 0 means there is nothing more. The gap between the protected head and the wrapping tail is
// skipped silently - Lost says how big it is.
ULONG GuardLogRead(ULONG From, _Out_writes_to_(Max, return) BC250_LOG_LINE* Lines, ULONG Max, _Out_ ULONG* Next)
{
    ULONG count = 0, sequence = From, oldest;
    KIRQL irql;

    *Next = From;
    if (Lines == NULL || Max == 0) return 0;

    // BD-097: the summary space, above every sequence number the ring can give. A read in it stays in it: when
    // its lines run out the count is 0 and Next does not move, which is what ends the caller's paging loop.
    if (From >= BC250_LOG_SUMMARY_SEQ)
    {
        ULONG offset;

        KeAcquireSpinLock(&g_LogLock, &irql);
        offset = From - BC250_LOG_SUMMARY_SEQ;
        sequence = offset % BC250_LOG_SUMMARY_LINES;
        // Nothing while another thread is writing a summary: the buffer is then half of this one and half of the
        // last. Nothing either for a block the buffer no longer holds, which is a reader that another caller's
        // summary overtook: a short read is an answer, a page of two summaries is not.
        if (g_SummaryThread == NULL && offset / BC250_LOG_SUMMARY_LINES == g_SummaryBlock)
            while (count < Max && sequence < g_SummaryWritten) Lines[count++] = g_Summary[sequence++];
        KeReleaseSpinLock(&g_LogLock, irql);
        *Next = From + count;
        return count;
    }

    KeAcquireSpinLock(&g_LogLock, &irql);
    // The head region: sequences 0 .. BC250_LOG_HEAD_LINES-1, kept for as long as the driver is loaded.
    while (count < Max && sequence < BC250_LOG_HEAD_LINES && sequence < g_LogNext)
        Lines[count++] = g_Log[sequence++];

    if (count < Max)
    {
        // The tail: the most recent BC250_LOG_TAIL_LINES from BC250_LOG_HEAD_LINES on. Anything older is gone.
        if (sequence < BC250_LOG_HEAD_LINES) sequence = BC250_LOG_HEAD_LINES;
        oldest = (g_LogNext > BC250_LOG_RING_LINES) ? g_LogNext - BC250_LOG_TAIL_LINES : BC250_LOG_HEAD_LINES;
        if (sequence < oldest) sequence = oldest;
        while (count < Max && sequence < g_LogNext) Lines[count++] = g_Log[LogSlot(sequence++)];
    }
    if (sequence > g_LogNext) sequence = g_LogNext;     // do not ask the tool to come back for nothing
    *Next = sequence;
    KeReleaseSpinLock(&g_LogLock, irql);
    return count;
}

// ---- the log, kept ---------------------------------------------------------------------------------------------
//
// E16 run 1 (0.7.2): with EnableFullWddm open, DxgkDdiStartDevice returned success, dxgkrnl called some DDIs,
// did not like an answer, stopped the device and unloaded the driver - and the ring went with the image, so the
// one run the ring was built for left nothing but "stage 39, 70, 79". Windows keeps no reason either
// (CM_PROB_FAILED_POST_START, problem status 0). Kto nie ma w głowie, ten ma w nogach (what the head forgets,
// the legs pay for): the ring can now be written out at the end of a stop.
//
// Behind its own gate, the KeepLog setting (0 by default, read here at each keep) in the Data key of the active
// state generation (driver/state/catalog.json; since 6282d18d, not Parameters\KeepLog, which no longer has an
// effect): a synchronous file write has no timeout, and a stop during a shutdown or over a wedged volume is no
// place for one on a board without a BMC. An experiment that expects dxgkrnl to end the start opens it.
//
// ONE text file per episode under the OS shared driver directory's Logs child (IoGetDriverDirectory,
// DriverDirectorySharedData; user mode finds the same directory with GetSharedServiceDirectory, which
// `bc250kmd_cli state paths` prints; not C:\BC250\kmdlog since 6282d18d). An episode is a device start or stop
// (GuardLogKeepEpisode, called by pnp.c). The file is named by the UTC time of the first keep of that episode, to
// the millisecond, and by the episode's label, in the format bc250kmd_cli log prints. The directory is created if
// it is missing. What became of the attempt is written to KeepStatus in the same Data key (the NTSTATUS of the
// create, or of the last write), because a line in the ring saying that the ring was not kept would be read by nobody.
// PASSIVE_LEVEL only: DxgkDdiStopDevice is, and everything here (Zw file and registry calls, paged pool) needs it.
//
// Up to 0.7.216.3 every call made its own file, and a start with the tracing gates open calls this about fifteen
// times (startup.c at each stage, gfx.c at each CP step and RLC boundary, gpumem.c at the bootstrap TLB). Fifteen
// files of one start, each holding the same early lines, and nothing ever removed one: unit A reached 11993 files
// and 134 MB. A later call now APPENDS the lines the file does not hold yet, at a byte offset this file keeps
// itself, so the checkpoints stay in order and in one file, and a crash between two of them leaves everything that
// was written before it. Each append writes its own "-- keep" line first, so a reader still sees where the
// checkpoints were. The number of files is bounded as well: see KeepPrune below.
#define BC250_LOG_KEEP_LINE (BC250_LOG_TEXT + 32)   // "%6lu %6lu.%03lu " is 18 characters, CR LF, and room to spare
#define BC250_KEEP_DIRECTORY L"Logs"
#define BC250_KEEP_SCAN_BYTES 8192u                 // one directory page; about 90 names of ours
#define BC250_KEEP_DELETE_MAX 512u                  // deletes in one prune: a bound on the work a PnP start does

static WCHAR g_KeepPath[160];           // the open episode's file, or empty when the next keep opens one
static LONGLONG g_KeepBytes;            // bytes that file already holds; the offset the next write uses
static ULONG g_KeepFrom;                // the first ring sequence that is not in it yet
static ULONG g_KeepAppends;             // keeps of this episode, the first one included
static PCWSTR g_KeepLabel = L"load";    // what the episode is; before the first GuardLogKeepEpisode, the load

static NTSTATUS KeepCheckFile(HANDLE File, BOOLEAN Directory)
{
    FILE_ATTRIBUTE_TAG_INFORMATION tag;
    IO_STATUS_BLOCK io;
    NTSTATUS status = ZwQueryInformationFile(File, &io, &tag, sizeof(tag), FileAttributeTagInformation);
    if (!NT_SUCCESS(status)) return status;
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != !!Directory)
        return STATUS_OBJECT_TYPE_MISMATCH;
    return STATUS_SUCCESS;
}

static void KeepStatus(NTSTATUS Status)
{
    HANDLE key;

    if (!NT_SUCCESS(OpenParameters(&key))) return;
    WriteDword(key, L"KeepStatus", (ULONG)Status);
    ZwFlushKey(key);
    ZwClose(key);
}

// A new episode: one device start, or one device stop. The next GuardLogKeep opens a file of its own for it; every
// keep after that appends to the same file. PASSIVE_LEVEL, and it touches nothing but these statics, so it is safe
// to call on a path that then never keeps anything (KeepLog closed, which is the release default).
void GuardLogKeepEpisode(_In_z_ PCWSTR Label)
{
    g_KeepPath[0] = 0;
    g_KeepBytes = 0;
    g_KeepFrom = 0;
    g_KeepAppends = 0;
    g_KeepLabel = Label;
}

// Keep at most BC250_KEEP_FILES of our ring files, the newest ones (driver/kmd/guard_keep_prune.h). Two passes over
// the directory: the first finds the oldest name that may stay, the second deletes what is older. Both are bounded,
// and so is the number of deletes in one prune: this runs inside a device start, and a directory that an older
// driver left with twelve thousand files must not hold that start for seconds. The rest goes at the next prune.
// Called once per episode, when its file is created, so the directory gains one file and loses at least one.
static void KeepPrune(HANDLE Directory)
{
    UNICODE_STRING pattern, path;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    BC250_KEEP_PRUNE* table;
    UCHAR* buffer;
    FILE_NAMES_INFORMATION* entry;
    WCHAR leaf[BC250_KEEP_NAME];
    ULONG offset, length, deleted = 0, failed = 0, pass, i;
    NTSTATUS status;

    table = (BC250_KEEP_PRUNE*)ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*table), BC250_TAG);
    buffer = (UCHAR*)ExAllocatePool2(POOL_FLAG_PAGED, BC250_KEEP_SCAN_BYTES, BC250_TAG);
    if (table == NULL || buffer == NULL)
    {
        if (table != NULL) ExFreePoolWithTag(table, BC250_TAG);
        if (buffer != NULL) ExFreePoolWithTag(buffer, BC250_TAG);
        return;
    }
    Bc250KeepPruneBegin(table);
    RtlInitUnicodeString(&pattern, BC250_KEEP_PATTERN);
    for (pass = 0; pass < 2; pass++)
    {
        BOOLEAN restart = TRUE;

        if (pass == 1 && Bc250KeepPruneLimit(table) == NULL) break;
        for (;;)
        {
            status = ZwQueryDirectoryFile(Directory, NULL, NULL, NULL, &io, buffer, BC250_KEEP_SCAN_BYTES,
                                          FileNamesInformation, FALSE, &pattern, restart);
            restart = FALSE;
            if (!NT_SUCCESS(status)) break;             // STATUS_NO_MORE_FILES ends it
            offset = 0;
            for (;;)
            {
                entry = (FILE_NAMES_INFORMATION*)(buffer + offset);
                length = entry->FileNameLength / sizeof(WCHAR);
                if (pass == 0) (void)Bc250KeepPruneOffer(table, entry->FileName, length);
                else if (Bc250KeepPruneGoes(table, entry->FileName, length))
                {
                    if (deleted + failed >= BC250_KEEP_DELETE_MAX) break;
                    // Bc250KeepPruneGoes has accepted the name, so it is shorter than the buffer. Terminated here,
                    // because a directory entry is not: the printing below takes a string, not a counted name.
                    for (i = 0; i < length; i++) leaf[i] = entry->FileName[i];
                    leaf[length] = 0;
                    RtlInitUnicodeString(&path, leaf);
                    InitializeObjectAttributes(&attributes, &path,
                        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, Directory, NULL);
                    if (NT_SUCCESS(ZwDeleteFile(&attributes))) deleted++; else failed++;
                }
                if (entry->NextEntryOffset == 0) break;
                offset += entry->NextEntryOffset;
                if (offset >= BC250_KEEP_SCAN_BYTES) break;     // a malformed chain must not walk off the buffer
            }
            if (pass == 1 && deleted + failed >= BC250_KEEP_DELETE_MAX) break;
        }
    }
    if (table->Offered > BC250_KEEP_FILES || deleted != 0 || failed != 0)
        GuardLog("keeplog: %lu ring file(s), %lu over the limit of %lu, %lu deleted, %lu refused",
                 (ULONG)table->Offered, Bc250KeepPruneGoing(table), (ULONG)BC250_KEEP_FILES, deleted, failed);
    ExFreePoolWithTag(buffer, BC250_TAG);
    ExFreePoolWithTag(table, BC250_TAG);
}

void GuardLogKeep(void)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    HANDLE directory = NULL;
    HANDLE file = NULL;
    LARGE_INTEGER now, offset;
    TIME_FIELDS t;
    BC250_LOG_LINE* page = NULL;
    char* text = NULL;
    char* cursor;
    size_t left;
    ULONG from, next = 0, returned, total, lost, above, i, rounds;
    BOOLEAN created = FALSE;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (GuardReadSetting(L"KeepLog", 0) == 0) return;
    if (g_KeepRoot == NULL) { KeepStatus(STATUS_INVALID_DEVICE_STATE); return; }

    // The directory first; FILE_OPEN_IF makes this a no-op when it is there, and a failure shows in the create
    // below. The handle stays open while a new file is made, because the prune enumerates through it.
    RtlInitUnicodeString(&path, BC250_KEEP_DIRECTORY);
    InitializeObjectAttributes(&attributes, &path,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, g_KeepRoot, NULL);
    status = ZwCreateFile(&directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &io, NULL,
                          FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, NULL, 0);
    if (!NT_SUCCESS(status)) { KeepStatus(status); return; }
    status = KeepCheckFile(directory, TRUE);
    if (!NT_SUCCESS(status)) { ZwClose(directory); KeepStatus(status); return; }

    if (g_KeepPath[0] == 0)
    {
        KeQuerySystemTime(&now);
        RtlTimeToTimeFields(&now, &t);
        status = RtlStringCchPrintfW(g_KeepPath, RTL_NUMBER_OF(g_KeepPath),
                                     L"ring-%04d%02d%02d-%02d%02d%02d-%03d-%ws.log",
                                     t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second, t.Milliseconds,
                                     g_KeepLabel);
        if (!NT_SUCCESS(status))
        {
            g_KeepPath[0] = 0;
            if (directory != NULL) ZwClose(directory);
            KeepStatus(status);
            return;
        }
        // Before the new file, so that the file this episode is about to write is never a candidate for the prune.
        if (directory != NULL) KeepPrune(directory);
        created = TRUE;
    }
    RtlInitUnicodeString(&path, g_KeepPath);
    InitializeObjectAttributes(&attributes, &path,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, directory, NULL);
    status = ZwCreateFile(&file, FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          created ? FILE_OVERWRITE_IF : FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, NULL, 0);
    ZwClose(directory);
    if (NT_SUCCESS(status)) {
        status = KeepCheckFile(file, FALSE);
        if (!NT_SUCCESS(status)) { ZwClose(file); file = NULL; }
    }
    if (!NT_SUCCESS(status))
    {
        // The file of this episode is gone (deleted by hand, another volume): the next keep starts a new one rather
        // than failing for the rest of the episode.
        g_KeepPath[0] = 0;
        KeepStatus(status);
        return;
    }
    if (created) { g_KeepBytes = 0; g_KeepFrom = 0; }
    from = g_KeepFrom;
    g_KeepAppends++;

    page = (BC250_LOG_LINE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, BC250_LOG_MAX_LINES * sizeof(BC250_LOG_LINE), BC250_TAG);
    text = (char*)ExAllocatePool2(POOL_FLAG_PAGED, BC250_LOG_MAX_LINES * BC250_LOG_KEEP_LINE, BC250_TAG);
    if (page != NULL && text != NULL)
    {
        GuardLogStats(&total, &lost, &above);
        cursor = text;
        left = BC250_LOG_MAX_LINES * BC250_LOG_KEEP_LINE;
        if (created)
            RtlStringCchPrintfExA(cursor, left, &cursor, &left, 0,
                                  "bc250kmd 0x%08X log snapshot, %ws: %lu lines, %lu lost to the wrap, %lu dropped "
                                  "above DISPATCH_LEVEL\r\n", BC250_KMD_VERSION, g_KeepLabel, total, lost, above);
        else
            RtlStringCchPrintfExA(cursor, left, &cursor, &left, 0,
                                  "-- keep %lu from sequence %lu: %lu lines, %lu lost to the wrap, %lu dropped "
                                  "above DISPATCH_LEVEL\r\n", g_KeepAppends, from, total, lost, above);
        offset.QuadPart = g_KeepBytes;
        status = ZwWriteFile(file, NULL, NULL, NULL, &io, text, (ULONG)(cursor - text), &offset, NULL);
        if (NT_SUCCESS(status)) g_KeepBytes += (LONGLONG)io.Information;

        // A page of the ring at a time, the way the escape reads it. The round count is a bound, not a need: the
        // ring holds RING_LINES and nothing logs during a stop, but a loop over a live structure gets a limit.
        for (rounds = 0; NT_SUCCESS(status) && rounds < BC250_LOG_RING_LINES / BC250_LOG_MAX_LINES + 2; rounds++)
        {
            returned = GuardLogRead(from, page, BC250_LOG_MAX_LINES, &next);
            if (returned == 0) break;
            cursor = text;
            left = BC250_LOG_MAX_LINES * BC250_LOG_KEEP_LINE;
            for (i = 0; i < returned; i++)
            {
                page[i].Text[BC250_LOG_TEXT - 1] = 0;
                RtlStringCchPrintfExA(cursor, left, &cursor, &left, 0, "%6lu %6lu.%03lu %s\r\n", page[i].Sequence,
                                      page[i].Milliseconds / 1000, page[i].Milliseconds % 1000, page[i].Text);
            }
            // The one write whose failure matters: a full volume leaves a file that exists and is short, which
            // is the most misleading thing this function could produce.
            offset.QuadPart = g_KeepBytes;
            status = ZwWriteFile(file, NULL, NULL, NULL, &io, text, (ULONG)(cursor - text), &offset, NULL);
            if (!NT_SUCCESS(status)) break;
            g_KeepBytes += (LONGLONG)io.Information;
            g_KeepFrom = next;
            if (next <= from) break;
            from = next;
        }
        // KeepStatus is the only voice this function has, so a file that is short or not on the disk must not
        // be reported as kept.
        if (NT_SUCCESS(status)) status = ZwFlushBuffersFile(file, &io);
    }
    else
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (text != NULL) ExFreePoolWithTag(text, BC250_TAG);
    if (page != NULL) ExFreePoolWithTag(page, BC250_TAG);
    ZwClose(file);
    KeepStatus(status);
}

// BD-195: a hot-plug step that stops the whole machine leaves no ring to read after the power cycle. With KeepLog
// set, the connector paths append the ring to the keep file before each new step, so the last line on the disk
// names the step that never returned. The keep is synchronous file I/O: PASSIVE_LEVEL with special kernel APCs on.
BOOLEAN GuardKeepActive(void)
{
    return KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreAllApcsDisabled() && GuardReadSetting(L"KeepLog", 0) != 0;
}

static void GuardStartConfirmed(void);

// Used only after the typed health policy has accepted the live start identity.
NTSTATUS GuardConfirmStartDurable(void)
{
    HANDLE key;
    NTSTATUS status=OpenParameters(&key);
    if (!NT_SUCCESS(status)) return status;
    status=WriteDword(key,L"UnconfirmedStarts",0);
    if (NT_SUCCESS(status)) status=ZwFlushKey(key);
    ZwClose(key);
    GuardLog("guard: healthy start confirmation persistence 0x%08X",status);
    if (NT_SUCCESS(status)) GuardStartConfirmed();
    return status;
}

// ---- settings the driver itself owns (cumode.c) -------------------------------------------------------------------
//
// GuardReadSetting folds "absent" into a default; the CU mode's boot guard must tell absent from zero and must know
// that a write reached the disk before the registers it protects are touched. PASSIVE_LEVEL only.

NTSTATUS GuardQuerySetting(_In_z_ PCWSTR Name, _Out_ ULONG* Value)
{
    // Settings resolve their package default; records still distinguish absent
    // from zero. No malformed value or API failure becomes an absent override.
    return DriverStateNumber(L"",Name,TRUE,Value);
}

// Written and flushed: success means it is on the disk.
NTSTATUS GuardStoreSetting(_In_z_ PCWSTR Name, ULONG Value)
{
    return DriverStateSet(L"",Name,REG_DWORD,&Value,sizeof(Value));
}

// Deleted and flushed; a value that was not there is success.
NTSTATUS GuardDeleteSetting(_In_z_ PCWSTR Name)
{
    return DriverStateClear(L"",Name);
}

// A volatile child of service state: the configuration manager drops it at every reboot, which is exactly the life
// of what it holds (this boot's firmware values, cumode.c). Never flushed; there is no disk copy to flush.
static NTSTATUS OpenVolatile(_In_z_ PCWSTR Subkey, BOOLEAN Create, _Out_ HANDLE* Key)
{
    return DriverStateOpenVolatile(Subkey,Create,Key);
}

NTSTATUS GuardVolatileQuery(_In_z_ PCWSTR Subkey, _In_z_ PCWSTR Name, _Out_ ULONG* Value)
{
    HANDLE key;
    NTSTATUS status;

    *Value = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) return STATUS_INVALID_DEVICE_STATE;
    status = OpenVolatile(Subkey, FALSE, &key);
    if (!NT_SUCCESS(status)) return status;
    status = ReadDword(key, Name, Value);
    ZwClose(key);
    return status;
}

NTSTATUS GuardVolatileStore(_In_z_ PCWSTR Subkey, _In_z_ PCWSTR Name, ULONG Value)
{
    HANDLE key;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) return STATUS_INVALID_DEVICE_STATE;
    status = OpenVolatile(Subkey, TRUE, &key);
    if (!NT_SUCCESS(status)) return status;
    status = WriteDword(key, Name, Value);
    ZwClose(key);
    return status;
}

// ---- the M15.12 hang-recovery record (docs/design/hang-recovery.md) -----------------------------------------------
//
//   <service key>\Parameters\HangRecovery   non-volatile, written and flushed by DxgkDdiResetEngine (wddm.c)
//     Attempts, Recovered, NotDrained, Refused  REG_DWORD  counters (Bc250HangVerdictCounts), never reset here
//     LastVerdict                               REG_DWORD  BC250_HANG_VERDICT_*; 0 = a kill was under way
//     LastSeq, LastFence                        REG_DWORD  the newest ring sequence, the OS fence it would abort
//                                               verdict 8: LastSeq is BC250_HANG_ADMISSION_*; LastFence is 0
//     LastKills, LastMicros                     REG_DWORD  SQ_CMD writes issued, time spent in the kill loop
//     LastTime                                  REG_QWORD  KeQuerySystemTime of this write (UTC FILETIME)
//     LastVersion                               REG_DWORD  BC250_KMD_VERSION of the writer
//     WavesPending                              REG_DWORD  capture admitted, no snapshot persisted yet
//
// The log ring tells the same story in prose, but it is about 1024 lines in memory and does not outlive the 0x116
// that a refusal leads to; this does, because every write is flushed before the caller goes on (the pending one
// before the first SQ_CMD write, the verdict before ResetEngine returns). A subkey rather than values of
// Parameters: the kmd-deploy kit compares the value names of Parameters with its capture and recover-after-boot.ps1
// writes captured values back, and neither looks at subkeys, so the record is neither a postflight difference nor
// overwritten by a recovery. PASSIVE_LEVEL only. A failure is logged and otherwise ignored: evidence, never a gate.
static NTSTATUS AddOne(HANDLE Key, PCWSTR Name)
{
    ULONG value;
    NTSTATUS status = ReadDword(Key, Name, &value);     // sets value to 0 first, so absent counts from 0

    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND) return status;
    return WriteDword(Key, Name, value + 1);
}

void GuardRecordHangRecovery(ULONG Verdict, ULONG Seq, ULONG Fence, ULONG Kills, ULONG Micros, BOOLEAN WavesPending)
{
    HANDLE parameters, key;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    LARGE_INTEGER now;
    ULONG counts = Bc250HangVerdictCounts(Verdict);
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) return;
    status = OpenParameters(&parameters);
    if (!NT_SUCCESS(status)) {
        GuardLog("hang record: verdict %lu NOT persisted, Parameters open 0x%08X", Verdict, status);
        return;
    }
    RtlInitUnicodeString(&name, L"HangRecovery");
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, parameters, NULL);
    status = ZwCreateKey(&key, KEY_READ | KEY_WRITE, &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    ZwClose(parameters);
    if (!NT_SUCCESS(status)) {
        GuardLog("hang record: verdict %lu NOT persisted, subkey create 0x%08X", Verdict, status);
        return;
    }
    if (NT_SUCCESS(status) && (counts & BC250_HANG_COUNT_ATTEMPT)) status = AddOne(key, L"Attempts");
    if (NT_SUCCESS(status) && (counts & BC250_HANG_COUNT_RECOVERED)) status = AddOne(key, L"Recovered");
    if (NT_SUCCESS(status) && (counts & BC250_HANG_COUNT_NOT_DRAINED)) status = AddOne(key, L"NotDrained");
    if (NT_SUCCESS(status) && (counts & BC250_HANG_COUNT_REFUSED)) status = AddOne(key, L"Refused");
    if (NT_SUCCESS(status)) status = WriteDword(key, L"LastVerdict", Verdict);
    if (NT_SUCCESS(status)) status = WriteDword(key, L"LastSeq", Seq);
    if (NT_SUCCESS(status)) status = WriteDword(key, L"LastFence", Fence);
    if (NT_SUCCESS(status)) status = WriteDword(key, L"LastKills", Kills);
    if (NT_SUCCESS(status)) status = WriteDword(key, L"LastMicros", Micros);
    if (NT_SUCCESS(status)) status = WriteDword(key, L"LastVersion", BC250_KMD_VERSION);
    // A new attempt replaces the marker, including when capture is disabled.
    // Final verdicts do not hide a failed or interrupted snapshot write.
    if (NT_SUCCESS(status) && Verdict == BC250_HANG_VERDICT_PENDING)
        status = WriteDword(key, L"WavesPending", WavesPending ? 1u : 0u);
    if (NT_SUCCESS(status)) {
        KeQuerySystemTime(&now);
        RtlInitUnicodeString(&name, L"LastTime");
        status = ZwSetValueKey(key, &name, 0, REG_QWORD, &now.QuadPart, sizeof(now.QuadPart));
    }
    if (NT_SUCCESS(status)) status = ZwFlushKey(key);   // on the disk before the caller kills, or refuses into 0x116
    ZwClose(key);
    GuardLog("hang record: verdict %lu seq %lu fence %lu kills %lu %lu us, persisted 0x%08X", Verdict, Seq, Fence,
             Kills, Micros, status);
}
// The buffer is driver-owned and initialized, never an escape pointer. Keep the
// failure header too: an empty partial read must not leave an old success visible.
NTSTATUS GuardRecordWaveSnapshot(const BC250_WAVE_HEADER* Header)
{
    HANDLE parameters, recovery, key;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    SECURITY_DESCRIPTOR descriptor;
    union { ULONG Align; UCHAR Bytes[128]; } aclStorage;
    PACL acl = (PACL)aclStorage.Bytes;
    // S-1-5-18 (SYSTEM) and S-1-5-32-544 (Administrators). Shader instructions
    // can belong to another client: keep the saved snapshot behind the live
    // command's administrator boundary, without restricting existing counters.
    static const ULONG systemSid[] = {0x00000101u, 0x05000000u, 18u};
    static const ULONG adminSid[] = {0x00000201u, 0x05000000u, 32u, 544u};
    NTSTATUS status;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady())
        return STATUS_INVALID_DEVICE_STATE;
    if (Header->AbiVersion != BC250_WAVE_ABI_VERSION ||
        Header->WaveCount > BC250_WAVE_MAX_RECORDS || Header->Kind != BC250_WAVE_KIND_PRE_KILL ||
        Header->Bytes != sizeof(*Header) + Header->WaveCount * sizeof(BC250_WAVE_RECORD))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&aclStorage, sizeof(aclStorage));
    status = RtlCreateSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
    if (NT_SUCCESS(status)) status = RtlCreateAcl(acl, sizeof(aclStorage.Bytes), ACL_REVISION);
    if (NT_SUCCESS(status)) status = RtlAddAccessAllowedAce(acl, ACL_REVISION, KEY_ALL_ACCESS, (PSID)systemSid);
    if (NT_SUCCESS(status)) status = RtlAddAccessAllowedAce(acl, ACL_REVISION, KEY_READ | DELETE, (PSID)adminSid);
    if (NT_SUCCESS(status)) status = RtlSetDaclSecurityDescriptor(&descriptor, TRUE, acl, FALSE);
    if (NT_SUCCESS(status)) status = RtlSetOwnerSecurityDescriptor(&descriptor, (PSID)systemSid, FALSE);
    if (!NT_SUCCESS(status)) return status;
    descriptor.Control |= SE_DACL_PROTECTED;
    status = OpenParameters(&parameters);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, L"HangRecovery");
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, parameters, NULL);
    status = ZwCreateKey(&recovery, KEY_READ | KEY_WRITE, &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    ZwClose(parameters);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, L"Waves");
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, recovery, &descriptor);
    status = ZwCreateKey(&key, KEY_READ | KEY_WRITE | WRITE_DAC | WRITE_OWNER, &attributes, 0, NULL,
                         REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(status)) { ZwClose(recovery); return status; }
    status = ZwSetSecurityObject(key, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                PROTECTED_DACL_SECURITY_INFORMATION, &descriptor);
    RtlInitUnicodeString(&name, L"Snapshot");
    if (NT_SUCCESS(status)) status = ZwSetValueKey(key, &name, 0, REG_BINARY, (PVOID)Header, Header->Bytes);
    // Data first: a failed write must retain the pending marker. One SYSTEM
    // hive flush then persists both the snapshot and its completion marker.
    if (NT_SUCCESS(status)) status = WriteDword(recovery, L"WavesPending", 0);
    if (NT_SUCCESS(status)) status = ZwFlushKey(key);
    ZwClose(key);
    ZwClose(recovery);
    return status;
}

// ---- BD-122: why a start failed, where a Code 43 device can still be read ------------------------------------
//
// GitHub #38: two testers get CM_PROB_FAILED_POST_START, and start-confirm.log said only "device problem 43,
// LastStage 91" at every logon. Stage 91 is StageStartFailed, which Bc250StartDevice reaches from any of its
// `goto failed` steps, and the NTSTATUS went to the log ring alone ("start failed 0x%08X"). A device with Code 43
// answers no escape, so nothing could read that ring: the tester could not name the step and neither could we.
// GuardLogKeep would have kept it in a file, but it is gated on Parameters\KeepLog (0 in every release) and is
// not called on that path anyway.
//
// So every start attempt that does not end with a started device now leaves its reason in the registry, with no
// gate at all. A subkey of Parameters, for the reason GuardRecordHangRecovery gives above: the kmd-deploy kit
// compares the VALUE NAMES of Parameters against its capture and recover-after-boot.ps1 writes captured values
// back, and neither walks subkeys, so this record is neither a postflight difference nor overwritten by a
// recovery.
//
//   <service key>\Parameters\StartRecord
//     Result             REG_SZ     attempt | started | failed | guard   (the only value a reader may trust)
//     Step               REG_SZ     the step of Bc250StartDevice, "-" when none was named
//     SubStep            REG_SZ     the step inside WddmStart / GpuStartupInitialize, "-" when none
//     Status             REG_DWORD  the NTSTATUS of the failure (0 for attempt and started)
//     Stage              REG_DWORD  BC250_STAGE at the record: 91 for a failure, 90 for the guard's refusal
//     UnconfirmedStarts  REG_DWORD  the boot-loop count read from Parameters at the record
//     Version            REG_DWORD  BC250_KMD_VERSION, so a reader knows which driver wrote this
//     TimeUtc            REG_SZ     UTC, yyyy-mm-ddThh:mm:ss.fffZ
//     Detail             REG_SZ     measured values of the refusing step (GuardStartDetail), failures only
//     Log                REG_MULTI_SZ  the BC250_START_RECORD_LINES ring lines that run UP TO the failure point
//                                   (GuardStartFailureMark), oldest first, failures only - not the teardown's own
//
// THE RULE THAT KEEPS THE RECORD CURRENT (requirement 2 of the defect, and what a consumer must implement):
// `Result` is written as "attempt" by GuardRecordStartAttempt at the top of every Bc250StartDevice, flushed,
// before any step of that start can fail - and `Log` is deleted in the same write. A failure then writes its
// values and sets `Result` to "failed" or "guard" LAST; a started device sets it to "started". Therefore
//   * Result "failed" or "guard" always belongs to the newest start attempt, and the step, status and lines next
//     to it are that attempt's;
//   * Result "started" means the newest attempt reached a started device, whatever an older boot had left here;
//   * Result "attempt" means the newest attempt neither failed nor finished - the machine stopped inside the
//     start (a bugcheck, a hang, AC loss), which is itself the answer. Such a record names NO step: it is written
//     once, at the top of the start, before any step runs, so Step is "-" and Stage is StageStartEnter. How far
//     that start got is in Parameters\LastStage and Parameters\StageHistory, which every stage flushes; the
//     consumers point a reader there. "attempt" also stands where a write of the record itself was torn (below).
// A stale failure can never be read as the current one, because the current attempt overwrote Result before it
// could fail, every value of the record is written again beside it, and a value that cannot be written is deleted
// rather than left standing. One case is outside this rule and cannot be inside it: a start refused above the
// attempt record, which is a device object this driver does not own (pnp.c) and must leave no trace for. The
// record then still belongs to the previous start, and the consumers compare TimeUtc with the boot's own time and
// say so. A reader that finds no subkey at all is reading a KMD older than 0.7.216.30.
//
// PASSIVE_LEVEL only, like every other writer in this file, and called from the PnP start path, which dxgkrnl
// serializes. A failure of any of these writes is logged in one line and otherwise ignored: this is evidence,
// never a gate, and a start must not fail because the registry did (requirement 4).
#define BC250_START_RECORD_LINES 64u            // the newest ring lines kept with a failure
#define BC250_START_RECORD_CHUNK 8u             // lines per GuardLogRead call: 8 * sizeof(BC250_LOG_LINE) of stack

// The step names. Written by the PnP start thread only (one start at a time, serialized by dxgkrnl) and read by
// the record writer on that same thread, so no lock and no interlocked access: a pointer to a string literal of
// this image, which the record writer cannot outlive. Safe at any IRQL - they are two stores.
static PCWSTR volatile g_StartStep;
static PCWSTR volatile g_StartSubStep;

void GuardStartStep(_In_opt_z_ PCWSTR Step)
{
    g_StartStep = Step;
    g_StartSubStep = NULL;      // a new step owns no sub-step until one names itself
}

void GuardStartSubStep(_In_opt_z_ PCWSTR SubStep)
{
    g_StartSubStep = SubStep;
}

// The ring sequence at the moment a failure became known, before any cleanup of that failure ran. The record is
// written last on the failure path, and the teardown of a GPU that had already started writes dozens of GuardLog
// lines of its own (WddmStop, GartStop, PspStop, GfxStop and the rest): without this mark the 64 kept lines would
// hold that unwind and not the step that refused, which is the one thing the record exists for. The first caller
// of a start wins, because the earliest failure point is where the lines must end. Same thread and lifetime as the
// step names: one load and two stores, safe at any IRQL.
static ULONG volatile g_StartLogMark;
static BOOLEAN volatile g_StartLogMarked;

void GuardStartFailureMark(void)
{
    if (g_StartLogMarked) return;
    g_StartLogMark = GuardLogSequence();
    g_StartLogMarked = TRUE;
}

// GitHub #38: one line of measured values for a step whose NTSTATUS alone does not say which rule refused it (the
// inherited timing). Same thread and lifetime as the step names: every attempt clears it, a failure or a refusal
// writes it as Detail, every other record deletes Detail. Integer conversions only, so any IRQL; a line too long
// for the buffer is kept truncated.
static WCHAR g_StartDetail[320];

void GuardStartDetail(_In_z_ _Printf_format_string_ PCWSTR Format, ...)
{
    va_list args;
    va_start(args, Format);
    (void)RtlStringCchVPrintfW(g_StartDetail, RTL_NUMBER_OF(g_StartDetail), Format, args);
    va_end(args);
}

static NTSTATUS WriteString(HANDLE Key, PCWSTR Name, PCWSTR Value)
{
    UNICODE_STRING name;

    RtlInitUnicodeString(&name, Name);
    return ZwSetValueKey(Key, &name, 0, REG_SZ, (PVOID)Value, (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR)));
}

static void DeleteString(HANDLE Key, PCWSTR Name)
{
    UNICODE_STRING name;

    RtlInitUnicodeString(&name, Name);
    (void)ZwDeleteValueKey(Key, &name);     // absent is the wanted end state too: nothing depends on the result
}

// UTC, because that is the clock every log in this project uses. KeQuerySystemTime is already UTC.
static void StartRecordTime(_Out_writes_(Chars) PWSTR Buffer, SIZE_T Chars)
{
    LARGE_INTEGER now;
    TIME_FIELDS fields;

    Buffer[0] = 0;
    KeQuerySystemTime(&now);
    RtlTimeToTimeFields(&now, &fields);
    if (!NT_SUCCESS(RtlStringCchPrintfW(Buffer, Chars, L"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", (int)fields.Year,
                                        (int)fields.Month, (int)fields.Day, (int)fields.Hour, (int)fields.Minute,
                                        (int)fields.Second, (int)fields.Milliseconds)))
        Buffer[0] = 0;
}

// The ring lines that run up to the failure as one REG_MULTI_SZ, oldest first: exactly the text GuardLog wrote, so
// a line here never holds more than bc250kmd_cli log would print (at most 159 characters, BC250_LOG_TEXT). Mark is
// the sequence the failure was noticed at, or 0 for the newest lines of all (a refusal that runs no cleanup); every
// line at or above it belongs to the unwind that followed and is left out. The buffer comes from paged pool -
// 64 * 160 WCHAR is far too much for a PnP stack - and the lines are read in small chunks.
static NTSTATUS StartRecordLog(HANDLE Key, ULONG Mark, _Out_ ULONG* Written)
{
    const SIZE_T chars = (SIZE_T)BC250_START_RECORD_LINES * BC250_LOG_TEXT + 1;
    BC250_LOG_LINE chunk[BC250_START_RECORD_CHUNK];
    UNICODE_STRING name;
    PWSTR buffer;
    SIZE_T used = 0;
    ULONG total, lost, above, from, next, got, i, lines = 0;
    BOOLEAN past = FALSE;
    NTSTATUS status;

    *Written = 0;
    buffer = (PWSTR)ExAllocatePool2(POOL_FLAG_PAGED, chars * sizeof(WCHAR), BC250_TAG);
    if (buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    GuardLogStats(&total, &lost, &above);
    if (Mark != 0 && Mark < total) total = Mark;    // the failure point, not the end of the teardown's lines
    from = (total > BC250_START_RECORD_LINES) ? total - BC250_START_RECORD_LINES : 0;
    while (lines < BC250_START_RECORD_LINES && !past)
    {
        got = GuardLogRead(from, chunk, BC250_START_RECORD_CHUNK, &next);
        if (got == 0) break;
        for (i = 0; i < got && lines < BC250_START_RECORD_LINES; i++)
        {
            const char* text = chunk[i].Text;
            SIZE_T j = 0;

            // At or past the failure point: everything from here on is the cleanup that ran after the failure, and
            // the ring may have wrapped past the mark while it ran. The sequence numbers only grow, so stop.
            if (chunk[i].Sequence >= total) { past = TRUE; break; }
            // An empty line would end the REG_MULTI_SZ in the middle of it. GuardLog never writes one; skip it
            // rather than lose every line after it.
            if (text[0] == '\0') continue;
            // ANSI to WCHAR by hand: the ring holds printable ASCII that this driver printed itself, so there is
            // no locale, no conversion table and no allocation in the way.
            while (text[j] != '\0' && j + 1 < BC250_LOG_TEXT && used + j + 2 <= chars)
            {
                buffer[used + j] = (WCHAR)(UCHAR)text[j];
                j++;
            }
            buffer[used + j] = L'\0';
            used += j + 1;
            lines++;
        }
        if (next == from) break;        // the ring gave what it has; do not spin on it
        from = next;
    }
    buffer[used] = L'\0';               // the empty string that ends a REG_MULTI_SZ
    RtlInitUnicodeString(&name, L"Log");
    status = ZwSetValueKey(Key, &name, 0, REG_MULTI_SZ, buffer, (ULONG)((used + 1) * sizeof(WCHAR)));
    ExFreePoolWithTag(buffer, BC250_TAG);
    *Written = lines;
    return status;
}

// A value this record could not write is removed instead of left behind: an older start's value beside a newer
// Result is the one lie the record must never tell. A failed delete can only mean the hive is beyond our reach,
// and the Result write below is then skipped anyway, so its status is nothing to act on.
static NTSTATUS WriteStringOrClear(HANDLE Key, PCWSTR Name, PCWSTR Value)
{
    NTSTATUS status = WriteString(Key, Name, Value);

    if (!NT_SUCCESS(status)) DeleteString(Key, Name);
    return status;
}

static NTSTATUS WriteDwordOrClear(HANDLE Key, PCWSTR Name, ULONG Value)
{
    NTSTATUS status = WriteDword(Key, Name, Value);

    if (!NT_SUCCESS(status)) DeleteString(Key, Name);
    return status;
}

// The first failure of a record's writes, for the one decision that depends on it: whether the real Result may be
// written at all. Every write is still attempted, whatever an earlier one answered.
static void StartRecordKeep(_Inout_ NTSTATUS* First, NTSTATUS Status)
{
    if (NT_SUCCESS(*First) && !NT_SUCCESS(Status)) *First = Status;
}

// One record. Result is what a reader trusts (see the rule above); Failed says whether the ring lines belong with
// it. Every failure here ends in one GuardLog line and nothing else: the caller's status is never touched.
static void StartRecordWrite(_In_z_ PCWSTR Result, BOOLEAN Failed, NTSTATUS Reason)
{
    HANDLE parameters, key;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    WCHAR time[32];
    PCWSTR step = g_StartStep ? g_StartStep : L"-";
    PCWSTR sub = g_StartSubStep ? g_StartSubStep : L"-";
    ULONG mark = g_StartLogMark;        // 0 until a failure path marked one: then the lines end there
    ULONG starts = 0, lines = 0;
    NTSTATUS status, first = STATUS_SUCCESS, logStatus = STATUS_SUCCESS;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !DriverStateReady()) {
        GuardLog("start record %ws: not written, IRQL or no Parameters path", Result);
        return;
    }
    status = OpenParameters(&parameters);
    if (!NT_SUCCESS(status)) {
        GuardLog("start record %ws: not written, Parameters open 0x%08X", Result, status);
        return;
    }
    // The boot-loop count belongs with a guard refusal and tells a failure apart from a refusal; ReadDword sets
    // it to 0 first, so an absent or unreadable value records 0 rather than a stale number.
    (void)ReadDword(parameters, L"UnconfirmedStarts", &starts);
    RtlInitUnicodeString(&name, L"StartRecord");
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, parameters, NULL);
    status = ZwCreateKey(&key, KEY_READ | KEY_WRITE, &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    ZwClose(parameters);
    if (!NT_SUCCESS(status)) {
        GuardLog("start record %ws: not written, subkey create 0x%08X", Result, status);
        return;
    }
    StartRecordTime(time, RTL_NUMBER_OF(time));
    // "attempt" first on every path: a torn or half-failed write then leaves a record nobody reads as a reason,
    // instead of a reason belonging to another start. The real Result is the last value written.
    //
    // No write below is skipped because an earlier one failed, and a value that could not be written is deleted:
    // a short-circuit here would leave an older boot's Step, Status or Log standing beside this start's fresh
    // Result, which a reader would then print as the current reason (a damaged or full hive is exactly the kind of
    // machine this record is for). `first` only decides whether the real Result may be written at all; if any value
    // of the record is missing, Result stays "attempt", which no consumer reads as a reason.
    StartRecordKeep(&first, WriteStringOrClear(key, L"Result", L"attempt"));
    StartRecordKeep(&first, WriteStringOrClear(key, L"Step", step));
    StartRecordKeep(&first, WriteStringOrClear(key, L"SubStep", sub));
    StartRecordKeep(&first, WriteDwordOrClear(key, L"Status", (ULONG)Reason));
    StartRecordKeep(&first, WriteDwordOrClear(key, L"Stage", (ULONG)GuardLastStage()));
    StartRecordKeep(&first, WriteDwordOrClear(key, L"UnconfirmedStarts", starts));
    StartRecordKeep(&first, WriteDwordOrClear(key, L"Version", BC250_KMD_VERSION));
    StartRecordKeep(&first, WriteStringOrClear(key, L"TimeUtc", time));
    if (Failed && g_StartDetail[0] != L'\0') StartRecordKeep(&first, WriteStringOrClear(key, L"Detail", g_StartDetail));
    else DeleteString(key, L"Detail");
    // The lines belong to the failure they were written for. An attempt or a started device deletes them, and so
    // does a failed write of them, so that no reader can pair this start's Result with another start's log.
    if (Failed) {
        logStatus = StartRecordLog(key, mark, &lines);
        if (!NT_SUCCESS(logStatus)) { DeleteString(key, L"Log"); lines = 0; }
    } else DeleteString(key, L"Log");
    status = first;
    if (NT_SUCCESS(status)) status = WriteStringOrClear(key, L"Result", Result);
    if (NT_SUCCESS(status)) status = ZwFlushKey(key);   // on the disk before the start returns Code 43 to dxgkrnl
    ZwClose(key);
    GuardLog("start record %ws %ws %ws 0x%08X, %u lines, write 0x%08X", Result, step, sub, Reason, lines, status);
    if (!NT_SUCCESS(logStatus)) GuardLog("start record %ws: log lines not kept, 0x%08X", Result, logStatus);
}

// The top of Bc250StartDevice: this attempt owns the record from here on. Nothing of the last start is left to
// pair with it - not its step, not its detail line and not its failure point.
void GuardRecordStartAttempt(void)
{
    GuardStartStep(NULL);
    g_StartDetail[0] = L'\0';
    g_StartLogMark = 0;
    g_StartLogMarked = FALSE;
    StartRecordWrite(L"attempt", FALSE, STATUS_SUCCESS);
}

// The boot-loop guard refused the start (stage 90). UnconfirmedStarts in the record says how many starts nobody
// confirmed; BC250_MAX_UNCONFIRMED_STARTS is the limit it reached.
void GuardRecordStartRefused(NTSTATUS Status)
{
    StartRecordWrite(L"guard", TRUE, Status);
}

// A step of Bc250StartDevice failed (stage 91). Called after GuardStage(StageStartFailed) and after the existing
// cleanup, so Stage is the stage a tester's start-confirm.log already prints; the lines still run up to the
// failure and not through the unwind, because the failure path marked that point (GuardStartFailureMark).
void GuardRecordStartFailed(NTSTATUS Status)
{
    StartRecordWrite(L"failed", TRUE, Status);
}

// The device is started (stage 39). Nothing in the record is a reason any more.
void GuardRecordStartStarted(void)
{
    GuardStartStep(NULL);
    StartRecordWrite(L"started", FALSE, STATUS_SUCCESS);
}

// ---- BD-090: a runtime restart in a confirmed boot -----------------------------------------------------------
//
// UnconfirmedStarts protects against a boot loop: a start that crashes the machine never reaches its stop, and
// dxgkrnl does not stop the adapter at a shutdown either, so across reboots the count only grows until a healthy
// desktop confirms it. The confirmation runs at logon (the start-confirm task). A runtime restart of the device
// (pnputil /restart-device, a live driver update, Device Manager) starts the driver again in the same boot, with
// no logon to confirm it: before this the second such restart was refused with Code 43 (b21 v2-restart cycle 3).
//
// The rule: an orderly stop of a start that completed gives its own count back, but only in a boot whose full
// table has once been confirmed healthy. The mark of that confirmation is a volatile key, which the configuration
// manager drops at every reboot, so the protection across reboots is unchanged: a crash, a power loss and a
// shutdown never reach the give-back, and a new boot has no mark until its own start is confirmed. A start that
// failed keeps its count (pnp.c calls this only for a device that was Started). No Linux counterpart: amdgpu has
// no start guard.
#define GUARD_BOOT_KEY L"GuardBoot"
#define GUARD_BOOT_CONFIRMED L"Confirmed"

static void GuardStartConfirmed(void)
{
    NTSTATUS status;

    g_StartCounted = FALSE;     // the caller zeroed the count: nothing of this start is left to give back
    status = GuardVolatileStore(GUARD_BOOT_KEY, GUARD_BOOT_CONFIRMED, 1);
    GuardLog("guard: boot marked confirmed 0x%08X", status);
}

void GuardReleaseStart(void)
{
    HANDLE key;
    ULONG starts, confirmed;
    NTSTATUS status;

    if (!g_StartCounted) return;
    g_StartCounted = FALSE;     // once per counted start, whatever happens below
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    status = GuardVolatileQuery(GUARD_BOOT_KEY, GUARD_BOOT_CONFIRMED, &confirmed);
    if (!NT_SUCCESS(status) || confirmed != 1) {
        GuardLog("guard: stop keeps the count, boot not confirmed (0x%08X)", status);
        return;
    }
    status = OpenParameters(&key);
    if (!NT_SUCCESS(status)) {
        GuardLog("guard: stop give-back open failed 0x%08X", status);
        return;
    }
    status = ReadDword(key, L"UnconfirmedStarts", &starts);
    // Never below zero: a value user mode already cleared stays cleared.
    if (NT_SUCCESS(status) && starts > 0) {
        status = WriteDword(key, L"UnconfirmedStarts", starts - 1);
        if (NT_SUCCESS(status)) status = ZwFlushKey(key);
        GuardLog("guard: orderly stop in a confirmed boot, count %u -> %u status 0x%08X", starts, starts - 1, status);
    }
    ZwClose(key);
}

// StopDevice can follow a successful StartDevice before Windows admits the
// adapter. That one case is what this record is for, and Device->Started is
// what separates it from the start-failure path, which unwinds through this
// same StopDevice: see the Device->Started gate below.
// KeepLog is not needed for this record. StartRecord remains intact.
// Result=attempt is incomplete; stopping means the entry snapshot is complete;
// stopped means StageStopDone was reached. This does not invent an OS refusal
// status: StopDevice has no argument with dxgkrnl's reason for stopping it.
// StartStage/HealthFlags precede teardown. Log ends at that same entry point,
// not after cleanup has replaced the ring. BootId and generation identify it.
static BOOLEAN g_StopRecordPending;

static NTSTATUS StopRecordOpen(_Out_ HANDLE* Key)
{
    HANDLE parameters;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status=OpenParameters(&parameters);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name,L"StopRecord");
    InitializeObjectAttributes(&attributes,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,parameters,NULL);
    status=ZwCreateKey(Key,KEY_READ|KEY_WRITE,&attributes,0,NULL,REG_OPTION_NON_VOLATILE,NULL);
    ZwClose(parameters);
    return status;
}

void GuardRecordStopBegin(BC250_DEVICE* Device)
{
    BC250_ESCAPE_START_HEALTH health;
    HANDLE key;
    WCHAR time[32], generation[24], epoch[24];
    ULONG stage=(ULONG)GuardLastStage(), lines=0, mark=GuardLogSequence();
    NTSTATUS status, first=STATUS_SUCCESS;
    g_StopRecordPending=FALSE;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) { StartHealthFault(Device); return; }
    StartHealthStop(Device,&health);
    // A previous confirmed boot/start is not this generation's confirmation.
    // A normal confirmed stop writes nothing and preserves an older diagnostic.
    if (!health.Generation || (health.Flags&BC250_START_HEALTH_CONFIRMED)) return;
    // The start-failure path of Bc250StartDevice unwinds through this same StopDevice (pnp.c, the WddmStart and
    // inherited-timing failures and the `failed:` label). Windows stopped nothing there: our own code did, to
    // undo a start that failed, and StartRecord already names the step, the NTSTATUS and the stage of that
    // failure. A record written here would say "Windows stopped the device" of our own cleanup, and it would
    // overwrite the one case this record exists for when a retry of a stopped start then fails. Device->Started
    // is set at the last point of a start that can fail, so a stop before it is always such an unwind.
    if (!Device->Started) { GuardLog("stop record: none, this stop unwinds a start that did not reach a started device"); return; }
    if (!DriverStateReady()) return;
    status=StopRecordOpen(&key);
    if (!NT_SUCCESS(status)) {
        GuardLog("stop record: open failed 0x%08X",status);
        return;
    }
    StartRecordTime(time,RTL_NUMBER_OF(time));
    (void)RtlStringCchPrintfW(generation,RTL_NUMBER_OF(generation),L"%llu",health.Generation);
    (void)RtlStringCchPrintfW(epoch,RTL_NUMBER_OF(epoch),L"%llu",health.Epoch);
    StartRecordKeep(&first,WriteStringOrClear(key,L"Result",L"attempt"));
    StartRecordKeep(&first,WriteStringOrClear(key,L"Step",L"StopDevice"));
    StartRecordKeep(&first,WriteStringOrClear(key,L"SubStep",L"unconfirmed start"));
    StartRecordKeep(&first,WriteStringOrClear(key,L"Detail",L"Windows stopped the device; StopDevice supplies no reason code."));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"Status",(ULONG)STATUS_SUCCESS));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"Stage",StageStopEnter));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"StartStage",stage));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"HealthFlags",health.Flags));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"BootId",SharedUserData->BootId));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"Version",BC250_KMD_VERSION));
    StartRecordKeep(&first,WriteStringOrClear(key,L"Generation",generation));
    StartRecordKeep(&first,WriteStringOrClear(key,L"Epoch",epoch));
    StartRecordKeep(&first,WriteStringOrClear(key,L"TimeUtc",time));
    DeleteString(key,L"StopTimeUtc");
    status=StartRecordLog(key,mark,&lines);
    if (!NT_SUCCESS(status)) DeleteString(key,L"Log");
    StartRecordKeep(&first,status);
    StartRecordKeep(&first,WriteDwordOrClear(key,L"LogLines",lines));
    if (NT_SUCCESS(first)) first=WriteStringOrClear(key,L"Result",L"stopping");
    if (NT_SUCCESS(first)) first=ZwFlushKey(key);
    ZwClose(key);
    g_StopRecordPending=NT_SUCCESS(first);
    GuardLog("stop record: stage %lu health 0x%lX, %lu lines, write 0x%08X",stage,health.Flags,lines,first);
}

void GuardRecordStopDone(void)
{
    HANDLE key;
    WCHAR time[32];
    NTSTATUS status, first=STATUS_SUCCESS;
    if (!g_StopRecordPending) return;
    g_StopRecordPending=FALSE;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL || !DriverStateReady()) return;
    status=StopRecordOpen(&key);
    if (!NT_SUCCESS(status)) { GuardLog("stop record: finish open failed 0x%08X",status); return; }
    StartRecordTime(time,RTL_NUMBER_OF(time));
    StartRecordKeep(&first,WriteStringOrClear(key,L"Result",L"attempt"));
    StartRecordKeep(&first,WriteDwordOrClear(key,L"Stage",(ULONG)GuardLastStage()));
    StartRecordKeep(&first,WriteStringOrClear(key,L"StopTimeUtc",time));
    if (NT_SUCCESS(first)) first=WriteStringOrClear(key,L"Result",L"stopped");
    if (NT_SUCCESS(first)) first=ZwFlushKey(key);
    ZwClose(key);
    if (!NT_SUCCESS(first)) GuardLog("stop record: finish failed 0x%08X",first);
}

