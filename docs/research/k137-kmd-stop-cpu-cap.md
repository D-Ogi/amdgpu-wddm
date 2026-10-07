# K137: a KMD stop leaves the CPU slower until a Windows restart

This is an investigation record of 2026-10-07 (defect BD-093). Its measured rows are facts
[M832, M833 and M834](../facts/kmd.md), and the raw files are in
[the K137 evidence directory](../../evidence/windows/2026-10-07-k137-stop-cpu-cap/README.md). This note holds
the constraints, the hypotheses, what killed each one, and the next tests. Read it before you plan another K137
measurement on the lab.

The source references are to the KMD 0.7.216.15 tree (branch `kmd/k137-cpu-baseline`). Line numbers change, so
this note names files and functions only.

## 1. The defect

After any stop of the GPU device with the native SMU owner on, the CPU of unit A is slower until the next Windows
restart. A `pnputil /restart-device`, a `/disable-device` and a driver update without a Windows restart all cause
it. One pinned busy PowerShell thread reads `% Processor Performance` 109 % (3481 MHz) after a boot and 87 %
(2779 MHz) after one stop, three runs of three (M832).

The cost is real. The Witcher 3 at the high preset ran 36.9 frames/s in this state and 44.8 frames/s after a
Windows restart (lab sessions 447 and 452, not in this repository). A CPU clock cap of 2800 MHz through SMU message
`0x8F` cost that game nothing (session 454). So the state is not a plain CPU clock cap.

Workaround: restart Windows after every driver update and after every live restart of the device. The
game-session harness refuses a trial below 3200 MHz (`cpu-ceiling.ps1` in the evidence directory).

## 2. Constraints

Every hypothesis must fit these. Each one has its file.

| ID | Constraint | Evidence |
|---|---|---|
| C1 | The stop makes the state, not the next start. The value falls while the device is disabled, and also after a stop whose next start failed | `k137-3-stopstart.txt`, `lab-ab-2.txt` |
| C2 | The stop's SMU message and the fan handback do not cause it. `StopDpmFloor` 0 sends no SMU message on the stop, and `EnableFanControl` 0 writes nothing to the fan chip. Both still give 87 % | `lab-ab-2.txt`, `lab-ab-3.txt`, `smu-log-r3-after.txt` |
| C3 | No clock or voltage that we can read changes. The SMU metrics give socclk 1254, memclk 450, dclk 1111 MHz and throttler `0x0000` in both states. The P-state table is the same. The KMD sends no CPU message | M833, `cpu-log-453.txt` |
| C4 | One reading tracks the state one for one: the gfx power at the GPU idle point (500 MHz, SMU VID 116, busy 0.0 %). It is 10.2 to 10.8 W after a boot and 17.2 W after a stop | M833 |
| C5 | The extra power is real. The smart plug read about 13 W more at the idle desktop in the K137 state (sessions 447 against 452 and 454, different sessions) | lab plug logs, not in this repository |
| C6 | The overlay does not cause or keep the state | `k137-5-overlay.txt` |
| C7 | A fixed GPU workload takes longer too. DWM's composition packet has the same size in both states. Its median GPU time goes from 0.190 ms to 0.259 ms at a higher reported clock | ETW comparison of sessions 406 and 447, not in this repository |
| C8 | `EnableNativeSmu` 0 is not a clean control. That start failed, so the arm also removed the GFX, PSP, RLC and GART bring-up | `k137-4-nosmu.txt`, `SmuPrepareClock` in `driver/kmd/startup.c` |
| C9 | A warm Windows restart is the only way back to the good state, and it is a risk. Two of four warm restarts in the bisect hung and needed an AC cycle | `boot-events-1.txt` |

C7 needs a correction of the lab notes. They quote "+71 % cycles" for the DWM packet and "+47 %" for the game
packet. The source files give +36 % and +33 % of wall time. Normalised to the reported clock, the GPU delivers
0.64 of the work per MHz on the DWM packet and 0.69 on the game packet.

## 3. What the stop does

The full-WDDM stop runs these steps in this order (`driver/kmd/pnp.c`): `CpuStop`, `DpmStop`, `SmuMetricsStop`,
`FanStop`, `HwmonStop`, `DpAudioStop`, `SmuOwnerStop`, `WddmStop`, `InteropStop`, `DcnStop`, `IhStop`,
`GfxPrepareStop`, `PspStop`, `GartPrepareStop`, `GfxStop`, `GartStop`, `VramStop` and `MmioStop`.

Four points of that sequence matter here:

- `DpmStop` applies the floor point, 1000 MHz with a forced VID. It is the only SMU message of the stop.
- `SmuOwnerStop` sends nothing. The allowlist in `driver/shim/bc250_clock.c` has no `UnforceGfxVid`, so the
  forced GFX voltage stays in the SMU after the driver leaves.
- `GfxPrepareStop` ends when `bc250_gfx_rlc_stop` sets `RLC_CNTL.RLC_ENABLE_F32` to 0.
- `PspStop` destroys the TMR. The next start asks the PSP to load the GFX and RLC firmware a second time in one
  boot.

A full-WDDM stop also leaves the GART translation off and does not write the firmware snapshot back
(`driver/kmd/gart.c`).

## 4. The hypotheses

An offline analysis ranked five hypotheses and then tried to refute each one. Three died on their mechanism in
review. The lab bundle of 14:51Z to 14:55Z then killed the other two.

| ID | Claim | Status | What decided it |
|---|---|---|---|
| H1 | The stop loses the RLC's own power gating. The ungated GFX island draws 6.4 W more, and the SMU answers with a chip-wide cut of the delivered clock | REFUTED on mechanism | Every start writes `RLC_CGCG_CGLS_CTRL` 0 and `RLC_PG_CNTL` 0 in `bc250_gfx_rlc_resume`, so gating is off in both states |
| H2 | The SMU holds a forced GFX operating point with no owner, and arbitrates the cores and the GPU down | REFUTED on mechanism | A fresh boot carries the same forced point and runs at 3481 MHz. The SMU fields that would show an arbitration do not change |
| H3 | The teardown and the second PSP load lose the droop and clock-stretch calibration. Both clock generators then stretch | REFUTED on mechanism | A clock that stretches down at the same VID cannot raise the idle gfx power. The extra 6.4 W needs a second mechanism |
| H4 | The stop leaves the memory hub or the fabric in a slower state | REFUTED by measurement | GPU memory bandwidth is the same in both states: copy 386 GB/s (M834) |
| H5 | The CPU loss is an artifact of the counter | REFUTED by measurement | A fixed-work chain with no memory traffic runs 2.2 to 2.3 times slower in the K137 state (M834) |

More detail on each:

- **H1.** The cold start does not keep the static per-WGP power gating that the PSP-started RLC sets. The
  RLC stage clears it at the same place in both starts (`bc250_cu_mode.c` records `RLC_PG_CNTL` 0x8 up to the
  constants stage and 0 after the RLC stage). Also, `pp_gfxoff` is 0 for full WDDM, so the RLC-to-SMU handshake
  is off in both states. The +6.4 W is real, but the gating story does not own it.
- **H2.** The force is real and never released, but it is the same in a good and a bad boot. The variant
  "forced, with no owner, across a teardown" names no state that the source shows. The claim that the
  driver-table address points at freed memory is wrong: the table is a fixed page in the reserved top of the
  VRAM carve-out (`driver/kmd/smu_metrics.c`). That code is also newer than the first K137 record.
- **H3.** The droop and stretch machinery exists (queue-3 messages `0x4B`, `0x4C`, `0x52`, `0x53`, all off our
  allowlist). No code of ours or of amdgpu sends them. The one test that would confirm H3 runs a droop
  calibration, which is a voltage-margin write. Do not run it.
- **H4.** It was the best fit for C7, because a composition blit is bandwidth work. `vkmembw` gives copy 386.3
  and 387.3 GB/s in the K137 state and 386.8 and 386.1 GB/s fresh. The memory path is not the cause.
- **H5.** It needed the work rate to stay the same while the counter fell. The work rate fell much more than the
  counter.

These ideas died before the ranking:

- **The extra power is a telemetry fiction.** The smart plug sees it (C5).
- **The second start forgets to enable clock gating.** Gating is off in both states (H1).
- **The floor apply on the stop.** `StopDpmFloor` 0 still gives the state (C2).
- **The fan handback.** `EnableFanControl` 0 still gives the state (C2).
- **The overlay.** C6.
- **The Windows power plan, CPPC or boost mode.** Earlier lab probes show these do not move the clock, and the CPU
  rail voltage is the same in both states.
- **A CPU clock cap or P-state limit.** The table, the granted clock and the CPU message counters do not change
  (C3). 2779 MHz is on no entry of the table.
- **A silent drop from 40 to 24 CU.** 24 CU gives about 33 frames/s at the high preset and 40 CU gives 46. The
  36.9 of the K137 state fits neither. Also, fewer CU lower the idle power.
- **A saturated package power limit.** The K137 session peaked 33 W below the healthy session.
- **No core enters its deep idle state after the stop.** This also happens in a good state (`lab-ab-1.txt`, state
  A, idle cores at 3500 MHz). It is a companion symptom at most.

## 5. The measurement that changed the picture

The bundle (`bundle.ps1`) timed a dependent chain of 64-bit multiply, add, shift and XOR on one thread, pinned to
logical processor 2, with no memory traffic. It also ran `vkmembw` in the same script. Four runs, two in each state,
on tester.20:

- Fresh boot: 501.7 to 581.2 iterations/us. K137 state: 195.4 to 259.5 iterations/us.
- The ratio of the means is 2.33. The ratio of the best runs is 2.24.
- GPU memory bandwidth: no change.

So the loss on ALU-bound code is about 2.2 times, not the 20 % that the counter probe shows. The counter
probe runs an interpreted PowerShell loop, which is a different mix of work. The counter that the bundle sampled
from a side job read 76 % to 78 % fresh and 87 % in the K137 state. That sampling method is not usable.

An inference from the chain, not a measurement: the chain has a dependency of about 6 cycles an iteration (a
3-cycle multiply and three 1-cycle operations on Zen 2). 581.2 iterations/us times 6 is 3487 MHz, which agrees
with the 3481 MHz of the counter probe in a fresh boot. 259.5 times 6 is 1557 MHz. The SMU metrics list 1555 MHz
as a clock of some cores in both states. Section 7 gives the test for this.

## 6. What holds now

- The state is made by the stop and stays until a Windows restart.
- The CPU loses delivered performance on ALU-bound code, by about 2.2 times on one thread.
- GPU memory bandwidth does not change.
- The GFX rail draws about 6.4 W more at the same idle point, and the wall meter sees extra power.
- No readback of the SMU that we decode shows a change. These readbacks can echo targets, not delivered clocks.

Open:

1. Do the CPU loss, the GPU loss of C7 and the extra gfx power have one cause?
2. Does SMU message `0x43` report a delivered clock? The header of `driver/shim/include/bc250_cpu.h` calls it the
   effective clock, and the undervolt guard (`BC250_CPU_STRETCH_MHZ`) depends on that meaning. Its answers come in
   a few steps only (3500, 1555, 1400, 933 and 900 MHz), which looks like a table of grants.
3. Is the core clock in the K137 state near 1555 MHz, or does the core run at its target with stalls?
4. Is the state in the GFX or PSP teardown, or in the SMU owner? No clean control exists. `EnableNativeSmu` 0
   failed to start (C8). `EnableFullWddm` 0 with the owner on is no control either, because a start without full WDDM
   sends no clock message.

## 7. The next tests

Every test below stays under three minutes. A device restart enters the K137 state cheaply. A warm Windows
restart leaves it and carries the hang risk of C9, so plan one warm restart for a whole set of tests.

1. **`0x43` under load.** Run `bc250kmd_cli cpu` inside the load window, before the job stops, and print the
   per-core answer beside the metrics table. Add `% Processor Time` of instance (0,2). If `0x43` reads about 1555
   MHz or 2780 MHz on the loaded core in the K137 state, it is a delivered clock. If it reads 3500 MHz in both states
   while the chain runs 2.2 times slower, our stretch guard is blind.
2. **Two chain lengths.** Run the fixed-work chain with a 6-cycle and a 12-cycle dependency in both states. A
   clock loss scales both by the same factor. Stalls or duty cycling can scale them differently.
3. **The start log, unfiltered.** `smu-log.ps1` filters out the `cumode:` line, which prints `RLC_PG_CNTL` and
   its power-gating enables at every start. Capture it in both states. If both read the same, H1 is dead by its
   own condition.
4. **A runtime release of the GFX force.** In the K137 state, send `RequestGfxclk` 1500 MHz and then
   `UnforceGfxVid` from an escape, with a `GetGfxVid` readback. This needs one allowlist entry for that escape
   only. If the state stays, H2 is dead without a stop-path change. Do not test the release in the stop path first:
   the last stop-path arm (`StopDpmFloor` 0) made the next start fail, and the warm restart after it hung.
5. **Fixed GPU work at a fixed clock.** Write `DpmMaxMHz` 1000 before the warm restart (the KMD reads it at each
   device start), then time one fixed compute dispatch in both states. Read `RLC_GPU_CLOCK_COUNT` against
   `RLC_REFCLOCK_TIMESTAMP` beside it, which needs these names in the CLI read list.
6. **A free baseline at every boot.** Run the measurement part of the bundle from the lab's startup task. Then
   every boot that the lab takes for another reason records a fresh state, and a test needs only a device restart.

Safety: the 87 C and 89 C thermal stops and the 300 W PSU limit apply. The KMD stays the only SMU owner. Do not
send the droop calibration messages.

## 8. Related defects

BD-094 is a separate defect: a revert of the CPU surface restored 3200 MHz, the top P-state, below the
firmware's boost. Branch `kmd/k137-cpu-baseline` fixes the restore value. Keep the two defects apart.
