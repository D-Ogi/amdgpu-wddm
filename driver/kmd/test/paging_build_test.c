/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* BD-235: the statuses of DxgkDdiBuildPagingBuffer through wddm.c's own WddmBuildPagingBufferImpl and
 * Bc250WddmBuildPagingBuffer (generate_paging_build_test.py extracts them). The builders, VidMm, the journal and the
 * record publication are mocks that a case programs: a status, the words written and the progress. Each case checks
 * the status the OS gets, what was published and journalled, the multipass position and the counters. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "paging_ddi_status.h"

#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define MAXULONGLONG (~0ull)
#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
typedef long NTSTATUS, LONG;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef unsigned char UCHAR, *PUCHAR, BOOLEAN, KIRQL;
typedef unsigned long long ULONGLONG;
typedef long long LONG64, LONGLONG;
typedef void *HANDLE, *PVOID;
typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3L)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER ((NTSTATUS)0xC01E0001L)

#include "paging_build_types.inc"   /* BC250_WDDM_PAGING_UNSUPPORTED, BC250_WDDM_XFER, journal kinds: the KMD's own */

/* d3dkmddi.h, only the members that the DDI code reads, in one struct instead of the operation union. */
typedef enum { DXGK_OPERATION_TRANSFER = 0, DXGK_OPERATION_FILL = 1, DXGK_OPERATION_MAP_APERTURE_SEGMENT = 5,
               DXGK_OPERATION_UNMAP_APERTURE_SEGMENT = 6, DXGK_OPERATION_VIRTUAL_TRANSFER = 8,
               DXGK_OPERATION_VIRTUAL_FILL = 9, DXGK_OPERATION_UPDATE_PAGE_TABLE = 14, DXGK_OPERATION_FLUSH_TLB = 15,
               DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES = 16 } DXGK_BUILDPAGINGBUFFER_OPERATION;
typedef enum { DXGK_PAGETABLEUPDATE_CPU_VIRTUAL = 0, DXGK_PAGETABLEUPDATE_GPU_VIRTUAL = 1,
               DXGK_PAGETABLEUPDATE_GPU_PHYSICAL = 2 } DXGK_PAGETABLEUPDATE_MODE;
typedef enum { DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM = 1, DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL = 2 }
    DXGK_MEMORY_TRANSFER_DIRECTION;
typedef struct { UINT PageTableLevel, StartIndex, NumPageTableEntries; DXGK_PAGETABLEUPDATE_MODE UpdateMode; }
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
typedef struct {
    PVOID pDmaBuffer; UINT DmaSize; PVOID pDmaBufferPrivateData; UINT DmaBufferPrivateDataSize;
    DXGK_BUILDPAGINGBUFFER_OPERATION Operation; UINT MultipassOffset;
    ULONGLONG DmaBufferGpuVirtualAddress; UINT DmaBufferWriteOffset; HANDLE hSystemContext;
    struct { UINT SegmentId; } MapApertureSegment, UnmapApertureSegment;
    struct { HANDLE hAllocation; } Fill;
    struct { ULONGLONG DestinationVirtualAddress, FillSizeInBytes; HANDLE hAllocation; } FillVirtual;
    struct { DXGK_MEMORY_TRANSFER_DIRECTION TransferDirection; ULONGLONG SourceVirtualAddress; HANDLE hAllocation; }
        TransferVirtual;
    struct { HANDLE hAllocation; struct { UINT SegmentId; } Source, Destination; } Transfer;
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE UpdatePageTable;
    struct { ULONGLONG RootPageTableAddress; } FlushTlb;
} DXGKARG_BUILDPAGINGBUFFER;
typedef NTSTATUS DXGKDDI_BUILDPAGINGBUFFER(HANDLE, DXGKARG_BUILDPAGINGBUFFER*);

/* The members of wddm.c's BC250_WDDM that the DDI code touches, with wddm.c's types. */
typedef int BC250_WDDM_KIND;
typedef struct {
    BC250_WDDM_KIND PagingOps[32]; volatile LONG PagingOpsOverflow;
    volatile LONG PagingMapsBuilt, PagingUnmapsBuilt; volatile LONG64 PagingMapPages, PagingUnmapPages;
    volatile LONG PagingInsufficientBuffer, PagingFillsBuilt, PagingTransfersBuilt, PagingUpdatesBuilt, PagingFlushesBuilt;
    volatile LONG64 PagingBytesMoved, PagingFillBytes;
    volatile LONG PagingXferCount[BC250WddmXferKinds]; volatile LONG64 PagingXferBytes[BC250WddmXferKinds];
    volatile LONG PagingUnsupported[BC250PagingNotContiguous + 1];
    volatile LONG PagingContractRefusals, PagingContractLastStatus, PagingContractLastOperation, PagingInnerFailures;
    BOOLEAN NativePteCopies;
    int PagingBuildLock;
    volatile LONG64 PagingDmaVaBuilds, PagingDmaZeroVaBuilds;
    volatile LONG PagingDmaVaObserved, PagingDmaVaMatched;
    ULONGLONG PagingDmaLastVa, PagingDmaLastRoot, PagingDmaLastPa, PagingDmaLastCpu;
} BC250_WDDM;
typedef struct { int Unused; } BC250_DEVICE;
typedef struct { ULONGLONG RootPhysical; int Captures; } BC250_WDDM_OBJECT;
#define BC250_WDDM_MAGIC_CONTEXT 0x43545854u
#define PAGING_PRIVATE_HEADER_BYTES 16u
enum { WddmDdiBuildPagingBuffer = 7, ProgressSiteBuildPagingBuffer = 3 };

/* ---- mocks ---------------------------------------------------------------------------------------------------- */
static BC250_WDDM wddm_state;
static BC250_WDDM_OBJECT context_state = { 0x40000, 0 };
static BC250_DEVICE device_state;
static unsigned bad, checks, logs;
static char last_log[256];
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* What the next builder call answers. */
static struct {
    NTSTATUS status;            /* the builder's status */
    ULONG written, next;        /* GPU update/flush: the command dwords and the next entry */
    BC250_WDDM_PAGING_UNSUPPORTED unsupported;
    ULONGLONG moved;            /* fill/transfer: the bytes built */
    UINT progress;              /* fill/transfer: the pages it advanced MultipassOffset by */
    NTSTATUS vidmm, publish;    /* VidMmUpdatePageTable, WddmPublishPagingRecord */
} mock;
static unsigned builder_calls, vidmm_calls, publishes, published_words, journal_updates, journal_notes;
static ULONG journal_start, journal_count, journal_kind; static BOOLEAN journal_cpu; static ULONGLONG journal_bytes;

static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static LONG64 InterlockedIncrement64(volatile LONG64* p) { return ++*p; }
static LONG64 InterlockedAdd64(volatile LONG64* p, LONG64 v) { return *p += v; }
static LONG InterlockedExchange(volatile LONG* p, LONG v) { LONG o = *p; *p = v; return o; }
static void KeEnterCriticalRegion(void) {}
static void KeLeaveCriticalRegion(void) {}
static void ExAcquirePushLockExclusive(int* l) { CHECK(*l == 0); *l = 1; }
static void ExReleasePushLockExclusive(int* l) { CHECK(*l == 1); *l = 0; }
static KIRQL KeGetCurrentIrql(void) { return 0; }
static PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p) { PHYSICAL_ADDRESS a; a.QuadPart = (LONGLONG)(ULONGLONG)p; return a; }
static void ProgressEnterInput(int Site, LONG Value) { (void)Site; (void)Value; }
static void ProgressExit(int Site, LONG Value) { (void)Site; (void)Value; }
static void GuardLog(const char* Format, ...)
{
    va_list a;
    va_start(a, Format); vsnprintf(last_log, sizeof(last_log), Format, a); va_end(a);
    if (strstr(Format, "***") != NULL) ++logs;      /* the failure lines, not the first-calls lines */
}
static BC250_WDDM* WddmOf(HANDLE h) { return h ? &wddm_state : NULL; }
static void WddmNoteKind(BC250_WDDM_KIND* Table, ULONG Value, volatile LONG* Overflow)
{ (void)Table; (void)Value; (void)Overflow; }
static BOOLEAN WddmFirstCalls(BC250_WDDM* Wddm, int Ddi) { (void)Wddm; (void)Ddi; return FALSE; }
static BC250_WDDM_OBJECT* WddmObject(HANDLE h, unsigned Magic) { (void)Magic; return h ? &context_state : NULL; }
static BOOLEAN WddmNativeAllocation(BC250_DEVICE* d, BC250_WDDM_OBJECT* c, DXGKARG_BUILDPAGINGBUFFER* b, BOOLEAN f)
{ (void)d; (void)c; (void)b; (void)f; return FALSE; }
static NTSTATUS WddmBuildNativeVirtual(BC250_DEVICE* d, ULONGLONG r, BOOLEAN f, DXGKARG_BUILDPAGINGBUFFER* b,
                                       ULONGLONG* m)
{ (void)d; (void)r; (void)f; (void)b; *m = 0; return STATUS_NOT_SUPPORTED; }
/* Every fill and transfer builder: the bytes and pages of the programmed slice, then its status. */
static NTSTATUS MockSlices(DXGKARG_BUILDPAGINGBUFFER* b, ULONGLONG* Moved)
{
    ++builder_calls;
    b->MultipassOffset += mock.progress;
    *Moved = mock.moved;
    return mock.status;
}
static NTSTATUS WddmBuildAperture(BC250_DEVICE* d, DXGKARG_BUILDPAGINGBUFFER* b, BOOLEAN u)
{ ULONGLONG m; (void)d; (void)u; return MockSlices(b, &m); }
static NTSTATUS WddmBuildPhysicalFill(BC250_DEVICE* d, DXGKARG_BUILDPAGINGBUFFER* b, ULONGLONG* m)
{ (void)d; return MockSlices(b, m); }
static NTSTATUS WddmBuildVirtualFill(BC250_DEVICE* d, ULONGLONG r, DXGKARG_BUILDPAGINGBUFFER* b, ULONGLONG* m)
{ (void)d; (void)r; return MockSlices(b, m); }
static NTSTATUS WddmBuildCapturedVirtualTransfer(BC250_DEVICE* d, ULONGLONG r, int* c, DXGKARG_BUILDPAGINGBUFFER* b,
                                                 ULONGLONG* m)
{ (void)d; (void)r; (void)c; return MockSlices(b, m); }
static NTSTATUS WddmBuildPhysicalTransfer(BC250_DEVICE* d, DXGKARG_BUILDPAGINGBUFFER* b, ULONGLONG* m)
{ (void)d; return MockSlices(b, m); }
static NTSTATUS WddmBuildNativePagingCopies(BC250_DEVICE* d, ULONGLONG r, DXGKARG_BUILDPAGINGBUFFER* b)
{ (void)d; (void)r; (void)b; ++builder_calls; return mock.status; }
static NTSTATUS WddmBuildPagingCopies(BC250_DEVICE* d, ULONGLONG r, DXGKARG_BUILDPAGINGBUFFER* b)
{ (void)d; (void)r; (void)b; ++builder_calls; return mock.status; }
static ULONG PagingPrivateQueuedDirectCapacity(ULONG PrivateBytes) { return PrivateBytes - PAGING_PRIVATE_HEADER_BYTES; }
static NTSTATUS GfxPagingBuildUpdate(BC250_DEVICE* d, const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* u, PVOID Dma,
    ULONG Offset, ULONG Free, ULONG Start, ULONG* Written, ULONG* Next, BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    (void)d; (void)u; (void)Dma; (void)Offset; (void)Free;
    ++builder_calls;
    *Written = mock.written; *Next = mock.next ? mock.next : Start; *Unsupported = mock.unsupported;
    return mock.status;
}
static NTSTATUS GfxPagingBuildFlush(BC250_DEVICE* d, ULONGLONG Root, PVOID Dma, ULONG Offset, ULONG Free,
                                    ULONG* Written, BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    (void)d; (void)Root; (void)Dma; (void)Offset; (void)Free;
    ++builder_calls;
    *Written = mock.written; *Unsupported = mock.unsupported;
    return mock.status;
}
static BOOLEAN VidMmRootPhysical(const ULONGLONG* Address, ULONGLONG* Root) { *Root = *Address; return TRUE; }
static BOOLEAN VidMmTranslatePaging(ULONGLONG Root, ULONGLONG Va, ULONGLONG* Pa, BOOLEAN* System)
{ (void)Root; (void)Va; *Pa = 0; *System = FALSE; return FALSE; }
static NTSTATUS VidMmUpdatePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* u) { (void)u; ++vidmm_calls; return mock.vidmm; }
static void PagingJournalUpdate(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* u, ULONG Start, ULONG Count,
                                ULONGLONG Dma, BOOLEAN Cpu)
{ (void)u; (void)Dma; ++journal_updates; journal_start = Start; journal_count = Count; journal_cpu = Cpu; }
static void PagingJournalNote(ULONG Kind, ULONGLONG Va, HANDLE Allocation, ULONGLONG Bytes, ULONGLONG Dma, ULONG Flags)
{ (void)Va; (void)Allocation; (void)Dma; (void)Flags; ++journal_notes; journal_kind = Kind; journal_bytes = Bytes; }
static NTSTATUS WddmPublishPagingRecord(DXGKARG_BUILDPAGINGBUFFER* b, ULONG Written, BOOLEAN Update, ULONG Start,
                                        ULONG Next)
{ (void)b; (void)Update; (void)Start; (void)Next; ++publishes; published_words += Written; return mock.publish; }

#include "paging_build_actual.inc"

/* ---- cases ---------------------------------------------------------------------------------------------------- */
static ULONG private_record[64];
static DXGKARG_BUILDPAGINGBUFFER build;
#define ADAPTER ((HANDLE)&device_state)

static void Fresh(DXGK_BUILDPAGINGBUFFER_OPERATION Operation, DXGK_PAGETABLEUPDATE_MODE Mode, UINT Offset)
{
    memset(&wddm_state, 0, sizeof(wddm_state));
    memset(&mock, 0, sizeof(mock));
    memset(&build, 0, sizeof(build));
    builder_calls = vidmm_calls = publishes = published_words = journal_updates = journal_notes = 0;
    journal_start = journal_count = journal_kind = 0; journal_cpu = FALSE; journal_bytes = 0; logs = 0;
    build.Operation = Operation;
    build.UpdatePageTable.UpdateMode = Mode;
    build.UpdatePageTable.NumPageTableEntries = 20;
    build.UpdatePageTable.PageTableLevel = 1;
    build.MultipassOffset = Offset;
    build.DmaSize = 4096;
    build.pDmaBufferPrivateData = private_record;
    build.DmaBufferPrivateDataSize = sizeof(private_record);
    build.hSystemContext = (HANDLE)&context_state;
    build.FlushTlb.RootPageTableAddress = 0x40000;
    build.FillVirtual.FillSizeInBytes = 64ull << 20;
}
static NTSTATUS Ddi(void) { return Bc250WddmBuildPagingBuffer(ADAPTER, &build); }

/* A GPU_PHYSICAL update or a FLUSH_TLB whose builder fails: the failure to the OS, nothing published or journalled,
 * the position as it was, counted once as an inner failure. */
static void InnerFailure(DXGK_BUILDPAGINGBUFFER_OPERATION Op, NTSTATUS Status, BC250_WDDM_PAGING_UNSUPPORTED Reason,
                         ULONG Written, UINT Offset, NTSTATUS ToOs)
{
    Fresh(Op, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, Offset);
    mock.status = Status; mock.unsupported = Reason; mock.written = Written; mock.next = Offset + 5;
    CHECK(Ddi() == ToOs);
    CHECK(builder_calls == 1 && publishes == 0 && journal_updates == 0 && journal_notes == 0);
    CHECK(build.MultipassOffset == Offset);
    CHECK(wddm_state.PagingInnerFailures == 1 && wddm_state.PagingContractRefusals == 1);
    CHECK(wddm_state.PagingUnsupported[Reason] == (Reason == BC250PagingSupported ? 0 : 1));
    CHECK(wddm_state.PagingUpdatesBuilt == 0 && wddm_state.PagingFlushesBuilt == 0);
    CHECK(wddm_state.PagingBuildLock == 0 && logs == 2);   /* the inner failure line and the wrapper's line */
}

int main(void)
{
    const ULONGLONG page = 4096;

    /* GPU_PHYSICAL update and FLUSH_TLB: every status that is not a result is a failure to the OS. */
    InnerFailure(DXGK_OPERATION_UPDATE_PAGE_TABLE, STATUS_INVALID_PARAMETER, BC250PagingSupported, 0, 0,
                 STATUS_INVALID_PARAMETER);                         /* an encoding or MC range refusal */
    CHECK(strstr(last_log, "outside the DDI") != NULL);
    InnerFailure(DXGK_OPERATION_UPDATE_PAGE_TABLE, STATUS_DEVICE_NOT_READY, BC250PagingNotReady, 0, 0,
                 STATUS_DEVICE_NOT_READY);                          /* not ready: counted as a reason too */
    InnerFailure(DXGK_OPERATION_UPDATE_PAGE_TABLE, STATUS_PENDING, BC250PagingSupported, 7, 0,
                 STATUS_UNSUCCESSFUL);                              /* not a result: words are not published */
    InnerFailure(DXGK_OPERATION_FLUSH_TLB, STATUS_DEVICE_NOT_READY, BC250PagingNotReady, 0, 0, STATUS_DEVICE_NOT_READY);
    InnerFailure(DXGK_OPERATION_FLUSH_TLB, STATUS_INVALID_PARAMETER, BC250PagingSupported, 0, 0, STATUS_INVALID_PARAMETER);
    /* A failure after a published prefix (the second slice of a multipass update, or the engine that goes into a
     * reset between two slices): the position stays at the prefix. */
    InnerFailure(DXGK_OPERATION_UPDATE_PAGE_TABLE, STATUS_INVALID_PARAMETER, BC250PagingSupported, 0, 10,
                 STATUS_INVALID_PARAMETER);
    InnerFailure(DXGK_OPERATION_UPDATE_PAGE_TABLE, STATUS_DEVICE_NOT_READY, BC250PagingNotReady, 0, 10,
                 STATUS_DEVICE_NOT_READY);

    /* Genuine DMA exhaustion: the prefix is published and journalled, the position moves to it, the OS gets
     * INSUFFICIENT_DMA_BUFFER; the next call starts there and completes. No slice is skipped or repeated. */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    mock.status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; mock.written = 34; mock.next = 10;
    CHECK(Ddi() == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    CHECK(publishes == 1 && published_words == 34 && journal_updates == 1 && journal_start == 0 && journal_count == 10);
    CHECK(build.MultipassOffset == 10 && wddm_state.PagingInsufficientBuffer == 1 && wddm_state.PagingUpdatesBuilt == 1);
    CHECK(wddm_state.PagingInnerFailures == 0 && wddm_state.PagingContractRefusals == 0);
    mock.status = STATUS_SUCCESS; mock.written = 34; mock.next = 20;
    CHECK(Ddi() == STATUS_SUCCESS);
    CHECK(publishes == 2 && journal_updates == 2 && journal_start == 10 && journal_count == 10);
    CHECK(build.MultipassOffset == 20 && wddm_state.PagingUpdatesBuilt == 2 && wddm_state.PagingInnerFailures == 0);
    /* No room at all: INSUFFICIENT_DMA_BUFFER with nothing written publishes nothing and keeps the position. */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 10);
    mock.status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    CHECK(Ddi() == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && publishes == 0 && build.MultipassOffset == 10);
    /* No private record space: INSUFFICIENT_DMA_BUFFER before the builder. */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    build.pDmaBufferPrivateData = NULL;
    CHECK(Ddi() == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && builder_calls == 0);
    /* Exact completion in one call. */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    mock.written = 54; mock.next = 20;
    CHECK(Ddi() == STATUS_SUCCESS && publishes == 1 && journal_count == 20 && build.MultipassOffset == 20);
    /* Zero-word successes are results: the CPU bootstrap update wrote the table itself, and a flush with no VMID to
     * invalidate has nothing to do. Neither publishes a record. */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    mock.next = 20;
    CHECK(Ddi() == STATUS_SUCCESS && publishes == 0 && wddm_state.PagingInnerFailures == 0 && build.MultipassOffset == 20);
    Fresh(DXGK_OPERATION_FLUSH_TLB, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    CHECK(Ddi() == STATUS_SUCCESS && publishes == 0 && journal_notes == 0 && wddm_state.PagingInnerFailures == 0);
    Fresh(DXGK_OPERATION_FLUSH_TLB, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    mock.written = 12;
    CHECK(Ddi() == STATUS_SUCCESS && publishes == 1 && journal_notes == 1 && journal_kind == BC250_PJ_FLUSH_TLB);
    CHECK(wddm_state.PagingFlushesBuilt == 1);
    /* A record that cannot be published is a failure too (it was already). */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    mock.written = 54; mock.next = 20; mock.publish = STATUS_INVALID_PARAMETER;
    CHECK(Ddi() == STATUS_INVALID_PARAMETER && journal_updates == 0 && wddm_state.PagingContractRefusals == 1);

    /* CPU_VIRTUAL: the journal records the update only when VidMm made it; a failure goes to the OS. */
    Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_CPU_VIRTUAL, 0);
    CHECK(Ddi() == STATUS_SUCCESS && vidmm_calls == 1 && journal_updates == 1 && journal_cpu && journal_count == 20);
    CHECK(builder_calls == 0 && publishes == 0 && wddm_state.PagingInnerFailures == 0);
    {
        static const NTSTATUS failures[] = { STATUS_INVALID_PARAMETER, STATUS_DEVICE_NOT_READY, STATUS_PENDING };
        unsigned i;
        for (i = 0; i < RTL_NUMBER_OF(failures); ++i) {
            Fresh(DXGK_OPERATION_UPDATE_PAGE_TABLE, DXGK_PAGETABLEUPDATE_CPU_VIRTUAL, 0);
            mock.vidmm = failures[i];
            CHECK(Ddi() == (failures[i] >= 0 ? STATUS_UNSUCCESSFUL : failures[i]));
            CHECK(vidmm_calls == 1 && journal_updates == 0 && builder_calls == 0 && publishes == 0);
            CHECK(wddm_state.PagingInnerFailures == 1 && wddm_state.PagingContractRefusals == 1);
        }
    }

    /* Fill and transfer: each builder's status goes to the OS as it is. A failure before the first page journals
     * nothing; a failure after N published pages (2528, the BD-235 fill) leaves the position where the builder left
     * it and journals the bytes that were built, and the OS still gets the failure, never SUCCESS. */
    {
        static const DXGK_BUILDPAGINGBUFFER_OPERATION ops[] = { DXGK_OPERATION_VIRTUAL_FILL, DXGK_OPERATION_FILL,
                                                                DXGK_OPERATION_VIRTUAL_TRANSFER, DXGK_OPERATION_TRANSFER };
        unsigned i;
        for (i = 0; i < RTL_NUMBER_OF(ops); ++i) {
            Fresh(ops[i], DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
            mock.status = STATUS_INVALID_PARAMETER;
            CHECK(Ddi() == STATUS_INVALID_PARAMETER && journal_notes == 0 && build.MultipassOffset == 0);
            CHECK(wddm_state.PagingContractRefusals == 1 && wddm_state.PagingBytesMoved == 0);
            Fresh(ops[i], DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
            mock.status = STATUS_INVALID_PARAMETER; mock.moved = 2528 * page; mock.progress = 2528;
            CHECK(Ddi() == STATUS_INVALID_PARAMETER && build.MultipassOffset == 2528);
            CHECK(journal_notes == 1 && journal_bytes == 2528 * page && wddm_state.PagingContractRefusals == 1);
            Fresh(ops[i], DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
            mock.status = STATUS_DEVICE_NOT_READY; mock.moved = 2528 * page; mock.progress = 2528;   /* a reset */
            CHECK(Ddi() == STATUS_DEVICE_NOT_READY && build.MultipassOffset == 2528);
            Fresh(ops[i], DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
            mock.status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; mock.moved = 2528 * page; mock.progress = 2528;
            CHECK(Ddi() == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && build.MultipassOffset == 2528);
            CHECK(wddm_state.PagingContractRefusals == 0 && wddm_state.PagingInsufficientBuffer == 1);
            Fresh(ops[i], DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 2528);
            mock.status = STATUS_PENDING; mock.moved = page; mock.progress = 1;
            CHECK(Ddi() == STATUS_UNSUCCESSFUL && wddm_state.PagingContractRefusals == 1);
            Fresh(ops[i], DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 2528);
            mock.moved = 3000 * page; mock.progress = 3000;
            CHECK(Ddi() == STATUS_SUCCESS && build.MultipassOffset == 5528 && wddm_state.PagingContractRefusals == 0);
        }
    }
    /* COPY_PAGE_TABLE_ENTRIES: the builder's status, as it is. */
    Fresh(DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES, DXGK_PAGETABLEUPDATE_GPU_PHYSICAL, 0);
    mock.status = STATUS_DEVICE_NOT_READY;
    CHECK(Ddi() == STATUS_DEVICE_NOT_READY && wddm_state.PagingContractRefusals == 1);

    printf("BuildPagingBuffer DDI: %u checks, %u failures\n", checks, bad);
    return bad ? 1 : 0;
}
