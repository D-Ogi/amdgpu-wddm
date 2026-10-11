/* SPDX-License-Identifier: MIT */
/* This harness includes the complete production hip_journal.c after removing
 * only its bc250kmd.h include. WDK scheduling/process primitives are mocked;
 * parsing, publication, admission, lifetime and ring transitions are production.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef LONG NTSTATUS;
#include <d3dkmthk.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef UCHAR KIRQL;
typedef SRWLOCK MOCK_SPIN_LOCK;
#define KSPIN_LOCK MOCK_SPIN_LOCK
typedef struct TEST_PROCESS { ULONG Id; LONG References; } TEST_PROCESS, *PEPROCESS;
typedef struct TEST_ESCAPE {
    HANDLE hDevice, hContext;
    D3DDDI_ESCAPEFLAGS Flags;
    void* pPrivateDriverData;
    ULONG PrivateDriverDataSize;
} DXGKARG_ESCAPE;
#define STATUS_SUCCESS ((NTSTATUS)0)
#undef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#undef STATUS_INTEGER_OVERFLOW
#define STATUS_INTEGER_OVERFLOW ((NTSTATUS)0xc0000095)
#define STATUS_PROCESS_IS_TERMINATING ((NTSTATUS)0xc000010a)
#define STATUS_CANCELLED ((NTSTATUS)0xc0000120)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xc0000035)
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif
#define PASSIVE_LEVEL 0
#define DISPATCH_LEVEL 2

static LONG checks, failures, logCount;
static __declspec(thread) KIRQL mockIrql;
static __declspec(thread) ULONG lockDepth;
static __declspec(thread) PEPROCESS currentProcess;
static TEST_PROCESS processA = {100, 0}, processB = {100, 0}; /* deliberate PID reuse */
static volatile LONG64 ticks;
static void (*copyInterleave)(void);
#define CHECK(c) do { InterlockedIncrement(&checks); if (!(c)) { InterlockedIncrement(&failures); printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void KeInitializeSpinLock(KSPIN_LOCK* Lock) { InitializeSRWLock(Lock); }
static void KeAcquireSpinLock(KSPIN_LOCK* Lock, KIRQL* Irql)
{ *Irql = mockIrql; AcquireSRWLockExclusive(Lock); mockIrql = DISPATCH_LEVEL; ++lockDepth; }
static void KeReleaseSpinLock(KSPIN_LOCK* Lock, KIRQL Irql)
{ --lockDepth; mockIrql = Irql; ReleaseSRWLockExclusive(Lock); }
static KIRQL KeGetCurrentIrql(void) { return mockIrql; }
static ULONGLONG KeQueryInterruptTime(void) { return (ULONGLONG)InterlockedIncrement64(&ticks); }
static PEPROCESS PsGetCurrentProcess(void) { return currentProcess; }
static HANDLE PsGetCurrentProcessId(void) { return ULongToHandle(currentProcess->Id); }
static void ObReferenceObject(PEPROCESS Process) { InterlockedIncrement(&Process->References); }
static void ObDereferenceObject(PEPROCESS Process) { InterlockedDecrement(&Process->References); }
static void InitializeListHead(LIST_ENTRY* Head) { Head->Flink = Head->Blink = Head; }
static void InsertTailList(LIST_ENTRY* Head, LIST_ENTRY* Entry)
{ Entry->Flink = Head; Entry->Blink = Head->Blink; Head->Blink->Flink = Entry; Head->Blink = Entry; }
static void RemoveEntryList(LIST_ENTRY* Entry) { Entry->Blink->Flink = Entry->Flink; Entry->Flink->Blink = Entry->Blink; }
static void GuardLog(const char* Format, ...) { (void)Format; InterlockedIncrement(&logCount); }
#define KeMemoryBarrier() MemoryBarrier()
#undef RtlZeroMemory
#undef RtlCopyMemory
#undef RtlCompareMemory
#define RtlCompareMemory MockCompareMemory
static void RtlZeroMemory(void* Pointer, SIZE_T Bytes)
{ if (Bytes > 1024) CHECK(lockDepth == 0 && mockIrql == PASSIVE_LEVEL); memset(Pointer, 0, Bytes); }
static void RtlCopyMemory(void* Destination, const void* Source, SIZE_T Bytes)
{
    if (Bytes > 96) {
        CHECK(lockDepth == 0 && mockIrql == PASSIVE_LEVEL);
        if (copyInterleave) { void (*callback)(void) = copyInterleave; copyInterleave = NULL; callback(); }
    }
    memcpy(Destination, Source, Bytes);
}
static SIZE_T RtlCompareMemory(const void* A, const void* B, SIZE_T Bytes)
{ return memcmp(A, B, Bytes) == 0 ? Bytes : 0; }
#include "../hip_journal.h"
#include "hip_journal_production.inc"

typedef struct TEST_BATCH { BC250_HIP_JOURNAL_UPLOAD Header; UCHAR Body[256]; } TEST_BATCH;
static PVOID adapter = (PVOID)(ULONG_PTR)0x1000, context = (PVOID)(ULONG_PTR)0x2000, device = (PVOID)(ULONG_PTR)0x3000;
static BC250_HIP_JOURNAL_OWNER owner;
static DXGKARG_ESCAPE EscapeFor(TEST_BATCH* Batch)
{
    DXGKARG_ESCAPE escape = {0};
    escape.hDevice = device; escape.hContext = context;
    escape.Flags.NoAdapterSynchronization = 1;
    escape.PrivateDriverDataSize = Batch->Header.TotalBytes;
    escape.pPrivateDriverData = Batch;
    return escape;
}
static TEST_BATCH MakeBatch(ULONGLONG Fence)
{
    TEST_BATCH batch = {0};
    BC250_HIP_DISPATCH_RECORD r = {0};
    BC250_HIP_POINTER_BINDING binding = {0};
    batch.Header.Magic = BC250_HIP_JOURNAL_MAGIC;
    batch.Header.Command = BC250_HIP_JOURNAL_COMMAND;
    batch.Header.Status = 0xffffffff;
    batch.Header.AbiVersion = 1;
    batch.Header.RecordCount = 1;
    batch.Header.IbVa = 0x4000010000ull;
    batch.Header.FenceVa = 0x4000030000ull;
    batch.Header.FenceValue = Fence;
    r.Bytes = 152; r.SymbolBytes = 5; r.KernargBytes = 8; r.BindingCount = 1;
    r.SymbolOffset = 96; r.KernargOffset = 104; r.BindingsOffset = 112;
    r.EntryVa = 0x4000060000ull; r.DescriptorVa = 0x4000050000ull; r.KernargVa = 0x4000080000ull;
    r.Grid[0] = r.Grid[1] = r.Grid[2] = r.Block[0] = r.Block[1] = r.Block[2] = 1;
    r.DispatchId = Fence;
    binding.Kind = BC250_HIP_BINDING_GLOBAL_BUFFER;
    binding.Flags = BC250_HIP_BINDING_INTERVAL_KNOWN;
    binding.Value = binding.Base = 0x40000a0000ull; binding.Bytes = 4096;
    memcpy(batch.Body, &r, sizeof(r));
    memcpy(batch.Body + r.SymbolOffset, "kern", 5);
    memcpy(batch.Body + r.KernargOffset, &binding.Value, 8);
    memcpy(batch.Body + r.BindingsOffset, &binding, sizeof(binding));
    batch.Header.TotalBytes = sizeof(batch.Header) + r.Bytes;
    return batch;
}
static void Reset(void)
{
    if (owner.Registered) HipJournalOwnerDestroy(&owner);
    CHECK(processA.References == 0 && processB.References == 0 && lockDepth == 0);
    memset(&owner, 0, sizeof(owner));
    memset(&g_HipDispatchJournal, 0, sizeof(g_HipDispatchJournal));
    g_HipInitialized = 0;
    currentProcess = &processA; mockIrql = PASSIVE_LEVEL;
    CHECK(HipJournalOwnerCreate(&owner, adapter, context, device) == STATUS_SUCCESS);
    CHECK(processA.References == 1);
}
static ULONGLONG Upload(ULONGLONG Fence)
{
    TEST_BATCH batch = MakeBatch(Fence);
    DXGKARG_ESCAPE escape = EscapeFor(&batch);
    CHECK(HipJournalEscape(adapter, &escape) == STATUS_SUCCESS);
    CHECK(batch.Header.UploadId && !batch.Header.Status && !batch.Header.NtStatus);
    return batch.Header.UploadId;
}
static NTSTATUS Cancel(ULONGLONG Id)
{
    TEST_BATCH batch = MakeBatch(1);
    DXGKARG_ESCAPE escape;
    batch.Header.TotalBytes = sizeof(batch.Header); batch.Header.RecordCount = 0;
    batch.Header.Operation = BC250_HIP_JOURNAL_CANCEL_OP; batch.Header.UploadId = Id;
    escape = EscapeFor(&batch);
    return HipJournalEscape(adapter, &escape);
}
static ULONGLONG Bind(ULONGLONG Fence, ULONG OsFence)
{
    ULONGLONG id = 0;
    CHECK(HipJournalBind(&owner, 0x4000010000ull, 0x4000030000ull, Fence, OsFence, 4096, &id) == STATUS_SUCCESS);
    CHECK(id != 0);
    return id;
}
static void TestShape(void)
{
    TEST_BATCH original = MakeBatch(1), bad;
    BC250_HIP_DISPATCH_RECORD* r;
    BC250_HIP_POINTER_BINDING* b;
    ULONG i;
    CHECK(sizeof(BC250_HIP_JOURNAL_UPLOAD) == 80 && sizeof(BC250_HIP_DISPATCH_RECORD) == 96);
    CHECK(sizeof(BC250_HIP_POINTER_BINDING) == 40 && offsetof(BC250_HIP_JOURNAL_UPLOAD, Operation) == 64);
    CHECK(offsetof(BC250_HIP_JOURNAL, Slots) == 80 && offsetof(BC250_HIP_JOURNAL_SLOT, Data) == 152);
    CHECK(offsetof(BC250_HIP_JOURNAL_SLOT, CancelRequested) == 144 &&
          offsetof(BC250_HIP_JOURNAL_SLOT, ReplayPending) == 145 && offsetof(BC250_HIP_JOURNAL_SLOT, Attempts) == 146);
    CHECK(sizeof(BC250_HIP_JOURNAL_SLOT) == 131224 && sizeof(BC250_HIP_JOURNAL) == 4199248);
    CHECK(HipPayloadValid((UCHAR*)&original, original.Header.TotalBytes));
    CHECK(!Bc250HipRecordValid(NULL, 0));
    for (i = 0; i < original.Header.TotalBytes; ++i) CHECK(!HipPayloadValid((UCHAR*)&original, i));
    for (i = 0; i < 15; ++i) {
        bad = original; r = (BC250_HIP_DISPATCH_RECORD*)bad.Body;
        b = (BC250_HIP_POINTER_BINDING*)(bad.Body + 112);
        switch (i) {
        case 0: r->Bytes = 0xffffffff; break;
        case 1: r->SymbolOffset = 0xffffffff; break;
        case 2: r->KernargBytes = 0xffffffff; break;
        case 3: r->BindingCount = 0xffffffff; break;
        case 4: r->Grid[2] = 0; break;
        case 5: r->DispatchId = 0; break;
        case 6: bad.Body[101] = 1; break;
        case 7: bad.Body[100] = 1; break;
        case 8: b->ArgumentOffset = 1; break;
        case 9: b->Value++; break;
        case 10: b->Base = MAXULONGLONG; break;
        case 11: b->Bytes = 0; break;
        case 12: b->Flags = 2; break;
        case 13: b->Kind = 0; break;
        case 14: r->Flags = 1; break;
        }
        CHECK(!HipPayloadValid((UCHAR*)&bad, bad.Header.TotalBytes));
    }
    bad = original; b = (BC250_HIP_POINTER_BINDING*)(bad.Body + 112);
    b->Flags = 0; b->Base = b->Bytes = 0;
    CHECK(HipPayloadValid((UCHAR*)&bad, bad.Header.TotalBytes)); /* unresolved non-NULL */
    b->Value = 0; memset(bad.Body + 104, 0, 8);
    CHECK(HipPayloadValid((UCHAR*)&bad, bad.Header.TotalBytes)); /* NULL */
}
static void TestOwnershipAndState(void)
{
    TEST_BATCH batch;
    DXGKARG_ESCAPE escape;
    ULONGLONG id, bound;
    ULONG i;
    Reset();
    for (i = 0; i < 7; ++i) {
        batch = MakeBatch(i + 1); escape = EscapeFor(&batch);
        switch (i) {
        case 0: currentProcess = &processB; break;
        case 1: escape.hContext = (PVOID)1; break;
        case 2: escape.hDevice = (PVOID)1; break;
        case 3: escape.Flags.Value = 0; break;
        case 4: escape.Flags.HardwareAccess = 1; break;
        case 5: batch.Header.Reserved[2] = 1; break;
        case 6: escape.PrivateDriverDataSize = BC250_HIP_JOURNAL_MAX_BYTES + 1; break;
        }
        CHECK(!NT_SUCCESS(HipJournalEscape(adapter, &escape)));
        CHECK(!owner.Enabled);
        currentProcess = &processA;
    }
    id = Upload(100);
    CHECK(owner.Enabled && HipFindId(id)->ProcessId == 100 && HipFindId(id)->ContextCookie == owner.Cookie);
    batch = MakeBatch(100); escape = EscapeFor(&batch);
    CHECK(HipJournalEscape(adapter, &escape) == STATUS_OBJECT_NAME_COLLISION);
    CHECK(HipJournalBind(&owner, 0x4000010000ull, 0x4000030008ull, 100, 7, 4096, &bound) == STATUS_INVALID_PARAMETER);
    CHECK(bound == 0 && HipFindId(id)->State == BC250_HIP_SLOT_UPLOADED);
    bound = Bind(100, 99); CHECK(id == bound && HipFindId(id)->OsFence == 99);
    CHECK(Cancel(id) == STATUS_DEVICE_BUSY);
    HipJournalSubmitted(id, 678, 5, 0x46dfc0000ull);
    CHECK(HipFindId(id)->State == BC250_HIP_SLOT_SUBMITTED && HipFindId(id)->Seq == 678);
    CHECK(Cancel(id) == STATUS_DEVICE_BUSY);
    HipJournalComplete(id);
    CHECK(HipFindId(id)->State == BC250_HIP_SLOT_COMPLETED && !(HipFindId(id)->Generation & 1));
    CHECK(Cancel(id) == STATUS_SUCCESS);
    id = Upload(101); CHECK(Cancel(id) == STATUS_SUCCESS);
    CHECK(HipFindId(id)->State == BC250_HIP_SLOT_CANCELLED);
    bound = Upload(101); CHECK(bound != id); /* retry of refused Submit */
    CHECK(Bind(101, 100) == bound);
    HipJournalFailed(bound, STATUS_INVALID_PARAMETER);
    CHECK(HipFindId(bound)->State == BC250_HIP_SLOT_FAILED);
    currentProcess = &processB;
    CHECK(Cancel(bound) == STATUS_ACCESS_DENIED);
    currentProcess = &processA;
    id = Upload(102); CHECK(Bind(102, 101) == id);
    HipJournalSubmitted(id, 679, 5, 0x46dfc0000ull);
    HipJournalOwnerDestroy(&owner);
    CHECK(HipFindId(id)->State == BC250_HIP_SLOT_SUBMITTED); /* never recycle live GPU work on destroy */
    CHECK(processA.References == 0);
    CHECK(HipJournalBind(&owner, 0, 0, 0, 0, 0, &bound) == STATUS_INVALID_DEVICE_STATE);
    memset(&owner, 0, sizeof(owner));
    CHECK(HipJournalOwnerCreate(&owner, adapter, context, device) == STATUS_SUCCESS);
    CHECK(HipFindId(id)->ContextCookie != owner.Cookie);
    CHECK(Cancel(id) == STATUS_INVALID_PARAMETER);
    HipJournalComplete(id);
    CHECK(HipFindId(id)->State == BC250_HIP_SLOT_COMPLETED);
}
static void TestPinsAndWrap(void)
{
    ULONGLONG ids[BC250_HIP_JOURNAL_SLOTS], id;
    TEST_BATCH batch;
    DXGKARG_ESCAPE escape;
    ULONG i;
    Reset();
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
        ids[i] = Upload(i + 1);
        if (i & 1) { CHECK(Bind(i + 1, i) == ids[i]); HipJournalSubmitted(ids[i], i + 1, 5, 0x100000); }
    }
    batch = MakeBatch(100); escape = EscapeFor(&batch);
    CHECK(HipJournalEscape(adapter, &escape) == STATUS_DEVICE_BUSY);
    CHECK(g_HipDispatchJournal.Overwritten == 0 && !batch.Header.UploadId);
    HipJournalComplete(ids[1]);
    id = Upload(100);
    CHECK(id > ids[31] && !HipFindId(ids[1]) && g_HipDispatchJournal.Overwritten == 1);
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) if (i != 1) CHECK(HipFindId(ids[i]) != NULL);
    CHECK(Cancel(id) == STATUS_SUCCESS);
    for (i = 0; i < 70; ++i) { id = Upload(200 + i); CHECK(Cancel(id) == STATUS_SUCCESS); }
    CHECK(g_HipDispatchJournal.Overwritten == 71);
}
static void CloseDuringCopy(void)
{
    ULONG i, found = 0;
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
        BC250_HIP_JOURNAL_SLOT* slot = &g_HipDispatchJournal.Slots[i];
        if (slot->State == BC250_HIP_SLOT_WRITING) { CHECK(slot->Generation & 1); ++found; }
    }
    CHECK(found == 1);
    HipJournalOwnerDestroy(&owner);
}
static void TestPublicationRace(void)
{
    TEST_BATCH batch = MakeBatch(1);
    DXGKARG_ESCAPE escape = EscapeFor(&batch);
    Reset();
    copyInterleave = CloseDuringCopy;
    CHECK(HipJournalEscape(adapter, &escape) == STATUS_PROCESS_IS_TERMINATING);
    CHECK(!owner.Registered && !owner.Enabled && !batch.Header.UploadId);
    CHECK(g_HipDispatchJournal.Slots[0].State == BC250_HIP_SLOT_FAILED);
    CHECK(!(g_HipDispatchJournal.Slots[0].Generation & 1));
}
static void TestResetReplay(void)
{
    ULONGLONG head, tail, rebound;
    LONG logs;
    Reset();
    head = Upload(1); tail = Upload(2);
    CHECK(Bind(1, 10) == head && Bind(2, 11) == tail);
    HipJournalSubmitted(head, 900, 5, 0x3000);
    HipJournalSubmitted(tail, 901, 5, 0x3000);
    HipJournalReset(head, FALSE);
    HipJournalReset(tail, TRUE);
    CHECK(HipFindId(head)->State == BC250_HIP_SLOT_FAILED && HipFindId(head)->Seq == 900);
    CHECK(HipFindId(tail)->State == BC250_HIP_SLOT_UPLOADED && HipFindId(tail)->ReplayPending &&
          HipFindId(tail)->Seq == 901 && HipFindId(tail)->Attempts == 1);
    CHECK(Cancel(tail) == STATUS_DEVICE_BUSY); /* the OS already owns the replay packet */
    HipJournalComplete(tail); /* stale old completion cannot release a replay pin */
    CHECK(HipFindId(tail)->State == BC250_HIP_SLOT_UPLOADED);
    CHECK(HipJournalBind(&owner, 0x4000010000ull, 0x4000030000ull, 2, 20, 4096, &rebound) == STATUS_SUCCESS);
    CHECK(rebound == tail && HipFindId(tail)->OsFence == 20 && !HipFindId(tail)->Seq &&
          !HipFindId(tail)->ReplayPending && HipFindId(tail)->Attempts == 2 && HipFindId(tail)->Status == STATUS_PENDING);
    HipJournalSubmitted(tail, 950, 7, 0x5000);
    CHECK(HipFindId(tail)->State == BC250_HIP_SLOT_SUBMITTED && HipFindId(tail)->Seq == 950);
    HipJournalComplete(tail);
    CHECK(HipFindId(tail)->State == BC250_HIP_SLOT_COMPLETED);
    logs = logCount;
    HipJournalSubmitted(tail, 951, 7, 0x5000);
    CHECK(logCount == logs); /* no fabricated attribution from a non-bound slot */
}
static DWORD WINAPI ConcurrentWorker(void* Argument)
{
    ULONG worker = (ULONG)(ULONG_PTR)Argument, i;
    currentProcess = &processA;
    for (i = 0; i < 64; ++i) {
        ULONGLONG fence = worker * 1000ull + i + 1, id = Upload(fence);
        CHECK(Bind(fence, (ULONG)fence) == id);
        HipJournalSubmitted(id, (ULONG)fence, 5, 0x3000);
        mockIrql = DISPATCH_LEVEL; /* real retirement enters from the DPC */
        HipJournalComplete(id);
        CHECK(mockIrql == DISPATCH_LEVEL && lockDepth == 0);
        mockIrql = PASSIVE_LEVEL;
    }
    return 0;
}
static void TestConcurrentPublication(void)
{
    HANDLE threads[8];
    ULONG i;
    Reset();
    for (i = 0; i < 8; ++i) {
        threads[i] = CreateThread(NULL, 0, ConcurrentWorker, (PVOID)(ULONG_PTR)i, 0, NULL);
        CHECK(threads[i] != NULL);
    }
    CHECK(WaitForMultipleObjects(8, threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (i = 0; i < 8; ++i) CloseHandle(threads[i]);
    CHECK(g_HipDispatchJournal.Uploads == 512 && g_HipDispatchJournal.Completed == 512 &&
          g_HipDispatchJournal.Refused == 0 && g_HipDispatchJournal.Overwritten == 480);
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
        BC250_HIP_JOURNAL_SLOT* slot = &g_HipDispatchJournal.Slots[i];
        CHECK(slot->State == BC250_HIP_SLOT_COMPLETED && !(slot->Generation & 1));
        CHECK(HipPayloadValid(slot->Data, slot->DataBytes));
    }
}
int main(void)
{
    currentProcess = &processA;
    TestShape(); TestOwnershipAndState(); TestPinsAndWrap(); TestPublicationRace();
    TestResetReplay(); TestConcurrentPublication();
    HipJournalOwnerDestroy(&owner);
    CHECK(processA.References == 0 && processB.References == 0 && mockIrql == PASSIVE_LEVEL && lockDepth == 0);
    printf("HIP journal: %ld checks, %ld failures\n", checks, failures);
    return failures ? 1 : 0;
}
