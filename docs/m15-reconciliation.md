# M13, M14 and M15: what remains, reconciled (the M15 amendment)

Date: 2026-09-30 (UTC). Status: proposed content for the amendment that the
[main roadmap](00-goal-and-roadmap.md) section "Direction after M12" (lines 123-141) requires before M14 and M15
enter the milestone table with exit criteria. It changes nothing by itself: a criterion below becomes binding
when this file is committed and the milestone table cites it. Nothing here is a measured result unless it cites
a `facts.md` row or a lab trial.

Markers used throughout:

- **MEASURED** (fact M-number or lab trial number): observed on unit A or on the host, with the source named.
- **DECIDED** (owner, date): the owner's instruction; the owner's words are kept in the owner's working notes.
- **DECIDED** (integrator, date): a technical decision taken under the owner's delegation of 2026-09-29, which
  made the integrator the lab operator and the technical decision maker.
- **PLANNED**: work not yet done; no result is claimed.

Lab trials cited by number (for example "trial 183") are the native D3D12 trial series on unit A. By the owner's
decision of 2026-09-28 their logs, configurations, hashes and metadata stay in the workspace and are not
published; this file names the trial and its headline outcome only. The exceptions are the trials behind a
`facts.md` row, whose write-ups and client-side records are committed as evidence: E47 and E49 (game sessions)
and E50 (M778-M782: rendering, Present, FL 12_1, ROV, conservative rasterization, DXR). Section 4 lists what the
rule means for closing M15.

## 1. Purpose and the owner's decisions it records

The owner asked on 2026-09-30 for one reconciled statement of the work left in M13 (accelerated desktop: hosted
RADV, GPU DWM, native presentation), M14 (system Direct3D 11 through a DDI user-mode driver with DXVK as the
engine) and M15 (native system Direct3D 12 with vkd3d-proton as the engine). This file is that statement and the
exit criteria for M14 and M15.

| Date | Decision | Effect here |
|---|---|---|
| 2026-09-25 | M11 is deferred to the final load-testing phase; its 24-hour and stuck-queue criteria stay open (roadmap lines 109-115) | M11 is outside M13-M15 |
| 2026-09-26 | ADR 0017: one hardware implementation; hosted ICD for M13; DXVK behind a system D3D11 DDI; vkd3d-proton behind a native D3D12 DDI; FL 12_0 Must, 12_1 Should, 12_2 not a target; 5 % bound for M14; no numeric M15 bound until its first measurement | Frames M14 and M15 |
| 2026-09-26 | Actual GPU rendering of the Windows desktop has priority; M11 24-hour tests are not resumed; Linux GLX debugging is deferred | Section 5 |
| 2026-09-27 | ADR 0018: the CPU present transport of ADR 0015 is retired; ADR 0019: the kernel driver targets the newest WDDM DDI version of the WDK | Sections 3 and 5 |
| 2026-09-28 | Every lab test is bounded to at most three minutes; a 30-minute GPU DWM run (M13.4) was declined | Section 3 |
| 2026-09-28 | GPU work for M13 and M14 goes ahead; long M13 tests are not required | Section 5 |
| 2026-09-28 | D3D12 compatibility and FL 12_1 are key; The Witcher 3 in its current next-gen DX12 edition with ray tracing, started from Steam, is the acceptance target | M15 table |
| 2026-09-28 | The goal is a native D3D12 driver through the system `d3d12.dll` and `dxgi.dll`; DXVK, vkd3d-proton and RADV stay inside the native drivers; no replacement DLL next to the application | M14 and M15 tables, section 5 |
| 2026-09-29 | Game trials may take five minutes; Present and the swap chain must be ready for HDR and 10-bit back buffers; multithreading is tested with the project's own clients before a game relies on it | M15 table |
| 2026-09-30 | Automatic DPM in the kernel driver, first ceiling 1500 MHz, hard ceiling 2000 MHz, thermal limits unchanged | Section 3 |
| 2026-09-30 | Game tests at the presets low, high and high with RT, each an interactive session of 7-10 minutes whose timer and end the operator controls | M15 table |
| 2026-09-30 | Off-GPU frame cost first; then CPU time spent copying is the priority; game sessions at most 7 minutes; the screen must not glitch | Sections 3 and 4 |
| 2026-09-30 | DWM composes on the GPU; the DPM ceiling of 2000 MHz is approved for RT tests | M13 table, section 3 |
| 2026-09-30 | DECIDED (integrator, under the GPU DWM decision): GPU DWM is promoted through a bounded trial ladder without a 30-minute soak and recorded as promotion, not as M13.4; its game steps are 7-minute sessions; GPU DWM becomes the lab's default desktop after the last ladder step; a UMD router keeps other D3D10/11 processes on the CPU UMD until M14 takes applications; the kernel driver's two interop switches become on by default and are closed automatically after an unclean boot | M13 table, section 4 |

## 2. Per milestone

The "bound" column names the lab limit a step must fit: **3 min** for every non-game trial, **7 min** for an
interactive game session.

### M13: accelerated desktop

Gates from [m13-accelerated-desktop-roadmap.md](m13-accelerated-desktop-roadmap.md) lines 21-27; route from
ADR 0017 point 2; G0 is the implementation gate of M13 (hosted RADV, GPU DWM, native presentation).

| Criterion | Evidence so far | Status | What remains | Bound |
|---|---|---|---|---|
| M13.1 correct visible desktop under full WDDM, CPU renderer allowed | MEASURED: visible desktop with softpipe and hardware flip (M406), bands removed (M407), llvmpipe with fluid mouse (M410); the CPU desktop UMD is the lab's deployed desktop since then | partially met | the Start-menu check (M407 "inconclusive"), a recorded move/resize check; the 30-minute duration is superseded (section 5) | 3 min |
| M13.2 cross-process sharing and synchronization, 1000 iterations | MEASURED: 1000 bidirectional exchanges with exact pixels and matching submitted/completed counts (M565, repeated in M575, M599); the imported texture survives normal and forced owner exit (M566) | partially met | resize, CPU map/unmap and residency cases; the native D3D12 UMD as producer for GPU DWM (never tested) | 3 min |
| M13.3 native D3D draws on the GPU, route recorded by ADR | MEASURED: exact native draws through Zink/RADV (M531), hosted fills and screens (M539-M541), sampler and texture negative controls (M554), 56-64 state images equal to WARP and CPU (M586, M599), submissions correlated with completed fences (M559) | partially met | one gate record that maps triangle, textured quad, blending, depth/stencil, render-to-texture and shader control flow to image comparisons with the hardware-route negative control; then the ADR 0017 amendment that confirms the hosted route (ADR 0017, Consequences) | 3 min |
| M13.4 DWM composition on the GPU, system-registered UMD | MEASURED: trial DWM050, 124.9 s: both 40712-pixel oracles, 4316 DWM DMA pairs on the BC-250 plus 182 on another adapter, 4274 exact runtime Present joins, no runtime-surface CPU maps (M723); audited as G0 implementation achieved for the tested path (M724). The UMD was selected by a trial router, not registered; kernel driver 0.7.170 | partially met | ladder T0-T3 below on the current kernel driver; registration through the lab's UMD swap procedure instead of a replaced file; the other adapter's 182 DMA pairs named (M13.4 forbids a WARP fallback) | 3 min |
| M13.5 GPU-to-display presentation with correct timing, no steady-state CPU full-frame copy | MEASURED: 4319 flips retire with matching scanout addresses, 59.944 Hz VSync (M719, M720); steady render interval without a missing VSync report (M723) | partially met | the same measurement under game load (ladder T6); DWM composition latency against the game's longest GPU packets | 7 min |
| M13.6 desktop lifecycle, ten repetitions of each transition | none recorded | not met | Windows restart, logoff/logon, DWM restart, blank/unblank, resize and fullscreen transitions, ten each, every repetition its own trial; ladder T3 covers two DWM restarts only | 3 min per repetition |
| M13.7 stable accelerated desktop (24-hour soak) | M8 compute regressions exist on earlier drivers only | superseded in duration (section 5) | the M8 regression on the final driver stays, as a bounded trial | 3 min |
| G0 (a) GDI/window uploads classified | MEASURED: all render image writes attributed to UpdateSubresource or initial data (M695, M720) | met for the tested path | repeat on the promoted artifacts (T2) | 3 min |
| G0 (b) kernel software-blit positive control | MEASURED: zero software-present blits in 89 in-render samples, positive rollback control Blit=1 (M702) | met for the tested path | repeat on the promoted artifacts (T2) | 3 min |
| G0 (c) descriptor writes documented | MEASURED: descriptor spans reconcile (M697, M701) | met for the tested path | none beyond T2 | 3 min |
| G0 (d) persistent-mapping census | MEASURED: 24635 map requests, runtime-surface maps 0 (M723) | met for the tested path | none beyond T2 | 3 min |
| G0 native presentation in the kernel driver | MEASURED: DWM's Flip path is copy-free (M545, ADR 0018 point 3); DWM050 ran with the GPU present blit switch on (M723) | partially met | ADR 0018 point 4: remove the software Blt packet and its CPU copy once the engine copy passes ADR 0018 point 5 | 3 min |
| Visual correctness on GPU DWM (BD-043, trails on move/resize) | MEASURED: owner saw no glitch in DWM022 (M595); later GPU DWM trials passed their oracles (M723) | in progress | the owner's visual check on the promoted artifacts during T2/T3 | 3 min |

**GPU DWM promotion ladder** (DECIDED, integrator, 2026-09-30; every step ends with the CPU desktop restored
until T7 passes):

| Step | What | Pass |
|---|---|---|
| T0 | host: hosted UMD that takes the adapter LUID and ICD path without the trial environment and logs nothing by default; 10-bit shared-surface support; hosted ICD from the current fork line; router with a registry kill switch that sends a restarted DWM to the CPU UMD; deployment kit on the current kernel driver | host gates pass; one lab window-client check exact |
| T1 | kernel interop switches on, DWM still on the CPU UMD | health flags 15, CPU composition exact, no 0x116 |
| T2 | DWM050 replay on the current kernel driver with the T0 artifacts | DWM050's oracles, G0 (a)-(d), other adapter named |
| T3 | router registered by the kit; DWM restarted twice (GPU, then kill switch to CPU) | both restarts inside the budget, correct images |
| T4 | 10-bit client under GPU DWM | three Presents S_OK, readback within BD-049's 1 LSB |
| T5 | native D3D12 Present client under GPU DWM | frames exact, zero kernel software blits |
| T6 | The Witcher 3 preset high, GPU DWM, CPU capture | route shots correct; fps, main-thread Ready time, DWM CPUs and DWM GPU packets against the CPU-DWM baseline of the same build |
| T7 | T6 again | a second clean session |

State on 2026-10-01: MEASURED, T1-T7 passed on KMD 0.7.182.1 (M771, M772). T4 and T5 were repeated with clients
rebuilt from main; T6 and T7 ran The Witcher 3 at 35.5 and 35.4 frames/s on the GPU-composed desktop against 21.1
on the CPU-composed one. GPU DWM is the lab default since 2026-10-01T01:50Z, recorded as promotion. Open: DWM050's
pixel oracles have not been re-run on the router artifacts, and the reboot behaviour of the GPU route is untested.

### M14: system Direct3D 11 through a DDI user-mode driver with DXVK as the engine

Scope from ADR 0017 point 4 and point 7. The criteria M14.1-M14.8 are proposed here as M14's exit criteria.

| Criterion | Evidence so far | Status | What remains | Bound |
|---|---|---|---|---|
| M14.1 system entry: `D3D11CreateDevice` through the system `d3d11.dll`/`dxgi.dll` reaches our UMD, no application-local DLL | MEASURED: first native system D3D11 GPU render, FL11_1, 64x64 checksum equal to the CPU route (M736); selection was a trial file route, because a live `UserModeDriverName` change is not picked up (M729) | partially met | registration as the adapter's D3D10/11 UMD for applications (today entries 1 and 2 name the CPU desktop UMD); the router of the GPU DWM ladder hands applications over when M14 is accepted | 3 min |
| M14.2 correctness against per-application DXVK on the same GPU | MEASURED: system route equals per-application DXVK byte for byte in all three benchmark scenes (M739); draws exact after the tie-free scene revision (M738, M739) | met (offscreen scenes) | the same comparison for one real D3D11 application | 3 min |
| M14.3 feature level from the exact driver pair | MEASURED: FL11_1 caps from the exact pair (M726, M751, M755); FL12_1 device with tiled resources tier 3, ROVs and conservative rasterization tier 3, tiled operations exact (M770) | met (FL11_1); FL12_1 functional witness only | none for FL11_1; FL12_1 rests on the sparse conformance item of M15.2 | 3 min |
| M14.4 window Present and resize | MEASURED: native GPU window Present with six scenes equal to runtime011 (M747); three resizes and twelve Presents exact (M748, M752) | met (functional) | copy-free composition of a D3D11 application window under GPU DWM (ladder T5 shape with a D3D11 client) | 3 min |
| M14.5 error propagation and recovery | MEASURED: creation OOM and recovery, Map READ after removal returns 0x887A0005, sticky removal (M754, M756) | met | none | - |
| M14.6 threading (ADR 0017 point 8) | MEASURED offline only: inline execution costs 1.5-1.9 times DXVK's worker threads per draw on CPU-bound work (`docs/design/d3d11-ddi-engine.md` line 273) | not met | the lab probe: do the runtime callbacks work from a thread outside a DDI entry; then workers with a per-device mutex, or a broker mode | 3 min |
| M14.7 5 % bound against per-application DXVK | not measured (`docs/design/d3d11-ddi-engine.md` line 279); the workload exists (`tools/win/d3d11bench`, `compare.py`) | not met | d3d11bench on unit A, system route against per-application DXVK from the same DXVK revision, same clocks, CU count and settings, distributions reported; then one D3D11 title | 3 min per run |
| M14.8 residency, offer and reclaim | not implemented (`docs/design/d3d11-ddi-engine.md` gaps table) | not met | the engine ABI minor with discarded-content placeholders and the hosted query from Vulkan memory to kernel allocation | 3 min |

### M15: native system Direct3D 12 with vkd3d-proton as the engine

Scope from ADR 0017 point 5 and the owner's decisions of 2026-09-28 to 2026-10-01. The criteria M15.1-M15.13 are
proposed here as M15's exit criteria.

| Criterion | Evidence so far | Status | What remains | Bound |
|---|---|---|---|---|
| M15.1 system entry: `D3D12CreateDevice` through the system `d3d12.dll`/`dxgi.dll` reaches our registered UMD; no application-local `d3d12.dll`, `d3d12core.dll` or `dxgi.dll` of ours | MEASURED: the fourth UMD name resolves after an adapter restart and the runtime enters `OpenAdapter12` (M768); engine control on unit A (M757); functional copy, draw, scene and Present through the system runtime in trials 024, 034, 036 and 053-056 (M778, M779); the game loads the system runtime with its own Agility SDK core in trial 068 onwards | met | none for the entry itself. Since 2026-10-01 (trials 227/228) the fourth registration entry holds the accepted triplet between sessions: a promotion session kept shell 430CB1AE, engine 106D09E5 and ICD 081DAF26 after a caps witness through the system runtime (device at FL 12_1, MaxSupportedFeatureLevel 12_1, all three DLLs loaded, sparse policy on), and a caps session on the registered triplet outside any swap repeated it; the trial kit swaps only the entries a candidate changes and restores them; the kill switch puts the diagnostic shell back at the same path. Task Manager reads `DirectX version: 12` on build 22621; its FL string comes from D3D11 (M14.1). Check 309 repeated the caps witness on the triplet registered on 2026-10-02, with nothing swapped (M780) | 3 min |
| M15.2 FL 12_0 (Must) | MEASURED: the system runtime reports FL 12_1 and tiled resources tier 3 and creates a device at FL 12_1 (trials 028, 029); since trial 059 without any client switch, by a driver-side sparse policy with a registry off switch (the off switch is not measured on the lab); a reserved resource mapped through `UpdateTileMappings` reads back exact (trials 058, 059); FL 12_1 again on the triplet registered on 2026-10-02, 12_2 refused (check 309) (M780); the whole Vulkan CTS sparse-resources list, aliasing and residency groups included, 19078 cases in 147 bounded batches: 10778 Pass, 8299 NotSupported with capability reasons, 0 Fail, 0 Crash, the one Timeout (KMD 0.7.191.1) passing with its whole batch on 0.7.192.1 (M773, E45) | met | none for the exit criterion; a comparison of the NotSupported set with RADV on Linux, and a D3D12-level tiled-resources conformance subset, can follow | 3 min per batch |
| M15.3 FL 12_1 (Should) | MEASURED: ROVs and conservative rasterization tier 3 reported (M564, M770); a conformance client with exact CPU oracles through the system runtime at FL 12_1: ROV exact (trials 237-240); conservative rasterization tier 3 exact, SV_InnerCoverage included, on an ICD carrying the RADV inner-coverage fix (trials 239, 240) (M781). The ICD registered at the time fails only inner coverage (trial 237): RADV drew overestimate plus inner coverage as plain underestimate, so partly covered pixels were lost. The fix rasterizes over and under at once with one extra sample and MSAA enabled; without the MSAA enable, trial 238 read the 153 partly covered pixels as fully covered. ROV, conservative tier 3 and indirect DispatchRays pass together on one candidate triplet (trial 240). Its engine and ICD, with a shell that adds FP16 primaries, were registered by trial 243 (check 244: FL 12_1, tiled tier 3) | met | the conformance client has not been rerun on the triplets registered after 2026-10-01, whose ICDs carry the same RADV change: rerun it on the accepted build before M15 closes; a wider conservative-rasterization subset can follow | 3 min |
| M15.4 Present: 8-bit today, architecture ready for HDR and 10-bit (owner, 2026-09-29) | MEASURED: 8-bit Present is the driver default, three frames exact including after ResizeBuffers (trials 053-056, M779); a 10-bit R10G10B10A2 chain presents on the CPU desktop UMD with a 1-LSB rounding difference (trial 098, BD-049); under GPU DWM a format ladder (Bgra8, ResizeBuffers to R16G16B16A16_FLOAT with SetColorSpace1 G10_NONE_P709, R10G10B10A2, Bgra8) presents 8 of 8 frames, 7 exact and one 10-bit frame with BD-049's 1 LSB, the FP16 window composed by DWM on the GPU route (trial 242, kernel driver 0.7.191.1 at DDI interface WDDM 2.9 with FP16 composed-surface admission); the shell carrying the FP16 row is registered since trial 243 | met for 8-bit, 10-bit and FP16 back buffers | HDR10 (G2084) is not offered yet (CheckColorSpaceSupport flags 0); HDR scan-out needs the WDDMVersion caps step, an EDID and a DCN plane colour path | 3 min |
| M15.5 ray tracing tier 1.1 | MEASURED: Vulkan-level controls: ray query triangle flow with a negative control (M760), direct TraceRays pipeline case (M761), hardware BVH intersection in the compiled program (M762); native D3D12: inline ray query exact (trial 062), state objects created and destroyed twice (trial 064), DispatchRays exact (trial 065); The Witcher 3 world renders with RT on (trial 120; on later builds M775, M777); indirect DispatchRays through `CreateCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS)` exact in five variants and with GPU-written arguments, 694 checks, 0 mismatches (trial 240), registered since trial 243 (M782). The shell reports tier 1.1 only with its `raytracing-tier` experiment, from the process environment or an application profile; without it the registered shell reports tier 0 (check 309, M780) | partially met | a DXR conformance subset in 3-minute batches; tier 1.1 as the driver's default report | 3 min |
| M15.6 multithreading tested with our own clients (owner, 2026-09-29) | MEASURED: four recording threads on one queue, 0 mismatches, no removal (trials 176, 180, 188); list reset and allocator churn on four threads, constants exact (trials 185, 186); copy-queue and direct-queue upload races, 0 mismatches in 2.4-2.5 G checked units each (trials 191, 192, 194); long-lived witness resources under churn, 0 mismatches (trials 195, 196) | met for these shapes | memory pressure that forces paging (trial 196 stopped at the client's cap with 5 GB of budget free) | 3 min |
| M15.7 acceptance target: The Witcher 3 next-gen DX12 from Steam at low, high and high with RT, interactive sessions, correct image | MEASURED at native 1080p fullscreen (no upscaler, frame generation or dynamic resolution) on one accepted build, the registered triplet with the release gate (shell adapter107, trial 247) and kernel driver 0.7.193.1, desktop composed on the GPU: low 45.3 fps (trial 246), high 31.3 fps with the 3D engine 96 % busy at 2000 MHz (trial 249), high with RT 9.3 fps with the 3D engine 98 % busy at 2000 MHz (trial 250); every shot correct in the three sessions, RT reflections and lighting included (M775, E47). Earlier builds: low 23.1 and high 18.5 fps (trials 166, 167, CPU-bound), high with RT 9.1 fps at an 1800 MHz thermal cap (trial 223). The streaming noise of section 3 was last seen in trial 184 | not met | the streaming-noise acceptance of section 3. The start through the Steam client is recorded at high with RT: 9.1 fps, the 3D engine 98 % busy, every shot correct, RT from the driver's application profile alone (trial 255, shell adapter109, M777, E49) | 7 min per preset |
| M15.8 stability: no bugcheck in the game sessions | MEASURED: three 0x116 in game trials (147, 151, 153); the cause of 151 and 153, a heap import that handed the VA to the GPU before residency, is fixed and the route passed (trials 155, 156); no 0x116 in trials 155-201; trial 245 (LOW, KMD 0.7.191.1, registered triplet) ended in 0x116 after about five minutes: the 147/208/209 class, a job queued before a runtime-backed heap import was unmapped and destroyed read its first page (225 read faults, 1228 waves halted), the timeout found no engine reset; on the release gate (shell adapter107: two-phase retirement over every engine context, a progress gate and a bounded quarantine of released imports) with kernel driver 0.7.193.1, trials 246, 249 and 250 (low, high, high with RT, 7 minutes each) ended without a bugcheck, a timeout or a TDR, and the GCVM protection fault status, which that driver never clears, still read 0 after trial 250: no VM fault since the driver loaded before trial 246 | partially met | a client reproducer of the 147 class (a runtime-backed heap released while a job queued on another engine context still reads it) that is caught on the previous shell and passes on the release gate, because session counts alone cannot show the class gone (one 0x116 in the eleven game sessions of trials 214-245); if it recurs, the kernel driver's UTCL2 decode, timeout register snapshot and destroy identity name the owner | 7 min |
| M15.9 performance bound | none: ADR 0017 point 7 gives M15 no numeric bound until its first measurement; the per-application measurements (M580, M607) used the retired CPU present path and do not compare | not met | a matched per-application vkd3d-proton against native measurement on the same route, preset, clocks and CU count; the owner then sets the bound | 7 min per side |
| M15.10 reproducibility: every accepted artifact built from a committed source revision in the public repositories | the kernel driver 0.7.197.1 (`0cbef549`), the D3D12 shell (`m15/replay-wake` `e9f5e701`) and the application router `674AD261` (`driver/umd/router`) are in `main`; the registered engine and ICD are published on branches that do not move (`amdgpu-wddm/registered-2026-10-02`, `c3710ac1` and `ae98c795`). Two deployed binaries still come from working trees with uncommitted changes: the compositor's Zink UMD `18BFC610` and the ICD `D672813F` of the allowlisted D3D11 route. A rebuild does not reproduce the hash either: the driver and the shell are built without `/Brepro`, so two builds of one tree differ in the PE timestamp, the PDB signature and the signature blob (measured 2026-10-03, same size, different hash); the per-file source manifest is what is compared | partly met | commit the two working trees; keep the source manifest as the equality check, or build with `/Brepro` and a deterministic signing step | [Registered on unit A](../README.md#registered-on-unit-a) |
| M15.11 video encoding on the GPU (owner, 2026-10-01; route decided by the owner the same day): a standard Windows application (Game Bar, Windows Camera or Chromium) records H.264 through a Media Foundation hardware encoder MFT in our driver package, whose motion estimation, prediction, transform, quantisation, reconstruction and deblocking run on unit A's GPU; entropy coding and bitstream assembly on the CPU are accepted by the owner's decision | MEASURED: unit A offers no hardware encoder MFT today, the inbox software `H264 Encoder MFT` is the only H.264 encoder, and nothing on the image disables hardware MFTs (M774, E46). The video block is not a route: under Linux amdgpu answers no UVD, VCE or VCN query and reports VCN firmware 0x00000000 (M46), and its IP discovery code adds no driver block for VCN 2.0.3 on this family. MEASURED on the development PC only (2026-10-05, `driver/umd/mft-h264`): the encoder exists and is complete as a Constrained Baseline CAVLC encoder with its eight stages on Direct3D 11 compute; the conformance sweep passes 65 of 65 cases, where each case requires the encoder's own GPU reconstruction to be sample-for-sample identical to the inbox H.264 decoder MFT's output; 1280x720 qp 26 with deblocking gives 6 of 6 pictures bit exact, exact nnz agreement and PSNR(Y) 47.21 dB; the asynchronous hardware MFT contract holds end to end (found with `MFT_ENUM_FLAG_HARDWARE`, `MF_TRANSFORM_ASYNC`, streaming refused before the unlock, 36-byte `MF_MT_MPEG_SEQUENCE_HEADER`, `ICodecAPI` round trips, CABAC refused, drain complete, every access unit decoded by the inbox decoder); the four Direct3D 11 input shapes a capture client delivers all pass (BGRA and NV12, plain and as one slice of a texture array); an ordinary `MFCreateSinkWriterFromURL` pipeline to `.mp4` picks the transform up and the file plays (`h264 / Constrained Baseline / 1280x720 / yuv420p`), 6 of 6 repeat runs; against the inbox CPU encoder at 1280x720 CBR 6 Mbit/s on the same source it is 4 % smaller at about 1 dB lower luma PSNR and 28 % slower per picture. **Every one of those numbers is from an NVIDIA RTX 4090 on the development PC: no stage of this encoder has run on unit A's GPU, and nothing of it is registered in the driver package** | not met | the shaders running on unit A at all through our D3D11 path, then the sweep and the throughput on unit A (experiments/E50); the per-adapter `MFT0` registration in the KMD INF (`driver/umd/mft-h264/INSTALL.md` route B); the Game Bar before/after oracle, then Windows Camera and Chromium | 3 min |
| M15.12 hang recovery (proposed 2026-10-01): a GPU hang caused by one application ends that application's device (DXGI_ERROR_DEVICE_REMOVED) and the desktop survives, instead of a bugcheck | every hang so far ends in 0x116: the KMD refuses ResetEngine and has no adapter reset (trials 147, 151, 153, 245); amdgpu has no working reset on this part either (M53) | not met | which GFX reset unit A survives (queue unmap and CP soft reset first, measured on a deliberately hung client), then DxgkDdiResetEngine for node 0 | 3 min |
| M15.13 capture and shared surfaces (proposed 2026-10-01): desktop and window capture (DXGI Desktop Duplication, Windows.Graphics.Capture) and cross-process/cross-API shared resources work on the GPU route, the input M15.11 recording needs | not measured on the GPU route | not met | a capture client with an exact oracle under GPU DWM; a D3D11-D3D12 shared-handle and keyed-mutex client | 3 min |
| M15.14 independent flip (owner, 2026-10-05): a fullscreen or borderless fullscreen D3D12 or D3D11 game whose window covers the output is scanned out from its own swap-chain buffer (DirectFlip or independent flip), so DWM does not compose its frames; DWM composes again when a window (the lab overlay, a notification) covers part of the game | MEASURED: the kernel driver reports `SupportDirectFlip` and `FlipCaps.FlipIndependent` (M65) and flips the scan-out in `DxgkDdiSetVidPnSourceAddress` (M97, `EnableVidPnFlip` on in the release); in The Witcher 3 at 1080p fullscreen and borderless (trials 157-161, 2026-09-30) DWM presented at the game's rate, so every game frame was composed: no game swap-chain buffer reached the scan-out | not met | find the layer that stops it (swap-chain allocation flags in the kernel driver, the shells' present path, the WSI path of ADR 0018); then one W3 session per mode (fullscreen, borderless, borderless with the overlay) with the ETW present mode per frame and the DWM present rate against the game rate; the criterion: no DWM composition of game frames in fullscreen and borderless without the overlay, and the frame rate at least that of the composed route | 7 min per mode |

## 3. Cross-cutting items

- **Streaming noise in the game (blocks M15.7).** MEASURED: permanent sparkle noise on streamed meshes and
  textures in trials 171 and 184, in 2 of 7 game sessions on optimised builds and 0 of 4 on unoptimised ones;
  no API error, no device removal. MEASURED, candidates ruled out: the simple upload race on the copy and direct
  queues (trials 191, 192, 194), long-lived resources under churn (trial 195), per-draw constants under four
  recording threads (trials 185, 186); a host review found no defect in the kernel driver's paging path over six
  fault classes and one visibility gap (a refused page-table update or TLB flush returns success with no counter).
  Remaining candidates, PLANNED tests: kernel paging under real memory pressure (the pressure client, second
  version); an early free made permanent because RADV's WDDM winsys frees a buffer's VA at destruction (the
  deferred-destroy ICD, two game sessions); invariant instrumentation stays in every game session. Acceptance:
  the mechanism named by a reproducer that fails before the fix and passes after it, and the preset matrix
  clean. The owner requires that the screen does not glitch (2026-09-30).
- **CPU cost of copying (owner priority, 2026-09-30).** MEASURED: RADV's IB gather copy from write-combined
  memory costs 1.83 ms of a 2.32 ms submit path per frame (trial 190). PLANNED: a no-copy submission design
  (in progress), validated by the scene client (exact) and a 7-minute session with CPU stacks against trial 190.
- **GPU DWM (M13, and the largest off-GPU lever for M15).** MEASURED: on the CPU desktop UMD, DWM used 4.2 of
  12 CPUs in the game at 20.3 fps, in twelve llvmpipe threads, and took 3.4 ms per frame from the game's main
  thread, which waited 7.7 ms per frame for a core (trial 178); raising the game's priority returned 2 ms and
  slowed its code about 10 % (trial 179). DWM on the GPU used 0.19 CPUs in the DWM050 trial of M723 (its process samples, 86.6 s). MEASURED
  by ladder steps T6 and T7 (M772): 35.5 and 35.4 frames/s against 21.1 at the same build and route, DPM reaching
  2000 MHz only on the GPU route, DWM at 8.6 % of one CPU in the game.
- **DPM and CU count.** DECIDED (owner, 2026-09-30): automatic DPM, ceiling 1500 MHz, hard ceiling 2000 MHz,
  2000 MHz approved for RT tests, thermal limits unchanged (stop and cool down above 85 C). MEASURED: the
  governor raises to 1500 MHz under the game (trials 145, 146) and stepped down on thermal soft at 85.2 C
  (trial 149); with RT the frame is GPU-bound (trial 183). The 40-CU mode of the roadmap's O1 is active on the
  lab and was validated at a cold boot. No `facts.md` row records DPM or 40 CU yet. Every performance comparison
  states the clock ceiling and the CU count. MEASURED: trial 202 (high with RT, 2000 MHz ceiling) reached 2000 MHz at 1000 mV under the game with no throttling at 67-68 C; its frame measurement was lost to a session tooling fault and is repeated as trial 203.
- **UMD registration and the router.** MEASURED: a live `UserModeDriverName` change is not picked up (M729);
  the DX12 name needs an adapter restart before the runtime sees it (M765, M768). Entries 1 and 2 (D3D9/10/11)
  name the CPU desktop UMD; the DX12 entry is swapped per game session. DECIDED (integrator, 2026-09-30): DWM
  goes to the hosted GPU UMD and every other D3D10/11 process to the CPU UMD through a router, until M14.1
  registers the DXVK UMD for applications.
- **Kernel driver switches and version.** MEASURED: the GPU present blit and DWM interop are start-latched
  registry switches, off by default (`driver/kmd/wddm.c` line 1772). PLANNED: both on by default and closed by the
  driver after an unclean boot, as it already resets DPM. The driver is compiled at WDDM 2.0
  (`driver/kmd/bc250kmd.h` line 42) although ADR 0019 targets the newest version; HDR scan-out needs a later one.
- **Vulkan WSI presentation (M12, ADR 0018).** Not met: the registered ICD presents through the retired CPU
  path, and dxgkrnl refused the kernel-thunk Present route (M677). A native D3D12 device on the adapter now
  exists, which the WSI's DXGI path needs (`docs/design/wsi-engine-present.md`).
- **Bounds.** DECIDED (owner): 3 minutes for every non-game lab trial (2026-09-28); game sessions 7-10 minutes
  controlled by the operator (2026-09-30), at most 7 minutes (2026-09-30). Every step in section 4 fits one of
  the two.
- **Publication of native D3D12 evidence.** DECIDED (owner, 2026-09-28): the native trials' lab payload stays in
  the workspace. A milestone closes by a commit that adds evidence and updates `facts.md` (roadmap line 25), so
  closing M15 needs the owner's decision on a publishable summary form of those trials. Committed so far:
  E47, E49 and E50 publish the trials' write-ups and client-side records (client trace, supervisor result,
  selected shell lines, caps witness), without the shell's full DDI trace, the kit archive or screenshots; M775,
  M777 and M778-M782 rest on them.

## 4. Order of work

| Order | Item | Why this position | Bounded validation |
|---|---|---|---|
| 1 | Streaming noise | blocks acceptance of every optimised build and of M15.7; faster builds show it more often | pressure client (3 min); deferred-destroy ICD, two 7-min sessions; instrumented build in every session |
| 2 | GPU DWM ladder T0-T7 | M13.4/M13.5 evidence and the largest off-GPU lever; every later game measurement depends on which DWM composes | T1-T5 3 min each, T6/T7 7 min each |
| 3 | IB gather no-copy | the owner's copy priority; 1.83 ms per frame measured | scene client exact (3 min); 7-min session with stacks against trial 190 |
| 4 | RT at the 2000 MHz ceiling | RT is GPU-bound; the clock is its first lever; the ceiling is reached (trial 202) | trial 203, 7 min, Tctl below 85 C |
| 5 | Preset matrix low, high, high with RT on one accepted build with GPU DWM | the owner's acceptance target | three 7-min sessions, every shot correct |
| 6 | M15.9 matched per-application measurement | M15 needs its first measurement before a bound exists | two 7-min sessions per preset |
| 7 | M14.6 threading probe, M14.7 5 % bound, M14.1 registration | M14 takes applications from the router only after its bound is measured | 3-min trials |
| 8 | Conformance: sparse CTS batches, DXR subset, M13.3 gate record, M13.6 lifecycle | turns functional witnesses into criteria | 3-min batches and repetitions |
| 9 | Reproducibility: kernel driver branch into main, fork commits published, WDDM version move (ADR 0019) | M15.10 and every closing commit | host gates; one exact-artifact trial per artifact |

**M15 closed** means all of the following are measured and recorded:

1. M15.1: an application's `D3D12CreateDevice` through the system runtime reaches the registered UMD of the
   accepted build, with a module witness and no application-local DLL of ours.
2. M15.2 and M15.3: FL 12_1 reported and a device created at it without any switch; the sparse CTS batches pass
   with every difference explained in `facts.md`; ROV and conservative-rasterization image tests pass.
3. M15.4: 8-bit, 10-bit and FP16 swap chains present exact frames through ResizeBuffers, under GPU DWM.
4. M15.5: inline ray query, DispatchRays and indirect DispatchRays exact; the DXR subset passes.
5. M15.6: the multithreaded clients pass on the accepted build, including forced paging.
6. M15.7: The Witcher 3 next-gen DX12 started from Steam at low, high and high with RT, one 7-minute
   interactive session each on the accepted build under GPU DWM: every screenshot correct, no glitch seen,
   frame-time distribution and GPU busy recorded.
7. M15.8: no bugcheck, TDR or device removal in those sessions and in the conformance batches.
8. M15.9: the matched per-application measurement is recorded and the owner's bound is set and met.
9. M15.10: every artifact of the accepted build comes from a committed public revision and is validated exactly.
10. The evidence enters `facts.md` in the form the owner decides for the native trials (section 3).

## 5. Superseded or retired items

| Item | Instruction | What replaces it |
|---|---|---|
| 24-hour soak of M13.7 | owner, 2026-09-28: long M13 tests are not required | the M8 regression on the final driver as a bounded trial; M11's own 24-hour criterion stays deferred (owner, 2026-09-25, roadmap lines 109-115) and is not resumed (owner, 2026-09-26) |
| 30-minute runs of M13.1 and M13.4 | owner, 2026-09-28: at most three minutes per lab test; the 30-minute M13.4 run was declined | the GPU DWM promotion ladder T0-T7, recorded as promotion, not as a 30-minute M13.4 result |
| M11 24-hour tests during this work | owner, 2026-09-26 | M11 stays open and deferred to the final phase |
| Linux GLX debugging | owner, 2026-09-26: deferred while the Windows GPU desktop is pursued | Linux stays a reference tool |
| Per-application D3D12 as the M15 route | owner, 2026-09-28: native D3D12 through the system runtime | per-application vkd3d-proton stays the M12 path and the M15.9 performance reference (ADR 0017 point 4) |
| CPU present transport (ADR 0015) | owner, 2026-09-27 (ADR 0018) | engine Present; the CPU transport is a diagnostic fallback behind a switch |
| WDDM 2.0 pin (ADR 0008 point 2) | owner, 2026-09-27 (ADR 0019) | the newest WDK version; the move itself is still open (section 3) |
| Five-minute game trials | owner, 2026-09-30: 7-10 minute interactive sessions, then at most 7 minutes | 7-minute sessions |
