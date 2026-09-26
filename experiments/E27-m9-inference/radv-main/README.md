# Current Mesa RADV on the BC250 WDDM backend

Current compute baseline: M414-M415, 2026-09-24. Mesa26.3.0-devel main
f333dd6d1c85297ac41773eaeb9b02f16acf1919 with the port in
[mesa-main-wddm2-bc250.patch](mesa-main-wddm2-bc250.patch).
[Measured controls, failed trials and performance](../../../evidence/windows/2026-09-24-E27-m9-recovery/radv-main/RESULT.md).
PROVENANCE: Mesa upstream and Collabora lfrb/mesa, MIT.

The public [Collabora WDDM2 branch](https://gitlab.freedesktop.org/lfrb/mesa/-/tree/wddm2)
remained at801c9763c6043f0de8408e905a5324eea06d81d7 when inspected.
This consolidated patch ports its44commits sinceb860e013, retains the BC250
caps/private-allocation/cache-intent/queue integration, and adapts it to current
main. SOURCE.json records exact inputs and patch/artifact hashes.

## Reproduce

Use a separate checkout at the exact main commit. Apply the consolidated patch
with git apply. It includes all required tracked source changes, including
shared Windows Vulkan runtime refactoring; do not apply the old fork series
again. The reverse-apply check against the built source passed.

The workspace [build.cmd](build.cmd) uses portable tools under `<BC250_ROOT>` (the workspace root,
by default the parent directory of this repository) and
installed MSVC19.44.35221: Meson1.12.0, WDK26100, debugoptimized/MD,
Vulkan amd only, no Gallium. Source scratch/mesa-radv-main-20260924, build
scratch/mesa-radv-main-build. Match or adjust the paths on another development
host. ACO compiles GPU shaders; LLVM is disabled for this RADV build. D3D
llvmpipe uses LLVM23.1.2 independently. Full build options are in the evidence.

## Use on the lab

Accepted DLL SHA256:
DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986.
The DLL and JSON are installed under C:\BC250\m9\radv-main-icd2.
Use C:\BC250\m9\radv-main\run.cmd followed by the program and its arguments.
The versioned [launcher](run.cmd) sets the Vulkan manifest and loader PATH for
that process. For example:

```bat
C:\BC250\m9\radv-main\run.cmd C:\BC250\m9\llama\llama-bench.exe --list-devices
```

For automated interactive tasks use an explicit headless conhost worker with
native completion markers, as in the evidence scripts. Windows Terminal
crashed during the first attempt. The launcher does not alter system-wide ICD
registration or the working D3D desktop. Old cache-intent-v2 remains available
for comparisons and rollback. Historical experiment scripts deliberately retain
their original ICD paths; use the current path for new work.

The M8 Vulkan integer driverVersion is26.2.99 for this26.3.0-devel build:
upstream vk_get_driver_version maps a development .0 version to the previous
minor with patch99. The exact source commit and DLL hash identify this build.

## Acceptance and limits

All8shader CPU hashes and both deterministic model references pass with native0.
One paired r3 benchmark reports TinyLlama1115.94/114.56 versus1078.17/111.38
prompt/generation tokens/s, and stories15M31766.21/471.30 versus30004.56/441.11.
Same KMD127,1000MHz/820mV, M412 desktop and launch method; cache disabled for
both benchmark sides. One ordered pair is not a universal speedup guarantee.

Sparse support is gated off on hardware requiring the NULL-PRT SMEM workaround
when the backend supplies no PRT alias control bit. Implementing the mirrored
VA policy is separate work. Assertions stay enabled. These controls establish
ordinary-buffer compute, not Vulkan conformance, full sparse/WSI support or
hardware D3D rendering. Linux and DZN were not built in this experiment.
All M9 DMA/cache/PFN/lifetime requirements remain tracked in the acceptance index.
