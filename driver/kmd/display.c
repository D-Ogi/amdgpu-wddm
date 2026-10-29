// Display side of the M3 miniport: one source, one target, one mode (the one the firmware left), present by
// CPU copy into the firmware's framebuffer. No MMIO. Written against the documented display-only DDI.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "wave_snapshot.h"
#include "hip_journal.h"
#include "paging_journal.h"
#include "escape_commands.h"
#include "amd_identity.h"
#include "log_rate.h"
#include "display_timing.h"
#include "hdmi_link.h"
#include "display_timing_refused.h"   // BD-122: the registers of a refused inherited timing, for #38
#include "display_modes.h"
#include "dcn_translate.h"          // DcnPrimaryPitch: the pitch of a primary of a smaller source mode
#include "lab_gate.h"               // BD-167: the raw diagnostic escapes below are lab-only
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
#include "dcn_latch_escape.h"
#endif
#include "boot_logo.h"              // the boot-screen mascot drawn into the firmware's framebuffer at start
#include "display_rotation.h"       // BD-247: which path rotations this driver shows, and the rotated bugcheck screen

// display_rotation.h names the path rotation values as plain numbers for its host test; these are the WDK's.
C_ASSERT(BC250_VPPR_UNINITIALIZED == D3DKMDT_VPPR_UNINITIALIZED && BC250_VPPR_IDENTITY == D3DKMDT_VPPR_IDENTITY);
C_ASSERT(BC250_VPPR_ROTATE90 == D3DKMDT_VPPR_ROTATE90 && BC250_VPPR_ROTATE180 == D3DKMDT_VPPR_ROTATE180);
C_ASSERT(BC250_VPPR_ROTATE270 == D3DKMDT_VPPR_ROTATE270);
C_ASSERT(BC250_VPPR_IDENTITY_OFFSET90 == D3DKMDT_VPPR_IDENTITY_OFFSET90);
C_ASSERT(BC250_VPPR_ROTATE270_OFFSET270 == D3DKMDT_VPPR_ROTATE270_OFFSET270);
C_ASSERT(BC250_VPPR_UNPINNED == D3DKMDT_VPPR_UNPINNED && BC250_VPPR_NOTSPECIFIED == D3DKMDT_VPPR_NOTSPECIFIED);

// The settings DisplayDrawBootLogo reads. On by default: a new feature of a release is usable after the install.
#define BC250_BOOT_LOGO_DEFAULT 1ul
#define BC250_BOOT_LOGO_ANCHOR_DEFAULT 350ul

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

// What the display is fetching at the moment the picture is in the surface. The lab trials of 2026-10-14 could
// not answer that with a camera: its frames show the mascot on the panel and then a black screen, and a
// photograph of a black screen does not say whether the pipe was blanked, pointed at another surface, or given
// black pixels by the first present. The log did not carry the address either, because the "display inherited:
// surface" field is SurfaceConfig, a format register, not an address.
// Three reads this driver already performs elsewhere, once per start, after the draw.
// A failed read says so and prints no value: a zero address with a zero blank state would read as "scanning out
// nothing", which is a conclusion drawn from a register that was never read.
static void DisplayLogBootLogoScanout(_In_ BC250_DEVICE* Device)
{
    ULONG low = 0, high = 0, control = 0, blank = 0;
    const char* which = "the primary surface address";
    NTSTATUS status;

    status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &low);
    if (NT_SUCCESS(status))
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &high);
    if (NT_SUCCESS(status))
    {
        which = "the OTG control";
        status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_CONTROL, &control);
    }
    if (NT_SUCCESS(status))
    {
        which = "the OTG blank control";
        status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL, &blank);
    }
    if (!NT_SUCCESS(status))
    {
        GuardLog("boot logo: scanout not read at %s, status 0x%08X", which, (ULONG)status);
        return;
    }
    GuardLog("boot logo: scanout 0x%llX inherited 0x%llX otg master %u blank %u control 0x%08X",
             ((ULONGLONG)high << 32) | low, (ULONGLONG)Device->Post.PhysicAddress.QuadPart,
             (control & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) ? 1u : 0u,
             (blank & OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK) ? 1u : 0u, blank);
}

// The boot-screen mascot, drawn once in DxgkDdiStartDevice and nowhere else (boot_logo.h, docs/design/boot-logo.md).
//
// The window it is in the surface is between this point and the first present: the firmware's display pipe is
// still scanning out the surface the GOP left, and this driver has not yet committed a VidPN. After the first
// present the compositor owns every pixel of that surface, so nothing here needs to undo itself.
//
// Four conditions, all of them refusals and none of them a start failure:
//   1. The setting BootLogo (REG_DWORD under the driver's Parameters, 1 by default) is 1.
//   2. The framebuffer is mapped, and its format and pitch were already proved by DisplayMapFramebuffer.
//   3. Post.TargetId is D3DDDI_ID_UNINITIALIZED. The documentation of DxgkCbAcquirePostDisplayOwnership says
//      that an uninitialized target "typically occurs after a system boot", and that "in the case of a
//      driver-to-driver upgrade, the previous driver will have provided the target identifier". An upgrade start
//      hands over a running desktop, not a boot screen: the mascot must not be painted over it.
//   4. The picture fits (boot_logo.c decides).
// The bugcheck display is a separate path (Bc250SystemDisplayEnable/Write) and never reaches this code.
void DisplayDrawBootLogo(_In_ BC250_DEVICE* Device)
{
    BC250_BOOT_LOGO_TARGET target;
    BC250_BOOT_LOGO_RESULT result;
    ULONG written = 0, anchor, microseconds;
    LARGE_INTEGER frequency;
    LONGLONG begin, end;

    // Said, not silent: a support file that holds no "boot logo:" line at all cannot tell a driver whose owner
    // turned the picture off from one in which this code never ran.
    if (GuardReadSetting(L"BootLogo", BC250_BOOT_LOGO_DEFAULT) != 1)
    {
        GuardLog("boot logo: off by setting (BootLogo 0)");
        return;
    }
    if (Device->Framebuffer == NULL) { GuardLog("boot logo: no framebuffer"); return; }
    if (Device->Post.TargetId != D3DDDI_ID_UNINITIALIZED)
    {
        GuardLog("boot logo: skipped, target %u is a driver handoff, not a boot", Device->Post.TargetId);
        return;
    }
    if ((ULONGLONG)Device->Post.Pitch * Device->Post.Height > Device->FramebufferLength)
    {
        GuardLog("boot logo: skipped, mapping shorter than the surface");
        return;
    }
    anchor = GuardReadSetting(L"BootLogoAnchorPermille", BC250_BOOT_LOGO_ANCHOR_DEFAULT);
    target.Width = Device->Post.Width;
    target.Height = Device->Post.Height;
    target.Pitch = Device->Post.Pitch;
    target.AnchorPermille = anchor;
    // Timed: this is the only write this driver makes to the inherited surface with a known byte count, so the
    // microseconds and the pixels together are the write-combine rate of that mapping, measured on the real
    // surface at every start instead of guessed. Nothing depends on the number; it is for the log.
    begin = KeQueryPerformanceCounter(&frequency).QuadPart;
    result = Bc250BootLogoDraw(Device->Framebuffer, Bc250BootLogoAsset(), &target, &written);
    end = KeQueryPerformanceCounter(NULL).QuadPart;
    microseconds = (frequency.QuadPart > 0 && end > begin)
                 ? (ULONG)(((ULONGLONG)(end - begin) * 1000000ull) / (ULONGLONG)frequency.QuadPart) : 0;
    GuardLog("boot logo: %s on %ux%u anchor %u, %u pixels, %u bytes, %u us",
             Bc250BootLogoResultText(result), target.Width, target.Height, anchor, written, written * 4u,
             microseconds);
    // After the draw, so it reports the hardware's state while the picture is in the surface. It answers the
    // question the camera could not: whether the inherited surface is the one being scanned out at that moment.
    DisplayLogBootLogoScanout(Device);
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

// The escape runs in the caller's context, and these escapes are for administrators. BD-167 (K4): check the
// effective caller through the subject context, which is the impersonation token when a thread impersonates a
// client. The old form read the process primary token, so a privileged service that impersonated an
// unprivileged client still passed. This is the same check WaveCallerIsAdmin already makes; both exist because
// CallerIsAdmin is called before WaveEscape is defined.
static BOOLEAN CallerIsAdmin(void)
{
    SECURITY_SUBJECT_CONTEXT subject;
    BOOLEAN admin;

    SeCaptureSubjectContext(&subject);
    SeLockSubjectContext(&subject);
    admin = SeTokenIsAdmin(SeQuerySubjectContextToken(&subject));
    SeUnlockSubjectContext(&subject);
    SeReleaseSubjectContext(&subject);
    return admin;
}

// Full WDDM owns the engines and translation tables for its entire lifetime.
// Diagnostic PLAN can also allocate/reset software state, so only known
// observational commands are admitted. The entry dispatcher still checks sizes.
static BOOLEAN WddmDiagnosticAllowed(const BC250_ESCAPE* Data, ULONG Bytes)
{
#ifndef AMDGPU_WDDM_LAB_FLAVOUR
    (void)Bytes;   // BD-167: only the lab-only RUN_GFX/RUN_IH STATE cases below read Bytes (the dcn-observe host harness has no WDK macros)
#endif
    switch (Data->Command) {
    case BC250_ESCAPE_RUN_START_HEALTH:
    case BC250_ESCAPE_OBSERVE_DCN:
    case BC250_ESCAPE_RUN_CLOCK:
    case BC250_ESCAPE_RUN_CPU:      // the operator's own surface, like RUN_CLOCK: one mailbox sequence, bounded
    case BC250_ESCAPE_RUN_DPAUDIO:  // reads, and the INDEX selects of indirect reads; the record (BC250_ESCAPE_DPAUDIO)
    case BC250_ESCAPE_GET_INFO:
    case BC250_ESCAPE_GET_LOG:
    case BC250_ESCAPE_LOG_SUMMARY:
    case BC250_ESCAPE_GET_PAGING_JOURNAL:
        return TRUE;
    // BD-167: the raw diagnostic commands are not compiled into the release dispatch, so the release allow-list
    // must not name them either. In the lab flavour they remain admissible under FullWddm.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    // Controlled writer: the dedicated handler requires Level Two and the
    // primary exclusive owner, unlike the refused raw RUN_DCNFLIP path.
    case BC250_ESCAPE_LATCH_PROBE_COMMAND:
        return TRUE;
    case BC250_ESCAPE_READ_REG:
    case BC250_ESCAPE_GET_MEMORY:
    case BC250_ESCAPE_VRAM_READ:
    case BC250_ESCAPE_RUN_DCN:
    case BC250_ESCAPE_RUN_FBDUMP:
        return TRUE;
    case BC250_ESCAPE_RUN_GFX:
        return Bytes>=sizeof(BC250_ESCAPE_GFX) &&
            ((const BC250_ESCAPE_GFX*)Data)->Op==BC250_GFX_OP_STATE;
    case BC250_ESCAPE_RUN_IH:
        return Bytes>=sizeof(BC250_ESCAPE_IH) &&
            ((const BC250_ESCAPE_IH*)Data)->Op==BC250_IH_OP_STATE;
#endif
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
    // BD-168: NtStatus as well, the same omission as LogEscape had above. No shipping consumer reads this reply's
    // NtStatus today, and that is the reason to set it now rather than when one does: a reader that falls back to
    // STATUS_DEVICE_NOT_READY on a zero would report that the driver is not ready, and the next reader of this
    // escape would hunt a device fault that is a missing administrator right.
    if (!CallerIsAdmin()) {
        journal->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        journal->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return STATUS_SUCCESS;
    }
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
    // BD-168: NtStatus as well, not Status alone. Bc250LogRead reports data->NtStatus when the reply is not
    // DONE and falls back to STATUS_DEVICE_NOT_READY when it is zero, so a support report taken without
    // administrator rights said "the driver is not ready" and kept no lines (GitHub #40 on tester.30).
    if (!CallerIsAdmin()) {
        log->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        log->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return STATUS_SUCCESS;
    }

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

static BOOLEAN WaveCallerIsAdmin(void)
{
    SECURITY_SUBJECT_CONTEXT subject;
    BOOLEAN admin;
    // Use the effective caller, including an impersonated client. A service's
    // privileged primary token must not authorize its unprivileged client.
    SeCaptureSubjectContext(&subject);
    SeLockSubjectContext(&subject);
    admin = SeTokenIsAdmin(SeQuerySubjectContextToken(&subject));
    SeUnlockSubjectContext(&subject);
    SeReleaseSubjectContext(&subject);
    return admin;
}

static void WaveEscape(BC250_DEVICE* Device, BC250_ESCAPE_WAVES* Data, ULONG Flags)
{
    ULONG abi = Data->AbiVersion, op = Data->Op, reserved = Data->Reserved;
    D3DDDI_ESCAPEFLAGS required = {0};
    BC250_WAVE_SNAPSHOT* snapshot;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    C_ASSERT(sizeof(BC250_WAVE_RECORD) == 96);
    C_ASSERT(sizeof(BC250_WAVE_HEADER) == 128);
    C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_WAVES, Snapshot) == 32);
    C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_WAVES, Status) == FIELD_OFFSET(BC250_ESCAPE, Status));

    // Capture inputs once, then work only on private storage. No caller-controlled
    // count, pointer, register offset or write value enters the hardware loop.
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Magic = BC250_ESCAPE_MAGIC;
    Data->Command = BC250_ESCAPE_GET_WAVES;
    Data->Version = BC250_KMD_VERSION;
    Data->AbiVersion = BC250_WAVE_ABI_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->Snapshot.Header.AbiVersion = BC250_WAVE_ABI_VERSION;
    Data->Snapshot.Header.DriverVersion = BC250_KMD_VERSION;
    Data->Snapshot.Header.Bytes = sizeof(BC250_WAVE_HEADER);
    Data->Snapshot.Header.Flags = BC250_WAVE_READ_ERROR;
    // HardwareAccess asks dxgkrnl for GPU idle, which defeats a live wave read.
    // Own locks protect selectors and lifetime without requesting that idle wait.
    required.NoAdapterSynchronization = 1;
    if (!WaveCallerIsAdmin()) {
        status = STATUS_ACCESS_DENIED;
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
    } else if (Flags == required.Value && abi == BC250_WAVE_ABI_VERSION && op == 0 && reserved == 0) {
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) status = STATUS_INVALID_DEVICE_STATE;
        else {
            snapshot = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*snapshot), 'vW2B');
            if (snapshot == NULL) status = STATUS_INSUFFICIENT_RESOURCES;
            else {
                status = GfxCaptureWaves(Device, snapshot);
                RtlCopyMemory(&Data->Snapshot, snapshot, sizeof(*snapshot));
                ExFreePoolWithTag(snapshot, 'vW2B');
            }
        }
        if (NT_SUCCESS(status)) Data->Status = BC250_ESCAPE_STATUS_DONE;
    }
    Data->NtStatus = status;
    Data->Snapshot.Header.Result = status;
}

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
static void ConnectorReplugEscape(BC250_DEVICE* Device, BC250_ESCAPE_CONNECTOR_REPLUG_DATA* Data, ULONG Flags)
{
    ULONG abi = Data->AbiVersion, reserved = 0, i, generation = 0;
    BOOLEAN queued = FALSE;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    D3DDDI_ESCAPEFLAGS required = {0};
    for (i = 0; i < RTL_NUMBER_OF(Data->Reserved); ++i) reserved |= Data->Reserved[i];
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Magic = BC250_ESCAPE_MAGIC;
    Data->Command = BC250_ESCAPE_CONNECTOR_REPLUG;
    Data->Version = BC250_KMD_VERSION;
    Data->AbiVersion = BC250_CONNECTOR_REPLUG_ABI;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    required.NoAdapterSynchronization = 1;
    // HardwareAccess would exclude the flip whose overlap this arm measures.
    // The connector's queue lock and stop drain protect this software request.
    if (!WaveCallerIsAdmin()) {
        status = STATUS_ACCESS_DENIED;
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
    } else if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (Flags == required.Value && abi == BC250_CONNECTOR_REPLUG_ABI && !reserved) {
        if (!LabDiagnosticsAllowed()) status = STATUS_ACCESS_DENIED;
        else {
            status = ConnectorReplug(Device, &generation, &queued);
            if (NT_SUCCESS(status)) {
                Data->Status = BC250_ESCAPE_STATUS_DONE;
                LabEscapeAdmitted(LabEscapeConnectorReplug, "lab:connector-replug");
                GuardLog("connector: lab replug generation %lu dpc queued %u", generation, queued);
            }
        }
    }
    Data->Generation = generation;
    Data->DpcQueued = queued;
    Data->NtStatus = (ULONG)status;
}
#endif

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
static void LatchProbeEscape(BC250_DEVICE* Device, BC250_ESCAPE_LATCH_PROBE* Data,
                             BOOLEAN Admin, ULONG Flags)
{
    ULONG abi=Data->AbiVersion, arm=Data->Arm, expected=Data->ExpectedVersion;
    ULONG reserved=0, i;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    D3DDDI_ESCAPEFLAGS required={0};
    for (i=0;i<RTL_NUMBER_OF(Data->Reserved);++i) reserved|=Data->Reserved[i];
    RtlZeroMemory(Data,sizeof(*Data));
    Data->Magic=BC250_ESCAPE_MAGIC;
    Data->Command=BC250_ESCAPE_LATCH_PROBE_COMMAND;
    Data->Version=BC250_KMD_VERSION;
    Data->AbiVersion=BC250_LATCH_PROBE_ABI;
    Data->Arm=arm;
    Data->ExpectedVersion=expected;
    Data->Status=BC250_ESCAPE_STATUS_REFUSED;
    // Zero means NT_SUCCESS, so explicitly mark steps that did not run.
    Data->Result.Status=Data->Result.UnlockStatus=Data->Result.RestoreStatus=
        Data->Result.EndStatus=STATUS_INVALID_DEVICE_STATE;
    required.HardwareAccess=1;
    if (!Admin) {
        status=STATUS_ACCESS_DENIED;
        Data->Status=BC250_ESCAPE_STATUS_NOT_ADMIN;
    } else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) {
        status=STATUS_INVALID_DEVICE_STATE;
    } else if (Flags==required.Value && abi==BC250_LATCH_PROBE_ABI &&
               expected==BC250_KMD_VERSION && arm<Bc250LatchArmCount && !reserved) {
        if (!LabDiagnosticsAllowed()) status=STATUS_ACCESS_DENIED;
        else if (!Device->FullWddm ||
                 InterlockedCompareExchange(&Device->RetainedPowerPhase,0,0)!=0)
            status=STATUS_DEVICE_NOT_READY;
        else {
            LabEscapeAdmitted(LabEscapeLatchProbe,"lab:latch-probe");
            status=DcnLatchProbe(Device,arm,&Data->Result);
            if (NT_SUCCESS(status)) Data->Status=BC250_ESCAPE_STATUS_DONE;
        }
    }
    Data->NtStatus=(ULONG)status;
}
#endif

NTSTATUS Bc250Escape(_In_ const HANDLE hAdapter, _In_ const DXGKARG_ESCAPE* Escape)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_ESCAPE* data = (BC250_ESCAPE*)Escape->pPrivateDriverData;
    D3DDDI_ESCAPEFLAGS softwareRead = {0};
    ULONG command;

    Bc250EscapeCommandsUnique();

    if (Escape->PrivateDriverDataSize >= 8 && data != NULL && data->Magic == BC250_ESCAPE_MAGIC &&
        data->Command == BC250_HIP_JOURNAL_COMMAND)
        return HipJournalEscape(device, Escape);
    if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE) || data == NULL || data->Magic != BC250_ESCAPE_MAGIC)
        return STATUS_INVALID_PARAMETER;
    // GET_LOG and GET_PAGING_JOURNAL with NoAdapterSynchronization alone (0.7.184.1): driver-image state only, so
    // they are answered ahead of the power-phase check (SoftwareReadEscape). The command is read once, so the read
    // that runs is the read that was admitted; every other flag combination takes the path below, as before.
    command = data->Command;
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (command == BC250_ESCAPE_CONNECTOR_REPLUG) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_CONNECTOR_REPLUG_DATA))
            return STATUS_INVALID_PARAMETER;
        ConnectorReplugEscape(device, (BC250_ESCAPE_CONNECTOR_REPLUG_DATA*)data, Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
#endif
    if (command == BC250_ESCAPE_RUN_BOARD_CAPS) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_BOARD_CAPS)) return STATUS_INVALID_PARAMETER;
        BoardProviderCaps(device, (BC250_ESCAPE_BOARD_CAPS*)data, Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    if (command == BC250_ESCAPE_GET_WAVES) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_WAVES)) return STATUS_INVALID_PARAMETER;
        WaveEscape(device, (BC250_ESCAPE_WAVES*)data, Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
    if (command == BC250_ESCAPE_RUN_BOARD_MEMORY) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_BOARD_MEMORY)) return STATUS_INVALID_PARAMETER;
        if (((BC250_ESCAPE_BOARD_MEMORY*)data)->Op == BC250_BOARD_MEMORY_OP_PROBE)
            BoardMemoryProbeRequest(device, (BC250_ESCAPE_BOARD_MEMORY_PROBE*)data, CallerIsAdmin(), Escape->Flags.Value);
        else
            BoardMemoryRequest(device, (BC250_ESCAPE_BOARD_MEMORY*)data, CallerIsAdmin(), Escape->Flags.Value);
        return STATUS_SUCCESS;
    }
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
    // The shell's FSR 4 identity value for the calling process: software state of our process object (amd_identity.c).
    if (command == BC250_ESCAPE_SET_PROCESS_POLICY) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_PROCESS_POLICY)) return STATUS_INVALID_PARAMETER;
        AmdIdentityEscape(WddmProcessIdentity(Escape->hKmdProcessHandle), (BC250_ESCAPE_PROCESS_POLICY*)data);
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
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (command == BC250_ESCAPE_LATCH_PROBE_COMMAND) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_LATCH_PROBE)) return STATUS_INVALID_PARAMETER;
        LatchProbeEscape(device,(BC250_ESCAPE_LATCH_PROBE*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS; // measurement and cleanup statuses remain separate
    }
#endif
    if (data->Command == BC250_ESCAPE_OBSERVE_DCN) {
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DCN_OBSERVE)) return STATUS_INVALID_PARAMETER;
        DcnObserve(device,(BC250_ESCAPE_DCN_OBSERVE*)data,CallerIsAdmin(),Escape->Flags.Value);
        return STATUS_SUCCESS; // typed operation status is in the reply
    }
    if (data->Command == BC250_ESCAPE_RUN_DPAUDIO) {
        // Three exact sizes: ABI 3, the whole structure, and its ABI 2 and ABI 1 prefixes. DpAudioRequest matches
        // the size against AbiVersion and touches nothing past the size it was given.
        if (Escape->PrivateDriverDataSize != sizeof(BC250_ESCAPE_DPAUDIO) &&
            Escape->PrivateDriverDataSize != BC250_DPAUDIO_ABI2_SIZE &&
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
    // BD-167: the raw memory, GART, PSP, submit/fence, interrupt-ring, scanout-dump and SDMA escapes exist only
    // in the lab flavour, and even there only when LabDiagnosticsAllowed() is true (test signing on and
    // Parameters\LabDiagnostics = 1). The release .sys does not contain them: the commands fall through to
    // UNKNOWN_COMMAND below. The arms keep their original body and indentation so the hardening diff stays readable.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (LabDiagnosticsAllowed()) {
    if (data->Command >= BC250_ESCAPE_GET_MEMORY && data->Command <= BC250_ESCAPE_VRAM_WRITE)
    {
        BC250_ESCAPE_MEMORY* memory = (BC250_ESCAPE_MEMORY*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_MEMORY)) return STATUS_INVALID_PARAMETER;
        LabEscapeAdmitted(LabEscapeMemory, "raw escape GET_MEMORY/VRAM_READ/VRAM_WRITE");
        if (!CallerIsAdmin()) memory->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else VramEscape(device, memory);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_GART)
    {
        BC250_ESCAPE_GART* gart = (BC250_ESCAPE_GART*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_GART)) return STATUS_INVALID_PARAMETER;
        gart->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) |
                      (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0);
        LabEscapeAdmitted(LabEscapeGart, "raw escape RUN_GART");
        if (!CallerIsAdmin()) gart->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else GartEscape(device, gart);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_PSP)
    {
        BC250_ESCAPE_PSP* psp = (BC250_ESCAPE_PSP*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_PSP)) return STATUS_INVALID_PARAMETER;
        psp->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) |
                     (device->MmioGartEnabled ? BC250_ESCAPE_FLAG_GART : 0) | (device->MmioPspEnabled ? BC250_ESCAPE_FLAG_PSP : 0);
        LabEscapeAdmitted(LabEscapePsp, "raw escape RUN_PSP");
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
        LabEscapeAdmitted(LabEscapeGfx, "raw escape RUN_GFX");
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
        LabEscapeAdmitted(LabEscapeFence, "raw escape RUN_FENCE");
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
        LabEscapeAdmitted(LabEscapeIh, "raw escape RUN_IH");
        if (!CallerIsAdmin()) ih->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else IhEscape(device, ih);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_DCN)
    {
        BC250_ESCAPE_DCN* dcn = (BC250_ESCAPE_DCN*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_DCN)) return STATUS_INVALID_PARAMETER;
        dcn->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0);
        LabEscapeAdmitted(LabEscapeDcn, "raw escape RUN_DCN");
        if (!CallerIsAdmin()) dcn->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else DcnEscape(device, dcn);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_DCNFLIP)
    {
        BC250_ESCAPE_DCNFLIP* flip = (BC250_ESCAPE_DCNFLIP*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_DCNFLIP)) return STATUS_INVALID_PARAMETER;
        flip->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->DcnWriteEnabled ? BC250_ESCAPE_FLAG_DCN_WRITE : 0) |
                      (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) | (device->VramWriteEnabled ? BC250_ESCAPE_FLAG_VRAM_WRITE : 0);
        LabEscapeAdmitted(LabEscapeDcnFlip, "raw escape RUN_DCNFLIP");
        if (!CallerIsAdmin()) flip->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else DcnFlipEscape(device, flip);
        return STATUS_SUCCESS;
    }
    if (data->Command == BC250_ESCAPE_RUN_FBDUMP)
    {
        BC250_ESCAPE_FBDUMP* fbdump = (BC250_ESCAPE_FBDUMP*)Escape->pPrivateDriverData;

        if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE_FBDUMP)) return STATUS_INVALID_PARAMETER;
        fbdump->Flags = (device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0);
        LabEscapeAdmitted(LabEscapeFbdump, "raw escape RUN_FBDUMP");
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
        LabEscapeAdmitted(LabEscapeSdma, "raw escape RUN_SDMACOPY/RUN_SDMAIB");
        if (!CallerIsAdmin()) sdmacopy->Status = BC250_ESCAPE_STATUS_NOT_ADMIN; else SdmaCopyEscape(device, sdmacopy);
        return STATUS_SUCCESS;
    }
    }   /* LabDiagnosticsAllowed */
#endif /* AMDGPU_WDDM_LAB_FLAVOUR */
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

    // BD-167 (K2/K9): arbitrary register read/write is lab-only, like the block above. Release falls through to
    // UNKNOWN_COMMAND. GET_INFO (the overlay's gate query) stays and is answered below.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (LabDiagnosticsAllowed() && (data->Command == BC250_ESCAPE_READ_REG || data->Command == BC250_ESCAPE_WRITE_REG))
    {
        ULONG value = 0;
        NTSTATUS status;

        LabEscapeAdmitted(LabEscapeRegister, "raw escape READ_REG/WRITE_REG");
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
#endif /* AMDGPU_WDDM_LAB_FLAVOUR */
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

// The VidPN scaling of a present path as modeset.c names it; BC250_SCALING_COUNT for "no preference" (unpinned,
// uninitialized, not specified) or an unrecognized value. Custom is retained firmware only.
static ULONG ScalingFromPath(D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling)
{
    switch (Scaling)
    {
    case D3DKMDT_VPPS_IDENTITY: return BC250_SCALING_IDENTITY;
    case D3DKMDT_VPPS_CENTERED: return BC250_SCALING_CENTERED;
    case D3DKMDT_VPPS_STRETCHED: return BC250_SCALING_STRETCHED;
    case D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX: return BC250_SCALING_ASPECT;
    case D3DKMDT_VPPS_CUSTOM: return BC250_SCALING_CUSTOM;
    default: return BC250_SCALING_COUNT;
    }
}

// Is the path's scaling one dxgkrnl pinned, as opposed to "no preference"?
static BOOLEAN ScalingPinned(D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling)
{
    return Scaling != D3DKMDT_VPPS_UNPINNED && Scaling != D3DKMDT_VPPS_UNINITIALIZED && Scaling != D3DKMDT_VPPS_NOTSPECIFIED;
}

// BD-139: the OTG0 measurement of the inherited TMDS start, for a pipe the connector just enabled. GitHub #45 review:
// with its own window up to BC250_REFRESH_MEASURE_MAX_HZ, not the 240 Hz of the TMDS start.
NTSTATUS DisplayMeasureRefresh(_In_ const BC250_DEVICE* Device, ULONG HTotal, ULONG VTotal, ULONG PixelKhz,
                               _Out_ ULONG* PixelHz, _Out_ ULONG* RefreshMilliHz)
{
    return DisplayMeasureFast(Device, HTotal, VTotal, PixelKhz, PixelHz, RefreshMilliHz);
}

NTSTATUS DisplayPrepareInheritedTiming(_Inout_ BC250_DEVICE* Device)
{
    D3DKMDT_VIDEO_SIGNAL_INFO signal;
    D3DKMDT_VIDEO_SIGNAL_INFO* Signal=&signal;
    NTSTATUS status=STATUS_SUCCESS;
    Device->InheritedSignalValid=FALSE;
    Device->InheritedTmds=FALSE;
    Device->Modeset.InheritedFixed=FALSE;
    Device->Modeset.InheritedSupport=0;
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
    if (!NT_SUCCESS(status)) { DisplayLogRefusedTiming(Device,status); return status; }
    if (Device->FullWddm) status=ModesetPrepareInherited(Device,Signal);
    if (!NT_SUCCESS(status)) return status; // the pipe diagnostic retains the actual failure and read mask
    Device->InheritedSignal=*Signal;
    Device->InheritedSignalValid=TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS DisplayValidateInheritedSignal(BC250_DEVICE* Device)
{
    D3DKMDT_VIDEO_SIGNAL_INFO signal = Device->InheritedSignal;
    const D3DKMDT_VIDEO_SIGNAL_INFO* saved = &Device->InheritedSignal;
    BOOLEAN tmds = Device->InheritedTmds, measuredTmds;
    NTSTATUS status;
    ULONGLONG delta;
    if (!Device->InheritedSignalValid) return STATUS_DEVICE_NOT_READY;
    Device->InheritedTmds = FALSE;
    status = DisplayReadInheritedTiming(Device,&signal);
    measuredTmds = Device->InheritedTmds;
    Device->InheritedTmds = tmds;
    if (!NT_SUCCESS(status)) return status;
    if (tmds != measuredTmds || signal.ActiveSize.cx != saved->ActiveSize.cx ||
        signal.ActiveSize.cy != saved->ActiveSize.cy || signal.TotalSize.cx != saved->TotalSize.cx ||
        signal.TotalSize.cy != saved->TotalSize.cy) return STATUS_DEVICE_BUSY;
    delta = signal.PixelRate > saved->PixelRate ? signal.PixelRate - saved->PixelRate : saved->PixelRate - signal.PixelRate;
    // DTO is exact. The counter-measured TMDS path has the half-window 0.5%
    // stability bound plus its 10 kHz rounding interval. Never publish a new
    // tuple under retained allocations, even when a repeat is within that bound.
    if (delta > (tmds ? saved->PixelRate / 200u + 10000u : 0u)) return STATUS_DEVICE_BUSY;
    return STATUS_SUCCESS;

}

// Reuse the .36 verifier, which preserves the latched TMDS classification.
NTSTATUS DisplayVerifyInheritedTiming(BC250_DEVICE *Device)
{
    return DisplayValidateInheritedSignal(Device);

}

// This driver preserves one inherited mode for the device start. Both mode
// enumeration and allocation descriptions consume exactly this cached tuple.
static NTSTATUS FillSignalInfo(_In_ const BC250_DEVICE* Device, _Out_ D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    if (ConnectorIsEnabled((BC250_DEVICE*)Device))
        return ConnectorLinkCurrentSignal((BC250_DEVICE*)Device, Signal);
    if (!Device->InheritedSignalValid) return STATUS_DEVICE_NOT_READY;
    *Signal=Device->InheritedSignal;
    return STATUS_SUCCESS;
}

// Display modes (modeset.c): every size of this start's mode list, each in every pixel format of display_modes.h.
// The native size keeps the firmware's pitch; a smaller size gets the pitch its primary allocation will have
// (DcnPrimaryPitch, gdi_admission.h). With a scaling pinned on the path, only the sizes that scaling admits.
static NTSTATUS OfferSourceMode(_In_ const BC250_DEVICE* Device, _In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId, D3DKMDT_VIDPN_PRESENT_PATH_SCALING PathScaling)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE* mode = NULL;
    NTSTATUS status;
    unsigned long formats[BC250_SOURCE_MODE_MAX], count, i;
    const BC250_MODE_LIST* list = &Device->Modeset.Modes;
    const ULONG sizes = list->Count ? list->Count : 1;     // no list (start not reached): the native size alone
    const ULONG filter = ScalingPinned(PathScaling) ? ScalingFromPath(PathScaling) : BC250_SCALING_COUNT;
    ULONG size, offered = 0;

    // One mode per pixel format of display_modes.h: the scan-out format first, then (full table only) the formats
    // the UMDs compose, so that DXGI lists modes for them too.
    count = Bc250SourceModeFormats(Device->ComposedSourceModes, formats, BC250_SOURCE_MODE_MAX);
    if (count == 0 || count > BC250_SOURCE_MODE_MAX) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    status = VidPn->pfnCreateNewSourceModeSet(hVidPn, SourceId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;

    for (size = 0; size < sizes && NT_SUCCESS(status); ++size)
    {
        const ULONG width = list->Count ? list->Modes[size].Width : Device->Post.Width;
        const ULONG height = list->Count ? list->Modes[size].Height : Device->Post.Height;
        const BOOLEAN native = (width == Device->Post.Width && height == Device->Post.Height);
        const ULONG support = ModesetScalingSupport(Device, width, height);
        if ((!native || Device->Modeset.InheritedFixed) && (support == 0 ||
            (ScalingPinned(PathScaling) && (filter == BC250_SCALING_COUNT || !(support & BC250_SCALING_BIT(filter))))))
            continue;
        for (i = 0; i < count && NT_SUCCESS(status); ++i)
        {
            const ULONG stride = Bc250SourceModeStride(formats[i], native ? Device->Post.Pitch : DcnPrimaryPitch(width));
            if (stride == 0) { status = STATUS_GRAPHICS_INVALID_STRIDE; break; }
            status = set->pfnCreateNewModeInfo(hSet, &mode);
            if (!NT_SUCCESS(status)) break;
            mode->Type = D3DKMDT_RMT_GRAPHICS;
            mode->Format.Graphics.PrimSurfSize.cx = width;
            mode->Format.Graphics.PrimSurfSize.cy = height;
            mode->Format.Graphics.VisibleRegionSize = mode->Format.Graphics.PrimSurfSize;
            mode->Format.Graphics.Stride = stride;
            mode->Format.Graphics.PixelFormat = (D3DDDIFORMAT)formats[i];
            mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
            mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
            status = set->pfnAddMode(hSet, mode);
            if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(hSet, mode);
            else offered++;
        }
    }
    // The native size is always cofunctional with identity; an empty set would leave dxgkrnl no mode at all.
    if (NT_SUCCESS(status) && offered == 0) status = STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
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
                                   D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, _Out_ BOOLEAN* Pinned,
                                   _Out_opt_ D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
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
        if (Signal != NULL) *Signal = pinned->VideoSignalInfo;
        set->pfnReleaseModeInfo(hSet, pinned);
    }
    VidPn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return (status == STATUS_GRAPHICS_MODE_NOT_PINNED) ? STATUS_SUCCESS : status;
}

// BD-247: the angle of the committed path, which the bugcheck screen (SystemDisplayWrite) and the scan-out caps
// trailer (wddm.c) read. The pipe itself is not reprogrammed: the compositor draws a rotated path's content already
// rotated into a primary of the physical size (display_rotation.h). Each change is logged once.
static void DisplayRecordRotation(BC250_DEVICE* Device, ULONG Angle, const char* Where)
{
    const ULONG value = Bc250RotationValue(Angle);
    if ((ULONG)Device->Rotation == value) return;
    GuardLog("display: rotation %lu -> %lu degrees (%s)",
        (ULONG)Bc250RotationAngle((ULONG)Device->Rotation, TRUE), Angle, Where);
    Device->Rotation = value;
}

// Exact inherited tuple only. The same check serves IsSupportedVidPn and the
// final commit, so a saved OS mode cannot silently reprogram this pipeline.
static const char* DisplayInheritedModeRefusal(const BC250_DEVICE* Device,
    const D3DKMDT_VIDPN_SOURCE_MODE* Mode, const D3DKMDT_VIDPN_PRESENT_PATH* Path,
    const D3DKMDT_VIDEO_SIGNAL_INFO* Target)
{
    const D3DKMDT_VIDEO_SIGNAL_INFO* actual = &Device->InheritedSignal;
    ULONG scaling = ScalingFromPath(Path->ContentTransformation.Scaling);
    if (!Device->InheritedSignalValid) return "signal-invalid";
    if (Mode->Type != D3DKMDT_RMT_GRAPHICS) return "source-type";
    if (Path->VidPnSourceId != 0 || Path->VidPnTargetId != BC250_CHILD_UID) return "path-ids";
    // BD-247: the retained pipe does not rotate either; the compositor's content arrives rotated.
    if (Bc250RotationAngle((ULONG)Path->ContentTransformation.Rotation, Device->FullWddm) == BC250_ROTATION_REFUSED)
        return "rotation";
    if (!Device->Modeset.InheritedSupport || (ScalingPinned(Path->ContentTransformation.Scaling) &&
        (scaling >= BC250_SCALING_COUNT || !(Device->Modeset.InheritedSupport & BC250_SCALING_BIT(scaling))))) return "scaling";
    if (Path->VisibleFromActiveTLOffset.cx || Path->VisibleFromActiveTLOffset.cy ||
        Path->VisibleFromActiveBROffset.cx || Path->VisibleFromActiveBROffset.cy) return "visible-offset";
    if (Mode->Format.Graphics.PrimSurfSize.cx != Device->Post.Width ||
        Mode->Format.Graphics.PrimSurfSize.cy != Device->Post.Height) return "source-size";
    if (Mode->Format.Graphics.VisibleRegionSize.cx != Device->Post.Width ||
        Mode->Format.Graphics.VisibleRegionSize.cy != Device->Post.Height) return "source-visible";
    if (!Bc250SourceModeAdmitted(Device->ComposedSourceModes,(ULONG)Mode->Format.Graphics.PixelFormat)) return "source-format";
    if (Mode->Format.Graphics.Stride != Bc250SourceModeStride((ULONG)Mode->Format.Graphics.PixelFormat,Device->Post.Pitch)) return "source-stride";
    if (Target->ActiveSize.cx != actual->ActiveSize.cx || Target->ActiveSize.cy != actual->ActiveSize.cy) return "target-active";
    if (Target->TotalSize.cx != actual->TotalSize.cx || Target->TotalSize.cy != actual->TotalSize.cy) return "target-total";
    if (Target->PixelRate != actual->PixelRate) return "target-clock";
    if (Target->ScanLineOrdering != actual->ScanLineOrdering) return "target-scan";
    if (!Target->HSyncFreq.Denominator || !Target->VSyncFreq.Denominator ||
        (ULONGLONG)Target->HSyncFreq.Numerator * actual->HSyncFreq.Denominator !=
            (ULONGLONG)actual->HSyncFreq.Numerator * Target->HSyncFreq.Denominator ||
        (ULONGLONG)Target->VSyncFreq.Numerator * actual->VSyncFreq.Denominator !=
            (ULONGLONG)actual->VSyncFreq.Numerator * Target->VSyncFreq.Denominator) return "target-frequency";
    return NULL;
}

static BOOLEAN DisplayInheritedModeMatches(const BC250_DEVICE* Device,
    const D3DKMDT_VIDPN_SOURCE_MODE* Mode, const D3DKMDT_VIDPN_PRESENT_PATH* Path,
    const D3DKMDT_VIDEO_SIGNAL_INFO* Target)
{
    return DisplayInheritedModeRefusal(Device, Mode, Path, Target) == NULL;
}

// Runtime connector modes are exact, progressive, unscaled EDID targets. The
// snapshot remains pinned across Windows callbacks; generation is rechecked
// before success or hardware use. Boot's InheritedSignal is never overwritten.
static void ConnectorSignal(const BC250_TARGET_TIMING* Timing, D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    RtlZeroMemory(Signal, sizeof(*Signal));
    Signal->VideoStandard = D3DKMDT_VSS_OTHER;
    Signal->ActiveSize.cx = Timing->HActive;
    Signal->ActiveSize.cy = Timing->VActive;
    Signal->TotalSize.cx = Timing->HActive + Timing->HBlank;
    Signal->TotalSize.cy = Timing->VActive + Timing->VBlank;
    Signal->PixelRate = (SIZE_T)Timing->PixelClockKhz * 1000u;
    Signal->HSyncFreq.Numerator = (UINT)Signal->PixelRate;
    Signal->HSyncFreq.Denominator = Signal->TotalSize.cx;
    Signal->VSyncFreq.Numerator = (UINT)Signal->PixelRate;
    Signal->VSyncFreq.Denominator = Signal->TotalSize.cx * Signal->TotalSize.cy;
    Signal->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

// BD-247: the identity and, on the full table, the three rotations (display_rotation.h). A rotation changes no
// timing and no plane setting: the source mode keeps the target's size, and its content arrives rotated.
static BOOLEAN ConnectorPathValid(const BC250_DEVICE* Device, const D3DKMDT_VIDPN_PRESENT_PATH* Path)
{
    return Path->VidPnSourceId == 0 && Path->VidPnTargetId == BC250_CHILD_UID &&
        Bc250RotationAngle((ULONG)Path->ContentTransformation.Rotation, Device->FullWddm) != BC250_ROTATION_REFUSED &&
        (!ScalingPinned(Path->ContentTransformation.Scaling) ||
         Path->ContentTransformation.Scaling == D3DKMDT_VPPS_IDENTITY) &&
        !Path->VisibleFromActiveTLOffset.cx && !Path->VisibleFromActiveTLOffset.cy &&
        !Path->VisibleFromActiveBROffset.cx && !Path->VisibleFromActiveBROffset.cy;
}

static BOOLEAN ConnectorSourceMatches(const BC250_DEVICE* Device,
    const D3DKMDT_VIDPN_SOURCE_MODE* Source, const BC250_TARGET_TIMING* Timing)
{
    return Source->Type == D3DKMDT_RMT_GRAPHICS &&
        Source->Format.Graphics.PrimSurfSize.cx == Timing->HActive &&
        Source->Format.Graphics.PrimSurfSize.cy == Timing->VActive &&
        Source->Format.Graphics.VisibleRegionSize.cx == Timing->HActive &&
        Source->Format.Graphics.VisibleRegionSize.cy == Timing->VActive &&
        Bc250SourceModeAdmitted(Device->ComposedSourceModes, Source->Format.Graphics.PixelFormat) &&
        Source->Format.Graphics.Stride == Bc250SourceModeStride(Source->Format.Graphics.PixelFormat,
            DcnPrimaryPitch(Timing->HActive));
}

static BOOLEAN ConnectorSignalMatches(const D3DKMDT_VIDEO_SIGNAL_INFO* Signal,
    const BC250_TARGET_TIMING* Timing)
{
    D3DKMDT_VIDEO_SIGNAL_INFO actual;
    ConnectorSignal(Timing, &actual);
    return Signal->ActiveSize.cx == actual.ActiveSize.cx && Signal->ActiveSize.cy == actual.ActiveSize.cy &&
        Signal->TotalSize.cx == actual.TotalSize.cx && Signal->TotalSize.cy == actual.TotalSize.cy &&
        Signal->PixelRate == actual.PixelRate && Signal->ScanLineOrdering == actual.ScanLineOrdering &&
        Signal->HSyncFreq.Denominator && Signal->VSyncFreq.Denominator &&
        (ULONGLONG)Signal->HSyncFreq.Numerator * actual.HSyncFreq.Denominator ==
            (ULONGLONG)actual.HSyncFreq.Numerator * Signal->HSyncFreq.Denominator &&
        (ULONGLONG)Signal->VSyncFreq.Numerator * actual.VSyncFreq.Denominator ==
            (ULONGLONG)actual.VSyncFreq.Numerator * Signal->VSyncFreq.Denominator;
}

static const BC250_TARGET_TIMING* ConnectorFindTiming(const BC250_DEVICE* Device,
    const BC250_CONNECTOR_SNAPSHOT* Snapshot, const D3DKMDT_VIDPN_SOURCE_MODE* Source,
    const D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    ULONG i;
    if (!Snapshot || Snapshot->Sink.Kind == BC250_SINK_NONE ||
        Snapshot->Sink.TargetCount > BC250_EDID_MAX_MODES) return NULL;
    for (i = 0; i < Snapshot->Sink.TargetCount; ++i) {
        const BC250_TARGET_TIMING* t = &Snapshot->Sink.Targets[i];
        if ((!Source || ConnectorSourceMatches(Device, Source, t)) &&
            (!Signal || ConnectorSignalMatches(Signal, t))) return t;
    }
    return NULL;
}

static NTSTATUS ConnectorOfferSources(BC250_DEVICE* Device, const BC250_CONNECTOR_SNAPSHOT* Snapshot,
    const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN VidPnHandle,
    const D3DKMDT_VIDEO_SIGNAL_INFO* PinnedTarget, ULONGLONG Eligible)
{
    D3DKMDT_HVIDPNSOURCEMODESET handle = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
    ULONG formats[BC250_SOURCE_MODE_MAX], count, i, j, n, offered = 0;
    NTSTATUS status = VidPn->pfnCreateNewSourceModeSet(VidPnHandle, 0, &handle, &set);
    if (!NT_SUCCESS(status)) return status;
    count = Bc250SourceModeFormats(Device->ComposedSourceModes, formats, BC250_SOURCE_MODE_MAX);
    for (i = 0; i < Snapshot->Sink.TargetCount && NT_SUCCESS(status); ++i) {
        const BC250_TARGET_TIMING* t = &Snapshot->Sink.Targets[i];
        if (!(Eligible & (1ull << i))) continue;
        if (PinnedTarget && !ConnectorSignalMatches(PinnedTarget, t)) continue;
        // Refresh alternatives share one source size, not duplicate source modes.
        for (n = 0; n < i; ++n)
            if ((Eligible & (1ull << n)) && Snapshot->Sink.Targets[n].HActive == t->HActive && Snapshot->Sink.Targets[n].VActive == t->VActive &&
                (!PinnedTarget || ConnectorSignalMatches(PinnedTarget, &Snapshot->Sink.Targets[n]))) break;
        if (n < i) continue;
        for (j = 0; j < count && NT_SUCCESS(status); ++j) {
            D3DKMDT_VIDPN_SOURCE_MODE* mode = NULL;
            status = set->pfnCreateNewModeInfo(handle, &mode);
            if (!NT_SUCCESS(status)) break;
            mode->Type = D3DKMDT_RMT_GRAPHICS;
            mode->Format.Graphics.PrimSurfSize.cx = t->HActive;
            mode->Format.Graphics.PrimSurfSize.cy = t->VActive;
            mode->Format.Graphics.VisibleRegionSize = mode->Format.Graphics.PrimSurfSize;
            mode->Format.Graphics.Stride = Bc250SourceModeStride(formats[j], DcnPrimaryPitch(t->HActive));
            mode->Format.Graphics.PixelFormat = (D3DDDIFORMAT)formats[j];
            mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
            mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
            status = set->pfnAddMode(handle, mode);
            if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(handle, mode);
            else ++offered;
        }
    }
    if (NT_SUCCESS(status) && !offered) status = STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignSourceModeSet(VidPnHandle, 0, handle);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseSourceModeSet(VidPnHandle, handle);
    return status;
}

static NTSTATUS ConnectorOfferTargets(BC250_DEVICE* Device, const BC250_CONNECTOR_SNAPSHOT* Snapshot,
    const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN VidPnHandle,
    const D3DKMDT_VIDPN_SOURCE_MODE* PinnedSource, ULONGLONG Eligible)
{
    D3DKMDT_HVIDPNTARGETMODESET handle = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE* set = NULL;
    ULONG i, offered = 0;
    NTSTATUS status = VidPn->pfnCreateNewTargetModeSet(VidPnHandle, BC250_CHILD_UID, &handle, &set);
    if (!NT_SUCCESS(status)) return status;
    for (i = 0; i < Snapshot->Sink.TargetCount && NT_SUCCESS(status); ++i) {
        D3DKMDT_VIDPN_TARGET_MODE* mode = NULL;
        if (!(Eligible & (1ull << i))) continue;
        if (PinnedSource && !ConnectorSourceMatches(Device, PinnedSource, &Snapshot->Sink.Targets[i])) continue;
        status = set->pfnCreateNewModeInfo(handle, &mode);
        if (!NT_SUCCESS(status)) break;
        ConnectorSignal(&Snapshot->Sink.Targets[i], &mode->VideoSignalInfo);
        mode->WireFormatAndPreference.Value = 0;
        mode->WireFormatAndPreference.Preference = i ? D3DKMDT_MP_NOTPREFERRED : D3DKMDT_MP_PREFERRED;
        mode->WireFormatAndPreference.Rgb = D3DKMDT_BITS_PER_COMPONENT_08;
        status = set->pfnAddMode(handle, mode);
        // A second timing with the same signal is the same mode to dxgkrnl (BD-189). edid.c keeps only the first,
        // and a duplicate that still arrives is skipped: failing the set would leave no VidPN at all.
        if (status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) {
            set->pfnReleaseModeInfo(handle, mode);
            status = STATUS_SUCCESS;
        }
        else if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(handle, mode);
        else ++offered;
    }
    if (NT_SUCCESS(status) && !offered) status = STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET_MODE;
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignTargetModeSet(VidPnHandle, BC250_CHILD_UID, handle);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseTargetModeSet(VidPnHandle, handle);
    return status;
}

static NTSTATUS ConnectorPivotModes(BC250_DEVICE* Device, const BC250_CONNECTOR_SNAPSHOT* Snapshot,
    const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN Handle, BOOLEAN SourcePivot, ULONGLONG* Eligible)
{
    ULONG i;
    NTSTATUS status;
    *Eligible = 0;
    if (SourcePivot) {
        D3DKMDT_HVIDPNSOURCEMODESET handle = NULL;
        const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
        const D3DKMDT_VIDPN_SOURCE_MODE* mode = NULL;
        status = VidPn->pfnAcquireSourceModeSet(Handle, 0, &handle, &set);
        if (!NT_SUCCESS(status)) return status;
        status = set->pfnAcquireFirstModeInfo(handle, &mode);
        while (NT_SUCCESS(status) && mode) {
            const D3DKMDT_VIDPN_SOURCE_MODE* next = NULL;
            for (i = 0; i < Snapshot->Sink.TargetCount; ++i)
                if (ConnectorSourceMatches(Device, mode, &Snapshot->Sink.Targets[i])) *Eligible |= 1ull << i;
            status = set->pfnAcquireNextModeInfo(handle, mode, &next);
            set->pfnReleaseModeInfo(handle, mode);
            mode = next;
        }
        VidPn->pfnReleaseSourceModeSet(Handle, handle);
    } else {
        D3DKMDT_HVIDPNTARGETMODESET handle = NULL;
        const DXGK_VIDPNTARGETMODESET_INTERFACE* set = NULL;
        const D3DKMDT_VIDPN_TARGET_MODE* mode = NULL;
        status = VidPn->pfnAcquireTargetModeSet(Handle, BC250_CHILD_UID, &handle, &set);
        if (!NT_SUCCESS(status)) return status;
        status = set->pfnAcquireFirstModeInfo(handle, &mode);
        while (NT_SUCCESS(status) && mode) {
            const D3DKMDT_VIDPN_TARGET_MODE* next = NULL;
            for (i = 0; i < Snapshot->Sink.TargetCount; ++i)
                if (ConnectorSignalMatches(&mode->VideoSignalInfo, &Snapshot->Sink.Targets[i])) *Eligible |= 1ull << i;
            status = set->pfnAcquireNextModeInfo(handle, mode, &next);
            set->pfnReleaseModeInfo(handle, mode);
            mode = next;
        }
        VidPn->pfnReleaseTargetModeSet(Handle, handle);
    }
    if (status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET || status == STATUS_GRAPHICS_DATASET_IS_EMPTY)
        status = STATUS_SUCCESS;
    return status;
}

static NTSTATUS ConnectorVidPn(BC250_DEVICE* Device, D3DKMDT_HVIDPN Handle,
    const DXGKARG_ENUMVIDPNCOFUNCMODALITY* Enum, BOOLEAN Commit, BOOLEAN* Supported)
{
    BC250_CONNECTOR_SNAPSHOT* snapshot = ConnectorSnapshotAcquire(Device);
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topologyHandle = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE source;
    D3DKMDT_VIDEO_SIGNAL_INFO target;
    const BC250_TARGET_TIMING* timing;
    SIZE_T count = 0;
    BOOLEAN sourcePinned = FALSE, targetPinned = FALSE;
    ULONGLONG eligible = ~0ull;
    NTSTATUS status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    *Supported = FALSE;
    if (!snapshot || snapshot->Sink.TargetCount > BC250_EDID_MAX_MODES) goto done;
    status = Device->Dxgk.DxgkCbQueryVidPnInterface(Handle, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) goto done;
    status = vidpn->pfnGetTopology(Handle, &topologyHandle, &topology);
    if (!NT_SUCCESS(status)) goto done;
    status = topology->pfnGetNumPaths(topologyHandle, &count);
    if (!NT_SUCCESS(status)) goto done;
    if (count == 0) {
        if (Commit) status = ConnectorLinkCommit(Device, snapshot, NULL, 0, 0, D3DKMDT_VPPS_IDENTITY);
        *Supported = NT_SUCCESS(status);
        goto done;
    }
    if (count != 1 || snapshot->Sink.Kind == BC250_SINK_NONE) goto unsupported;
    status = topology->pfnAcquireFirstPathInfo(topologyHandle, &path);
    if (!NT_SUCCESS(status)) goto done;
    if (!path || !ConnectorPathValid(Device, path)) goto unsupported;
    status = SourceModeIsPinned(vidpn, Handle, 0, &sourcePinned, &source);
    if (NT_SUCCESS(status)) status = TargetModeIsPinned(vidpn, Handle, BC250_CHILD_UID, &targetPinned, &target);
    if (!NT_SUCCESS(status)) goto done;
    timing = ConnectorFindTiming(Device, snapshot, sourcePinned ? &source : NULL, targetPinned ? &target : NULL);
    if (!timing) goto unsupported;
    if (Commit) {
        if (!sourcePinned) { status = STATUS_SUCCESS; goto done; }
        if (!targetPinned) goto unsupported;
        status = ConnectorLinkCommit(Device, snapshot, timing, source.Format.Graphics.PrimSurfSize.cx,
            source.Format.Graphics.PrimSurfSize.cy, D3DKMDT_VPPS_IDENTITY);
        if (!NT_SUCCESS(status)) goto done;
        Device->CommittedSourceFormat = (ULONG)source.Format.Graphics.PixelFormat;
        Device->CommittedSourceWidth = source.Format.Graphics.PrimSurfSize.cx;
        Device->CommittedSourceHeight = source.Format.Graphics.PrimSurfSize.cy;
        Device->ModeActive = TRUE;
        DisplayRecordRotation(Device, Bc250RotationAngle((ULONG)path->ContentTransformation.Rotation, Device->FullWddm),
            "connector commit");
    } else if (Enum) {
        D3DKMDT_VIDPN_PRESENT_PATH update = *path;
        BOOLEAN changed = FALSE;
        if ((Enum->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && Enum->EnumPivot.VidPnSourceId != 0) ||
            (Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && Enum->EnumPivot.VidPnTargetId != BC250_CHILD_UID))
            goto unsupported;
        if (Enum->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && !sourcePinned)
            status = ConnectorPivotModes(Device, snapshot, vidpn, Handle, TRUE, &eligible);
        else if (Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && !targetPinned)
            status = ConnectorPivotModes(Device, snapshot, vidpn, Handle, FALSE, &eligible);
        if (!NT_SUCCESS(status)) goto done;
        if (!eligible) goto unsupported;
        if (!sourcePinned && !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && Enum->EnumPivot.VidPnSourceId == 0))
            status = ConnectorOfferSources(Device, snapshot, vidpn, Handle, targetPinned ? &target : NULL, eligible);
        if (NT_SUCCESS(status) && !targetPinned &&
            !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && Enum->EnumPivot.VidPnTargetId == BC250_CHILD_UID))
            status = ConnectorOfferTargets(Device, snapshot, vidpn, Handle, sourcePinned ? &source : NULL, eligible);
        if (!NT_SUCCESS(status)) goto done;
        if (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_SCALING) {
            RtlZeroMemory(&update.ContentTransformation.ScalingSupport, sizeof(update.ContentTransformation.ScalingSupport));
            update.ContentTransformation.ScalingSupport.Identity = 1;
            changed = TRUE;
        }
        if (path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_ROTATION)
            changed = TRUE;
        // BD-247: dxgkrnl reads the rotation support on every update, so every update states all of it.
        if (changed) {
            BC250_ROTATION_SUPPORT_FILL(update.ContentTransformation.RotationSupport, Device->FullWddm);
            status = topology->pfnUpdatePathSupportInfo(topologyHandle, &update);
        }
    }
    *Supported = NT_SUCCESS(status);
    goto done;
unsupported:
    status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
done:
    if (path) topology->pfnReleasePathInfo(topologyHandle, path);
    if (snapshot && !ConnectorGenerationCurrent(Device, snapshot->Generation)) {
        *Supported = FALSE;
        status = STATUS_DEVICE_BUSY;
        if (Commit) Device->ModeActive = FALSE;
    }
    ConnectorSnapshotRelease(snapshot);
    return status;
}

#include "display_inherited_vidpn.h"

NTSTATUS Bc250IsSupportedVidPn(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_ISSUPPORTEDVIDPN* IsSupportedVidPn)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    SIZE_T paths = 0;
    NTSTATUS status;

    IsSupportedVidPn->IsVidPnSupported = FALSE;
    if (device->Modeset.InheritedFixed && !ConnectorIsEnabled(device)) {
        status = DisplayInheritedVidPn(device, IsSupportedVidPn->hDesiredVidPn, NULL, FALSE,
            &IsSupportedVidPn->IsVidPnSupported);
        return status == STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED ? STATUS_SUCCESS : status;
    }
    if (IsSupportedVidPn->hDesiredVidPn == NULL)        // the empty VidPN is always supported
    {
        IsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    if (ConnectorIsEnabled(device)) {
        status = ConnectorVidPn(device, IsSupportedVidPn->hDesiredVidPn, NULL, FALSE,
            &IsSupportedVidPn->IsVidPnSupported);
        return status == STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED ? STATUS_SUCCESS : status;
    }
    status = device->Dxgk.DxgkCbQueryVidPnInterface(IsSupportedVidPn->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(IsSupportedVidPn->hDesiredVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;
    status = topology->pfnGetNumPaths(hTopology, &paths);
    if (!NT_SUCCESS(status)) return status;

    // One source, one target: zero or one path. Modes are constrained in EnumVidPnCofuncModality; a pinned
    // source mode must also have a format of display_modes.h, the set CommitVidPn accepts, and a size of this
    // start's mode list (modeset.c) that the path's pinned scaling, if any, admits.
    IsSupportedVidPn->IsVidPnSupported = (paths <= 1);
    if (paths == 1)
    {
        D3DKMDT_VIDPN_SOURCE_MODE mode;
        BOOLEAN pinned;
        // A VidPN whose source mode set cannot be read is judged by its topology alone, as before 0.7.201.
        if (NT_SUCCESS(SourceModeIsPinned(vidpn, IsSupportedVidPn->hDesiredVidPn, 0, &pinned, &mode)) &&
            pinned && mode.Type == D3DKMDT_RMT_GRAPHICS)
        {
            const ULONG width = (ULONG)mode.Format.Graphics.PrimSurfSize.cx;
            const ULONG height = (ULONG)mode.Format.Graphics.PrimSurfSize.cy;
            const ULONG support = ModesetScalingSupport(device, width, height);
            const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
            if (!Bc250SourceModeAdmitted(device->ComposedSourceModes, (unsigned long)mode.Format.Graphics.PixelFormat) ||
                support == 0)
                IsSupportedVidPn->IsVidPnSupported = FALSE;
            else if (NT_SUCCESS(topology->pfnAcquireFirstPathInfo(hTopology, &path)) && path != NULL)
            {
                const D3DKMDT_VIDPN_PRESENT_PATH_SCALING scaling = path->ContentTransformation.Scaling;
                const BOOLEAN native = (width == device->Post.Width && height == device->Post.Height);
                // At the native size every scaling dxgkrnl can pin is the 1:1 shape; custom is never offered.
                if (ScalingPinned(scaling) && (!native || device->Modeset.InheritedFixed) &&
                    (ScalingFromPath(scaling) == BC250_SCALING_COUNT || !(support & BC250_SCALING_BIT(ScalingFromPath(scaling)))))
                    IsSupportedVidPn->IsVidPnSupported = FALSE;
                if (Bc250RotationAngle((ULONG)path->ContentTransformation.Rotation, device->FullWddm) ==
                    BC250_ROTATION_REFUSED)
                    IsSupportedVidPn->IsVidPnSupported = FALSE;     // BD-247: an offset, or display-only
                if (device->Modeset.InheritedFixed) {
                    BOOLEAN targetPinned;
                    D3DKMDT_VIDEO_SIGNAL_INFO target;
                    NTSTATUS targetStatus = TargetModeIsPinned(vidpn,IsSupportedVidPn->hDesiredVidPn,
                        path->VidPnTargetId,&targetPinned,&target);
                    if (!NT_SUCCESS(targetStatus) || (targetPinned &&
                        !DisplayInheritedModeMatches(device,&mode,path,&target)))
                        IsSupportedVidPn->IsVidPnSupported = FALSE;
                }
                topology->pfnReleasePathInfo(hTopology, path);
            }
        }
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

    if (ConnectorIsEnabled(device)) {
        BOOLEAN supported;
        return ConnectorVidPn(device, Enum->hConstrainingVidPn, Enum, FALSE, &supported);
    }
    if (device->Modeset.InheritedFixed) {
        BOOLEAN supported;
        return DisplayInheritedVidPn(device, Enum->hConstrainingVidPn, Enum, FALSE, &supported);
    }
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
            step = OfferSourceMode(device, vidpn, Enum->hConstrainingVidPn, path->VidPnSourceId,
                                   path->ContentTransformation.Scaling);

        // Target mode set: the same.
        if (NT_SUCCESS(step)) step = TargetModeIsPinned(vidpn, Enum->hConstrainingVidPn, path->VidPnTargetId, &pinned, NULL);
        if (NT_SUCCESS(step) && !pinned &&
            !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && Enum->EnumPivot.VidPnTargetId == path->VidPnTargetId))
            step = OfferTargetMode(device, vidpn, Enum->hConstrainingVidPn, path->VidPnTargetId);

        // Path transformations, reported where nothing is pinned yet. Scaling (modeset.c): what the pinned source
        // mode's size gets, or with no source mode pinned what the native size gets (identity and every scaling of
        // this start's level). Never all zero: dxgkrnl needs at least one, and identity is always the native shape.
        if (NT_SUCCESS(step))
        {
            if (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_SCALING)
            {
                D3DKMDT_VIDPN_SOURCE_MODE source;
                ULONG support = 0;
                RtlZeroMemory(&update.ContentTransformation.ScalingSupport, sizeof(update.ContentTransformation.ScalingSupport));
                if (NT_SUCCESS(SourceModeIsPinned(vidpn, Enum->hConstrainingVidPn, path->VidPnSourceId, &pinned, &source)) &&
                    pinned && source.Type == D3DKMDT_RMT_GRAPHICS)
                    support = ModesetScalingSupport(device, (ULONG)source.Format.Graphics.PrimSurfSize.cx,
                                                    (ULONG)source.Format.Graphics.PrimSurfSize.cy);
                else
                    support = ModesetScalingSupport(device, device->Post.Width, device->Post.Height);
                if (support == 0 && !device->Modeset.InheritedFixed) support = BC250_SCALING_BIT(BC250_SCALING_IDENTITY);
                update.ContentTransformation.ScalingSupport.Identity = (support & BC250_SCALING_BIT(BC250_SCALING_IDENTITY)) ? 1 : 0;
                update.ContentTransformation.ScalingSupport.Centered = (support & BC250_SCALING_BIT(BC250_SCALING_CENTERED)) ? 1 : 0;
                update.ContentTransformation.ScalingSupport.Stretched = (support & BC250_SCALING_BIT(BC250_SCALING_STRETCHED)) ? 1 : 0;
                update.ContentTransformation.ScalingSupport.AspectRatioCenteredMax =
                    (support & BC250_SCALING_BIT(BC250_SCALING_ASPECT)) ? 1 : 0;
                update.ContentTransformation.ScalingSupport.Custom =
                    (support & BC250_SCALING_BIT(BC250_SCALING_CUSTOM)) ? 1 : 0;
                changed = TRUE;
            }
            if (path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_ROTATION)
            {
                RtlZeroMemory(&update.ContentTransformation.RotationSupport, sizeof(update.ContentTransformation.RotationSupport));
                // A WDDM 1.3+ driver has "path independent rotation" and must say which offsets it supports: on a
                // test-signed system dxgkrnl bugchecks (0x113, subtype 0x1C) when the primary path's support lacks
                // Offset0 or carries any other offset (E16 run 007, read in DMMVIDPNPRESENTPATH::SetRotationSupport).
                // For a display-only driver dxgkrnl sets the bit by itself, so that path stays as it was.
                // BD-247: the full table also offers the three rotations (display_rotation.h).
                BC250_ROTATION_SUPPORT_FILL(update.ContentTransformation.RotationSupport, device->FullWddm);
                changed = TRUE;
            }
            // dxgkrnl judges RotationSupport on EVERY pfnUpdatePathSupportInfo call, also when only the scaling
            // branch above asked for it, and the copy taken from the path may still be all zero there.
            if (changed && device->FullWddm)
                BC250_ROTATION_SUPPORT_FILL(update.ContentTransformation.RotationSupport, TRUE);
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
    // The first 32 calls, every power of two, and a request that changes the state within the first 1024 calls.
    // BD-247: after a rotated commit dxgkrnl repeated FALSE past call 32, and a later TRUE was not in the log.
    const BOOLEAN logged=call<=32 || (call&(call-1))==0 ||
        (call<=1024 && (Visibility->Visible ? TRUE : FALSE)!=device->SourceVisible);

    // Normal PASSIVE_LEVEL visibility DDI only. The shared quiet restore helper
    // must remain usable at bugcheck without a logger, allocation or lock.
    if (logged) GuardLog("display visibility: call %ld source 0x%08X visible %u previous %u hardware %u blanked %u",
        call,Visibility->VidPnSourceId,Visibility->Visible,device->SourceVisible,device->VidPnFlipEnabled,device->DcnBlanked);
    if (Visibility->VidPnSourceId != 0) status=STATUS_INVALID_PARAMETER;
    else if (device->DcnWriteEnabled || device->VidPnFlipEnabled ||
             InterlockedCompareExchange(&device->StartVisibilityPending,0,0)) {
        // BD-179: a confirmed OS TRUE is the only release of the startup hold, so the hardware request is
        // serialized against the HPD worker's restore, which holds only Connector.Operation. The order is
        // the one CommitVidPn uses: StartHealth, then Connector.Operation. The section covers the hardware
        // request alone: the BD-195 diagnostic below calls ConnectorLinkNeedsPowerOn, whose contract is
        // Connector.Operation not held (connector.h), and the publication below needs no hardware.
        ConnectorOperationAcquire(device);
        status=DcnSetSourceVisibility(device,Visibility->Visible);
        ConnectorOperationRelease(device);
    }
    else if (!Visibility->Visible && device->Framebuffer != NULL)
        RtlZeroMemory(device->Framebuffer, device->FramebufferLength); // legacy no-MMIO display-only fallback
    if (NT_SUCCESS(status))
    {
        device->SourceVisible = Visibility->Visible;
        // Visibility does not suppress requested VSync signals. A visible source
        // can queue a flip immediately, so ensure its report source is armed.
        WddmSourceVisibility(device, Visibility->Visible);
    }
    DisplayRetainVisibility(device,Visibility,status,begin);
    if (logged) GuardLog("display visibility: call %ld status 0x%08X current %u blanked %u",
        call,status,device->SourceVisible,device->DcnBlanked);
    // BD-195: visibility alone never programs a link. A visible source over a connected sink whose link is off means
    // that no commit has applied a mode since the plug. Logged past the 32-call bound, for the first 16 such calls.
    if (Visibility->Visible && ConnectorIsEnabled(device)) {
        BOOLEAN active = FALSE, connected = FALSE;
        if (ConnectorLinkNeedsPowerOn(device,&active,&connected) && InterlockedIncrement(&device->VisibilityLinkOff)<=16)
            GuardLog("display visibility: call %ld visible 1 with the link off and a sink connected, no commit applied",call);
    }
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
    if (Commit->Flags.PathPowerTransition) {
        // A power-on transition must program the kept path (d3dkmddi.md:31540). After a physical detach the link is
        // off, so with a connected sink the connector commit below applies the path's mode (BD-195). Power-off, an
        // active link, a suspended connector and the inherited route keep the power-only answer.
        BOOLEAN active = FALSE, connected = FALSE;
        const BOOLEAN connector = ConnectorIsEnabled(device);
        const BOOLEAN needs = connector && ConnectorLinkNeedsPowerOn(device, &active, &connected);
        if (Commit->Flags.PathPoweredOff || !needs) {
            if (InterlockedIncrement(&device->CommitPowerOnly) <= 16)
                GuardLog("link: CommitVidPn power-transition only powered-off %u connector %u link-active %u connected %u",
                    Commit->Flags.PathPoweredOff, connector, active, connected);
            return STATUS_SUCCESS;                                      // no mode change, power only
        }
    }
    if (device->Modeset.InheritedFixed && !ConnectorIsEnabled(device)) {
        BOOLEAN supported;
        return DisplayInheritedVidPn(device, Commit->hFunctionalVidPn, NULL, TRUE, &supported);
    }

    if (ConnectorIsEnabled(device)) {
        BOOLEAN supported;
        GuardLog("link: CommitVidPn affected-source %lu power-transition %u powered-off %u", (ULONG)Commit->AffectedVidPnSourceId,
            Commit->Flags.PathPowerTransition,Commit->Flags.PathPoweredOff);
        // The connector owns the whole transaction and preserves the old mode
        // on admission or recovered mutation failure.
        status = ConnectorVidPn(device, Commit->hFunctionalVidPn, NULL, TRUE, &supported);
        // A power-on whose kept path the new sink does not offer: the link stays off, and the transition succeeds.
        // Windows commits a full mode for the new sink after Indicate(TRUE).
        if (Commit->Flags.PathPowerTransition && status == STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED)
            status = STATUS_SUCCESS;
        return status;
    }

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
    if (device->Modeset.InheritedFixed && paths != 1) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;

    status = SourceModeIsPinned(vidpn, Commit->hFunctionalVidPn, 0, &pinned, &mode);
    if (!NT_SUCCESS(status)) return status;
    if (!pinned) return STATUS_SUCCESS;     // a path without a pinned source mode: nothing to show, not an error
                                            // (STATUS_GRAPHICS_MODE_NOT_PINNED has success severity anyway)

    // The timing on the wire never changes. The source mode is one of this start's sizes (modeset.c: the native
    // one, or a smaller one that pipe 0's scaler puts on the native timing), in a format that display_modes.h
    // offers. A composed format leaves the plane as it is (8-bit, the firmware's): the UMDs compose such buffers,
    // and SetVidPnSourceAddress refuses them. Each change of the committed format or size is logged once.
    if (mode.Type != D3DKMDT_RMT_GRAPHICS ||
        ModesetScalingSupport(device, (ULONG)mode.Format.Graphics.PrimSurfSize.cx,
                              (ULONG)mode.Format.Graphics.PrimSurfSize.cy) == 0 ||
        !Bc250SourceModeAdmitted(device->ComposedSourceModes, (unsigned long)mode.Format.Graphics.PixelFormat))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    {
        const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
        ULONG scaling = BC250_SCALING_COUNT;            // no preference: modeset.c takes the default
        ULONG angle = 0;                                // BD-247: no path pins nothing
        status = topology->pfnAcquireFirstPathInfo(hTopology, &path);
        if (device->Modeset.InheritedFixed && (!NT_SUCCESS(status) || path == NULL))
            return NT_SUCCESS(status) ? STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED : status;
        if (NT_SUCCESS(status) && path != NULL)
        {
            scaling = ScalingFromPath(path->ContentTransformation.Scaling);
            angle = Bc250RotationAngle((ULONG)path->ContentTransformation.Rotation, device->FullWddm);
            if (device->Modeset.InheritedFixed) {
                D3DKMDT_VIDEO_SIGNAL_INFO target;
                BOOLEAN targetPinned = FALSE;
                status = TargetModeIsPinned(vidpn,Commit->hFunctionalVidPn,path->VidPnTargetId,&targetPinned,&target);
                if (NT_SUCCESS(status) && (!targetPinned || !DisplayInheritedModeMatches(device,&mode,path,&target)))
                    status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
            }
            if (NT_SUCCESS(status) && angle == BC250_ROTATION_REFUSED) status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
            topology->pfnReleasePathInfo(hTopology, path);
            if (!NT_SUCCESS(status)) return status;
        }
        status = ModesetCommit(device, (ULONG)mode.Format.Graphics.PrimSurfSize.cx,
                               (ULONG)mode.Format.Graphics.PrimSurfSize.cy, scaling);
        if (!NT_SUCCESS(status)) return status;
        DisplayRecordRotation(device, angle, "commit");
    }
    if (device->CommittedSourceFormat != (ULONG)mode.Format.Graphics.PixelFormat ||
        device->CommittedSourceWidth != (ULONG)mode.Format.Graphics.PrimSurfSize.cx ||
        device->CommittedSourceHeight != (ULONG)mode.Format.Graphics.PrimSurfSize.cy)
    {
        device->CommittedSourceFormat = (ULONG)mode.Format.Graphics.PixelFormat;
        device->CommittedSourceWidth = (ULONG)mode.Format.Graphics.PrimSurfSize.cx;
        device->CommittedSourceHeight = (ULONG)mode.Format.Graphics.PrimSurfSize.cy;
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
    BOOLEAN first=!device->CommitSeen;
    StartHealthEnter(device);
    status=CommitVidPnCore(hAdapter,Commit);
    // Only one inherited mode is accepted. Redundant commits of it preserve
    // the epoch; detach, failed commit and path power-off close presentation.
    StartHealthDisplayLocked(device,device->SourceVisible,
        (NT_SUCCESS(status) || ConnectorIsEnabled(device)) && device->ModeActive && !Commit->Flags.PathPoweredOff);
    StartHealthLeave(device);
    if (first && device->Modeset.InheritedFixed)
        GuardLog("display: first inherited commit status %08lX source %lux%lu pitch %u support %lX active %u power %u/%u",
            (ULONG)status,device->CommittedSourceWidth,device->CommittedSourceHeight,device->Post.Pitch,
            device->Modeset.InheritedSupport,(UINT)device->ModeActive,
            Commit->Flags.PathPowerTransition,Commit->Flags.PathPoweredOff);
    // Outside the start-health mutex: the audio endpoint follows the monitor's power (DPMS off is an unplug for
    // audio, as Linux's az_disable), and takes only its own leaf lock (dpaudio.c).
    if (NT_SUCCESS(status) && Commit->Flags.PathPowerTransition)
        DpAudioPathPower(device,(BOOLEAN)!Commit->Flags.PathPoweredOff);
    return status;
}

NTSTATUS Bc250UpdateActiveVidPnPresentPath(_In_ const HANDLE hAdapter, _In_ const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* const Update)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const D3DKMDT_VIDPN_PRESENT_PATH_SCALING scaling = Update->VidPnPresentPathInfo.ContentTransformation.Scaling;
    ULONG width, height, support, angle;
    NTSTATUS result;

    if (ConnectorIsEnabled(device)) {
        BC250_CONNECTOR_SNAPSHOT* snapshot = ConnectorSnapshotAcquire(device);
        D3DKMDT_VIDEO_SIGNAL_INFO signal;
        const BC250_TARGET_TIMING* timing;
        NTSTATUS status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
        if (!snapshot) return STATUS_DEVICE_NOT_READY;
        if (ConnectorPathValid(device, &Update->VidPnPresentPathInfo) && device->ModeActive) {
            status = ConnectorLinkCurrentSignal(device, &signal);
            timing = NT_SUCCESS(status) ? ConnectorFindTiming(device, snapshot, NULL, &signal) : NULL;
            if (!timing || timing->HActive != device->CommittedSourceWidth ||
                timing->VActive != device->CommittedSourceHeight)
                status = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
            if (!ConnectorGenerationCurrent(device, snapshot->Generation)) status = STATUS_DEVICE_BUSY;
        }
        // Identity scaling only: updating the path cannot change the already selected
        // timing. A timing change goes through CommitVidPN with both modes pinned.
        // BD-247: the rotation can change here (smooth rotation); the timing and the plane stay as they are.
        if (NT_SUCCESS(status))
            DisplayRecordRotation(device, Bc250RotationAngle(
                (ULONG)Update->VidPnPresentPathInfo.ContentTransformation.Rotation, device->FullWddm), "connector update");
        ConnectorSnapshotRelease(snapshot);
        return status;
    }
    // The retained transform has no programmable crop or alternate route.
    // Check this even when scaling is unpinned and the call would be a no-op.
    if (device->Modeset.InheritedFixed &&
        (!device->InheritedSignalValid || Update->VidPnPresentPathInfo.VidPnSourceId != 0 ||
         Update->VidPnPresentPathInfo.VidPnTargetId != BC250_CHILD_UID ||
         Update->VidPnPresentPathInfo.VisibleFromActiveTLOffset.cx != 0 ||
         Update->VidPnPresentPathInfo.VisibleFromActiveTLOffset.cy != 0 ||
         Update->VidPnPresentPathInfo.VisibleFromActiveBROffset.cx != 0 ||
         Update->VidPnPresentPathInfo.VisibleFromActiveBROffset.cy != 0))
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    // BD-247: the identity or, on the full table, a rotation; it reprograms nothing (display_rotation.h).
    angle = Bc250RotationAngle((ULONG)Update->VidPnPresentPathInfo.ContentTransformation.Rotation, device->FullWddm);
    if (angle == BC250_ROTATION_REFUSED) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    // The scaling of the active path (the user's "GPU scaling" choice): the same source mode, a new placement.
    if (!ScalingPinned(scaling)) { DisplayRecordRotation(device, angle, "update"); return STATUS_SUCCESS; }
    width = DisplaySourceWidth(device);
    height = DisplaySourceHeight(device);
    support = ModesetScalingSupport(device, width, height);
    if (ScalingFromPath(scaling) == BC250_SCALING_COUNT || !(support & BC250_SCALING_BIT(ScalingFromPath(scaling))))
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    result = ModesetCommit(device, width, height, ScalingFromPath(scaling));
    if (NT_SUCCESS(result)) DisplayRecordRotation(device, angle, "update");
    return result;
}

NTSTATUS Bc250RecommendMonitorModes(_In_ const HANDLE hAdapter, _In_ const DXGKARG_RECOMMENDMONITORMODES* const Recommend)
{
    // Tell the OS that this monitor can do the mode it is showing right now. Without an EDID (EnableDisplayModes 0,
    // or the AUX read failed) the OS would otherwise assume a default monitor that may not list the firmware's
    // resolution; with one, the mode is in the EDID already and the add below answers "already in the mode set".
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE* mode = NULL;
    NTSTATUS status;

    if (ConnectorIsEnabled(device)) {
        BC250_CONNECTOR_SNAPSHOT* snapshot = ConnectorSnapshotAcquire(device);
        ULONG i;
        if (Recommend->VideoPresentTargetId != BC250_CHILD_UID) {
            ConnectorSnapshotRelease(snapshot);
            return STATUS_INVALID_PARAMETER;
        }
        if (!snapshot) return STATUS_DEVICE_NOT_READY;
        status = STATUS_SUCCESS;
        for (i = 0; i < snapshot->Sink.TargetCount && NT_SUCCESS(status); ++i) {
            status = Recommend->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Recommend->hMonitorSourceModeSet, &mode);
            if (!NT_SUCCESS(status)) break;
            ConnectorSignal(&snapshot->Sink.Targets[i], &mode->VideoSignalInfo);
            mode->ColorBasis = D3DKMDT_CB_SRGB;
            mode->ColorCoeffDynamicRanges.FirstChannel = 8;
            mode->ColorCoeffDynamicRanges.SecondChannel = 8;
            mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
            mode->ColorCoeffDynamicRanges.FourthChannel = 8;
            mode->Origin = D3DKMDT_MCO_DRIVER;
            mode->Preference = i ? D3DKMDT_MP_NOTPREFERRED : D3DKMDT_MP_PREFERRED;
            status = Recommend->pMonitorSourceModeSetInterface->pfnAddMode(Recommend->hMonitorSourceModeSet, mode);
            if (!NT_SUCCESS(status)) {
                Recommend->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Recommend->hMonitorSourceModeSet, mode);
                if (status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) status = STATUS_SUCCESS;
            }
        }
        if (!ConnectorGenerationCurrent(device, snapshot->Generation)) status = STATUS_DEVICE_BUSY;
        ConnectorSnapshotRelease(snapshot);
        return status;
    }
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
    const BC250_DEVICE* device = (const BC250_DEVICE*)hAdapter;
    // DriverScaling=0 means scaling in the display pipeline (including a
    // retained firmware scaler), not an absence of scaling support. A value
    // of 1 promises a software/graphics-engine scaling implementation.
    RtlZeroMemory(&Capability->VidPnHWCaps, sizeof(Capability->VidPnHWCaps));
    // BD-247: a rotated image is drawn by the graphics engine (the compositor renders it rotated), never by the
    // display pipeline, which scans out linear surfaces unrotated. The OS clears the bit on a path that is not
    // rotated (display/querying-vidpnhardware-capabilities.md).
    Capability->VidPnHWCaps.DriverRotation = device->FullWddm ? 1 : 0;
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
    const BOOLEAN runtime = InterlockedCompareExchange(&device->RuntimePostValid, 0, 0) != 0;
    const DXGK_DISPLAY_INFORMATION* post = runtime ? &device->RuntimePost : &device->Post;
    PVOID framebuffer = runtime ? device->RuntimeFramebuffer : device->Framebuffer;
    SIZE_T length = runtime ? device->RuntimeFramebufferLength : device->FramebufferLength;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Flags);
    device->SystemDisplayReady=FALSE;
    *Width=0;*Height=0;*ColorFormat=D3DDDIFMT_UNKNOWN;
    if (!runtime && InterlockedCompareExchange(&device->RuntimeDisplayChanged, 0, 0))
        return STATUS_DEVICE_NOT_READY;
    // A dynamic target must retain its current-size handoff surface. Boot's GOP
    // tuple is used only until a runtime timing has been committed.
    if (TargetId!=BC250_CHILD_UID && TargetId!=D3DDDI_ID_UNINITIALIZED) return STATUS_NOT_SUPPORTED;
    if (!framebuffer || !IsPostFormatSupported(post->ColorFormat) ||
        !post->Width || !post->Height ||
        (ULONGLONG)post->Width*4>post->Pitch ||
        (ULONGLONG)post->Pitch*post->Height>length)
        return STATUS_DEVICE_NOT_READY;
    // BD-179: the system display is a distinct owner. A bugcheck during StartDevice
    // must be able to show its validated handoff surface without an OS TRUE.
    InterlockedExchange(&device->StartVisibilityPending,0);
    // The serialized restore entry stays as on main: this callback is one of the OS's
    // serialized bugcheck/reset entries and must not take the normal writer protocol.
    status=DcnRestorePostDisplaySerialized(device);
    if (!NT_SUCCESS(status)) return status;
    if (!runtime) ModesetRestoreQuiet(device);
    // BD-247: the OS draws the bugcheck screen in the desktop's orientation, so a rotated path reports its logical
    // size and SystemDisplayWrite turns each block onto the unrotated firmware surface.
    Bc250RotationLogicalSize(Bc250RotationAngle((ULONG)device->Rotation, TRUE), post->Width, post->Height, Width, Height);
    *ColorFormat = post->ColorFormat;
    device->SystemDisplayReady=TRUE;
    return STATUS_SUCCESS;
}

void Bc250SystemDisplayWrite(_In_ const PVOID MiniportDeviceContext, _In_ const PVOID Source, _In_ const UINT SourceWidth,
                             _In_ const UINT SourceHeight, _In_ const UINT SourceStride, _In_ const UINT PositionX,
                             _In_ const UINT PositionY)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    const BOOLEAN runtime = InterlockedCompareExchange(&device->RuntimePostValid, 0, 0) != 0;
    const DXGK_DISPLAY_INFORMATION* post = runtime ? &device->RuntimePost : &device->Post;
    UCHAR* framebuffer = runtime ? device->RuntimeFramebuffer : device->Framebuffer;
    SIZE_T length = runtime ? device->RuntimeFramebufferLength : device->FramebufferLength;
    if (!runtime && InterlockedCompareExchange(&device->RuntimeDisplayChanged, 0, 0)) return;
    if (!device->SystemDisplayReady || !framebuffer || !Source ||
        (ULONGLONG)post->Width * 4 > post->Pitch || (ULONGLONG)post->Pitch * post->Height > length) return;
    // BD-247: the position and the block are logical (SystemDisplayEnable reported the logical size); the clip to
    // that size and to the source stride, and the turn onto the physical surface, are display_rotation.h's.
    Bc250RotationWriteBlock(Bc250RotationAngle((ULONG)device->Rotation, TRUE), framebuffer, (unsigned int)post->Pitch,
        (unsigned int)post->Width, (unsigned int)post->Height, (const unsigned char*)Source, SourceWidth, SourceHeight,
        SourceStride, PositionX, PositionY);
}
