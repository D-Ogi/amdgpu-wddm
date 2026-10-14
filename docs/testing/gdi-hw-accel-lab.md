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
3. Put the driver under test in place. An installed package is one way. A single-image arm is the
   other: the shared KMD lab kit puts the image in the driver store folder the service loads from,
   keeps the original by hash and gives it back afterwards. Check the SHA256 of the loaded KMD
   against the built file either way.
4. Read the guard log. Find the line `wddm: GDI hardware acceleration gate 1, kernels 1,
   capability 1`, and the line behind it, `wddm: gdi code 6 kernels, 5120 bytes, sha256 <hash>`.
5. Compare that hash with `BC250_GDI_CODE_SHA256` of
   [driver/kmd/gdi_kernels.generated.h](../../driver/kmd/gdi_kernels.generated.h). A different hash
   means the installed driver is not the build under test.
6. Check the registry value. `EnableGdiHwAccel` must be 1 under the adapter's `Parameters` key,
   where the package INF writes it with NOCLOBBER.
7. Start the thermal guard for the whole pass, and the camera watch beside it where the operator's
   own lab rules allow a camera.

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

Nothing has to be configured for the second producer of these commands. dxgkrnl builds GDI commands
of its own for the driver tests, with the sub-rectangle array inside the command's fixed part, and
that producer is gated by a registry value or by a flag of the calling process, not of the adapter
(the design page, "The second producer, and the shape it builds"). The driver admits the shape either
way, so the trial is valid whether or not the HLK client was put into that mode.

Record the configuration, and record it as configuration. Read that registry value on the HLK client
before the run and note the answer beside the log package. Its name and key are in the local notes
beside the binaries, outside this repository. An enabled gate does not say that the test called the
producer, that the embedded-subrectangle branch was taken, or that such a command reached this
driver, and the per-process route cannot be read from the registry at all, so an absent value does
not exclude it. Only the driver's own counters say what arrived: `GdiRenderCommands` with
`GdiRenderPeakCommandLength` for the calls, and the guard log for the shapes it dropped. Do not
report the embedded shape as exercised unless a counter or a log line shows it.

## Trial 2: the HLK job, WoW64

Run the same job in its WoW64 form on the same system. The 32-bit test binary exercises the 32-bit
side of the package. The driver's kernel path is the same, so a result that differs between the two
runs points at the package and not at the translation.

## Trial 3: the desktop check on 22631

Run this trial on the 22631 system, which is the default entry. Keep it inside three minutes.

The work itself is driven and not typed: an SSH session has no desktop, so the workload runs in the
lab user's own session as a one-shot task, the way the game input server does. It opens Registry
Editor and Device Manager, which are classic windows the CDD draws through GDI, moves each of them
across the whole virtual screen, resizes them by steps, scrolls their own classic controls, and
closes what it opened. The owner sees all of it happen on the lab's screen.

1. Tell the owner through the overlay what is about to happen.
2. Read the driver's GDI lines. The start line must say `gate 1`. Record the
   `wddm: CreateContext: GDI dma <bytes> bytes for cmdbuf <bound>` line of the first GDI context and
   compare the two numbers with the design: at the aperture of this lab the bound is 196608 bytes and
   the DMA buffer is 3989504 bytes, which is three orders larger than the page-sized buffer of every
   other context, and this trial is the first time dxgkrnl is asked for it. A context creation that
   fails here, or a line that never appears, is the result, and the bound can be lowered with the
   arithmetic of `Bc250GdiContextDmaBytes`.
3. Run the workload once, bounded, and keep the record it writes: the windows it opened, and how
   many moves, resizes, scrolls and forced repaints it asked for.
4. Take a screenshot with `mon.py screenshot` and compare it with what the camera sees. Both must
   show an ordinary desktop: no torn window, no black rectangle, no stale content.
5. Read the driver's GDI lines again, and the totals of the stop. The stop writes
   `wddm: gdi totals` into the log ring, so the log file of the stop (`KeepLog`) carries it over the
   restart that stopped the driver. A stop of the device on purpose is not needed, and it would
   leave the processor slower until the next restart (K137).
6. Check the counters of that line: `item-refused`, `batch-refused` and `fallback`. The first two
   must be 0 in an ordinary desktop session.
7. Record `wddm: gdi capacity-short<n> peak cmdlen<bytes> want<bytes>`. `peak cmdlen` is the largest
   command buffer the CDD actually sent. It is not what the bound was chosen against: the bound comes
   from what one indirect buffer can carry (`BC250_GDI_CMDBUF_BOUND`, 196608 bytes), and this number
   says how far the field stays below it. It must stay below the bound, `capacity-short` must be 0,
   and the expectation from the producer measurement is that it stays at or under 65536 bytes. Keep
   it beside `peak rects`, which says how many sub-rectangles the largest call carried.
8. Record the lines of rule 3 and the input scope: `wddm: gdi ib-full<n> ib-over<n> dma-short<n>`,
   `wddm: gdi above-producer<n> scope-refused<n>`, `wddm: gdi dma append<n> submitted<n>
   raced<n>`, `wddm: gdi dma unknown<n> full<n> small<n> lost<n>` and `wddm: gdi committed
   dwords<n>`. **Any drawing this driver dropped is a failed trial.** `batch-refused` is the count
   of dropped calls and `omitted subrects` the drawing lost with them, whatever stopped the call: a
   command buffer above the bound, a buffer an earlier call had filled (`ib-full`), a window too
   small for the smallest batch (`small`), a window that could not be placed or a context with
   every slot taken (`unknown`, `full`), or the unreachable checks (`ib-over`, `lost`,
   `no-space`). None of them is an acceptable observation about the operating system's capacity in
   an ordinary desktop session, and each of them stops the trial series until the design page's
   limitation section or the driver accounts for it. `ib-over`, `lost` and `dma-short` are states
   the design says cannot happen at all, so a nonzero value there is a defect of the driver.
   `append`, `submitted` and `raced` are measurements of how dxgkrnl actually uses one DMA buffer,
   and `above-producer` whether anything on this system sends more than the ordinary producer's own
   65536-byte command buffer. Those four are measurements and no failure by themselves, and a
   nonzero `raced` is the first witness of a submission arriving while a call of this context is
   still translating: record it with the whole pair of `dma` lines, because the design is prepared
   for that ordering and has never seen it. Compare `committed dwords` with `dwords` of the totals
   line: they are equal when nothing was dropped after its stream was written.

## Trial 4: the off switch

Set `EnableGdiHwAccel` to 0, restart the machine, and repeat trial 3 in two minutes. The driver must
then report no GDI acceleration, write `gate 0` in its start line, and draw the desktop through the
present path as the previous release does. This trial proves the field off switch.

## Trial 5: what the processor pays for a CPU-visible surface

The GDI acceleration page asks for "no significant performance penalty when the CPU accesses GPU
memory", and nothing on this host can answer that. The design page records it as an assumption. This
trial measures it with [tools/win/gdi-cpuwrite](../../tools/win/gdi-cpuwrite/gdi-cpuwrite.c). Keep it
inside three minutes.

The operating system's own GDI surface is out of reach of a user-mode process.
`D3DKMT_CREATESTANDARDALLOCATION` of WDK 10.0.26100 holds one union member, `ExistingHeapData`
(`ref/ddi-display/d3dkmthk.md:21498` of the workspace reference set). No process can ask dxgkrnl for
a `D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE` surface from user mode. The tool therefore asks this driver
directly, with a 48-byte LB7A block and the GDI1 trailer of type 2. CreateAllocation then runs
`Bc250Lb7aAdmit` and places that allocation with the same `WddmGdiAllocationPolicy` call a staging
surface of the CDD takes. The measurement covers our own mapping policy, which is what the assumption
is about.

1. Build the client with `tools/win/gdi-cpuwrite/build.ps1`. The build compares both private-data
   magic numbers and the four policy lines against `driver/kmd/gdi_private.h`, so a tool that reports
   a placement the driver no longer makes does not build.
2. Run it on the lab, 16 MiB and three passes. It opens no window and it needs no desktop.
3. Record the `POLICY`, `CPUWRITE` and `PENALTY` lines. One run covers three memory classes: ordinary
   committed pages, a type-0 surface and a type-2 staging surface.
4. Record all three kinds of access to each class. The store, the copy and the read are each a kind
   the kernel fallback performs.

The baseline is cached memory of the same size, and 16 MiB is partly in the processor's cache. That
is the comparison this question needs. A GDI operation touches a small rectangle, so the kernel
fallback gets the same cache.

A stop of the driver is not part of this trial. The allocation needs our device, which a stopped
driver does not give. A device stop also leaves the processor about 20 percent slower until a restart
(K137), which would make the two halves of such a comparison different machines. The system-memory
class of the same run is the separation the plan asked for.

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
| `overflow1` in such a line | the guard log | an item found no space after the batch was guaranteed, which the induction of `gdi_batch.c` makes unreachable. The whole call is dropped rather than drawn in part. A defect of the driver: record the counts and keep the log |
| `call` above 0 in `wddm: gdi drops` | the stop lines | an argument of the call failed the admission tests, so the driver could not read the command buffer or write the DMA buffer. Every one of them dropped every operation of that call, and it is lost drawing like any other drop. The sub-rectangles are not in `omitted subrects`, because a call that cannot be walked cannot be counted |
| `malformed`, `outside` above 0 | the same line | the command buffer did not hold what its own sizes said. Keep the log |
| `item-refused` above 0 | the `wddm: gdi overflow` line at the stop | an admitted item could not be described by either emitter. The item drew nothing |
| `batch-refused` above 0 | the `wddm: gdi batch-refused` line at the stop | a whole call drew nothing, whatever stopped it: every drop of a call reaches this counter, and `omitted subrects` is the drawing lost with them. In an ordinary desktop session this is a failed trial. Read `capacity-short`, `peak cmdlen`, `ib-full`, `small`, `unknown` and `scope-refused` beside it: together they say what the OS sent and which limit the call met |
| `capacity-short` above 0 | the `wddm: gdi capacity-short` line at the stop | a call's worst case was larger than the capacity it was given. The cause is a command buffer above the bound, or an earlier call that took part of the same DMA buffer. By itself it costs nothing, because the call is still translated when what is in it fits. Record `peak cmdlen`. The bound cannot be raised far. One indirect buffer carries the worst case of 200 KiB and no more. A producer above that needs the compaction or multi-indirect-buffer work of the design page, not a larger number |
| `scope-refused` above 0 | the `wddm: gdi above-producer` line at the stop | a call at or below the ordinary producer's own 65536-byte command buffer drew nothing. This is a failed trial and it stops the series: an ordinary paint was lost. Keep the log, the length, the reason the dropped line names and the sub-rectangle count. A zero here is not a witness that nothing was dropped: read `omitted subrects` as well |
| `ib-over` above 0 | the same line | the finished stream of a call passed what one indirect buffer can carry. The capacity arithmetic makes this unreachable, so a nonzero count is a defect of the driver and not of the input. The call drew nothing |
| `ib-full` above 0 | the same line | a call found the DMA buffer already filled by earlier calls to the length one indirect buffer can carry, so it drew nothing. It is a dropped paint, not an acceptable capacity observation: treat it as a failed trial and record `dma append`, `submitted` and `raced` beside it |
| `dma-short` above 0 | the same line | a call at the start of a buffer reported less space than this context asked for, which `DXGK_CONTEXTINFO` says cannot happen. Keep the log |
| `unknown`, `full`, `small` above 0 | the `wddm: gdi dma unknown` line at the stop | a call's place in its DMA buffer could not be established. Or every one of the eight slots was taken. Or the window of the call was too small for the smallest batch. The call drew nothing in each case. Record the whole `dma` pair of lines: these say which ordering of calls and submissions the OS produced |
| `raced` above 0 | the `wddm: gdi dma append` line at the stop | a submission named a buffer while a call of this context was still translating into it. The slot was kept, so nothing was lost. This is the first observation of that ordering: record it with the `dma` lines and the submission's own log line |
| `lost` above 0 | the `wddm: gdi dma unknown` line at the stop | a call could not publish what it had written, so its whole call was dropped and its pointer was not advanced. The design says this cannot happen: a nonzero value is a defect of the driver, with the `dma publish refused` line beside it |
| `committed dwords` below `dwords` | the `wddm: gdi committed` and `wddm: gdi totals` lines | a call's stream was generated and then dropped. The difference is dwords that never reached the ring, and the reason is in the `dropped` line of that call |
| `above-producer` above 0 | the same line | something on this system sends longer command buffers than the measured producer does. Not a fault. It is the signal that the producer measurement no longer covers the field |
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
