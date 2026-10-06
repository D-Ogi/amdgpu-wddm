# BD-075 round 2 on unit A: D3D12 shared resources, 25 rows, 23 as expected

Date: 2026-10-06, 05:29:34Z to 05:34:15Z. Seven passes of the `tools/win/capture-share` shared-resource
kit on unit A. Release 0.7.207.100-tester.12 (train b18r1), kernel driver 0.7.207.1 (ABI `0x000700CF`),
40 CU, GPU desktop composition. Each pass ran with `-Trace`, and each pass stayed far inside the
three-minute bound: the longest pass spent 52 s in its cells, and all seven together spent 229 s.

Of that run context the pass summaries print the kernel driver ABI alone, in their `dpm` lines. The
release name, the CU count and the composition route come from the lab's install record of the day,
which is the local workspace state file and not this repository. Train b19 reached the lab at 06:44Z,
after these passes. The kit prints no release, CU or route line of its own, and it should, as the E52
kit does.

This is the first lab check of round 2 of the BD-075 fix. It is also the first measurement of
cross-API sharing on the GPU D3D11 route (the `.gpu11` rows) and the first completed
`ALLOW_SIMULTANEOUS_ACCESS` row on this driver. The defect is `BD-075` in the local review backlog.
The kit and the client are `tools/win/capture-share`.

## What ran

The D3D12 shell under test is a candidate. It is not in the registered release.

| Role | SHA-256 | Where |
|---|---|---|
| candidate D3D12 shell (under test) | `A08113051B2278A7E77BBA539B9C8ED54FDDAFC6286649A2FCD0CAD8131388B6` | branch `bd075/d3d12-shared-real`, pushed to `C:\BC250\tmp\bd075\amdgpu_wddm_d3d12.dll` |
| registered D3D12 shell (b18r1) | `1926DBAF31DD2EA86EF044CE72C7C0C616E3474CA1C12B7F213BF9702789F4B3` | `C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_d3d12.dll` |
| client and peer | `FD9A6AAFFC6A9B526E46AE94AD34820EB9D26C16262C5948B4C84AF0902A3B5B` | `capshare.exe` and `capshare-peer.exe` |
| D3D11 shell of every `.gpu11` row | `E748418C` (short form, as the kit prints it) | router `GpuUmdPath`, `C:\BC250\m14\app-route-001\gpu\amdgpu_wddm_d3d11.dll` |

Pass 1 swapped the candidate in (`install swapped`). Passes 2 to 6 found it already in place
(`install already`) and kept it (`restore kept (A0811305 stays registered)`). Pass 7 put the release
shell back (`restore swapped`, then `registered 1926DBAF`). The lab therefore ended the check on the
registered release, with no leftover.

The three `.gpu11` passes added `capshare.exe` and `capshare-peer.exe` to the AppRouter allowlist and
restored it afterwards to `[d3d11bench.exe,d3d11mt.exe,d3d11fl12.exe,dxdiag.exe]`. The four other
passes never read the allowlist (`allow not-needed`).

The two sides of a `.gpu11` row are two different driver builds: the D3D12 side is the candidate shell
above, and the D3D11 side is the router's `GpuUmdPath` copy. The kit prints both, so that no row can be
read as one build talking to itself.

## Verdict

| Pass | Trial id | Rows | `TOTAL` | Cells |
|---|---|---|---|---|
| 1 | `20261006T052934Z` | 8, the cpu11 arm and `w12` | `ok=7/8 skips=1` | 52 s |
| 2 | `20261006T053040Z` | 2, the keyed-mutex pair on cpu11 | `ok=1/2 skips=0` | 26 s |
| 3 | `20261006T053113Z` | 5, D3D12 creates and D3D11 opens on gpu11 | `ok=5/5 skips=0` | 37 s |
| 4 | `20261006T053158Z` | 3, D3D11 creates and D3D12 opens on gpu11 | `ok=3/3 skips=0` | 21 s |
| 5 | `20261006T053227Z` | 3, the keyed-mutex rows on gpu11 | `ok=3/3 skips=0` | 38 s |
| 6 | `20261006T053313Z` | 2, D3D12 to D3D12 | `ok=2/2 skips=0` | 27 s |
| 7 | `20261006T053347Z` | 2, D3D12 to D3D12 | `ok=2/2 skips=0` | 28 s |

25 rows ran. 23 of them scored `ok=1`, which means the row matched its `expect=` value. Two scored
`ok=0`, and in both the driver did more than the kit asked for. No row removed a device: every row
reports `removed=0`. One row reports an HRESULT at all, and it is the known CPU-UMD record-length
refusal (`hr=0x8007000E` in `s11to12`).

## Every row

`retries`, `opens`, `refusals`, `fence_signal` and `fence_wait` come from the DDI trace of the D3D12
shell, so they count what the D3D12 side did. A D3D11 open is not in them.

| Pass | Cell | Route A, B | `expect` | `result` | `decided_ms` | Trace | `ok` |
|---|---|---|---|---|---|---|---|
| 1 | `w12` | d3d12, - | pass | pass | 6562 | - | 1 |
| 1 | `s11to11` | cpu11, cpu11 | content | pass | 609 | - | 1 |
| 1 | `s12to11` | d3d12, cpu11 | content | pass | 6188 | retries 1/1, accepted 1 | 1 |
| 1 | `s12to11-srgb` | d3d12, cpu11 | format-alias | pass | 6188 | retries 1/1, accepted 1 | 0 |
| 1 | `s12to11-rgba8` | d3d12, cpu11 | content | pass | 6203 | retries 1/1, accepted 1 | 1 |
| 1 | `s12to11-fence` | d3d12, cpu11 | skip | skip | 5281 | - | 1 |
| 1 | `s11to12` | cpu11, d3d12 | record-length | fail `0x8007000E` | 5219 | refusals 1 | 1 |
| 1 | `neg-s12to11-fence` | d3d12, cpu11 | gate-only | mismatch, gate `violated:fence-1` | 5766 | retries 1/1, accepted 1 | 1 |
| 2 | `km12to11` | d3d12, cpu11 | content | mismatch | 10782 | retries 1/1, accepted 1 | 0 |
| 2 | `km12to11-finish` | d3d12, cpu11 | content | pass | 11344 | retries 1/1, accepted 1 | 1 |
| 3 | `s12to11.gpu11` | d3d12, gpu11 | content | pass | 6062 | retries 1/1, accepted 1 | 1 |
| 3 | `s12to11-srgb.gpu11` | d3d12, gpu11 | content | pass | 6047 | retries 1/1, accepted 1 | 1 |
| 3 | `s12to11-rgba8.gpu11` | d3d12, gpu11 | content | pass | 6656 | retries 1/1, accepted 1 | 1 |
| 3 | `s12to11-fence.gpu11` | d3d12, gpu11 | content | pass | 6687 | retries 1/1, accepted 1 | 1 |
| 3 | `neg-s12to11-fence.gpu11` | d3d12, gpu11 | mismatch | mismatch, gate `violated:fence-1` | 5766 | retries 1/1, accepted 1 | 1 |
| 4 | `s11to12.gpu11` | gpu11, d3d12 | content | pass | 5563 | opens 1/1 | 1 |
| 4 | `s11to12-fence.gpu11` | gpu11, d3d12 | content | pass | 6156 | opens 1/1 | 1 |
| 4 | `s11to12-srgb.gpu11` | gpu11, d3d12 | content | pass | 5515 | opens 1/1 | 1 |
| 5 | `km11to12.gpu11` | gpu11, d3d12 | content | pass | 10375 | opens 1/1 | 1 |
| 5 | `km12to11.gpu11` | d3d12, gpu11 | content | pass | 11125 | retries 1/1, accepted 1 | 1 |
| 5 | `km12to11-finish.gpu11` | d3d12, gpu11 | content | pass | 11140 | retries 1/1, accepted 1 | 1 |
| 6 | `f12to12` | d3d12, d3d12 | pass | pass | 11625 | - | 1 |
| 6 | `s12to12` | d3d12, d3d12 | content | pass | 11265 | retries 1/1, accepted 1, opens 1/1 | 1 |
| 7 | `s12to12-fence` | d3d12, d3d12 | content | pass | 11828 | retries 1/1, accepted 1, opens 1/1 | 1 |
| 7 | `s12to12-simultaneous` | d3d12, d3d12 | content | pass | 11922 | retries 1/1, accepted 1, opens 1/1 | 1 |

A "-" in the trace column means every counter is zero.

Counted over the 25 rows: 16 rows create a shared surface through the D3D12 shell, and each of them
shows exactly one refused allocation shape and one accepted retry (`retries=1/1 accepted=1`). Seven
rows open a shared handle through the D3D12 shell, and each shows `opens=1/1`. One D3D12 open was
refused, and it is the `s11to12` row. Four rows need neither (`w12`, `s11to11`, `s12to11-fence` and
`f12to12`).

The accepted retry is one line in the trace of each creating row, for example in `s12to11`:

```
shared surface: the runtime refused the ordinary allocation shape; retrying as a shared linear
surface (format 87, 256x256, heap of 262144 bytes aligned to 65536 becomes 262144 bytes, default
alignment, pitch 1024)
```

The format number in that line follows the row: 87 (`B8G8R8A8_UNORM`) in `s12to11`, 91
(`B8G8R8A8_UNORM_SRGB`) in the two sRGB-creating rows, and 28 (`R8G8B8A8_UNORM`) in `s12to11-rgba8`.

## What this measures for the first time

1. The GPU D3D11 route, 11 rows, 11 as expected. The rows are `s12to11.gpu11`, `s12to11-srgb.gpu11`,
   `s12to11-rgba8.gpu11`, `s12to11-fence.gpu11`, `neg-s12to11-fence.gpu11` (pass 3),
   `s11to12.gpu11`, `s11to12-fence.gpu11`, `s11to12-srgb.gpu11` (pass 4) and `km11to12.gpu11`,
   `km12to11.gpu11`, `km12to11-finish.gpu11` (pass 5). Both sRGB directions pass. Both fence
   directions pass. Both sides report feature level 12_1 in these rows (`fl=A:12_1,B:12_1`).
2. The negative control of the GPU route does what a control must do.
   `neg-s12to11-fence.gpu11` skips the GPU wait (`note=f1=1 inject=skip-wait`), and the row ends
   `result=mismatch`, `gate=violated:fence-1`, with both content oracles failing
   (`B:A=mismatch,A:B=mismatch`). A gate that cannot be violated proves nothing, and this one can.
3. D3D12 to D3D12, four rows, all pass: `f12to12`, `s12to12`, `s12to12-fence` and
   `s12to12-simultaneous`. The last is the first completed `ALLOW_SIMULTANEOUS_ACCESS` row on this
   driver.

## The keyed-mutex question, answered

`km12to11` on the cpu11 arm still reads poison. Pass 2 reports `result=mismatch` at
`stage=A`, `content=poison`, `got=rgba:5aa53cc3 want=rgba:eb33c383`, `diff=65536/65536`,
`max_delta=195`, with `checks=B:P0=pass,B:A=mismatch,A:B=pass`. The same cell with
`--creator-finish` (`km12to11-finish`) passes. `km12to11.gpu11` passes without `--creator-finish`.

Those three rows together place the defect. The D3D12 side releases the key correctly, because the
GPU-route opener reads the right pixels from the same handover. The stale read belongs to the
CPU-route opener, which reads through its own mapping and orders nothing against the GPU's write.
That is a limit of the CPU user-mode driver, outside BD-075.

## The two rows that differ from the kit

Both are kit expectations, not driver defects.

1. `s12to11-srgb` on the cpu11 arm. The kit expects a refused open
   (`expect=format-alias`), and the row reports `result=pass` with all three content oracles passing.
   The summary says so in the row itself: `ok=0 why=result=pass is not the expected refused open`.
   The CPU-route opener therefore keeps the sRGB description instead of dropping it. The review claim
   that it drops the sRGB view is refuted by this measurement, and the kit row is the thing to fix.
2. `km12to11` on the cpu11 arm, as above. The kit expects `content`, and the row reports
   `mismatch` with `ok=0 why=result=mismatch at stage=A hr=-`. The cause is the CPU-route opener, not
   the shared-resource path.

## The two fence slots

`fence_signal=0` and `fence_wait=0` in all 25 rows, including every cross-process fence row
(`s12to11-fence.gpu11`, `s11to12-fence.gpu11`, `s12to12-fence`, `f12to12` and both negative
controls), and those rows still pass. So the runtime completes shared-fence work on this single-node
adapter without calling `pfnSignalFence` or `pfnWaitForFence`. What is still not measured is a fence
that another process opened and that reaches these slots.

## The lab after the check

The kernel driver reports its DPM state before and after each pass.

- Tctl 65.0 C before pass 1, and 68.6 C after pass 7. Over all fourteen samples the range is 65.0 C
  to 68.6 C. The 87 C rule was never near.
- The thermal event counter reads `thermal 23 err 0` in all fourteen samples. No thermal event and no
  DPM error happened during the check.
- The GPU idled at 500 MHz and 820 mV before every pass (`throttle idle`) and sat at 1000 MHz and
  820 mV after every pass (`throttle none`). The ceiling stayed at the release default,
  `cap 1500 max 1500`.
- `requested dpm, reason none` in all fourteen samples. The kernel driver owned the clock throughout.

## Not in this check

- The untraced confirmation pass on the installed release. The candidate shell
  `A0811305` rides train b20. Train b19 ships the fail-fast form of the driver, which refuses a
  shared create without removing the device. BD-075 stays open until that pass runs.
- Desktop and window capture. The capture half of M15.13 is a separate trial.
- The per-cell DDI traces. Passes 1 to 6 ran with `-Keep`, so each row left
  `C:\BC250\tmp\bd075\<trial id>\<cell>.ddi.log` on the lab. These files stayed there and are not in
  this directory.

## Files

| Path | What it holds |
|---|---|
| `p1-20261006T052934Z-summary.txt` | pass 1: header, the 8 rows with their trace counters and verdict lines, `TOTAL`, DPM before and after, install and restore |
| `p2-20261006T053040Z-summary.txt` | pass 2, the same shape, 2 rows |
| `p3-20261006T053113Z-summary.txt` | pass 3, 5 rows, plus the two `router` lines of the gpu11 arm |
| `p4-20261006T053158Z-summary.txt` | pass 4, 3 rows |
| `p5-20261006T053227Z-summary.txt` | pass 5, 3 rows |
| `p6-20261006T053313Z-summary.txt` | pass 6, 2 rows |
| `p7-20261006T053347Z-summary.txt` | pass 7, 2 rows, and the restore of the release shell |
| `SHA256SUMS.txt` | the SHA-256 of each other file in this directory |

The seven files are unchanged byte copies of the pulled summaries, byte-for-byte equal to the
operator consoles of the same passes except for the two trailing lines of each console: one empty
line and one `pull C:\BC250\tmp\bd075\<trial id>\summary.txt`. This directory does not repeat them.

The timestamps inside the summaries (`07:29:34.650` and so on) come from the lab's own clock, which
runs two hours ahead of UTC. The trial ids in the file names are UTC.

## Redaction

Before the commit, every copied file was searched for IPv4 addresses, MAC addresses, GUIDs, SSIDs,
serial numbers, user names and host names. The search found none of them, and nothing was changed.
The only absolute paths are under `C:\BC250\` and `C:\Program Files\amdgpu-wddm\`. The hexadecimal
values are SHA-256 file hashes, HRESULTs, DDI flag words and the two RGBA checksums of the
`km12to11` mismatch.
