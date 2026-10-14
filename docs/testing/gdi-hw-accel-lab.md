# Lab plan: kernel GDI hardware acceleration

This page is the lab plan for the GDI acceleration of [docs/design/gdi-hw-accel.md](../design/gdi-hw-accel.md).
Both increments are in the driver, the kernels are in the image, and the driver reports the
capability, so this plan applies to a build of `feat/gdi-hw-accel`. Nothing of it has run yet.

Run it only on a build whose start log says `kernels 1`. On an image without the kernels
`Bc250GdiKernelsPresent` returns 0, the driver reports the caps of the previous release, and nothing
below tests the new code.

The plan has four trials. Trial 1 and trial 2 are the HLK job on the 26200 system. Trial 3 is the
desktop check on the 22631 system. Trial 4 is the off switch. Every trial outside the HLK job ends
inside three minutes.

## Before the first trial

1. Read the lab state in `STATE.md` and check which driver is installed.
2. Check the thermal rule. Stop any trial at 87 degrees Celsius and cool the board down.
3. Install the driver under test. Check the SHA256 of the loaded KMD against the built file.
4. Read the guard log. Find the line `wddm: GDI hardware acceleration gate 1, kernels 1,
   capability 1`, and the line behind it, `wddm: gdi code 6 kernels, 5120 bytes, sha256 <hash>`.
5. Compare that hash with `BC250_GDI_CODE_SHA256` of
   [driver/kmd/gdi_kernels.generated.h](../../driver/kmd/gdi_kernels.generated.h). A different hash
   means the installed driver is not the build under test.
6. Check the registry value. `EnableGdiHwAccel` must be 1 under the adapter's `DefaultsV2` key.
7. Start the thermal guard and the camera watch for the whole pass.

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
| `kernel-refused` above 0 | the `wddm: gdi overflow` line at the stop | an admitted item reached the dispatch and its argument block could not be built. The item drew nothing |
| the refusal classes | the same line | each count names one admission rule of `bc250_gdi_refusal`. A nonzero count is a case the host tests do not cover |
| `red-first` above 0 | the same line | a ColorFill reached a red-first destination, where the colour format of the contract is not certain |
| The temperature | `temp.py`, and the thermal guard | stop at 87 degrees Celsius. An HLK pass runs longer than any other trial here |

## Limits of this plan

Do not run the HLK job while the capability bit is clear. The job then does not appear, and its
absence is not a pass.

Do not read a passing HLK job as proof of exact arithmetic. The job runs with `-tolerance`, so it
accepts a small difference for each pixel. The bit exact statement comes from the host test
`driver/kmd/test/run_gdi_render.ps1` and not from the lab.

Do not run trial 3 and a game in the same session. A game's present path and the GDI path share the
graphics ring, and a timeout would then have two candidate causes.

Keep the first trial of a new image short and watch the screen. This is the first run of our own
gfx1013 machine code from a GDI DMA buffer, this part has no working GPU reset (facts M53), and a
kernel that hangs the graphics ring costs a restart of the machine.
