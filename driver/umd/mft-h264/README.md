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
are eight Direct3D 11 compute shaders; entropy coding (CAVLC) and bitstream assembly are on the CPU,
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
Studio; no WDK or SDK installation is needed. Output goes to `$BC250_ROOT\scratch\build\mft-h264`,
never to drive C:. The shaders are compiled offline with `fxc`, so the shipped DLL has no
`d3dcompiler` dependency and the bytecode is part of the artifact. The generated shader headers under
`shaders/gen` and `tests/gen` are build output and are not committed.

Four gates, each one fatal:

1. every shader entry point compiles with `/WX`;
2. the DLL, the test and the tool compile with `/W4 /WX`;
3. no binary imports a C runtime DLL (the MFT is loaded into Game Bar, Chromium and the frame server,
   which is what made the RADV ICD use `/MT` as well - see `docs/build.md`);
4. `mfthost.exe --selftest` passes, which needs no GPU.

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
| `--mft` | the asynchronous hardware MFT contract end to end: found through `MFTEnumEx` with `MFT_ENUM_FLAG_HARDWARE`, streaming refused before `MF_TRANSFORM_ASYNC_UNLOCK`, `MF_MT_MPEG_SEQUENCE_HEADER` on the output type, `ICodecAPI` round trips including the packed UINT64 quantiser, CABAC refused honestly, the `NeedInput`/`HaveOutput`/`DrainComplete` sequence, a key frame at the start of a new segment after a drain, D3D11 texture input, `DllCanUnloadNow` answering S_FALSE while an object of ours lives, an odd frame size refused during negotiation, the VUI repeating the input type's colour description, and every access unit decoded |
| `--texin` | the four Direct3D 11 input shapes a capture client actually delivers: BGRA and NV12, each as a plain texture and as one slice of a texture array, plus `IMFRealTimeClientEx` |
| `--compare` | our quality, size and speed against the inbox software `H264 Encoder MFT` on the same source at the same settings |
| `--sinkwriter` | an ordinary `MFCreateSinkWriterFromURL` pipeline to `.mp4` picks the transform up and the file plays |

`sweep.ps1` is the conformance sweep: 68 cases from 64x48 to 1920x1080, every qp from 6 to 51 with
deblocking on, visible sizes that are not a whole number of macroblocks, GPU-sourced input, NV12 in
system memory on a wider stride, CBR and still mode. Each case requires bit exactness against the
inbox decoder.

## Measured on the development PC, 2026-10-05

These numbers come from an **NVIDIA GeForce RTX 4090**, not from BC-250 silicon: this machine has no
BC-250 in it, and `gpu_pipeline.cpp` falls back to the first adapter when `1002:13FE` is absent.
Nothing in this component has yet run on unit A's GPU. The lab plan that closes that gap is
[experiments/E50](../../../experiments/E50-m15-11-encoder-mft-lab/README.md).

- conformance sweep: **68 of 68 cases pass**, 0 fail.
- 1280x720, 6 pictures, qp 26, deblocking on: **6 of 6 pictures bit exact**, nnz agreement exact
  (0 disagreements), mean PSNR(Y) 47.21 dB, 6.32 ms per picture. The stages of that picture: the GPU
  stage takes 5.40 ms, the device's own timestamps measure 4.15 ms of dispatches inside it, and
  4.97 ms of it is the readback that waits for them. The CPU half takes 0.92 ms: 0.88 ms of CAVLC
  and 0.04 ms of NAL assembly.
- hardware MFT contract: `MF_TRANSFORM_ASYNC` set, `MF_MT_MPEG_SEQUENCE_HEADER` 36 bytes, 6 fed,
  7 `NeedInput`, 6 `HaveOutput`, 6 outputs, 1 key frame, drain complete, inbox decoder accepted
  6 of 6 access units, 4.70 ms per picture through the transform interface. A second segment after
  `COMMAND_DRAIN` opens with a key frame that carries its own parameter sets, and the sequence header
  on the output type stays what the stream carries when a setting arrives mid-stream.
- the VUI colour description is the client's own, byte for byte, over five input types: NV12 tagged
  BT.601, NV12 tagged BT.709, NV12 full range, NV12 with no description (which gives "unspecified"),
  and ARGB32, where the import shader converts and the answer is BT.709 studio range.
- Direct3D 11 input shapes: **4 of 4 cases pass**, which are BGRA plain, BGRA array slice 2 of 4,
  NV12 plain and NV12 array slice 2 of 4. The NV12 cases decode at PSNR(Y) 42.76 dB against the exact
  bytes written into the slice. That is what proves the encoder read the right slice and both planes.
- against the inbox CPU encoder, 1280x720, CBR 6 Mbit/s, same source: ours 178882 bytes at PSNR
  Y/Cb/Cr 44.89/44.03/42.36 dB, inbox 185964 bytes at 45.85/46.72/46.53 dB. We are 4 % smaller,
  about 1 dB behind on luma and 4 dB behind on chroma. Those figures are deterministic. The time per
  picture is not: this run gives ours 6.49 ms against the inbox 7.05 ms, while earlier retained runs
  of the same test put the inbox between 4.69 ms and 7.07 ms. One run on a shared development PC
  decides nothing about speed.
- sink writer: 6 of 6 repeat runs exit 0, deterministic 179781 bytes, all 6 pictures through our
  transform, and `ffprobe` reads the file as `h264 / Constrained Baseline / 1280x720 / yuv420p /
  6 frames`.

## What is not done yet

- Nothing has run on unit A. On unit A the eight `cs_5_0` shaders go through our D3D11 UMD compute
  path (DXBC to SPIR-V through dxbc-spirv), which this component has never exercised.
- Throughput on unit A is unknown and is the real risk: 4.15 ms of GPU per 720p picture on a 4090
  against 24 CU inside a 300 W board that is also rendering.
- The driver package does not register the transform yet. `INSTALL.md` has the exact keys; the KMD INF
  has no MFT section.
- No input sample allocator (`MF_SA_D3D11_ALLOCATE_SAMPLES`), which the frame server prefers but does
  not require.
- Constrained Baseline only.
