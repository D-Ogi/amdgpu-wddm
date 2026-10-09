# Defaults audit: tester.11 (installer 73f6d175)

Date: 2026-10-04. Question: what does the installed product (tester.11 defaults) NOT do on the GPU, or not enable
for ordinary apps and games, although the owner would reasonably assume it does?

Legend: **V** = verified in source or by a lab read. **I** = inferred, not measured.
"wt" = `<BC250_ROOT>\scratch\release\wt` (release/installer-t11, 73f6d175).
Lab probes (read-only, two ssh calls): `probe1.out` (registry, files, MFT count) and
`probe2.out` (filtered `dxdiag /t`). The scripts `probe1.ps1` and `probe2.ps1` are beside them.

Status note (team lead, 2026-10-04): the lab and the unpublished tester.11 zip 9CC61A71 already carry AppRouter
`gpu-default`, with a router rule that keeps Windows components on the CPU UMD (router F65C0E04, branch
fix/approuter-gpu-default 06a0975b, BD-061). The package payload behind that zip is still the app-route-001 D3D11
stack, so row 2 is urgent. The lab itself points GpuUmdPath at app-route-quiet since 2026-10-04T01:10Z (`STATE.md`),
which a tester does not get.

Status note (2026-10-07, row 4): measured on unit A with `tools/win/d3d9probe`. With `bc250umd.dll` in the D3D9 slot,
`CreateDeviceEx` fails with D3DERR_NOTAVAILABLE and Windows does not fall back to D3D9On12. With the slot empty, D3D9
renders through `d3d9on12.dll` on our D3D12 driver. Branch `installer/d3d9on12` writes the empty slot in both views
and ships no stub. 32-bit D3D9 still gets no device, because there is no 32-bit D3D12 UMD (row 3).
Branch `umd/wow64-d3d12` adds that UMD (`wow64\d3d12`, the fourth `UserModeDriverNameWow` entry); not yet measured on
unit A.

Status note (2026-10-07, row 18): the premise of the row is wrong for Windows 11. Measured on the development PC
(build 26200, x64 and x86): the Direct3D 10.0 runtime asks a driver that exports both entries for `OpenAdapter10_2`,
not `OpenAdapter10`, and creates its device at a D3D11-family interface, like the 10.1 and 11 runtimes
(`evidence/windows/2026-10-07-BD081-d3d10-entry-devpc`). Our router exports both entries, so a D3D10.0 application
takes the AppRouter decision at `OpenAdapter10_2` and gets the application GPU UMD under `gpu-default`, x64 and x86.
The router's `OpenAdapter10` path is reached by no measured runtime; it stays on the CPU UMD. No router or default
changes. The check on unit A is one run of `tools/win/d3d10probe --trace-entry`: the route line must read
`entry=OpenAdapter10_2 route=gpu` and the probe PASS.

Status note (2026-10-08, row 20): branch `m15/shader-model-68` raises the ceiling. The shell lists every release
model up to the engine's answer, at most 6.8, and answers DDI type 1091 (`engine-ddi/INTEGRATION.md` "Shader
model"). The runtime decides what an application sees (fact M840). Unit A's System32 runtime 10.0.22621 does not
know the 6.7 and 6.8 release values, so an application without an Agility SDK core still sees 6.6. A game with
an Agility SDK core of 1.615 or later, such as The Witcher 3 5.0 (1.619.4), sees up to 6.8. The experiments
`shader-model-68-off` and `shader-model-67-off` set the ceiling back to 6.7 and 6.6. Not yet measured on unit A:
`tools/win/d3d12caps` `d3d12sm68` is the check.

## Gaps, sorted by severity

| # | Gap | What a user gets today | Evidence | Sev | What exists already | Smallest fix |
|---|---|---|---|---|---|---|
| 1 | D3D11 router shipped as `allowlist` with only `dxdiag.exe` (73f6d175) | Every other D3D11/D3D10.1 process goes to the CPU UMD. That UMD negotiates only the D3D10.0 DDI interfaces (V: `SUPPORT_D3D10_1 0`, `SUPPORT_D3D11 0`), so the best it offers is FL 10_0 (V: Task Manager on the lab read "DirectX version: 12 (FL 10.0)"). D3D11 games that require FL 11_0 then probably fail to create a device rather than run slowly (I); `INSTALL.md` says "slow". D3D12 games that probe D3D11 first (RotTR) needed to be allowlisted even in DX12 mode (V). The control app has no AppRouter page (V). Status: fixed in the unpublished 9CC61A71 candidate (gpu-default + router F65C0E04), but see row 2 | wt `tools/release/installer/registry-defaults.json` (`app_router`); wt `driver/umd/router/router-policy.h` `DecideApp` -> `NotAllowed`; `<BC250_ROOT>\scratch\bd058\cpu-src\src\gallium\frontends\d3d10umd\State.h:39-40`, `Adapter.cpp:119-130`; `STATE.md` native-caps227 block ("FL 10.0"), K112 note in the M14.1 block; no AppRouter code in `<BC250_ROOT>\scratch\release\control-app\wt\tools\win\amdgpu_wddm_control\src\*.cs`; wt `docs/testing/INSTALL.md` "What works" | HIGH | `Mode gpu-default` (router-policy.h `ParseAppMode`); router F65C0E04 (fix/approuter-gpu-default 06a0975b) | Publish gpu-default only together with row 2. Correct the `INSTALL.md` text. Add AppRouter (mode, Allow, Deny) to the control app |
| 2 | The D3D11 GPU stack in the package is older than the lab-validated one | The package carries app-route-001: shell E748418C, engine 8E9B3187, ICD D672813F ("ICD r5"), config A9B498ED (V). RotTR's D3D11 benchmark bugchecked 0x116 at its first frame on ICD r5 (K109: SQC instruction fetch at an unmapped 32-bit-heap VA) (V). It also lacks the deferred destroy v3, the K118 prefetch padding and the shader disk cache (V: no `MESA_SHADER_CACHE_*` strings in D672813F). With gpu-default, every D3D11 game on a tester PC runs this ICD | wt `tools/release/release-sources.json` (d3d11-umd rows); `<BC250_ROOT>\scratch\m15\offgpu\KNOWLEDGE.md` K109, K110; `STATE.md` M14.1 block (app-route-throttle, app-route-icdmt, app-route-quiet) | HIGH | Two newer roots, details below the table (app-route-quiet, the lab default since 01:10Z; app-route-dp5, which also uses the D3D12 ICD) | Replace the four `payload/d3d11/*` rows in `release-sources.json` with app-route-quiet (or app-route-dp5) and rebuild. The config record must be the one of the same root: it pins the engine and ICD hashes, and a mismatch fails D3D11CreateDevice with 0x80070017 (`<BC250_ROOT>\scratch\m15\dp5-freeze\make-route-stage.py:23`). Then one D3D11 game session before the zip is published |
| 3 | No 32-bit (WoW64) user-mode driver for any API | 32-bit D3D9/10/11/12, Vulkan and OpenGL processes find no driver of ours (V: registry and files). What they then do (WARP, an error) is not measured (I) | Lab probe1: `UserModeDriverNameWow`, `VulkanDriverNameWow`, `HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers` absent; nothing of ours in SysWOW64; wt `driver/kmd/bc250kmd.inf:64-72`; `INSTALL.md` ("32-bit applications do not have a driver yet") | HIGH | Work in progress (`wow64-d3d`); the INF comment describes the Wow line | x86 builds of the router, CPU UMD, GPU shells and RADV, then the Wow values in the class key and the WOW6432Node Khronos key |
| 4 | D3D9 has no driver (64-bit) | The D3D9 slot of `UserModeDriverName` is `bc250umd.dll`, a 4 KB stub whose `OpenAdapter` returns E_NOTIMPL (V). D3D9 games get no hardware device (I); whether Windows falls back to D3D9On12 for this adapter is unmeasured (I) | Lab probe1 `UserModeDriverName` entry 0; wt `driver/umd-stub/bc250umd.c`; the project roadmap decision "D3D9 remains per-application; its separate native DDI is deferred" (recorded in the workspace, outside this repository) | HIGH | Per-app DXVK d3d9 rendered on the lab (M499, 660 frames); the DXVK fork contains d3d9 | Short term: measure D3D9On12 on the lab (one 3-minute trial) and document per-app DXVK d3d9. Long term: a D3D9 DDI shell |
| 5 | D3D12 ray tracing reported only to `witcher3.exe` | Every other D3D12 app sees RaytracingTier 0 (V), so other RT games (The Ascent RT, 3DMark RT tests) offer no RT | wt `driver/umd/d3d12/adapter-caps.cpp:131`, `engine-ddi/caps.cpp:180-184`; `docs/m15-reconciliation.md` M15.5 ("without it the registered shell reports tier 0", check 309, M780) | HIGH | The `raytracing-tier` experiment and the profile mechanism; indirect DispatchRays exact (M782) | Report tier 1.1 by default once a DXR subset passes (AddToStateObject and existing collections still refuse), or ship profiles for known RT titles |
| 6 | The system Vulkan ICD is old and presents through the CPU | Vulkan games (and per-app DXVK or vkd3d-proton) get ICD CF3948D6 from 2026-09-27, branch radv-wddm2-gdi-immediate (V). It presents through the GDI CPU path (V: M12 not met; about 17 ms per game frame, M610). No shader disk cache (V: strings). It predates the deferred-destroy v3, K118 and inner-coverage fixes (I, from build date). Sparse probably needs `RADV_EXPERIMENTAL=sparse`, because the sparse policy is chained only by the D3D12 shell; per-app vkd3d-proton would then get no FL12 (I) | wt `tools/release/release-sources.json` (vulkan-icd row); `STATE.md` "Registered ICD CF3948D6 (M665)"; `docs/m15-reconciliation.md` "Vulkan WSI presentation (M12)"; wt `driver/umd/d3d12/README.md` (instance policy) | HIGH | The D3D12 ICD F9DCB33B (same RADV, newer, disk cache); design `docs/design/wsi-engine-present.md` | Ship a current ICD build as the system ICD now (fixes and cache). GPU Present for Vulkan is the M12 work |
| 7 | No OpenGL driver | `OpenGLDriverName` absent (V). Windows then uses its software OpenGL 1.1 (I), so GL 3+/4 apps (Minecraft Java, emulators) fail (I) | Lab probe1 | MED | The Mesa fork (WGL + zink is buildable); Microsoft's OpenCL/OpenGL Compatibility Pack (GLon12 over our D3D12), untested | Test the Compatibility Pack on the lab (one trial) and document it; later register an OpenGL ICD |
| 8 | Unprofiled D3D12 games run a configuration never measured in a game | Without a profile: `present-noprimary` and `present-cached` off (surfaces with the primary flag, write-combined), `recording-bind`, `retire-handoff` and `deferred-replay` off (V). Every accepted game measurement used the witcher3.exe profile (246/249/250, 305-307) (V). Speed and correctness of the default path under GPU DWM are unmeasured (I) | wt `driver/umd/d3d12/heap-import.cpp:357`, `device-engine.cpp:320-331, 429`, `ddi-trace.h:39-56`; `STATE.md` "Application profile since 2026-10-01T14:20Z" and the native-caps308 block | MED | The five experiment switches; the profile mechanism | Decide each switch's default (make the measured-safe ones the driver's behaviour), then one session with an unprofiled D3D12 game |
| 9 | 24 CUs by default | Testers get 24 CUs (V). The lab ran 40 CUs for the game measurements up to 2026-10-03 (V) and reads `CuModeLastApplied 24` since tester.11 (V). The control app has no CU setting (V) | wt `tools/release/installer/install.ps1:22`, `driver/kmd/cumode.c:10`; `docs/m15-reconciliation.md` "DPM and CU count"; lab probe1 | MED | KMD CuMode 40 with its fallback guard; installer option `-CuMode 40` | Document `-CuMode 40` in `INSTALL.md` or offer it in the control app; the owner decides the default |
| 10 | One output, one mode, no EDID | Only the mode the firmware set at boot (lab: 1920x1200 at 59 Hz, V). No resolution or refresh change, no second monitor (V). A high-refresh monitor probably stays at the firmware's rate (I) | wt `driver/kmd/pnp.c:135-136, 338`, `display.c:600-640, 942`; lab probe1 (`WmiMonitorID` count 0, one screen) | MED | - | EDID over DCN AUX plus a real mode list (large) |
| 11 | No video decode or encode on the GPU | 0 hardware video encoder, decoder and processor MFTs (V, lab). No D3D11 or D3D12 video DDI (V). Playback runs on the CPU (I); Game Bar recording probably unavailable (I). M15.11 not met | Lab probe1; `docs/m15-reconciliation.md` M15.11, M46 (no VCN) | MED | M15.11 plan (compute-based encoder MFT); `mft-encode` work | Follow M15.11. Nothing small exists |
| 12 | Shader disk cache only on the D3D12 path | The D3D11 GPU ICD D672813F and the system Vulkan ICD CF3948D6 have no Mesa disk cache, so shaders compile again at every start (V: strings; stutter and load-time effect I) | String search: `MESA_SHADER_CACHE_*` present only in F9DCB33B, absent in D672813F, CF3948D6, 66FE8F31 and zink 396A61F9 | MED | Branch `amdgpu-wddm/disk-cache-win` (in D73D827D and F9DCB33B) | Covered by rows 2 and 6 |
| 13 | TDR delay differs from the lab | The lab runs `TdrDelay 10` since 2026-09-28 (V). The installer leaves the Windows default of 2 s (V). With no GPU reset (M15.12), a GPU job over 2 s on a tester PC ends in 0x116 where the lab would have waited (I). Status: closed on branch gui/tdr-default. The installer writes TdrDelay 10. It uses the new defaults table group graphics_drivers. The uninstaller writes the value from before the installation again. The Help page of amdgpu-wddm Control has the waiting time, where the user can change it | `STATE.md` (trial 118/119 block: "Lab: TdrDelay=10 since 23:30Z"); lab probe1; no `TdrDelay` anywhere in wt `tools/release` | MED | `<BC250_ROOT>\scratch\m15\game-recon\set-tdr-delay.ps1` | Installer writes `TdrDelay 10` (uninstall removes it), or the owner decides against it |
| 14 | A process that uses both D3D11 and D3D12 loads two different RADV builds under gpu-default | `d3d11\amdgpu_wddm_radv.dll` (D672813F) and `d3d12\amdgpu_wddm_radv.dll` (F9DCB33B) in one process (V, hashes). That is why `witcher3.exe` is denied (V). RotTR DX12 with ROTTR.exe allowed ran once without a problem (K112) | `<BC250_ROOT>\scratch\m15\app-route\approute.py:69-72`; wt `tools/release/release-sources.json` | MED | app-route-dp5 uses ICD F9DCB33B, the same file as the D3D12 path (below the table) | Ship app-route-dp5 in row 2, or one ICD build for both shells |
| 15 | Standard (non-admin) users lose the driver | The start-confirm task runs only for administrators: a standard user falls back to Basic Display after two restarts, with DPM off (V, documented) | wt `install.ps1:744` (GroupId S-1-5-32-544); `INSTALL.md` "The small blue window after logon" | LOW | - | Run the task as SYSTEM at the logon of any user |
| 16 | No HDR scan-out, no VRR | 10-bit and FP16 swap chains are composed, but output is 8-bit; HDR10 is not offered (`CheckColorSpaceSupport` flags 0) (V). No VRR (I, from WDDMVersion 2.0) | wt `driver/kmd/display.c:637-639`, `wddm.c:2700`; `docs/m15-reconciliation.md` M15.4 | LOW | M15.4 architecture (FP16 and 10-bit rows in `driver/contract/amdgpu_wddm_surface_format.h`) | Later WDDMVersion step plus EDID plus DCN colour path |
| 17 | No hardware-accelerated GPU scheduling | `Hardware Scheduling: DriverSupportState:AlwaysOff Enabled:False`, `Driver Model: WDDM 2.0`, `Block List: DISABLE_HWSCH` (V) | probe2 (dxdiag); wt `driver/kmd/wddm.c:2700` | LOW | - | ADR 0019 WDDMVersion move |
| 18 | D3D10.0 apps always on the CPU | `OpenAdapter10` calls go to the CPU UMD even under gpu-default (V). Status 2026-10-07: no Windows 11 runtime calls `OpenAdapter10`; D3D10.0 applications arrive at `OpenAdapter10_2` and take the GPU UMD (measured on the development PC, see the status note above; unit A check open) | wt `driver/umd/router/router-policy.h` `D3d10Entry`; `router.cpp` header comment; `evidence/windows/2026-10-07-BD081-d3d10-entry-devpc` | LOW | - | None in the router: run `tools/win/d3d10probe --trace-entry` on unit A |
| 19 | D3D11 has no driver command lists | THREADING caps 0, so the runtime emulates deferred contexts (V); the cost is unmeasured (I). RotTR's D3D11 route ran 2.68 fps with pipeline compiles inline on the game thread (K110) | wt `driver/umd/dxvk/adapter-caps.h` (`D3D11DDICAPS_THREADING`); `KNOWLEDGE.md` K110 | LOW | - | After row 2 |
| 20 | Shader model ceiling 6.6 | SM 6.7 and 6.8 are never reported (V) | wt `driver/umd/d3d12/engine-ddi/caps.cpp:265, 306-307` | LOW | vkd3d-proton supports more | Raise the ceiling when a title needs it |
| 21 | No OpenCL; DirectML unverified | The OpenCL Vendors key is empty (V). DirectML on our D3D12 is untested (I) | Lab probe1 | LOW | ADR 0016 (clvk); CLon12 in the Compatibility Pack | Test the Compatibility Pack together with row 7 |
| 22 | Capture on the GPU route untested | Desktop Duplication and Windows.Graphics.Capture are not measured under GPU DWM (M15.13) | `docs/m15-reconciliation.md` M15.13 | LOW | - | M15.13 capture client |

Row count: 22.

## Row 2 detail: lab-validated D3D11 GPU stacks newer than the package

Both roots use the same shell and engine. They differ only in the ICD and the config record.

### app-route-quiet (the lab's GpuUmdPath since 2026-10-04T01:10Z)

Local stage: `<BC250_ROOT>\scratch\m15\icd-quiet2\stage\app-route-quiet\gpu`. Lab copy: `C:\BC250\m14\app-route-quiet\gpu`.
Hashes from the stage's `SHA256SUMS.txt` (V).

| File | SHA-256 | Source |
|---|---|---|
| `amdgpu_wddm_d3d11.dll` (shell) | `7E9F1692157E79FEC9B0BC2E9F92E73BD4907D00ECBF4885E5B88ABC1B3F76A1` | bc250-win branch `m14/dxgi-private-hints` 7abf21de ("Link the D3D11 shell against the static CRT", on 3970c6aa "DXGI hints accept engine-private resources"); worktree `<BC250_ROOT>\scratch\m14\offer-hint\wt`, artifact `<BC250_ROOT>\scratch\m14\offer-hint\artifacts` |
| `amdgpu_wddm_dxvk.dll` (engine) | `7D28D8F57368C89082E65FD33991AE988B3523C7922909206F0598BF6FBB0554` | DXVK fork branch `amdgpu-wddm/ddi-engine-inline-throttle` ebeadc49 ("[ddi] d3d11: Retire submissions in the initializer's throttle wait", on 0c187731); `<BC250_ROOT>\scratch\m14\engine-throttle\dxvk` |
| `amdgpu_wddm_radv.dll` (ICD) | `D73D827D16D5304D481CF85A07EB38652B17AB3E88CC53A901A6F9757D204808` | mesa-wddm branch `amdgpu-wddm/quiet-stderr` 89ef0dc8 (on cbcc03fc, on `amdgpu-wddm/disk-cache-win` c7d28ba0 = b3e45baf K118 prefetch padding + nine disk-cache commits); deferred destroy v3 lineage of B4D82C4E; `<BC250_ROOT>\scratch\m15\icd-quiet2\SOURCE.txt` says "local only, not pushed" |
| `amdgpu_wddm_d3d11.config` | `78C8B5E7670FB9A1B2F0E8B0FC0C28779D0D32F229852EC4B375906358992C1A` | Caps carried from fl12caps001 (measured on D672813F/8E9B3187), re-encoded with this engine and ICD hash; the exact-pair measurement is planned in `<BC250_ROOT>\scratch\m15\icd-quiet2\LAB-PLAN.md` (quietcaps001). Whether it ran is not recorded in `STATE.md` |

Passed on it (V, `STATE.md` M14.1 block and the 2026-10-04T01:02Z block):
- 2026-10-02 04:03-04:07Z: d3d11fl12 runs at FL 12_1. d3d11bench draws checksum `03a5b8ea7dbea991`. d3d11mt passes with 4 threads, checksum `db13de5647af4c51`, equal to earlier runs. 8 threads also pass. stderr stayed 0 bytes in every run.
- 2026-10-04 (router F65C0E04, gpu-default): d3d11bench checksum 03a5b8ea. Edge, WebView2 and steamwebhelper render on the GPU UMD.
- `STATE.md` records no game session on exactly this root. The no-fault RotTR D3D11 benchmark (K110) ran on app-route-icdmt. That root has the same shell and engine, and ICD B4D82C4E, the predecessor of D73D827D in the same lineage.

### app-route-dp5 (newer: the D3D12 path's ICD)

Local stage: `<BC250_ROOT>\scratch\m15\dp5-freeze\stage\app-route-dp5\gpu`. Built by `<BC250_ROOT>\scratch\m15\dp5-freeze\make-route-stage.py`
from app-route-quiet with only the ICD replaced and the config re-encoded (carried caps).

| File | SHA-256 | Source |
|---|---|---|
| `amdgpu_wddm_d3d11.dll` | `7E9F1692...` (as above) | as above |
| `amdgpu_wddm_dxvk.dll` | `7D28D8F5...` (as above) | as above |
| `amdgpu_wddm_radv.dll` | `F9DCB33B6349642EA1FC849245C1651A849E5CB0E832BC087865327DCA7766EC` | mesa-wddm branch `amdgpu-wddm/draw-path` ae98c795 (on 89ef0dc8); the same file as the release's `payload/d3d12/amdgpu_wddm_radv.dll` |
| `amdgpu_wddm_d3d11.config` | `FD848638B5DD2A5195DEFFAE30C0BDB6D64044F7D42A8566828A89A7B2131DCC` | Carried caps re-encoded for F9DCB33B (`make-route-stage.py:23-33`) |

Passed on it: `STATE.md` native-caps302 block, "D3D11 route on root app-route-dp5 (checksums equal)" (V). That is a
lighter check than app-route-quiet's. This root also closes row 14, because the D3D11 and D3D12 shells then load the
same ICD build. Both roots need one step before `release-sources.json` can name a public source: push the ICD and
engine branches (M15.10).

## Checked and not gaps

- **(V) GPU desktop on by default:** `DwmForceCpu 0`, `RequireKmdSwitches 1`. The router falls back to the CPU route by itself when the KMD closes the interop switches.
- **(V) D3D12 FL 12_1 and tiled resources tier 3 for every app:** `AmdgpuWddmSparseBinding` is absent on the lab, which means on (wt `driver/umd/d3d12/README.md`). dxdiag lists 12_1, and the installer checks FL 12_1 after it installs.
- **(V) The three release-gate fixes (M15.8) are on for every D3D12 app:** each one is an "-off" switch. The fix is therefore the default (wt `driver/umd/d3d12/ddi-trace.h:45-52`).
- **(V) KMD gates:** all open, `EnableFullWddm 2`. The values at 0 (`EnableMmioWrite`, `EnableHangBugcheck`, `EnableSdmaIbControl`, `EnableSdmaVaControl`, `EnableSdmaPteControl`, `EnableRlcReloadReset`, `TraceUmdProbes`) are diagnostic only.
- **(V) DPM:** on (`DpmMode 1`, `DpmLastMode 1` on the lab). The 1500 MHz ceiling is the owner's decision, not a gap. Note that the lab measured the published game numbers at 2000 MHz and 40 CUs (row 9).
- **(V) 64-bit Vulkan registration:** the installer registers the ICD both in the class key (`VulkanDriverName`) and in `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`.
- **(V) D3D11 caps for routed apps:** the package's D3D11 caps record A9B498ED decodes to FL 12_1, tiled tier 3, ROVs, conservative rasterization tier 3.
- **(V) 10-bit and FP16 swap chains:** the driver accepts them, and DWM composes them (M15.4).
- **(I) Mesh shaders, VRS, sampler feedback and view instancing reported as not supported:** this follows from the hardware generation (gfx1013), not from a default.
- **Known and tracked, not hidden:** a GPU hang ends in 0x116 (M15.12). Row 13 makes the exposure larger on tester PCs than on the lab.
