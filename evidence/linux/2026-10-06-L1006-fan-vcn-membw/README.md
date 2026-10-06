# L1006 visit 1: fan controller, video block, amdgpu power surface, memory bandwidth and heat

Date: 2026-10-06, about 12:34 to 12:48 UTC. Unit A, the diagnostic stick's Linux (Alpine, kernel
`6.18.52-0-lts`), GRUB entry `bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand once).
Commands ran over SSH as root. The machine came from Windows by the stick's loader steering
(`scripts/to-linux.ps1`) and went back the same way (`scripts/to-windows.sh`). No firmware boot entry
changed.

## The clock of this boot

The Linux clock of unit A read about 2 h ahead of UTC (the RTC holds local time). The skew is
2 h 00 min to the second: the thermal run starts at Linux `14:43:09` and the smart plug, read on the
development PC in UTC, shows the load step at `12:43:09`. The directory names of the raw outputs
(`collect-20261006T143733Z` and so on) carry the skewed clock. The UTC times in this file are the
Linux times minus 2 h. The `t` fields of the fan files are Unix seconds of the same skewed clock.

## What ran, in order

| UTC | Step | Tool | Output |
|---|---|---|---|
| 12:36:07 | Fan controller read, amdgpu not loaded | `scripts/fanprobe.py` | `fan/fanprobe-read.jsonl` |
| 12:36:19 to 12:37:00 | Fan controller write test on fan index 1, then restore | `scripts/fanprobe.py write` | `fan/fanprobe-write.jsonl` |
| 12:37:33 | `modprobe amdgpu` once, then read-only collection | `scripts/amdgpu-collect.sh` | `collect/` |
| 12:41:35 | `vkmembw` under RADV: both placements, no warmup, negative control | `scripts/membw-run.sh` | `membw/` |
| 12:43:09 to 12:45:14 | 110 s of continuous copies, 1 Hz samples, then 15 s of cooling | `scripts/thermal-load.sh` | `thermal/` |

The development PC read the smart plug's power during the thermal run (`thermal/plug-wall-watts.txt`,
UTC, watts, plug telemetry as `plug.py` scales it).

## Fan controller (NCT6686D embedded controller)

`fanprobe.py` reads and writes the EC through `/dev/port` (base `0x0A20`, page, index and data ports at
base + 4, 5, 6). No `nct668x` driver ran, and the script refuses to run if one is. The register
meanings come from the mainline `nct6683` driver and from `nct6687d`.

Read, with amdgpu not loaded:

- Customer ID at `0x602` reads `0x16 0x2B`, that is `0x162B`. The mainline driver's list names this value
  AMD.
- Build date bytes at `0x604` to `0x606` read 21, 7, 28 (2021-07-28). Firmware version at `0x608` reads
  1.0. Both agree with the E01 kernel log (`07/28/21`, `1.0`).
- `HWM_CFG` at `0x180` reads `0x80` (monitoring on). Tachometer inputs 0 to 4 and duty outputs 0 to 4 are
  present.
- Fan index 1 turns at 1360 RPM. The other four tachometers read 0 RPM.
- All five duty read-backs (`0x160` to `0x164`) read 204. The write targets `0xA28` to `0xA2F` all read
  128.
- Mode mask `0xA00` reads `0xE0`: bits 0 to 4 are clear, so the EC runs its own curve on all five
  channels, and bit 1 is clear. Command `0xA01` reads 0. Engine status `0xCF8` reads `0x60`
  (`CFG_CHECK_DONE` and `CFG_LOCK`).
- One register read (four port accesses, each one a `pread` or `pwrite` system call) costs 9.38 us
  (9.40 us in the second run). This number includes the system call cost, so it is an upper bound for
  the bus.

Write test on fan index 1 (the fan that turns), 1 Hz samples:

1. Before: 1395 to 1400 RPM, duty read-back 204, Tctl 70.375 C.
2. Open handshake: the script writes `0x80` to `0xA01`. Status goes from `0x60` to `0x08` (`CFG_PHASE`)
   after one 1 ms poll. Close: the script writes `0x40`. Status goes from `0x08` to `0x60` after three or
   four polls. `CFG_INVALID` never sets, and `CFG_LOCK` is set after every close.
3. Mode bit 1 set (`0xA00` reads `0xE2`), write target 255: the write target reads back 255. The duty
   read-back reads 255 one second later. The fan reaches 1741 RPM after 3 s and stays at 1699 to 1749.
4. Write target 102 (40 %): read-back 102 after one second. The fan falls to 794 to 800 RPM after 3 s and
   to 771 to 774 RPM after 10 s.
5. Restore: write target 128 and mode `0xE0`, both read back. The duty read-back returns to 204 within
   one second. The fan reads 1232, 1330 and 1357 RPM at 1, 2 and 3 s, then 1360 to 1379 RPM. The EC curve
   then moves the duty to 207 by itself.

The duty read-back of channel 1 follows the written value. The other four channels were not sampled
during the write, so this test does not show whether their read-back is a separate value.

## amdgpu, video block and power surface (`collect/`)

amdgpu loaded once, 184 s after boot, with `modprobe exit 0`. The collection wrote nothing to the GPU
after the load. Results:

- IP blocks (`collect/dmesg-after.txt`): common, gmc, ih, psp, smu, dm, gfx, sdma. There is no VCN and no
  JPEG block.
- IP discovery hardware id 12 (UVD/VCN, `collect/ip_discovery.txt`): major 2, minor 0, revision 3,
  instance 0, `harvest 0x0`, three base addresses `0x00007800 0x00007E00 0x02403000`.
- `collect/firmware_info.txt`: VCN, UVD and VCE firmware version 0. SMC `0x00580600` (88.6.0). VBIOS
  `113-AMDRBN-003`.
- `collect/pm_info.txt`: `VCN: Powered down`. 1500 MHz SCLK, 450 MHz MCLK, 906 mV VDDGFX, 1199 mV VDDNB,
  58.06 W average and 64.02 W current SoC power, GPU temperature 70 C.
- `collect/pm-sysfs.txt`: `pp_dpm_sclk` levels 1000, 1500 (active) and 2000 MHz. `pp_dpm_mclk` and
  `pp_dpm_fclk` 450 MHz, `pp_dpm_socclk` 1254 MHz. `power_dpm_force_performance_level` is `auto`.
  `pp_od_clk_voltage` shows 1500 MHz at 906 mV and the range SCLK 1000 to 2000 MHz, VDDC 700 to 1129 mV.
  The file `pp_features` does not exist. `gpu_busy_percent` answers "Not supported". VRAM is 8589934592
  bytes, all CPU visible. GTT is 4020457472 bytes (3834 MiB).
- `collect/hwmon.txt`: the amdgpu node has `freq1`, `in0` (vddgfx 906 mV), `in1` (vddnb 806 mV), `power1`
  (PPT 58.2 W) and `temp1` (edge 70 C). It has no `power1_cap` file.
- `collect/idle-10s.txt`: columns are Unix time, `gpu_busy_percent` (empty), then `temp1_input` of hwmon0
  (NVMe), hwmon1 (k10temp Tctl) and hwmon2 (amdgpu edge). Tctl falls from 73.375 to 72.75 C in 10 s.

`collect/pre.txt` reads "k10temp before: 56850". That value is the first `temp1_input` in hwmon order,
which is the NVMe Composite sensor (hwmon0), not Tctl. The label in the file is wrong. The Tctl before
the amdgpu load is in the fan files: 70.375 C.

## Memory bandwidth under RADV (`membw/`)

`vkmembw` is `tools/win/vkmembw/vkmembw.c` unchanged, built for Linux with a four-function stand-in for
`windows.h` (`vkmembw-linux/windows.h`) and the SPIR-V headers of the Windows build. Build command in
the reference root: `gcc -O2 -I. -o vkmembw vkmembw.c -ldl -lm`. The Linux binary stayed on the target
and has no recorded hash.

| File | SHA-256 |
|---|---|
| `vkmembw.c` (same as `tools/win/vkmembw/vkmembw.c`) | `e0568f20adb604f1947fe261d1a3df6cd3b76377a60888b47a1398b6cbc3ac7d` |
| `windows.h` (Linux stand-in) | `12d8d80c72358ded9f6462b59123f848903e35933e1ffd41d2edbbba80c0258b` |
| `write_spv.h` | `6b953701cb7086042faf9fd2d6e48dc26a22536f9b347e6d4965f2f8c4ff07d1` |
| `copy_spv.h` | `ccfd165afc752bdd277be57736a547bc6946ee2807f606b3c2186e9427f288be` |
| `read_spv.h` | `0f44fa3257d9f3751b9e7e872fbea65bccbcdd5331cb50943187101d21d79609` |

The three SPIR-V headers are byte identical to the headers next to the Windows binary of E48
(`vkmembw.exe`, SHA-256 `bfa13d6e20d3e86a2df05d62c777a9795e0c9efc92d6cdf83b33a07002f5df6e`).

The Vulkan driver is RADV from the reference root `/opt/bc250` (`scripts/chroot-run.sh`, a read-only
loop mount of the Debian build root on the Windows volume). It reports `AMD BC-250 (RADV GFX1013)`,
driver version `0x06802063` and API 1.4.363. `0x06802063` decodes as 26.2.99, which is how Mesa encodes
a `26.3.0-devel` version.

Medians of 20 timed dispatches, GB = 10^9 bytes, copy counts read plus write, SCLK 1500 MHz in every
sample:

| Run | Placement | write | copy | read |
|---|---|---|---|---|
| `both.txt`, 2 s warmup | local (type 0) | 445.4 | 397.4 | 393.8 |
| `both.txt`, 2 s warmup | host (type 2, heap 0) | 445.5 | 397.2 | 395.7 |
| `cold.txt`, no warmup | local | 445.3 | 396.9 | 395.1 |
| `cold.txt`, no warmup | host | 399.6 | 397.0 | 395.2 |
| `neg.txt`, negative control | local | 445.5 | 397.1 | 394.8 |

Every positive run checks its copy and its read sum and ends `result ok`, exit 0. The negative control
copies and reads one element short. Both of its checks fail and it exits 1 (`membw/exits.txt`), which is
the expected result. The best single write dispatch reads 449.5 to 449.9 GB/s.

## Heat at amdgpu's own clock (`thermal/`)

`thermal-load.sh` runs `vkmembw --warmup-ms 110000 --iters 5 --placement local`: 10039 batches of eight
copies in 109830 ms. The script stops the load if Tctl stays at or above 87 C for 10 s, or reaches 89 C
once, or after 170 s. No stop rule fired.

- SCLK stays at 1500 MHz in all 110 load samples. VDDGFX reads 899 or 906 mV.
- PPT (hwmon `power1_input`) rises from 62.2 W to 80.2 to 85.0 W under load, median 83.2 W.
- Tctl rises from 71.5 C to 81.375 to 81.5 C at the end. Edge rises from 71 C to 81 C.
- Wall power (`plug-wall-watts.txt`): 110.2 W just before the load, 170.9 to 174.4 W under load, 112.8 W
  after it.
- 15 s after the load, Tctl is 79.75 C and PPT is about 60 W.

The load output (`thermal/load.txt`) reads write 445.4, copy 397.0 and read 395.3 GB/s at the end of
the 110 s, the same as the 2 s runs.

## Redaction and hashes

`REDACTED.txt` lists the removed items (a MAC address and the stick's USB serial number in the two
kernel logs). `sha256.txt` holds the SHA-256 of every file in this directory except this README, scripts included.
