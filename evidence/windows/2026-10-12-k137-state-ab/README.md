# K137 state A against state B in one boot (2026-10-12, BD-093)

Unit A, Windows 11, desktop on the GPU route, 40 CU, one boot of 2026-10-12T07:49:14 UTC (the `lastboot=` field
of every capture, which the script prints through `ToUniversalTime`, and which the `uptime_s=` of the same line
confirms). Every capture was one bounded script over SSH, shorter than three minutes.
The `HH:mm:ss` stamps inside the files are UTC, except the `dpm` lines of `bc250kmd_cli`, which carry the lab's
local time (UTC+2). This README gives UTC.

State A is the state after a Windows restart with no GPU device stop since the boot. State B is the state after
one `pnputil /restart-device` of the GPU in the same boot. The project calls state B the K137 state.

The analysis of these files is [`docs/research/k137-kmd-stop-cpu-cap.md`](../../../docs/research/k137-kmd-stop-cpu-cap.md),
section 9. The facts are M877 to M883, M885 and M886 in [`docs/facts/kmd.md`](../../../docs/facts/kmd.md) and
M884 in [`docs/facts/hardware.md`](../../../docs/facts/hardware.md). The earlier leg of the same defect is
[the 2026-10-07 directory](../2026-10-07-k137-stop-cpu-cap/README.md).

## Build

| Name | Kernel driver | Where the version comes from |
|---|---|---|
| live | the 0.7.216 line, ABI word `0x000700D8` | the `driver 0x000700D8` field of every `dpm` and `cpu` line |

The capture prints the ABI word only, so no file in this directory names the build number of the running
driver. The two `*-kmdlog.txt` files do print `0.7.216.27` at line 41, but as the threshold of an earlier
behaviour change (BD-114), not as the live build. The lab's deployment record of that morning, which is not in
this repository, gives KMD 0.7.216.36 (`bc250kmd.sys`
SHA-256 prefix `7290210C`) as the live driver from 07:21:45Z. No file of this arm installed, updated or
restarted a driver. The arm wrote no register, sent no SMU setter and changed no clock, voltage, BIOS setting
or firmware.

## Instruments

- **Kernel meter** (`scripts/bd093probe.cs`, built as `bd093probe.exe` with
  `csc.exe /optimize+ /platform:x64`, SHA-256 `768F01299F0A786B54AF0C43EE7567E9DEEE56E44CFA972CF7356638EBE33FE7`).
  Seven C# loops on one thread, pinned by `SetThreadAffinityMask` and raised to `THREAD_PRIORITY_HIGHEST`, timed
  with `Stopwatch` over the 10 MHz QPC. Each loop returns the number of operations that retired, so the printed
  `ops_per_us` divided by the core clock in MHz is operations per cycle, a property of the instruction mix alone.
  The seven kernels are `loop_add` (8 folded adds a loop, front end bound), `mix_xoradd` (4 XOR and 4 add, partly
  folded), `dep_imul` (4 dependent 64-bit multiplies, 3 cycles each), `dep_mix` (one dependent multiply, add,
  shift and XOR, the 6-cycle chain of the 2026-10-07 bundle), `tput_add` (8 independent adds), `tput_imul`
  (4 independent multiplies) and `dep_fpadd` (8 dependent double adds, 3 cycles each). The same binary ran in
  both states.
- **Chunk histogram** (`hist` mode of the same probe). The probe runs `dep_mix` in chunks of 500 or 2000
  operations and records the rate of each chunk. `mean_over_median` near 1.0 means no chunk lost time. The mode
  has two controls: `steal=1` starts a second thread on the same logical CPU, which must drive
  `mean_over_median` to about 0.5, and `contend=1` starts a thread on another logical CPU.
- **Sweep and threads** (`sweep` and `threads` modes). `sweep` runs `dep_mix` on each logical CPU in turn.
  `threads` runs `dep_mix` on n logical CPUs at once and prints the sum of the rates.
- **Pointer-chase latency** (`latency` mode). A dependent pointer chase over 16 KiB, 256 KiB, 4 MiB and 64 MiB,
  printed as nanoseconds for one access.
- **Golden time base** (`timebase` mode). The probe reads the GPU's 100 MHz golden counter through the KMD and
  divides by QPC seconds. A reading near 100.000 MHz shows that the wall-clock base of every other number held.
- **Windows performance counter.** `Win32_PerfFormattedData_Counters_ProcessorInformation` instance `0,2`, fields
  `PercentProcessorPerformance`, `PercentProcessorTime`, `ProcessorFrequency` and
  `PercentofMaximumFrequency`. `PercentProcessorPerformance` is an APERF over MPERF ratio against the nominal
  3194 MHz. Its refresh window is the provider's own, not the probe's load window, so it is a coarse witness and
  its granularity is 1 % of 3194 MHz, which is 32 MHz.
- **SMU getters** (`bc250kmd_cli cpu readback`, `bc250kmd_cli cpu`, `bc250kmd_cli dpm`). `cpu readback` is the
  only verb that sends SMU message `0x43` while a core is loaded. The `cores` line of a readback is the answer of
  `0x43` per core index. The `cores` line of a bare `cpu` call is the cached start-time snapshot. The `dpm` line
  carries the GPU operating point, the rail powers and Tctl.
- **GC counters** (`C:\BC250\bc250rd\bc250rd_cli.exe`, the deployed read-only reader). The arm read
  `RLC_GPU_CLOCK_32_RES_SEL`, `RLC_REFCLOCK_TIMESTAMP_LSB` and `_MSB`, `RLC_GPU_CLOCK_32` and the
  `RLC_GPU_CLOCK_COUNT_LSB` and `_MSB` latch pair as a witness. `rlc-clock.txt` holds the offsets that
  `bc250rd` was given.
- **GPU bandwidth** (`C:\BC250\vkmembw\vkmembw.exe`). 256 MiB buffers, GPU timestamps, copy and read checked
  against CPU oracles, in the device-local and the host-visible placement.
- **Smart plug.** Three LAN telemetry samples a state, each with the `dpm` line read right after it. The plug
  scales are the TinyTuya convention and are not calibrated for this model, so the plug numbers compare state A
  with state B and are not absolute wall figures.

## The sequence

Ten steps ran in this order, all inside the one boot.

| Step | What ran | File | Time (UTC) |
|---|---|---|---|
| T0 | deploy and tool check: the probe pushed, six tools found, `bc250rd` attached | not in a file | about 08:25 |
| T1 | state read of the healthy machine | `A-state.txt` | 08:27:10 to 08:27:22 |
| T2 | CPU capture, state A | `A-cpu.txt`, `A-kernels.txt`, `A-kernels.err` | 08:27:33 to 08:28:14 |
| T3 | GPU capture, state A | `A-gpu.txt`, `A-vkmembw.txt`, `A-vkmembw.err`, `A-kmdlog.txt`, `rlc-clock.txt` | 08:28:35 to 08:28:45 |
| T4 | plug telemetry, state A | `plug-telemetry.txt` | 08:29:19 to 08:29:51 |
| T5 | control: PnP restart of a device that is not the GPU, state A | `A-control.txt`, `A-pnputil.out`, `A-pnputil.err` | 08:30:01 to 08:30:22 |
| T6 | enter state B: one `pnputil /restart-device` of the GPU | `enter-state.txt` | 08:30:34 to 08:30:38 |
| T7 | GPU capture, state B | `B-gpu.txt`, `B-vkmembw.txt`, `B-vkmembw.err`, `B-kmdlog.txt` | 08:31:07 to 08:31:21 |
| T8 | CPU capture, state B | `B-cpu.txt`, `B-kernels.txt`, `B-kernels.err` | 08:31:46 to 08:32:32 |
| T9 | plug telemetry, state B | `plug-telemetry.txt` | 08:33:15 to 08:33:48 |
| T10 | pull: the lab files copied to the development PC | this directory | about 08:34 |

`A-kernels.err`, `B-kernels.err` and `A-pnputil.err` are empty, which is the result of those two tools. The
`*-vkmembw.err` files hold the winsys trace of `vkmembw` and no error. `rlc-clock.txt` is the register list that
the capture wrote for `bc250rd`, so it is an input of T3 and T7 and not a measurement.

The development-PC runner that drove these steps is not in this repository. It is a wrapper around
`tools/win/target.py` with the subcommands `deploy`, `state`, `cpu`, `gpu`, `control`, `enter` and `pull`, one
subcommand a step, each with its own SSH bound.

`scripts/` holds four files: the two lab-side scripts that the runner pushed (`bd093-capture.ps1` and
`bd093-enter-state.ps1`), the source of the kernel meter (`bd093probe.cs`), and `parse-check.ps1`, which reads
the other two with the PowerShell 5.1 parser before they are pushed and runs nothing. It is a development-side
gate of the kit, not a step of the arm.

## Per-step verdicts

| Step | Verdict | The number that decides it |
|---|---|---|
| T0 | pass | `# bars` printed by `bc250rd info`, so the register reader was attached |
| T1 | healthy | `A-state.txt`: `STATE-GUESS dep_mix_ops_per_us=581.7 verdict=healthy-like` |
| T2 | pass, one clock fits all seven kernels | `A-kernels.txt`: see the table below |
| T3 | the positive control of the GPU clock counter FAILED | `A-gpu.txt`: `mhz_by_refclk=100.0` at 500 MHz idle and under load alike |
| T4 | pass | `plug-telemetry.txt`: wall 81.5, 80.4, 80.4 W, gfx 10.2, 10.2, 9.0 W |
| T5 | fired, and the result is null | `A-control.txt`: `meter-after dep_mix_ops_per_us=581.6 ratio=1.000` |
| T6 | pass | `enter-state.txt`: `Device restarted successfully`, `device after status=OK`, `uptime 2453 ms` |
| T7 | the GPU measurement | `B-gpu.txt`: gfx 16.6 W at the same idle point, bandwidth intact |
| T8 | the CPU measurement | `B-cpu.txt`, `B-kernels.txt`: see the tables below |
| T9 | pass | `plug-telemetry.txt`: wall 91.1, 90.8, 91.0 W, gfx 17.2 W three times |
| T10 | pass | the files of this directory |

## Results

### The kernel table

`A-kernels.txt` and `B-kernels.txt`, one run of each kernel in each state. The cycles-per-operation column is
the design of the kernel on a 4-wide integer core and is modelled, not measured: a dependent chain is the sum
of its latencies divided by its counted operations, and `tput_add` is the one entry whose loop has to be
counted, because 8 independent adds plus the index increment and the compare are 10 operations, so 2.5 cycles
an iteration over 8 counted operations is 0.3125. The implied-clock and last columns are products of that
column, so both are modelled too. The last column is the quotient of the state-B rate and 2782 MHz.

| Kernel | cycles/op | A ops/us | implied clock in A | B ops/us | B over A | cycles/op in B at 2782 MHz |
|---|---|---|---|---|---|---|
| `loop_add` | 1 | 3488.82 | 3488.8 MHz | 1817.83 | 0.521 | 1.53 |
| `mix_xoradd` | 1 | 3489.71 | 3489.7 MHz | 1274.62 | 0.365 | 2.18 |
| `dep_imul` | 3 | 1162.91 | 3488.7 MHz | 927.43 | 0.798 | 3.00 |
| `dep_fpadd` | 3 | 1163.27 | 3489.8 MHz | 930.08 | 0.800 | 2.99 |
| `dep_mix` | 6 | 581.55 | 3489.3 MHz | 259.40 | 0.446 | 10.72 |
| `tput_imul` | 1 | 3488.73 | 3488.7 MHz | 929.30 | 0.266 | 2.99 |
| `tput_add` | 0.3125 | 11163.24 | 3488.5 MHz | 1665.33 | 0.149 | 1.67 |

In state A the seven implied clocks span 3488.5 to 3489.8 MHz, a range of 0.04 %. State A is one clock and a
fixed cost a kernel.

In state B the same arithmetic gives 520.4 MHz (`tput_add`), 927.4 MHz (`tput_imul`), 1274.6 MHz
(`mix_xoradd`), 1556.4 MHz (`dep_mix`), 1817.8 MHz (`loop_add`), 2782.3 MHz (`dep_imul`) and 2790.2 MHz
(`dep_fpadd`). One clock cannot produce that span, so in state B at least one cost a kernel changed.

At 2782 MHz the two pure latency chains read 3.00 and 2.99 cycles an operation, unchanged from state A.
`tput_imul` falls to the `dep_imul` rate, which means that no two independent multiplies overlap any more.

The same arm read `dep_mix` three more times in state A outside the capture: 581.66 in `A-state.txt`, 581.69
and 581.62 in `A-control.txt`.

Every state-B number of this directory is one run. The dispersion of one kernel over the modes of this leg
shows how far that goes: `dep_mix` reads 253.16 (the `hist` median), 259.40 (`kernels`), 260.18 (`sweep cpu=2`),
287.30 (`sweep cpu=0`) and 296.18 (`threads n=1`) in state B, a span of 17 %, against 571.43 to 581.69 in state
A, a span of 1.8 %. So the state-B costs are good to about one decimal of a cycle. The ratios of the table are
far outside that span and are not at risk from it.

### The performance counter, the time base and the theft control

| Reading | State A | State B | File |
|---|---|---|---|
| `PercentProcessorPerformance` under load, three samples | 108, 108, 108 | 91, 86, 87 | `A-cpu.txt`, `B-cpu.txt` |
| implied clock of those samples against 3194 MHz | 3449.5 MHz | 2906.5, 2746.8, 2778.8 MHz | the same |
| `golden_mhz` of the GPU golden counter | 99.801 | 99.769 | the same |
| `hist` 2000 operations a chunk, `mean_over_median` | 0.988 | 0.967 | the same |
| `hist` median of the same run | 571.43 ops/us | 253.16 ops/us | the same |
| `hist` 2000 operations a chunk, `slow_chunk_pct` | 0.03 | 0.27 | the same |
| `hist` 500 operations a chunk, `mean_over_median` | 0.933 | 0.890 | the same |
| `hist` 500 operations a chunk, `slow_chunk_pct` | 0.02 | 0.36 | the same |
| `hist` with `steal=1`, `mean_over_median` | 0.503 | 0.493 | the same |

The counter and the kernel meter agree inside the counter's granularity of 32 MHz. In state A the counter says
3449.5 MHz and the kernels say 3488.8 MHz, which is 1.2 % apart, or one counter step plus a rounding. In state B
the counter says 2746.8 to 2906.5 MHz and the latency chains say 2782.3 MHz.

The `steal=1` control drove `mean_over_median` to about 0.5 in both states, so the detector worked in both legs.
With no stolen thread, `mean_over_median` is 0.988 in state A and 0.967 in state B at 2000 operations a chunk,
and 0.933 against 0.890 at 500. The median itself fell 2.26 times, so the loss is inside the chunks and not
between them.

The finer grain is the weaker result. The distribution is not untouched either: the share of slow chunks rises
from 0.03 % to 0.27 % at 2000 operations and from 0.02 % to 0.36 % at 500. That is 9 to 18 times more slow
chunks in state B. Those shares cannot carry a loss of 2.26 times, so the reading stands. The right words are
"almost no chunk lost time", not "no chunk lost time".

`golden_mhz` is 0.20 % and 0.23 % from the 100.000 MHz reference and the two readings are 0.03 % apart, so the
wall-clock base did not move between the legs.

The `contend=1` arm is not comparable between the legs. In state A it recorded `noise_ops=0`, so the second
thread did no work and the arm never fired. In state B it recorded `noise_ops=1565240000`. Only the `steal`
control is valid in both.

### SMU message `0x43`

Three `cpu readback` calls ran inside the load window in each state. In both states all three printed

    cpu: cores 1555 3500 1555 1555 1400 1400 1400 933 MHz; P-states 3200 2550 2325 1960 1820 1600 1271 800 MHz

byte for byte (`A-cpu.txt`, `B-cpu.txt`). So `0x43` reports 3500 MHz on the loaded core in both states while the
kernel meter and the Windows counter both read about 2782 MHz in state B. The mapping of a core index to a
logical CPU is an assumption and no file proves it. What the files prove is that the pattern is identical in both
states.

The CPU rail reads 1199 mV in state A and 1193 mV in state B under load. The P-state table, the force mask
`0x77`, the processor count and the message counters (`reads 80 writes 0 refusals 0 reverts 0`) are the same in
both states. The idle `cores` lines of the other files differ between files inside one state, so they are not
comparable between the states.

### Every logical CPU, and the SMT siblings

`sweep`, `dep_mix` on each logical CPU in turn:

| State | Range over the 12 logical CPUs |
|---|---|
| A | 577.70 to 581.60 ops/us |
| B | 257.64 to 287.30 ops/us |

`threads`, `dep_mix` on n logical CPUs at once, the sum of the rates:

| n | A sum | A per thread | B sum | B per thread |
|---|---|---|---|---|
| 1 | 577.75 | 577.75 | 296.18 | 296.18 |
| 2 | 1091.13 | 545.57 | 256.20 | 128.10 |
| 4 | 2188.24 | 547.06 | 515.24 | 128.81 |
| 6 | 3280.46 | 546.74 | 773.33 | 128.89 |
| 8 | 4375.44 | 546.93 | 1032.36 | 129.05 |

The `threads` mode pins thread i to logical CPU i (`scripts/bd093probe.cs`, `ModeThreads`), so the n = 2 row is
logical CPUs 0 and 1 and its own n = 1 row is the baseline of that pair. Read the rows against that baseline
and not against the `kernels` run, which is pinned to logical CPU 2: the per-CPU spread of state B is 11.5 %,
so the two are not interchangeable.

In state A the pair sums to 1091.13, which is 1.89 times one thread's 577.75, so a second thread costs 5.6 % of
the per-thread rate: that is what a latency-bound chain does. In state B the pair sums to 256.20 against a
single-thread 296.18, which is 0.865: the aggregate does not rise at all when the second thread starts, and
each thread runs at 128.10 ops/us. Beyond two threads state B scales at about 129 ops/us a thread, so each
further pair adds about what the first pair delivers. A throughput-bound core shares its rate between its
threads and a latency-bound one does not.

That logical CPUs 0 and 1 are the two threads of one core is the usual topology of this part and is an
assumption here: no file of this arm reads the topology. The state-A drop of 5.6 % at n = 2 is weak support for
it. What the files show without that assumption is that the pair's aggregate does not rise in state B.

### Memory latency

`latency`, one dependent pointer chase, nanoseconds for one access. The cycle columns use 3488.8 MHz for state A
and 2782.3 MHz for state B.

| Working set | A ns | B ns | B over A | A cycles | B cycles |
|---|---|---|---|---|---|
| 16 KiB | 2.086 | 3.455 | 1.656 | 7.28 | 9.61 |
| 256 KiB | 4.339 | 5.498 | 1.267 | 15.14 | 15.30 |
| 4 MiB | 21.680 | 23.239 | 1.072 | 75.65 | 64.65 |
| 64 MiB | 152.452 | 153.104 | 1.004 | 531.7 | 425.9 |

The 64 MiB chase is flat to 0.4 % in absolute time, so no memory or fabric path got slower. The 16 KiB chase
costs 2.33 cycles more in state B. The 256 KiB chase costs the same number of cycles in both states.

### The GPU

| Reading | State A | State B | File |
|---|---|---|---|
| `vkmembw` device-local copy, median | 383.9 GB/s | 388.7 GB/s | `A-gpu.txt`, `B-gpu.txt` |
| `vkmembw` device-local read, median | 374.3 GB/s | 384.7 GB/s | the same |
| `vkmembw` device-local write, median | 371.0 GB/s | 369.6 GB/s | the same |
| `vkmembw` host-visible copy, median | 385.6 GB/s | 388.4 GB/s | the same |
| CPU cost a copy dispatch | 1.464 ms | 1.472 ms | the same |
| `RLC_GPU_CLOCK_32` rate at the 500 MHz idle point, three samples | 100.0, 100.0, 100.0 MHz | 100.0, 100.0, 100.0 MHz | the same |
| `RLC_GPU_CLOCK_32` rate under `vkmembw` at 1700 MHz | 100.0, 100.0, 99.9, 100.1 MHz | 100.1, 100.0, 99.9, 100.1, 100.0 MHz | the same |
| `RLC_GPU_CLOCK_32_RES_SEL` | `0x00000000` | `0x00000000` | the same |
| `RLC_GPU_CLOCK_COUNT_LSB` and `_MSB` latch pair | 0 and 0 | 0 and 0 | the same |

`RLC_GPU_CLOCK_32` advanced at the 100 MHz reference rate at a 500 MHz idle point and at a 1700 MHz load point,
in both states, with `RES_SEL` 0 and no other divider setting written. The counter therefore does not count the
GFX clock in that configuration, and the arm has no instrument for the GPU's delivered clock. The latch pair
stayed at 0, which is what a pair does that updates only when `RLC_CAPTURE_GPU_CLOCK_COUNT` is written, and the
arm wrote nothing. An unimplemented or masked register reads the same, so the latch reading is an
interpretation of a null read.

Bandwidth is intact in state B and 1.3 % to 2.8 % better on copy and read than in state A.

### Power at one operating point in one boot

Both states idled at 500 MHz, 820 mV, SMU VID 116, busy 0.0 %, DPM idle state 500 MHz.

| Reading | State A | State B | File |
|---|---|---|---|
| gfx rail, three plug samples | 10.2, 10.2, 9.0 W | 17.2, 17.2, 17.2 W | `plug-telemetry.txt` |
| gfx rail, the capture lines | 10.2 W, 9.6 W | 16.6 W, 17.2 W | `A-gpu.txt`, `A-state.txt`, `B-gpu.txt`, `B-cpu.txt` |
| wall power, three plug samples | 81.5, 80.4, 80.4 W | 91.1, 90.8, 91.0 W | `plug-telemetry.txt` |
| Tctl at those samples | 61.5, 60.2, 59.5 C | 63.8, 63.9, 63.9 C | the same |

The gfx rail draws 7.0 W more than its highest state-A sample. The wall meter draws 10.2 W more by the mean of
the three samples a state (80.8 W against 91.0 W). The closing note of `plug-telemetry.txt` gives +9.9 W at the
wall, which is a different pairing of the same six samples, and that file is raw and is not edited.

The wall rises by more than the gfx rail, and the package field of the same six `dpm` lines closes most of that
gap: package power reads 40.5, 36.7 and 35.4 W in state A and 47.7, 46.4 and 47.6 W in state B, a rise of
9.7 W by the mean, of which the gfx rail is 7.0 W. The rest sits inside the package and the `soc` field does
not resolve it, because that field moves by 3.9 W between samples of state A alone (4.8, 1.0 and 0.9 W). A wall
rise of 10.2 W against a package rise of 9.7 W is what a power supply adds.

The highest temperature of the whole arm is 67.2 C, in `A-gpu.txt` right after `vkmembw`.

### The start state of the RLC

The `cumode:` line of each start, which prints `RLC_PG_CNTL` before and after the KMD's RLC stage:

| State | Start line | `RLC_PG_CNTL` before the RLC stage | after the RLC stage |
|---|---|---|---|
| A | `initial 1500MHz/VID101 observed 1000MHz/VID116` | `0x00000008`, power-gating enables `0x8` | `0x00800000`, enables `0x0` |
| B | `initial 1000MHz/VID116 observed 1000MHz/VID116` | `0x00800000`, enables `0x0` | `0x00800000`, enables `0x0` |

`A-kmdlog.txt` lines 52 and 231, `B-kmdlog.txt` lines 51 and 230. The value before the stage differs between a
boot start and a restart start. The value after the stage is identical. Both starts report `pp_gfxoff 0`, 40 CU
applied, `SA0-3 CC 0xFFE00000` and `SPI 0x1F`.

### The control: a PnP restart of a device that is not the GPU

`A-control.txt` and `A-pnputil.out`. The script read the meter, restarted one non-GPU device, waited for the
device to report OK and read the meter again:

| Reading | Value |
|---|---|
| device picked | `ROOT\KDNIC\0000`, the Microsoft Kernel Debug Network Adapter |
| device skipped, because it carries the session | the Realtek PCIe GbE Family Controller |
| `pnputil` result | `Device restarted successfully`, `exit=0 seconds=0.1` |
| device after the restart | `status=OK` |
| meter before | `dep_mix_ops_per_us=581.7` |
| meter after | `dep_mix_ops_per_us=581.6`, `ratio=1.000` |

`ROOT\KDNIC\0000` is a root-enumerated software device. The step exercises the OS PnP stop and start path. It
does not tear down a PCI function and it does not move a device through a D-state, so it excludes less than a
PCI control would.

### Health flags

The `health` line of `A-cpu.txt` reads `flags=15 epoch=5 completed=12522 ready_ms=2322840`. The one of
`B-cpu.txt` reads `flags=7 epoch=5 completed=8 ready_ms=83543`. The device of state B had been started 83 s
earlier and had completed 8 heartbeats, against 12522 in state A, so the missing flag belongs to the age of the
start and not to the K137 state. No reading of this arm depends on that flag.

## Not in this directory

- **The warm restart that hung.** After T10 the lead asked for one warm Windows restart from state B. The
  System log of unit A gives `shutdown.exe` at 08:35:33Z, Kernel-Power 109 at 08:35:47Z and a clean
  Kernel-General 13 at 08:35:48Z, then no start event. The screen was black at 08:41:41Z, SSH and the emergency
  channel did not answer and the wall meter read 71 W. AC was cut at 08:43:29Z and restored at 08:43:49Z, and
  Kernel-General 12 followed at 08:44:05Z, 16 s later. The read at 08:45:20Z gives
  `dep_mix_ops_per_us=581.3 verdict=healthy-like`, gfx idle 10.2 W, GPU device status OK and Tctl 65.2 C, so the
  boot cleared the state. Those events and that read are in the lead's lab record, not in a file of this
  directory, so fact M886 rests on them and nothing in the repository can be checked against it. Exporting the
  System-log lines and the 08:45:20Z state read into this directory would close that, and the fact says so too.
- The plan of the arm, which names each capture and what it discriminates.
- The development-PC runner, and `bd093probe.exe` itself. `scripts/bd093probe.cs` is its source and the SHA-256
  of the binary is above.
- The earlier W3, ETW and cross-session plug numbers that the research note quotes.

## What these files do not show

- The mechanism. No file reads a dispatch or issue counter of a core, and the chip exposes no such counter to
  us today.
- The delivered GPU clock in either state. The free MMIO counter that could have answered it is not a GFX clock
  counter in the configuration read, which the positive control of T3 showed. The arm tried no other route. The
  subgroup-scope clock of `VK_KHR_shader_clock` over a known wall interval is one that is still open.
- Whether one cause owns the CPU loss and the extra 7 W. The two readings move together across one trigger. One
  trigger is not one cause.
- Whether a PCI function teardown of a device that is not the GPU also makes the state. The control device was
  a software device.
- Why the warm restart from state B hung, and whether state B plays a part in it.
- Any number for 24 CU, for another DPM ceiling or for another idle point. The arm held one operating point.

## Privacy

Nothing was changed before the copy, so the raw files are the output of the tools. Checked and found clean: no
IPv4 address, MAC address, serial number, Wi-Fi name or user name. The host name `BC250-A` stays in the `BEGIN`
line of each capture. The GPU device instance path, including its `4&19CAF403&0&0041` suffix, stays as the tools
printed it, as it does in the earlier Windows evidence of this repository. The GUID `{1ca05180-...}` in
`A-state.txt` is the public display adapter interface class, not a device identifier. The development-PC runner
is left out because it names this workstation's directory layout.

`sha256.txt` holds the SHA-256 of every file of this directory.
