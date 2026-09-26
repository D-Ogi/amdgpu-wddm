# ADR 0017: graphics stack direction after M12 (hosted ICD for M13, system D3D11 on DXVK, native D3D12 on vkd3d-proton)

Date: 2026-09-26. Status: accepted direction. Amends ADR 0009 point 2 (candidate routes) and point 3 (how the
route is recorded); M13's acceptance criteria in the [main roadmap](../00-goal-and-roadmap.md) and the
[M13.1-M13.7 gates](../m13-accelerated-desktop-roadmap.md) are unchanged. Nothing in this ADR is a measured
result unless it cites a `facts.md` row.

## Context

ADR 0009 made a native Direct3D user-mode driver the goal of M13 and listed two candidate routes, both built on
Mesa's `d3d10umd` frontend: over Zink on our RADV, or over a gallium hardware driver with a WDDM winsys. It
left the choice to experiment and said the ADR would be amended when the owner's decision is recorded.

E34 measured the first candidate in part: a native D3D shader drawn through Zink on our RADV with exact pixels
(M531); a shared WDDM allocation imported into RADV on a second device and written by the GPU (M532, M533); a
hardware D3D device receiving runtime allocations through the runtime callbacks (M534, `hKMResource` zero,
cause unknown; the three follow-up checks of M535 leave the cause unknown but show the shared handle works
from both user-mode drivers, the CPU driver's second-device open passing and the GPU candidate's failing in
its diagnostic import path). No documented way exists for a second device to open a runtime-created back
buffer, so the two-device sharing of M532/M533 is a control, not a route.

The route that follows from those measurements, worked out in the owner's working notes and accepted there
on 2026-09-26, is one ADR 0009 did not name: a **hosted ICD**. One Vulkan device per D3D runtime device, created
inside the runtime's `CreateDevice` entry and driven through the runtime's callbacks (allocation, GPU VA,
residency, context, submission, fences, present), so that the D3D user-mode driver and the Vulkan driver share
one device session instead of two. E34 has since measured its first steps: hosted RADV devices executing GPU
fills through the runtime callbacks (M539), persistent hosted Zink screens surviving the destruction of a
sibling device (M540), and rendering into runtime-owned shared allocations without a CPU mapping of the
surface (M541). None of these is GPU composition by DWM, native Present or a lifetime proof.

The same notes settled what comes after M13, and the owner decided the points that were theirs to decide
(the decisions are dated 2026-09-26 and quoted in the working notes; this ADR carries their content).

## Decision

1. **Architecture.** One hardware implementation: our kernel-mode driver, RADV/ACO and the WDDM integration
   around them. Everything above it is an existing open implementation: no Direct3D DDI, no D3D12 engine and
   no shader compiler is written from nothing.
2. **M13 route: hosted ICD.** The D3D user-mode driver hosts a RADV device per D3D runtime device, driven
   through the runtime callbacks, with explicit callback, thread and lifetime ownership and a versioned
   private contract between the user-mode driver and the ICD. The `d3d10umd`/Zink build is the bring-up
   control and the native path of that hosted driver; the llvmpipe build of the same frontend stays the CPU
   baseline for comparison and for M13.1. A complete D3D11 rebuilt inside `d3d10umd` is not planned. Upstream
   Mesa proposes removing the `d3d10umd` frontend (its merge request was acknowledged on 2026-09-26, not merged
   on that date); the frontend and target are therefore carried in the project's Mesa fork as project code.
3. **What M13 must prove, restated, not changed.** GPU composition by DWM is proven only by all three
   together: a correct composed image with an independent readback of the primary; GPU execution and
   completed fences attributable to DWM's own composition, not to a GPU-rendered client composed on the CPU;
   and instrumented exclusion of steady-state full-frame CPU copies, including persistent CPU mappings that
   carry copies without `Lock2`/`Unlock2`. PresentMon and Present/Submit event counts are supporting context.
4. **System Direct3D 11 (proposed M14): DXVK is the engine** behind a system D3D11 DDI (owner decision). Whether
   the boundary sits at DXVK's `DxvkContext`/`DxvkDevice` layer or at its COM `ID3D11Device` objects is a
   design point of the bounded port, decided by measurement. Per-application DXVK and vkd3d-proton remain the
   M12 path and the performance reference.
5. **Native Direct3D 12 (proposed M15): vkd3d-proton through the public Vulkan API** is the default engine
   behind a native D3D12 DDI (owner decision). Feature level 12_0 is Must, 12_1 is Should, 12_2 is not a target
   (its features need RDNA2-class hardware). A bounded spike on the hosted contract confirms the engine before
   any broad DDI work: a native D3D12 device, the runtime's monitored fence imported into vkd3d-proton as a
   timeline semaphore, descriptor-heap handles passed through, residency through the runtime callbacks. If it
   fails, the written reason decides whether the DDI goes directly onto RADV internals; an own PAL-like hardware
   layer is not pursued.
6. **Licensing.** No relicensing for this plan. vkd3d-proton (LGPL) is loaded as a separate DLL. The AGPL
   virtio-d3d11 prototype stays read-only prior art; any reuse of its code is a new, specific decision. The
   project's Mesa-side changes are offered under Mesa's MIT terms in the public fork so that upstream can take
   them.
7. **Performance bounds (owner decision).** The system D3D path (Microsoft runtime plus our DDI user-mode
   driver) is bounded at 5 % of the per-application DXVK/vkd3d-proton path on the same title, settings, clocks
   and builds; this is the regression target for proposed M14. Proposed M15 gets no numeric bound until its
   first measurement. M12's requirement (fixed benchmark set within 10 % of Linux at the same shader clock, or
   the gap explained by measurement) stands. Measurements control runtime, threading, compiler cache and
   environment, report distributions and variance, and claim no perfect isolation.
8. **Threading in the hosted driver is a measurement, not an assumption.** The WDK serializes runtime entry
   per device; it grants no worker thread permission to call runtime callbacks. DXVK's pipeline workers reach
   RADV's shader memory allocation, so that boundary is designed and measured explicitly (CPU compilation on
   workers, device operations marshalled to the runtime's domain), with callback and thread identity, ordering,
   lifetime, progress and frame-time cost recorded. No option is selected here.
9. **OpenGL and OpenCL** stay as decided: Zink over WGL, clvk (ADR 0016).
10. **M14 and M15 are proposed numbers.** They enter the main roadmap's milestone table only with their exit
    criteria, by a later amendment; this ADR adopts their content, not their numbers.

## Consequences

- ADR 0009 point 2 is amended: the hosted ICD is the route under implementation for M13.3 and M13.4. Point 3
  is satisfied as far as the direction goes; the route is confirmed as M13's route when M13.3 passes with the
  evidence its gate requires, and this ADR is then amended with that evidence.
- The `hKMResource` zero of M534 is closed as an investigation (M535) and is not a blocker for the hosted
  route: the hosted driver owns the runtime allocations itself. It is not explained. It is reopened if a
  required operation depends on that output and fails with evidence attributable to it.
- Sparse resources (M12.1) and the rest of M12's acceptance are not reduced by anything here: an advertised
  tier and a game benchmark do not replace CTS, aliasing and residency evidence.
- The direction discussion, its proposals and reviews are the owner's working notes and stay outside this
  repository; only accepted content and the owner's decisions enter it, through this file and its amendments.
- Nothing here is measured beyond the `facts.md` rows cited. M13's route is not closed, M14 and M15 do not
  exist as milestones, and no performance figure is claimed.

## Source basis

- Facts M531-M536 and M539-M541 with their evidence directories (`evidence/windows/2026-09-26-E34-*`).
- Owner decisions of 2026-09-26 on DXVK, the D3D12 feature levels, vkd3d-proton, licensing, the 5 % bound and
  the integration of these notes, recorded in the owner's working notes.
- Upstream Mesa merge request "remove d3d10umd frontend" (acknowledged 2026-09-26), local copy in the
  workspace reference catalog; the project's RFC to Mesa, gitlab.freedesktop.org/mesa/mesa issue 16424.
- WDK 10.0.26100 D3D runtime callback contracts (`ref/ddi-display` in the workspace).
