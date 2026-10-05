# Native system D3D11 at FL12_1 with tiled resources (fl12native002)

The system D3D11 runtime on unit A creates a device at FL12_1 (0xC100) through the native UMD shell and runs
the d3d11fl12 tiled witness with every check exact. Started 2026-09-30T02:52Z, supervisor 14.93 s,
status `passed`: Capture, Install, Cpu, Gpu, Restore and Verify all ran; baseline restored, postflight verified,
process trees closed.

Module set (full SHA-256 in `stage-manifest.json`, manifest 774E421C): shell E748418C (bc250-win 4b273f3e,
WDDM 2.0 DDI), engine r8 8E9B3187 (D-Ogi/dxvk amdgpu-wddm/ddi-engine-fl12 0c187731), ICD r5 D672813F
(policy patch 0006, CPU map 0007, preamble wait 0008), v2 config A9B498ED encoded from the exact-pair caps
record fl12caps001 (FL 0xC100, tiled tier 3, conservative tier 3, ROVs, typed UAV loads, stencil ref, VP/RT index),
client d3d11fl12 0EC07C7A (bc250-win 74ad7d02), router 089C14B3. Baseline KMD 0.7.173.1, desktop UMD 4176D1DF.

Routing: the errors003 file route replaced the registered desktop UMD path with the router for the Cpu and Gpu
phases only; no registry value changed, and Verify found registration, boot, KMD generation/epoch and the DWM
process and modules unchanged. The router selects the native shell only for this exact client path with the
explicit flag and a 60 s enable lease.

- Cpu (routing control): the same client without the flag loaded the CPU desktop UMD copy, never the native shell;
  D3D11CreateDevice at FL12_1..11_0 returned 0x887A0004 (DXGI_ERROR_UNSUPPORTED), client ended normally (exit 1).
  fl12native001 stopped here only because its admission misread this outcome; 002 changed only that admission.
- Gpu: FEATURE_LEVEL 0xC100. OPTIONS1 tiled tier 2 (the runtime reports tier 3 as 2 in OPTIONS1), OPTIONS2 tiled
  tier 3, ROVs, conservative tier 3, typed UAV additional formats, PS stencil ref; OPTIONS3 VP/RT index. Tiled
  sequence exact against CPU expectations: GetResourceTiling (two 128x128 tiles), a draw into the mapped tile
  with the unmapped tile reading zero, UpdateTileMappings, UpdateTiles, CopyTiles to a linear buffer,
  CopyTileMappings alias, ResizeTilePool; device not removed. The shell logged `instance policy sparse=1`
  (value absent, default on), CreateDevice at interface 0x000B0020 (WDDM 2.0), no DDI error, no second-chance
  exception.
- The runtime asked GetCaps type 151 once; WDK 10.0.26100 defines no type 151 (d3d10umddi.h goes from 150 to 152),
  so the shell answers E_NOTIMPL and the device was created regardless.

Files: `result.json`, `Cpu-summary.json`, `Gpu-summary.json`, the client's own output (`*-client.txt`) and
`ddi-log.txt` (debug strings, exit lines and trial-directory module paths; base and exception addresses removed).
Raw runner output stays in scratch/m14/fl12/fl12native002/ops.

Scope: one bounded functional witness of FL12_1 device creation and tiled resources through the system runtime.
Not performance, not Present or composition, not conservative rasterization, ROV or typed-UAV shader execution,
and not a registered-UMD deployment.
