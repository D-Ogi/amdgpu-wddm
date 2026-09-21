# E18: the full WDDM table, stage B - VidMm's page tables in the hardware's format

Date: 2026-09-21. State: run 001 (plan) prepared. ADR 0008 stage B; follows E16 (stage A, facts M71).

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

(after the runs)
