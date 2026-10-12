# K137 state A against state B, round 2 (2026-10-12, BD-093)

Unit A, Windows 11, desktop on the GPU route, 40 CU, one boot of 2026-10-12T12:11:55 UTC (the `lastboot=` field
of every capture). That boot was a fresh boot after a kernel driver install. Every capture was one bounded script
over SSH, shorter than three minutes. The `HH:mm:ss` stamps inside the files are UTC, except the `dpm` lines of
`bc250kmd_cli`, which carry the lab's local time (UTC+2). This file gives UTC.

State A is the state after a Windows restart with no GPU device stop since the boot. State B is the state after
one `pnputil /restart-device` of the GPU in the same boot. State B2 is the state after a second one. The project
calls state B the K137 state.

This arm is the second one of 2026-10-12. The first one is
[the round 1 directory](../2026-10-12-k137-state-ab/README.md). Round 1 read the state with managed (RyuJIT)
kernels. This arm reads it with a probe that emits its own x86-64 code, so it knows every instruction and every
byte that it runs. The analysis is section 10 of
[`docs/research/k137-kmd-stop-cpu-cap.md`](../../../docs/research/k137-kmd-stop-cpu-cap.md). The facts are
M887 to M895 in [`docs/facts/kmd.md`](../../../docs/facts/kmd.md), M896 in
[`docs/facts/hardware.md`](../../../docs/facts/hardware.md) and M897 in
[`docs/facts/tooling.md`](../../../docs/facts/tooling.md).

## Build

| Name | Kernel driver | Where the version comes from |
|---|---|---|
| live | the 0.7.216 line, ABI word `0x000700D8` | the `driver 0x000700D8` field of every `dpm` and `cpu` line |

The capture prints the ABI word only, so no file in this directory names the build number of the running driver.
The `*-kmdlog.txt` files print `0.7.216.27` at line 41 as the threshold of an earlier behaviour change (BD-114),
not as the live build. The lab's deployment record of that day, which is not in this repository, gives KMD
0.7.216.39 (`bc250kmd.sys` SHA-256 prefix `5223BD63`) beside the user-mode files of package
0.7.216.100-tester.29, installed before the boot of 12:11:55Z.

The arm wrote to the hardware twice, both times through the KMD as the single SMU owner, and released both
writes itself: a CPU clock ceiling of 2800 MHz (SMU message `0x8F`) and a GPU DPM floor of 1500 MHz. It sent no
other SMU setter and wrote no register, BIOS setting or firmware.

## Instruments

- **Rate law probe** (`scripts/bd093law.cs`, built as `bd093law.exe` with `csc.exe /optimize+ /platform:x64`,
  SHA-256 `97ACE083A6D1297D755C6D70C3A95419EEC1A1EB46E68668D376C80A06B08BC3`). The probe writes x86-64 machine
  code into executable memory and calls it. It uses volatile registers only. Every loop ends in `sub rcx, 1` and
  `jnz`, so the instruction count of an iteration is the body plus 2. One thread, pinned to logical CPU 2 unless
  a line says otherwise, timed with the 10 MHz QPC, best of 3 windows of 50 ms. Its modes:
  - `selftest`: an emitted loop that returns its own iteration count, the two clock witnesses and healthy bands.
  - `law` (full) and `quick`: the clock witnesses, then the rows below. `quick` runs the clock, the sweep, the
    knee, the ports and the pair.
  - `pairs`: an ALU throughput loop alone on CPU 0, then on CPU 0 together with each other logical CPU. Each
    thread reports its own best window, so the two windows need not overlap.
  - `cache`: a dependent pointer chase over 16 KiB to 64 MiB.
  - `cpuid`: the CPUID leaves of LAB-PLAN L-10 and XCR0, pinned.
- **Clock witnesses** (the `clock` line). `imul16` is 16 dependent `imul r64` (48 cycles at 3 cycles each).
  `fp8` is 8 dependent `addsd` (24 cycles at 3 cycles each). Each prints the clock that its nominal latency
  implies. A witness is a lower bound: if an instruction costs at least its nominal latency, the core clock is at
  least the printed value. `fp_over_imul` near 1.00 means that both witnesses agree.
- **Rows of the law.** `sweep8` and `sweep32`: 6, 12, 18, 24, 36 and 48 independent `add r64, imm8` (4 bytes)
  or `add r64, imm32` (7 bytes), with the least squares slope `slope_cyc_per_instr` (k) over cycles against
  instructions. `knee`: the 8-`addsd` chain plus m independent adds. `port`: 24 instructions of one kind (ALU
  add, `imul`, `addsd`, 128-bit `paddq`, 256-bit `vpaddq`, load, store). `pair`: 4 dependent against 4
  independent multiplies. `nop`: 8, 16 or 32 nops of 1, 8 or 15 bytes. `deplen`: 8 to 48 dependent multiplies.
  The `cyc_per_iter` column of every row is wall time times the start clock of that run's `clock` line, so it is
  modelled at the witness clock.
- **Capture script** (`scripts/bd093law-capture.ps1`). The phases `law`, `lawmap`, `smu`, `pmc`, `clockcap`,
  `load` and `force`, one flushed line a step, and an `END phase=... exit=...` line whatever happens.
- **The round 1 kernel meter** (`bd093probe.exe`, SHA-256 prefix `768F0129`, unchanged). The `state` and `cpu`
  phases of the round 1 capture script run it. Its source and scripts are in
  [the round 1 `scripts/` directory](../2026-10-12-k137-state-ab/scripts/), and its kernels are described in
  [the round 1 README](../2026-10-12-k137-state-ab/README.md).
- **SMU getters** (`bc250kmd_cli cpu readback`, `cpu`, `dpm`). `cpu readback` sends message `0x43` per core and
  queue 0 message `0x3D` (the feature mask). The `smu` phase reads it idle, under a CPU load whose own work is
  printed, and after the load, then prints the start log of the KMD.
- **Windows counters.** `\Processor Information(0,2)` through CIM, as in round 1.
- **Smart plug** (`plug-log.txt`). One LAN telemetry sample before and after every phase. The plug scales are
  the TinyTuya convention, not calibrated for this model, and the plug's measurement age is unknown.

## The sequence

| Step | What ran | File | Time (UTC) |
|---|---|---|---|
| T0 | deploy, tool check, `selftest` on the lab | not in a file | about 12:17 |
| T1 | state read of the healthy machine | `A-state.txt` (first read) | 12:17:25 to 12:17:37 |
| T2 | law, state A | `A-law.txt`, `A-law-selftest.out`, `A-law-cpuid.out`, `A-law-full.out`, `A-law-run.out` | 12:17:50 to 12:18:01 |
| T2b | lawmap, state A, run twice | `A-lawmap.txt` (both runs), `A-law-pairs.out`, `A-law-cache.out` (second run) | 12:18:12 to 12:18:28 |
| T3 | SMU getters and start log, state A | `A-smu.txt`, `A-kmdlog.txt` | 12:18:39 to 12:19:02 |
| T4 | PMC sources, state A | `A-pmc.txt`, `A-pmc-run.out` | 12:19:12 to 12:19:13 |
| T5 | CPU clock ceiling 2800 MHz, state A | `A-clockcap.txt`, `A-clockcap-*` | 12:19:24 to 12:19:48 |
| T6 | GPU load, state A | `A-load.txt`, `A-load-*`, `A-loadmembw.*` | 12:19:59 to 12:20:22 |
| T7 | round 1 CPU capture, state A | `A-cpu.txt`, `A-kernels.txt` | 12:20:32 to 12:21:14 |
| T8 | enter state B: `pnputil /restart-device` of the GPU at 12:21:31 | `enter-state.txt` (first half) | 12:21:28 to 12:21:34 |
| T9 | SMU getters and start log, state B | `B-smu.txt`, `B-kmdlog.txt` | 12:21:48 to 12:22:08 |
| T10 | law, state B | `B-law.txt` and its `.out` files | 12:22:21 to 12:22:34 |
| T10b | lawmap, state B | `B-lawmap.txt`, `B-law-pairs.out`, `B-law-cache.out` | 12:22:52 to 12:22:56 |
| T11 | round 1 CPU capture, state B | `B-cpu.txt`, `B-kernels.txt` | 12:23:11 to 12:23:53 |
| T12 | GPU load, state B | `B-load.txt`, `B-load-*`, `B-loadmembw.*` | 12:24:05 to 12:24:29 |
| T13 | CPU clock ceiling 2800 MHz, state B | `B-clockcap.txt`, `B-clockcap-*` | 12:24:44 to 12:25:11 |
| T14 | GPU DPM floor 1500 MHz, then off, state B | `B-force.txt`, `B-force-*` | 12:25:23 to 12:26:07 |
| T15 | enter state B2: a second `pnputil /restart-device` at 12:26:26 | `enter-state.txt` (second half) | 12:26:22 to 12:26:29 |
| T16 | law, state B2 | `B2-law.txt` and its `.out` files | 12:26:38 to 12:26:51 |
| T17 | SMU getters and start log, state B2 | `B2-smu.txt`, `B2-kmdlog.txt` | 12:27:03 to 12:27:24 |
| T18 | pull | this directory | about 12:27 |
| T19 | warm Windows restart, then a state read | `boot-t19.txt`, `A-state.txt` (second read) | boot 12:28:05, read 12:29:42 |

Every phase log of the new script ends with `END phase=<name> state=<state> exit=0`. The `END` line of each
enter says "the device has been stopped and started once in this boot". That is a fixed string of
`bd093-enter-state.ps1` and not a count: the second enter was the second stop of the boot.

The plan promised a state A run of the DPM floor excursion before T14. No step ran it, so the floor arm has no
state A answer.

The empty `.err` files are the result of those tools. `*-loadmembw.err` holds the winsys trace of `vkmembw` and
no error, and `*-loadmembw.out` is empty because the phase killed the loader when the measurement ended. The
`smu` phase also wrote `<state>-smu-run.out`, a byte-identical copy of `<state>-kmdlog.txt`, and an empty
`<state>-smu-run.err`. Those two are left out. `plug-log.txt` is the runner's raw log and is not edited.

## Per-step verdicts

| Step | Verdict | The number that decides it |
|---|---|---|
| T1 | healthy | `STATE-GUESS dep_mix_ops_per_us=581.6 verdict=healthy-like` |
| T2 | pass, the positive control of the probe | selftest `failures=0`, both witnesses 3489.8 and 3490.1 MHz, `dep4_cyc=11.995` |
| T2b | pass, the sibling map is measured | CPU 0 with CPU 1 `sum_over_solo=1.077 verdict=shares_a_core`, every other pair 2.006 to 2.008 |
| T3 | pass | features `0xDD602C7D same_as_record=yes`, the load job did 581.61 operations/us |
| T4 | pass | `wpr.exe` lists 18 profile sources, CIM read 68, 69 and 59 % |
| T5 | pass, the positive control of the clock ceiling | `capped_clock_followed=True target_mhz=2800 witness_mhz=2792.3`, k 0.2650, 0.2641, 0.2639 |
| T6 | pass | `GATE PASSED`, island 58.0 and 58.7 W, k 0.2644 and 0.2657 |
| T7 | pass | seven kernels on one clock, `dep_mix` 581.72 operations/us |
| T8 | pass | `Device restarted successfully`, `device after status=OK` |
| T9 | the SMU measurement | features `0xDD602C7D`, the load job did 257.73 operations/us |
| T10 | the measurement | witnesses 3407.4 and 3423.1 MHz, k 1.8045 |
| T10b | the pairs measurement | see the pairs table |
| T11 | continuity | `dep_mix` 259.56 operations/us |
| T12 | pass | `GATE PASSED`, island 33.2 and 37.5 W, k 1.8069 and 1.8066 |
| T13 | pass | `capped_clock_followed=True witness_mhz=2724.7`, k 1.8086, 1.7769, 1.8132 |
| T14 | pass, floor set and released | gfx 22.0 W at 1500 MHz, 17.2 W after, k 1.8123, 1.8057, 1.8158 |
| T15 | pass | `Device restarted successfully`, `device after status=OK` |
| T16 | the dose measurement | k 1.8146, witnesses 3406.0 and 3408.7 MHz |
| T17 | pass | features `0xDD602C7D`, the load job did 259.29 operations/us |
| T19 | pass, no hang | `BOOT UP windows boot 2026-10-12T12:28:05Z after 71s`, then `dep_mix_ops_per_us=581.7 verdict=healthy-like` |

The state B and B2 selftests failed one healthy band each. The state B selftest ran 10 s before the law, read the
witnesses at 3135.7 and 2982.3 MHz (`fp_over_imul=0.9511`) and `dep4_cyc=13.527`, and printed "selftest FAILED:
the lines below are recorded but may not be quoted as measurements". The law of the same phase read 3407.4 and
3423.1 MHz (`fp_over_imul=1.0046`) 10 s later. The B2 selftest read 3406.0 and 3421.0 MHz and failed only the
`dep4` band (11.0 to 13.5 cycles) with 14.449 cycles. These bands describe a healthy core. The plan's positive
control for the probe is the state A selftest, which passed. The `dep4` band fails in state B because the pair
row of the law measures 14.1 to 14.6 cycles there in every run. This file quotes the state B law lines for that
reason, and states it here.

## Results

### The clock and the slot slope

All 16 law runs. The two witnesses are the `clock` line at the start of each run. k is `slope_imm8`.

| Run | imul16 MHz | fp8 MHz | `fp_over_imul` | k imm8 | r2 | k imm32 | imm32 over imm8 |
|---|---|---|---|---|---|---|---|
| A law | 3489.5 | 3490.6 | 1.0003 | 0.2644 | 0.99951 | 0.2665 | 1.008 |
| A clockcap base | 3490.3 | 3490.3 | 1.0000 | 0.2650 | 0.99960 | 0.2648 | 0.999 |
| A clockcap 2800 | 2792.3 | 2792.4 | 1.0000 | 0.2641 | 0.99946 | 0.2659 | 1.007 |
| A clockcap released | 3489.6 | 3490.6 | 1.0003 | 0.2639 | 0.99950 | 0.2660 | 1.008 |
| A load idle | 3490.8 | 3490.7 | 1.0000 | 0.2644 | 0.99951 | 0.2652 | 1.003 |
| A load loaded | 3490.2 | 3489.7 | 0.9999 | 0.2657 | 0.99962 | 0.2641 | 0.994 |
| B law | 3407.4 | 3423.1 | 1.0046 | 1.8045 | 0.99999 | 1.8117 | 1.004 |
| B load idle | 3410.4 | 3421.5 | 1.0033 | 1.8069 | 0.99995 | 1.8151 | 1.005 |
| B load loaded | 3396.5 | 3414.7 | 1.0054 | 1.8066 | 0.99999 | 1.8092 | 1.001 |
| B clockcap base | 3406.5 | 3394.3 | 0.9964 | 1.8086 | 0.99999 | 1.8191 | 1.006 |
| B clockcap 2800 | 2724.7 | 2736.1 | 1.0042 | 1.7769 | 1.00000 | 1.7736 | 0.998 |
| B clockcap released | 3405.2 | 3423.4 | 1.0054 | 1.8132 | 0.99997 | 1.8094 | 0.998 |
| B force before | 3405.6 | 3422.5 | 1.0050 | 1.8123 | 0.99998 | 1.8191 | 1.004 |
| B force at floor | 3409.5 | 3426.6 | 1.0050 | 1.8057 | 0.99996 | 1.7996 | 0.997 |
| B force after | 3409.4 | 3417.7 | 1.0024 | 1.8158 | 0.99998 | 1.8199 | 1.002 |
| B2 law | 3406.0 | 3408.7 | 1.0008 | 1.8146 | 0.99996 | 1.8199 | 1.003 |

Without a ceiling, the imul16 witness reads 3489.5 to 3490.8 MHz in the 5 state A runs and 3396.5 to 3410.4 MHz
in the 9 state B and B2 runs. The means are 3490.1 and 3406.3 MHz, a ratio of 0.976. The fp8 witness reads
3394.3 to 3426.6 MHz in the same state B runs, and `fp_over_imul` stays between 0.9964 and 1.0054. Under the
2800 MHz ceiling the state B witness reads 2724.7 MHz against 2792.3 MHz in state A, the same ratio of 0.976.

k reads 0.2639 to 0.2657 cycles per instruction in state A (mean 0.2647) and 1.8045 to 1.8158 in state B and B2
(mean 1.8098), a ratio of 6.84. The fit has r2 of 0.99946 or more in every run. The slope over the 7-byte
`imm32` encoding is the same as over the 4-byte `imm8` encoding to 1 % in both states.

### Nops, the knee, the sweep

Cycles per instruction of the nop rows:

| Nop size | A law, x8, x16, x32 | B law, x8, x16, x32 | B2 law, x8, x16, x32 |
|---|---|---|---|
| 1 byte | 0.2000, 0.2000, 0.2001 | 1.1964, 1.1951, 1.1935 | 1.1952, 1.1941, 1.1921 |
| 8 bytes | 0.2000, 0.2000, 0.2000 | 1.1950, 1.1943, 1.1915 | 1.1952, 1.1921, 1.1924 |
| 15 bytes | 0.2727, 0.2105, 0.2286 | 1.1951, 1.1937, 1.1927 | 1.1952, 1.1927, 1.1918 |

The knee, cycles per iteration of the 8-`addsd` chain plus m independent adds:

| Run | m0 | m4 | m8 | m12 | m16 | m20 | m24 | m32 | m40 |
|---|---|---|---|---|---|---|---|---|---|
| A, all 6 runs | 23.99 to 24.00 | 23.99 to 24.01 | 23.99 to 24.00 | 23.99 to 24.00 | 23.99 to 24.01 | 23.99 to 24.01 | 24.00 to 24.02 | 23.99 to 24.01 | 24.00 to 24.01 |
| B law | 23.88 | 24.84 | 23.98 | 30.02 | 35.99 | 42.09 | 48.13 | 60.02 | 72.03 |
| B load idle | 23.90 | 23.97 | 24.02 | 30.01 | 36.01 | 42.06 | 48.08 | 60.65 | 72.09 |
| B load loaded | 23.87 | 23.90 | 23.96 | 30.70 | 36.05 | 42.06 | 48.02 | 60.02 | 71.96 |
| B clockcap base | 23.89 | 23.95 | 23.99 | 29.99 | 36.00 | 41.97 | 47.97 | 60.01 | 72.05 |
| B clockcap 2800 | 23.91 | 23.94 | 24.01 | 30.00 | 35.99 | 41.95 | 47.97 | 60.04 | 72.01 |
| B clockcap released | 23.88 | 23.91 | 23.98 | 30.00 | 36.01 | 41.95 | 48.44 | 60.09 | 72.03 |
| B force before | 23.89 | 23.93 | 24.82 | 29.99 | 36.00 | 42.10 | 47.99 | 60.05 | 71.92 |
| B force at floor | 23.90 | 23.98 | 24.02 | 30.02 | 35.98 | 42.03 | 48.30 | 60.06 | 72.15 |
| B force after | 23.90 | 23.95 | 23.97 | 29.98 | 36.04 | 42.07 | 48.02 | 60.03 | 72.03 |
| B2 law | 23.87 | 23.94 | 23.99 | 30.00 | 36.04 | 42.01 | 48.02 | 59.96 | 71.98 |

In state B the knee is flat at 23.9 to 24.0 cycles from m0 to m8, with 2 outliers of 30 points (24.84 and 24.82).
From m12 on it reads 1.5 times (m + 8) cycles: 30, 36, 42, 48, 60 and 72. In state A it is flat to m40.

The add sweep, cycles per iteration of n independent adds plus 2:

| Run | x6 | x12 | x18 | x24 | x36 | x48 |
|---|---|---|---|---|---|---|
| A law | 2.000 | 3.703 | 5.299 | 6.956 | 10.129 | 13.094 |
| B law | 12.691 | 23.520 | 34.583 | 45.083 | 66.840 | 88.535 |
| B2 law | 12.678 | 23.528 | 34.285 | 45.725 | 67.138 | 88.820 |

Alone, one more add costs 1.80 cycles in state B. Beside the FP chain of the knee, one more add costs 1.50 cycles.

### The port map

Cycles per instruction, 24 instructions of one kind plus the 2 loop instructions.

| Run | alu | mul | fp | vec128 | vec256 | load | store | vec256 over vec128 |
|---|---|---|---|---|---|---|---|---|
| A law | 0.2676 | 0.9229 | 0.9228 | 0.4616 | 0.4613 | 0.4617 | 0.9230 | 0.999 |
| A clockcap base | 0.2677 | 0.9230 | 0.9232 | 0.4615 | 0.4614 | 0.4617 | 0.9234 | 1.000 |
| A clockcap 2800 | 0.2677 | 0.9231 | 0.9231 | 0.4615 | 0.4616 | 0.4617 | 0.9232 | 1.000 |
| A clockcap released | 0.2676 | 0.9227 | 0.9228 | 0.4614 | 0.4615 | 0.4614 | 0.9228 | 1.000 |
| A load idle | 0.2677 | 0.9233 | 0.9236 | 0.4616 | 0.4618 | 0.4616 | 0.9235 | 1.000 |
| A load loaded | 0.2677 | 0.9231 | 0.9233 | 0.4616 | 0.4616 | 0.4615 | 0.9234 | 1.000 |
| B law | 1.7386 | 2.2546 | 1.3833 | 1.3821 | 1.6677 | 1.6909 | 1.7625 | 1.207 |
| B load idle | 1.7525 | 2.2575 | 1.3824 | 1.3826 | 1.6689 | 1.6923 | 1.7636 | 1.207 |
| B load loaded | 1.7374 | 2.2480 | 1.3810 | 1.3804 | 1.6623 | 1.6875 | 1.7562 | 1.204 |
| B clockcap base | 1.7460 | 2.2560 | 1.3817 | 1.3806 | 1.6693 | 1.6903 | 1.7606 | 1.209 |
| B clockcap 2800 | 1.7075 | 2.2052 | 1.3843 | 1.3801 | 1.6299 | 1.6546 | 1.7237 | 1.181 |
| B clockcap released | 1.7505 | 2.2525 | 1.3807 | 1.3804 | 1.6653 | 1.6905 | 1.7602 | 1.206 |
| B force before | 1.7574 | 2.2537 | 1.3822 | 1.3818 | 1.6644 | 1.6897 | 1.7607 | 1.205 |
| B force at floor | 1.7452 | 2.2574 | 1.3817 | 1.3821 | 1.6735 | 1.6925 | 1.7637 | 1.211 |
| B force after | 1.7509 | 2.2566 | 1.3819 | 1.3803 | 1.6703 | 1.6916 | 1.7627 | 1.210 |
| B2 law | 1.7436 | 2.2556 | 1.3789 | 1.3796 | 1.6698 | 1.6912 | 1.7615 | 1.210 |

The `B law` row over the `A law` row: alu 6.50, mul 2.44, fp 1.50, vec128 2.99, vec256 3.62, load 3.66, store
1.91. The 256-bit row costs 1.181 to 1.211 times the 128-bit row in all 10 state B and B2 runs, and 0.999 to 1.000
times in all 6 state A runs. The 256-bit row does twice the work of the 128-bit row in each instruction.

### The pair and the chain length

| Row | A, 6 runs | B and B2, 10 runs |
|---|---|---|
| `dep_imul_x4` (4 dependent multiplies), cycles | 11.995 to 12.001 | 14.114 to 14.624 |
| `ind_imul_x4` (4 independent multiplies), cycles | 3.998 to 4.001 | 9.559 to 9.783 |
| dependent over independent | 2.999 to 3.000 | 1.477 to 1.496 |

| Chain | latency at 3 cycles | A law | B law | B2 law |
|---|---|---|---|---|
| `dep_imul_x8` | 24 | 23.994 | 23.998 | 23.991 |
| `dep_imul_x16` | 48 | 47.999 | 49.238 | 47.941 |
| `dep_imul_x32` | 96 | 95.976 | 95.859 | 96.004 |
| `dep_imul_x48` | 144 | 143.954 | 144.013 | 144.299 |

### The round 1 kernels again

`A-kernels.txt` and `B-kernels.txt`, one run of each kernel, pinned to logical CPU 2.

| Kernel | A ops/us | B ops/us | B over A | round 1, B ops/us |
|---|---|---|---|---|
| `loop_add` | 3489.26 | 1801.44 | 0.516 | 1817.83 |
| `mix_xoradd` | 3489.23 | 1287.94 | 0.369 | 1274.62 |
| `dep_imul` | 1162.99 | 928.06 | 0.798 | 927.43 |
| `dep_mix` | 581.72 | 259.56 | 0.446 | 259.40 |
| `tput_add` | 11162.80 | 1663.66 | 0.149 | 1665.33 |
| `tput_imul` | 3489.44 | 928.66 | 0.266 | 929.30 |
| `dep_fpadd` | 1163.28 | 930.22 | 0.800 | 930.08 |

State B reproduces round 1 to 1.1 % on every kernel and to 0.1 % on five of the seven.

At the B witness clock of 3407.4 MHz, the managed `dep_fpadd` costs a modelled 29.3 cycles for its 8 dependent
additions, and the managed `dep_imul` a modelled 3.67 cycles a multiply. Both are above the latency of their
chains. The emitted 8-`addsd` chain (`knee_m0`) delivers 1141.38 additions/us in state B against 1163.88 in state
A, which is 0.981. In state A under the 2800 MHz ceiling the same emitted chain delivers 930.73 additions/us.

### The Windows counter, the time base and the theft control

| Reading | State A | State B | File |
|---|---|---|---|
| `pct_processor_performance` under load, three samples | 108, 108, 108 | 90, 86, 87 | `A-cpu.txt`, `B-cpu.txt` |
| clock that it implies against 3194 MHz | 3449.5 MHz | 2874.6, 2746.8, 2778.8 MHz | the same |
| `pct_of_maximum_frequency` in the same samples | 100, 100, 100 | 92, 87, 88 | the same |
| `golden_mhz` | 99.802 | 99.767 | the same |
| `hist` 2000 operations a chunk, `mean_over_median` | 0.988 | 0.968 | the same |
| `hist` 500 operations a chunk, `mean_over_median` | 0.933 | 0.891 | the same |
| `hist` with `steal=1` | 0.503 | 0.493 | the same |
| `hist` with `contend=1` | 0.988, `noise_ops=0` | 0.967, `noise_ops=0` | the same |

In state A the counter's clock is 0.989 of the imul16 witness. In state B it is 0.81 to 0.84 of it. The
`contend=1` arm recorded `noise_ops=0` in both states, so it did not fire in either leg.

### Every logical CPU, and the threads

`sweep`, `dep_mix` on each logical CPU in turn, operations/us:

| CPU | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A | 577.22 | 580.96 | 581.55 | 581.48 | 581.42 | 581.37 | 581.36 | 581.25 | 581.27 | 581.30 | 581.01 | 581.16 |
| B | 290.55 | 256.02 | 257.45 | 257.93 | 260.81 | 259.77 | 259.56 | 259.57 | 261.47 | 252.33 | 275.95 | 267.99 |

`threads`, `dep_mix` on logical CPUs 0 to n-1 at once, the sum:

| n | 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| A | 577.64 | 1090.70 | 2187.00 | 3278.79 | 4374.13 |
| B | 288.88 | 256.00 | 514.94 | 773.34 | 1032.02 |

### The sibling map and the pairs

`A-law-pairs.out` and `B-law-pairs.out`, ALU throughput loop, work/us. `A-lawmap.txt` holds a first state A run
with the same map (solo 11953.32, CPU 0 with CPU 1 1.076, every other pair 2.006 to 2.008).

| Pair | A: sum over solo | A: verdict | B: a | B: b | B: sum over solo | B: verdict |
|---|---|---|---|---|---|---|
| solo CPU 0 | 11952.48 work/us | | 2088.33 | | | |
| 0 and 1 | 1.077 | `shares_a_core` | 2060.98 | 893.43 | 1.415 | `shares_a_core` |
| 0 and 2 | 2.008 | `separate_cores` | 1766.18 | 1782.78 | 1.699 | `unclear` |
| 0 and 3 | 2.008 | `separate_cores` | 1766.73 | 1785.44 | 1.701 | `unclear` |
| 0 and 4 | 2.008 | `separate_cores` | 1765.91 | 1784.74 | 1.700 | `unclear` |
| 0 and 5 | 2.008 | `separate_cores` | 1765.58 | 1784.71 | 1.700 | `unclear` |
| 0 and 6 | 2.007 | `separate_cores` | 2047.48 | 1797.18 | 1.841 | `separate_cores` |
| 0 and 7 | 2.006 | `separate_cores` | 2065.88 | 1785.51 | 1.844 | `separate_cores` |
| 0 and 8 | 2.006 | `separate_cores` | 2064.80 | 1800.96 | 1.851 | `separate_cores` |
| 0 and 9 | 2.007 | `separate_cores` | 2058.65 | 1805.12 | 1.850 | `separate_cores` |
| 0 and 10 | 2.007 | `separate_cores` | 2054.30 | 1900.79 | 1.894 | `separate_cores` |
| 0 and 11 | 2.007 | `separate_cores` | 2058.88 | 1912.62 | 1.902 | `separate_cores` |

In state B, CPU 0 keeps 0.980 to 0.989 of its solo rate beside a partner on CPUs 6 to 11, and 0.845 to 0.846
beside a partner on CPUs 2 to 5. Each thread of a pair reports its own best of 3 windows, so the two windows
need not overlap and a pair sum is not a concurrent sum. The `threads` mode is concurrent. It shows no cost of
that kind for `dep_mix`: 128.0 to 129.0 operations/us a thread at n = 2, 4, 6 and 8.

CPUID leaf 8000001D sub-leaf 3 gives a 16-way 4 MiB cache shared by 6 logical CPUs (`EAX` bits 25 to 14 read 5).
The force mask `0x77` of the `cpu` lines enables cores 0 to 2 and 4 to 6.

### Memory latency

`cache` mode of the new probe, nanoseconds an access, pinned to logical CPU 2:

| Size | A run 1 | A run 2 | B |
|---|---|---|---|
| 16 KiB | 2.034 | 2.058 | 3.075 |
| 32 KiB | 2.030 | 2.031 | 3.015 |
| 512 KiB | 7.324 | 5.434 | 8.088 |
| 2 MiB | 17.560 | 12.859 | 15.758 |
| 3 MiB | 24.382 | 13.395 | 16.091 |
| 4 MiB | 28.029 | 32.295 | 37.775 |
| 6 MiB | 93.402 | 92.725 | 96.081 |
| 8 MiB | 111.438 | 110.385 | 116.983 |
| 64 MiB | 152.181 | 151.956 | 152.844 |

The plan's positive control is the round 1 state A value at 16 KiB, 4 MiB and 64 MiB (2.086, 21.680 and
152.452 ns). 16 KiB and 64 MiB pass. 4 MiB fails, at 28.029 and 32.295 ns. The 512 KiB to 3 MiB points of the two
state A runs differ by up to 1.82 times. This file therefore reads no point between 512 KiB and 4 MiB.

`latency` mode of the round 1 meter: 16 KiB 2.083 and 3.444 ns, 256 KiB 4.339 and 5.498 ns, 4 MiB 22.106 and
23.518 ns, 64 MiB 152.421 and 153.155 ns, state A and state B.

### SMU, start log and CPUID

| Reading | A | B | B2 |
|---|---|---|---|
| feature mask (`0x3D`), idle and after the load | `0xDD602C7D` | `0xDD602C7D` | `0xDD602C7D` |
| `0x43` per core under load, `smu` phase | 1555 3500 1555 1555 1400 1400 1400 933 MHz | 1555 3500 3500 1555 3500 1400 3500 1555 MHz | 3500 3500 3500 1555 1555 1400 1400 933 MHz |
| `0x43` under load, `cpu` phase, three readbacks | the A pattern three times | the A pattern twice, `3500 3500 1555 1555 1400 1400 1400 933` once | not run |
| work of the load beside the `smu` readback, `dep_mix` | 581.61 ops/us | 257.73 ops/us | 259.29 ops/us |
| start line | `initial 1500MHz/VID101 observed 1000MHz/VID116` | `initial 1000MHz/VID116 observed 1000MHz/VID116` | `initial 1000MHz/VID116 observed 1000MHz/VID116` |
| `RLC_PG_CNTL` before and after the RLC stage | `0x00000008`, `0x00800000` | `0x00800000`, `0x00800000` | `0x00800000`, `0x00800000` |
| CPUID leaves and XCR0, pinned CPU 2 | the reference | byte-identical | byte-identical |
| `core_invariant`, XCR0 | `0x000C0800`, `0x7` | `0x000C0800`, `0x7` | `0x000C0800`, `0x7` |

The core index 1, which reads 3500 MHz in every readback, is the loaded core if the index follows the logical CPU
order, which no file proves. No CPUID dump was taken after the warm restart.

### PMC sources

`A-pmc.txt`: `wpr.exe -pmcsources` prints `Maximum selectable profile sources: 16.` and lists 18 sources,
among them `TotalIssues` (2), `TotalCycles`
(19), `InstructionRetired` (25), `FRRetiredx86Instructions` (28) and `InstructionsRetiredFixed` (32).
`xperf.exe` and `tracelog.exe` are not installed. `typeperf.exe` is present. The CIM control read
`processor_performance` 68, 69 and 59 for instances 0,0, 0,1 and 0,10 at idle.

### The causal arms

| Arm | State | Before | During | After |
|---|---|---|---|---|
| CPU clock ceiling 2800 MHz | A | 3490.3 MHz, k 0.2650 | 2792.3 MHz, k 0.2641 | 3489.6 MHz, k 0.2639 |
| CPU clock ceiling 2800 MHz | B | 3406.5 MHz, k 1.8086 | 2724.7 MHz, k 1.7769 | 3405.2 MHz, k 1.8132 |
| GPU load | A | 3490.8 MHz, k 0.2644, gfx 10.2 W | 3490.2 MHz, k 0.2657, gfx 58.0 and 58.7 W | |
| GPU load | B | 3410.4 MHz, k 1.8069, gfx 17.2 W | 3396.5 MHz, k 1.8066, gfx 33.2 and 37.5 W | |
| GPU DPM floor 1500 MHz | B | 3405.6 MHz, k 1.8123, gfx 17.4 W | 3409.5 MHz, k 1.8057, gfx 22.0 W | 3409.4 MHz, k 1.8158, gfx 17.2 W |
| second device restart | B2 | | | 3406.0 MHz, k 1.8146, gfx 17.2 W |

- Under the ceiling in state A, `add_x24` delivers 9631.89 work/us against 12035.50, which is 0.800.
- Under the ceiling in state B, k is 1.8 % below its own base. The other nine state B and B2 runs span 1.8045 to
  1.8158.
- Both GPU loads ran at 2000 MHz, 1000 mV (SMU VID 88) and busy 96.6 to 96.8 %. The SMU metrics give the GFX
  rail 987 mV in state A and 993 mV in state B. Package power reads 100.8 and 104.5 W in state A and 74.8 and
  77.4 W in state B. The loader is `vkmembw` with a 90 s warm-up, killed when the measurement ended, so it printed
  no bandwidth.
- The `after_gfx_w=17.2` of both load phases was read 3 s after the load, at 1000 MHz during the descent. It is not
  an idle reading.
- At the floor the GPU ran at 1500 MHz, 919 mV and SMU VID 100, with Tctl 63.9 to 64.0 C. After `dpm floor off`
  it returned to 500 MHz, 820 mV and VID 116.

### Power at the idle point and at the wall

| Reading | A | B | B2 | A after T19 |
|---|---|---|---|---|
| gfx rail at 500 MHz, 820 mV, busy 0.0 % | 9.6 to 10.2 W | 16.6 to 17.4 W | 17.2 W | 10.2 W |
| wall, the plug sample before each phase | 80.7 to 81.3 W | 90.8 to 94.9 W | 91.0 and 93.6 W | 81.7 W |

The state A wall range leaves out two samples: 88.4 W at 12:17:22, the first sample of the session, and 84.8 W at
12:20:29, the first sample after the load phase. The samples after the two load phases, 141.3 W in state A and
169.6 W in state B, were read 4 s after the loader was killed, and the plug's measurement age is unknown. They are
not a reading of load power.

The highest Tctl of the arm is 71.1 C, under the GPU load of state A.

## Not in this directory

- The plan of the arm and the mechanism document that it tests. They are local lab documents.
- The development-PC runner `bd093law-run.py`, because it names this workstation's directory layout. It is a
  wrapper around `tools/win/target.py` with one subcommand a step, each with its own SSH bound, and it took the
  plug sample around each phase.
- `bd093law.exe` itself. `scripts/bd093law.cs` is its source and its SHA-256 is above.
- The `final-*` copies of the last pull. They are byte-identical to the files here, except
  `final-A-state.txt`, which holds both state reads and is `A-state.txt` here. The CPUID, law and other A files
  of that pull are the state A leg again, not a second run.

## What these files do not show

- The mechanism. No file reads a dispatch, issue or retire counter of a core. WPR lists such sources, and this
  arm did not record them.
- The core clock itself. The two witnesses are lower bounds that rest on a 3-cycle latency of `imul r64` and of
  `addsd`. No file reads APERF or MPERF directly.
- The delivered GPU clock or the GPU work under load. The loader printed no bandwidth.
- The state A answer of the DPM floor excursion.
- Whether the pair sums of state B are concurrent. The `pairs` mode does not align its windows.

## Privacy

Nothing was changed before the copy, so the raw files are the output of the tools. Checked and found clean: no
IPv4 address, MAC address, serial number, Wi-Fi name or user name. The word `serial` in the files is the request
counter of `cpu readback` and of `dpm floor`. The host name `BC250-A` stays in the `BEGIN` line of each capture.
The GPU device instance path, including its `4&19CAF403&0&0041` suffix, stays as the tools printed it, as in
round 1. The GUID `{1ca05180-...}` in `A-state.txt` and in the `smu` and `clockcap` logs is the public display
adapter interface class, not a device identifier. The `ctxFFFF...` values in the `*-kmdlog.txt` files are kernel
addresses of that boot.

`sha256.txt` holds the SHA-256 of every file of this directory except itself.
