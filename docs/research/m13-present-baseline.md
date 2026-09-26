# Working full-WDDM desktop baseline

Current checkpoint: M406-M412,2026-09-24. The owner prioritized visible display
before further M9 acceptance work. [Evidence and limits](../../evidence/windows/2026-09-24-E26-desktop-resume/mesa-main/RESULT.md).

Later steps (KMD 0.7.130.1 after M420, the E34 native D3D measurements M531-M534) are
indexed in workspace `STATE.md` and [facts.md](../facts.md); the M13 route discussion is
in workspace `agent-discussion/ROADMAP.md`. This note keeps the CPU-renderer baseline.

## What changed

The compute profile used the stub D3D UMD and disabled presentation gates.
Mesa softpipe plus the existing hardware flip path makes the desktop visible.
The frontend then copied pixels during RotateResourceIdentities, leaving
kernel identities fixed. It now rotates backing, allocation, GPUVA and CPU
mapping, retargets live RTV/SRV and bound views, and preserves runtime handles.
The owner confirms that the visible descending redraw bands disappeared.

PROVENANCE: Mesa801c9763c6043f0de8408e905a5324eea06d81d7, MIT.
The [incremental patch](../../experiments/E26-wddm-desktop/mesa-resource-rotation.patch)
applies after the E26 branch-labels prototype. Build and extracted positive/
negative tests are preserved with the evidence. Local Microsoft source:
ref/ddi-display/dxgiddi.md, DXGI base functions, RotateResourceIdentities;
exact declarations use WDK26100. Source document hashes are in sources.json.

## Lab configuration

KMD0.7.130.1 supersedes129 after M420 OS DMA aperture placement; the D3D configuration is
unchanged, DWM4448 and the Windows boot were retained through15:30:33. The currently registered D3D10/11 DLL is
C:\BC250\m13\mesa-main-umd\bc250d3d.dll, SHA256
D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D.
This is Mesa 26.3.0-devel llvmpipe with LLVM 23.1.2 CPU JIT. Mesa upstream
commit is f9a2d34a19c496e552b6cd603e7807f1025a2e98. M410/M411 binaries and
earlier softpipe DLLs remain on the lab for rollback.
The first UserModeDriverName slot remains bc250umd.dll; the next two select
this Mesa DLL. A DWM restart alone did not refresh the cached registration;
PnP disable/enable loaded the new path, then DWM was restarted once.

EnablePresentBlit, EnableDcnWrite and EnableVidPnFlip are1. Existing compute
and paging gates stay enabled. EnableMmioWrite and EnableRlcReloadReset stay0.
EnableFullWddm is a one-shot start gate; its consumed on-disk0 does not describe
the running full table. Do not blindly reuse old M9 startup scripts that close
display gates. Preserve the display profile unless an experiment deliberately
changes it, with that change recorded. No persistent auto-start policy changed.

## Responsiveness follow-up

M408 softpipe caused alternating cursor stalls and skipped overlay seconds,
with380-400ms ordinary two-draw frames. M410 switches to llvmpipe CPU JIT,
waits for raster completion before presentation, and adds missing TGSI-to-NIR
translation for Direct3D texture/sampler operands. The initial llvmpipe trial
hit a translator assertion before LLVM; limited stacks isolated it. Eight
actual translator controls pass; aliasing sampler to texture fails six.

The owner confirms fluid mouse motion with the overlay visible. Across56
post-startup two-draw samples, median draw plus render wait is3.9055ms and
maximum5.830ms. Startup JIT still has second-scale costs; this is not a matched
animation benchmark or broad shader conformance. Preserve the measured build.

## Validation and remaining acceptance

Actual scanout and the owner confirm visible desktop and intact overlay.
Two-device shared red/blue and green staging controls report zero pixel
mismatches. A concurrent64KiB GPU residency probe passes all four readbacks.
The M412 final check records DWM4448 from13:58:18 to14:01:27,407 hardware flips
and no TDR. Current Mesa/LLVM passes the same shared/pixel and GPU controls;
104 two-draw samples have median draw plus wait4.591ms, maximum6.043ms.
This is not a matched performance comparison with M410. Owner fluid-cursor
confirmation above belongs to M410; current-build input feedback remains open. Generic GPU draws are not proved: DWM rendering is CPU llvmpipe, while
presentation uses hardware flip and hardware VSync.

M13.1 remains open: the synthetic Start-menu check did not display Start;
distinguish input/focus from shell/renderer failure, test movement and resize,
and complete30minutes of acceptance observation. M13.3-M13.4 still require
actual GPU execution of native Direct3D and DWM draws. Do not call this a fully
accelerated desktop or close M9 based on the display result.

## Renderer visibility and LLVM baseline (2026-09-24)

M409 adds the active graphics pipeline panel to the lab overlay. It identifies
DWM's loaded UMD by SHA256 and a checked build manifest, reports the live KMD
table, and separates CPU drawing, enabled CPU Blt, DCN hardware flips, GPU
compute completion and SDMA paging. Counts are cumulative, sampled every
5 seconds. Unknown module hashes do not inherit a renderer label.

LLVM 19.1.7 was the Windows CI pin used for the first working M410 baseline.
M411 upgraded the same Mesa source to LLVM 23.1.2 without API changes. M412
then moved the desktop UMD to current Mesa main (26.3.0-devel), preserving our
Gallium patches. Both builds pass 8 actual TGSI/NIR controls, 1236 upstream
arithmetic JIT checks, live D3D shared/pixel controls and concurrent GPU reads.
No matched benchmark establishes a speedup from either version change.

## Current build and upstream policy

Prefer current upstream dependencies where practical, per AGENTS.md/CLAUDE.md.
Record exact revisions and validate before promoting the lab baseline.

- LLVM: tag llvmorg-23.1.2, commit85ac560262434c9ccfc0c183ec22d4138ed647fb;
  source ref/llvm-project-23.1.2, install toolchain/llvm2312, Release MT X86.
- Mesa: main commitf9a2d34a19c496e552b6cd603e7807f1025a2e98, dated2026-09-24;
  source scratch/mesa-main-20260924, build scratch/mesa-main-llvm23-build.
- Apply [the consolidated Gallium patch](../../experiments/E26-wddm-desktop/mesa-main-bc250-gallium.patch)
  to that clean Mesa commit. It includes the test sources and targets.
- [Build script](../../experiments/E26-wddm-desktop/build-mesa-main-llvm23.cmd)
  uses LLVM23 explicitly; build/deployment/control logs are in M412 evidence.
  llvm-config resides in scratch/llvm2312-build/bin, not the install bin.

The separate Vulkan RADV/WDDM2 ICD remains on its previous fork. Official main
lacks that winsys. Next dependency work is to port its Windows integration and
BC250 changes to current Mesa, then run shader, residency and model controls
plus a matched performance comparison before replacing the compute ICD.
This concrete port requirement is not a reason to keep the desktop on old Mesa.


## Vulkan migration alongside the retained desktop

M414-M415 port RADV/WDDM2 compute to current Mesa main f333dd6d with ACO.
The [new per-process compute baseline](../../experiments/E27-m9-inference/radv-main/README.md)
passes shader/model and paired benchmark controls while DWM4448 remains alive
from13:58:18 through14:49:35. No OS/device/DWM reset during this migration.
This uptime alone does not close manual desktop acceptance. A Windows Terminal
0xc0000005 during the first launch is a separate unresolved compatibility issue;
headless conhost works for the subsequent compute runs. D3D remains the M412
CPU llvmpipe/LLVM23.1.2 module and existing display gates.
