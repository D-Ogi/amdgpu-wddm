/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* BD-235 Q3a: the paging journal's fence and sequence stamps through paging_journal.c's own code
 * (generate_paging_journal_test.py extracts it). A fill or transfer that failed after it built a prefix keeps its
 * record, marked BC250_PJ_FLAG_FAILED with the builder's status; the OS submitted none of that prefix, so no later
 * submission may stamp its fence or sequence on it, also when the OS reuses the same paging buffer range. The record
 * layout and the ring come from the driver's headers; the kernel calls and the DXGK types are mocks. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define FIELD_OFFSET(t, f) offsetof(t, f)
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef unsigned int UINT;
typedef unsigned char BOOLEAN, KIRQL;
typedef uintptr_t ULONG_PTR, KSPIN_LOCK;
typedef void* HANDLE;

#include "paging_journal.h"
#include "paging_identity.h"

/* d3dukmdt.h and d3dkmddi.h, only the members PagingJournalUpdate reads. */
typedef struct { ULONGLONG Valid : 1, Segment : 5, LargePage : 1; } DXGK_PTE;
typedef struct {
    UINT PageTableLevel, StartIndex, NumPageTableEntries;
    const DXGK_PTE* pPageTableEntries;
    struct { UINT Repeat : 1, InitialUpdate : 1, NotifyEviction : 1, Use64KBPages : 1; } Flags;
    ULONGLONG FirstPteVirtualAddress, AllocationOffsetInBytes;
    HANDLE hAllocation, hProcess;
} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;

/* ---- mocks ---------------------------------------------------------------------------------------------------- */
static unsigned bad, checks;
static ULONGLONG clock_now;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static void RtlZeroMemory(void* p, size_t n) { memset(p, 0, n); }
static void KeInitializeSpinLock(KSPIN_LOCK* l) { *l = 0; }
static void KeAcquireSpinLock(KSPIN_LOCK* l, KIRQL* Old) { CHECK(*l == 0); *l = 1; *Old = 0; }
static void KeReleaseSpinLock(KSPIN_LOCK* l, KIRQL Old) { (void)Old; CHECK(*l == 1); *l = 0; }
static KIRQL KeGetCurrentIrql(void) { return 0; }
static ULONGLONG KeQueryInterruptTime(void) { return ++clock_now; }
static HANDLE PsGetCurrentProcessId(void) { return (HANDLE)4; }
static HANDLE PsGetCurrentThreadId(void) { return (HANDLE)8; }
static ULONG HandleToULong(HANDLE h) { return (ULONG)(uintptr_t)h; }

void PagingJournalInit(void);
void PagingJournalUpdate(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update, ULONG SliceStart, ULONG SliceCount,
                         ULONGLONG Dma, BOOLEAN Cpu);
void PagingJournalNote(ULONG Kind, ULONGLONG Va, HANDLE Allocation, ULONGLONG Bytes, ULONGLONG Dma, ULONG Flags,
                       ULONG Failure);
void PagingJournalDestroy(ULONGLONG Va, HANDLE Allocation, ULONGLONG Bytes, ULONG Flags, ULONG Creator,
                          ULONG BlobVersion, ULONGLONG GemFlags);
void PagingJournalGfxSubmit(ULONG Seq, ULONG Fence, ULONGLONG Ib1, ULONGLONG Root, ULONGLONG Context, ULONG Node,
                            ULONG Process, ULONG ContextFlags, ULONG Vmid);
void PagingJournalStampFence(ULONGLONG DmaStart, ULONG DmaBytes, ULONG Fence);
void PagingJournalStampSeq(ULONG Fence, ULONG Seq);
ULONG PagingJournalRead(ULONGLONG From, BC250_PAGING_JOURNAL_RECORD* Page, ULONG Max, ULONGLONG* Next, ULONGLONG* Total,
                        ULONGLONG* Lost);

#include "paging_journal_actual.inc"

/* ---- cases ---------------------------------------------------------------------------------------------------- */
#define PAGE 4096ull
#define BUF 0x7F0000000ull                      /* a paging buffer's GPU VA, which the OS reuses */
#define BUF_BYTES 0x10000u
#define INVALID_PARAMETER 0xC000000Dul          /* the status of the BD-235 fill */
#define DEVICE_NOT_READY 0xC00000A3ul
static BC250_PAGING_JOURNAL_RECORD* At(ULONGLONG n) { return &g_PagingJournal.Entry[n % BC250_PAGING_JOURNAL_ENTRIES]; }

/* The flag is one bit that no other kind's flags use, so no other record reads as failed. */
static void FlagBit(void)
{
    const ULONG others = BC250_PJ_FLAG_REPEAT | BC250_PJ_FLAG_INITIAL | BC250_PJ_FLAG_EVICTION | BC250_PJ_FLAG_64KB |
                         BC250_PJ_FLAG_TO_SYSTEM | BC250_PJ_FLAG_UMD_ALLOCATION | BC250_PJ_FLAG_PROCESS |
                         BC250_PJ_FLAG_LARGE | BC250_PJ_FLAG_SEGMENT_MASK;
    CHECK(BC250_PJ_FLAG_FAILED != 0 && (BC250_PJ_FLAG_FAILED & (BC250_PJ_FLAG_FAILED - 1)) == 0);
    CHECK((BC250_PJ_FLAG_FAILED & others) == 0);
}

static void FailedBuildThenReuse(void)
{
    BC250_PAGING_JOURNAL_RECORD page[BC250_PAGING_JOURNAL_MAX];
    ULONGLONG next, total, lost;
    DXGK_PTE ptes[3];
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update;

    PagingJournalInit();
    /* 0: a virtual fill built at the start of the buffer, submitted with fence 10 as SDMA sequence 100. */
    PagingJournalNote(BC250_PJ_VIRTUAL_FILL, 0x100000, (HANDLE)0xA1, 16 * PAGE, BUF, 0, 0);
    PagingJournalStampFence(BUF, BUF_BYTES, 10);
    PagingJournalStampSeq(10, 100);
    CHECK(At(0)->Fence == 10 && At(0)->Seq == 100 && At(0)->Flags == 0 && At(0)->Index == 0);

    /* 1: the BD-235 fill in the same buffer range: 2528 pages built, then the failure. The record keeps the prefix. */
    PagingJournalNote(BC250_PJ_VIRTUAL_FILL, 0x200000, (HANDLE)0xA2, 2528 * PAGE, BUF, 0, INVALID_PARAMETER);
    CHECK(At(1)->Kind == BC250_PJ_VIRTUAL_FILL && At(1)->Va == 0x200000 && At(1)->Allocation == 0xA2);
    CHECK(At(1)->Offset == 2528 * PAGE && At(1)->Dma == BUF && At(1)->Time != 0);
    CHECK(At(1)->Flags == BC250_PJ_FLAG_FAILED && At(1)->Index == INVALID_PARAMETER);
    CHECK(At(1)->Fence == 0 && At(1)->Seq == 0);

    /* 2: the OS reuses the range: a transfer built there (its own flag kept), submitted with fence 11, sequence 101.
     * The failed record between it and the previous submission takes neither; the earlier record keeps its own. */
    PagingJournalNote(BC250_PJ_VIRTUAL_TRANSFER, 0x300000, (HANDLE)0xA3, 4 * PAGE, BUF, BC250_PJ_FLAG_TO_SYSTEM, 0);
    PagingJournalStampFence(BUF, BUF_BYTES, 11);
    PagingJournalStampSeq(11, 101);
    CHECK(At(2)->Fence == 11 && At(2)->Seq == 101 && At(2)->Flags == BC250_PJ_FLAG_TO_SYSTEM);
    CHECK(At(1)->Fence == 0 && At(1)->Seq == 0);
    CHECK(At(1)->Offset == 2528 * PAGE && At(1)->Flags == BC250_PJ_FLAG_FAILED && At(1)->Index == INVALID_PARAMETER);
    CHECK(At(0)->Fence == 10 && At(0)->Seq == 100);

    /* A submission of the range with no new build: no record takes fence 12. */
    PagingJournalStampFence(BUF, BUF_BYTES, 12);
    PagingJournalStampSeq(12, 102);
    CHECK(At(1)->Fence == 0 && At(1)->Seq == 0);
    CHECK(At(2)->Fence == 11 && At(2)->Seq == 101 && At(0)->Fence == 10);

    /* 3-5: the walk passes a failed record and goes on: the records on both sides of it in the range still take the
     * fence of their submission. */
    PagingJournalNote(BC250_PJ_FILL, 0, (HANDLE)0xB1, 8 * PAGE, BUF + 0x100, 0, 0);
    PagingJournalNote(BC250_PJ_TRANSFER, 0, (HANDLE)0xB2, 2528 * PAGE, BUF + 0x200, 0, DEVICE_NOT_READY);
    PagingJournalNote(BC250_PJ_FILL, 0, (HANDLE)0xB3, 8 * PAGE, BUF + 0x300, 0, 0);
    PagingJournalStampFence(BUF, BUF_BYTES, 13);
    PagingJournalStampSeq(13, 103);
    CHECK(At(3)->Fence == 13 && At(3)->Seq == 103 && At(5)->Fence == 13 && At(5)->Seq == 103);
    CHECK(At(4)->Fence == 0 && At(4)->Seq == 0);
    CHECK(At(4)->Flags == BC250_PJ_FLAG_FAILED && At(4)->Index == DEVICE_NOT_READY && At(4)->Offset == 2528 * PAGE);
    CHECK(At(1)->Fence == 0 && At(2)->Fence == 11);

    /* 6-7: a flush and a GPU update in another buffer stamp as before: failure word 0 sets nothing, and the
     * update's own flags (segment bits, large page) do not read as failed. */
    memset(ptes, 0, sizeof(ptes));
    ptes[0].Valid = 1; ptes[0].Segment = 1; ptes[0].LargePage = 1;
    ptes[1].Valid = 1; ptes[1].Segment = 1;
    memset(&update, 0, sizeof(update));
    update.PageTableLevel = 1; update.StartIndex = 7; update.NumPageTableEntries = 3; update.pPageTableEntries = ptes;
    update.hProcess = (HANDLE)0xC0;
    PagingJournalNote(BC250_PJ_FLUSH_TLB, 0, NULL, 0, BUF + BUF_BYTES, 0, 0);
    PagingJournalUpdate(&update, 0, 3, BUF + BUF_BYTES + 0x40, FALSE);
    CHECK(At(6)->Flags == 0 && At(6)->Index == 0);
    CHECK((At(7)->Flags & BC250_PJ_FLAG_FAILED) == 0 && (At(7)->Flags & BC250_PJ_FLAG_LARGE) != 0);
    CHECK(At(7)->Valid == 2 && At(7)->Index == 7);
    PagingJournalStampFence(BUF + BUF_BYTES, BUF_BYTES, 20);
    PagingJournalStampSeq(20, 110);
    CHECK(At(6)->Fence == 20 && At(6)->Seq == 110 && At(7)->Fence == 20 && At(7)->Seq == 110);
    CHECK(At(1)->Fence == 0 && At(4)->Fence == 0);

    /* The live read (bc250kmd_cli journal) returns the failed records as they are. */
    CHECK(PagingJournalRead(0, page, BC250_PAGING_JOURNAL_MAX, &next, &total, &lost) == 8);
    CHECK(next == 8 && total == 8 && lost == 0);
    CHECK(page[1].Flags == BC250_PJ_FLAG_FAILED && page[1].Index == INVALID_PARAMETER && page[1].Fence == 0);
    CHECK(page[4].Flags == BC250_PJ_FLAG_FAILED && page[4].Index == DEVICE_NOT_READY && page[4].Seq == 0);
}

/* A crash of the test dies quietly into this console: no abort message box, no Windows Error Reporting dialog.
 * windows.h would collide with the mock types above, so SetErrorMode is declared here (WinBase.h). */
__declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int Mode);
#define SEM_FAILCRITICALERRORS 0x0001u
#define SEM_NOGPFAULTERRORBOX 0x0002u
#define SEM_NOOPENFILEERRORBOX 0x8000u

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not swallow the FAIL lines */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    FlagBit();
    FailedBuildThenReuse();
    printf("paging journal: %u checks, %u failures\n", checks, bad);
    return bad ? 1 : 0;
}
