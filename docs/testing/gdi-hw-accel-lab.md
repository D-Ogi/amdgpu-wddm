# Lab plan: kernel GDI hardware acceleration

This page is the lab plan for the GDI acceleration of [docs/design/gdi-hw-accel.md](../design/gdi-hw-accel.md).
Both increments are in the driver, the kernels are in the image, and the driver reports the
capability, so this plan applies to a build of `feat/gdi-hw-accel`. Nothing of it has run yet.

Run it only on a build whose start log says `kernels 1`. On an image without the kernels
`Bc250GdiKernelsPresent` returns 0, the driver reports the caps of the previous release, and nothing
below tests the new code.

The plan has five trials. Trial 1 and trial 2 are the HLK job on the 26200 system. Trial 3 is the
desktop check on the 22631 system. Trial 4 is the off switch. Trial 5 measures the processor's
access to a CPU-visible GDI surface, which is the one condition of the GDI acceleration page that
the host tests cannot answer. Every trial outside the HLK job ends inside three minutes.

## Before the first trial

1. Read the lab state in `STATE.md` and check which driver is installed.
2. Check the thermal rule, which is the campaign's and not a new one: stop when Tctl holds 87
   degrees Celsius for 10 seconds, or at once at 89, and cool the board down between trials.
3. Install the driver under test. Check the SHA256 of the loaded KMD against the built file.
4. Read the guard log. Find the line `wddm: GDI hardware acceleration gate 1, kernels 1,
   capability 1`, and the line behind it, `wddm: gdi code 6 kernels, 5120 bytes, sha256 <hash>`.
5. Compare that hash with `BC250_GDI_CODE_SHA256` of
   [driver/kmd/gdi_kernels.generated.h](../../driver/kmd/gdi_kernels.generated.h). A different hash
   means the installed driver is not the build under test.
6. Check the registry value. `EnableGdiHwAccel` must be 1 under the adapter's `DefaultsV2` key.
7. Start the thermal guard for the whole pass, and the camera watch beside it (the owner's
   suspension of the camera was revoked on 2026-10-12, which `CLAUDE.md` records in its lab camera
   paragraph).

If the capability line says `kernels 0`, stop. The driver reports no GDI acceleration, the HLK job
does not apply, and nothing below tests the new code.

## Trial 1: the HLK job, native

Run `GDIHW_Functional test for GDI HW ACC (v1.1)` on the 26200 VHDX system, x64 native.

1. Boot the 26200 entry and check that the HLK client reaches the controller.
2. Select the graphics adapter in HLK Studio and find the job in the Tests tab.
3. Start the job. Its command is `Gditest_hw.exe -tolerance -clipping -all`.
4. Watch the job to its end. Microsoft gives two minutes as the expected run time and 120 minutes as
   the timeout.
5. Save the HLK log package. Copy the guard log and the system event log beside it.

The job carries `Configdisplay.exe`, `Gditest_hw.exe` and `TDRWatch.exe`. It needs no restart and it
runs by itself. HLK offers it only while device bitmaps and GDI acceleration are both on, so an
absent job means the capability did not reach the operating system.

## Trial 2: the HLK job, WoW64

Run the same job in its WoW64 form on the same system. The 32-bit test binary exercises the 32-bit
side of the package. The driver's kernel path is the same, so a result that differs between the two
runs points at the package and not at the translation.

## Trial 3: the desktop check on 22631

Run this trial on the 22631 system, which is the default entry. Keep it inside three minutes.

1. Open the guard log in one window and the overlay in another.
2. Tell the owner through the overlay what is about to happen.
3. Open Notepad, Registry Editor and Device Manager. All three draw with GDI.
4. Drag a classic window across the screen. Drag it between the two monitors as well.
5. Open and close the Start menu, then open Explorer and scroll a long folder listing.
6. Resize a window by its border, slowly, so the operating system repaints it many times.
7. Take a screenshot with `mon.py screenshot` and compare it with what the camera sees.
8. Stop the driver. Read the `wddm: gdi totals` line that the stop writes.

## Trial 4: the off switch

Set `EnableGdiHwAccel` to 0, restart the machine, and repeat trial 3 in two minutes. The driver must
then report no GDI acceleration, write `gate 0` in its start line, and draw the desktop through the
present path as the previous release does. This trial proves the field off switch.

## Trial 5: what the processor pays for a CPU-visible surface

The GDI acceleration page asks for "no significant performance penalty when the CPU accesses GPU
memory", and nothing on this host can answer that. The design page records it as an assumption. This
trial measures it. Keep it inside three minutes.

1. Write a bounded test that allocates one `D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE` surface, maps it
   the way the CDD does, and writes it sequentially with the processor. 16 MiB, three passes.
2. Measure the same write into ordinary system memory of the same size in the same run.
3. Record both rates, the surface's segment and the mapping the driver gave it. Numbers only, no
   screen output.
4. Repeat once with the driver stopped, to separate our mapping from the board's memory.

What it decides: a penalty that makes the capability dishonest. BD-021 and BD-022 are this project's
own cache-policy findings, and the 2026-09-29 game110 history turned on a cached versus an uncached
mapping, so the direction of the answer cannot be assumed from shared physical memory. If the rate
is far below the system-memory one, the capability goes off in the release defaults and the number
goes into the facts graph with this evidence.

## What to watch

| What | Where | What it means |
| --- | --- | --- |
| A DWM crash | the system event log, and the screen itself | the compositor died while our commands drew its surfaces. Keep the dump |
| A timeout of the graphics engine | the guard log, event 4101, a black screen | a GDI DMA buffer did not finish. This part has no working GPU reset, so recover by a restart |
| An artefact on the screen | the camera, and `mon.py screenshot` | a dropped operation, a wrong rectangle or wrong arithmetic. Note the window and the operation |
| `wddm: RenderGdi1 ...` | the guard log | the path ran. The first lines carry the command count, the byte count and the dword count |
| `overflow1` in such a line | the guard log | one command buffer did not fit in 256 KiB. Operations were dropped. Record the counts |
| `call` above 0 in `wddm: gdi drops` | the stop lines | the driver refused a whole call. Every one of them dropped every operation of that call |
| `malformed`, `outside` above 0 | the same line | the command buffer did not hold what its own sizes said. Keep the log |
| `item-refused` above 0 | the `wddm: gdi overflow` line at the stop | an admitted item could not be described by either emitter. The item drew nothing |
| `batch-refused` above 0 | the `wddm: gdi batch-refused` line at the stop | a command buffer held more sub-rectangles than 256 KiB can guarantee, so that call drew nothing. Read `peak rects` beside it: that is what the buffer size has to grow to |
| `fallback` above 0 | the same line | an item gave up the per-row engine path for a dispatch, which is the capacity contract working, not a fault |
| the refusal classes | the same line | each count names one admission rule of `bc250_gdi_refusal`. A nonzero count is a case the host tests do not cover |
| `red-first` above 0 | the same line | a ColorFill reached a red-first destination, where the colour format of the contract is not certain |
| The temperature | `temp.py`, and the thermal guard | stop at 87 degrees Celsius. An HLK pass runs longer than any other trial here |

## Limits of this plan

Do not run the HLK job while the capability bit is clear. The job then does not appear, and its
absence is not a pass.

Do not read a passing HLK job as proof of exact arithmetic. The job runs with `-tolerance`, so it
accepts a small difference for each pixel. The bit exact statement comes from the host test
`driver/kmd/test/run_gdi_render.ps1` and not from the lab.

One question this plan does NOT answer: whether `DxgkDdiRenderGdi` would accept
`STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER` and call again with `MultipassOffset`, as the
`DxgkDdiRender` family does. The status is not in its documented table, where every value but
`STATUS_SUCCESS` bugchecks, and this part has no working GPU reset, so the experiment costs a
machine restart if the documentation is right. It stays out until there is a reason to spend that.

Do not run trial 3 and a game in the same session. A game's present path and the GDI path share the
graphics ring, and a timeout would then have two candidate causes.

Keep the first trial of a new image short and watch the screen. This is the first run of our own
gfx1013 machine code from a GDI DMA buffer, this part has no working GPU reset (facts M53), and a
kernel that hangs the graphics ring costs a restart of the machine.
