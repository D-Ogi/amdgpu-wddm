# E18: the full WDDM table, stage B - VidMm's page tables in the hardware's format

Date: 2026-09-21. State: DONE - stage B's exit criterion met in run 003 (M72, M73). ADR 0008 stage B; follows E16 (stage A, facts M71).

## Why

Stage A showed what VidMm asks for: about a thousand `UpdatePageTable` calls at the start, then `FlushTlb` and
`VirtualFill`, and a root page table per process (M67, M71). Stage B answers them for real: every `DXGK_PTE` becomes the
entry the GPU's walker reads (`driver/shim/bc250_pte.c`, amdgpu's flags, host-tested) and is written into the table
page VidMm named, by the CPU, because the carve-out is reachable by physical address (M31). No VMID is pointed at these
tables yet, so nothing but the witness reads them: a wrong entry in stage B costs a wrong reading, not a wrong DMA.

## Hypotheses

- H1 (run 001, `EnableGpuVa` = 0, plan only: nothing is written). The log settles the three questions of
  `bc250_pte.h`: (1) `PageAddress` of a segment 1 entry is a byte offset into the segment (low 12 bits zero, magnitude
  below the segment's size; the primary's known offset 0xACE000 style values, not frame numbers); (2) system memory
  pages, if any show up, carry segment 0; (3) level 0 is the leaf: level 3 is the root VidMm passes to
  `SetRootPageTable`, levels 3..1 name tables inside segment 1. Predicted: 0 refused entries and 0 bad calls. A refusal
  is a finding, not a failure: it names the field the translator does not understand.
- H2 (run 002, `EnableGpuVa` = 1 with `EnableVramWrite` = 1). The same traffic is written. The witness (`bc250rd`
  reading VRAM by physical address) finds at the root's address a table whose valid entries name level 2 tables inside
  the segment, and can walk one chain down to a leaf that names the shared primary (the address of
  `SetVidPnSourceAddress`). The desktop behaves as in stage A: presents arrive, fences retire, no TDR, no bugcheck.
- H3 (run 003, `tools/win/kmtprobe`). A D3DKMT client creates an allocation, maps it at the GPU virtual address it
  chose, makes it resident, writes a pattern; the walk from that process's root reaches leaves whose physical pages
  hold the pattern. This is stage B's exit criterion (research document section 5.2).

## Safety

As E16: one-shot gate, start budget, AutoReboot, the owner's word through the overlay, STOP flag honoured. New in this
experiment: CPU writes into VRAM pages that VidMm allocated for page tables (run 002 onwards). They stay inside
segment 1 by construction (bounds-checked offset, one page per call), never touch the firmware framebuffer below the
segment or the reserved tail above it, and no register is written.

## Procedure

`e18_target.ps1` is E16's target script with one more switch: `-Phase gate -Full 1 -GpuVa 0|1` sets `EnableGpuVa`
and `EnableVramWrite` together. Install with every gate closed, check stage 61, confirm, open the gate, read the ring
(`vidmm:` lines) and the summary, close the gate.

## Result

### Run 001 (2026-09-21, bc250kmd 0.7.10, plan)

Evidence: `evidence/windows/2026-09-21-E18-run-001/`. Facts M72.

- H1 is half right and the wrong half is the useful one. Level 0 is the leaf, level 3 the root, host memory is
  segment 0 - as predicted. `PageAddress` is NOT a byte offset: it is a page frame number. The translator refused all
  9011 valid entries (rc -22) instead of mis-mapping them, which is what the units parameter was built for.
- Not predicted at all: the 1028 page table updates at adapter start come in `CPU_VIRTUAL` mode, with a pointer, for
  the system paging process - although the driver declares `GPU_PHYSICAL`. 0.7.10 refused them as calls.
- bc250kmd 0.7.11: units = page frames, CPU_VIRTUAL calls written through VidMm's pointer. Run 002 opens `EnableGpuVa`.

### Runs 002 and 003 (2026-09-21, bc250kmd 0.7.11 and 0.7.12, `EnableGpuVa` = 1)

Evidence: `evidence/windows/2026-09-21-E18-run-003/`. Facts M73, M74.

- **H2 holds.** 280643 entries written at the start, none refused; the desktop's kernel side behaves as in stage A. The
  leaf for the shared primary names the very address `SetVidPnSourceAddress` gets. The witness walked the paging
  process's chain in run 002; the CDD's root sat above what the read escape allowed (fixed in 0.7.12).
- **H3 holds**, and with it the exit criterion: `kmtprobe` got the GPU VA it asked for, and the witness found 16 leaves
  for exactly that range whose pages hold the pattern. One detour: a client without a context never gets a
  `SetRootPageTable`, so the witness had to find the chain by scanning the table pages below the top of the segment.
- A lesson paid for with a restart: never install over a running full table (M74). Close the gate first.
- What stage B does not show: that the GPU's walker agrees. No VMID points at these tables; stage C does that.
