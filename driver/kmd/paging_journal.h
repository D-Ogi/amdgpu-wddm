#pragma once
// The paging journal: the last BC250_PAGING_JOURNAL_ENTRIES page table updates, virtual fills and transfers,
// TLB flushes and allocation destructions VidMm asked this driver for, in one fixed-layout nonpaged ring
// (g_PagingJournal) that a kernel dump reader finds through the map symbol and that `bc250kmd_cli journal`
// reads live through BC250_ESCAPE_GET_PAGING_JOURNAL (docs/design/paging-journal.md). Records are written by
// BuildPagingBuffer (PASSIVE_LEVEL) and DestroyAllocation, stamped with the OS fence by SubmitCommand and with
// the SDMA sequence by the paging submit under wddm->Lock (DISPATCH_LEVEL). The journal's spin lock is a leaf:
// nothing is taken under it, and every walk is bounded by the ring.
#include "bc250kmd_escape.h"

#define BC250_PAGING_JOURNAL_MAGIC 0x4E524A50u      // "PJRN"
// Version 2 (KMD 0.7.193.1): the record layout and size are version 1's exactly; what changed is that the
// fields DESTROY, UPDATE and the new GFX_SUBMIT kind used to leave zero now carry process, thread and context
// identity (bc250kmd_escape.h, BC250_PJ_FLAG_PROCESS). A reader of version 1 decodes a version 2 ring correctly
// and simply sees nothing in those words; a reader must not read identity out of a version 1 ring.
#define BC250_PAGING_JOURNAL_VERSION 2u
#define BC250_PAGING_JOURNAL_ENTRIES 1024u

typedef struct _BC250_PAGING_JOURNAL {
    unsigned long Magic, Version;                   // BC250_PAGING_JOURNAL_MAGIC, BC250_PAGING_JOURNAL_VERSION
    unsigned long EntryBytes, Entries;              // sizeof(BC250_PAGING_JOURNAL_RECORD), BC250_PAGING_JOURNAL_ENTRIES
    unsigned long long Next;                        // records written since DriverEntry; record n is Entry[n % Entries]
    unsigned long long Reserved[3];
    BC250_PAGING_JOURNAL_RECORD Entry[BC250_PAGING_JOURNAL_ENTRIES];
} BC250_PAGING_JOURNAL;
// The dump reader (scratch bsod-analysis pagingjournal.py) relies on this offset and on the record layout of
// bc250kmd_escape.h; both are checked at compile time, not documented by hand.
typedef char BC250_PAGING_JOURNAL_ENTRY_OFFSET_CHECK[(FIELD_OFFSET(BC250_PAGING_JOURNAL, Entry) == 48) ? 1 : -1];
