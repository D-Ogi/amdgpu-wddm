/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* KMD193 quality gate hang-witness (tools/quality/hang_witness.py): the two decode helpers the hang
 * instrumentation stands on, compiled from the driver's own headers and checked against the measured numbers of
 * trial 245 (scratch m15/game-recon/bsod-245/REPORT-245.md).
 *
 *   ih_fault.h         the UTCL2 fault vector: GPU VA, VMID, write, retry; the latched
 *                      GCVM_L2_PROTECTION_FAULT_STATUS fields; the per-second bucket that decides how often the
 *                      DPC logs and reads the latch.
 *   paging_identity.h  the journal identity packing: which record word each kind now carries, which is the one
 *                      thing both the driver, bc250kmd_cli and bsod-analysis/pagingjournal.py must agree on.
 */
#include <stdio.h>
#include <string.h>
#include "ih_fault.h"
#include "paging_identity.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void FaultVectorDecode(void)
{
    /* Trial 245: 225 no-retry read vectors on one page, src_data1 0x50, ring_id 0xDE, VMID 1, the faulting page
     * 0x1_1BC4_0000. src_data[0] is the page frame. */
    CHECK(Bc250IhFaultVa(0x11BC40u, 0x50u) == 0x11BC40000ull);
    CHECK(!Bc250IhFaultIsWrite(0x50u));
    CHECK(!Bc250IhFaultIsRetry(0x50u));
    /* The low four bits of src_data[1] are address bits 44-47, which the write and retry bits must not reach
     * into and which must not be lost. */
    CHECK(Bc250IhFaultVa(0u, 0xFu) == 0xF00000000000ull);
    CHECK(Bc250IhFaultVa(0xFFFFFFFFu, 0u) == 0xFFFFFFFFull << 12);
    CHECK(Bc250IhFaultVa(1u, 0xF0u) == 0x1000ull);          /* write|retry alone add nothing to the address */
    CHECK(Bc250IhFaultIsWrite(0x20u) && !Bc250IhFaultIsRetry(0x20u));
    CHECK(Bc250IhFaultIsRetry(0x80u) && !Bc250IhFaultIsWrite(0x80u));
    CHECK(Bc250IhFaultIsWrite(0xA1u) && Bc250IhFaultIsRetry(0xA1u));
    CHECK(Bc250IhFaultVa(0xA1u, 0xA1u) == ((0xA1ull << 12) | (1ull << 44)));
}

static void FaultStatusDecode(void)
{
    /* GCVM_L2_PROTECTION_FAULT_STATUS, gc_10_1_0_sh_mask.h: MORE_FAULTS bit 0, WALKER_ERROR 1-3,
     * PERMISSION_FAULTS 4-7, MAPPING_ERROR 8, CID 9-17, RW 18, VMID 20-23. */
    unsigned long status = 0x00100201u;         /* CID 1, RW 0, VMID 1, MORE_FAULTS 1 */
    CHECK(BC250_GCVM_FAULT_MORE(status) == 1u);
    CHECK(BC250_GCVM_FAULT_CID(status) == 1u);
    CHECK(BC250_GCVM_FAULT_VMID(status) == 1u);
    CHECK(BC250_GCVM_FAULT_RW(status) == 0u);
    status = 0x00141000u;                       /* CID 8 (TCP), RW 1, VMID 1 */
    CHECK(BC250_GCVM_FAULT_CID(status) == 8u);
    CHECK(BC250_GCVM_FAULT_RW(status) == 1u);
    CHECK(BC250_GCVM_FAULT_VMID(status) == 1u);
    CHECK(BC250_GCVM_FAULT_MORE(status) == 0u);
    status = 0x000001F0u;                       /* every permission bit, a mapping error, no CID */
    CHECK(BC250_GCVM_FAULT_PERMISSIONS(status) == 0xFu);
    CHECK(BC250_GCVM_FAULT_MAPPING(status) == 1u);
    CHECK(BC250_GCVM_FAULT_WALKER_ERROR(0x0000000Eu) == 7u);
    /* The CID names are the gfxhub's own table; an index past it is named, never read past. */
    CHECK(strcmp(Bc250GfxhubClientName(0u), "CB/DB") == 0);
    CHECK(strcmp(Bc250GfxhubClientName(8u), "TCP") == 0);
    CHECK(strcmp(Bc250GfxhubClientName(BC250_GFXHUB_CLIENT_COUNT - 1u), "SDMA3") == 0);
    CHECK(strcmp(Bc250GfxhubClientName(BC250_GFXHUB_CLIENT_COUNT), "unknown") == 0);
    CHECK(strcmp(Bc250GfxhubClientName(0x1FFu), "unknown") == 0);    /* the whole nine-bit field */
}

static void FaultBucket(void)
{
    BC250_IH_FAULT_STATE f;
    unsigned i;

    memset(&f, 0, sizeof(f));
    /* The first vector of a second opens a bucket and is reported; the rest of that second are counted only.
     * 245's burst was 225 vectors inside 114 microseconds: one report, one latch read. */
    CHECK(Bc250IhFaultNote(&f, 2084ull, 1u, 0xDEu, 0u, 0x11BC40u, 0x50u) == 1);
    for (i = 1; i < 225; i++)
        CHECK(Bc250IhFaultNote(&f, 2084ull, 1u, 0xDEu, 0u, 0x11BC40u, 0x50u) == 0);
    CHECK(f.Total == 225 && f.InSecond == 225 && f.Bursts == 1 && f.PreviousInSecond == 0);
    CHECK(f.FirstVa == 0x11BC40000ull && f.VmId == 1u && f.RingId == 0xDEu && f.SrcData1 == 0x50u);
    CHECK(!f.Write && !f.Retry);
    /* The next second reports again and carries what the one before it counted. */
    CHECK(Bc250IhFaultNote(&f, 2085ull, 2u, 0u, 0u, 0x20u, 0xA1u) == 1);
    CHECK(f.Total == 226 && f.InSecond == 1 && f.Bursts == 2 && f.PreviousInSecond == 225);
    CHECK(f.FirstVa == (0x20000ull | (1ull << 44)) && f.VmId == 2u && f.Write && f.Retry);
    /* A second that comes back (an interrupt-time reading that did not advance, or a bucket that reopens) is a
     * new bucket, not a silent merge: Second is compared, never ordered. */
    CHECK(Bc250IhFaultNote(&f, 2084ull, 1u, 0u, 0u, 1u, 0u) == 1);
    CHECK(f.Bursts == 3 && f.PreviousInSecond == 1 && f.InSecond == 1 && f.Total == 227);
    /* Second zero is a legal bucket key (the first second after a driver load). */
    memset(&f, 0, sizeof(f));
    CHECK(Bc250IhFaultNote(&f, 0ull, 0u, 0u, 0u, 0u, 0u) == 1);
    CHECK(Bc250IhFaultNote(&f, 0ull, 0u, 0u, 0u, 0u, 0u) == 0);
    CHECK(f.Bursts == 1 && f.Total == 2);
}

static void JournalKindEncoding(void)
{
    BC250_PAGING_JOURNAL_RECORD r;

    /* Record size and field order are version 1's: the identity rides in the words a kind left unused, and
     * nothing in the layout may move (paging_journal.h, the dump reader's struct.calcsize). */
    CHECK(sizeof(r) == 72);

    /* DESTROY. 245's release group: a 384 KiB VRAM allocation at 0x1_1BC4_0000, destroyed in the same
     * millisecond as the unmap of its pages. */
    memset(&r, 0, sizeof(r));
    r.Kind = BC250_PJ_DESTROY_ALLOCATION;
    r.Va = 0x11BC40000ull;
    r.Offset = 393216ull;
    r.Flags = BC250_PJ_FLAG_UMD_ALLOCATION;
    Bc250PjDestroyIdentity(&r, 11324u, 7036u, 11324u, 2u, 0x4ull);
    CHECK(r.Level == 11324u && r.Index == 7036u && r.Count == 11324u && r.Valid == 2u && r.Dma == 0x4ull);
    CHECK(r.Kind == BC250_PJ_DESTROY_ALLOCATION && r.Va == 0x11BC40000ull && r.Offset == 393216ull);
    CHECK(r.Flags == BC250_PJ_FLAG_UMD_ALLOCATION);     /* identity sets no flag of its own */
    CHECK(r.Fence == 0 && r.Seq == 0);

    /* An UPDATE with an allocation keeps it and stays unflagged; an unmap (no allocation) takes hProcess. */
    memset(&r, 0, sizeof(r));
    r.Kind = BC250_PJ_UPDATE_GPU;
    r.Allocation = 0xFFFFC00Faac70ull;
    Bc250PjUpdateProcess(&r, 0xFFFFC00F11110ull);
    CHECK(r.Allocation == 0xFFFFC00Faac70ull && (r.Flags & BC250_PJ_FLAG_PROCESS) == 0);
    memset(&r, 0, sizeof(r));
    r.Kind = BC250_PJ_UPDATE_GPU;
    Bc250PjUpdateProcess(&r, 0xFFFFC00F11110ull);
    CHECK(r.Allocation == 0xFFFFC00F11110ull && (r.Flags & BC250_PJ_FLAG_PROCESS) != 0);
    /* A null hProcess leaves the record as it was rather than flagging a zero. */
    memset(&r, 0, sizeof(r));
    r.Kind = BC250_PJ_UPDATE_GPU;
    Bc250PjUpdateProcess(&r, 0ull);
    CHECK(r.Allocation == 0 && r.Flags == 0);

    /* GFX_SUBMIT: 245's last game job, seq 604859, OS fence 53276, IB1 0x2_00B2_0000, root 0x46DFE1000. */
    memset(&r, 0, sizeof(r));
    Bc250PjGfxSubmit(&r, 604859u, 53276u, 0x200B20000ull, 0x46DFE1000ull, 0xFFFFC00Fae010ull, 0u, 11324u,
                     BC250_PJ_CTX_UMD, 1u);
    CHECK(r.Kind == BC250_PJ_GFX_SUBMIT && r.Kind == 9u);
    CHECK(r.Seq == 604859u && r.Fence == 53276u);
    CHECK(r.Va == 0x200B20000ull && r.Offset == 0x46DFE1000ull && r.Allocation == 0xFFFFC00Fae010ull);
    CHECK(r.Level == 0u && r.Index == 11324u && r.Count == BC250_PJ_CTX_UMD);
    CHECK(r.Valid == 1u && r.Dma == 0ull);              /* KMD214: Valid the VMID; a stamp walk must see Dma zero */
    Bc250PjGfxSubmit(&r, 1u, 2u, 3u, 4u, 5u, 1u, 6u, BC250_PJ_CTX_UMD | BC250_PJ_CTX_SYSTEM, 15u);
    CHECK(r.Level == 1u && r.Count == 3u && r.Valid == 15u);

    /* The kind numbers are an on-disk contract with two readers outside this repository's build. */
    CHECK(BC250_PJ_UPDATE_CPU == 1u && BC250_PJ_UPDATE_GPU == 2u && BC250_PJ_VIRTUAL_FILL == 3u);
    CHECK(BC250_PJ_VIRTUAL_TRANSFER == 4u && BC250_PJ_FLUSH_TLB == 5u && BC250_PJ_DESTROY_ALLOCATION == 6u);
    CHECK(BC250_PJ_TRANSFER == 7u && BC250_PJ_FILL == 8u && BC250_PJ_GFX_SUBMIT == 9u);
    CHECK(BC250_PJ_FLAG_PROCESS == 64u);                /* below the segment mask of KMD183 */
    CHECK((BC250_PJ_FLAG_PROCESS & BC250_PJ_FLAG_SEGMENT_MASK) == 0u);
    CHECK(BC250_PJ_CTX_UMD == 1u && BC250_PJ_CTX_SYSTEM == 2u);
}

int main(void)
{
    FaultVectorDecode();
    FaultStatusDecode();
    FaultBucket();
    JournalKindEncoding();
    if (failures != 0) { printf("FAIL: %d check(s)\n", failures); return 1; }
    puts("PASS: fault VA and status decode, gfxhub CID names, per-second bucket, journal kind encoding");
    return 0;
}
