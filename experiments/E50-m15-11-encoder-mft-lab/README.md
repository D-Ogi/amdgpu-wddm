# E50: the H.264 encoder MFT on unit A's GPU

State: planned, not run. Written before the run.

Question: do the eight compute shaders of `driver/umd/mft-h264` run on unit A's GPU through our D3D11
user-mode driver, and does a standard Media Foundation pipeline on unit A record through the transform?
This is the one gate the rest of M15.11 sits behind: every number recorded for this component so far
comes from an NVIDIA GeForce RTX 4090 on the development PC, and none from BC-250 silicon.

Scope: one 3-minute trial per stage, five stages, run in order and stopped at the first stage that
fails. No driver change, no promotion, no registry value left behind. The transform is registered with
`mftreg --register-global` before a stage that needs it and removed with `--unregister-global` after it,
in the same script, so a failure cannot survive the trial. Game Bar's own recording is the only
interactive stage; it is not a game session, so the 3-minute bound applies to it as well.

## Hypotheses

- **H1** The eight `cs_5_0` shaders compile and dispatch on our D3D11 path. They go through
  DXBC to SPIR-V (dxbc-spirv), which this component has never exercised; the 557 KB inter-prediction
  kernel is the largest shader we have put through it.
- **H2** The GPU reconstruction on unit A is bit exact against unit A's own inbox H.264 decoder MFT,
  with the same `nnz` agreement as on the development PC. The arithmetic is integer and normative, so
  a difference is a bug in the shader path, not a rounding difference.
- **H3** 720p encoding is slower on unit A than on the 4090 by roughly the ratio of the two parts, and
  the GPU share grows: 4.05 ms of GPU per 720p picture on the 4090 is the number to beat down to a
  usable frame rate. A result that makes recording useless is still a result and closes M15.11's
  throughput question honestly.
- **H4** With the transform registered, Game Bar stops refusing with "your PC doesn't meet the
  hardware requirements for capture" and records. That refusal, captured first with the transform
  absent, is the before half of the oracle.
- **H5** Whole-board power stays under the 300 W wall during encoding, with headroom, because the
  encoder is a small compute load next to a game.

## Procedure

Preparation, on the development PC: build the component
(`pwsh driver\umd\mft-h264\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget`), record the SHA-256 of
`amdgpu_wddm_mft_h264.dll`, `mfthost.exe` and `mftreg.exe`, and push the three binaries to
`C:\BC250\mft-h264\` with `tools\win\target.py push`. Check the overlay and tell the owner what is
about to happen (`tools\win\bc250mon\mon.py panel`). Sample Tctl before each stage and cool down above
87 C.

**Stage 1 - the before half of the Game Bar oracle (no registration, ~20 s).**
`gamebar-trace.ps1` as `LAB-REQUEST.md` R2 describes it: one ETW session, Win+Alt+R on the lab's
screen, 20 s, stop. Expected: Game Bar refuses, and the trace shows no encoder MFT activation. Keep
the `.etl`; decode it on the development PC, never on the lab.

**Stage 2 - do the shaders run at all (no registration, ~60 s).**
```
C:\BC250\mft-h264\mfthost.exe --selftest
C:\BC250\mft-h264\mfthost.exe --encode --width 320 --height 240 --frames 3 --qp 26 --out C:\BC250\tmp\e50
```
The smallest case in the sweep. `--selftest` needs no GPU and separates a CPU-side failure from a
shader failure. Expected: `3 of 3 pictures bit exact`, `nnz agreement ... exact`. A shader compile
failure here is the whole result of the trial: record the exact HRESULT and which entry point, and
stop.

**Stage 3 - the conformance sweep on unit A (no registration, 3 minutes).**
`tests\sweep.ps1` is 65 cases and will not fit in three minutes on this hardware. Run it in bounded
batches, one batch per trial, in this order: the 320x240 qp row first (it is the cheapest and the
widest), then 640x480 with deblocking, then 1280x720, then 1920x1080 last. Record per case: bit
exactness, `nnz` agreement, PSNR(Y), ms per picture and GPU ms. Stop a batch at its first failure.

**Stage 4 - the transform contract and a real pipeline on unit A (register and remove, 3 minutes).**
```
set BC250_ALLOW_HKLM_MFT=1
C:\BC250\mft-h264\mftreg.exe --register-global
C:\BC250\mft-h264\mftreg.exe --enum
C:\BC250\mft-h264\mfthost.exe --mft --texin --sinkwriter --width 1280 --height 720 --frames 30 --qp 26 --deblock --out C:\BC250\tmp\e50
C:\BC250\mft-h264\mftreg.exe --unregister-global
C:\BC250\mft-h264\mftreg.exe --enum
```
`--enum` before and after is the proof that nothing was left registered. `--texin` matters here and
not on the development PC: the NV12 texture array is the shape the frame server delivers, and on unit
A it goes through our own driver's planar texture support. Pull the `.mp4` and decode it on the
development PC with the inbox decoder and with `ffmpeg`.

**Stage 5 - the after half of the Game Bar oracle (registered, ~60 s).**
Register as in stage 4, then repeat stage 1. Expected: Game Bar records, the trace names our CLSID
`{A32438F0-0D79-4CA9-A5BF-9F3C80837253}`, and a file appears under the signed-in user's
`Videos\Captures`. Unregister in the same script, whatever the outcome. Then Windows Camera and
Chromium, one trial each, same register-and-remove shape.

Sample the smart plug (`scratch\smartplug\plug.py telemetry`) at the start and the end of stages 3,
4 and 5, and record the raw DPS values and the query time next to the stage.

## What this decides

Whether M15.11 can be met at all on this hardware, and at what frame rate and picture size. Stage 2
alone answers the shader-path question; stage 3 turns the development PC's conformance claim into a
unit A claim; stages 4 and 5 are the acceptance oracle the criterion names.

## Evidence

`evidence/windows/<date>-E50-encoder-mft-unit-a/` with one directory per stage, the `RESULT.md` that
`docs/01-evidence-rules.md` asks for, the binary hashes, and the before and after `--enum` output of
every stage that registered anything.

## References

- the component: `driver/umd/mft-h264/README.md`, its registration specification `INSTALL.md`
- the criterion: `docs/m15-reconciliation.md` row M15.11
- the Media Foundation survey of unit A: facts M774, `evidence/windows/2026-10-01-E46-mf-encoder-survey/`
- the absent video engine: facts M46
