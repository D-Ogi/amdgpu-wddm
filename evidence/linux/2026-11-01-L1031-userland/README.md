# L1031 userland: matched shader coherency (L33), DP audio tone (L38), stick tools (L14)

Date: 2026-11-01. Boot B, about 00:28 to 00:31 UTC (L33, L38). Boot A of the same night, between 23:56
and 00:01 UTC (L33 Vulkan inventory). Boot C, 01:02 UTC (L14). Unit A, the diagnostic stick's Linux (Alpine,
kernel `6.18.52-0-lts`), GRUB entry `bc250.mode=network` (amdgpu blacklisted at boot, loaded by hand
once per boot). Mesa 26.1.6 (RADV), Vulkan 1.4.354.

## The clock

The Linux clock of unit A read local time (UTC+1) and was about 28 s ahead. `l33-result.txt` starts at
`utc=2026-11-01T01:28:25Z` on that clock, which is about 00:27:57Z. The microphone captures
(`l38-listen*.txt`) were made on the development PC and carry its UTC clock.

## L33: the E27 shader coherency probe on Linux (`out/boot-b/`)

`scripts/l33.sh` builds `shader-coherency-probe.c` of E27 and `vkcompute.c` of E14 on the lab and runs
the probe with the E14 `inthash.spv`. `l33-result.txt` records the SHA-256 of the three sources (SPIR-V
`ca33985e...`, probe `3601637b...`, `vkcompute.c` `2a4d423b...`) and the build (exit 0, two
unused-variable warnings).

Clock: a manual performance level was refused (`power_dpm_force_performance_level`, write error). The
OD write `vc 0 1000 820` and its commit returned 0, and `amdgpu_pm_info` then read SCLK 1000 MHz.

| Run | Rounds | Result | Exit |
|---|---|---|---|
| normal, 16 rounds | 16 of 16 completed | 0 mismatches: each GPU hash equals the CPU oracle | 0 |
| `--stale-input-control` | stopped at round 1 | round 0 matches, round 1 keeps the round 0 hash, mismatch | 1 |

Each run uses three host-coherent buffers of 4 MiB in memory type 3, heap 1 (`same_allocations=1`,
`host_coherent=1`, two dispatches per round). The stale-input control fails as the probe expects, so
the normal run would detect a stale input.

The clock restore commands ran at the end, but the last `pp_dpm_sclk` read shows level 1 at 1077 MHz
as the current level. So this file does not show a return to the default 1500 MHz. The next restart
ended that runtime setting.

`l33-normal.txt` and `l33-stale.txt` are the two probe outputs. The Boot A files
`l33-vulkaninfo-full.txt` and `l33-vulkaninfo-summary.txt` hold `vulkaninfo` of the same Mesa build.
The submit-to-idle times in these files are not comparable with Windows timings.

## L38: DP audio tone (`out/boot-b/`)

`scripts/l38-tone.sh` plays a 1 kHz sine at 48 kHz through `speaker-test` on `hw:0,3` (exit 0) and
reads the DCCG and Azalia DTO registers of `scripts/l38-dto.regs` before, during and after it
(`l38-tone.txt`). All eight registers read the same values in the three reads, for example
`DCCG_AUDIO_DTO_SOURCE` `0x00100010` and `DCCG_AUDIO_DTO1_MODULE` `0x005B6184`. This repeats the
audio result of L38 (audio), which is already in Done.

- The substream files `/proc/asound/card0/pcm3p/sub0/status` and `hw_params` did not exist when the
  script read them. So no controller-side rate was recorded.
- The development PC recorded two microphone captures of the lab (`l38-listen.txt`, 18 s from
  00:29:37Z, and `l38-listen2.txt`, 30 s from 00:30:17Z). Only the analysis numbers are kept. No window
  has a 1 kHz level clearly above its neighbours. This attempt is inconclusive: it does not show that
  the tone was audible or silent.

The H.264 and HEVC encoder half of L38 did not run.

## L14: tools on the stick (`out/boot-c/`, `scripts/`)

`scripts/l14-stick.sh`, `scripts/l14-install.sh` and `scripts/l14b.sh` copy the instruments of this
campaign onto the stick under `bc250/tools` and add a README there. After the last copy in Boot C, the
directory held 72 files, 2.8 MiB, and the stick went back to read-only
(`console-l14-stick.txt`). `scripts/stick-entry.sh` adds a stick GRUB entry with the kernel parameters
`amd_smn_debugfs_enable` and `iomem=relaxed` for the L49 and L35 reads.

## Files

- `out/boot-b/`: L33 and L38, from the userland output directory of Boot B (`bootB-final`).
- `out/boot-a/`: the Boot A `vulkaninfo` files.
- `out/boot-c/console-l14-stick.txt`: one console block from the session tool log.
- `scripts/boot-a/`: the Boot A commands, taken from the session tool log.

## Limits

- L33 is one run of 16 rounds and one control on one Mesa build, at 1000 MHz.
- L38 measured no rate. The microphone result is inconclusive.

## Redaction and hashes

`REDACTED.txt` lists the removed items, counts only. `sha256.txt` holds the SHA-256 of every file in
this directory except itself.
