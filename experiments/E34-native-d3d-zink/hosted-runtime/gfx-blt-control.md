# Native GFX Blt positive control

Status: control002 passes all five hardware copy cases (M609), after control001
stopped at an incorrect GTT residency expectation before submission (M608).
The revised gate measures residency; independent physical placement and engine
Present integration remain open.

Hypothesis: the exact production row builder and DMA_DATA emitter copy the
requested rectangle on node0, and its queued monitored fence retires only after
the destination is visible to an independent readback copy.

Build with build-gfx-blt-control.ps1 -Root <workspace> -OutDir <scratch directory>.
The build links driver/kmd/{blit_plan,gfx_blt,gfx_copy}.c directly. Running without
arguments or with --help does not create a device or start the watchdog. Only
--run performs GPU work. This executable does not install a driver, replace an
ICD/UMD, change registry settings, draw a window or restart DWM.

After explicit lab handback, verify the exact KMD153 SYS/health/clock, STOP,
temperature below85C and no other test. Preserve pre-test memory state and KMD
counters because game presentation showed a separate copy-cost anomaly. Hash
check the executable and retain its exact source/build record. Announce the test
on the overlay and launch --run in a durable bounded task, collecting output.

The probe allocates/maps/makes resident buffers using the existing E27 helpers,
creates a GFX UMD context and a monitored fence on that context. The five cases
are small GTT-to-GTT, GTT-to-VRAM, VRAM-to-VRAM and VRAM-to-GTT rectangles with
command capacities7,14,35,35dwords, followed by1920x1200 GTT-to-VRAM with a larger
command buffer. Pitches differ and both rectangles have nonzero origins. Each
allocation is initialized in full: nonuniform source pattern, sentinel destination
and readback. Every batch signals and waits for the same context's hardware fence
before reusing the IB. Final readback uses the older E27 direct-memory DMA_DATA
packet, without the new row builder or L2 selectors, then checks every word of
the destination allocation against an independently calculated result, including
padding and the area outside the copied rectangle.

Expected positive: five COPY_RESULT PASS records, no mismatches, increasing
monitored-fence values, successful final result, node0 completion and matched
paging/residency evidence, no TDR or retained allocations after normal teardown.
A timeout, device loss, wrong image word, nonresident buffer or cleanup error is
a failure requiring inspection, not a reason to rerun automatically. The probe
has a180s process watchdog and bounded5s fence waits. If work remains unresolved,
it defers buffer destruction to process/device cleanup rather than freeing an IB
still in flight. Preserve the dump/logs and independently inspect the adapter.

The host suite covers a row larger than the CP packet limit, but this initial
hardware probe does not: it submits many packets through ordinary-sized rows.
Nor does it test the DDI present allocation list, all-rectangle prevalidation,
MultipassOffset integration, cross-context producer synchronization or CDD/DWM
interop. Those remain requirements before advertising the cap or completing G0.

## Residency gate correction after control001

The revised probe requires QueryAllocationResidency status1 for every buffer
before and after the copy. It no longer interprets this enum as a GTT/VRAM
placement measurement. Each allocation already completes MakeResident and its
paging fence. Status2/3, unknown values and API errors remain failures.

The five heap combinations refer to the requested BC2A heaps and KMD153's exact
supported read/write segment masks (c3499f1b, wddm.c): GTT is restricted to
aperture2, VRAM to memory segment1. This is a source contract, not an independent
physical mapping readout. The WDDM2 system-memory segment is implicit; specifying
the aperture ID selects system memory for GPU-virtual allocations. See the local
Microsoft documentation at revision110f60ea, display/gpu-segments.md, and
[GPU segments](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments).
The [residency enum](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmt_allocationresidencystatus)
does not expose a SegmentId. The primary acceptance of this control is actual
GPU copy contents, padding preservation and fence completion; it cannot claim
independent physical-placement validation. No placement conclusion is silently
substituted by a PASS label. DDI integration and full G0 acceptance remain open.

The runner now takes an explicit fresh -OutDir under C:\BC250\m13 with a
three-digit run suffix. It still rejects an existing start receipt, validates
the executable hash, and requires all five content and30 residency receipts.
