E24 run 006, 2026-09-22 12:01-12:03: bc250kmd 0.7.29 pre-image (0.7.28, commit 963f23a) on unit A, first
bring-up of the 11:59:32 boot (the freshness guard of M78 passed). The VidPn flip gate stayed CLOSED this time:
the experiment has no use for it, and run 005 left the owner watching M100's unpainted primary for four minutes.

The run that tests the pDmaBuffer fix of M108. Same shape as run 005 - eight stages, the `fence gfx x2` control
for the IH ring, then `kmtprobe` holding 64, then 256, then 512 MB.

What the fix did. dxgkrnl now sees the bytes we write. Where run 005 built all four fills at shadow offset 0x0,
this run advances them: 0x0, 0x140, 0x640, 0xB40 - each one starting exactly where the previous ended (80 dwords
= 0x140 bytes, then 320 dwords = 0x500 bytes twice). The DDI agrees from its own side: the call at 83.898 s
reports `65216 bytes free` of a 65536-byte paging buffer, which is 65536 minus the 320 bytes of the fill before
it. VidMm is packing several operations into one buffer and counting what we put there, which is what the
contract describes and what run 005 could not do.

What the fix did not do. `DxgkDdiSubmitCommand` is still absent from the DDI tally - not one call in 110 s
against 1581 BuildPagingBuffer calls - and node 1 still ends at `0 hardware submitted`. So M108's pointer was a
real defect and not the whole story.

Where the paging buffer actually goes (M110). One line of this log answers it:

    41      0.078 wddm: CreateContext node 1 engine 0x1 flags 0x00000005

0x5 is SystemContext | VirtualAddressing (d3dkmddi.h, DXGK_CREATECONTEXTFLAGS at line 1512; VirtualAddressing is
the third bit, line 1521). VidMm creates node 1's paging system context with virtual addressing, and a context
that addresses virtually is submitted through DxgkDdiSubmitCommandVirtual - the DDI this driver had wired to
node 0 alone. Three further measurements in this run agree:

  - `D3DKMTMakeResident` returned STATUS_PENDING with PagingFenceValue 7002 for each of the three probes, every
    probe then completed and freed, so VidMm's paging fence did complete. Nothing reached the hardware, and
    SubmitCommand was never called, so the completion can only have come through the one remaining path - the
    software completion at the end of Bc250WddmSubmitCommandVirtual.
  - SubmitCommandVirtual runs ahead of the Present count by exactly one per probe: 70/70 before the first fill,
    83/82, 104/102, 122/119. Four fills, three extra submissions, because the third and fourth fill were packed
    into one buffer (offsets 0x640 and 0xB40) and submitted once.
  - The driver logged none of those node-1 submissions, because the log budget for that DDI was spent on the
    desktop's own presents in the first five seconds. `0 hardware submitted` could therefore not be told apart
    from `never submitted`, which is the blind spot 0.7.29 closes (per-node logging and per-node counters).

The rest of the run: 872,415,232 bytes of fills built, no transfers, no insufficient-buffer, none of the five
unsupported reasons, no bugcheck, no TDR, device status OK, clean undo, 68.1 to 72.6 C.

Files: `run-006-console.txt` (the whole console), `run-006-ring.log` (the driver's own 492-line ring log, where
the four fills, the CreateContext line and the counters are), `run-006-script.sh`.
