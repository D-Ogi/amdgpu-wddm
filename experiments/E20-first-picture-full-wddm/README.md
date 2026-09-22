# E20: a picture under the full WDDM table

Date: 2026-09-22. State: DONE (run 008, M84). Runs 001-008. Follows E19 (stage C, M77, M80).
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

- H4 (runs 003 and 004, 0.7.16; written before both). With `OpenAllocation` returning our object through
  `DxgkCbGetHandleData`, the source slot's handle is one of our allocation objects, and it is the shadow surface of
  `CreateAllocation` (1920 x 1200, `D3DDDIFMT_A8R8G8B8`, pitch 7680, size 0x8CA000). Its GPU VA (`0x8DC000` in run
  002; any page-aligned value counts) translates through the context's root page table to VRAM inside segment 1,
  and the last byte translates to first + size - 1: one contiguous range, as a memory-segment allocation is by
  definition. Run 003 checks this with the blit gate closed (`sources translated contiguous` > 0 in the summary, no
  `blit skipped` lines); run 004 opens the gate and is H3's test: the overlay screenshot is not the black frame.
  Falsifiers: the handle stays NULL (then `DxgkCbGetHandleData` does not give back `hAllocation` of
  CreateAllocation); the walk finds an invalid entry (then the CDD's source is not mapped when it presents, or the
  root is not this context's); the range is not contiguous; the picture stays black with the gate open (then the CPU
  never wrote into that surface, and the pixels are elsewhere).

## Safety

Runs 003 and 004 (0.7.16): with the full table and EnableGpuVa open, every Blt present reads its list entry 1, looks
the handle up on our object list (no dereference of the value) and walks the page tables read-only through short
mappings - gate or no gate. `EnablePresentBlit` guards only the mapping of the source and the copy into the firmware
framebuffer, which is the only memory written. No engine is started in either run.

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
0.7.14 printed the list only when the counters were non-zero and did not even log the pointer. Evidence:
`evidence/windows/2026-09-22-E20-run-002/run-001-console.txt`.

### Run 002 (2026-09-22 02:12, bc250kmd 0.7.15)

H2b held in half (facts M82). The pointer is not NULL and never changes; the source slot is filled, the same in
all eight presents, and its third qword `0x8DC000` sits exactly one surface (`0x8CA000`) above `0x12000`, which
reads as a GPU virtual address. Refuted: qword 0 is not our object pointer, it is zero - and that zero is ours,
because `OpenAllocation` hands dxgkrnl a NULL `hDeviceSpecificAllocation` and gets it back faithfully. RosKmd maps
`hAllocation` with `DxgkCbGetHandleData` and returns its own pointer; 0.7.16 does the same. The destination slot
is all zero: for a present to the primary the destination appears to be implied by the VidPn source. H2's question
(which arm of the union) is answered by the layout: 24-byte entries at indices 1 and 2.

Jak sobie pościelesz, tak się wyśpisz - as you make your bed, so you will sleep in it. We made it with a NULL.

Evidence: `evidence/windows/2026-09-22-E20-run-002/run-002-console.txt`.

### Runs 003, 005, 007 (0.7.16, 0.7.17, 0.7.18 with the blit gate closed)

H4 held in the end, and in two steps that were not in it. Run 003: `DxgkCbGetHandleData` gave nothing back for the
CDD's handles, so 0.7.17 built the opened object from `pPrivateDriverData` instead. Run 005: handles handed out, and
the 24-byte reading of slot 1 still said 0. Run 007: the same bytes read as 32-byte `DXGK_PRESENTALLOCATIONINFO`
entries name our object in entry 1 and the other one in entry 2 (facts M83), and the source translates through the
GDI context's root to one contiguous VRAM range of exactly 0x8CA000 bytes. No refusal, no TDR.

### Run 008 (2026-09-22 03:15, bc250kmd 0.7.18, EnablePresentBlit 1)

H3 holds, by the only instrument that can see it: the owner, at the monitor, saw the whole desktop, correct, for the
minute the full table ran (facts M84). The driver's log shows every present copied (3 rectangles / 144 rows, 586
rows for a full refresh, and so on). The overlay's screenshot is still the black 3586-byte frame - `CopyFromScreen`
reads the CDD's surfaces through GDI, not the scanout, so under the full table it cannot see what the firmware
framebuffer shows. H3's exit criterion as written ("the screenshot is no longer black") was the wrong instrument;
the observation replaces it and the README of the evidence says so.

Nie taki diabeł straszny, jak go malują - the devil is not as black as he is painted. Nor was the screen.

Evidence: `evidence/windows/2026-09-22-E20-run-008/`.
