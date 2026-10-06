# driver/umd/mft-h264

`amdgpu_wddm_mft_h264.dll`, the H.264 encoder Media Foundation transform of the driver package. It
registers as a **hardware** encoder MFT, so a standard Windows application that asks Media Foundation
for a hardware H.264 encoder - Game Bar, Windows Camera, Chromium - finds it and records through it.
This is requirement M15.11 in [docs/m15-reconciliation.md](../../../docs/m15-reconciliation.md).

The part the hardware cannot do is done in software, and the part the hardware can do runs on the GPU.
Cyan Skillfish has no usable video engine: amdgpu answers no UVD, VCE or VCN query on this part and
reports VCN firmware `0x00000000` (facts M46), and Media Foundation on unit A offers no hardware
encoder at all today (facts M774). So the encoder is **compute**: motion estimation, intra and inter
prediction, the forward and inverse transform, quantisation, reconstruction and the deblocking filter
are eight Direct3D 11 compute shaders. Entropy coding (CAVLC) and bitstream assembly are on the CPU,
which the owner accepted on 2026-10-01.

Output: H.264 Constrained Baseline, one slice per picture, I and P pictures, CAVLC. No CABAC, no
B pictures, no HEVC.

## Layout

| Path | Contents |
|---|---|
| `src/mft_h264.*` | the transform object: `IMFTransform`, the asynchronous event model, `ICodecAPI`, `IMFShutdown`, `IMFRealTimeClientEx`, `IMFAttributes`, the D3D11 device manager and the input sample paths |
| `src/mft_register.*` | one source of truth for the registration: name, flags, media types and the attribute blob, used by the live object, by the local registration of the test and by `tools/mftreg` |
| `src/encoder.*` | frame level: GOP structure, rate control, the join between the GPU half and the CPU half |
| `src/gpu_pipeline.*` | the Direct3D 11 host side: buffers, views, dispatches, the reference ping-pong, timestamp queries |
| `src/h264_*`, `src/bitwriter.h`, `src/mb_layout.h` | the CPU half: the clause 9.2 variable length code tables, slice and macroblock syntax, the bit writer with start code emulation prevention |
| `shaders/` | `cs_import` (NV12, planar, BGRA, and a system memory variant), `cs_me`, `cs_mb` (intra and inter entry points), `cs_deblock`, and `h264_common.hlsli` |
| `tests/` | `mfthost.exe`, the host test, and `sweep.ps1`, the conformance sweep |
| `tools/mftreg/` | prints the registration bytes for the INF, and performs a machine-wide registration behind two guards |
| `INSTALL.md` | the exact files and registry keys the installer has to write |
| `PROVENANCE.md` | where the code and the tables come from |

## Build

```
pwsh driver\umd\mft-h264\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
```

Headers and import libraries come from the SDK NuGet packages, the compiler from the installed Visual
Studio. No WDK or SDK installation is needed. Output goes to `$BC250_ROOT\scratch\build\mft-h264`,
never to drive C:. The shaders are compiled offline with `fxc`, so the shipped DLL has no
`d3dcompiler` dependency and the bytecode is part of the artifact. The generated shader headers under
`shaders/gen` and `tests/gen` are build output and are not committed.

Four gates, each one fatal:

1. every shader entry point compiles with `/WX`.
2. the DLL, the test and the tool compile with `/W4 /WX`.
3. no binary imports a C runtime DLL (the MFT is loaded into Game Bar, Chromium and the frame server,
   which is what made the RADV ICD use `/MT` as well - see `docs/build.md`).
4. `mfthost.exe --selftest` passes, which needs no GPU.

The build is reproducible, and the recorded artifact names the toolchain that made it. `/Brepro` is on
every `cl` and `link` call, which removes the timestamp from the object and from the image. The script
prints the compiler version, the SDK version and the SHA-256 of all three binaries, so one log line
carries what another machine needs to compare. `KitVersion` defaults to `10.0.26100.0`. The compiler
is whichever MSVC toolset the installed Visual Studio holds, which the script picks by name, so a
different toolset gives different bytes and the log says which one ran.

## Tests

```
pwsh driver\umd\mft-h264\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
$env:BC250_ROOT\scratch\build\mft-h264\mfthost.exe --all --width 1280 --height 720 --frames 6 --qp 26 --deblock --out <dir>
pwsh driver\umd\mft-h264\tests\sweep.ps1
```

`--all` runs six stages. The oracle in every one of them is the **inbox Windows H.264 decoder MFT**:
what it reconstructs is by definition what a conformant decoder reconstructs.

| Stage | What it establishes |
|---|---|
| `--selftest` | the H.264 tables are structurally sound: prefix freeness and Kraft sums of every `coeff_token`, `total_zeros` and `run_before` table, zig-zag scans are permutations, tables 8-14, 8-15 and 9-4 round trip, the bit writer round trips 45150 bits exactly, no start code emulation, SPS and PPS build, the declared level follows the frame size and the bitrate, the colour description reaches the SPS, and an odd frame size is refused rather than rounded down |
| `--encode` | the GPU reconstruction is sample-for-sample identical to the inbox decoder's output, and the non-zero coefficient counts the shaders report agree with the CPU's own count of the levels (a wrong `nC` context makes a different but still parseable bitstream, which a decoder cannot see) |
| `--mft` | the asynchronous hardware MFT contract end to end: found through `MFTEnumEx` with `MFT_ENUM_FLAG_HARDWARE`, streaming refused before `MF_TRANSFORM_ASYNC_UNLOCK`, `MF_MT_MPEG_SEQUENCE_HEADER` on the output type, `ICodecAPI` round trips including the packed UINT64 quantiser, CABAC refused honestly, the `NeedInput`/`HaveOutput`/`DrainComplete` sequence, a key frame at the start of a new segment after a drain, D3D11 texture input, `DllCanUnloadNow` answering S_FALSE while an object of ours lives, an odd frame size refused during negotiation, the VUI repeating the input type's colour description, `MFT_ENUM_ADAPTER_LUID` naming the client's own adapter as the documented 8 byte blob, and every access unit decoded |
| `--texin` | the four Direct3D 11 input shapes a capture client actually delivers: BGRA and NV12, each as a plain texture and as one slice of a texture array, plus `IMFRealTimeClientEx` |
| `--compare` | our quality, size and speed against the inbox software `H264 Encoder MFT` on the same source at the same settings |
| `--sinkwriter` | an ordinary `MFCreateSinkWriterFromURL` pipeline to `.mp4` picks the transform up and the file plays |

### Where the GPU time goes

`BC250_MFT_STAGE_TIMING` in the environment turns on per-stage GPU timing, off by default and read
once in `GpuEncoder::Initialize`. `1` places one timestamp query per stage of the picture (import,
motion, mode, deblock); `2` places one after every dispatch, which `--encode` then prints dispatch by
dispatch with its thread group count. Every mark is one more query inside the command stream, so
level 2 reports a total above the same picture's uninstrumented cost: it says where the time goes, it
does not quote a throughput. `--encode` also reports, always, how long the thread spent recording the
picture's commands and how long it then waited in the one blocking `Map`.

`sweep.ps1` is the conformance sweep: 68 cases from 64x48 to 1920x1080, every qp from 6 to 51 with
deblocking on, visible sizes that are not a whole number of macroblocks, GPU-sourced input, NV12 in
system memory on a wider stride, CBR and still mode. Each case requires bit exactness against the
inbox decoder.

## Measured on the development PC, 2026-10-05

These numbers come from an **NVIDIA GeForce RTX 4090**, not from BC-250 silicon: this machine has no
BC-250 in it, and the host test hands the transform a device of that adapter (`CreateTestDevice` in
`tests/mfthost.h`). The transform itself takes the BC-250 adapter alone: `gpu_pipeline.cpp` answers
`DXGI_ERROR_NOT_FOUND` when `1002:13FE` is absent, so a client that asked Media Foundation for a
hardware encoder goes back to the encoder of Windows. An earlier version took the first adapter
instead, which gave such a client a software encode on this transform.
Nothing in this component has yet run on unit A's GPU. The lab plan that closes that gap is
[experiments/E52](../../../experiments/E52-m15-11-encoder-mft-lab/README.md).

Every figure below comes from one retained run, and the logs of that run are in
[evidence/windows/2026-10-05-E52-encoder-mft-dev-pc](../../../evidence/windows/2026-10-05-E52-encoder-mft-dev-pc/README.md).
The binaries that produced them: `amdgpu_wddm_mft_h264.dll` 454656 bytes, SHA-256
`DA890BBB819EE2B09EFD2F2ADCE6B91CFB47B269BAD623D8327C83A398E216A2`, `mfthost.exe` 570368 bytes,
`93B2A97CE41D5ED90F9B8B5A44248E1CD1EC19FBD90B81AC9C22AF46BF350725`, `mftreg.exe` 412672 bytes,
`4C55F72BF52B99AEED2B6DC26C2C103AA532C329A98CAF5D8EBAD5F49B58610F`, built with MSVC 14.44.35207 and
SDK 10.0.26100.0. Two clean builds into two empty directories gave those three hashes.

- conformance sweep: **68 of 68 cases pass**, 0 fail (`sweep.txt`).
- 1280x720, 6 pictures, qp 26, deblocking on: **6 of 6 pictures bit exact**, nnz agreement exact
  (0 disagreements), 215930 bytes, mean PSNR(Y) 47.21 dB. The same bytes in all three repeats.
- the cost of one of those pictures, in the three repeats (`all.txt`, `all-2.txt`, `all-3.txt`):
  8.66, 6.09 and 6.00 ms in total. The device's own timestamps measure 4.14, 4.03 and 4.05 ms of
  dispatches. The GPU stage around them takes 7.73, 5.18 and 5.09 ms, of which the readback waits
  5.11, 4.80 and 4.78 ms. The CPU half takes 0.93, 0.92 and 0.91 ms, of which CAVLC takes 0.89, 0.88
  and 0.87 ms. The first repeat ran while other work loaded the machine. Treat the dispatch time and
  the CPU time as measurements, and the total as an upper bound of a shared PC.
- hardware MFT contract: `MF_TRANSFORM_ASYNC` set, `MF_MT_MPEG_SEQUENCE_HEADER` 36 bytes, 6 fed,
  7 `NeedInput`, 6 `HaveOutput`, 6 outputs, 1 key frame, drain complete, inbox decoder accepted
  6 of 6 access units, 173283 bytes, and 4.66, 4.74 and 4.83 ms per picture through the transform
  interface. A second segment after `COMMAND_DRAIN` opens with a key frame that carries its own
  parameter sets, and the sequence header on the output type stays what the stream carries when a
  setting arrives mid-stream.
- the VUI colour description is the client's own, byte for byte, over five input types: NV12 tagged
  BT.601, NV12 tagged BT.709, NV12 full range, NV12 with no description (which gives "unspecified"),
  and ARGB32, where the import shader converts and the answer is BT.709 studio range.
- `MFT_ENUM_ADAPTER_LUID` on the live transform is an 8 byte blob holding the LUID of the adapter of
  the device the client handed over, `00000000:000243c1` in this run. `mftreg --enum` reports the same
  LUID for that adapter.
- Direct3D 11 input shapes: **4 of 4 cases pass**, which are BGRA plain, BGRA array slice 2 of 4,
  NV12 plain and NV12 array slice 2 of 4. The NV12 cases decode at PSNR(Y) 42.76 dB against the exact
  bytes written into the slice. That is what proves the encoder read the right slice and both planes.
- against the inbox CPU encoder, 1280x720, CBR 6 Mbit/s, same source: ours 178882 bytes at PSNR
  Y/Cb/Cr 44.89/44.03/42.36 dB, inbox 185964 bytes at 45.85/46.72/46.53 dB. We are 4 % smaller,
  about 1 dB behind on luma and 4 dB behind on chroma. Those figures repeat exactly. The time per
  picture does not: the three repeats give ours 6.03, 6.14 and 5.86 ms against the inbox 6.35, 6.67
  and 6.97 ms, while two retained runs of 2026-10-05 before this revision put the inbox at 4.69 ms
  and at 7.05 ms for the same work. Repeats on a shared development PC decide no speed ratio.
- sink writer: 6 of 6 repeat runs exit 0, each writing the same 179781 bytes, all 6 pictures through
  our transform, and `ffprobe` reads the file as `h264 / Constrained Baseline / 1280x720 / yuv420p /
  6 frames`.

## What is not done yet

- Nothing has run on unit A. On unit A the eight `cs_5_0` shaders go through our D3D11 UMD compute
  path (DXBC to SPIR-V through dxbc-spirv), which this component has never exercised.
- Throughput on unit A is unknown and is the real risk: 4.03 to 4.14 ms of GPU per 720p picture on a
  4090, against the 24 or 40 compute units of the BC-250 (the CU mode of `docs/design/cu-mode.md`)
  inside a 300 W board that is also rendering.
- The release installer registers the transform from release 0.7.207.100-tester.12 (route A of
  `INSTALL.md`, written by `tools/release/installer/mft-h264.ps1`). The KMD INF still has no MFT
  section, so a driver package installed by `pnputil` alone registers nothing.
- Whether a per-adapter registration shape exists for a display driver package is not settled.
  `INSTALL.md` records what the enumeration does on the development PC with a machine-wide
  registration, and withdraws the earlier `MFT0` claim.
- No input sample allocator (`MF_SA_D3D11_ALLOCATE_SAMPLES`), which the frame server prefers but does
  not require.
- Constrained Baseline only.
