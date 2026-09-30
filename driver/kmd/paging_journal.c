// The paging journal (paging_journal.h): who mapped, unmapped, filled, moved or destroyed what, and when, as
// VidMm's operations reach this driver. Written for the 118/119/151 class of GPU page faults (game-recon
// bsod-151, REPORT-151.md): a job faulted on 65 pages that a paging buffer had touched 0.3 ms earlier, and
// nothing in the dump said which operation, on whose allocation, or why. The ring lives in the image (nonpaged,
// zero-initialized data) so that a kernel dump holds it whole: no pool, no MDL, no register. Kto nie ma w głowie,
// ten ma w nogach (who has no memory walks twice): the journal is the memory, so that the next dump walks once.
#include "bc250kmd.h"
#include "paging_journal.h"

BC250_PAGING_JOURNAL g_PagingJournal;
static KSPIN_LOCK g_PagingJournalLock;
static BOOLEAN g_PagingJournalReady;

void PagingJournalInit(void)
{
    RtlZeroMemory(&g_PagingJournal, sizeof(g_PagingJournal));
    KeInitializeSpinLock(&g_PagingJournalLock);
    g_PagingJournal.Magic = BC250_PAGING_JOURNAL_MAGIC;
    g_PagingJournal.Version = BC250_PAGING_JOURNAL_VERSION;
    g_PagingJournal.EntryBytes = sizeof(BC250_PAGING_JOURNAL_RECORD);
    g_PagingJournal.Entries = BC250_PAGING_JOURNAL_ENTRIES;
    g_PagingJournalReady = TRUE;
}

// The time is taken inside the lock, so that times rise with record indices (as GuardLog's do). A caller above
// DISPATCH_LEVEL (none today) loses its record rather than the machine.
static void PagingJournalCommit(_In_ const BC250_PAGING_JOURNAL_RECORD* Record)
{
    BC250_PAGING_JOURNAL_RECORD* slot;
    KIRQL irql;

    if (!g_PagingJournalReady || KeGetCurrentIrql() > DISPATCH_LEVEL) return;
    KeAcquireSpinLock(&g_PagingJournalLock, &irql);
    slot = &g_PagingJournal.Entry[g_PagingJournal.Next % BC250_PAGING_JOURNAL_ENTRIES];
    *slot = *Record;
    slot->Time = KeQueryInterruptTime();
    g_PagingJournal.Next++;
    KeReleaseSpinLock(&g_PagingJournalLock, irql);
}

// One BuildPagingBuffer slice of an UPDATE_PAGE_TABLE request: its entries [SliceStart, SliceStart + SliceCount).
// Va is what the slice's first entry maps: FirstPteVirtualAddress is the request's first entry (d3dkmddi.h,
// WDDM 2.0), and a level-L entry of this driver's 512-entry tables (BC250_WDDM_PTES_PER_LEVEL) spans
// 4 KiB << (9 * L). Valid counts the Windows Valid bits of the slice; the others zero their entries.
void PagingJournalUpdate(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update, ULONG SliceStart, ULONG SliceCount,
                         ULONGLONG Dma, BOOLEAN Cpu)
{
    BC250_PAGING_JOURNAL_RECORD r;
    ULONG i, valid = 0, span;

    if (Update == NULL) return;
    RtlZeroMemory(&r, sizeof(r));
    if (Update->pPageTableEntries != NULL && SliceStart <= Update->NumPageTableEntries &&
        SliceCount <= Update->NumPageTableEntries - SliceStart)
        for (i = 0; i < SliceCount; i++)
            if (Update->pPageTableEntries[Update->Flags.Repeat ? 0 : SliceStart + i].Valid) valid++;
    span = Update->PageTableLevel < 5 ? 12 + 9 * Update->PageTableLevel : 63;
    r.Kind = Cpu ? BC250_PJ_UPDATE_CPU : BC250_PJ_UPDATE_GPU;
    r.Level = Update->PageTableLevel;
    r.Index = Update->StartIndex + SliceStart;
    r.Count = SliceCount;
    r.Valid = valid;
    r.Flags = (Update->Flags.Repeat ? BC250_PJ_FLAG_REPEAT : 0u) | (Update->Flags.InitialUpdate ? BC250_PJ_FLAG_INITIAL : 0u) |
              (Update->Flags.NotifyEviction ? BC250_PJ_FLAG_EVICTION : 0u) | (Update->Flags.Use64KBPages ? BC250_PJ_FLAG_64KB : 0u);
    r.Va = Update->FirstPteVirtualAddress + ((ULONGLONG)SliceStart << span);
    r.Allocation = (ULONGLONG)(ULONG_PTR)Update->hAllocation;
    r.Offset = Update->AllocationOffsetInBytes;
    r.Dma = Dma;
    PagingJournalCommit(&r);
}

// Every other kind: a virtual fill or transfer (Va is the destination or source address, Bytes what the build
// moved), a physical one (Va 0), a TLB flush (only its position in the paging buffer) or DestroyAllocation
// (Va the UMD's requested address, Bytes the allocation's size).
void PagingJournalNote(ULONG Kind, ULONGLONG Va, _In_opt_ HANDLE Allocation, ULONGLONG Bytes, ULONGLONG Dma, ULONG Flags)
{
    BC250_PAGING_JOURNAL_RECORD r;

    RtlZeroMemory(&r, sizeof(r));
    r.Kind = Kind;
    r.Flags = Flags;
    r.Va = Va;
    r.Allocation = (ULONGLONG)(ULONG_PTR)Allocation;
    r.Offset = Bytes;
    r.Dma = Dma;
    PagingJournalCommit(&r);
}

// The OS submits the paging buffer the records were built into: DmaStart/DmaBytes is that submission's GPU
// range (SubmitCommand's DmaBufferVirtualAddress and DmaBufferSize), Fence its SubmissionFenceId. Records built
// into the range and not yet claimed take it. The walk runs backwards and stops at the first claimed record of
// the same range, which is the previous submission of a reused buffer; a build the OS never submitted (a refused
// buffer) would be claimed by the next submission of its range, which the reader tells from the times.
void PagingJournalStampFence(ULONGLONG DmaStart, ULONG DmaBytes, ULONG Fence)
{
    ULONGLONG n, live;
    KIRQL irql;

    if (!g_PagingJournalReady || KeGetCurrentIrql() > DISPATCH_LEVEL || DmaBytes == 0 || Fence == 0) return;
    KeAcquireSpinLock(&g_PagingJournalLock, &irql);
    live = g_PagingJournal.Next < BC250_PAGING_JOURNAL_ENTRIES ? g_PagingJournal.Next : BC250_PAGING_JOURNAL_ENTRIES;
    for (n = 0; n < live; n++) {
        BC250_PAGING_JOURNAL_RECORD* r = &g_PagingJournal.Entry[(g_PagingJournal.Next - 1 - n) % BC250_PAGING_JOURNAL_ENTRIES];
        if (r->Dma == 0 || r->Dma < DmaStart || r->Dma - DmaStart >= DmaBytes) continue;
        if (r->Fence != 0) break;
        r->Fence = Fence;
    }
    KeReleaseSpinLock(&g_PagingJournalLock, irql);
}

// The paging submit gave the buffer that carries Fence its SDMA sequence (GfxSubmitPaging, under wddm->Lock at
// DISPATCH_LEVEL). Same walk; a resubmission of a preempted packet keeps the first sequence recorded.
void PagingJournalStampSeq(ULONG Fence, ULONG Seq)
{
    ULONGLONG n, live;
    KIRQL irql;

    if (!g_PagingJournalReady || KeGetCurrentIrql() > DISPATCH_LEVEL || Fence == 0 || Seq == 0) return;
    KeAcquireSpinLock(&g_PagingJournalLock, &irql);
    live = g_PagingJournal.Next < BC250_PAGING_JOURNAL_ENTRIES ? g_PagingJournal.Next : BC250_PAGING_JOURNAL_ENTRIES;
    for (n = 0; n < live; n++) {
        BC250_PAGING_JOURNAL_RECORD* r = &g_PagingJournal.Entry[(g_PagingJournal.Next - 1 - n) % BC250_PAGING_JOURNAL_ENTRIES];
        if (r->Fence != Fence) continue;
        if (r->Seq != 0) break;
        r->Seq = Seq;
    }
    KeReleaseSpinLock(&g_PagingJournalLock, irql);
}

// Records from index From on, at most Max, into the caller's nonpaged page (the copy runs under the spin lock).
// Lost says how many of the requested records the ring had already overwritten; Next is the index to ask for
// next and Total the records written so far.
ULONG PagingJournalRead(ULONGLONG From, _Out_writes_to_(Max, return) BC250_PAGING_JOURNAL_RECORD* Page, ULONG Max,
                        _Out_ ULONGLONG* Next, _Out_ ULONGLONG* Total, _Out_ ULONGLONG* Lost)
{
    ULONGLONG total, oldest, from;
    ULONG count = 0;
    KIRQL irql;

    *Next = From;
    *Total = 0;
    *Lost = 0;
    if (!g_PagingJournalReady || Page == NULL || Max == 0) return 0;
    KeAcquireSpinLock(&g_PagingJournalLock, &irql);
    total = g_PagingJournal.Next;
    oldest = total > BC250_PAGING_JOURNAL_ENTRIES ? total - BC250_PAGING_JOURNAL_ENTRIES : 0;
    *Lost = From < oldest ? oldest - From : 0;
    from = From < oldest ? oldest : From;
    if (from > total) from = total;
    while (count < Max && from + count < total) {
        Page[count] = g_PagingJournal.Entry[(from + count) % BC250_PAGING_JOURNAL_ENTRIES];
        count++;
    }
    *Next = from + count;
    *Total = total;
    KeReleaseSpinLock(&g_PagingJournalLock, irql);
    return count;
}
