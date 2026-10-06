# E52 on unit A: the H.264 encoder MFT, 75 of 75 cases, and the inbox comparison

Date: 2026-10-06. Two runs on unit A: the validation run `20261006T031702Z` (109 s) and the
comparison run `20261006T034145Z` (25 s). Release 0.7.207.100-tester.12 (train b18r1), kernel
driver 0.7.207.1, 40 CU (`CuMode 40 applied 40 confirmed 40`). No game and no other trial ran at
the same time. Each run stayed inside the three-minute bound.

This is the first lab run of the encoder in `driver/umd/mft-h264`. The plan is
[experiments/E52](../../../experiments/E52-m15-11-encoder-mft-lab/README.md). The host baseline on
the development PC (RTX 4090) is
[2026-10-05-E52-encoder-mft-dev-pc](../2026-10-05-E52-encoder-mft-dev-pc/README.md).

## What ran

The binaries come from `bc250-win` main `d5593267`. Its newest `driver/umd/mft-h264` commit is
`a08f50cb`. The kit refuses to start when a hash does not match.

| File | Bytes | SHA-256 |
|---|---|---|
| `mfthost.exe` | 571904 | `ADA0E923F962181581F988879344FA0E4E5ACEF7739C37E5F09C9C5D253DBAC0` |
| `mftreg.exe` | 412672 | `39DCA8B89E692A818A7C4EBCD95145D5FAA2E7D4203653215ACA2B4E44350C84` |
| `amdgpu_wddm_mft_h264.dll` (not pushed) | 455168 | `141969A3F212619AD6AAFDEEE011AA3F00FDF8C4DC154409993CFF19F54683D5` |

The application route:

1. The AppRouter policy was in `allowlist` mode. Its `GpuUmdPath` is
   `C:\BC250\m14\app-route-001\gpu\amdgpu_wddm_d3d11.dll`.
2. The kit added `mfthost.exe` to `Allow` and set `RouteLogDirectory` to the run directory. After
   the cases, the kit wrote back both values. The readback was identical to the values before the run.
3. The router wrote one route line for each process that opened an adapter. The validation run has 74
   lines and the comparison run has 5. Every line says `route=gpu reason=app-allowed fallback=0`,
   with the GPU UMD above as `module=`.

The machine-wide registration (route A of `driver/umd/mft-h264/INSTALL.md`): tester.12 registers our
transform machine wide from `C:\Program Files\amdgpu-wddm\mft\amdgpu_wddm_mft_h264.dll`. That file has
the pinned hash `141969A3`, so it is the same DLL as the kit pin. A case can activate that copy
instead of its `MFTRegisterLocal` copy. The code is the same in both. Two starts at 03:13Z and
03:15:59Z refused in the preflight, because the kit then required no registration. The kit now
accepts the registration only with that path, that hash and no 32-bit copy. The kit fails a run in
which the registration changes. Both runs report `release before and after, not touched by this run`.

The kit is `lab-e52.ps1` in the local workspace, `scratch/m15/video-encode/lab-e52`, outside this
repository. The comparison run used the file with SHA-256
`D0181E3AAC04F3165375A0AFEFA2C5526385AA6E9C7348F169C598B645C37C5A` (67486 bytes). The file changed
at 03:41:34Z, after the validation run and about 10 s before the comparison run started. Nobody
kept the state that ran the validation run, so this directory gives no hash for it. The `RUN.md` of
the kit pins `5C8C6D36`, the reviewed state before the route A change.

## Verdict

Validation run `20261006T031702Z`: `TOTAL ok=75/75 run` and `VERDICT exit=0: every case that ran
passed and the lab is as it was`.

1. `selftest`: 0 failures.
2. 72 encode cases: the gate case `smoke-320-q26`, `t720-1` to `t720-3`, the 22 structural cases and
   the 46 quantiser cases of the sweep. In every case, every picture is bit exact against the inbox
   H.264 decoder of unit A, and the nnz agreement is exact.
3. In all 72 encode cases, the bit rate is equal to the development PC value. PSNR(Y) is equal to
   it at the two decimals that the tool prints.
4. All four `t720` streams have the SHA-256 `25042BA8...`: the three of this run and `t720-1` of the
   comparison run. The `mft-720` stream has `DB8B24E5...`. Both values are equal to the hashes of
   the development PC streams. The development PC kept no sweep streams, so this check covers these
   two streams only.
5. `mft-720`: the asynchronous hardware MFT contract, end to end. The inbox decoder accepted 6 of 6
   access units, and the drain completed. `MFT_ENUM_ADAPTER_LUID` is `00000000:00008959`, the LUID
   that `mftreg --enum` read for `1002:13FE` on this boot. The stream has 173283 bytes. The time
   through the transform interface is 29.31 ms per picture.
6. `sink-720`: the sink writer wrote a 179781 byte `.mp4`, with 6 pictures through our transform.
   The development PC wrote the same size. The SHA-256 is different (`A7EC2208` here, `55064382` on
   the development PC). The kit records the `.mp4` as a note only, because the inbox MP4 sink makes
   the container.
7. 73 of the 74 cases with a device printed `client device on adapter 1002:13FE`. The other case
   is `sink-720`, which does not print that line, and the kit does not require it there. All 74
   cases have their own route line (see above).
8. The lab after the run: Tctl 65 C before and 69.4 C after (overlay source). KMD faults 0 to 0,
   timeouts 0 to 0. The System log has no TDR, no bugcheck and no WHEA event.

Comparison run `20261006T034145Z`: 6 of 6 cases (`selftest`, `smoke-320-q26`, `t720-1` and the three
comparison cases), the same verdict line, Tctl 65 C to 70.2 C, KMD faults and timeouts 0 to 0.

## Comparison with the inbox encoder

`mfthost --compare` encodes one source with our encoder and with the inbox software `H264 Encoder
MFT` of Windows. Both encoders get the same constant bit rate. The tool then decodes both streams.
The settings come from the kit: qp 26, deblocking, CBR 6000000 bit/s at 1280x720 and CBR 12000000
bit/s at 1920x1080. The bit rate column uses the nominal rate of 30 pictures per second. The values
come from `compare-720.txt`, `compare-720-60.txt` and `compare-1080-60.txt` of the comparison run.

| Case | Encoder | ms per picture | Pictures per second | Bytes | bit/s | PSNR Y (dB) | PSNR Cb (dB) | PSNR Cr (dB) |
|---|---|---|---|---|---|---|---|---|
| `compare-720`, 1280x720, 6 pictures | ours | 28.21 | 35.4 | 178882 | 7155280 | 44.89 | 44.03 | 42.36 |
| | inbox | 9.39 | 106.5 | 185964 | 7438560 | 45.85 | 46.72 | 46.53 |
| `compare-720-60`, 1280x720, 60 pictures | ours | 29.38 | 34.0 | 1509404 | 6037616 | 42.64 | 40.82 | 39.31 |
| | inbox | 2.11 | 474.6 | 1607210 | 6428840 | 45.65 | 45.39 | 45.60 |
| `compare-1080-60`, 1920x1080, 60 pictures | ours | 44.89 | 22.3 | 2987545 | 11950180 | 41.47 | 39.01 | 37.80 |
| | inbox | 3.40 | 293.9 | 3320028 | 13280112 | 46.06 | 45.50 | 45.16 |

The decoder returned all pictures in each row: 6, 60 and 60.

What the numbers mean:

1. Our encoder is bit exact and conformant on unit A, but it is slower than the inbox CPU encoder.
   At 1280x720 over 60 pictures, ours takes 29.38 ms per picture and the inbox encoder 2.11 ms. At
   1920x1080 the times are 44.89 ms and 3.40 ms.
2. 1920x1080 has 2.25 times the pixels of 1280x720. Our time per picture is only 1.53 times larger
   (44.89 ms against 29.38 ms). This points at a fixed serial cost in each picture that does not grow
   with the picture size. These runs do not show where that cost is.
3. The inbox encoder gives a higher PSNR in all three planes in all three rows. Its bit rate is also
   higher: by 4.0 % (6 pictures), 6.5 % (720p, 60 pictures) and 11.1 % (1080p, 60 pictures).
4. Six pictures are too few for a speed value. The inbox encoder takes 9.39 ms per picture over 6
   pictures and 2.11 ms over 60. Use the 60-picture rows for speed.
5. The 6-picture row is equal to the development PC row for both encoders. The byte counts (178882
   and 185964) and the PSNR of all three planes are the same.

## Where the time goes in the validation run

Each encode case prints a split for each picture. "GPU busy" is the device's own timestamp of the
dispatches. "Readback" is the wait for the result, inside the GPU stage. "CPU" is the CAVLC and NAL
half on the CPU.

| Case | Settings | Total (ms) | GPU stage (ms) | GPU busy (ms) | Readback (ms) | CPU (ms) | CAVLC (ms) |
|---|---|---|---|---|---|---|---|
| `t720-1` | 1280x720, 6 pictures, qp 26 | 29.92 | 26.46 | 24.08 | 25.44 | 3.46 | 3.31 |
| `t720-2` | the same | 28.25 | 24.58 | 22.18 | 23.56 | 3.66 | 3.51 |
| `t720-3` | the same | 28.17 | 24.51 | 22.11 | 23.52 | 3.65 | 3.50 |
| `sw-1080p-q30` | 1920x1080, 3 pictures, qp 30 | 49.07 | 42.04 | 38.05 | 40.37 | 7.03 | 6.76 |

For reference, the development PC needed 6.00 to 8.66 ms in total for the same 720p case. Of that
time, 4.03 to 4.14 ms were dispatches and 0.91 to 0.93 ms were the CPU half.

## Not in these runs

- The `--texin` cases. BD-071 makes the NV12 pair fail on b18r1, and the BGRA pair runs in the same
  process. The BGRA pair passed on unit A in train b18 (120 of 120 pictures at 1920x1080).
- Game Bar, Windows Camera and Chromium (E52 stages 1 and 5). They need a separate trial.

## Files

| Path | What it holds |
|---|---|
| `20261006T031702Z/summary.txt` | the validation run: the header, one line for each case, the verdict |
| `20261006T031702Z/<case>.txt`, `<case>.err` | stdout and stderr of `mfthost.exe` for each of the 75 cases (`selftest.err` is empty) |
| `20261006T031702Z/route-mfthost.exe-<pid>.log` | the 74 route lines of the AppRouter, one for each process that opened an adapter |
| `20261006T031702Z/mftreg-enum-before.txt` | `mftreg --enum` before the cases: the H.264 encoders machine wide and for each adapter, with the LUIDs |
| `20261006T031702Z/<case>/mfthost_amdgpu_wddm_dxvk.log` | the engine log of the 6 witness cases (the kit does not use it as a route witness) |
| `20261006T034145Z/` | the same set for the comparison run: 6 cases, 5 route lines, 2 engine logs, and the three `compare-*.txt` tables |
| `SHA256SUMS-streams.txt` | the 76 streams that are not in the repository (75 `.264`, 1 `.mp4`): SHA-256, bytes, path |
| `SHA256SUMS.txt` | the SHA-256 of each other file in this directory |

The console output of each run (`run-1006c.out` and `run-1006-compare.out` in the kit directory) is
`summary.txt` plus one line that names the tar file. This directory does not repeat it.

## Redaction

Before the commit, a script searched all copied files for IPv4 addresses, MAC addresses, GUIDs,
drive paths, user and host names, SSIDs and serial numbers. Nothing needed redaction. All files are
unchanged byte copies of the pulled run directories. The search found these matches, and all of
them stay:

- The only IPv4-like string is the release version `0.7.207.100`.
- The only GUIDs are two Media Foundation class IDs: the inbox `H264 Encoder MFT` and our transform.
- Every drive path is under `C:\BC250\` or `C:\Program Files\amdgpu-wddm`.
- The 16-digit hexadecimal values in the `.err` files are the `identity=` values of the
  `hosted paging` lines. They change in each process.
