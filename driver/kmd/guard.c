// Boot-loop guard, breadcrumbs and log (ADR 0006, points 3 and 4). This lab has no kernel debugger and a
// hang costs the owner a walk to the machine, so the driver leaves its own trail:
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
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "guard_keep_prune.h"
#include "hang_recovery.h"
#include <ntstrsafe.h>
#include <stdarg.h>

static UNICODE_STRING g_ParametersPath;
static WCHAR g_History[256];
static BC250_STAGE g_LastStage;
// BD-090: this image's start wrote and flushed one count, and no confirmation has cleared it since. Per image on
// purpose: a stop runs in the image of the start it ends (a restart or an update unloads the image in between).
// Start, confirmation (under StartHealth.Lifecycle, closed by StartHealthClose) and stop never overlap.
static BOOLEAN g_StartCounted;

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

static NTSTATUS OpenParameters(_Out_ HANDLE* Key)
{
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &g_ParametersPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(Key, KEY_READ | KEY_WRITE, &attributes, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
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

NTSTATUS GuardInit(_In_ PUNICODE_STRING RegistryPath)
{
    static const WCHAR suffix[] = L"\\Parameters";
    USHORT bytes = RegistryPath->Length + sizeof(suffix);

    // The ring first, so that everything DriverEntry does from here on is in it, including a failure below.
    KeInitializeSpinLock(&g_LogLock);
    g_LogStart = KeQueryInterruptTime();
    g_LogReady = TRUE;

    g_ParametersPath.Buffer = (PWCH)ExAllocatePool2(POOL_FLAG_PAGED, bytes, BC250_TAG);
    if (g_ParametersPath.Buffer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    g_ParametersPath.MaximumLength = bytes;
    g_ParametersPath.Length = 0;
    RtlCopyUnicodeString(&g_ParametersPath, RegistryPath);
    return RtlAppendUnicodeToString(&g_ParametersPath, suffix);
}

void GuardCleanup(void)
{
    if (g_ParametersPath.Buffer != NULL) ExFreePoolWithTag(g_ParametersPath.Buffer, BC250_TAG);
    RtlZeroMemory(&g_ParametersPath, sizeof(g_ParametersPath));
}

void GuardStage(BC250_STAGE Stage)
{
    HANDLE key;
    WCHAR item[16];
    UNICODE_STRING name;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return;
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

ULONG GuardReadSetting(_In_z_ PCWSTR Name, ULONG Default)
{
    HANDLE key;
    ULONG value;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !NT_SUCCESS(OpenParameters(&key))) return Default;
    status = ReadDword(key, Name, &value);
    ZwClose(key);
    return NT_SUCCESS(status) ? value : Default;
}

// A one-shot gate: the value 1 is read and put back to 0 on disk before anything it opens has run, so a bugcheck
// behind the gate cannot repeat itself at the next boot (E16 run 007 did, once). Any other value is left alone.
ULONG GuardConsumeSetting(_In_z_ PCWSTR Name, ULONG Default)
{
    HANDLE key;
    ULONG value;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !NT_SUCCESS(OpenParameters(&key))) return Default;
    status = ReadDword(key, Name, &value);
    if (NT_SUCCESS(status) && value == 1)
    {
        status=WriteDword(key,Name,0);
        if (!NT_SUCCESS(status)) {
            GuardLog("gate: one-shot closure write failed, refused (0x%08X)",status);
            ZwClose(key);
            return 0;
        }
        status=ZwFlushKey(key);
        if (!NT_SUCCESS(status)) {
            // Never run a one-shot experiment unless its reset is durable.
            // Default is for an absent setting, not permission after this failure.
            GuardLog("gate: one-shot closure flush failed, refused (0x%08X)",status);
            ZwClose(key);
            return 0;
        }
    }
    ZwClose(key);
    return NT_SUCCESS(status) ? value : Default;
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
    if (g_LogNext >= BC250_LOG_RING_LINES) g_LogLost++;      // this line overwrites one the tail still held
    entry = &g_Log[LogSlot(g_LogNext)];
    entry->Sequence = g_LogNext++;
    entry->Milliseconds = (ULONG)((now - g_LogStart) / 10000ull);
    RtlCopyMemory(entry->Text, line, sizeof(entry->Text));
    KeReleaseSpinLock(&g_LogLock, irql);
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
// Behind its own gate, Parameters\KeepLog (REG_DWORD, 0 by default, preserved across upgrades, read here at the
// stop): a synchronous file write has no timeout, and a stop during a shutdown or over a wedged volume is no
// place for one on a board without a BMC. An experiment that expects dxgkrnl to end the start opens it.
//
// ONE text file per episode under C:\BC250\kmdlog, where an episode is one device start or one device stop
// (GuardLogKeepEpisode, called by pnp.c). The file is named by the UTC time of the first keep of that episode, to
// the millisecond, and by the episode's label, in the format bc250kmd_cli log prints. The directory is created if
// it is missing. What became of the attempt is written to Parameters\KeepStatus (the NTSTATUS of the create, or of
// the last write), because a line in the ring saying that the ring was not kept would be read by nobody.
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
#define BC250_KEEP_DIRECTORY L"\\??\\C:\\BC250\\kmdlog"
#define BC250_KEEP_SCAN_BYTES 8192u                 // one directory page; about 90 names of ours
#define BC250_KEEP_DELETE_MAX 512u                  // deletes in one prune: a bound on the work a PnP start does

static WCHAR g_KeepPath[160];           // the open episode's file, or empty when the next keep opens one
static LONGLONG g_KeepBytes;            // bytes that file already holds; the offset the next write uses
static ULONG g_KeepFrom;                // the first ring sequence that is not in it yet
static ULONG g_KeepAppends;             // keeps of this episode, the first one included
static PCWSTR g_KeepLabel = L"load";    // what the episode is; before the first GuardLogKeepEpisode, the load

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
    WCHAR name[BC250_KEEP_NAME + RTL_NUMBER_OF(BC250_KEEP_DIRECTORY) + 2];
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
                    if (NT_SUCCESS(RtlStringCchPrintfW(name, RTL_NUMBER_OF(name), L"%ws\\%ws",
                                                       BC250_KEEP_DIRECTORY, leaf)))
                    {
                        RtlInitUnicodeString(&path, name);
                        InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                                   NULL, NULL);
                        if (NT_SUCCESS(ZwDeleteFile(&attributes))) deleted++; else failed++;
                    }
                    else failed++;
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

    // The directory first; FILE_OPEN_IF makes this a no-op when it is there, and a failure shows in the create
    // below. The handle stays open while a new file is made, because the prune enumerates through it.
    RtlInitUnicodeString(&path, BC250_KEEP_DIRECTORY);
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&directory, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &io, NULL,
                          FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) directory = NULL;

    if (g_KeepPath[0] == 0)
    {
        KeQuerySystemTime(&now);
        RtlTimeToTimeFields(&now, &t);
        status = RtlStringCchPrintfW(g_KeepPath, RTL_NUMBER_OF(g_KeepPath),
                                     L"%ws\\ring-%04d%02d%02d-%02d%02d%02d-%03d-%ws.log", BC250_KEEP_DIRECTORY,
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
    if (directory != NULL) ZwClose(directory);

    RtlInitUnicodeString(&path, g_KeepPath);
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&file, FILE_GENERIC_WRITE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          created ? FILE_OVERWRITE_IF : FILE_OPEN,
                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
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
    HANDLE key;
    NTSTATUS status;

    *Value = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return STATUS_INVALID_DEVICE_STATE;
    status = OpenParameters(&key);
    if (!NT_SUCCESS(status)) return status;
    status = ReadDword(key, Name, Value);
    ZwClose(key);
    return status;          // STATUS_OBJECT_NAME_NOT_FOUND when absent
}

// Written and flushed: success means it is on the disk.
NTSTATUS GuardStoreSetting(_In_z_ PCWSTR Name, ULONG Value)
{
    HANDLE key;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return STATUS_INVALID_DEVICE_STATE;
    status = OpenParameters(&key);
    if (!NT_SUCCESS(status)) return status;
    status = WriteDword(key, Name, Value);
    if (NT_SUCCESS(status)) status = ZwFlushKey(key);
    ZwClose(key);
    return status;
}

// Deleted and flushed; a value that was not there is success.
NTSTATUS GuardDeleteSetting(_In_z_ PCWSTR Name)
{
    HANDLE key;
    UNICODE_STRING name;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return STATUS_INVALID_DEVICE_STATE;
    status = OpenParameters(&key);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, Name);
    status = ZwDeleteValueKey(key, &name);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) status = STATUS_SUCCESS;
    if (NT_SUCCESS(status)) status = ZwFlushKey(key);
    ZwClose(key);
    return status;
}

// A volatile subkey of Parameters: the configuration manager drops it at every reboot, which is exactly the life
// of what it holds (this boot's firmware values, cumode.c). Never flushed; there is no disk copy to flush.
static NTSTATUS OpenVolatile(_In_z_ PCWSTR Subkey, BOOLEAN Create, _Out_ HANDLE* Key)
{
    HANDLE parent;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    *Key = NULL;
    status = OpenParameters(&parent);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, Subkey);
    InitializeObjectAttributes(&attributes, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, parent, NULL);
    status = Create ? ZwCreateKey(Key, KEY_READ | KEY_WRITE, &attributes, 0, NULL, REG_OPTION_VOLATILE, NULL)
                    : ZwOpenKey(Key, KEY_READ, &attributes);
    ZwClose(parent);
    return status;
}

NTSTATUS GuardVolatileQuery(_In_z_ PCWSTR Subkey, _In_z_ PCWSTR Name, _Out_ ULONG* Value)
{
    HANDLE key;
    NTSTATUS status;

    *Value = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return STATUS_INVALID_DEVICE_STATE;
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

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return STATUS_INVALID_DEVICE_STATE;
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
//     LastKills, LastMicros                     REG_DWORD  SQ_CMD writes issued, time spent in the kill loop
//     LastTime                                  REG_QWORD  KeQuerySystemTime of this write (UTC FILETIME)
//     LastVersion                               REG_DWORD  BC250_KMD_VERSION of the writer
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

void GuardRecordHangRecovery(ULONG Verdict, ULONG Seq, ULONG Fence, ULONG Kills, ULONG Micros)
{
    HANDLE parameters, key;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    LARGE_INTEGER now;
    ULONG counts = Bc250HangVerdictCounts(Verdict);
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_ParametersPath.Buffer == NULL) return;
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

