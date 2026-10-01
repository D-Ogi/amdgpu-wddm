# The paging journal (bc250kmd 0.7.180)

Why: the 0x116 hangs of native D3D12 game trials 118, 119 and 151 (`scratch\m15\game-recon\bsod-151\REPORT-151.md`,
local) share one shape. A GFX job takes no-retry page faults on 65 contiguous pages of runtime-backed D3D12 memory
0.5-1.6 ms after its submission, 0.3 ms after a paging buffer retired. The kernel dump held the SDMA ring, the IH
ring and the log, but not the one thing that names the cause: which paging operation touched those pages, on
whose allocation, and whether VidMm was evicting, the UMD was freeing, or the game was destroying. The KMD log
cannot carry that: a paging burst is 170 operations in 0.2 s and the log ring is 1024 lines.

## What it records

`driver/kmd/paging_journal.c` keeps `g_PagingJournal`, a fixed-layout ring of 1024 records (72 bytes each,
`BC250_PAGING_JOURNAL_RECORD` in `bc250kmd_escape.h`) in the driver image's nonpaged data, so a kernel dump holds it
whole and a map symbol finds it. One record per:

| Kind | When | Va | Allocation | Offset | Flags |
|---|---|---|---|---|---|
| `update-cpu` | UPDATE_PAGE_TABLE, CPU_VIRTUAL (the paging process, immediate) | first entry's GPU VA | hAllocation | AllocationOffsetInBytes | Repeat, InitialUpdate, NotifyEviction, Use64KBPages |
| `update-gpu` | one BuildPagingBuffer slice of a GPU_PHYSICAL update | same | same | same | same |
| `vfill` | VIRTUAL_FILL built | destination VA | hAllocation | bytes moved | |
| `vtransfer` | VIRTUAL_TRANSFER built | source VA | hAllocation | bytes moved | TO_SYSTEM for local-to-system |
| `flush-tlb` | FLUSH_TLB built | 0 | 0 | 0 | |
| `destroy` | DestroyAllocation | the UMD's requested VA | the handle | UmdBytes or Size | UMD_ALLOCATION |
| `transfer`, `fill` | physical TRANSFER / FILL built | 0 | hAllocation | bytes moved | |

An update record also carries the page table level, the entry index (StartIndex + slice start), the slice's entry
count and how many of those entries have the Windows Valid bit: a slice with `Valid 0` zeroes its entries, i.e. it
unmaps. The VA comes from `FirstPteVirtualAddress` (WDDM 2.0's field of the request) plus the slice's offset at that
level's span (4 KiB << 9L for this driver's 512-entry tables).

From 0.7.183.1 an update record's `Flags` bits 16-31 also name the segments its valid entries point into
(`DXGK_PTE.Segment`, `BC250_PJ_FLAG_SEGMENT`): bit 16 + s for segment s below 15, so bit 16 is system memory and
bit 17 segment 1, and bit 31 for any segment from 15 up. The CLI prints them as `seg 0,1`. A record of an older
driver, or one whose slice has no valid entry, has the bits clear. Record size and layout are unchanged.

Each GPU-path record stores its position in the paging buffer (`DmaBufferGpuVirtualAddress + DmaBufferWriteOffset`
at the build). SubmitCommand on the paging node stamps the OS `SubmissionFenceId` into the records of the submitted
range (`PagingJournalStampFence`, walking back and stopping at the previous submission of a reused buffer), and the
paging submit stamps the SDMA sequence by that fence (`PagingJournalStampSeq`, under `wddm->Lock`). So a record
reads: this slice unmapped VA X of allocation A at time T, went to the GPU in the paging buffer with fence F as
SDMA sequence S. The SDMA ring walk of the dump reader (`sdmawalk.py`) and the log's "paging submit ... seq" lines
name the same S.

Times are `KeQueryInterruptTime()` taken under the journal's spin lock, so they rise with record indices; the log's
`Milliseconds` are `(Time - GuardInit's start) / 10000`.

## How it is read

- Live: `bc250kmd_cli journal [from]` (`BC250_ESCAPE_GET_PAGING_JOURNAL`, administrators, 64 records per escape,
  admitted under full WDDM as an observational command). The CLI prints one line per record and follows `Next`.
  `bc250kmd_cli journal follow SECONDS [MS]` is the sampler for a trial: one process, one adapter handle, one
  escape per interval, printing the records added since the previous read. It replaced a loop that spawned the
  CLI once a second: next to the desktop's present heartbeat that loop left the lab's sshd accepting no new
  connection for as long as it ran (2026-09-30), while a single long process never did. Up to 0.7.182.1 the
  follow mode still found and opened the adapter for every read; from 0.7.183.1 it opens it once and holds it.
  A failed read drops the handle and the next interval reopens it, so a PnP disable/enable of the adapter no
  longer ends the sampler, and a journal whose total fell below the cursor (a reloaded driver) is read again
  from its oldest record.
- From a dump: `scratch\m15\game-recon\bsod-analysis\pagingjournal.py` (local) reads `g_PagingJournal` through the
  build's map, checks `Magic`/`Version`/`EntryBytes`, and lists the records around a fault VA.

## Costs and limits

- 74 KB of nonpaged image data; one spin lock acquisition per record and per stamp, each walk bounded by the ring
  (1024 records) at DISPATCH_LEVEL or below. Nothing is logged, no register is touched.
- A refused build whose buffer the OS never submitted keeps `Fence 0` until the next submission of the same buffer
  range claims it; the reader tells the two apart by time. A preempted paging packet resubmitted with its original
  fence keeps the first SDMA sequence recorded.
- The ring covers 1024 operations: 1-30 s of a game's paging traffic. The fault this is for happened 0.3 ms after
  the last paging retirement, well inside that.
- Allocation handles are the driver's own objects (`BC250_WDDM_OBJECT*`), not the UMD's D3DKMT handles; the UMD
  side correlates by VA.
