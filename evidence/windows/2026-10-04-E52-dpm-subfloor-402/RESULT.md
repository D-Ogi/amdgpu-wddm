# E52: the firmware accepts a GFX clock below 1000 MHz (session 402)

Date: 2026-10-04, 23:37-23:59Z. Unit A, Windows 11 Pro build 22631, kernel driver 0.7.205.1
(ABI `0x000700CD`, GUI package b17 = 0.7.205.100-tester.11), `DpmMaxMHz` 1500. Workload: the
Rise of the Tomb Raider DirectX 12 benchmark, run to the end twice (56.81 and 49.49 frames/s overall).
No runner thermal stop.

Kernel driver 0.7.205 is the first build with two thermal-only operating points under the lab floor:
900 MHz and 800 MHz, both at 820 mV (VID 116, owner decision 2026-10-05). The load never asks for
them. Only the thermal cap goes there. This directory holds what the firmware did when it got there.

## What the files are

All three are extracts of one file, the driver log of that session
(`scratch\m15\native-caps001\attempts\native-caps402\c\game-kernel.log`, 126 MB, outside this
repository). The extracts are `grep` output, unedited.

| File | Extract | Lines |
|---|---|---|
| `subfloor-samples.txt` | `clock clock backend=kmd-smu ... MHz=800\|900` - the clock readback stream | 53 |
| `governor-subfloor-lines.txt` | `dpm: telemetry` and `dpm: summary` lines that read `SMU 800\|900 MHz VID 116` | 24 |
| `clock-histogram.txt` | the count of every `MHz=<n> VID=<n>` pair in the whole log | 8 |

## What it shows

1. **The firmware ran at 800 MHz and at 900 MHz, both at VID 116.** The clock readback stream holds
   49 samples at `MHz=800 VID=116` (23:45:38.428 to 23:51:29.735) and 4 samples at `MHz=900 VID=116`.
   The governor's own lines report the same operating point 22 times at 800 MHz and 2 times at 900 MHz.
   The two streams are two instruments on the same hardware, which is why their counts differ.
2. **No point was refused.** The string `sub-floor refused`, which `driver/kmd/dpm.c` logs when the SMU
   or the readback rejects a point under the lab floor, is absent from the log (0 matches).
3. **Temperature at the sub-floor points.** At 800 MHz: 81.875 to 87.750 C, mean 84.55 C (n = 49).
   At 900 MHz: 81.500 to 81.750 C (n = 4). 87.750 C is also the maximum of the whole session.
4. **The cap, not the load, asked for it.** Every governor line in `governor-subfloor-lines.txt` reads
   `want 1000 cap 800` (or `cap 900`) with `throttle thermal-soft` and GPU busy 96.8 % to 100.0 %.
5. **The clock histogram of the session**: 800 MHz 49, 900 MHz 4, 1000 MHz 170, 1100 MHz 181,
   1200 MHz 45, 1300 MHz 17, 1400 MHz 15, 1500 MHz 5 (486 readbacks in all).

## What it does not show

- It is one session on one unit. It does not show the lowest clock this firmware accepts: the table
  stops at 800 MHz, so nothing under 800 MHz was requested.
- It does not show that the SMU accepts a clock under 1000 MHz with any voltage other than 820 mV.
  Both sub-floor points carry the floor's own VID 116.
- Nothing here measures what the sub-floor buys. The benchmark rate and the temperature curve of this
  session are not a controlled comparison against a build without the sub-floor.

## The record this corrects

Six comments and one line of `docs/hardware.md` said that the firmware has never run below 1000 MHz
and cited fact M47. M47 records what the power tables publish (`sclk` levels 1000, 1500 and 2000 MHz,
overdrive range 1000 to 2000 MHz), which is a different statement. The 1000 MHz bound of the imported
Linux path (`CYAN_SKILLFISH_SCLK_MIN`) is amdgpu's own clamp, not a measured hardware floor. Fact
M785 records what this session measured. The withdraw-on-refusal mechanism stays as insurance for a
part that does refuse.
