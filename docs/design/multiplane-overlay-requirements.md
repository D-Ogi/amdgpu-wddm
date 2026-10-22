# Multiplane overlay requirements

Status: implementation in progress. No MPO capability is enabled by this work
yet. The existing native-size DirectFlip path remains the control. BD-223 covers
scaled D3D12 flip chains and Vulkan chains presented through vk-wsi-dxgi.

## Release boundary

An opaque RGB fullscreen overlay is an intermediate test, not the complete MPO
release gate. Unsupported configurations must be refused in Check before
promotion. Such refusals do not replace the documented minimum feature set.
Do not advertise a capability until the corresponding implementation and tests
exist. A correct picture and a PresentMon independent-flip classification are
both needed. Neither proves HLK compliance by itself.

## Microsoft requirements and evidence

The [MPO hardware requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-hardware-requirements)
were checked against Microsoft Learn on 2026-10-22. The local conceptual source
revision is `110f60eaf2ac5836e644d320c1e92c1011f2af5e`. Interface declarations use
WDK 10.0.26100, with this driver registered as WDDM 2.9.

| Requirement | Implementation needed | Acceptance evidence still needed |
|---|---|---|
| Overlapping and separate planes with black uncovered regions | Two actual display pipes with correct composition order | Pixel tests of overlap, separation and uncovered regions |
| Enable/disable every plane, including the desktop | Persistent per-plane state with omitted planes unchanged | Disable each plane and return to desktop |
| Premultiplied alpha on a per-plane basis | MPC blending and alpha-capable line-buffer configuration | Known alpha ramps over known background colours |
| Source and destination clipping on every plane | Checked rectangle and sampling-phase calculations | Edge crops, negative placement, overflow refusals |
| Independent shrink and stretch | DPP scaler and DML inputs for each plane | Both directions, with the other plane unchanged |
| Bilinear and better filtering | Correct coefficients, taps and bank lifetime | Pattern images and reference output for both filters |
| A YUV plane, BT.601/709, normal/full range | Plane layout, chroma sampling and colour conversion | Reference colour bars in each matrix/range combination |
| Atomic attributes and multi-plane updates | One transaction, one latch boundary | VBlank-crossing updates with no mixed frame/shape |
| Independent plane updates | Per-plane pending state and PresentId retirement | Separate update calls across a refresh boundary |
| Active-output constraints | Capabilities bound to the current topology | Output changes invalidate stale admission plans |

The [MPO caps DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_getmultiplaneoverlaycaps)
and its format/count/filter fields describe the available configurations. No
exception to the baseline requirements above was established from those fields.
Rotation and mirroring are separately reported capabilities. Leave them false
until implemented. Keep format and colour space separate: RGB10A2 SDR support
does not imply PQ, HLG or FP16 HDR support.

## Current implementation boundary

The source-mode scaler already supplies identity/upscale arithmetic and RGB
coefficient programming. `Bc250ScalerProgramLocked` adds programming within a
caller-owned OTG0 update lock. The host register model checks access failures
and that the helper never releases or triggers the caller's transaction.

`Bc250ScalerAxisPlan` now supplies checked source crops, destination clipping,
negative placement, mirror geometry, and shrink/stretch phase calculations.
The AMD viewport function and `fixed31_32` helpers are extracted without edits.
The separate Windows adapter checks dimensions and clears output on refusal.
Its host tests include exact rational comparisons, sample-position conservation,
and the DCN201 exact-8:1 ratio adjustment with intermediate 31.32 rounding.
`Bc250ScalerPlaneGeometry` computes both axes for full-resolution and 4:2:0
components. It publishes a plan only if all component calculations succeed.
Output clip coordinates need not be even. Chroma ratios use the original source
extent before division and truncation, including when that extent is odd.
Neither helper selects taps, checks bandwidth, or programs a plane.
Its mathematical mirror support does not enable a mirror capability.

Open clipping requirement: the imported AMD path requires even source origins
for 4:2:0. The adapter refuses odd origins explicitly, without rounding them.
No Microsoft exception for this restriction has been established. A supported
source-origin and chroma-phase implementation remains required before claiming
complete clipping coverage. The geometry API takes clip coordinates in active
output pixels. The DDI adapter must establish and implement its coordinate
conversion before passing rectangles to this helper.

`Bc250ScalerBufferPlan` imports DCN20 line-buffer accounting used by DCN201.
It checks luma, chroma and alpha capacity in AMD's configuration order. It
refuses a configuration when none can hold the requested vertical taps and
ratio. The existing desktop scaler now uses this check too. Mode enumeration
and programming use the same admission result.

Open filter requirement: AMD's two-tap coefficient table is ModifiedLanczos.
Two taps alone do not establish bilinear output. The DCN two-tap hardware mode
and the uploaded coefficients need separate verification against bilinear
reference images. The better-than-bilinear path also needs image tests. No
filter quality capability is established by line-buffer admission.

The host filter planner now prepares all 25 AMD 64-phase tables and a separate
linear kernel. Linear weights are exact S1.12 values for (1-t, t), t=phase/64.
The host oracle checks them against bilinear interpolation of image samples.
AMD table selection is checked at each ratio boundary. Odd tap counts receive
zero padding for the final coefficient pair. This does not program a bank or
establish phase order, image quality or filtering support on the hardware.
The MPO writer must preserve these weights instead of replacing them with an
unverified hardware two-tap mode. Hardware validation, bank lifetime and DML
stay open.

`Bc250ScalerPrepare` now combines the geometry, default/requested tap selection,
line-buffer admission and four coefficient plans in private caller storage.
It recomputes geometry after selecting the taps. A refusal clears all output.
Insufficient buffer capacity does not cause a silent filter-quality reduction.
Default taps follow the AMD DCN201 selection block. Identity uses one tap and
explicit linear mode uses two taps for each nonidentity component. This is
CPU preparation only. Bandwidth/DML, format admission, bank lifetime and the
display transaction remain open. No MPO DDI calls this preparation layer yet.

The coefficient writer now stages pipe 0 or 1 under a caller-owned OTG0 lock,
at PASSIVE_LEVEL, with coefficient memory already on. It verifies that selected
and current banks agree before staging. Every pair uses an explicit phase/pair
selector, so the upload does not depend on the old tap count. Staging does not
write SCL_MODE. The short select step must share the geometry/address transaction.

One owner record per pipe reserves a staged bank. A failed or uncertain write
poisons that record. It can be invalidated only after the display owner quiesces
the hardware. Tickets increase across invalidation to reject stale commits.
Observation releases bank ownership only after the OTG lock is clear and both
bank fields report the submitted target. This is not PresentId retirement.

These entry points have host-model coverage, not a hardware witness. The model
assumes the inactive-bank write behavior used by AMD's update sequence. The
first hardware arm must check that behavior and explicit phase/pair addressing.
All legacy scaler, stop, reset and mode writers must join the same ownership
protocol before these helpers can be called by MPO. Filter uploads belong to
filter changes, not every address-only flip. Filter reuse and the complete
plane transaction remain to be connected. No MPO capability is enabled here.

`dcn_geometry.c` prepares 29 DSCL/HUBP register words for either pipe before
the display lock. It selects RGB, YCbCr444 or YCbCr420 scaler processing,
keeps separate luma/chroma phases and viewports, and accounts for line-buffer
alpha. These processing modes do not admit a surface format or establish CSC.
The locked writer checks the OTG lock and preserves coefficient-bank fields.
It performs no coefficient upload, allocation, stall, enable or unlock.
It reports every attempted write, including a failed write that may have reached
hardware. The caller must restore the complete transaction after such a failure.

This helper is for progressive output. At 8:1, the inactive bottom-field initial
phase can exceed the four-bit integer field. Its encoding keeps the low four bits,
as AMD's register macro does. The active top phase is checked without truncation.
The host controls cover both pipes, clipping, chroma modes, the exact-8 boundary,
all read failures and each write failing before or after its effect. They do not
prove hardware latch, bandwidth, colour accuracy or scanout. Surface format,
mirror/rotation, address, routing, bank ownership and power must be integrated
by the plane transaction owner before use. These helpers are not yet in the
live MPO callback or the KMD build source list.

This is not a completed MPO path. Missing work includes the second plane,
displayable-allocation intent, UMD/router callbacks, MPO3 admission and commit,
plane-specific retirement, bandwidth validation, and all uncovered requirements
in the table. The transaction owner must restore the last coherent state after
a partial write. The helper does not implement that recovery. Coefficient work
must be prepared outside the interrupt callback, with its bank lifetime tracked.

KMD and UMD/router changes use separate commits. Every stage retains native-size
presentation, GPU composition fallback, and legacy visibility/mode/stop behavior.

## AMD implementation reference

PROVENANCE: Linux amdgpu DC, MIT, revision
`7d0a66e4bb9081d75c82ec4957c50034cb0ea449`.

DCN201 selects `dcn20_program_front_end_for_ctx` and `dcn10_lock_all_pipes`.
`dc.c` locks before committing surface updates and unlocks after them.
`dcn20_hwseq.c` programs the DPP scaler, HUBP viewport and surface address inside
that transaction. `dcn201_dpp.c` selects `dpp1_dscl_set_scaler_manual_scale`.
Port the relevant source through the shim, preserving provenance and documenting
WDDM-specific changes. Do not copy Linux's long polling into an interrupt DDI.

## HLK coverage and selection

The retained HLK 10.1.26100.8328 catalog contains these associations. They are
coverage inputs, not a claim that the SDK has selected or passed these jobs for
a new driver. IDs below are local catalog IDs. The test names identify the jobs.

| Requirement family | Associated jobs |
|---|---|
| WDDM13 MultiplaneOverlaySupport / MultiPlaneOverlayVideoProcessing | 784: DX11 VideoProcessing for Overlay. 785: DX11 Video Rotation for overlay |
| WDDM13 MultiplaneOverlaySupport | 4838-4839: CreateDecodeSwapChain For CompositionSurfaceHandle, Native/WoW64 |
| WDDM13 MultiplaneOverlaySupport | 4840-4841: CreateSwapChain, Native/WoW64 |
| WDDM13 MultiplaneOverlaySupport | 4842-4843: CreateSwapChain For CompositionSurfaceHandle, Native/WoW64 |
| WDDM13 MultiplaneOverlaySupport | 4844-4845: CreateSwapChainForHwnd, Native/WoW64 |
| WDDM22 MultiPlaneOverlay3 | 4780: iFlip Present Desktop - Multihead. 4781: iFlip Present UAP |

Microsoft's [video-processing test](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/bff94b26-0a7a-434d-8a93-333f383f2e3d)
lists the WDDM13 overlay requirements. The
[Multihead test](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/1d24afbc-dfac-4994-880c-dd5af8581471)
lists MultiPlaneOverlay3 and requires multiple outputs. Do not substitute a
single-output run or mark it passed without execution. Record its applicability
from the selected target and the kit's rules.

For the frozen candidate, rediscover target features, apply the applicable
playlist and filters, and retain the SDK-selected tests and package hashes.
Reconcile that result with this table before queueing. Any missing applicable
test remains open. Existing feature discovery from a non-MPO driver cannot
establish the new selection. A catalog association alone is not proof of an
unconditional selection rule or of the test's coverage of every hardware limit.
