# K137: a KMD stop leaves the CPU slower until a Windows restart

This is an investigation record of 2026-10-07 (defect BD-093). Its measured rows are facts
[M832, M833 and M834](../facts/kmd.md), and the raw files are in
[the K137 evidence directory](../../evidence/windows/2026-10-07-k137-stop-cpu-cap/README.md). This note holds
the constraints, the hypotheses, what killed each one, and the next tests. Read it before you plan another K137
measurement on the lab.

**Sections 1 to 8 are the record of 2026-10-07. [Section 9](#9-the-arm-of-2026-10-12) is the arm of
2026-10-12. It answers two of the four open questions of [section 6](#6-what-holds-now) and narrows the other
two, runs three of the six next tests of [section 7](#7-the-next-tests), and refutes one inference of
[section 5](#5-the-measurement-that-changed-the-picture). [Section 10](#10-the-second-arm-of-2026-10-12) is the
second arm of the same day. It refutes the clock that section 9 found and measures the cost that replaces it.
Read section 10 first, then section 9, then the section you need.**
The marks `REFUTED 2026-10-12` and `ANSWERED 2026-10-12` in the older sections point at section 9. The marks
`ROUND 2` point at section 10.

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
| C10 | The state keeps the cost of a dependent chain and loses instruction-level parallelism. Seven kernels calibrate one clock before the stop and seven clocks after it. The two pure latency chains keep 3.0 cycles an operation at about 2782 MHz. `tput_imul` falls to the `dep_imul` rate and `tput_add` keeps 0.149 of its rate | M877 |
| C11 | A PnP stop and start of a device that is not the GPU leaves the work rate unchanged (ratio 1.000). The device was root-enumerated software, so this excludes the OS PnP path and not a PCI function teardown | M885 |
| C12 | Main-memory latency does not change (64 MiB chase flat to 0.4 %) and L1 load-to-use costs 2.33 cycles more | M881 |

C7 needs a correction of the lab notes. They quote "+71 % cycles" for the DWM packet and "+47 %" for the game
packet. The source files give +36 % and +33 % of wall time. Normalised to the reported clock, the GPU delivers
0.64 of the work per MHz on the DWM packet and 0.69 on the game packet.

The arm of 2026-10-12 adds C10 to C12 and strengthens three of the older rows. C4 and C5 now hold inside one
boot and on two instruments ([M882](../facts/kmd.md#m882)). C9 gained a further hung warm restart
([M886](../facts/kmd.md#m886)). Section 9 states the new rows with their numbers. C10 is the one every
hypothesis now has to fit.

**ROUND 2.** The clock of about 2782 MHz in C10 is refuted, and the L1 cycle count of C12 rested on it.
Emitted latency chains put the clock of the state at 3396.5 MHz or more ([M887](../facts/kmd.md#m887)). Each
independent instruction costs about 1.81 cycles of issue in the state against 0.265 after a boot
([M889](../facts/kmd.md#m889)). C10 keeps its rates and its conclusion that the cost of an operation changed.
Section 10 has the numbers.

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

The arm of 2026-10-12 touches three of these verdicts. H1 keeps its verdict and now has a register witness for
its own condition ([M883](../facts/kmd.md#m883)). H4 keeps its verdict and gains the latency half that the
bandwidth result could not give ([M881](../facts/kmd.md#m881)). **H5 was refuted for the wrong reason.** The
counter is right in both states, and the work rate of a throughput-bound kernel falls further because the cost
of an operation also rose ([M878](../facts/kmd.md#m878)).

**ROUND 2.** The counter is not right in the K137 state. It reads 0.81 to 0.84 of the clock that emitted
latency chains give in the same leg ([M888](../facts/kmd.md#m888)). H5 keeps its verdict on the work rate
alone. Section 10 has the numbers.

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

**REFUTED 2026-10-12.** The chain costs about 10.7 cycles an iteration in the K137 state, not 6
([M877](../facts/kmd.md#m877)). The 1557 MHz of the paragraph above is the artefact of holding that cost fixed.
The core runs at about 2782 MHz in the K137 state, which the Windows counter reported all along
([M878](../facts/kmd.md#m878)). Section 9 has the numbers.

**ROUND 2.** The 2782 MHz clock and the agreement with the counter of the paragraph above are refuted. Two
emitted latency chains put the clock at 3396.5 MHz or more in the K137 state, which is 0.976 of the healthy
clock ([M887](../facts/kmd.md#m887)). At that clock the chain of this section costs about 13.1 cycles an
iteration, not 10.7. The 1557 MHz reading stays refuted. Section 10 has the numbers.

## 6. What holds now

- The state is made by the stop and stays until a Windows restart.
- The CPU loses delivered performance on ALU-bound code, by about 2.2 times on one thread.
- GPU memory bandwidth does not change.
- The GFX rail draws about 6.4 W more at the same idle point, and the wall meter sees extra power.
- No readback of the SMU that we decode shows a change. These readbacks can echo targets, not delivered clocks.

Open:

1. Do the CPU loss, the GPU loss of C7 and the extra gfx power have one cause? **Still open 2026-10-12.** The
   CPU loss and the extra power have one trigger inside one boot ([M882](../facts/kmd.md#m882)). One trigger is
   not one cause, and the GPU half has no instrument that has worked yet (section 9).
2. Does SMU message `0x43` report a delivered clock? The header of `driver/shim/include/bc250_cpu.h` calls it the
   effective clock, and the undervolt guard (`BC250_CPU_STRETCH_MHZ`) depends on that meaning. Its answers come in
   a few steps only (3500, 1555, 1400, 933 and 900 MHz), which looks like a table of grants.
   **ANSWERED 2026-10-12:** no. `0x43` prints the same per-core pattern in both states, so the guard is blind to
   this state ([M879](../facts/kmd.md#m879)).
   **ROUND 2:** the answer stands, and the gap it hides is small. `0x43` reads 3500 MHz on core index 1 in
   every readback, at most 103.5 MHz above the latency clock of the state ([M893](../facts/kmd.md#m893)). The guard misses the
   issue cost, which no clock reading shows.
3. Is the core clock in the K137 state near 1555 MHz, or does the core run at its target with stalls?
   **ANSWERED 2026-10-12:** neither of the two. The core runs at about 2782 MHz, which is 0.8 of the healthy
   clock, and the cost of an operation rises on top of that ([M877](../facts/kmd.md#m877)).
   **ROUND 2:** the 2782 MHz and the 0.8 are refuted. The core runs at 3396.5 MHz or more, 0.976 of the healthy
   clock ([M887](../facts/kmd.md#m887)). Each independent instruction costs 6.8 times its healthy issue cycles
   ([M889](../facts/kmd.md#m889)). So the second half of the question holds: the core runs near its target
   and issues far less.
4. Is the state in the GFX or PSP teardown, or in the SMU owner? No clean control exists. `EnableNativeSmu` 0
   failed to start (C8). `EnableFullWddm` 0 with the owner on is no control either, because a start without full WDDM
   sends no clock message. **Still open 2026-10-12.** The non-GPU control fired and read a ratio of 1.000, so the
   OS PnP path alone is excluded, but the control was a software device
   ([M885](../facts/kmd.md#m885)).
   **ROUND 2:** the second arm drops the `EnableNativeSmu` 0 control on purpose (section 10.7).

## 7. The next tests

Every test below stays under three minutes. A device restart enters the K137 state cheaply. A warm Windows
restart leaves it and carries the hang risk of C9, so plan one warm restart for a whole set of tests.

1. **`0x43` under load.** Run `bc250kmd_cli cpu` inside the load window, before the job stops, and print the
   per-core answer beside the metrics table. Add `% Processor Time` of instance (0,2). If `0x43` reads about 1555
   MHz or 2780 MHz on the loaded core in the K137 state, it is a delivered clock. If it reads 3500 MHz in both states
   while the chain runs 2.2 times slower, our stretch guard is blind.
   **DONE 2026-10-12.** The second branch held ([M879](../facts/kmd.md#m879)). The verb is `cpu readback`, not
   a bare `cpu` call, which prints the cached start-time snapshot.
2. **Two chain lengths.** Run the fixed-work chain with a 6-cycle and a 12-cycle dependency in both states. A
   clock loss scales both by the same factor. Stalls or duty cycling can scale them differently.
   **DONE 2026-10-12, and widened.** Seven kernels ran instead of two chain lengths, and they scale by seven
   different factors ([M877](../facts/kmd.md#m877)).
3. **The start log, unfiltered.** `smu-log.ps1` filters out the `cumode:` line, which prints `RLC_PG_CNTL` and
   its power-gating enables at every start. Capture it in both states. If both read the same, H1 is dead by its
   own condition.
   **DONE 2026-10-12.** The value after the RLC stage is the same in both states, so H1 is dead by its own
   condition ([M883](../facts/kmd.md#m883)).
4. **A runtime release of the GFX force.** In the K137 state, send `RequestGfxclk` 1500 MHz and then
   `UnforceGfxVid` from an escape, with a `GetGfxVid` readback. This needs one allowlist entry for that escape
   only. If the state stays, H2 is dead without a stop-path change. Do not test the release in the stop path first:
   the last stop-path arm (`StopDpmFloor` 0) made the next start fail, and the warm restart after it hung.
   **Not run. Still the test it was.**
   **ROUND 2:** a DPM floor of 1500 MHz in the K137 state left the state in place
   ([M891](../facts/kmd.md#m891)). That arm sent no `UnforceGfxVid`, so this test is still owed.
5. **Fixed GPU work at a fixed clock.** Write `DpmMaxMHz` 1000 before the warm restart (the KMD reads it at each
   device start), then time one fixed compute dispatch in both states. Read `RLC_GPU_CLOCK_COUNT` against
   `RLC_REFCLOCK_TIMESTAMP` beside it, which needs these names in the CLI read list.
   **The MMIO instrument of this test is gone.** `RLC_GPU_CLOCK_32` counted the 100 MHz reference at both GFX
   clock points sampled, 500 and 1700 MHz, with `RES_SEL` at its reset value, and the `RLC_GPU_CLOCK_COUNT` pair
   read zero without a capture write ([M884](../facts/hardware.md#m884)). Rebuild this test around a timed
   dispatch at a pinned clock with no counter, and try the one instrument that is left untried: the
   subgroup-scope clock of `VK_KHR_shader_clock`, which compiles to `s_memtime` on gfx1013, counted over a known
   wall interval from inside the dispatch.
6. **A free baseline at every boot.** Run the measurement part of the bundle from the lab's startup task. Then
   every boot that the lab takes for another reason records a fresh state, and a test needs only a device restart.
   **Not run. Still worth the startup task, now with the seven-kernel probe of section 9.**

Safety: the 87 C and 89 C thermal stops and the 300 W PSU limit apply. The KMD stays the only SMU owner. Do not
send the droop calibration messages.

## 8. Related defects

BD-094 is a separate defect: a revert of the CPU surface restored 3200 MHz, the top P-state, below the
firmware's boost. Branch `kmd/k137-cpu-baseline` fixes the restore value. Keep the two defects apart.

## 9. The arm of 2026-10-12

Ten steps ran on unit A between 08:25Z and 08:34Z on 2026-10-12, all inside one boot. State A is the healthy
state after the boot of 07:49:14. State B is the state after one `pnputil /restart-device` of the GPU at
08:30:35Z. The raw files are in
[the 2026-10-12 evidence directory](../../evidence/windows/2026-10-12-k137-state-ab/README.md), which gives the
order of the steps, the instruments and the per-step verdicts. The measured rows are facts
[M877 to M883, M885 and M886](../facts/kmd.md) and [M884](../facts/hardware.md#m884).

The arm wrote no register, sent no SMU setter and changed no clock, voltage or firmware setting. It spent one
GPU device restart, one non-GPU device restart and one warm Windows restart. Every call stayed under three
minutes. The highest temperature was 67.2 C.

### 9.1 What it measured

**The CPU half is not a clock deficit.** Seven kernels of one pinned thread calibrate one clock in state A, 3488.5
to 3489.8 MHz over all seven, a spread of 0.04 %. In state B the same arithmetic gives seven clocks between
520.4 and 2790.2 MHz, so at least one cost an operation changed. The reading that fits every row is a clock of
about 2782 MHz with a collapse of instruction-level parallelism on top of it.

**ROUND 2.** The rates of the table below reproduce within 1.1 %, but the reading is refuted. The clock is
3396.5 MHz or more ([M887](../facts/kmd.md#m887)), so the last column of the table does not hold. The two
"pure latency chains" are managed loops that are rate bound in the state.

| Kernel | cycles/op | A ops/us | B ops/us | B over A | modelled cycles/op in B at 2782 MHz |
|---|---|---|---|---|---|
| `loop_add` | 1 | 3488.82 | 1817.83 | 0.521 | 1.53 |
| `mix_xoradd` | 1 | 3489.71 | 1274.62 | 0.365 | 2.18 |
| `dep_imul` | 3 | 1162.91 | 927.43 | 0.798 | 3.00 |
| `dep_fpadd` | 3 | 1163.27 | 930.08 | 0.800 | 2.99 |
| `dep_mix` | 6 | 581.55 | 259.40 | 0.446 | 10.72 |
| `tput_imul` | 1 | 3488.73 | 929.30 | 0.266 | 2.99 |
| `tput_add` | 0.3125 | 11163.24 | 1665.33 | 0.149 | 1.67 |

The `cycles/op` column is the design of each kernel on a 4-wide integer core, and `tput_add` is the one entry
that needs its loop counted: 8 independent adds plus the index increment and the compare are 10 operations, so
2.5 cycles an iteration over 8 counted operations is 0.3125. The low end of the state-B span, 520.4 MHz, is
that column times the measured rate, so it is modelled and not read.

The two pure latency chains keep 3.0 cycles an operation each. `tput_imul` falls to the `dep_imul` rate, so no
two independent multiplies overlap any more. `tput_add` keeps 0.149 of its healthy rate, the worst loss of the
seven. Every state-B rate is one run, and the same `dep_mix` kernel spans 253.16 to 296.18 operations/us over
the five modes of this leg in state B, a spread of 17 %, against 1.8 % in state A: read the state-B column to
one decimal of a cycle, not two. The ratios are far outside that spread. Keep this table. It is the signature of
the state ([M877](../facts/kmd.md#m877)).

**The clock itself is 0.8 of the healthy one, and Windows reported it all along.** The APERF over MPERF counter
reads 108 % under load in state A, which is 3449.5 MHz against the kernels' 3488.8 MHz, and 91, 86 and 87 % in
state B, which is 2906.5, 2746.8 and 2778.8 MHz against the latency chains' 2782.3 MHz. One counter step is
32 MHz. In state A the two instruments are 39 MHz apart, which is 1.2 steps, so they agree to about 1 % and not
exactly. In state B the three samples bracket the kernel figure ([M878](../facts/kmd.md#m878)).

**ROUND 2.** The state-B half of the paragraph above is refuted. In the second arm the counter read 90, 86 and
87 % in state B while emitted latency chains in the same leg read 3407.4 MHz or more. So the counter reads
0.81 to 0.84 of the latency clock in that state ([M888](../facts/kmd.md#m888)). It bracketed a managed-loop
figure, not the clock. No file reads APERF or MPERF.

**Our own effective-clock instrument is blind.** Three `bc250kmd_cli cpu readback` calls inside the load window
print `cores 1555 3500 1555 1555 1400 1400 1400 933 MHz` byte for byte in both states. SMU message `0x43` does
not see the state, and the undervolt stretch guard gates on it ([M879](../facts/kmd.md#m879)).

**The state is chip-wide and it makes the core throughput-bound.** `dep_mix` reads 577.70 to 581.60 operations/us
on each of the 12 logical CPUs in state A and 257.64 to 287.30 in state B. Two threads on logical CPUs 0 and 1
then deliver 128.10 operations/us each and sum to 256.20, which is 0.865 of the 296.18 that one thread of that
same pair reads: in state B the aggregate does not rise at all when the second thread starts. In state A the
same pair sums to 1.89 times one thread's rate, so a second thread there costs 5.6 % of it. The pair is the two
threads of one core if the topology is the usual one, which no file of this arm proves
([M880](../facts/kmd.md#m880)).

**ROUND 2.** The second arm measures the topology: logical CPUs 0 and 1 share a core, and every other
logical CPU is on another core than CPU 0 ([M896](../facts/hardware.md#m896)).

**Memory is untouched and L1 is not.** The 64 MiB pointer chase reads 152.452 ns an access in state A and
153.104 ns in state B, flat to 0.4 %. The 16 KiB chase goes 2.086 to 3.455 ns, a modelled 7.28 to 9.61 cycles.
The 256 KiB chase costs the same number of cycles in both states ([M881](../facts/kmd.md#m881)).

**ROUND 2.** The cycle counts of the paragraph above used the refuted clock of 2782 MHz. At the latency clock
of the second arm, 16 KiB costs a modelled 11.73 cycles against 7.27, and 256 KiB 18.73 against 15.14. So L2
also costs more cycles in the state. The 64 MiB result stands ([M895](../facts/kmd.md#m895)).

**The extra GFX power reproduces inside one boot.** At 500 MHz, 820 mV and busy 0.0 % the gfx rail reads 9.0 to
10.2 W in state A and 17.2 W in all three state-B samples. The smart plug reads 80.4 to 81.5 W against 90.8 to
91.1 W at the wall ([M882](../facts/kmd.md#m882)).

**The RLC gating state after the KMD's RLC stage is identical.** `RLC_PG_CNTL` before the stage reads `0x8`
after a boot and `0x00800000` after a device restart. After the stage both read `0x00800000`
([M883](../facts/kmd.md#m883)).

**The OS PnP path alone does not make the state.** A restart of `ROOT\KDNIC\0000` in state A left the work meter
at a ratio of 1.000 ([M885](../facts/kmd.md#m885)).

### 9.2 What it refuted

1. **The 1.55 GHz reading of section 5.** The 6-cycle chain costs about 10.7 cycles an operation in state B, so
   the quotient that gave 1557 MHz was built on a cost that had changed.
   **ROUND 2:** the refutation stands, with about 13.1 cycles at the clock of
   [M887](../facts/kmd.md#m887) in place of 10.7.
2. **A uniform factor.** No one factor fits the seven kernels. The ratios run from 0.149 to 0.800.
3. **Stolen time.** With 2000 operations a chunk the mean over the median is 0.988 in state A and 0.967 in
   state B, and with 500 operations a chunk 0.933 and 0.890, while the `steal` control drove the 2000-operation
   figure to 0.503 and 0.493. The median itself fell 2.26 times, so the loss is inside the chunks and not
   between them. The distribution is not untouched: the share of slow chunks rises from 0.03 % to 0.27 % at
   2000 operations and from 0.02 % to 0.36 % at 500. Those shares are too small to carry a loss of this size.
4. **One or two slow CPUs.** All 12 logical CPUs lose the same way.
5. **A memory or fabric path that got slower.** The 64 MiB latency is flat and the GPU bandwidth is 1.3 % to
   2.8 % better in state B (copy 383.9 against 388.7 GB/s, read 374.3 against 384.7 GB/s).
6. **A moving time base.** The GPU golden counter reads 99.801 MHz in state A and 99.769 MHz in state B against
   its 100.000 MHz reference, so every wall-clock number of this defect stands.
7. **`RLC_GPU_CLOCK_32` as a GFX clock counter.** It advanced at 99.9 to 100.1 MHz in 15 of 15 samples, at a
   500 MHz idle point and under load at 1700 MHz, in both states, with `RES_SEL` 0
   ([M884](../facts/hardware.md#m884)). The arm caught this with its own positive control, so it drew no GPU
   clock conclusion.

### 9.3 What stays open

1. **The mechanism of the CPU half.** The shape is a core that still retires a dependent chain at its full
   3.00 cycles but issues far fewer independent operations a cycle. Dispatch or issue throttling fits that
   shape. So does a di/dt or electrical-design-current management mode that holds the issue width down. No
   counter on this part shows either to us, and the arm added none.
   **ROUND 2:** the shape is now measured in emitted code, and the "full 3.00 cycles" holds only for long
   chains. Section 10.4 gives the verdicts on the candidate mechanisms.
2. **The GPU half.** The project knows no host-readable counter of the delivered GFX clock on this part, so C7
   and the 0.64 of work per MHz stay an inference from wall time. Next test: one fixed compute dispatch at a
   pinned `DpmMaxMHz`, timed in both states, with no clock counter. The untried instrument is the subgroup-scope
   clock of `VK_KHR_shader_clock` ([M884](../facts/hardware.md#m884)).
3. **One cause or two.** The CPU loss and the extra 7.0 W have one trigger inside one boot. Nothing shows one
   cause.
4. **A PCI control.** The non-GPU control was a root-enumerated software device. A PnP stop and start of a
   non-GPU PCI function, which does tear down a function and move a D-state, is still owed.
5. **Why a warm restart out of state B hangs.** The restart after this arm hung after a clean OS shutdown and
   before POST, and an AC cycle recovered the machine in 16 s ([M886](../facts/kmd.md#m886)). That is at least
   the third such hang in this defect's work, after the two of C9. It is not every restart: the lead's record of
   the same day has one more hang after a plain driver uninstall and a later warm restart out of a stop state
   that booted normally (lab record, not in this repository). Treat a warm restart out of state B as a risk and
   plan one for a whole set of tests.
   **ROUND 2:** the warm restart that closed the second arm, out of a state after two device restarts, did not
   hang ([M891](../facts/kmd.md#m891)). One restart that booted does not show that the hang is gone.

### 9.4 Lessons for the next arm

- **Keep the seven-kernel meter.** One kernel cannot tell a clock change from a cost change. Seven can, and the
  table is cheap: both legs took about 40 s.
- **Bring a positive control for every new instrument.** The GPU clock counter failed its own control, which is
  the only reason the arm reported no GPU clock.
- **Run the theft control in both legs.** The `contend` arm recorded `noise_ops` 0 in state A and never fired,
  so only the `steal` control was valid in both.
- **A probe that answers after a timeout proves only that the machine is up now.** It does not show how the
  machine got there. The System log and the recovery timeline are the evidence of a boot.

### 9.5 What this changes in the CPU part of the driver

Two decisions in `driver/shim` rest on the quantity this arm measured, and both need a second look.

- **The release clock bound.** The header `driver/shim/include/bc250_cpu.h` keeps "an effective 2.74 to
  2.79 GHz" as the project's record for unit A and asks the release for no more than stock until a trial settles
  it. That figure is the K137 state: the same unit reads 3488.8 MHz healthy, and 2.78 GHz is what one device
  restart leaves behind ([M877](../facts/kmd.md#m877), [M878](../facts/kmd.md#m878)). The bound itself may still
  be the right one, but the measurement behind it was taken in the defect state and has to be retaken after a
  boot.
- **The undervolt stretch guard.** `BC250_CPU_STRETCH_MHZ` compares SMU message `0x43` with the target, and
  `0x43` prints the same per-core pattern in both states ([M879](../facts/kmd.md#m879)). So the guard cannot see
  a state that costs 0.8 of the clock and more of the throughput. It still protects against the stretch it was
  written for. It is not a witness of this one.

**ROUND 2.** Both bullets keep their conclusion with other numbers. The figure of 2.74 to 2.79 GHz is the
Windows counter's reading of the K137 state, and the latency clock of that state is 3396.5 MHz or more
([M888](../facts/kmd.md#m888), [M887](../facts/kmd.md#m887)). The guard's `0x43` stays within 103.5 MHz of
the latency clock in both states, under the 200 MHz threshold of `BC250_CPU_STRETCH_MHZ`
([M893](../facts/kmd.md#m893)). So the state costs almost no clock, and the guard cannot see the issue cost
that it does have.

## 10. The second arm of 2026-10-12

The second arm ran on unit A between 12:17Z and 12:29Z on 2026-10-12, inside the boot of 12:11:55Z. State A is
the healthy state after that boot. State B is the state after one `pnputil /restart-device` of the GPU at
12:21:31Z. State B2 is the state after a second one at 12:26:26Z. The raw files and the per-step verdicts are in
[the round 2 evidence directory](../../evidence/windows/2026-10-12-BD093-k137-round2/RESULT.md). The measured
rows are facts [M887 to M895](../facts/kmd.md), [M896](../facts/hardware.md#m896) and
[M897](../facts/tooling.md#m897).

The arm sent two SMU setters through the KMD, the only SMU owner, and released both. One was a CPU clock ceiling
of 2800 MHz (message `0x8F`). The other was a GFX DPM floor of 1500 MHz. The arm spent two GPU device restarts
and one warm Windows restart. Every call stayed under three minutes, and the highest temperature was 71.1 C.

### 10.1 The instrument

The first arm timed managed kernels. The JIT chose their instructions, and nobody disassembled its output. So a
slow kernel could mean a slow clock or a dearer instruction, and that arm could not tell which. The second arm
emits its own x86-64 code (`bd093law`), so it knows every instruction and every byte that it runs. It reads two
numbers in each run:

- **The clock witness.** A chain of 16 dependent `imul` (48 cycles) and a chain of 8 dependent `addsd` (24
  cycles) each give a clock. If an instruction costs at least its latency, the core clock is at least that value.
- **The issue cost k.** This is the least squares slope of cycles against the count of independent `add`
  instructions, 6 to 48 in a loop. Its cycles are wall time times the witness clock, so k is modelled.

### 10.2 What it measured

| Reading | State A | States B and B2 | B over A |
|---|---|---|---|
| `imul` clock, 5 and 9 uncapped runs | 3489.5 to 3490.8 MHz | 3396.5 to 3410.4 MHz | 0.976 by the means |
| `addsd` clock over `imul` clock | 0.9999 to 1.0003 | 0.9964 to 1.0054 | |
| k, cycles an independent `add` | 0.2639 to 0.2657 | 1.8045 to 1.8158 | 6.84 by the means |
| `imm32` form over `imm8` form | 0.994 to 1.008 | 0.997 to 1.006 | |
| nops of 1, 8 and 15 bytes, cycles each | 0.20 to 0.27 | 1.19 to 1.20 | |
| 256-bit vector row over 128-bit row | 0.999 to 1.000 | 1.181 to 1.211 | |
| 4 dependent `imul`, cycles | 11.995 to 12.001 | 14.114 to 14.624 | |
| 4 independent `imul`, cycles | 3.998 to 4.001 | 9.559 to 9.783 | |
| `% Processor Performance` under load | 108 % | 86 to 90 % | |
| gfx rail at the 500 MHz idle point | 9.6 to 10.2 W | 16.6 to 17.4 W | |
| gfx rail under one GPU load | 58.0 and 58.7 W | 33.2 and 37.5 W | 0.57 and 0.64 |
| `tput_add` of the first arm's meter | 11162.80 ops/us | 1663.66 ops/us | 0.149 |

**The clock is almost intact.** Two chains on two different units agree to 0.6 % in every state-B run
([M887](../facts/kmd.md#m887)). The 2782 MHz of the first arm came from managed loops that are rate bound in
the state. Under a real 2800 MHz ceiling, the healthy core runs the emitted FP chain at 930.73 additions/us. The
managed kernel reads 930.22 in state B, and that coincidence produced the reading of 2790 MHz.

**Every independent instruction costs more issue time.** k rises 6.84 times, and neither the encoding length nor
the nop length changes it ([M889](../facts/kmd.md#m889)). An add sweep costs 1.80 cycles an add in state B. Beside
an FP chain, the knee row stays flat up to 8 adds and then costs 1.50 cycles an add. Independent multiplies still
overlap, but a chain of 4 costs more than its latency. Long chains cost their latency.

**The cost depends on the kind of instruction.** The seven port rows cost 1.38 to 2.25 cycles an instruction in
state B, which is 1.50 to 6.50 times state A ([M890](../facts/kmd.md#m890)). The 256-bit vector row costs more
than the 128-bit row in state B only.

**The Windows counter does not read this clock.** It reads 0.81 to 0.84 of the `imul` clock in state B and 0.989
of it in state A ([M888](../facts/kmd.md#m888)).

**The GFX island draws less power under load.** One `vkmembw` load held the island at 2000 MHz, 1000 mV and 96.6
to 96.8 % busy in both states ([M892](../facts/kmd.md#m892)). The gfx rail then read about 21 to 25 W less in
state B, while it reads about 7 W more at idle. The loader printed no bandwidth, so no GPU work rate stands
beside that power.

**Memory, topology, SMU and CPUID.** L1 and L2 cost more cycles at the latency clock, and 64 MiB is flat
([M895](../facts/kmd.md#m895)). Logical CPUs 0 and 1 share a core ([M896](../facts/hardware.md#m896)). The SMU
feature mask, `0x43` on core index 1 and the CPUID leaves do not change ([M893](../facts/kmd.md#m893)). `wpr.exe`
lists TotalCycles, InstructionRetired and TotalIssues as profile sources ([M897](../facts/tooling.md#m897)).

**A group component waits for a concurrent run.** In state B, CPU 0 keeps 0.845 to 0.846 of its ALU rate beside
a thread on CPUs 2 to 5. Beside a thread on CPUs 6 to 11 it keeps 0.980 to 0.989 ([M894](../facts/kmd.md#m894)).
The two threads of that probe do not measure in one window. A concurrent kernel of the first arm shows no such
cost.

### 10.3 The causal arms

| Arm | State A | State B | What the rule of the plan concludes |
|---|---|---|---|
| CPU ceiling of 2800 MHz | the clock follows, k 0.2650, 0.2641 and 0.2639 | the clock follows to 2724.7 MHz, k 1.8086, 1.7769 and 1.8132 | the cost does not come through the clock grant |
| GPU load above 20 W | k 0.2644 idle and 0.2657 loaded, at 58.0 and 58.7 W | k 1.8069 idle and 1.8066 loaded, at 33.2 and 37.5 W | real GFX current does not throttle the cores |
| GFX DPM floor of 1500 MHz | not run | k 1.8123, 1.8057 and 1.8158, gfx 17.4, 22.0 and 17.2 W | the commanded operating point does not hold the state |
| second device restart | not applicable | k 1.8146, gfx 17.2 W | one latched state, full at the first stop |
| SMU feature mask | `0xDD602C7D` | `0xDD602C7D` in B and B2 | no SMU feature changes |
| CPUID leaves and XCR0 | the reference | byte-identical in B and B2 | no visible core configuration changes |
| warm restart out of B2 | not applicable | booted with no hang, state cleared | one restart that booted |

The rows are facts [M891](../facts/kmd.md#m891), [M892](../facts/kmd.md#m892) and
[M893](../facts/kmd.md#m893). Under the ceiling, k in state B steps 1.8 % for a clock step of 20 %. So the cap
counts in cycles and not in wall time. The plan promised a floor arm in state A as well, and its sequence had
none.

### 10.4 The mechanisms

The analysis before this arm ranked candidates for the CPU half. These verdicts apply the rules of the plan as
written.

| Candidate | Verdict | Deciding readings |
|---|---|---|
| a CPU throttle that answers real GFX current | refuted | k holds at a 58 W island in state A and at 33 to 38 W in state B, and the floor arm moves nothing |
| the same throttle on wrong accounting, which reads a current that is not there | open | the ceiling, load and dose arms fit it, and the unchanged feature mask weakens it |
| a core configuration latched by the second PSP firmware load | open | the unchanged mask and the dose arm promote it, and CPUID gives it no candidate field |
| a cap on front-end supply | refuted | the encoding length and the nop length do not change the cost |
| a lost back-end resource on one port | neither | no port row costs twice the others, and the knee is flat below its corner |
| contention from the SMT sibling | refuted | CPU 1 is the only sibling, and a twofold mechanism cannot make 6.8 times |
| stolen time | refuted for the issue cost | the emitted FP chain loses 1.9 %, and theft can hide only inside the 2.4 % that the clock witnesses lose |
| an artefact of managed code | refuted for the issue cost, true for the first arm's clock | emitted code reproduces the cost, and the 2782 MHz came from a rate-bound loop |
| an unreleased GFX force (H2) | refuted as the commanded point, untested as an unforced one | the floor arm sent no `UnforceGfxVid` |
| lost droop and stretch calibration (H3) | weaker | the clock deficit is 0.976, not 0.8 |

Two rows point different ways. The plan reads a 256-bit penalty in state B alone as the strongest evidence for a
throttle on GFX current. The load arm refutes that throttle in both legs. The two rows speak about different
halves of a limiter, its action inside the core and its input. This page leaves that conflict open.

A latched configuration that splits each 256-bit operation in two would cost about twice on the wide row. The
row costs 1.2 times. That counts against the latched candidate, and the plan has no rule for it.

### 10.5 What stays open

1. **Wrong accounting or a latched configuration.** No rule of this arm separates the two candidates that are
   left.
2. **The back-end question.** Dispatch-stall counters would decide it. Their event numbers are not on record.
3. **The GPU half.** The island draws less under the same load. No work rate stands beside that power.
4. **One cause or two.** The CPU cost and the extra idle power still have one trigger and no shared cause.
5. **The warm-restart hang.** One restart out of state B2 booted. That does not show that the hang is gone.
6. **The 4 MiB residual.** The new chase failed its own control at 4 MiB, so the ladder from 512 KiB to 4 MiB is
   not a measurement.
7. **A PCI control.** Item 4 of section 9.3 still holds.

### 10.6 Lessons for the next arm

- **Time code that you emit.** The first arm's clock came from a managed loop whose instruction count nobody
  knew.
- **Use two latency witnesses on two units.** One chain is a lower bound. Two that agree rule out a single unit
  that slowed.
- **Make each control valid in the state under test.** The selftest bands are healthy bands, and the dependent
  `imul` band fails in state B by measurement.
- **Measure both threads of a pair in one window.** Separate best windows make a sum that nobody can read.
- **Take plug samples inside the load window.** The samples after the loads here contradict the package power
  and cannot be read.

### 10.7 The next arm

The question is now wrong accounting against a latched core configuration, with the back-end question beside it.
The proposal is one KMD build, one deploy and one warm restart, then one boot with a state A leg and a state B
leg:

1. Print the per-rail current of the SMU metrics table beside every `dpm` line. A limiter on wrong accounting
   sees a CPU-rail current that does not follow the work.
2. Read the firmware status words once in each state, each name logged before its read, with the plug armed.
3. Read a fixed list of core MSRs through a read-only escape. APERF and MPERF also settle the Windows counter.
4. Run the GFX force release of test 4 of section 7 as an escape-scoped allow-list entry.
5. Capture TotalCycles, InstructionRetired and TotalIssues with `wpr.exe` over the law in both states. This
   gives a cycle count that does not rest on a latency.
6. Take the dispatch-stall event numbers from the AMD processor reference, with its revision on record.
7. Time a shader with `s_memtime` against `s_memrealtime`, and let `vkmembw` finish under the load.
8. Take plug samples inside the load window.

The kit also needs a concurrent pairs mode, a chase that reproduces from 512 KiB to 4 MiB, a selftest that does
not use healthy bands as a gate, and a floor arm in state A.

The BD-093 record named a stop with `EnableNativeSmu` 0 as the control of the next arm. This arm dropped it on
purpose, because that start fails and also removes the GFX, PSP, RLC and GART bring-up (C8). It is superseded,
not unfinished.
