# BC250 Open Test Pack

Owner-requested plan, 2026-09-24. Saved from the owner's proposal for future
implementation. This is a test roadmap, not evidence that a tool is installed,
a workload is supported, or a conformance/performance requirement has passed.
Upstream support, versions, licenses and exact commands must be checked when
pinning each dependency. The linked descriptions below are proposed uses.

The pack serves M8/M9 compute, M10 presentation, M11 stability, M12 API coverage
and M13 desktop work. It supplements the M9 DMA/startup acceptance gates; it does
not replace them with a demo or a benchmark. Correctness unlocks each next layer.

## Proposed components

| Layer | Software | Intended role |
|---|---|---|
| Identification | Vulkan-Tools: vulkaninfo, vkcube | Actual ICD/device/capabilities, then basic presentation after its WSI gate |
| Correctness | VK-GL-CTS / dEQP | Compute, memory, descriptors, synchronization, rendering and WSI regression oracle |
| Reproduction | GFXReconstruct | Small captures, replay, resource dumps and matched Windows/Linux reproduction |
| Investigation | RenderDoc | Inspect failed frames, pipelines, descriptors and buffers; not automated pass/fail |
| Compute | Canonical E14, VkFFT, vkpeak | Content hashes, numerical workloads, larger buffers, barriers and bounded long dispatches |
| Software reference | SwiftShader | Secondary CPU Vulkan oracle alongside same-unit Linux/RADV |
| Native graphics | Vulkan-Samples, Sascha Willems examples, Filament | Increasing rendering complexity and reference images |
| Multiple engines | DiligentSamples, bgfx examples | Comparable scenes through different abstractions/backends |
| API translation | DXVK + dxvk-tests, ANGLE, Zink; later vkd3d-proton | D3D/OpenGL/GLES workloads after native Vulkan acceptance |
| Applications | Godot demos, GZDoom + Freedoom | Engine/shader/streaming/present integration and final demonstrations |
| Telemetry | PresentMon CLI and KMD logs | Frame pacing and correlation with submits, fences, stalls and boot/device state |

## Gates and workload corpus

### Identification and canonical correctness

Start every run with the selected adapter, device identity, actual loaded ICD
and other graphics modules, their hashes and versions, capabilities, boot/device
generation and invocation. Reject accidental WARP, another GPU, a wrong ICD or a
silent backend fallback. Keep sensitive identifiers in private lab artifacts;
publish stable redacted unit labels rather than hardware UUIDs or serials.

Retain E14 as the canonical small compute control, with the existing Linux/CPU
hashes and deliberately wrong-shader check. A fence completion or absence of a
crash is never a content oracle. Integer outputs require exact comparison;
floating-point tests need a written tolerance and reference method.

Use selected CTS cases before benchmarks. Initial proposed selectors:

- dEQP-VK.compute.*
- dEQP-VK.memory.*
- dEQP-VK.binding_model.*
- dEQP-VK.synchronization.*
- dEQP-VK.pipeline.*
- dEQP-VK.renderpass.*
- dEQP-VK.wsi.* only after offscreen rendering passes

Resolve selectors against the pinned CTS case list and produce a concrete case
manifest. Wildcards are planning labels, not verified executable case names.
Report unsupported/skipped/not-run separately from pass. Do not call a regression
subset Khronos certification or a conformant implementation.

### Capture, replay and image oracles

Build a small deterministic GFXReconstruct corpus:

- offscreen traces, kept separate from WSI traces;
- a single-dispatch trace;
- transfer, compute and rendering in one dependency graph;
- golden buffers and images captured on the same BC-250 under Linux/RADV.

Verify portability, required features, versions and replay options for each
trace; replay is not automatically equivalent across operating systems. Retain
resource content checks and original invocation. Use RenderDoc for investigation
when a case fails, without making successful capture an acceptance condition.

Use VkFFT FFT/inverse-FFT numerical tests as well as timing. Use vkpeak for
synthetic ALU/type/dispatch pressure only: throughput output does not itself
validate arithmetic or predict application performance. SwiftShader is an
independent auxiliary oracle within its actual capabilities, not a substitute
for the same-hardware Linux reference.

### Graphics complexity

Progress from a deterministic offscreen triangle and small Vulkan-Samples or
Sascha Willems cases to Filament glTF/PBR, Diligent glTF/Asteroids and bgfx
examples such as drawstress. Pin scene assets and their licenses separately.

Use exact hashes for designed integer/buffer/image controls and specified
numerical/image tolerances for floating-point PBR. Classify unexpected black
output, missing regions, wrong channels, corrupt geometry and stale frames
separately. A legitimate black reference frame is not a failure by itself.

### Translators and applications

Only open each branch after its native Vulkan prerequisites pass:

- DXVK + dxvk-tests: focused D3D9/D3D11 cases before arbitrary games. Check native
  Windows deployment/support constraints for the pinned build.
- ANGLE: its dEQP coverage with an explicitly selected Vulkan backend.
- Zink: verify the pinned driver's requirements before OpenGL regression cases.
- vkd3d-proton: later, after its actual Vulkan features and descriptor limits;
  record the exact development/native-Windows build configuration.

Every run must prove the loaded translation/backend DLLs and selected device.
Passing with a different backend is not passing our Vulkan stack.

Proposed showcase: Diligent Asteroids, Filament glTF Viewer, GZDoom with Freedoom,
and optionally an official Godot 3D/particles demo. Use deterministic cameras,
seeds, assets and frame ranges where an image oracle is expected.

The Forge is not selected in the owner's proposal because the cited documentation
reportedly removed Windows Vulkan switching. Recheck that claim only if adding
it later; it is not a newly verified fact in this plan.

## Execution profiles

| Profile | Target duration | Proposed contents |
|---|---|---|
| smoke | 3-5 minutes | ICD identity, E14, selected CTS compute/memory, offscreen triangle; 1000 vkcube frames only if WSI is admitted |
| regression | 30-90 minutes | CTS shards, replay corpus, Vulkan-Samples, VkFFT and admitted basic DXVK/ANGLE cases |
| showcase | 10-20 minutes | Asteroids, Filament PBR, Godot particles, GZDoom/Freedoom |
| soak | 6-12 hours | Resize/alt-tab/fullscreen, swapchain recreation, memory churn, repeated process launch |
| forensic | Until first failure | One isolated case, detailed KMD log, trace/resources, immediate profile stop |

Durations are targets, not a reason to skip required cases silently. The proposed
soak profile does not shorten the roadmap's separate 24-hour M11 requirement.
PresentMon CSV should accompany admitted screen tests, with measured capture
overhead and a declared unavailable/not-run outcome if instrumentation fails.

## Runner rules

1. Confirm identities, versions, hashes, capabilities, workload prerequisites,
   operating point, temperature and owner STOP before admission.
2. A fence timeout, TDR, frozen present, unexpected black image or oracle mismatch
   ends the entire profile immediately. Do not launch another workload afterward.
   Preserve partial logs and record the last accepted/completed work. Killing a
   process is not evidence that GPU DMA stopped; recovery is a separate action.
3. Performance is eligible for publication only after the matching correctness
   controls pass. Keep instrumented captures separate from ordinary timing runs.
4. Record source commit, dependency/asset lock, SHA-256 of binaries, exact command,
   capabilities, private device identity, boot/device generation, loaded modules,
   KMD log, exit/timeout/oracle outcome, and applicable images/resources for every run.
5. Use same-unit Linux/RADV as the primary reference and SwiftShader as an
   auxiliary reference. Match models, scenes, data types, clocks and relevant
   settings; disclose remaining differences.
6. Pin dependencies and assets to immutable commits/releases and content hashes.
   Prefer current upstream when selecting a pin, but never fetch a moving latest
   during a recorded test. This is compatible with the project's bleeding-edge policy.
7. The original proposal required WSI tests to return to display-only until the
   full table was safe. Before implementing that action, reconcile it with the
   actual accepted lab profile and working full-WDDM desktop in STATE.md. Do not
   turn it into an unconditional PnP/reset after every test; recovery/rollback
   must preserve known-good state and verified GPU ownership.
8. Launch graphics applications on the lab, respecting the development-PC GUI
   rule. This saved plan does not launch tools, download dependencies or change
   the installed driver.

## First implementation and dashboard

Proposed v1 inventory: Vulkan-Tools, a selected VK-GL-CTS subset, E14,
GFXReconstruct, VkFFT, Vulkan-Samples, DiligentSamples, Filament and PresentMon.
Inventory membership does not bypass per-case gates. Translators and games are
a later layer, even when their binaries are available.

Implement the runner/manifest and canonical positive controls first, then add
one gated corpus at a time. The dashboard must show four independent axes:
correctness, stability, presentation and performance. A good-looking demo does
not mask a compute mismatch; passing offscreen CTS does not prove desktop/WSI.

## Upstream links supplied with the proposal

These are research inputs; exact revisions and support claims are not validated
by saving this document. Tracking parameters have been removed.

- [Vulkan-Tools](https://github.com/KhronosGroup/Vulkan-Tools/blob/main/vulkaninfo/vulkaninfo.md)
- [VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS)
- [GFXReconstruct](https://github.com/LunarG/gfxreconstruct)
- [RenderDoc](https://github.com/baldurk/renderdoc)
- [VkFFT](https://github.com/DTolm/VkFFT)
- [vkpeak](https://github.com/nihui/vkpeak)
- [SwiftShader](https://github.com/google/swiftshader/blob/master/README.chromium)
- [Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples)
- [Filament build notes](https://github.com/google/filament/blob/main/BUILDING.md)
- [DiligentEngine](https://github.com/DiligentGraphics/DiligentEngine)
- [bgfx](https://github.com/bkaradzic/bgfx)
- [DXVK](https://github.com/doitsujin/dxvk)
- [ANGLE](https://github.com/google/angle)
- [Zink documentation](https://docs.mesa3d.org/drivers/zink.html)
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton)
- [GZDoom](https://github.com/ZDoom/gzdoom)
- [Godot](https://github.com/godotengine/godot)
- [The Forge](https://github.com/ConfettiFX/The-Forge)
- [PresentMon](https://github.com/GameTechDev/PresentMon)
