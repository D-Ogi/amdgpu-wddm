# E20: a picture under the full WDDM table

Date: 2026-09-22. State: run 001 done (H1 refuted as written); run 002 (0.7.15) prepared. Follows E19 (stage C, M77, M80).
Design brief: `scratch\tmp\present_design.md` (an Opus agent; not a source of facts).

## Why

Under the full table the screen is black (facts M71). The brief's reading of our own logs: every `Present` of the CDD
arrives with `NumSrcAllocations = NumDstAllocations = 0` (E16 run 009, E18 run 003), and the cause is ours -
`CreateContext` answers `AllocationListSize = 0`, so dxgkrnl has nowhere to put the two surfaces of a Blt. A driver
that cannot name the source cannot show it. The shortest picture is then a CPU copy from the source surface into the
firmware's framebuffer inside `DxgkDdiPresent` (PASSIVE_LEVEL, no register, no ring): the display-only table's own
method, fed from VidMm's memory. Moving the scanout (DCN) is the later, better answer and needs measurements of its own.

Czarno to widzę - "I see it black", which is how a Pole says the outlook is poor. Here it is also the literal bug.

## Hypotheses

- H1 (run 001, 0.7.14). With `AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT` (256) for a GDI context and
  nothing else changed, the CDD's presents arrive with `NumSrcAllocations` 1 and `NumDstAllocations` 1, and stage A's
  result stands otherwise: CommitVidPn, presents, software fences, no TDR, no bugcheck, gate closes, picture returns.
  Falsifiers: the counts stay 0 (then the list size is not the cause); or dxgkrnl answers the list with a demand this
  driver does not meet (a 0x113 verification bugcheck, a refused context, a call to a DDI the table lacks).
- H2 (run 001). Which arm of `DXGKARG_PRESENT`'s union a GpuMmu driver is given - the header does not say. The build
  logs the first three qwords of the first two entries raw. Prediction: `DXGK_PRESENTALLOCATIONINFO` (32-byte entries:
  handle, GPU virtual address, physical address, segment id), because that structure exists to carry a virtual
  address. It shows as: qword 0 = one of our `BC250_WDDM_OBJECT` pointers (kernel address), qword 1 = a page-aligned
  GPU VA in the range VidMm maps for that process, qword 2 = a segment offset or physical address, or 0. The other
  reading, `DXGK_ALLOCATIONLIST` (24-byte entries), shows as: qword 0 = the handle, qword 1 = a small bitfield value
  (write flag, segment id in bits 1..5), qword 2 = the address. If qword 0 of entry 1 (raw[3]) is a kernel pointer,
  the entries are 24 bytes and it is the second reading.
- H3 (run 002, the build after this one; written down now so that run 001 cannot bend it). The source surface of the
  CDD's Blt is a segment 1 (VRAM) allocation of the format and size `CreateAllocation` recorded (1920 x 1200,
  `D3DDDIFMT_A8R8G8B8`), and copying its dirty rectangles into the firmware framebuffer by physical address gives a
  desktop picture: the overlay's screenshot is no longer the 3.6 kB black frame of E16 run 009.

- H2b (run 002, 0.7.15; written after run 001, before run 002). The counters of H1 are not where the surfaces are:
  `d3dkmddi.h` defines `DXGK_PRESENT_SOURCE_INDEX 1` and `DXGK_PRESENT_DESTINATION_INDEX 2`, fixed indices into
  `pAllocationList` (24-byte `DXGK_ALLOCATIONLIST` entries). Prediction: with the 256-entry list of 0.7.14 the
  pointer is not NULL, and entries 1 and 2 each hold one of our `BC250_WDDM_OBJECT` pointers in qword 0 (a kernel
  address, the same two values present after present: the shadow surface and the primary of `CreateAllocation`), a
  small flags value in qword 1, and in qword 2 the surface's GPU virtual address (the union's WDDM 2.0 member).
  Falsifiers: a NULL pointer; qword 0 not a kernel address; values that change wildly between presents. The
  context of flags 0x5 (system, list 0) would then also explain nothing arriving there. If it holds, H3's blit
  has its source: handle -> our allocation record (size, pitch, format) and VA -> VRAM offset through the
  context's page tables.

## Safety

Run 001 starts no engine: no GART, no PSP, no ring, no interrupt source - so facts M78 does not apply and no fresh
boot is needed. It reads at most 48 bytes of a list dxgkrnl sized at 256 entries. Install with the gate closed (M74),
gate one-shot, AutoReboot on, STOP flag honoured, 60 to 90 s under the full table.

## Procedure

`e19_target.ps1` phases `install`, `state`, `confirm`, `gate -Full 1 -GpuVa 1`, `log`, `gate -Full 0`.

## Result

### Run 001 (2026-09-22 02:03, bc250kmd 0.7.14)

H1 is refuted as written: the GDI context (flags 0x6) was answered `lists 256/0`, and all eight logged presents
still came with `source 0 dest 0`. Its second half held: the desktop survived the list (no TDR, no bugcheck, no
live kernel report, gate closed, picture back). H2 got no answer, and that is the build's fault, not dxgkrnl's:
0.7.14 printed the list only when the counters were non-zero and did not even log the pointer. Evidence is
recorded with run 002.
