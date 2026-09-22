# E24 - the paging node in the full table (stage D of ADR 0008, per ADR 0013)

State: **the node answers real paging traffic correctly, and dxgkrnl never submits what it built** (run 005,
facts M107 and M108). Four `VIRTUAL_FILL`s, 832 MB, translated and emitted with no refusal and no crash - H1
holds, and so does the half of H2 that is about packets. Then `DxgkDdiSubmitCommand` was never called at all and
node 1 finished at 0 hardware submitted, so the packets never reached SDMA0. The open question is no longer in
our packet path but in what makes VidMm submit a paging buffer: a cap we report wrong, or an operation VidMm
discards because the probe never waits on its paging fence. Next: settle that, then a run that proves the
hardware half of H2 and H4 - and without `-VidPnFlip 1`, which this run had no use for and which left the owner
looking at M100's unpainted primary for four minutes.

Getting here cost a bugcheck: run 004's first real call blew the kernel stack on a 0x5B00-byte local (M104,
M105), fixed in 0.7.26 with `tools/win/stackbudget.py` now failing any build whose fixed frames reach 4 KB.

## Why

E23 (facts M95) proved the SDMA copy and fill packets and the fence outside the WDDM table. This experiment is the
table itself: node 1 (`DXGK_ENGINE_TYPE_COPY` on SDMA0) behind `EnablePagingNode`, `BuildPagingBuffer` answering
`DXGK_OPERATION_VIRTUAL_TRANSFER`/`DXGK_OPERATION_VIRTUAL_FILL` with real SDMA packets, and `SubmitCommand` running
them on the live SDMA0 ring with a fence that retires through the IH DPC as `DXGK_INTERRUPT_DMA_COMPLETED` for
`NodeOrdinal` 1. Z jednego palca dwa razy nie strzelisz (you cannot fire twice with the same finger) - the gate
stays closed until the host build and every existing replay are green first.

## Build

bc250kmd 0.7.23: `driver/kmd/gfx.c` (`GfxPagingBuild`,
`GfxSubmitPaging`, `GfxPagingFenceArrived`, `GfxPagingSubmitReady`, `GfxPagingSubmitFail`, `GfxPagingNodeGate`,
`GfxPagingShadowBytes`, `BC250_GFX::Sdma0RingLock`), `driver/kmd/wddm.c` (node 1 in caps/NODEMETADATA/CreateContext
only when the gate is open, per-node fence bookkeeping, the two new `BuildPagingBuffer` operations, `SubmitCommand`'s
node 1 branch), `driver/kmd/bc250kmd.inf` (`EnablePagingNode`, closed by every install). Design:
`docs/design/paging-node.md`. No VMID for node 1: paging virtual addresses are resolved to physical addresses on
the CPU through the existing `VidMmTranslate()` (stage B, facts M73), so SDMA never has a VMID pointed at anything
for this path. No `SDMA_OP_INDIRECT`: `BuildPagingBuffer` fills a driver-owned shadow buffer with the same
hardware-proven `bc250_sdma_emit_copy_linear`/`emit_fill` E23 used (M95), and `SubmitCommand` pushes those dwords
straight onto the live SDMA0 ring at `DISPATCH_LEVEL`, where `Device->GartLock` may never be taken.

Host tests: `driver/shim/test/run_paging.ps1`. `run_sdma_copy.ps1` and `run_sdma_faults.ps1` must stay EXACT MATCH -
this stage touches `driver/shim/bc250_sdma.h`'s callers, not its packet emitters, so nothing there should move.

## Hypotheses

- H1: `VidMmTranslate()` on a VidPn-committed surface's GPU virtual address, through the root `SetRootPageTable`
  recorded for `hSystemContext`, resolves to the same physical address the CPU-side allocation actually has - i.e.
  the paging node's addresses agree with stage B's own page tables, not just with a synthetic escape's.
- H2: the packets `GfxPagingBuild` writes into the shadow buffer and the ones it copies into `pDmaBuffer` are byte
  for byte the same, and `SubmitCommand`'s push of the shadow's bytes onto SDMA0 runs them with a fence that
  retires through the IH DPC as `DXGK_INTERRUPT_DMA_COMPLETED`, `NodeOrdinal` 1 - the same completion path node 0's
  stage C already proved (facts M77), now on the second engine and through the interrupt rather than a poll.
- H3: with `EnablePagingNode` closed, the table is unchanged from today's one-node table in every caps query,
  `NODEMETADATA` answer and `CreateContext` refusal - the regression bar. With it open, nothing about node 0's own
  behaviour changes (`GfxSubmitIb`, its fence, its watchdog) and no TDR fires on either node during a gated run.
- H4: a CDD surface's contents arrive through a `VIRTUAL_TRANSFER` on SDMA0 and the desktop still shows through the
  existing blit or flip path (ADR 0011) - the exit criterion: VidMm chose to move something over the paging node and
  the picture on screen is unaffected by having let it.

## Gate flow

E20's gate flow: `experiments/E19-full-wddm-stage-c/e19_target.ps1 -Phase gate -Full 1 -Engines 1 -GpuVa 1
-GpuSubmit 1 -Blit 1 -PagingNode 1` (the script writes `EnablePagingNode` only together with `EnableGpuSubmit`, as
the node needs the engines stage C brought up; `-Full 0` closes everything). What to look for in the log and the
escape's summary: paging operations counted by kind (`VIRTUAL_TRANSFER`, `VIRTUAL_FILL`, and the three
`BC250_WDDM_PAGING_UNSUPPORTED` refusal reasons), node 1 fences submitted and retired, no hardware fence timeout on
either node, `WddmSummary`'s per-node counters agreeing with the escape's own. Exit criterion: H4.

## Runs

| Run | Build | What | Result |
|---|---|---|---|
| 001 | 0.7.24 (85fbf5c) | full table with every stage C gate plus `-PagingNode 1`, then the E19 run 003 bring-up: gart, psp, ih, `gfx run 8` | start, gart, psp and ih all fine; `gfx run 8` hung the whole machine (no bugcheck, no TDR, power button). H1-H4 untested. M96. `evidence/windows/2026-09-22-E24-paging-node-run-001/` |
| 002 | 0.7.24 (85fbf5c) | the control: node gate CLOSED, one escape call per stage | stages 1-5 returned, stage 6 (CP) hung the machine - and this was the second bring-up of its boot, the shape of M78. `evidence/windows/2026-09-22-E24-paging-node-run-002/` |
| 003 | 0.7.25 (5dcf020) | fresh boot, node and flip gates open, one escape call per stage, dumps before and after | all eight stages returned, node 1 advertised and given a context by dxgkrnl, 1079 page-table operations and no transfers, no TDR, clean undo (M101, M102, M103). H3 holds; H1, H2 and H4 still untested for want of paging traffic. `evidence/windows/2026-09-22-E24-paging-node-run-003/` |
| 004 | 0.7.25 (5dcf020) | fresh boot, same gates, then the two controls E24 was missing: `fence gfx x2` for the IH ring, then `kmtprobe --size 512M --hold 20` for VidMm | the ring delivers (202 interrupts, 203 vectors, `client 4 source 87` - M106, which refutes the reading of M98/M103); the pressure reached `GfxPagingBuild` and bugchecked 0x50 in `nt!_chkstk` on our own 23 KB stack frame (M104, M105). H1, H2, H4 still untested - the crash came before a packet was built. `evidence/windows/2026-09-22-E24-paging-node-run-004/` |
| 005 | 0.7.26 (4042b0e) | fresh boot, same gates, pressure in three steps: 64, 256, 512 MB | four `VIRTUAL_FILL`s answered, 872,415,232 bytes, 80 and 320 dwords of CONST_FILL at physical 0x271C62000 and 0x281C62000, no TDR, no bugcheck, clean undo (M107): **H1 holds and H2's build half with it**. But `DxgkDdiSubmitCommand` was never called and node 1 ends at 0 hardware submitted (M108), so nothing ran - H2's hardware half and H4 still open. `evidence/windows/2026-09-22-E24-paging-node-run-005/` |
