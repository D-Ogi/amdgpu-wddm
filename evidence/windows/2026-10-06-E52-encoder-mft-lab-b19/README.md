# E52 on unit A: the encoder MFT on train b19, 82 of 82 cases

Date: 2026-10-06. One run on unit A, archive `20261006T081819Z`, 59 s from the first case to the
verdict. Release 0.7.208.100-tester.13 (train b19) at `C:\Program Files\amdgpu-wddm`, 40 CU
(`CuMode 40 applied 40 confirmed 40`). The KMD reports `driver 0x000700D0`, and `0xD0` is 208. No game
and no other trial ran at the same time. The run stayed inside the three-minute bound.

This is the second lab run of the encoder in `driver/umd/mft-h264`. The first is
[2026-10-06-E52-encoder-mft-lab](../2026-10-06-E52-encoder-mft-lab/README.md), 75 of 75 cases on
tester.12 (train b18r1). The host baseline on the development PC is
[2026-10-05-E52-encoder-mft-dev-pc](../2026-10-05-E52-encoder-mft-dev-pc/README.md). The plan is
[experiments/E52](../../../experiments/E52-m15-11-encoder-mft-lab/README.md).

Three things differ from the first run:

1. The binaries come from branch `e52/encoder-perf` at `e4d1f630`, which is the throughput batch and
   the fixes of its two reviews. The first run used `main` at `d5593267`.
2. The installed release is b19 tester.13, and it registers its own transform machine wide.
3. The kit runs 82 cases instead of 75. Three cases are new (`db-waves-320`, `t720-60`, `t1080-60`),
   and the three `--compare` cases now run in the same trial.

## What ran

The kit pins the hash of every binary it pushes and refuses to start when one does not match. The
sizes and hashes below are the kit's own `SHA256SUMS.txt` and the header of `summary.txt`.

| File | Bytes | SHA-256 |
|---|---|---|
| `mfthost.exe` | 656896 | `E316E891191A6EA10A73D0483EC307249AE0054E6E89E7E558842A17B3521D18` |
| `mftreg.exe` | 431616 | `4E9A43966056D3805FDA188A43FF4C6A259059A5DB0D94433D204EC49AD81A95` |
| `amdgpu_wddm_mft_h264.dll` (not pushed) | 510464 | `D3E29E6ED32446BC61ABD3A7AFC46CB3F4C03D1E029A384CC693FA4C0C9872CA` |

Every case loads the transform with `MFTRegisterLocal` from the host's own copy of that DLL. The
installed release keeps its own copy at `C:\Program Files\amdgpu-wddm\mft\amdgpu_wddm_mft_h264.dll`.
The run reports that copy as `release-other: 0.7.208.100-tester.13 (b19) 141969A3`. The run prints the
first eight digits only. The same prefix belongs to the tester.12 release DLL of the first run, whose
full value is `141969A3F212619AD6AAFDEEE011AA3F00FDF8C4DC154409993CFF19F54683D5`.

The first start, at 08:17Z, refused in the preflight and ran no case. The kit pinned the hash of the
machine-wide registration to the DLL it pushes, so it called b19's own registration foreign:
`C:\Program Files\amdgpu-wddm\mft\amdgpu_wddm_mft_h264.dll is 141969A3, not the pinned D3E29E6E`. It
wrote no policy value and `VERDICT exit=1`. `run-b19-1.out` holds that console. The kit now keeps a
table of release DLL hashes and accepts a match as `release-other`. The 08:18Z run reports
`release-other: 0.7.208.100-tester.13 (b19) 141969A3 before and after, not touched by this run`.

The application route:

1. The AppRouter policy was in `allowlist` mode. Its `GpuUmdPath` is
   `C:\Program Files\amdgpu-wddm\d3d11\amdgpu_wddm_d3d11.dll`, the installed release. The first run
   pointed at a staged copy under `C:\BC250\m14\app-route-001\gpu\`.
2. The kit added `mfthost.exe` to `Allow` and set `RouteLogDirectory` to the run directory. After the
   cases it wrote both values back. The readback was identical to the values before the run.
3. The router wrote 81 route lines into 80 files. One file holds two lines, because Windows gave the
   same process id twice. Every line reads `route=gpu reason=app-allowed fallback=0` with the GPU UMD
   above as `module=`. Only `selftest` has no route line, because it opens no device.

The kit is `lab-e52.ps1` in the local workspace, `scratch/m15/video-encode/lab-e52`, outside this
repository. The state that ran this trial is 77140 bytes with SHA-256
`A35A960B95265CE6FB1BA9A04B5B72599BBBB9341E7165F8CBCC11E4D606CD0C`. Its host test of the scoring rule
(`lab-e52-rule-test.ps1`, 138 checks, 0 failed) ran on the state before the `release-other` change.
This directory holds no record of a rule-test run after that change.

## Verdict

`TOTAL ok=82/82 run, 82 in the table, cases_s=58, elapsed_s=59` and `VERDICT exit=0: every case that
ran passed and the lab is as it was`.

1. `selftest`: `selftest PASS: 0 failure(s)`. Its `.err` file is empty.
2. 76 encode cases, 386 pictures in all, and every picture bit exact against unit A's inbox H.264
   decoder. The per-case counts are 3 cases of 1 picture, 53 of 3, 7 of 4, 7 of 6, 3 of 8, 1 of 10 and
   2 of 60. The inbox decoder returned every picture it was given in all 76 cases. `nnz agreement GPU
   vs CPU: exact (0 disagreements)` in all 76.
3. 74 of the 76 encode cases print `import exact`. The two `--gpu-source` cases do not, and the
   scoring rule does not ask for it there, because there is no system-memory source to compare.
4. Every case matched the bit rate and the PSNR(Y) that `lab-e52.ps1` pins for it. The summary prints
   the pinned value next to the measured one on each row. Those pinned values were recorded again from
   this build before the trial, so they differ from the first run: `t720-1` wrote 209758 bytes at
   47.23 dB here against 215930 bytes at 47.21 dB on tester.12, and `qp-26` wrote 10175 bytes at
   45.72 dB against 10557 bytes at 45.55 dB. The perf branch changed the streams, and unit A agrees
   with the development PC on the new ones.
5. `mft-720`: the asynchronous hardware MFT contract, end to end. `MFT_ENUM_ADAPTER_LUID` is
   `00000000:00008aca`, the LUID that `mftreg --enum` read for `1002:13FE` on this boot. The inbox
   decoder accepted 6 of 6 access units and the drain completed. The stream has 217641 bytes at
   8705640 bit/s, and 7.82 ms per picture through the transform interface. The transform asked for 2
   pictures before its first access unit, so it pipelines.
6. `sink-720`: the sink writer wrote a 197615 byte `.mp4` in 74.4 ms, with 6 pictures through our
   transform. The container is the inbox MP4 sink, so the kit records the size as a note only.
7. 81 of the 82 cases printed `client device on adapter 1002:13FE`. The one that did not is
   `selftest`, which opens no device.
8. The lab after the run: Tctl 65.1 C before and 68.1 C after, overlay source. KMD faults 0 to 0 and
   timeouts 0 to 0. The System log has no TDR (4101/4116/117/141), no bugcheck (41/1001) and no WHEA.
   The policy was restored and read back identical.

## Throughput

The two 60-picture cases are where the throughput targets are read. Both encode 60 pictures, time the
last 50 and run the same 60 pictures a second time with two pictures in flight. "GPU busy" is the
device's own timestamp of the dispatches. "Map wait" is the serial pass waiting for the result.

| Case | Serial ms | Serial /s | Pipelined ms | Pipelined /s | GPU busy ms | Readback ms | CAVLC ms | Map wait ms |
|---|---|---|---|---|---|---|---|---|
| `t720-60`, 1280x720, qp 26, deblocking | 6.54 | 153.0 | 2.46 | 406.3 | 3.61 | 4.25 | 1.70 | 3.93 |
| `t1080-60`, 1920x1080, qp 26, deblocking | 11.71 | 85.4 | 4.59 | 218.1 | 6.53 | 7.57 | 3.42 | 6.89 |

The pipelined pass wrote the same bytes as the serial pass in both cases: 1932665 and 4176036. Its map
wait is 0.00 ms in both. The serial pass spends more time waiting for the result than the GPU spends
working.

For scale, the same `t720-1` case took 29.92 ms per picture with 24.08 ms of GPU busy on tester.12 and
takes 9.84 ms with 4.89 ms here.

## Against the inbox encoder

`mfthost --compare` encodes one source with our encoder and with the inbox software `H264 Encoder MFT`
of Windows, at the same constant bit rate, and then decodes both streams. The rate is 6000000 bit/s at
1280x720 and 12000000 bit/s at 1920x1080. The bit rate rows use the nominal rate of 30 pictures per
second. The 60-picture rows time 50 of the 60 pictures. The 6-picture row times all 6.

| Case | Encoder | Bytes | bit/s | ms per picture | Pictures per second | PSNR Y (dB) | PSNR Cb (dB) | PSNR Cr (dB) |
|---|---|---|---|---|---|---|---|---|
| `compare-720`, 6 pictures | ours | 197185 | 7887400 | 10.04 | 99.6 | 47.07 | 46.19 | 45.18 |
| | inbox | 185964 | 7438560 | 8.07 | 123.9 | 45.85 | 46.72 | 46.53 |
| `compare-720-60`, 60 pictures | ours | 1542203 | 6168812 | 6.46 | 154.9 | 44.31 | 41.57 | 40.66 |
| | inbox | 1607210 | 6428840 | 1.51 | 662.1 | 45.65 | 45.39 | 45.60 |
| `compare-1080-60`, 60 pictures | ours | 3089727 | 12358908 | 11.51 | 86.9 | 43.75 | 40.56 | 39.16 |
| | inbox | 3320028 | 13280112 | 2.65 | 377.0 | 46.06 | 45.50 | 45.16 |

The decoder returned all pictures in each row: 6, 60 and 60. Our pipelined pass reaches 6.65 ms and
150.5 pictures per second over 6 pictures, 2.75 ms and 363.3 over 60 at 720p, and 4.55 ms and 219.8
over 60 at 1080p. It held 2 pictures at once over 5 of 6 and over 50 of 60 pictures. The byte and PSNR
rows are the serial pass.

Each compare case also prints the gap picture by picture. The columns are the intra picture, the mean
over the rest, the worst picture and its index.

| Case | Plane | Picture 0 | The rest | Worst | At |
|---|---|---|---|---|---|
| `compare-720-60` | Y | -1.44 | -1.34 | -2.88 | 33 |
| | Cb | 3.06 | -3.94 | -8.47 | 30 |
| | Cr | 2.80 | -5.06 | -9.45 | 30 |
| `compare-1080-60` | Y | -1.81 | -2.32 | -4.13 | 19 |
| | Cb | 3.08 | -5.07 | -7.95 | 30 |
| | Cr | 2.81 | -6.15 | -10.67 | 30 |

What the numbers mean:

1. Our encoder is bit exact and conformant on unit A, and it is still slower than the inbox CPU
   encoder on the BC-250's own CPU. Over 60 pictures the inbox encoder is 4.3 times faster at 720p
   (1.51 ms against 6.46) and 4.3 times faster at 1080p (2.65 ms against 11.51). Against our pipelined
   pass it is 1.8 times faster at 720p (1.51 against 2.75) and 1.7 times at 1080p (2.65 against 4.55).
2. The inbox encoder spends more bits over 60 pictures: 4.2 % more at 720p (1607210 bytes against
   1542203) and 7.5 % more at 1080p (3320028 against 3089727). Over 6 pictures ours spends 6.0 % more
   (197185 against 185964).
3. Over 6 pictures our luma is ahead of the inbox encoder, 47.07 dB against 45.85. Over 60 pictures it
   is behind. The gap grows inside a group of pictures, which the `picture 0` and `the rest` columns
   show directly: our intra picture is ahead on chroma by about 3 dB in both cases, and the worst
   picture of each run is 30 or later out of 60.
4. 1920x1080 has 2.25 times the pixels of 1280x720. Our serial time per picture grows by 1.78
   (11.51 ms against 6.46) and our GPU busy time by 1.81 (6.53 ms against 3.61). On tester.12 the
   serial time grew by 1.53, which the fixed per-picture cost explained. That cost is smaller now.

## Against the targets

The three targets are the ones the perf work set before this trial. The local write-up
`scratch/m15/video-encode/PERF-RESULT.md` states them and holds the development-PC column.

| Target | Result on unit A |
|---|---|
| T1: 1080p at 60 pictures per second or more, 720p at 120 or more, at the DPM's normal clock | **Met.** 85.4 and 153.0 serial, from `t1080-60` and `t720-60`. The compare cases give 86.9 and 154.9 |
| T2: GPU busy at 8 ms per 1080p picture or less | **Met.** 6.53 ms, from `t1080-60`. The prediction before the run was 11 to 12 ms, about 40 % over |
| T3: PSNR-Y within 1.0 dB and chroma within 1.5 dB of the inbox encoder at an equal bit rate | **Missed.** Over 60 pictures at the nominal rate the mean gap is Y -1.34, Cb -3.82, Cr -4.94 dB at 720p and Y -2.31, Cb -4.94, Cr -6.00 dB at 1080p |

The T2 prediction named the group shared memory of `cs_me` (about 9.8 KB per 32-thread group) as the
reason it would be missed. The measurement says otherwise. The prediction had priced the serial wall
time of a picture, not the GPU busy time inside it.

The named causes of T3 are the ones the development-PC write-up diagnosed: 16x16 is the only motion
partition, a P picture carries no intra macroblock, and the mode decision reads luma only. This run
measures the gap and does not test those causes.

## Not in this run

- The `--texin` cases. The NV12 pair fails on this release through BD-071, and `mfthost` runs the four
  `--texin` cases in one process, so the BGRA pair cannot be isolated. The BGRA pair passed on unit A
  in train b18, 120 of 120 pictures at 1920x1080.
- Game Bar, Windows Camera and Chromium (E52 stages 1 and 5). They find the transform only through the
  machine-wide registration, which this trial does not write. They need a separate trial with
  `mftreg --register-global` and `--unregister-global` inside the same script.
- A smart-plug sample. The run records no wall power.
- A GPU clock sample. The KMD was in its own DPM mode for the whole run: `requested dpm, reason none`,
  `up 900 target 800 down 650 permille`, `hot step 500 ms`, and an idle state of 500 MHz after 3000 ms
  under 2 permille busy. The idle counters moved from `entries 224 exits 223` before to
  `entries 224 exits 224` after, with 0 refusals.

## Files

| Path | What it holds |
|---|---|
| `20261006T081819Z/summary.txt` | the header, one row for each of the 82 cases with its per-picture split, the epilogue checks and the verdict |
| `20261006T081819Z/<case>.txt`, `<case>.err` | stdout and stderr of `mfthost.exe` for each case (`selftest.err` is empty) |
| `20261006T081819Z/compare-720.txt`, `compare-720-60.txt`, `compare-1080-60.txt` | the three comparison tables against the inbox encoder |
| `20261006T081819Z/mftreg-enum-before.txt` | `mftreg --enum` before the cases: the H.264 encoder MFTs machine wide and for each adapter, with the LUIDs |
| `20261006T081819Z/route-mfthost.exe-<pid>.log` | the 81 route lines of the AppRouter, in 80 files |
| `20261006T081819Z/<case>/mfthost_amdgpu_wddm_dxvk.log` | the engine log of the 10 witness cases (the kit does not use it as a route witness) |
| `run-b19-1.out` | the console of the first start, which the preflight refused |
| `SHA256SUMS-streams.txt` | the 78 streams that are not in the repository (77 `.264`, 1 `.mp4`): SHA-256, bytes, path |
| `SHA256SUMS.txt` | the SHA-256 of each other file in this directory |

The console of the run itself (`run-b19-2.out` in the kit directory) is `summary.txt` plus one line
that names the tar file. This directory does not repeat it.

## Redaction

Before the commit, a script searched every copied file for IPv4 addresses, MAC addresses, GUIDs,
drive paths, user and host names, SSIDs and serial numbers. Nothing needed redaction. All copied files
are unchanged byte copies of the pulled run directory. The search found these matches, and all of them
stay:

- The only IPv4-like string is the release version `0.7.208.100`.
- The only GUIDs are two Media Foundation class identifiers: the inbox `H264 Encoder MFT`
  (`{6CA50344-051A-4DED-9779-A43305165E35}`) and our transform
  (`{A32438F0-0D79-4CA9-A5BF-9F3C80837253}`).
- Every drive path is under `C:\BC250\` or `C:\Program Files\amdgpu-wddm`.
- No MAC address, no host name, no user name and no serial number appears at all.
