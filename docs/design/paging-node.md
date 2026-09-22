# Stage D design note: the paging node on SDMA0 (ADR 0008 stage D, ADR 0013)

Read-only survey plus the decisions taken from it, 2026-09-22, against bc250kmd 0.7.22. Every claim carries a
`file:line`; lines marked **DECISION** are this note's own choice, not something measured. Style follows
`P:\BC-250\scratch\tmp\stageC_design.md`, stage C's own brief.

## 1. What ADR 0013 asks for and what is already true

- Two nodes: node 0 `DXGK_ENGINE_TYPE_3D` (unchanged), node 1 `DXGK_ENGINE_TYPE_COPY` on SDMA0, the paging node
  (ADR 0013 points 1-2).
- Point 4's precondition - SDMA's seven defects closed and its trap identified on the IH ring before node 1 is
  reported - is already met: facts M59/M60 closed the ring-pointer defects, M95 is the positive control for
  `SDMA_OP_COPY`/`COPY_LINEAR`/`CONST_FILL` on this hardware, and the trap vector is `client_id 8` (SDMA0) /
  `src_id 224` (`bc250_sdma.h:75-80`, facts M59). Nothing new has to be measured to open the gate; only the
  table has to learn to report it.
- Everything stays byte-for-byte identical with `EnablePagingNode` closed: node 1 is never named in caps, never
  created, never submitted to. The new code paths are additions, not edits to node 0's, wherever that is
  possible (section 4).

## 2. The VMID question, decided against two measured facts and no new ones

ADR 0013 point 2 and the task both ask directly: does the paging node need a VMID pointed at the paging
process's page tables, the way stage C points VMID 1 at a context's root before a gfx-ring IB (facts M77,
`wddm.c:463-473`)?

**Decision: no. The paging node's SDMA submissions carry only physical (MC) addresses and never reprogram a
VMID.** Two already-measured facts make this possible instead of a guess:

1. `VidMmTranslate()` (`driver/kmd/vidmm.c:229-268`) is a working, measured, CPU-side software walk of VidMm's
   own page tables (stage B, facts M73): given a root page table's physical address and a GPU virtual address,
   it returns the physical address the GPU's own walker would have found, and whether the page is VRAM or
   system memory. E20's diagnostic Blt already uses this exact mechanism to read a surface by its GPU VA
   (`wddm.c:1903-1911`) without ever pointing a VMID at anything.
2. `bc250_sdma_emit_copy_linear`/`emit_fill` (`bc250_sdma.h:136-146`) take physical MC addresses, not GPU
   virtual ones, and M95 measured them working on this hardware in exactly that form.

Chaining the two - resolve the operation's virtual addresses to physical with `VidMmTranslate`, then emit the
same physical-address packets M95 already proved - reuses two independent, already-measured facts. The
alternative the task also asks about (fetch the paging buffer as an SDMA IB via `SDMA_OP_INDIRECT`, in which
the VMID field of `SDMA_PKT_INDIRECT_HEADER` (`reference/sdma_v5_0.c:448-449`) selects the page tables the
copy/fill packets *inside* the IB are translated through) would additionally need: a VMID assigned to the
paging context, `bc250_gmc_set_vmid_pd()` pointed at its root (stage C's own mechanism, itself only proven for
a PM4 NOP payload whose *content* left no trace it was actually fetched through the VMID - M77's own stated
limit), and - unmeasured on this ASIC - that `SDMA_OP_COPY`/`SDMA_OP_CONST_FILL` packets *inside* a
VMID-fetched IB have their own `src_addr`/`dst_addr` fields translated through that same VMID rather than read
as physical. Hard rule 5 ("positive control first") and rule 4 (no "impossible" or "definitive" without an
evidence id - the converse holds for "works" too) both point the same way: build the cut that reuses two
measured facts, not the one that bets on a third, unmeasured one. The IB/VMID route stays available as a later
upgrade with its own positive control (E24's H4, section 8) if the physical-only cut turns out to be too slow
or too narrow once it is running.

One consequence: `hSystemContext` (`DXGKARG_BUILDPAGINGBUFFER::hSystemContext`, d3dkmddi.h:5069) is still
needed, but only to find *which root* to resolve virtual addresses against - it is the same context object
`SetRootPageTable` already records a `RootPhysical` for (`wddm.c:1416-1432`), and facts M72 already shows it
being bootstrapped first ("the page tables of the system paging process ... before `SetRootPageTable`
1:0x0"). `WddmObject(pBuildPagingBuffer->hSystemContext, BC250_WDDM_MAGIC_CONTEXT)` finds the same object
`Bc250WddmSetRootPageTable` filled in, with no new bookkeeping.

**Limit, stated rather than hidden**: an operation whose source or destination resolves to *system* memory
(`VidMmTranslate`'s `System` out-parameter true), or that fails to translate at all, is answered inertly -
zero bytes written to `pDmaBuffer`, `STATUS_SUCCESS` - exactly as stage A answered every paging operation, and
counted separately from operations that were built. This is deliberate: this driver's GART maps host pages
into MC space only for allocations *this driver* made through `bc250_shim_mem_alloc(BC250_MEM_GTT, ...)`
(`bc250_shim.h:31-38`, used throughout `gfx.c`/`ih.c`); it has no mechanism to map an arbitrary allocation
VidMm resolved to system memory into MC space on demand, and building one is out of scope for a first cut.
E24's exit criterion (section 8) only needs local-to-local transfers - the CDD's own surfaces, VRAM-resident by
`wddm.c:1541-1543` (`SupportedReadSegmentSet`/`WriteSegmentSet` name the local segment only) - so this limit
does not block it.

## 3. IB versus writing straight into the ring

**Decision: write straight into SDMA0's live ring, never through `SDMA_OP_INDIRECT`.** Reasons, in order:

1. Avoiding VMID entirely (section 2) removes the one reason `amdgpu_ttm_copy_mem_to_mem` needs an IB on
   Linux: there, a TTM/scheduler job is a first-class object with its own fence, dependencies and possible
   requeueing across rings, and an IB is how such a job is handed to a ring without the CPU blocking on
   allocation. This driver has none of that (ADR 0012, the scheduler half, is explicitly later work); a
   paging submission here is a single, synchronous, one-in-flight push, indistinguishable in shape from the
   ring-test IB stage C already writes straight into a page (`bc250_gfx_ib_ring_test_build`,
   `bc250_gfx.c:1689-1712`) - except that one *is* fetched indirectly, for a reason that does not apply here:
   it exists to prove `PACKET3_INDIRECT_BUFFER` fetches correctly, which is not this node's question.
2. dxgkrnl's paging buffer is **system memory** (`PagingBufferSegmentId = 0`, `wddm.c:1049`), which this
   driver has never given a GART/MC mapping of its own; doing so on demand for a buffer VidMm - not this
   driver - allocated would be new GART-management surface for no measured benefit.
3. The buffer is small (`BC250_WDDM_PAGING_BUFFER_BYTES = 0x10000`, `wddm.c:57`) and, per the one-in-flight
   rule of section 5, at most one batch is ever outstanding: the SDMA0 ring (`BC250_SDMA_RING_SIZE`, see
   `bc250_sdma.c`) has ample room for it directly, the same way stage C's IB already fits inside the gfx
   ring's budget (`stageC_design.md` section 3.4).

## 4. How `pDmaBuffer`'s bytes become a ring push (the DISPATCH_LEVEL wall)

This is the part that is not obvious from the DDI names, and it decided most of the rest of this note.

- `DXGKDDI_BUILDPAGINGBUFFER` and `DXGKDDI_SETROOTPAGETABLE` are `_IRQL_requires_(PASSIVE_LEVEL)`
  (d3dkmddi.h:5084, :5125) - the same level `SubmitCommandVirtual` runs at, which is why stage C's
  `GfxSubmitIb` can take `Device->GartLock` (a `FAST_MUTEX`).
- `DXGKDDI_SUBMITCOMMAND` - the DDI a paging buffer is submitted through, `hContext == NULL`,
  `NodeOrdinal` naming the node (`wddm.c:1694-1695`, already true for node 0 today) - is
  `_IRQL_requires_(DISPATCH_LEVEL)` (d3dkmddi.h:4385), **exactly**, not `<=`. `GartLock` cannot be taken there;
  neither can `GartDevice()`, `bc250_gmc_flush_gpu_tlb()`'s poll, or anything the sequence/logging wrapper of
  `sequence.c` does. This is the same wall stage C's design brief names for why `SubmitCommand` keeps the
  software-only path (`stageC_design.md` section 3.1) - stage D is the first time this driver has to cross it
  for real hardware work.

So two problems, not one: **(a)** BuildPagingBuffer (PASSIVE_LEVEL) has to turn operation parameters into
packet dwords and put them somewhere SubmitCommand (DISPATCH_LEVEL) can find them without dereferencing a
physical address (no `MmMapIoSpaceEx` at DISPATCH_LEVEL) or touching anything GartLock-shaped; **(b)**
SubmitCommand has to push those dwords onto the real SDMA0 ring and ring a doorbell without any of the
primitives every other ring write in this driver uses.

**(a) - a shadow buffer.** BuildPagingBuffer writes the packet dwords into `pDmaBuffer` (the documented
contract: VidMm owns that buffer and may inspect it) *and*, at the same offset
(`DXGKARG_BUILDPAGINGBUFFER::DmaBufferWriteOffset`, available from interface `0x5012`, which this driver is
compiled above), into a second, driver-owned buffer that stays mapped for the life of the device start - a
GTT allocation from the same pool `bc250_gfx_fence_page_alloc()` and friends already use. `SubmitCommand`
later reads `[StartOffset, EndOffset)` out of *that* buffer, never out of `DmaBufferPhysicalAddress`: the two
coordinate spaces are the same (dxgkrnl accumulates one buffer across many `BuildPagingBuffer` calls before
one `SubmitCommand` flushes it, and both offsets are measured from the same buffer start), and the shadow is
already a CPU pointer, no mapping needed at DISPATCH_LEVEL.

**Corrected by E24 run 006 (facts M110), and the correction is smaller than it looks.** The shadow, the shared
coordinate space and the accumulation across calls are all as described - run 006 measured dxgkrnl packing four
fills into one buffer at 0x0, 0x140, 0x640, 0xB40 (M111). What is wrong above is the name of the door the flush
comes through. VidMm creates node 1's paging system context with `DXGK_CREATECONTEXTFLAGS::VirtualAddressing`
set, and a context that addresses virtually is submitted through `DxgkDdiSubmitCommandVirtual`; this driver saw
`DxgkDdiSubmitCommand` called exactly zero times in 110 seconds against 1581 `BuildPagingBuffer` calls.
`DXGKARG_SUBMITCOMMANDVIRTUAL` has no `DmaBufferSubmissionStartOffset` at all - it names the submission by
`DmaBufferVirtualAddress`, a GPU address - so the offset into the shadow is recovered by subtracting
`DXGKARG_BUILDPAGINGBUFFER::DmaBufferGpuVirtualAddress` (`d3dkmddi.h:5071`, "GPU virtual address of the start of
the DMA buffer"), recorded on every call that writes bytes. Everything downstream of that subtraction - the
shadow, `GfxSubmitPaging`, the fence, the IH DPC - is unchanged.

**Corrected again by E24 run 007 (facts M112).** On this build that GPU address is 0 for the system paging
buffer, and the submission's `DmaBufferVirtualAddress` is already the byte offset the shadow is indexed by:
the three submissions named 0x0, 0x140 and 0x640, which are the shadow offsets of the fills they covered.
Treating 0 as "no buffer" dropped the high-water mark on the floor and completed every one of them in software.
A recorded base of 0 means the coordinate is the offset. A non-zero base still subtracts; this machine has not
produced one.

The subtraction itself is the one thing that must never be guessed, because a wrong offset here does not draw a
wrong picture, it runs a wrong DMA. Two properties hold it down. **One tracked buffer, not several.** The first
version of this tracked four, on the reasoning that nothing obliges VidMm to use one paging buffer at a time -
which is true, and beside the point: the shadow above is a single buffer indexed by `DmaBufferWriteOffset`, so
two paging buffers being built at once write over each other at the same offsets. Tracking four would have been
a claim to know where four buffers' packets are while the shadow can hold one, and the failure mode is handing
SDMA0 another buffer's bytes with a matching address to vouch for them. So the driver tracks the buffer whose
packets are in the shadow now, and a submission naming any other is completed in software and counted. If that
counter is ever non-zero, the answer is a shadow per buffer, not a longer table. **A high-water mark.**
`DXGKARG_SUBMITCOMMANDVIRTUAL::DmaBufferSize` may name the whole buffer rather than the submitted length - it
has no other field that could - so the driver records how far into the buffer it has actually written packets
and cuts a submission back to it, resetting the mark whenever the buffer changes or dxgkrnl restarts one at
offset 0. The invariant is the short one: **no byte reaches SDMA0 that this driver did not put in the shadow for
the buffer being submitted.**

The address and the mark are one value in two words, and they are only ever touched together under the WDDM
lock. The first version of this kept them as two interlocked singles, and review 24 found both halves of what
that always costs: a raise that lands after another builder's reset stamps one buffer's mark onto the other's
address and the mismatched pair then *stays*, and a reader that takes the address before a switch and the mark
after it gets a pair that never existed. Neither is a stale read a range check can catch - both look like a
clean match and put another buffer's packets on a live ring. `gfx.c`'s own `PagingBuildersActive` says two
builders can genuinely run at once, so this is not a theoretical interleaving. There was never anything to win
by being lock-free on a path that runs a few times a second.

One thing the DDI side still does not do: `Bc250WddmSubmitCommand`'s own node-1 branch uses dxgkrnl's
`DmaBufferSubmissionStartOffset`/`EndOffset` directly and has no buffer identity of its own. It is unreachable
on this machine (M110) and correct on its own terms, since those offsets are that DDI's own coordinates - but
if it ever becomes reachable it reaches `WddmSubmitPagingHardware` without the check above, and that is the
first thing to revisit.

Packets are emitted into the shadow through the *existing, measured* emitters, unmodified: a small, throwaway
`struct amdgpu_ring` is built on the stack, pointed at the shadow buffer (`.ring = shadow + offset/4`,
`.buf_mask`/`.max_dw` sized to the room left in the *dxgkrnl* buffer so `amdgpu_ring_alloc`'s own overflow
check - `ndw > ring->max_dw` (`bc250_ring.c:26`) - becomes exactly the `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER`
answer the task asks for, with `MultipassOffset` set to the offset that failed), and `bc250_sdma_emit_copy_linear`/
`emit_fill` are called on it exactly as `SdmaCopyEscape` already calls them on a real ring (`gfx.c:900-901`).
`amdgpu_ring_commit` is never called on this fake ring - there is nothing to commit, no doorbell to ring, and
calling it would try to ring one.

The ring is throwaway; the **device behind it is not**. The first cut of this gave the throwaway ring a
throwaway `struct amdgpu_device` as well, declared as a local next to it, and that was two defects in one line.
The fatal one is size: the struct is 0x5B00 bytes, an x64 kernel thread's whole stack is 24 KB, and dxgmms2 has
already spent some of it by the time it calls `DdiBuildPagingBuffer` - so the first `VIRTUAL_FILL` VidMm ever
sent us bugchecked unit A with 0x50 in `nt!_chkstk` (facts M104, E24 run 004). The quieter one is that the local
was never initialised, so `ring->adev->dev` - which `bc250_ring.c` passes to `dev_err` on every refusal path -
was whatever the stack held. Both go away with the same change: `bc250_sdma_paging_copy/fill` take the caller's
live `adev`, which `GfxPagingBuild` has in `Gfx->PagingDevicePtr`, and refuse a NULL one. Nothing on this path
writes through the device, touches a register or rings a doorbell; it is read for `dev_err` and NULL-checked by
`bc250_sdma_copy.c`, and that is all. `tools/win/stackbudget.py` now fails any build whose fixed frames reach
4 KB, so the size half cannot come back quietly.

**(b) - a dedicated, DISPATCH_LEVEL-safe push, guarded by a spinlock instead of `GartLock`.** The ring write
path itself (`amdgpu_ring_alloc`/`_write`/`_write_multiple`, `bc250_ring.c`) is pure memory and does not need
PASSIVE_LEVEL; `amdgpu_ring_commit()`'s doorbell write does, indirectly, because it reaches
`bc250_shim_wdoorbell64()` through `ring->adev->backend` (`gpumem.c:406-426`), a plain pointer field every
GartLock-protected call swaps in and out for its own duration (`gfx.c:516-517` and everywhere else) - not
something a DISPATCH_LEVEL caller may read or fight over.

So node 1's submit path does not call `amdgpu_ring_commit()`. It calls `amdgpu_ring_alloc` +
`amdgpu_ring_write_multiple` (the shadow's dwords) + `bc250_sdma_emit_fence` on the **live**
`adev->sdma.instance[0].ring` (the same struct every SDMA0 escape already writes through - not a copy: a copy
would fork `.wptr` into two counters describing one ring, which is worse than the problem it would solve), then
publishes the write-back slot and rings the doorbell itself, through two new primitives that need no `adev`/
`backend` at all:

- `GpuMemDoorbellWrite(Device, index, value)` (`gpumem.c`, new): the same one 64-bit store
  `bc250_shim_wdoorbell64` makes into `Device->GpuMem`'s mapped doorbell page, reached directly from
  `Device`, not through an `amdgpu_device`'s `backend`. DISPATCH_LEVEL safe: plain mapped memory, one
  interlocked counter.
- the write-back slot: `*ring->wptr_cpu_addr = wptr_bytes` - already a plain volatile store,
  `bc250_ring.c:115-116`, unchanged.

A new spinlock, `BC250_GFX::Sdma0RingLock`, is the only new synchronization primitive. It is held narrowly,
around the read-modify-write of `ring->wptr`/`count_dw` and the wptr-slot-plus-doorbell publish, by **every**
writer of the SDMA0 ring: the ring test and `GfxFenceEscape`'s SDMA arm, `SdmaCopyEscape`, and node 1's new
path. The first three already serialize against each other and against everything else through `GartLock`
(a much bigger critical section, held for the whole escape); they only need the extra, narrow lock because
node 1's DISPATCH_LEVEL path cannot take `GartLock` at all and SMP means "one CPU at a time" is not implied by
IRQL alone. Taking a spinlock from inside an already-`GartLock`-held, `<= APC_LEVEL` call is ordinary Windows
practice (`StatsLock` in `ih.c` is the same shape: "held over memory only: never across a register access").

`GfxSubmitReady()`'s equivalent for node 1 needs its own `struct amdgpu_device*`/`struct amdgpu_ring*` pair
that is safe to dereference at DISPATCH_LEVEL without `GartDevice()`. This is the same problem `ih.c` already
solved for its DPC (`ih->DpcAdev`, a device-lifetime copy made once at `ih init`, read thereafter with no
lock because nothing but the DPC ever touches it, `ih.c:319-324`) - reused here for the *pointer*, not a
struct copy: `Gfx->PagingRing = &adev->sdma.instance[0].ring` and `Gfx->PagingDevicePtr = adev`, captured once
(under `GartLock`, inside `GfxEscape`'s `RUN` arm, the first time stage 8 completes with the
`EnablePagingNode` gate open - mirroring exactly where `ih->DpcAdev` is filled in). `adev` itself is the
persistent structure `GartDevice()` sets up once per device start (`gart.c`'s comment: "set up if it was not
yet"), not something recreated per call, so the pointer stays valid for the life of the device start; `pnp.c`'s
stop order (`WddmStop` before `GfxStop`, drained the same way stage C's watchdog already is) keeps it that way.

## 5. Fences, one-in-flight, and the IH DPC

Node 1 gets its **own** hardware channel, parallel to node 0's stage C one (`wddm.c`'s `Hw*`/`SubmitTimer`/
`SubmitDpc` fields), not a generalization of it: the two channels differ enough in level (DISPATCH vs
PASSIVE_LEVEL), lock (spinlock vs `GartLock`) and trigger (`SubmitCommand` vs `SubmitCommandVirtual`) that
sharing code would cost more clarity than it saves, and - more importantly - **duplicating rather than
generalizing node 0's fields is what keeps node 0's behaviour byte-for-byte identical with the gate closed**
(section 6). One property does have to become per-node rather than stay a single shared pair, because it is
observably wrong with two nodes: `BC250_WDDM::SubmittedFence`/`SubmittedNode`/`CompletionPending` (`wddm.c:190-
193`) and the matching preemption triple are a single slot each, correct "only because there is exactly one
node" (the `C_ASSERT` at `wddm.c:245` says so). With node 1 real, both nodes can have an outstanding software or
hardware completion at once; sharing one slot would silently drop one of them. These six become two-element
arrays indexed by node ordinal (`[BC250_WDDM_NODE_3D]`/`[BC250_WDDM_NODE_COPY]`); `WddmCompleteFence`,
`WddmPreemptFence` and `WddmReportDpcRoutine` take/report a node index. With the gate closed only index 0 is
ever touched, by the same call sites as today - the array collapses to the scalar it replaces.

Node 1's own fence slot: a fresh slot on SDMA0's existing 16-slot fence page (`BC250_SDMA_FENCE_SLOTS`,
`bc250_sdma.h:85`) that no escape uses - slots 0/1 are the ring tests', 2/3 are `SdmaCopyEscape`'s and
`GfxFenceEscape`'s SDMA arm's. `BC250_PAGING_FENCE_SLOT = 4` (mirroring `BC250_SUBMIT_FENCE_SLOT = 10` on the
gfx ring's own, separate, fence page - "one past what the escapes use").

The IH side needs **no new vector recognition**, by the same argument stage C's own brief reached
(`stageC_design.md` section 4.1, step 1-2 for node 0): `Bc250DpcRoutine` (`pnp.c:192-197`) already calls
`WddmGpuFence()` on *every* DPC regardless of which vector arrived, because completion is decided by polling a
memory slot, not by parsing the vector that woke the DPC. A second call, `WddmGpuFencePaging()`, added next to
it, polls SDMA0's slot 4 the same way; whether the vector that triggered this particular DPC was CP end-of-pipe
(client 20/181) or SDMA0's trap (client 8/224, facts M59) does not matter to either poll. This also answers
the task's question about mapping the trap to `DXGK_INTERRUPT_DMA_COMPLETED` for node 1: the mapping is not a
vector filter in `ih.c` at all, it is "node 1's fence slot now holds the sequence number", exactly as node 0's
answer is "node 0's fence slot now holds it" - `ih.c` stays a vector counter for diagnostics, unchanged.

One in flight, exactly as stage C (section 3.4 of its own brief): `amdgpu_ring_alloc` never looks at the read
pointer, so a second push before the first fence lands risks wrapping the ring onto unread packets. The 500 ms
watchdog (`BC250_WDDM_SUBMIT_TIMEOUT_MS`, `wddm.c:508`) is copied for node 1 with its own timer/DPC: a fence
that does not arrive is completed in software and the ring path is marked failed for node 1 only, never
touching node 0's `SubmitFailed`/ring - the two nodes fail independently, on their own hardware.

## 6. Caps, `NODEMETADATA`, `PagingNode`, and what stays inert with the gate closed

- `EnablePagingNode` (`HKR, Parameters, EnablePagingNode, 0x00010001, 0`), read once in `WddmStart` next to
  `EnableGpuSubmit`'s read in `GfxStart`, stored as `wddm->NodeCount` (1 or 2) and `gfx->PagingGate`.
  `BC250_WDDM_NODE_COUNT` (a compile-time constant used in bounds checks today) becomes a runtime field;
  `BC250_WDDM_NODE_COUNT_MAX = 2u` sizes the new arrays.
- `DXGK_DRIVERCAPS.GpuEngineTopology.NbAsymetricProcessingNodes = wddm->NodeCount` (was the macro).
- `DXGK_DRIVERCAPS.MemoryManagementCaps.PagingNode = (wddm->NodeCount > 1) ? BC250_WDDM_NODE_COPY :
  BC250_WDDM_NODE_3D` - ADR 0013 point 2, node 1 is the paging node once it exists at all; with the gate
  closed the answer is exactly today's `BC250_WDDM_NODE_3D`.
- `Bc250WddmGetNodeMetadata`: node 1, when named, answers `EngineType = DXGK_ENGINE_TYPE_COPY`,
  `FriendlyName = L"BC-250 SDMA0"`, `GpuMmuSupported = TRUE` (it *can* be addressed with a VMID; section 2
  chooses not to, which is a policy of this build, not a hardware limit worth mis-declaring). The bound check
  (`node >= BC250_WDDM_NODE_COUNT`) becomes `node >= wddm->NodeCount`.
- `Bc250WddmCreateContext` accepts `NodeOrdinal == BC250_WDDM_NODE_COPY` only when `wddm->NodeCount > 1`
  (mirrors the existing check, one more accepted value); `PagingCompanionNodeId` for a node-0 context becomes
  the reported paging node (today always `BC250_WDDM_NODE_3D`, matching `PagingNode` above) rather than the
  hardcoded constant.
- `Bc250WddmSubmitCommand`: today's node argument resolution (`context ? context->NodeOrdinal :
  pSubmitCommand->NodeOrdinal`, `wddm.c:1694-1695`) is untouched; a new branch fires only when
  `node == BC250_WDDM_NODE_COPY && wddm->NodeCount > 1 && DmaBufferSize != 0`, calling the new
  `GfxSubmitPaging()` path (section 4); every other case - including node 1 traffic when the gate somehow
  still reports one node, which cannot happen since `NodeOrdinal` would never name it - keeps today's
  `WddmCompleteSoftware`.
- `Bc250WddmBuildPagingBuffer`: the `DXGK_OPERATION_VIRTUAL_TRANSFER`/`VIRTUAL_FILL` arms are new code, added
  next to the existing `UPDATE_PAGE_TABLE` arm, not a change to it; with the gate closed `wddm->NodeCount == 1`
  and VidMm is never told a paging node exists other than node 0, so - by dxgkrnl's own contract, not a check
  this driver adds - it has no occasion to send `VIRTUAL_TRANSFER`/`VIRTUAL_FILL` at all. The arms are written
  defensively (log and answer inertly on any unexpected call) rather than relying on that alone.
- `PreemptCommand`/`ResetFromTimeout` (ADR 0008 point 5: never fail) extend to node 1 by construction: both
  already work from the DDI's own `NodeOrdinal` argument (`PreemptCommand`, `wddm.c:1741-1742`) or from
  scanning whichever channel has something pending (`ResetFromTimeout` becomes: check both channels, forget
  whichever is pending, without a report, exactly as node 0's one channel does today). Neither DDI's *shape*
  changes; both simply have two channels to check instead of one once the gate is open.
- `QueryDependentEngineGroup`/`QueryEngineStatus`/`CalibrateGpuClock`: bound checks move from the
  `BC250_WDDM_NODE_COUNT` macro to `wddm->NodeCount`; behaviour for node 0 is unchanged, node 1 answers the
  same shape (one dependent node - itself; always responsive; the CPU counter as its clock).

**What never runs with the gate closed**: `GfxSubmitPaging`, `WddmGpuFencePaging`, the shadow-buffer build in
`BuildPagingBuffer`'s new arms, the `Sdma0RingLock` acquisitions added to the escape paths (they still execute
- the lock itself is cheap and always safe to take - but nothing on the other side of it ever contends, since
node 1 never submits), and the second array slot everywhere the fence-pair arrays are read. `wddm->NodeCount`
stays 1, `PagingNode` stays `BC250_WDDM_NODE_3D`, node 1 is never named to dxgkrnl, `CreateContext` for node 1
is refused exactly as any out-of-range `NodeOrdinal` is today. This is the regression bar the task sets, and
it is met by construction (new branches, not edited ones) rather than by a runtime flag guarding old code.

## 7. GuardLog and the summary

Every paging operation kind seen (`DXGK_OPERATION_VIRTUAL_TRANSFER`, `_VIRTUAL_FILL`, and the already-existing
`UPDATE_PAGE_TABLE`/others) is counted the way `WddmNoteKind`/`Wddm->PagingOps` already counts
`BuildPagingBuffer`'s operations today (`wddm.c:170,1668`) - unchanged table, new values simply start
appearing in it once the gate is open. New counters, reported in `WddmSummaryOf` next to the existing DDI/
adapter-info/paging-op tables: transfers built, fills built, bytes moved, insufficient-buffer answers,
unsupported-direction answers (section 2's limit), node 1 submissions/completions/timeouts/refusals (mirroring
`HwSubmitted`/`HwCompleted`/`HwTimeouts`/`HwRefused`). `bc250kmd_cli`'s `log summary`/`gfx state` output needs
no change to show these: they ride the same `GuardLog` ring every other summary line already uses.

## 8. What remains open

1. **Not measured on hardware by this note**: whether `VidMmTranslate`'s resolution of a VidPn-committed
   surface's GPU VA agrees with what `TRANSFER_VIRTUAL`/`FILL_VIRTUAL` actually name for it - stage B proved
   the mechanism against a D3DKMT probe's own allocation (facts M73), not against a paging operation VidMm
   itself issued. E24's H1 is exactly this question.
2. **The IB/VMID route (section 2, section 3) is deliberately not built.** If the physical-only cut proves
   too narrow (system-memory-resident allocations that need real eviction) or too slow, the upgrade path is
   additive: a VMID for the system context, `SDMA_PKT_INDIRECT_HEADER_VMID`, and its own positive control -
   not a rewrite of what this note builds.
3. ~~**`Sdma0RingLock`'s narrow scope is this note's own reasoning, not yet measured**: that no escape path holds
   it across anything that could block (a register poll, `KeStallExecutionProcessor`) is true by construction
   (section 4) but worth a second look in code review before it ships.~~ Taken: the second look (section 9)
   found that "every writer of the SDMA0 ring takes it" was not actually true of `StageSdma`'s own ring test,
   fixed there; every writer's own hold is still a single push with no poll under it, unchanged.
4. **Whether dxgkrnl ever sends more than one `BuildPagingBuffer` batch before a `SubmitCommand` in a way that
   would overflow `BC250_WDDM_PAGING_BUFFER_BYTES`'s shadow copy** is unmeasured; the `MultipassOffset` path
   (section 4a) is what the DDI contract provides for exactly this, and this note relies on dxgkrnl using it
   rather than assuming the shadow is always big enough. Section 9 raises a related, also unmeasured question:
   whether a batch that mixes a built operation with one `GfxPagingBuild` answers "unsupported" leaves the
   shadow copy internally consistent with what dxgkrnl believes it holds.

## 9. What run 001 hung on

E24 run 001 (2026-09-22 09:41-09:48, bc250kmd 0.7.24, commit `85fbf5c`, fact M96,
`evidence/windows/2026-09-22-E24-paging-node-run-001/`) ran gate-open (`EnablePagingNode` 1, full stage C
table) through E19 run 003's own bring-up: gate -> gart enable (OK) -> psp load (OK) -> ih init (OK, ring
enabled) -> `gfx run 8` at 09:42:48, which never returned. Both network addresses died inside that one escape
call, the monitor went black, no bugcheck, no TDR, no dump - a hard hang, not a crash. The same sequence
completed on 0.7.13 (E19 run 003) and on 0.7.23-era code with the gate closed (E15/E23, stage 7). The one
thing this run added on top of both of those is `EnablePagingNode` open, so this section treats 0.7.23's own
diff (`git show abea97f --stat`) as the suspect set.

**Read, not guessed: two invariant violations, fixed.**

- **Gap A.** Every comment above `GfxPagingBuild`/`PagingRangeContiguous` claimed the function runs "PASSIVE_
  LEVEL, under GartLock, like GfxSubmitIb." It does not: `Bc250WddmBuildPagingBuffer` (wddm.c, the
  `VIRTUAL_TRANSFER`/`VIRTUAL_FILL` arms `abea97f` added) never takes `Device->GartLock` before calling into
  it - confirmed by grepping every `ExAcquireFastMutex(&Device->GartLock)` in the tree, all in gart.c/psp.c/
  ih.c/gfx.c's own bring-up and submit paths, none in wddm.c. The obvious fix, taking `GartLock` there too,
  does not work: `ExAcquireFastMutex` raises IRQL to APC_LEVEL, and `VidMmTranslate` (`vidmm.c:238`) refuses
  anything but exactly PASSIVE_LEVEL - a GartLock-held call into it would fail every translation, silently,
  for as long as the lock was held. So nothing serialized `GfxPagingBuild` against `TearDown()` freeing
  `Gfx->PagingShadowMem` out from under it. Fixed with `PagingBuildersActive` (gfx.c: struct field,
  `InterlockedIncrement`/`InterlockedDecrement` around the whole body, a bounded drain-wait in `TearDown()`
  before the free) - the same rundown-counter-plus-bounded-wait shape `WddmStop` already uses for a hardware
  submission in flight, chosen because there is no GPU reset on this part (facts M53), so an unbounded wait
  was never an option and a lock is not possible here.
- **Gap B.** Section 4's claim that "every writer of the SDMA0 ring takes `Sdma0RingLock`" was not true of
  `StageSdma`'s own ring-test push (stage 7): the bring-up escape reprogrammed the live SDMA0 ring with no
  lock at all - the one direct hit on the task's hypothesis (b). Fixed: `StageSdma` now takes `Sdma0RingLock`
  narrowly around `bc250_sdma_ring_test_submit`, released before `bc250_sdma_ring_test_wait`'s poll, matching
  every other writer's shape (submit under the lock, wait outside it).

**What closer reading takes back.** Both gaps need a precondition that, on rereading `GfxPagingBuild`'s own
first check, provably did not hold during run 001. `GfxPagingBuild` bails to `BC250PagingNotReady` before
touching the shadow buffer or any hardware if `!gfx->PagingReady` (gfx.c:1238), and `GfxPagingSubmitReady`
(gfx.c:1130-1133) requires the same flag; `PagingReady` is set true only at the tail of a *successful* stage-8
`GfxEscape` RUN (gfx.c:460) and cleared only by `TearDown()`, which runs only from `GfxStop`'s quiet-stop path
and the escape's PLAN branch (gfx.c:351, gfx.c:436) - never from the middle of a RUN. Since run 001's
`gfx run 8` never returned, `PagingReady` never became true and `TearDown()` was never called during the run:
Gap A's teardown race and Gap B's concurrent-writer race both need a window that was never open on this
specific run. Both fixes stand - they close a real hazard for the case that matters, ordinary desktop use with
`PagingReady` true, which E22 already measured as continuous (1079 `BuildPagingBuffer` calls/minute) - but
neither is confirmed, or now even likely, to be what run 001 actually hung on. Reporting this section as
closing the hang would not be honest.

**Hypotheses checked against the task's list, with what ruled them in or out:**

1. **(a) lock ordering inversion / IRQL mismatch** - not found. `GartLock`, `Sdma0RingLock` and `wddm->Lock`
   are never nested (grepped across driver/kmd: every `Sdma0RingLock`/`wddm->Lock` acquisition is a short,
   self-contained critical section - `WddmGpuFencePaging`, wddm.c:661-682, is two flag reads under
   `wddm->Lock` and nothing else). REFUTED for the paths read.
2. **(b) escape reprogramming SDMA0 while paging concurrently writes** - this is Gap B, CONFIRMED as a real
   invariant violation and FIXED, but per the paragraph above not provably live during run 001 itself, since
   `PagingReady` gates the only concurrent writer (`GfxSubmitPaging`).
3. **(c) spin/poll under a held lock** - not found. `bc250_ring.c`'s `amdgpu_ring_alloc`/`_commit`/
   `_insert_nop`/`_undo` (the primitives every ring writer uses) contain no polling; `StageSdma`'s fix
   explicitly releases `Sdma0RingLock` before `bc250_sdma_ring_test_wait`. REFUTED for the paths read.
4. **(d)/(f) per-node watchdog/DPC/ISR firing into half-initialized state** - the one paging-specific
   instance, `WddmGpuFencePaging`, runs unconditionally on every DPC (pnp.c's `Bc250DpcRoutine`) but is
   entirely driven by `wddm->PagingHwPending`, which only becomes true inside `WddmSubmitPagingHardware` -
   itself gated on `GfxPagingSubmitReady`/`PagingReady`. With `PagingReady` false throughout run 001 this
   function is two spinlock-protected flag reads and a return: REFUTED as a cause of this run. **Not checked
   this session**: `PreemptCommand`, `ResetFromTimeout`, `QueryEngineStatus`, `CalibrateGpuClock`,
   `GetNodeMetadata` and `CreateContext`'s node-1 branches (section 6 describes them as straightforward
   `NodeOrdinal`/`wddm->NodeCount` generalizations of already-correct node-0 code, not reread line-by-line
   here). `wddm->NodeCount` becomes 2 at `WddmStart` (wddm.c:1079), read once from the gate at device start -
   before run 001's script issued a single escape - so dxgkrnl believed node 1 existed for the whole run,
   independent of `PagingReady`, and had the whole run to call any of these against it. This is the largest
   gap this session leaves open, and the leading remaining suspect.
5. **(e) something inside stage 8 itself that 0.7.23 broke** - REFUTED by diff: `StageInterrupts` (gfx.c:
   53-61, stage 8's own function) has zero lines touched by `abea97f` (`git show abea97f -- driver/kmd/gfx.c
   | grep StageInterrupts` - no output). Whatever hung inside `gfx run 8` did so in an environment 0.7.23
   changed around stage 8 (struct layout, `wddm->NodeCount`, the new DDI arms), not in stage 8's own code.
6. **This note's own earlier suspects** (`VidMmTranslate` cost under continuous traffic; stale shadow bytes
   pushed as garbage packets) both assumed `GfxPagingBuild` doing real work, which needs `PagingReady`.
   DOWNGRADED to refuted-for-this-run alongside Gap A/B, for the same reason.

Net: the two concrete, fixed defects (Gap A, Gap B) are real but not provably this run's cause. The strongest
lead this session could not close is item 4's unread DDI branches - node 1 was visible to dxgkrnl for the
whole run, with stages 1-8 still running underneath it.

**Fix, in one place.** driver/kmd/gfx.c: `PagingBuildersActive` (struct field, `InterlockedIncrement`/
`Decrement` around `GfxPagingBuild`'s body, a bounded drain-wait in `TearDown()` before `bc250_shim_mem_free`
of `PagingShadowMem`) and `Sdma0RingLock` taken narrowly around `StageSdma`'s ring-test submit. No new gate -
both fixes sit entirely inside code the existing `PagingGate`/`PagingReady` machinery already reaches.

**Safer lab sequence, before `EnablePagingNode` may do real work.** Run the bring-up (gart/psp/ih/
`gfx run 8`) to a *successful* return first, gate open but the desktop otherwise quiesced - no compositor
churn forcing `BuildPagingBuffer` traffic mid-sequence (E22's 1079/minute figure was ordinary desktop use, not
a worst case) - so `PagingReady`'s flip from false to true happens with nothing else in flight. Only once
`bc250kmd_cli gfx state` reports `StagesDone == 8` and no `Failed` should anything that depends on real node-1
traffic (an app doing real paging, or a deliberate probe) be layered on top. Section 8 item 3 (struck through
above) is the standing reminder to keep rereading this before it ships.

**A cheap experiment that cannot re-hang the machine the way run 001 did.** The leading open suspect (item 4)
is dxgkrnl's own awareness of node 1's *existence*, independent of any real paging traffic - `wddm->NodeCount`
is 2 for the whole run regardless of `PagingReady`. That is separable from everything else 0.7.23 added: a
temporary, loudly-logged, never-shipped one-line override at wddm.c:1079 that pins `wddm->NodeCount` to
`BC250_WDDM_NODE_COUNT` (1) even when the gate reads 1, then a rerun of exactly run 001's sequence. Every
other 0.7.23 change (Gap A/B's fixes, the interlocked `FenceSeq`, the split ring test, the new struct fields)
stays compiled in and running; the only thing removed is dxgkrnl's belief that node 1 exists. Not zero-risk -
nothing is, on hardware with no GPU reset - but it repeats a shape already proven twice (0.7.13, E15/E23) with
one bit flipped, rather than repeating run 001's own already-hung sequence outright. If it completes cleanly,
item 4's DDI branches move from "leading suspect" to "confirmed cause, needs a real fix"; if it still hangs,
item 4 is cleared and the search moves to what changes even with `NodeCount` at 1 (the struct layout, the
interlocked `FenceSeq`) - a genuinely surprising result worth an immediate stop rather than a further rerun.
