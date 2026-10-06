# The paging journal (bc250kmd 0.7.180, identity 0.7.193.1)

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
| `gfx-submit` | one GFX IB reached the ring (0.7.193.1) | the IB1 GPU VA | the KMD context object | the page table root | |

An update record also carries the page table level, the entry index (StartIndex + slice start), the slice's entry
count and how many of those entries have the Windows Valid bit: a slice with `Valid 0` zeroes its entries, i.e. it
unmaps. The VA comes from `FirstPteVirtualAddress` (WDDM 2.0's field of the request) plus the slice's offset at that
level's span (4 KiB << 9L for this driver's 512-entry tables).

From 0.7.183.1 an update record's `Flags` bits 16-31 also name the segments its valid entries point into
(`DXGK_PTE.Segment`, `BC250_PJ_FLAG_SEGMENT`): bit 16 + s for segment s below 15, so bit 16 is system memory and
bit 17 segment 1, and bit 31 for any segment from 15 up. The CLI prints them as `seg 0,1`. A record of an older
driver, or one whose slice has no valid entry, has the bits clear. Record size and layout are unchanged.

## Identity (0.7.193.1, journal version 2)

Trial 245's 0x116 (`scratch\m15\game-recon\bsod-245\REPORT-245.md`, local) had the unmap and the destroy of
the faulting page in the journal and still could not name a cause: every record word that could have said who
asked for the operation was zero, and the pending GFX job named a sequence, a fence and a node but no context.
So the fields each kind left unused now carry identity. The record layout, size and field order do not move;
`g_PagingJournal.Version` goes to 2 so that a reader need not guess, and a version 1 ring is read exactly as
before.

| Kind | `Level` | `Index` | `Count` | `Valid` | `Dma` |
|---|---|---|---|---|---|
| `destroy` | the process that called DestroyAllocation | its thread | the process that created the allocation | the BC2A blob version (0: not a UMD allocation) | the BC2A `gem_flags` |
| `gfx-submit` | the scheduler node | the process that created the context | `BC250_PJ_CTX_UMD`, `_SYSTEM` | - | - |

An `update` record with no `hAllocation` - which is every unmap, and the only paging record 245 had for the
faulting page - puts `DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE.hProcess` where the allocation handle would be and
sets `BC250_PJ_FLAG_PROCESS` (bit 6, below KMD183's segment mask) to say so.

A `destroy` by a System worker means VidMm deferred the destroy; a destroy on the application's own submit
thread means the user-mode side released the memory itself. That difference is what separates "VidMm freed it
late" from "somebody freed memory a queued job still names", which is the open question of 245.

The `gfx-submit` record is written in `gfx.c SubmitIbLocked` once the IB is committed to the ring, so its
`Seq` is the same GFX sequence the KMD log and the dump's fence slots carry, and its `Fence` the OS
`SubmissionFenceId`. The context value is never dereferenced by anything: `wddm.c` logs one
`context %p pid ... umd ... system ...` line per context at CreateContext, and that is what the value is
matched against.

### Write rate and ring coverage

One record per GFX submit makes the journal's busiest writer the game, not VidMm. Measured on 245 (Witcher 3
DX12 LOW, native 1080p, GPU DWM; `d245.journal.json`, 1024 records over 10.723 s):

| | records/s |
|---|---|
| paging records, average over the 10.7 s window | 95.5 |
| paging records, worst 1 s inside it | 523 |
| paging records, worst 2 s inside it | 271/s (542 records) |
| GFX submits, the 20.8 ms before the hang (10 jobs) | about 480 |
| GFX submits at the game's frame rate (27.6 fps, several jobs a frame) | about 300 |

At 300 GFX submits a second the 1024-record ring holds about 1.8 s of a paging burst (271 + 300 = 571/s); at
the 480/s submit burst measured just before the hang it holds about 1.4 s. So the ring no longer guarantees the
last 2 s of a game under a paging burst, where before this revision it held 3.8 s of the same burst. 2048
records (147 KB of nonpaged image data instead of 74 KB) would restore 2 s at the 480/s submit rate; the cost
is that the two stamp walks, which run under the journal's spin lock at DISPATCH_LEVEL on every paging submit,
double their worst-case bound. The ring is left at 1024 here because the fault class this instrumentation is
for happens inside microseconds of its submit (245: 29 us after the previous job's end-of-pipe, 2.08 ms after
the destroy), well inside 1.4 s; the trade is recorded so that the next duration criterion can make it
knowingly.

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
- Flags: up to 0.7.183.1 every read was a `HardwareAccess` escape, for which dxgkrnl takes the adapter lock; a
  game's threads then waited behind each sampler read (WrResource waits readied by bc250kmd_cli on Witcher 3's main
  thread, 1.2-1.3 ms per frame, lab profile of 2026-10-01). From 0.7.184.1 the driver also answers
  `GET_PAGING_JOURNAL` and `GET_LOG` with `NoAdapterSynchronization` alone, in any power phase: both copy
  driver-image rings under their own spin locks and touch no hardware (`display.c`, `SoftwareReadEscape`). The CLI
  sends them that way and falls back to `HardwareAccess` against an older driver; `journal follow` counts the
  escapes of each kind in its final line. `log summary` keeps `HardwareAccess` for its first page, because the
  summary walks state a stop frees and reads display registers. `tools/win/bc250kmd_cli/test_escape_flags.py`
  keeps the driver's and the CLI's lists equal. Because the summary is a Level Two escape, nothing may ask for it
  on a repeating schedule: `tools/win/lab-runner/kmdlog-stream.ps1` reads the header line with `log 0`, which
  prints the same line from a `GET_LOG` page (it polled `log summary` every 500 ms until 0.7.213).
- From a dump: `scratch\m15\game-recon\bsod-analysis\pagingjournal.py` (local) reads `g_PagingJournal` through the
  build's map, checks `Magic`/`Version`/`EntryBytes`, and lists the records around a fault VA. From journal
  version 2 it also decodes the identity fields above, and `bc250kmd_cli journal` prints them at the end of the
  record's line.

## Costs and limits

- 74 KB of nonpaged image data; one spin lock acquisition per record and per stamp, each walk bounded by the ring
  (1024 records) at DISPATCH_LEVEL or below. Nothing is logged, no register is touched.
- A refused build whose buffer the OS never submitted keeps `Fence 0` until the next submission of the same buffer
  range claims it; the reader tells the two apart by time. A preempted paging packet resubmitted with its original
  fence keeps the first SDMA sequence recorded.
- The ring covers 1024 operations. Up to 0.7.192.1 that was 1-30 s of a game's paging traffic; from 0.7.193.1 the
  GFX submit record shares the ring and brings it down to about 1.4-1.8 s under a paging burst (see "Write rate
  and ring coverage" above). The faults this is for happen inside microseconds of their submit, well inside that.
- Allocation handles are the driver's own objects (`BC250_WDDM_OBJECT*`), not the UMD's D3DKMT handles; the UMD
  side correlates by VA.
