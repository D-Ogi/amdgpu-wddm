/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_PAGING_IDENTITY_H
#define BC250_PAGING_IDENTITY_H
// KMD193: who asked for a paging operation and who submitted a job, packed into the fields each journal kind
// already left unused (bc250kmd_escape.h, BC250_PJ_FLAG_PROCESS and the DESTROY/GFX_SUBMIT notes). The record
// layout does not move: 245's dump had the unmap and the destroy of the faulting page and still could not name
// a process, a thread or a context, because those words were zero (scratch game-recon bsod-245 item 3/4).
//
// Header-only and WDK-free so that the writers here are the same code the host test runs
// (driver/kmd/test/ih_fault_test.c, quality gate ih-fault) and the same meanings the dump reader
// (bsod-analysis/pagingjournal.py) and `bc250kmd_cli journal` print.
#include "bc250kmd_escape.h"

// DestroyAllocation. Creator and Version are what CreateAllocation kept on the object; Process and Thread are
// the caller of the destroy, which is the interesting one: a VidMm-deferred destroy runs on a System worker and
// a release straight off the application's submit thread does not.
static __inline void Bc250PjDestroyIdentity(BC250_PAGING_JOURNAL_RECORD* Record, unsigned long Process,
                                            unsigned long Thread, unsigned long Creator, unsigned long Version,
                                            unsigned long long GemFlags)
{
    Record->Level = Process;
    Record->Index = Thread;
    Record->Count = Creator;
    Record->Valid = Version;
    Record->Dma = GemFlags;
}

// An UPDATE_PAGE_TABLE slice with no hAllocation - which is every unmap, and the only paging record 245 had for
// the faulting page - puts hProcess where the allocation handle would be, and says so in Flags.
static __inline void Bc250PjUpdateProcess(BC250_PAGING_JOURNAL_RECORD* Record, unsigned long long Process)
{
    if (Record->Allocation != 0 || Process == 0) return;
    Record->Allocation = Process;
    Record->Flags |= BC250_PJ_FLAG_PROCESS;
}

// One GFX IB on the ring. Allocation is the submitting KMD context object as a value, never dereferenced: by
// the time a dump is read the object may be freed, and the point is only to tell two contexts apart and to
// match the line against the context's own CreateContext log. Valid is the VMID the IB ran at (KMD214; 0 in
// records of earlier drivers, which ran every WDDM job at VMID 1).
static __inline void Bc250PjGfxSubmit(BC250_PAGING_JOURNAL_RECORD* Record, unsigned long Seq, unsigned long Fence,
                                      unsigned long long Ib1, unsigned long long Root, unsigned long long Context,
                                      unsigned long Node, unsigned long Process, unsigned long ContextFlags,
                                      unsigned long Vmid)
{
    Record->Kind = BC250_PJ_GFX_SUBMIT;
    Record->Seq = Seq;
    Record->Fence = Fence;
    Record->Va = Ib1;
    Record->Offset = Root;
    Record->Allocation = Context;
    Record->Level = Node;
    Record->Index = Process;
    Record->Count = ContextFlags;
    Record->Valid = Vmid;
}

#endif
