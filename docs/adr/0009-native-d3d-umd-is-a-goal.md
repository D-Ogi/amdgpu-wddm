# ADR 0009: a native Direct3D user-mode driver is a goal (M13)

Date: 2026-09-21. Status: accepted (direction only; the route is chosen when M12 closes). Supersedes point 2 of
ADR 0005, second sentence ("A native D3D UMD is not a goal"). The rest of ADR 0005 stands: Vulkan is still first,
and Direct3D applications still reach the GPU through DXVK and vkd3d-proton first (M12).

## Context

ADR 0005 accepted a desktop that composes on WARP for good: without a Direct3D user-mode driver, DWM and
everything that uses the system's Direct3D render in software, and only applications that bring their own API
(Vulkan, or DXVK/vkd3d DLLs next to the executable) see the GPU. The owner's decision of 2026-09-21: that is a
stage, not the end. The end is a BC-250 on which Windows itself is accelerated.

ADR 0005 called a native UMD "far beyond this project" on the strength of a sizing that has since been wrong by
two orders of magnitude for M0 to M6 (roadmap, "Sizing"). That is not evidence that M13 is small. It is evidence
that the sizing was never a reason.

## Decision

1. M13 is a native Direct3D user-mode driver, measured by the desktop: `D3D11CreateDevice` on the hardware
   adapter succeeds with no DLL next to the application, DWM composes on our adapter, and the desktop survives
   M11's soak on it.
2. No Direct3D DDI is written from nothing. The driver is built from an existing open implementation on top of
   what M8 to M12 have proven. Candidates known today, none of them measured:
   - Mesa's `d3d10umd` gallium frontend over Zink, over our RADV: no new hardware code at all, every layer
     already running by M12. What DDI level it implements and whether the Direct3D 11 runtime and DWM of the
     lab's Windows accept it is the first thing to measure.
   - The same frontend over a gallium hardware driver with a WDDM winsys: fewer layers, a second winsys.
   - Anything that exists by then and did not on 2026-09-21.
3. The route is chosen by experiment when M12 closes, and this ADR is amended with the choice and its evidence.
   Until then nothing in M7 to M12 is bent towards a guess about it, with one exception that costs nothing:
   E16's `-Phase d3d` records what the runtime asks a user-mode driver for, and its results are kept as M13's
   baseline.

## Consequences

- `driver/umd-stub` stops being a throwaway: it is where M13's driver will be registered, and the INF mechanics
  E16 run 2 proves (`UserModeDriverName`, the second package) are M13's first step already taken.
- The kernel-mode driver has to serve two user-mode clients with different habits (RADV's winsys and a D3D
  runtime's UMD) through one private interface. `driver/contract/` is versioned for that reason; M13 may add to
  it, never fork it.
- Direct3D 12 natively is not part of M13's criterion. DWM does not need it, vkd3d-proton covers applications
  (M12), and whether it follows is a later decision.
- Nothing here is measured.

## Scheduling amendment, 2026-09-23

The owner requested full WDDM repair immediately after M8 and a desktop sub-roadmap. M13 diagnostic
bring-up and backend feasibility work may therefore proceed before M12 closes. This supersedes the
ordering restriction in point3 and the timing clause in this ADR's status line. It does not select a
GPU backend or close M9-M12. The native hardware D3D and accelerated DWM goals remain unchanged;
[M13.1-M13.7](../m13-accelerated-desktop-roadmap.md) define measurable intermediate gates. Software
rendering is an explicitly temporary control. Backend selection still requires an experiment and ADR update.

## Route discussion, 2026-09-26

Point 2's first candidate has been measured in part: `d3d10umd` over Zink over our RADV draws a native D3D shader
with exact pixels (facts M531), reaches a shared WDDM allocation through a second device (M532, M533), and a
hardware D3D device receives runtime allocations through the runtime callbacks (M534; `hKMResource` zero, cause
unknown). The route now argued for is one point 2 did not name: a hosted ICD, one Vulkan device per D3D runtime
device driven through the runtime's callbacks, with DXVK as the engine behind the D3D11 DDI and the
`d3d10umd`/Zink build as the control. That argument lives in the workspace working roadmap,
`agent-discussion/ROADMAP.md` (R0002, review draft), and in the two root proposals it was made from. This ADR is
amended with the choice, as point 3 says, only after the owner's decision is recorded there. Nothing in this
section selects a route.
