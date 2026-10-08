// Display side of the M3 miniport: one source, one target, one mode (the one the firmware left), present by
// CPU copy into the firmware's framebuffer. No MMIO. Written against the documented display-only DDI.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "paging_journal.h"
#include "log_rate.h"
#include "display_timing.h"
#include "display_modes.h"

static ULONG g_Presents;

static BOOLEAN IsPostFormatSupported(D3DDDIFORMAT Format)
{
    return Format == D3DDDIFMT_X8R8G8B8 || Format == D3DDDIFMT_A8R8G8B8;
}

NTSTATUS DisplayMapFramebuffer(_Inout_ BC250_DEVICE* Device)
{
    // M3 copies 32-bit pixels and nothing else. Any other firmware format is refused, not converted.
    if (!IsPostFormatSupported(Device->Post.ColorFormat)) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    if (Device->Post.Pitch < Device->Post.Width * 4) return STATUS_GRAPHICS_INVALID_STRIDE;

    Device->FramebufferLength = (SIZE_T)Device->Post.Pitch * Device->Post.Height;
    Device->FramebufferCacheProtect = PAGE_WRITECOMBINE;
    Device->Framebuffer = MmMapIoSpaceEx(Device->Post.PhysicAddress, Device->FramebufferLength,
                                         PAGE_READWRITE | PAGE_WRITECOMBINE);
    // Record the successful choice so aliases of these physical pages agree.
    if (Device->Framebuffer == NULL) {
        Device->FramebufferCacheProtect = PAGE_NOCACHE;
        Device->Framebuffer = MmMapIoSpaceEx(Device->Post.PhysicAddress, Device->FramebufferLength,
                                             PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (Device->Framebuffer == NULL)
    {
        Device->FramebufferLength = 0;
        Device->FramebufferCacheProtect = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

// What the CPU can see in the firmware framebuffer at device start. M117 read the BAR0 mapping as zeros and
// M118 read VRAM offset 0 as zeros, both seconds after a full-table start. This sample is earlier: the mapping
// display-only actually draws through, plus the carve-out view of the same pixels once VramStart has run.
void DisplayLogFramebufferSample(_In_ BC250_DEVICE* Device)
{
    SIZE_T midOff;
    ULONG first, mid;

    if (Device->Framebuffer == NULL || Device->FramebufferLength < sizeof(ULONG) || Device->Post.Pitch < 4 ||
        Device->Post.Height == 0 || Device->Post.Width == 0)
    {
        GuardLog("fb sample: no mapping");
        return;
    }
    midOff = ((SIZE_T)Device->Post.Height / 2) * Device->Post.Pitch + ((SIZE_T)Device->Post.Width / 2) * 4;
    first = *(volatile ULONG*)Device->Framebuffer;
    mid = (midOff + sizeof(ULONG) <= Device->FramebufferLength) ?
          *(volatile ULONG*)((UCHAR*)Device->Framebuffer + midOff) : 0;
    GuardLog("fb sample mapped: phys 0x%llX first 0x%08X mid 0x%08X",
             (ULONGLONG)Device->Post.PhysicAddress.QuadPart, first, mid);

    if (Device->VramEnabled && Device->VramLength != 0)
    {
        ULONGLONG offset = 0, at;
        PHYSICAL_ADDRESS phys;
        PVOID view;
        ULONG protection;

        if (VramFramebufferOffset(Device, &offset) && offset < Device->VramLength &&
            (ULONGLONG)Device->FramebufferLength <= Device->VramLength - offset)
        {
            at = (ULONGLONG)Device->VramPhysical.QuadPart + offset;
            phys.QuadPart = (LONGLONG)at;
            protection=VramMappingProtection(Device,at,Device->FramebufferLength,PAGE_READONLY);
            view = protection ? MmMapIoSpaceEx(phys, Device->FramebufferLength,protection) : NULL;
            if (view == NULL)
                GuardLog("fb sample physical: no mapping for 0x%llX", at);
            else
            {
                first = *(volatile ULONG*)view;
                mid = (midOff + sizeof(ULONG) <= Device->FramebufferLength) ?
                      *(volatile ULONG*)((UCHAR*)view + midOff) : 0;
                MmUnmapIoSpace(view, Device->FramebufferLength);
                GuardLog("fb sample physical: 0x%llX first 0x%08X mid 0x%08X", at, first, mid);
            }
        }
    }
}

void DisplayUnmapFramebuffer(_Inout_ BC250_DEVICE* Device)
{
    // The bugcheck display writes through this pointer: take it away before the mapping goes.
    PVOID mapping = Device->Framebuffer;
    SIZE_T length = Device->FramebufferLength;

    Device->SystemDisplayReady = FALSE;
    Device->Framebuffer = NULL;
    Device->FramebufferLength = 0;
    Device->FramebufferCacheProtect = 0;
    if (mapping != NULL) MmUnmapIoSpace(mapping, length);
}

// ---- adapter ----------------------------------------------------------------------------------------------

NTSTATUS Bc250QueryAdapterInfo(_In_ const HANDLE hAdapter, _In_ const DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (QueryAdapterInfo->Type != DXGKQAITYPE_DRIVERCAPS) return STATUS_NOT_SUPPORTED;
    if (QueryAdapterInfo->OutputDataSize < RTL_SIZEOF_THROUGH_FIELD(DXGK_DRIVERCAPS, SupportSmoothRotation))
        return STATUS_BUFFER_TOO_SMALL;

    {
        DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(caps, QueryAdapterInfo->OutputDataSize);
        caps->WDDMVersion = DXGKDDI_WDDMv1_2;
        caps->HighestAcceptableAddress.QuadPart = -1;
        caps->SupportNonVGA = TRUE;
        // No hardware pointer (MaxPointerWidth/Height stay 0): dxgkrnl draws the cursor into the image it presents.
    }
    return STATUS_SUCCESS;
}

NTSTATUS Bc250SetPointerPosition(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETPOINTERPOSITION* SetPointerPosition)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(SetPointerPosition);
    return STATUS_SUCCESS;      // only "hide" can arrive while we report no pointer capability
}

NTSTATUS Bc250SetPointerShape(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETPOINTERSHAPE* SetPointerShape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(SetPointerShape);
    return STATUS_NOT_SUPPORTED;
}

// The escape runs in the caller's context. Registers and memory are for administrators.
static BOOLEAN CallerIsAdmin(void)
{
    PACCESS_TOKEN token = PsReferencePrimaryToken(PsGetCurrentProcess());
    BOOLEAN admin = SeTokenIsAdmin(token);

    PsDereferencePrimaryToken(token);
    return admin;
}

// Full WDDM owns the engines and translation tables for its entire lifetime.
// Diagnostic PLAN can also allocate/reset software state, so only known
// observational commands are admitted. The entry dispatcher still checks sizes.
static BOOLEAN WddmDiagnosticAllowed(const BC250_ESCAPE* Data, ULONG Bytes)
{
    switch (Data->Command) {
    case BC250_ESCAPE_RUN_START_HEALTH:
    case BC250_ESCAPE_OBSERVE_DCN:
    case BC250_ESCAPE_RUN_CLOCK:
    case BC250_ESCAPE_RUN_CPU:      // the operator's own surface, like RUN_CLOCK: one mailbox sequence, bounded
    case BC250_ESCAPE_RUN_DPAUDIO:  // reads, and the INDEX selects of indirect reads; the record (BC250_ESCAPE_DPAUDIO)
    case BC250_ESCAPE_GET_INFO:
    case BC250_ESCAPE_READ_REG:
    case BC250_ESCAPE_GET_MEMORY:
    case BC250_ESCAPE_VRAM_READ:
    case BC250_ESCAPE_GET_LOG:
    case BC250_ESCAPE_LOG_SUMMARY:
    case BC250_ESCAPE_GET_PAGING_JOURNAL:
    case BC250_ESCAPE_RUN_DCN:
    case BC250_ESCAPE_RUN_FBDUMP:
        return TRUE;
    case BC250_ESCAPE_RUN_GFX:
        return Bytes>=sizeof(BC250_ESCAPE_GFX) &&
            ((const BC250_ESCAPE_GFX*)Data)->Op==BC250_GFX_OP_STATE;
    case BC250_ESCAPE_RUN_IH:
        return Bytes>=sizeof(BC250_ESCAPE_IH) &&
            ((const BC250_ESCAPE_IH*)Data)->Op==BC250_IH_OP_STATE;
    default:
        return FALSE;
    }
}

// The two reads a lab sampler polls during a trial, answered with NoAdapterSynchronization alone from 0.7.184.1 on.
// What they touch, and nothing else:
// - the log ring (guard.c: g_Log under g_LogLock) or the paging journal (paging_journal.c: g_PagingJournal under
//   g_PagingJournalLock), driver-image globals set up in DriverEntry (entry.c, GuardInit and PagingJournalInit) that
//   live until the image unloads;
// - the caller's token (CallerIsAdmin), a nonpaged bounce page of our own, and the captured escape buffer;
// - for the reply's Flags, three scalars of the adapter context (FullWddm, Mmio compared with NULL, VramEnabled),
//   read and never dereferenced. RUN_START_HEALTH, RUN_CU_MODE, RUN_DPM and RUN_INTEROP already reach the same
//   context with NoAdapterSynchronization (Bc250Escape).
// No register, no device memory, no SMU message, no state a stop frees or a power transition turns off. So they need
// neither the Level Two exclusion a HardwareAccess escape buys nor Bc250Escape's power-phase check, and dxgkrnl
// takes no adapter lock for them. Up to 0.7.183.1 they came as HardwareAccess calls, and a lab profile of a Witcher 3 session
// (2026-10-01) put the game's main thread in WrResource waits readied by bc250kmd_cli, 1.2-1.3 ms per frame.
// LOG_SUMMARY is not one of them: WddmSummary walks the WDDM block WddmStop frees and DcnLogVsyncSnapshot reads OTG
// registers, so it stays a HardwareAccess (Level Two) call and LogEscape still refuses it otherwise.
// tools/win/bc250kmd_cli/test_escape_flags.py keeps this list equal to the CLI's SoftwareRead().
static BOOLEAN SoftwareReadEscape(ULONG Command)
{
    switch (Command) {
    case BC250_ESCAPE_GET_LOG:
    case BC250_ESCAPE_GET_PAGING_JOURNAL:
        return TRUE;
    default:
        return FALSE;
    }
}

// BC250_ESCAPE_GET_PAGING_JOURNAL: the paging journal (paging_journal.c), a page of records at a time, like the log:
// no gate, no register, administrators only (the records name allocation handles and GPU addresses), and a nonpaged
// bounce buffer because the copy happens under a spin lock. Reached with HardwareAccess or, from 0.7.184.1, with
// NoAdapterSynchronization alone (SoftwareReadEscape).
static NTSTATUS PagingJournalEscape(_In_ const BC250_DEVICE* device, _In_ const DXGKARG_ESCAPE* Escape)
{
    BC250_ESCAPE_PAGING_JOURNAL* journal = (BC250_ESCAPE_PAGING_JOURNAL*)Escape->pPrivateDriverData;
    BC250_PAGING_JOURNAL_RECORD* page;
    ULONGLONG next = 0, total = 0, lost = 0;
    ULONG returned;

    if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_PAGING_JOURNAL)) return STATUS_INVALID_PARAMETER;
    journal->Version = BC250_KMD_VERSION;
    if (!CallerIsAdmin()) { journal->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; return STATUS_SUCCESS; }
    page = (BC250_PAGING_JOURNAL_RECORD*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                         BC250_PAGING_JOURNAL_MAX * sizeof(BC250_PAGING_JOURNAL_RECORD),
                                                         BC250_TAG);
    if (page == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    returned = PagingJournalRead(journal->From, page, BC250_PAGING_JOURNAL_MAX, &next, &total, &lost);
    RtlCopyMemory(journal->Records, page, (SIZE_T)returned * sizeof(BC250_PAGING_JOURNAL_RECORD));
    ExFreePoolWithTag(page, BC250_TAG);
    journal->NtStatus = 0;
    journal->Flags = device->FullWddm ? BC250_ESCAPE_FLAG_FULL_WDDM : 0;
    journal->Capacity = BC250_PAGING_JOURNAL_ENTRIES;
    journal->Returned = returned;
    journal->Next = next;
    journal->Total = total;
    journal->Lost = lost;
    journal->Status = BC250_ESCAPE_STATUS_DONE;
    return STATUS_SUCCESS;
}

// BD-097: one ring line for a summary that went beside the ring, rate limited (log_rate.h). A caller that polls
// summaries would otherwise still rotate the ring, one line a poll: 720 lines an hour at the old 5 s cadence,
// against 768 rotating lines. The first lines are written in full and then one a BC250_LOG_RATE_INTERVAL_MS, so
// a reader always sees that summaries were taken and how often, without the ring paying for each one. The state
// needs no lock of its own: BC250_ESCAPE_LOG_SUMMARY is a Level Two escape, so dxgkrnl serializes two of them on
// this adapter, which is the same guarantee WddmSummary is called under.
static BC250_LOG_RATE g_SummaryNoteRate;

static void LogSummaryNote(ULONG First, ULONG Lines, ULONG Dropped, ULONG RingSeq)
{
    BC250_LOG_RATE_NOTE note;
    ULONG outcome = Bc250LogRateDecide(&g_SummaryNoteRate, GuardLogMilliseconds(), &note);

    if (outcome == BC250_LOG_RATE_LINE)
        GuardLog("log: summary of %lu lines beside the ring at seq %lu (%lu did not fit), read from %lu",
                 Lines, RingSeq, Dropped, First);
    else if (outcome == BC250_LOG_RATE_SUMMARY)
        GuardLog("log: summary of %lu lines beside the ring at seq %lu: %lu taken in %llu ms, %lu of %lu silent",
                 Lines, RingSeq, note.Pending, note.ElapsedMs, note.Skipped, note.Calls);
}

// BC250_ESCAPE_GET_LOG and BC250_ESCAPE_LOG_SUMMARY. The driver's own log, a page of lines at a time. This is how an
// experiment reads the trail on a headless machine with no kernel debugger and no DebugView, so it needs no gate: it
// touches no register, no memory of the device and nothing the caller did not already own. Administrators only all
// the same, because the lines name physical addresses. LOG_SUMMARY first writes the WDDM counter tables, so that
// one call gets both: into the ring for the evidence form, and beside the ring for the polling form (BD-097, the
// block below); with the gate closed that is a single line saying so. The caller decided
// `summary` from one read of the command: a command that changes between two looks at the buffer must not be able
// to get a summary past the check that guards it.
static NTSTATUS LogEscape(_In_ BC250_DEVICE* device, _In_ const DXGKARG_ESCAPE* Escape, BOOLEAN summary)
{
    BC250_ESCAPE_LOG* log = (BC250_ESCAPE_LOG*)Escape->pPrivateDriverData;
    BC250_LOG_LINE* page;
    ULONG from, total, lost, above, next, returned, summaryFrom = 0;

    if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_LOG)) return STATUS_INVALID_PARAMETER;
    log->Version = BC250_KMD_VERSION;   // before any refusal, so that a refused caller still learns the build
    if (!CallerIsAdmin()) { log->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; return STATUS_SUCCESS; }

    // The summary walks the WDDM state that WddmStop frees, and what keeps the two apart is dxgkrnl's
    // synchronization (see WddmSummary). With HardwareAccess, and without NoAdapterSynchronization, the escape
    // is a Level Two call, which is a guarantee about this call and not only about StopDevice; bc250kmd_cli
    // asks for exactly that, and a caller that does not is refused rather than trusted.
    if (summary && (!Escape->Flags.HardwareAccess || Escape->Flags.NoAdapterSynchronization))
    {
        log->Status = BC250_ESCAPE_STATUS_REFUSED;
        log->NtStatus = (ULONG)STATUS_INVALID_DEVICE_REQUEST;
        return STATUS_SUCCESS;
    }

    // The lines are read into pool of our own and copied out afterwards, not read straight into the caller's
    // buffer. GuardLogRead fills its output under a spin lock, i.e. at DISPATCH_LEVEL, and nothing documents
    // pPrivateDriverData as nonpaged - dxgkrnl captures it, but at what pool type is not stated anywhere. A
    // page fault there would be a bugcheck on the first `bc250kmd_cli log`, so this does not rely on it. It
    // also keeps the lock hold to one page of copying instead of one page plus a user buffer, and every
    // GuardLog on every other processor waits behind that lock.
    page = (BC250_LOG_LINE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, BC250_LOG_MAX_LINES * sizeof(BC250_LOG_LINE),
                                            BC250_TAG);
    if (page == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    from = log->From;
    if (summary)
    {
        // BD-097: the polling form writes the block beside the ring. One summary is about 320 lines, 42 % of
        // the ring's rotating tail, and the b23 lab read (2026-10-08) measured what a 5 s poll of it does: the
        // whole ring rotated in about 12 s and the mode sets an hour of evidence was about were gone. The block
        // goes to its own storage, addressed from BC250_LOG_SUMMARY_SEQ, and the ring gets one rate-limited
        // line that says where it was taken. The CLI needs nothing new for it: the sequence it asks for next is
        // the one answered here, and the summary space pages like the ring.
        //
        // `log summary` with a position of its own (the evidence form, which prints the whole ring) still
        // writes into the ring, where the block belongs between the lines it is read with.
        BOOLEAN beside = (from == BC250_LOG_FROM_SUMMARY) && GuardLogSummaryBegin();

        if (beside)
        {
            ULONG first = BC250_LOG_SUMMARY_SEQ, lines = 0, dropped = 0, ringSeq = 0;

            WddmSummary(device);
            // The block's own first sequence, not the constant: each summary answers from the next block of the
            // space, so that a reader of the block before it is refused instead of given a page of each.
            GuardLogSummaryEnd(&first, &lines, &dropped, &ringSeq);
            summaryFrom = first;
            from = summaryFrom;
            LogSummaryNote(first, lines, dropped, ringSeq);
        }
        else
        {
            // Where the summary starts, taken before it is written, so that `log summary` can print only the
            // lines it caused instead of the whole ring again. Another processor logging in between lands in the
            // same window; that is one or two extra lines, not a wrong answer.
            summaryFrom = GuardLogSequence();
            WddmSummary(device);
            if (from == BC250_LOG_FROM_SUMMARY) from = summaryFrom;
        }
    }
    else if (from == BC250_LOG_FROM_SUMMARY)
    {
        from = 0;                       // the sentinel means nothing without a summary; do not read past the end
    }
    else if (from >= BC250_LOG_SUMMARY_SEQ)
    {
        // A page read inside the summary space. It answers the block the driver holds now, so that a caller
        // whose own block was replaced by a newer summary reads that from the same escape instead of seeing an
        // empty page it cannot tell from the end of its block (BD-097).
        summaryFrom = GuardLogSummaryBase();
    }
    log->SummaryFrom = summaryFrom;
    log->NtStatus = 0;
    log->Flags = (device->FullWddm ? BC250_ESCAPE_FLAG_FULL_WDDM : 0) |
                 (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) |
                 (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0);
    log->HeadLines = BC250_LOG_HEAD_LINES;
    log->RingLines = BC250_LOG_RING_LINES;
    log->From = from;                   // what was actually read, so the sentinel is visible as a number
    // Both calls write their outputs under the log's spin lock, so they get locals, for the reason the lines
    // get the bounce buffer. The copy length is the local as well: a count read back from the caller's buffer
    // is a count the caller can change between the two lines.
    GuardLogStats(&total, &lost, &above);
    returned = GuardLogRead(from, page, BC250_LOG_MAX_LINES, &next);
    RtlCopyMemory(log->Lines, page, (SIZE_T)returned * sizeof(BC250_LOG_LINE));
    ExFreePoolWithTag(page, BC250_TAG);
    log->Total = total;
    log->Lost = lost;
    log->Above = above;
    log->Next = next;
    log->Returned = returned;
    log->Status = BC250_ESCAPE_STATUS_DONE;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250Escape(_In_ const HANDLE hAdapter, _In_ const DXGKARG_ESCAPE* Escape)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_ESCAPE* data = (BC250_ESCAPE*)Escape->pPrivateDriverData;
    D3DDDI_ESCAPEFLAGS softwareRead = {0};
    ULONG command;

    if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE) || data == NULL || data->Magic != BC250_ESCAPE_MAGIC)
        return STATUS_INVALID_PARAMETER;
    // GET_LOG and GET_PAGING_JOURNAL with NoAdapterSynchronization alone (0.7.184.1): driver-image state only, so
    // they are answered ahead of the power-phase check (SoftwareReadEscape). The command is read once, so the read
    // that runs is the read that was admitted; every other flag combination takes the path below, as before.
    command = data->Command;
    softwareRead.NoAdapterSynchronization = 1;
    if (SoftwareReadEscape(command) && Escape->Flags.Value == softwareRead.Value)
        return command == BC250_ESCAPE_GET_LOG ? LogEscape(device, Escape, FALSE) : PagingJournalEscape(device, Escape);
    // Dispatch the adapter-owned snapshot before touching subsystem/lifecycle fields.
    if (data->Command == BC250_ESCAPE_RUN_START_HEALTH) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_START_HEALTH)) return STATUS_INVALID_PARAMETER;
        StartHealthRequest(device,(BC250_ESCAPE_START_HEALTH*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // CU mode: adapter-owned software snapshot as well (cumode.c), no BAR access.
    if (data->Command == BC250_ESCAPE_RUN_CU_MODE) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_CU_MODE)) return STATUS_INVALID_PARAMETER;
        CuModeRequest(device,(BC250_ESCAPE_CU_MODE*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // DPM: the governor's published snapshot, software state as well (dpm.c); ABI 3 adds the SMU metrics copy.
    if (data->Command == BC250_ESCAPE_RUN_DPM) {
        if (Escape->PrivateDriverDataSize != BC250_DPM_ABI3_SIZE &&
            Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DPM) &&
            Escape->PrivateDriverDataSize != BC250_DPM_ABI1_SIZE) return STATUS_INVALID_PARAMETER;
        DpmRequest(device,(BC250_ESCAPE_DPM*)data,Escape->PrivateDriverDataSize,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // DPM runtime tuning (0.7.185): thresholds and floor the governor thread takes at its next tick; software state,
    // locking and lifetime argued at DpmTuneRequest (dpm.c). Two exact sizes: ABI 2 (0.7.197) and its ABI 1 prefix;
    // DpmTuneRequest matches the size against AbiVersion and touches nothing past the size it was given.
    if (command == BC250_ESCAPE_RUN_DPM_TUNE) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DPM_TUNE) &&
            Escape->PrivateDriverDataSize != BC250_DPM_TUNE_ABI2_SIZE &&
            Escape->PrivateDriverDataSize != BC250_DPM_TUNE_ABI1_SIZE) return STATUS_INVALID_PARAMETER;
        DpmTuneRequest(device,(BC250_ESCAPE_DPM_TUNE*)data,Escape->PrivateDriverDataSize,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // The operator's V/F curve: software state like the tuning above, so the same flags and the same
    // argument (DpmCurveRequest, dpm.c). One exact size, because the structure has one ABI.
    if (command == BC250_ESCAPE_RUN_DPM_CURVE) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DPM_CURVE)) return STATUS_INVALID_PARAMETER;
        DpmCurveRequest(device,(BC250_ESCAPE_DPM_CURVE*)data,Escape->PrivateDriverDataSize,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // The board's hardware monitor: the sampler's published snapshot, software state as well (hwmon.c). No port
    // is read here, so it belongs with the other software snapshots, ahead of the power-phase check below.
    if (command == BC250_ESCAPE_RUN_HWMON) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_HWMON)) return STATUS_INVALID_PARAMETER;
        HwmonRequest(device,(BC250_ESCAPE_HWMON*)data,Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // The case fan control: its published snapshot, and requests the governor thread applies at its next step. No
    // port is touched by the escape itself (fan.c), so it sits with the software snapshots as well.
    if (command == BC250_ESCAPE_RUN_FAN) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_FAN)) return STATUS_INVALID_PARAMETER;
        FanRequest(device,(BC250_ESCAPE_FAN*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // Interop switches: the start's decision and the session marker, software state as well (interop.c).
    if (data->Command == BC250_ESCAPE_RUN_INTEROP) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_INTEROP)) return STATUS_INVALID_PARAMETER;
        InteropRequest(device,(BC250_ESCAPE_INTEROP*)data,Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    // Only adapter-owned health, the separately joined SMU owner and the two
    // software reads above support NoAdapterSynchronization. Other diagnostics
    // rely on OS Level Two/Three exclusion and must not enter a
    // powered-down/partially restored subsystem.
    // RUN_CPU (0.7.210) is admitted with NoAdapterSynchronization as well, because its READ operation, and its KEEP
    // from 0.7.213, are adapter-owned software state and CpuRequest refuses that flag for every operation that
    // touches the mailbox. The power-phase half below still applies to it: a CPU message during a retained power
    // transition is exactly what must not run.
    if ((data->Command!=BC250_ESCAPE_RUN_CLOCK && data->Command!=BC250_ESCAPE_RUN_CPU &&
         Escape->Flags.NoAdapterSynchronization) ||
        (data->Command!=BC250_ESCAPE_RUN_CLOCK &&
         InterlockedCompareExchange(&device->RetainedPowerPhase,0,0)!=0)) {
        data->Status=BC250_ESCAPE_STATUS_REFUSED;
        return STATUS_DEVICE_NOT_READY;
    }
    if (device->FullWddm && !WddmDiagnosticAllowed(data,Escape->PrivateDriverDataSize)) {
        // Only Status is at a common offset in every escape structure.
        data->Status=BC250_ESCAPE_STATUS_REFUSED;
        return STATUS_DEVICE_BUSY;
    }
    if (data->Command == BC250_ESCAPE_OBSERVE_DCN) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DCN_OBSERVE)) return STATUS_INVALID_PARAMETER;
        DcnObserve(device,(BC250_ESCAPE_DCN_OBSERVE*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS; // typed operation status is in the reply
    }
    if (data->Command == BC250_ESCAPE_RUN_DPAUDIO) {
        // Two exact sizes (0.7.216): ABI 2, the whole structure, and its ABI 1 prefix. DpAudioRequest matches the
        // size against AbiVersion and touches nothing past the size it was given.
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DPAUDIO) &&
            Escape->PrivateDriverDataSize != BC250_DPAUDIO_ABI1_SIZE) return STATUS_INVALID_PARAMETER;
        DpAudioRequest(device,(BC250_ESCAPE_DPAUDIO*)data,Escape->PrivateDriverDataSize,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS; // typed operation status is in the reply
    }
    // The CPU surface (0.7.210): its write operations send mailbox messages on the firmware's queue 3, so it sits
    // here, past the Level Two gate, and not with the software snapshots above. CpuRequest itself admits READ with
    // NoAdapterSynchronization only, KEEP with either flag word, and every other operation with HardwareAccess
    // only (cpu.c).
    if (data->Command == BC250_ESCAPE_RUN_CPU) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_CPU)) return STATUS_INVALID_PARAMETER;
        CpuRequest(device,(BC250_ESCAPE_CPU*)data,Escape->PrivateDriverDataSize,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS; // typed operation status is in the reply
    }
    if (data->Command == BC250_ESCAPE_RUN_CLOCK) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_CLOCK)) return STATUS_INVALID_PARAMETER;
        SmuClockRequest(&device->Smu,(BC250_ESCAPE_CLOCK*)data,CallerIsAdmin(),
            (BOOLEAN)Escape->Flags.HardwareAccess,(BOOLEAN)Escape->Flags.NoAdapterSynchronization);
        return STATUS_SUCCESS; // typed operation status is in the reply
    }
    if (data->Command >= BC250_ESCAPE_GET_MEMORY && data->Command <= BC250_ESCAPE_VRAM_WRITE)
    {
        BC250_ESCAPE_MEMORY* memory = (BC250_ESCAPE_MEMORY*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_MEMORY)) return STATUS_INVALID_PARAMETER;
        if (!CallerIsAdmin()) memory->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else VramEscape(device, memory);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_GART)
    {
        BC250_ESCAPE_GART* gart = (BC250_ESCAPE_GART*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_GART)) return STATUS_INVALID_PARAMETER;
        gart->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) |
                      (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0);
        if (!CallerIsAdmin()) gart->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else GartEscape(device, gart);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_PSP)
    {
        BC250_ESCAPE_PSP* psp = (BC250_ESCAPE_PSP*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_PSP)) return STATUS_INVALID_PARAMETER;
        psp->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) |
                     (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) | (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0);
        if (!CallerIsAdmin()) psp->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else PspEscape(device, psp);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_GFX)
    {
        BC250_ESCAPE_GFX* gfx = (BC250_ESCAPE_GFX*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_GFX)) return STATUS_INVALID_PARAMETER;
        gfx->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) |
                     (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) | (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0) |
                     (device->MmioGfxEnabled ? BC250_ESCAPE_FLAG_GFX : 0);
        if (!CallerIsAdmin()) gfx->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else GfxEscape(device, gfx);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_FENCE)
    {
        BC250_ESCAPE_FENCE* fence = (BC250_ESCAPE_FENCE*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_FENCE)) return STATUS_INVALID_PARAMETER;
        fence->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) |
                       (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0) | (device->MmioGfxEnabled ? BC250_ESCAPE_FLAG_GFX : 0) |
                       (device->MmioIhEnabled ? BC250_ESCAPE_FLAG_IH : 0);
        if (!CallerIsAdmin()) fence->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else GfxFenceEscape(device, fence);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_IH)
    {
        BC250_ESCAPE_IH* ih = (BC250_ESCAPE_IH*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_IH)) return STATUS_INVALID_PARAMETER;
        ih->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) |
                    (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0) | (device->MmioGfxEnabled ? BC250_ESCAPE_FLAG_GFX : 0) |
                    (device->MmioIhEnabled ? BC250_ESCAPE_FLAG_IH : 0);
        if (!CallerIsAdmin()) ih->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else IhEscape(device, ih);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_DCN)
    {
        BC250_ESCAPE_DCN* dcn = (BC250_ESCAPE_DCN*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_DCN)) return STATUS_INVALID_PARAMETER;
        dcn->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0);
        if (!CallerIsAdmin()) dcn->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else DcnEscape(device, dcn);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_DCNFLIP)
    {
        BC250_ESCAPE_DCNFLIP* flip = (BC250_ESCAPE_DCNFLIP*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_DCNFLIP)) return STATUS_INVALID_PARAMETER;
        flip->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->DcnWriteEnabled ? BC250_ESCAPE_FLAG_DCN_WRITE : 0) |
                      (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) | (device->VramWriteEnabled ? BC250_ESCAPE_FLAG_VRAM_WRITE : 0);
        if (!CallerIsAdmin()) flip->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else DcnFlipEscape(device, flip);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_FBDUMP)
    {
        BC250_ESCAPE_FBDUMP* fbdump = (BC250_ESCAPE_FBDUMP*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_FBDUMP)) return STATUS_INVALID_PARAMETER;
        fbdump->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0);
        if (!CallerIsAdmin()) fbdump->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else FbdumpEscape(device, fbdump);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_SDMACOPY || data->Command == BC250_ESCAPE_RUN_SDMAIB)
    {
        BC250_ESCAPE_SDMACOPY* sdmacopy = (BC250_ESCAPE_SDMACOPY*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_SDMACOPY)) return STATUS_INVALID_PARAMETER;
        sdmacopy->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) |
                          (device->VramWriteEnabled ? BC250_ESCAPE_FLAG_VRAM_WRITE : 0) | (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) |
                          (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0) | (device->MmioGfxEnabled ? BC250_ESCAPE_FLAG_GFX : 0);
        if (!CallerIsAdmin()) sdmacopy->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else SdmaCopyEscape(device, sdmacopy);
        return STATUS_SUCCESS;
    }
    if (command == BC250_ESCAPE_GET_PAGING_JOURNAL) return PagingJournalEscape(device, Escape);
    if (command == BC250_ESCAPE_GET_LOG || command == BC250_ESCAPE_LOG_SUMMARY)
        return LogEscape(device, Escape, command == BC250_ESCAPE_LOG_SUMMARY);
    // 2026-09-22: every gate this driver has, not only the four READ_REG/WRITE_REG cared about before - the
    // overlay's live panel reads GET_INFO for exactly this, and a caller that only ever checked the first four
    // bits sees the same answer it always did (they are unchanged).
    data->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) |
                  (device->MmioWriteEnabled ? BC250_ESCAPE_FLAG_MMIO_WRITE : 0) |
                  (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) | (device->VramWriteEnabled ? BC250_ESCAPE_FLAG_VRAM_WRITE : 0) |
                  (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) | (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0) |
                  (device->MmioGfxEnabled ? BC250_ESCAPE_FLAG_GFX : 0) | (device->MmioIhEnabled ? BC250_ESCAPE_FLAG_IH : 0) |
                  (device->FullWddm ? BC250_ESCAPE_FLAG_FULL_WDDM : 0);
    data->NtStatus = 0;

    if (data->Command == BC250_ESCAPE_READ_REG || data->Command == BC250_ESCAPE_WRITE_REG)
    {
        ULONG value = 0;
        NTSTATUS status;

        if (!CallerIsAdmin())
        {
            data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
            return STATUS_SUCCESS;
        }
        if (data->Command == BC250_ESCAPE_WRITE_REG)
        {
            status = MmioWrite(device, data->RegOffset, data->RegValue);
            GuardLog("escape: write 0x%05X = 0x%08X -> 0x%08X", data->RegOffset, data->RegValue, status);
            if (NT_SUCCESS(status)) status = MmioRead(device, data->RegOffset, &value);
        }
        else
        {
            status = MmioRead(device, data->RegOffset, &value);
        }
        data->RegValue = value;
        data->NtStatus = (unsigned long)status;
        data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
        return STATUS_SUCCESS;
    }
    if (data->Command != BC250_ESCAPE_GET_INFO)
    {
        data->Status = BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;
        return STATUS_SUCCESS;
    }
    data->Status = BC250_ESCAPE_STATUS_DONE;
    data->Version = BC250_KMD_VERSION;
    data->LastStage = (unsigned long)GuardLastStage();
    data->Width = device->Post.Width;
    data->Height = device->Post.Height;
    data->Pitch = device->Post.Pitch;
    data->ColorFormat = (unsigned long)device->Post.ColorFormat;
    data->Presents = g_Presents;            // display-only present count (Bc250PresentDisplayOnly); 0 under FullWddm
    {
        // Reserved[0]/[1] (2026-09-22): blits and flips, the full table's own present counters - both 0 outside
        // FullWddm, same as g_Presents above is 0 inside it. See bc250kmd_escape.h's comment on BC250_ESCAPE.
        LONG blits = 0, flips = 0;
        WddmCounters(device, &blits, &flips);
        data->Reserved[0] = (unsigned long)blits;
        data->Reserved[1] = (unsigned long)flips;
    }
    return STATUS_SUCCESS;
}

// ---- VidPN ------------------------------------------------------------------------------------------------

NTSTATUS DisplayPrepareInheritedTiming(_Inout_ BC250_DEVICE* Device)
{
    D3DKMDT_VIDEO_SIGNAL_INFO signal;
    D3DKMDT_VIDEO_SIGNAL_INFO* Signal=&signal;
    NTSTATUS status=STATUS_SUCCESS;
    Device->InheritedSignalValid=FALSE;
    RtlZeroMemory(Signal, sizeof(*Signal));
    Signal->VideoStandard = D3DKMDT_VSS_OTHER;
    Signal->TotalSize.cx = Device->Post.Width;
    Signal->TotalSize.cy = Device->Post.Height;
    Signal->ActiveSize = Signal->TotalSize;
    Signal->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
    // Display-only retains its documented unspecified-timing path without MMIO.
    // Full WDDM must provide the actual inherited timing or refuse the mode;
    // nominal60Hz and zero blanking are not hardware measurements.
    if (Device->FullWddm) status=DisplayReadInheritedTiming(Device,Signal);
    if (!NT_SUCCESS(status)) return status;
    Device->InheritedSignal=*Signal;
    Device->InheritedSignalValid=TRUE;
    return STATUS_SUCCESS;
}

// This driver preserves one inherited mode for the device start. Both mode
// enumeration and allocation descriptions consume exactly this cached tuple.
static NTSTATUS FillSignalInfo(_In_ const BC250_DEVICE* Device, _Out_ D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    if (!Device->InheritedSignalValid) return STATUS_DEVICE_NOT_READY;
    *Signal=Device->InheritedSignal;
    return STATUS_SUCCESS;
}

static NTSTATUS OfferSourceMode(_In_ const BC250_DEVICE* Device, _In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE* mode = NULL;
    NTSTATUS status;
    unsigned long formats[BC250_SOURCE_MODE_MAX], count, i;

    // One mode per pixel format of display_modes.h, all of the inherited geometry: the scan-out format first,
    // then (full table only) the formats the UMDs compose, so that DXGI lists modes for them too.
    count = Bc250SourceModeFormats(Device->ComposedSourceModes, formats, BC250_SOURCE_MODE_MAX);
    if (count == 0 || count > BC250_SOURCE_MODE_MAX) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    status = VidPn->pfnCreateNewSourceModeSet(hVidPn, SourceId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;

    for (i = 0; i < count && NT_SUCCESS(status); ++i)
    {
        const ULONG stride = Bc250SourceModeStride(formats[i], Device->Post.Pitch);
        if (stride == 0) { status = STATUS_GRAPHICS_INVALID_STRIDE; break; }
        status = set->pfnCreateNewModeInfo(hSet, &mode);
        if (!NT_SUCCESS(status)) break;
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics.PrimSurfSize.cx = Device->Post.Width;
        mode->Format.Graphics.PrimSurfSize.cy = Device->Post.Height;
        mode->Format.Graphics.VisibleRegionSize = mode->Format.Graphics.PrimSurfSize;
        mode->Format.Graphics.Stride = stride;
        mode->Format.Graphics.PixelFormat = (D3DDDIFORMAT)formats[i];
        mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        status = set->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignSourceModeSet(hVidPn, SourceId, hSet);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseSourceModeSet(hVidPn, hSet);
    return status;
}

static NTSTATUS OfferTargetMode(_In_ const BC250_DEVICE* Device, _In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE* set = NULL;
    D3DKMDT_VIDPN_TARGET_MODE* mode = NULL;
    NTSTATUS status;

    status = VidPn->pfnCreateNewTargetModeSet(hVidPn, TargetId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;

    status = set->pfnCreateNewModeInfo(hSet, &mode);
    if (NT_SUCCESS(status))
    {
        status=FillSignalInfo(Device, &mode->VideoSignalInfo);
        // From interface 2.2 the preference shares one word with the wire formats the target accepts, and a
        // target mode must name at least one (186 named none: seven commits on the lab, all blank, no mode set).
        // The inherited GOP signal is 8-bit RGB. MinimumVSyncFreq (2.9) is not part of a V1 mode info: dxgkrnl
        // allocates the V1 size, and 188's write past it ended in bugcheck 0x113 (7, ..., C0000005) in AddMode.
        mode->WireFormatAndPreference.Value = 0;
        mode->WireFormatAndPreference.Preference = D3DKMDT_MP_PREFERRED;
        mode->WireFormatAndPreference.Rgb = D3DKMDT_BITS_PER_COMPONENT_08;
        if (NT_SUCCESS(status)) status = set->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignTargetModeSet(hVidPn, TargetId, hSet);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return status;
}

static NTSTATUS SourceModeIsPinned(_In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                   D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId, _Out_ BOOLEAN* Pinned,
                                   _Out_opt_ D3DKMDT_VIDPN_SOURCE_MODE* Copy)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE* pinned = NULL;
    NTSTATUS status;

    *Pinned = FALSE;
    status = VidPn->pfnAcquireSourceModeSet(hVidPn, SourceId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;
    status = set->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(status) && pinned != NULL)
    {
        *Pinned = TRUE;
        if (Copy != NULL) *Copy = *pinned;
        set->pfnReleaseModeInfo(hSet, pinned);
    }
    VidPn->pfnReleaseSourceModeSet(hVidPn, hSet);
    return (status == STATUS_GRAPHICS_MODE_NOT_PINNED) ? STATUS_SUCCESS : status;
}

static NTSTATUS TargetModeIsPinned(_In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                   D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, _Out_ BOOLEAN* Pinned)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE* set = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE* pinned = NULL;
    NTSTATUS status;

    *Pinned = FALSE;
    status = VidPn->pfnAcquireTargetModeSet(hVidPn, TargetId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;
    status = set->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(status) && pinned != NULL)
    {
        *Pinned = TRUE;
        set->pfnReleaseModeInfo(hSet, pinned);
    }
    VidPn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return (status == STATUS_GRAPHICS_MODE_NOT_PINNED) ? STATUS_SUCCESS : status;
}

NTSTATUS Bc250IsSupportedVidPn(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_ISSUPPORTEDVIDPN* IsSupportedVidPn)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    SIZE_T paths = 0;
    NTSTATUS status;

    IsSupportedVidPn->IsVidPnSupported = FALSE;
    if (IsSupportedVidPn->hDesiredVidPn == NULL)        // the empty VidPN is always supported
    {
        IsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    status = device->Dxgk.DxgkCbQueryVidPnInterface(IsSupportedVidPn->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(IsSupportedVidPn->hDesiredVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;
    status = topology->pfnGetNumPaths(hTopology, &paths);
    if (!NT_SUCCESS(status)) return status;

    // One source, one target: zero or one path. Modes are constrained in EnumVidPnCofuncModality; a pinned
    // source mode must also have a format of display_modes.h, the set CommitVidPn accepts.
    IsSupportedVidPn->IsVidPnSupported = (paths <= 1);
    if (paths == 1)
    {
        D3DKMDT_VIDPN_SOURCE_MODE mode;
        BOOLEAN pinned;
        // A VidPN whose source mode set cannot be read is judged by its topology alone, as before 0.7.201.
        if (NT_SUCCESS(SourceModeIsPinned(vidpn, IsSupportedVidPn->hDesiredVidPn, 0, &pinned, &mode)) &&
            pinned && mode.Type == D3DKMDT_RMT_GRAPHICS &&
            !Bc250SourceModeAdmitted(device->ComposedSourceModes, (unsigned long)mode.Format.Graphics.PixelFormat))
            IsSupportedVidPn->IsVidPnSupported = FALSE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Bc250RecommendFunctionalVidPn(_In_ const HANDLE hAdapter, _In_ const DXGKARG_RECOMMENDFUNCTIONALVIDPN* const Recommend)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Recommend);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS Bc250EnumVidPnCofuncModality(_In_ const HANDLE hAdapter, _In_ const DXGKARG_ENUMVIDPNCOFUNCMODALITY* const Enum)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
    NTSTATUS status;

    status = device->Dxgk.DxgkCbQueryVidPnInterface(Enum->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(Enum->hConstrainingVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;

    status = topology->pfnAcquireFirstPathInfo(hTopology, &path);
    while (NT_SUCCESS(status) && status != STATUS_GRAPHICS_DATASET_IS_EMPTY && path != NULL)
    {
        const D3DKMDT_VIDPN_PRESENT_PATH* next = NULL;
        D3DKMDT_VIDPN_PRESENT_PATH update = *path;
        BOOLEAN pinned;
        BOOLEAN changed = FALSE;
        NTSTATUS step;

        // Source mode set: offer our one mode unless a mode is pinned or this set is the pivot.
        step = SourceModeIsPinned(vidpn, Enum->hConstrainingVidPn, path->VidPnSourceId, &pinned, NULL);
        if (NT_SUCCESS(step) && !pinned &&
            !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && Enum->EnumPivot.VidPnSourceId == path->VidPnSourceId))
            step = OfferSourceMode(device, vidpn, Enum->hConstrainingVidPn, path->VidPnSourceId);

        // Target mode set: the same.
        if (NT_SUCCESS(step)) step = TargetModeIsPinned(vidpn, Enum->hConstrainingVidPn, path->VidPnTargetId, &pinned);
        if (NT_SUCCESS(step) && !pinned &&
            !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && Enum->EnumPivot.VidPnTargetId == path->VidPnTargetId))
            step = OfferTargetMode(device, vidpn, Enum->hConstrainingVidPn, path->VidPnTargetId);

        // Path transformations: identity only, reported where nothing is pinned yet.
        if (NT_SUCCESS(step))
        {
            if (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_SCALING)
            {
                RtlZeroMemory(&update.ContentTransformation.ScalingSupport, sizeof(update.ContentTransformation.ScalingSupport));
                update.ContentTransformation.ScalingSupport.Identity = 1;
                changed = TRUE;
            }
            if (path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_ROTATION)
            {
                RtlZeroMemory(&update.ContentTransformation.RotationSupport, sizeof(update.ContentTransformation.RotationSupport));
                update.ContentTransformation.RotationSupport.Identity = 1;
                // A WDDM 1.3+ driver has "path independent rotation" and must say which offsets it supports: on a
                // test-signed system dxgkrnl bugchecks (0x113, subtype 0x1C) when the primary path's support lacks
                // Offset0 or carries any other offset (E16 run 007, read in DMMVIDPNPRESENTPATH::SetRotationSupport).
                // For a display-only driver dxgkrnl sets the bit by itself, so that path stays as it was.
                if (device->FullWddm) update.ContentTransformation.RotationSupport.Offset0 = 1;
                changed = TRUE;
            }
            // dxgkrnl judges RotationSupport on EVERY pfnUpdatePathSupportInfo call, also when only the scaling
            // branch above asked for it, and the copy taken from the path may still be all zero there.
            if (changed && device->FullWddm)
            {
                update.ContentTransformation.RotationSupport.Identity = 1;
                update.ContentTransformation.RotationSupport.Offset0 = 1;
            }
            if (changed) step = topology->pfnUpdatePathSupportInfo(hTopology, &update);
        }

        if (!NT_SUCCESS(step))
        {
            topology->pfnReleasePathInfo(hTopology, path);
            return step;
        }
        status = topology->pfnAcquireNextPathInfo(hTopology, path, &next);
        topology->pfnReleasePathInfo(hTopology, path);
        path = next;
        if (status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) { status = STATUS_SUCCESS; break; }
    }
    return (status == STATUS_GRAPHICS_DATASET_IS_EMPTY) ? STATUS_SUCCESS : status;
}

static void DisplayRetainVisibility(_Inout_ BC250_DEVICE* Device,
    _In_ const DXGKARG_SETVIDPNSOURCEVISIBILITY* Visibility, NTSTATUS Status, LONGLONG BeginQpc)
{
    BC250_VISIBILITY_EVENT* event=&Device->VisibilityHistory[
        ((ULONG)Device->VisibilityCalls-1u)%BC250_VISIBILITY_HISTORY_COUNT];
    if (Visibility->Visible)
    {
        if (Device->VisibilityTrueCalls==0) Device->VisibilityFirstTrueStatus=Status;
        Device->VisibilityTrueCalls++;
        Device->VisibilityLastTrueStatus=Status;
    }
    else Device->VisibilityFalseCalls++;
    if (!NT_SUCCESS(Status)) Device->VisibilityFailures++;
    Device->VisibilityLastSource=Visibility->VidPnSourceId;
    Device->VisibilityLastRequested=Visibility->Visible;
    Device->VisibilityLastStatus=Status;
    event->Call=(ULONG)Device->VisibilityCalls;
    event->Source=Visibility->VidPnSourceId;
    event->Requested=Visibility->Visible;
    event->Status=Status;
    event->SourceVisible=Device->SourceVisible;
    event->Blanked=Device->DcnBlanked;
    event->BeginQpc=BeginQpc;
    event->EndQpc=KeQueryPerformanceCounter(NULL).QuadPart;
}

static NTSTATUS SetVisibilityCore(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETVIDPNSOURCEVISIBILITY* Visibility)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    LONG call=InterlockedIncrement(&device->VisibilityCalls);
    LONGLONG begin=KeQueryPerformanceCounter(NULL).QuadPart;
    NTSTATUS status=STATUS_SUCCESS;

    // Normal PASSIVE_LEVEL visibility DDI only. The shared quiet restore helper
    // must remain usable at bugcheck without a logger, allocation or lock.
    if (call<=32) GuardLog("display visibility: call %ld source 0x%08X visible %u previous %u hardware %u blanked %u",
        call,Visibility->VidPnSourceId,Visibility->Visible,device->SourceVisible,device->VidPnFlipEnabled,device->DcnBlanked);
    if (Visibility->VidPnSourceId != 0) status=STATUS_INVALID_PARAMETER;
    else if (device->VidPnFlipEnabled) status=DcnSetVisibility(device,Visibility->Visible);
    else if (!Visibility->Visible && device->SourceVisible && device->Framebuffer != NULL)
        RtlZeroMemory(device->Framebuffer, device->FramebufferLength); // legacy no-MMIO display-only fallback
    if (NT_SUCCESS(status))
    {
        device->SourceVisible = Visibility->Visible;
        // Visibility does not suppress requested VSync signals. A visible source
        // can queue a flip immediately, so ensure its report source is armed.
        WddmSourceVisibility(device, Visibility->Visible);
    }
    DisplayRetainVisibility(device,Visibility,status,begin);
    if (call<=32) GuardLog("display visibility: call %ld status 0x%08X current %u blanked %u",
        call,status,device->SourceVisible,device->DcnBlanked);
    return status;
}

NTSTATUS Bc250SetVidPnSourceVisibility(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETVIDPNSOURCEVISIBILITY* Visibility)
{
    BC250_DEVICE* device=(BC250_DEVICE*)hAdapter;
    NTSTATUS status;
    StartHealthEnter(device);
    status=SetVisibilityCore(hAdapter,Visibility);
    if (NT_SUCCESS(status)) StartHealthVisibilityLocked(device,device->SourceVisible);
    StartHealthLeave(device);
    return status;
}

static void DisplayRetainCommitPower(_Inout_ BC250_DEVICE* Device,
    _In_ const DXGKARG_COMMITVIDPN* Commit)
{
    InterlockedIncrement(&Device->CommitPowerCalls);
    Device->CommitLastPowerTransition=(BOOLEAN)Commit->Flags.PathPowerTransition;
    Device->CommitLastPoweredOff=(BOOLEAN)Commit->Flags.PathPoweredOff;
}

static NTSTATUS CommitVidPnCore(_In_ const HANDLE hAdapter, _In_ const DXGKARG_COMMITVIDPN* const Commit)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE mode;
    SIZE_T paths = 0;
    BOOLEAN pinned;
    NTSTATUS status;

    DisplayRetainCommitPower(device,Commit);
    if (!device->CommitSeen) { device->CommitSeen = TRUE; GuardStage(StageFirstCommitVidPn); }
    if (Commit->Flags.PathPowerTransition) return STATUS_SUCCESS;       // no mode change, power only

    status = device->Dxgk.DxgkCbQueryVidPnInterface(Commit->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(Commit->hFunctionalVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;
    status = topology->pfnGetNumPaths(hTopology, &paths);
    if (!NT_SUCCESS(status)) return status;
    // From here on the source counts as unusable until this commit has validated its mode: presents are
    // dropped while ModeActive is clear, so a surface of another size can never be copied from.
    device->ModeActive = FALSE;
    if (paths == 0) return STATUS_SUCCESS;                               // source detached: nothing to program

    status = SourceModeIsPinned(vidpn, Commit->hFunctionalVidPn, 0, &pinned, &mode);
    if (!NT_SUCCESS(status)) return status;
    if (!pinned) return STATUS_SUCCESS;     // a path without a pinned source mode: nothing to show, not an error
                                            // (STATUS_GRAPHICS_MODE_NOT_PINNED has success severity anyway)

    // The only geometry we can show is the one already on the wire, in a format that display_modes.h offers.
    // A composed format leaves the plane as it is (8-bit, the firmware's): the UMDs compose such buffers, and
    // SetVidPnSourceAddress refuses them. Each change of the committed format is logged once.
    if (mode.Type != D3DKMDT_RMT_GRAPHICS ||
        mode.Format.Graphics.PrimSurfSize.cx != device->Post.Width ||
        mode.Format.Graphics.PrimSurfSize.cy != device->Post.Height ||
        !Bc250SourceModeAdmitted(device->ComposedSourceModes, (unsigned long)mode.Format.Graphics.PixelFormat))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    if (device->CommittedSourceFormat != (ULONG)mode.Format.Graphics.PixelFormat)
    {
        device->CommittedSourceFormat = (ULONG)mode.Format.Graphics.PixelFormat;
        GuardLog("display: CommitVidPn source %ux%u format %u stride %u",
                 (UINT)mode.Format.Graphics.PrimSurfSize.cx, (UINT)mode.Format.Graphics.PrimSurfSize.cy,
                 (UINT)mode.Format.Graphics.PixelFormat, (UINT)mode.Format.Graphics.Stride);
    }
    device->ModeActive = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250CommitVidPn(_In_ const HANDLE hAdapter, _In_ const DXGKARG_COMMITVIDPN* const Commit)
{
    BC250_DEVICE* device=(BC250_DEVICE*)hAdapter;
    NTSTATUS status;
    StartHealthEnter(device);
    status=CommitVidPnCore(hAdapter,Commit);
    // Only one inherited mode is accepted. Redundant commits of it preserve
    // the epoch; detach, failed commit and path power-off close presentation.
    StartHealthDisplayLocked(device,device->SourceVisible,
        NT_SUCCESS(status) && device->ModeActive && !Commit->Flags.PathPoweredOff);
    StartHealthLeave(device);
    // Outside the start-health mutex: the audio endpoint follows the monitor's power (DPMS off is an unplug for
    // audio, as Linux's az_disable), and takes only its own leaf lock (dpaudio.c).
    if (NT_SUCCESS(status) && Commit->Flags.PathPowerTransition)
        DpAudioPathPower(device,(BOOLEAN)!Commit->Flags.PathPoweredOff);
    return status;
}

NTSTATUS Bc250UpdateActiveVidPnPresentPath(_In_ const HANDLE hAdapter, _In_ const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* const Update)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (Update->VidPnPresentPathInfo.ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY &&
        Update->VidPnPresentPathInfo.ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED)
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250RecommendMonitorModes(_In_ const HANDLE hAdapter, _In_ const DXGKARG_RECOMMENDMONITORMODES* const Recommend)
{
    // The monitor has no descriptor in M3 (no EDID), so the OS would assume a default monitor that may not
    // list the firmware's resolution. Tell it that this monitor can do the mode it is showing right now.
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE* mode = NULL;
    NTSTATUS status;

    status = Recommend->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Recommend->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(status)) return status;

    status=FillSignalInfo(device, &mode->VideoSignalInfo);
    if (!NT_SUCCESS(status)) {
        Recommend->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Recommend->hMonitorSourceModeSet, mode);
        return status;
    }
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 8;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;

    status = Recommend->pMonitorSourceModeSetInterface->pfnAddMode(Recommend->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(status))
    {
        Recommend->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Recommend->hMonitorSourceModeSet, mode);
        if (status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) status = STATUS_SUCCESS;
    }
    return status;
}

NTSTATUS Bc250QueryVidPnHWCapability(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* Capability)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&Capability->VidPnHWCaps, sizeof(Capability->VidPnHWCaps));     // no hardware rotation, scaling or cloning
    return STATUS_SUCCESS;
}

// ---- present ----------------------------------------------------------------------------------------------

static void CopyRect(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_PRESENT_DISPLAYONLY* Present, RECT Rect)
{
    LONG y;

    if (Rect.left < 0) Rect.left = 0;
    if (Rect.top < 0) Rect.top = 0;
    if (Rect.right > (LONG)Device->Post.Width) Rect.right = (LONG)Device->Post.Width;
    if (Rect.bottom > (LONG)Device->Post.Height) Rect.bottom = (LONG)Device->Post.Height;
    if (Rect.left >= Rect.right || Rect.top >= Rect.bottom) return;

    for (y = Rect.top; y < Rect.bottom; y++)
    {
        const UCHAR* from = (const UCHAR*)Present->pSource + (SIZE_T)y * Present->Pitch + (SIZE_T)Rect.left * 4;
        UCHAR* to = (UCHAR*)Device->Framebuffer + (SIZE_T)y * Device->Post.Pitch + (SIZE_T)Rect.left * 4;
        RtlCopyMemory(to, from, (SIZE_T)(Rect.right - Rect.left) * 4);
    }
}

NTSTATUS Bc250PresentDisplayOnly(_In_ const HANDLE hAdapter, _In_ const DXGKARG_PRESENT_DISPLAYONLY* Present)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    ULONG i;

    if (Present->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (Present->BytesPerPixel != 4 || Present->Pitch <= 0) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    if (!device->Started || device->Framebuffer == NULL || !device->SourceVisible || !device->ModeActive)
        return STATUS_SUCCESS;

    if (!device->PresentSeen) GuardStage(StageFirstPresent);

    // The source image is the complete new frame, so a move is just one more rectangle that changed.
    for (i = 0; i < Present->NumMoves; i++) CopyRect(device, Present, Present->pMoves[i].DestRect);
    for (i = 0; i < Present->NumDirtyRects; i++) CopyRect(device, Present, Present->pDirtyRect[i]);
    g_Presents++;

    if (!device->PresentSeen) { device->PresentSeen = TRUE; GuardStage(StageFirstPresentDone); }
    return STATUS_SUCCESS;
}

// ---- system display (bugcheck screen): high IRQL, no pool, no registry ---------------------------------------

NTSTATUS Bc250SystemDisplayEnable(_In_ const PVOID MiniportDeviceContext, _In_ const D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                  _In_ const PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, _Out_ UINT* Width, _Out_ UINT* Height,
                                  _Out_ D3DDDIFORMAT* ColorFormat)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    NTSTATUS status;
    UNREFERENCED_PARAMETER(Flags);
    device->SystemDisplayReady=FALSE;
    *Width=0;*Height=0;*ColorFormat=D3DDDIFMT_UNKNOWN;
    // Single inherited output, advertised as always connected by QueryChildStatus.
    if (TargetId!=BC250_CHILD_UID && TargetId!=D3DDDI_ID_UNINITIALIZED) return STATUS_NOT_SUPPORTED;
    if (!device->Framebuffer || !IsPostFormatSupported(device->Post.ColorFormat) ||
        !device->Post.Width || !device->Post.Height ||
        (ULONGLONG)device->Post.Width*4>device->Post.Pitch ||
        (ULONGLONG)device->Post.Pitch*device->Post.Height>device->FramebufferLength)
        return STATUS_DEVICE_NOT_READY;
    status=DcnRestorePostDisplay(device);
    if (!NT_SUCCESS(status)) return status;
    *Width = device->Post.Width;
    *Height = device->Post.Height;
    *ColorFormat = device->Post.ColorFormat;
    device->SystemDisplayReady=TRUE;
    return STATUS_SUCCESS;
}

void Bc250SystemDisplayWrite(_In_ const PVOID MiniportDeviceContext, _In_ const PVOID Source, _In_ const UINT SourceWidth,
                             _In_ const UINT SourceHeight, _In_ const UINT SourceStride, _In_ const UINT PositionX,
                             _In_ const UINT PositionY)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    UINT y, width, height;

    if (!device->SystemDisplayReady || device->Framebuffer == NULL ||
        PositionX >= device->Post.Width || PositionY >= device->Post.Height) return;
    width = min(SourceWidth, device->Post.Width - PositionX);
    width = min(width, SourceStride / 4);
    height = min(SourceHeight, device->Post.Height - PositionY);
    for (y = 0; y < height; y++)
        RtlCopyMemory((UCHAR*)device->Framebuffer + (SIZE_T)(PositionY + y) * device->Post.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR*)Source + (SIZE_T)y * SourceStride, (SIZE_T)width * 4);
}
