# K137: a KMD stop leaves the CPU slower until a Windows restart (2026-10-07, BD-093)

Unit A, Windows 11, desktop on the GPU route, 40 CU. Every run was one bounded script over SSH
(`python tools/win/target.py ps <script>`), shorter than three minutes. The times in the file names of the dev PC
and the `HH:mm:ss` stamps that the scripts print are lab local time (UTC+2). This README gives UTC.

The analysis of these files is [`docs/research/k137-kmd-stop-cpu-cap.md`](../../../docs/research/k137-kmd-stop-cpu-cap.md).
The facts are M832 to M834 in [`docs/facts/kmd.md`](../../../docs/facts/kmd.md).

## Builds

| Name | Kernel driver | Where the version comes from |
|---|---|---|
| tester.20 | release payload of KMD 0.7.216.14, device `DriverVer` 0.7.216.100, `bc250kmd.sys` SHA-256 `BB58D62A...` | `lab-restore-t20.txt`, `lab-restore-t20-2.txt` |
| candidate | KMD 0.7.216.15, branch `kmd/k137-cpu-baseline`, commit `e0e01993`, `bc250kmd.sys` SHA-256 `3B7220FF...` | `lab-deploy.txt`, the `device OK 0.7.216.15` lines of `lab-ab-*.txt` |

The candidate adds the SMU metrics clocks to `bc250kmd_cli dpm` (the `smu clocks` line) and the `StopDpmFloor`
switch. `k137-*.txt` and `cpu-log-453.txt` do not print a driver version. The operator's notes of that morning
give tester.20 for them.

## Instruments

- **Counter probe** (`lab-ab.ps1`, `k137-overlay.ps1`, `cpu-ceiling.ps1`): a PowerShell busy loop in a job,
  pinned to logical processor 2, and the counter `\Processor Information(0,2)\% Processor Performance`. The
  scripts print `perf %` and `3194 x perf / 100` as MHz (3194 MHz is the nominal clock that Windows reports).
- **Busiest-instance probe** (`k137-stopstart.ps1`, and the probe that wrote `k137-1-boot.txt` and
  `k137-2-devrestart.txt`): the same busy loop, not pinned. The script reads the counter of the busiest
  instance. Other load on the machine dilutes these values.
- **Fixed-work probe** (`bundle.ps1`): a dependent chain of 64-bit multiply, add, shift and XOR in C#, on one
  thread pinned to logical processor 2. It prints iterations per microsecond, 600 million iterations a run, three
  runs. It touches no memory. The script also runs `vkmembw` (256 MiB, GPU timestamps, copy and read checked
  against CPU oracles) and prints one `bc250kmd_cli dpm` line.
- **SMU metrics** (`bc250kmd_cli dpm` of the candidate): the SMU metrics table. `cpu cores` is the table field
  `CoreFrequency[]`, not the answer of SMU message `0x43`.

## Files

| File | Script | Time (UTC) | Build | Content |
|---|---|---|---|---|
| `cpu-log-453.txt` | | about 10:17 | tester.20 | KMD log lines of session 453 and `cli cpu`: `reads 20 writes 0`, `CPU sent 0 refused 0`, P-state table |
| `k137-1-boot.txt` | busiest-instance probe | about 10:28 | tester.20 | after a Windows restart: perf 107.4 %, 3429 MHz |
| `k137-2-devrestart.txt` | busiest-instance probe | about 10:30 | tester.20 | after `pnputil /restart-device`: perf 86.2 %, 2753 MHz |
| `k137-3-stopstart.txt` | `k137-stopstart.ps1` | 10:37:40 to 10:38:16 | tester.20 | before, after `/disable-device`, after `/enable-device` |
| `k137-4-nosmu.txt` | `k137-stopstart.ps1` | 10:42:13 to 10:42:47 | tester.20, `EnableNativeSmu` 0 | the same three steps without the SMU owner |
| `k137-5-overlay.txt` | `k137-overlay.ps1` | 12:32:13 to 12:33:34 | tester.20 | the overlay on, a device restart, the overlay stopped twice, the overlay started again |
| `lab-deploy.txt` | `lab-deploy.ps1` | about 12:38 | candidate | installation of the candidate, then a Windows restart |
| `lab-ab-1.txt` | `lab-ab.ps1 -Label r1` | 12:44:08 to 12:44:51 | candidate, defaults | state A, `pnputil /restart-device`, state B |
| `lab-ab-2.txt` | `lab-ab.ps1 -Label r2` | 12:56:51 to 12:57:31 | candidate, `StopDpmFloor` 0 | the same |
| `lab-ab-3.txt` | `lab-ab.ps1 -Label r3` | 13:17:17 to 13:18:00 | candidate, `EnableFanControl` 0 | the same |
| `smu-log-r3-after.txt` | `smu-log.ps1` | about 13:19 | candidate, `EnableFanControl` 0 | filtered KMD log of the start after the device restart of r3 |
| `boot-events-1.txt` | `boot-events.ps1` | about 13:26 | candidate | System events 12, 13, 41, 109, 1074, 6005, 6006, 6008 from 11:57 to 13:25 |
| `smu-log-fresh.txt` | `smu-log.ps1` | about 13:27 | candidate | filtered KMD log of the first start after the boot of 13:25:42 |
| `lab-restore-t20.txt` | `lab-restore-t20.ps1` | about 13:42 | tester.20 | tester.20 installed again, then a Windows restart |
| `lab-restore-t20-2.txt` | `lab-restore-t20.ps1` | about 14:47 | tester.20 | the same, after the hang-recovery trials of another branch |
| `bundle-1.txt` | `bundle.ps1 -Label b1` | 14:50:40 | tester.20 | first run, see the notes below |
| `bundle-1-stateB.txt` | `bundle.ps1 -NoRestart -Label b1-k137` | 14:51:02 | tester.20 | K137 state, run 1 |
| `bundle-1-stateB2.txt` | `bundle.ps1 -NoRestart -Label b1-k137r` | 14:51:34 | tester.20 | K137 state, run 2 |
| `bundle-1-stateA.txt` | `bundle.ps1 -NoRestart -Label b1-fresh` | 14:54:02 | tester.20 | after a Windows restart (14:51:53, boot 14:52:28), run 1 |
| `bundle-1-stateA2.txt` | `bundle.ps1 -NoRestart -Label b1-fresh2` | 14:54:51 | tester.20 | the same boot, run 2 |
| `scripts/` | | | | every script named above |

In the `lab-ab` and `bundle` files, state A is a boot with no KMD stop since the boot. State B is the state after
one KMD stop. The project calls state B "the K137 state".

## Results

### Counter probe, candidate (`lab-ab-*.txt`)

| Run | Switch | Boot before state A | State A | State B | Idle gfx and package power, A | Idle gfx and package power, B |
|---|---|---|---|---|---|---|
| r1 | defaults | warm restart, 12:42:59 | 109 %, 3481 MHz | 87 %, 2779 MHz | 10.8 W, 42.9 W | 17.2 W, 48.3 W |
| r2 | `StopDpmFloor` 0 | warm restart, 12:45:44 | 109 %, 3481 MHz | 87 %, 2780 MHz | 10.2 W, 36.6 W | no reading: the next start failed |
| r3 | `EnableFanControl` 0 | AC cycle, 13:12:44 | 109 %, 3481 MHz | 87 %, 2779 MHz | 10.8 W, 37.2 W | 17.2 W, 45.9 W |

The idle power readings are the KMD's SMU metrics at the GPU idle point, 500 MHz, 820 mV, SMU VID 116, busy 0.0 %.
In all ten SMU readings of these files, under load and at idle, the SMU metrics show socclk 1254 MHz, memclk
450 MHz, dclk 1111 MHz and throttler `0x0000`. In the five readings under load the table field `cpu cores` shows
3500 MHz at index 1 in both states. The files do not prove which index is the loaded core.
`smu-log-r3-after.txt` adds gfx 17.8 W at the 1000 MHz start point, and the start line
`initial 1000MHz/VID116`. `smu-log-fresh.txt` has `initial 1500MHz/VID101` and gfx 10.7 W at 500 MHz.

### Fixed-work probe, tester.20 (`bundle-1-state*.txt`)

| File | State | Fixed work, three runs (iterations/us) | Counter perf | vkmembw local copy / read / write (GB/s, median) | `dpm` line |
|---|---|---|---|---|---|
| `bundle-1-stateB.txt` | K137 | 199.3 / 259.5 / 256.4 | 0 % (the counter line failed) | 386.3 / 380.1 / 367.5 | 1400 MHz, 899 mV, 73.0 W, gfx 30.0 W |
| `bundle-1-stateB2.txt` | K137 | 195.4 / 244.7 / 240.9 | 87.2 % | 387.3 / 380.5 / 364.7 | cut off |
| `bundle-1-stateA.txt` | fresh | 504.3 / 577.3 / 575.5 | 76.1 % | 386.8 / 373.5 / 370.2 | 1500 MHz, 919 mV, 81.7 W, gfx 39.7 W |
| `bundle-1-stateA2.txt` | fresh | 501.7 / 508.3 / 581.2 | 77.8 % | 386.1 / 374.6 / 369.5 | 1500 MHz, 919 mV, 88.2 W, gfx 39.7 W |

- Fixed work: the mean of the six fresh runs is 541.4 iterations/us, the mean of the six K137 runs 232.7. The ratio
  is 2.33. The ratio of the best runs is 581.2 / 259.5 = 2.24.
- GPU memory bandwidth: the same in both states, and the same as the earlier baseline of fact M776 (copy 386 to
  389 GB/s). The host-visible placement also gives copy 376.6 to 387.5 GB/s in all four files.
- The counter sampled by the side job of `bundle.ps1` reads lower in the fresh state (76 % to 78 %) than in the
  K137 state (87 %). On this instrument the counter does not agree with the fixed work.

### Exclusions

- **The stop's SMU message.** With `StopDpmFloor` 0 the stop sends no SMU message (r2), and state B still
  shows 87 %. The next start failed: `device Error`, then `no display adapter` (`lab-ab-2.txt`).
- **The fan handback.** With `EnableFanControl` 0 the KMD writes nothing to the fan chip
  (`smu-log-r3-after.txt`, `fan: control off (EnableFanControl 0)`), and state B still shows 87 % (r3).
- **The overlay.** 3481 MHz with the overlay on in a fresh boot. 2779 MHz after a device restart with the
  overlay on, with the overlay task stopped and its process gone (two probes 15 s apart), and with the overlay
  started again (`k137-5-overlay.txt`).
- **A disable alone.** `k137-3-stopstart.txt` shows a lower value while the device is disabled (75.6 %) and after
  the enable (81.5 %). The KMD is not loaded while the device is disabled, so a following start is not
  necessary for the state. These three values come from the busiest-instance probe, not the pinned one.
- **CPU messages.** The KMD sent no CPU message: `cli cpu` reads `reads 20 writes 0`, the session log reads
  `CPU sent 0 refused 0`, and the P-state table is 3200, 2550, 2325, 1960, 1820, 1600, 1271 and 800 MHz in both
  states (`cpu-log-453.txt`, `k137-2-devrestart.txt`).

### `EnableNativeSmu` 0 is not a clean control

`k137-4-nosmu.txt` shows 3472 MHz after the disable and 3473 MHz after the enable, with the SMU owner off. The file
also shows `status Error` after the enable, so that start failed. The KMD source refuses a full-WDDM start
without the SMU owner (`SmuPrepareClock` runs before the GART, PSP and GFX bring-up). So this arm removed the SMU
owner and the whole GFX, PSP, RLC and GART bring-up together. The file does not show the device state before the
disable. The value before the disable, 3221 MHz, is below the fresh value. The arm shows that the PnP disable
and the reaction of Windows to it do not cause the state. It does not show that the SMU owner causes it.

### Two warm restarts hung

`boot-events-1.txt` shows four warm restarts of this bisect:

| Event 1074 (restart) | Shutdown | Next start | Result |
|---|---|---|---|
| 12:38:32 | event 13 at 12:42:44 | 12:42:59 | normal |
| 12:45:14 | event 13 at 12:45:29 | 12:45:44 | normal |
| 12:59:10 | event 13 at 12:59:24, a clean OS shutdown | 13:12:44 | no boot until an AC cycle |
| 13:21:38 | no event 6006 or 13 | 13:25:42 with event 41 | hung in the shutdown, AC cycle |

The restart of 12:59:10 came after the failed start of r2. The file also holds two unexpected restarts at 12:06:43
and 12:14:48 (event 41). Those belong to earlier work of that day, and this directory does not explain them.

## Notes on the files

- `bundle-1.txt`: the first run defined its function under the name `Measure`. PowerShell resolves `Measure` to
  the alias of `Measure-Object`, so that run measured nothing and printed an empty line. Its
  `pnputil /restart-device` ran, and `device OK` is its output. That device restart made the K137 state of
  `bundle-1-stateB*.txt`. A cpu-ceiling probe before this run read 3481 MHz. Its output is not in a file.
- `bundle-1-stateB.txt` ran a version of `bundle.ps1` whose counter line read `$c.CounterSamples` and failed.
  The fixed work and `vkmembw` in that file are valid. `scripts/bundle.ps1` is the corrected version that wrote
  the other three files.
- `bundle-1-stateB2.txt` ends in the middle of the `dpm` line. The dev PC closed the pipe of the copy early.
- `lab-ab-2.txt`, state B: the device did not start, so there is no `dpm` reading.
- The smart-plug samples of game sessions 447, 452 and 454, which the research note quotes, are not in this
  directory.

## What these files do not show

- The mechanism. No file reads a delivered clock of a CPU core or of the GPU. The SMU readings that change
  nothing (`cpu cores`, socclk, memclk, throttler) can be target readbacks.
- Whether the 2.2x of the fixed work and the 20 % of the counter probe measure the same loss. The two probes run
  different code and read different instruments.
- Whether the extra 6.4 W of gfx power at idle and the CPU loss have one cause.
- The GPU side. The `dpm` lines of the bundle are at different GPU clocks (1400 against 1500 MHz) and do not
  compare GPU work. No fixed GPU workload ran in both states.
- Why two warm restarts hung, and whether the K137 state plays a part in it.

## Privacy

Nothing was changed before the copy. Checked and found clean: no IPv4 address, MAC address, serial number, UUID,
Wi-Fi name or user name. The host name `BC250-A` stays in `boot-events-1.txt`. The scripts match the GPU by its
hardware ID with a wildcard, and no file holds a device instance suffix.

`sha256.txt` holds the SHA-256 of every file of this directory.
