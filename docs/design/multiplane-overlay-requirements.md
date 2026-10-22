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

`dcn_mpc.c` prepares a DPP0/1 -> MPCC0/1 -> OPP0 tree in private storage.
DCN201 defaults and blend packing come from the AMD import. The adapter selects
pixel alpha, global alpha or their combination, with premultiplied input when
requested. Both blend backgrounds are black. The remaining plane keeps
TOP_LAYER_ONLY after a removal, so its alpha still applies to black. It does not
switch to PASSTHROUGH. Both-off requires black OTG blanking from the owner.
disconnecting the output mux alone is not proof of black scanout.

The locked writer checks OTG0 and both MPCC lock selectors before any write.
The owner must reserve and arm both instances while quiesced. It preserves
unrelated control, stereo and output-rate fields. The mux is written last.
It never arms or releases locks and never waits. An uncertain write requires
the owner to restore the complete prior transaction before unlock. Host tests
cover topology changes, alpha combinations and every read/write fault boundary.
They do not establish pixel accuracy, atomic hardware latch or retirement.
This helper is not in the live KMD source list or MPO callback.

`dcn_surface_layout.c` prepares linear packed BGRA8, BGRX8, RGBA8, RGB10A2
and AMD AYCrCb8 layouts without MMIO. The last remains a preparation candidate.
The selected HUBP1 functions provide format, crossbar and pixel-pitch fields.
The adapter checks the complete pitched allocation, its subrange and the local
memory segment before MC-to-UMA conversion. It rejects address overflow,
physical addresses outside the AMD field width, misalignment and unsupported
rotation/mirroring. It retains the existing 4 KiB scanout address policy and
the AMD 32-pixel linear pitch alignment. Refusal clears the output.

These are field values, not whole-register overwrite permissions. BGRX8 records
that pixel alpha is absent, even though HUBP shares its BGRA8 encoding. The
transaction owner must configure CNVC alpha, colour conversion, DCC/tiling,
bandwidth and residency before using the plan. Imported RGBE branches that
need the absent DCN201 ALPHA_PLANE_EN field refuse in the adapter. Host layout
and range tests do not establish YUV scanout or add a format capability.
The internal AMD_AYCRCB8 identifier must not be mapped directly to DXGI AYUV
until channel ordering is established. The Microsoft
[DXGI format reference](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
defines AYUV memory bytes as V, U, Y, A through its R8G8B8A8 view.
AMD's enum name and format number alone
do not prove the same mapping. Use asymmetric chroma patterns to distinguish it.

This is not a completed MPO path. Missing work includes second-plane integration,
displayable-allocation intent, UMD/router callbacks, MPO3 admission and commit,
plane-specific retirement, bandwidth validation, and all uncovered requirements
in the table. The transaction owner must restore the last coherent state after
a partial write. The helper does not implement that recovery. Coefficient work
must be prepared outside the interrupt callback, with its bank lifetime tracked.

KMD and UMD/router changes use separate commits. Every stage retains native-size
presentation, GPU composition fallback, and legacy visibility/mode/stop behavior.

### DML topology controls

The existing `Bc250Dml201Plan` admits one progressive full-size RGB8 pipe.
It is not an MPO bandwidth planner. The imported DML20 numerical code can
process more pipes, but their geometry and topology must describe the actual
prepared display transaction.

`driver/shim/test/dml201_mpo_test.c` exercises that code with synthetic inputs.
The fixture uses a 1920x1200 output, a 154 MHz pixel clock, and linear RGB8
surfaces. Two pipes share OTG0. Separate controls change the OTG and DPP limits.
The source mapping follows `dcn20_populate_dml_pipes_from_context` and the
watermark/register calculation follows `dcn20_calculate_wm` and
`dcn20_calculate_dlg_params` in the pinned AMD source.

The one-pipe fixture computes an urgent watermark of 5.36 us. Adding a second
native-size plane changes it to 6.16 us. A one-pipe watermark is therefore not
a substitute for this two-pipe calculation. Scaling a 2400-line source to
1200 output lines also changes that pipe's line-delivery field. These results
use AMD's default SoC bounds, not measured memory latency or available clocks.

The model admits two pipes sharing one timing and rejects them with only one
DPP available. It rejects separate OTGs under a one-OTG limit. Increasing that
limit to two admits the separate-output control. Upscale and downscale cases
produce per-pipe DLG, TTU and RQ values without an AMD assertion.

The topology test includes the pinned numerical implementation directly.
`Bc250Dml201PlanGroup` binds one or two prepared scaler plans to the same
calculation. Each plane supplies its surface size, pitch and original source
extent. Viewport, clipped output extent, quantized ratio and final taps come
from `Bc250ScalerPrepare`. The group retains separate RQ/DLG/TTU and DPP clocks,
with common watermarks and global clocks. It rejects plans that require a split
or ODM. It clears failed output so no partial plan becomes available.

This boundary covers linear RGB surfaces with 32-bit storage, including RGB10A2
storage. The output timing still describes 8-bpc RGB. It does not admit HDR,
YUV, compression, rotation or mirrors. The caller must check that the format and
transaction match these restrictions. Zero enabled planes require the separate
blank/drain path. AMD's cursor budget is retained on each RGB pipe until the
hardware placement is established. These are implementation bounds, not a
reduction of the Microsoft requirements above.

`dml201_group_test.c` uses the public adapter with real scaler preparation.
It compares one native plane with the existing planner, then adds a second
plane, both scale directions, AMD and linear filters, a clipped surface with
a nonzero source origin, and cursors. Invalid bounds, taps, pitch, subsampling,
workspace and pipe counts clear the output. The raw topology test remains a
separate control. Neither test establishes measured memory latency, available
clocks, hardware register acceptance or HLK results. The transaction owner,
clock/service contract and DDI integration still require implementation and
hardware tests before application of these plans.

### Blanked requestor setup

`Bc250Dcn201GroupProgramBlanked` maps the computed group to HUBP0/HUBP1 and
programs each requestor, then the shared watermarks once. It rejects invalid
fields and inconsistent output timing before MMIO. Both physical HUBPs must
report blank, TTU disabled and no outstanding requests. Current DISP/DPP clocks
must meet both planes' requirements. The function leaves clocks and locks
unchanged and never unblanks a plane. The caller owns the stopped/locked OTG,
the memory-service clock contract and complete replay after a write failure.

The remap comes from original AMD register names through regcalc. The generator
checks all corresponding field masks. Host tests use the production display
accessors, reverse the plane-to-HUBP mapping, distinguish the two scaling plans,
and inject failures before and after every I/O operation. Failure leaves the
model blank and stops writes. This does not establish physical drain, power
state or latch behavior. The function is for setup/recovery. It does not replace
the independently latched live MPO update path required above.

## AMD implementation reference

PROVENANCE: Linux amdgpu DC, MIT, revision
`7d0a66e4bb9081d75c82ec4957c50034cb0ea449`.

DCN201 selects `dcn20_program_front_end_for_ctx` and `dcn10_lock_all_pipes`.
`dc.c` locks before committing surface updates and unlocks after them.
`dcn20_hwseq.c` programs the DPP scaler, HUBP viewport and surface address inside
that transaction. `dcn201_dpp.c` selects `dpp1_dscl_set_scaler_manual_scale`.
Port the relevant source through the shim, preserving provenance and documenting
WDDM-specific changes. Do not copy Linux's long polling into an interrupt DDI.

### DCN201 surface dispatch audit, 2026-10-22

The generic DCN20 surface functions are not the selected DCN201 implementation.
Resolve the function table before importing a register sequence. Paths below
are relative to `drivers/gpu/drm/amd/display/` at the revision above.

| Selected path | Source | Consequence |
|---|---|---|
| Surface configuration | `dc/hubp/dcn201/dcn201_hubp.c:44-58` | Calls HUBP1 DCC, tiling, size and pixel-format functions |
| Surface address | `dc/hubp/dcn201/dcn201_hubp.c:118-122` | Selects `hubp1_program_surface_flip_and_addr`, not HUBP2 |
| Address space | `dc/hwss/dcn201/dcn201_hwseq.c:85-150` | Converts MC addresses to UMA before HUBP programming. Our owner must use checked segment conversion |
| Address commit order | `dc/hubp/dcn10/dcn10_hubp.c:374-469` | High address first. For progressive video, chroma precedes luma. Primary luma low is last |
| Surface pitch | `dc/hubp/dcn10/dcn10_hubp.c:163-201` | Counts pixels minus one. Zero chroma pitch is explicitly unsafe |
| Format and CSC | `dc/dpp/dcn201/dcn201_dpp.c:44-188` | CNVC format, alpha and input CSC are separate from HUBP format |
| CSC bank selection | `dc/dpp/dcn10/dcn10_dpp_cm.c:489-572` | Generic code alternates ICSC/COMA. DCN201 resource initializers do not supply its debug field or COMA registers. See the correction below |
| Transaction lock | `dc/hwss/dcn201/dcn201_hwseq.c:525-553` | Locks the top pipe's TG. Triple-buffer mode uses different lock callbacks |

Two source findings change admission assumptions:

- `dc/resource/dcn201/dcn201_resource.c:574-597` advertises NV12 and P010 as
  false. `amdgpu_dm/amdgpu_dm_plane.c:800-803` uses those flags to omit the
  formats from DRM plane enumeration. The corresponding cases in common HUBP
  and DPP code therefore do not establish a supported Linux scanout path.
- `hubp201_program_surface_config` accepts rotation and horizontal-mirror
  parameters but does not program them. A generic HUBP1/2 rotation helper is
  not evidence that DCN201 uses it. Mathematical mirror geometry remains
  separate from hardware support.

Both restrictions already appear in AMD's
[2021 cyan_skillfish submission](https://www.spinics.net/lists/amd-gfx/msg68663.html).
The current upstream resource file also retains the two false format flags.
No inspected source explains their cause. This does not prove a silicon limit.

Packed AYUV has format cases in the selected HUBP1 and CNVC code. It is a
candidate for the YUV investigation, not an established substitute for NV12.
The generic DML fallback to `dm_444_32` is not a checked AYUV bandwidth plan.
Neither a hardware image witness nor complete Windows format/HLK coverage
exists for this route. The Microsoft YUV requirement remains open.

The checked linear layout, locked surface writer and CPU CNVC preparation now
exist. Complete CSC with explicit matrix-bank ownership and the full transaction.
Keep rotation, mirroring and unverified YUV formats out of capability admission.
Retain their implementation and validation work in the requirement matrix.

### Locked linear surface writer

`dcn_surface_write` captures the selected HUBP1 DCC, tiling and address functions.
It combines them with the checked pitch, format and crossbar fields. Device
topology counts come from the owner and must be powers of two that fit their
fields. They are not inferred from the surface size. The plan is linear,
uncompressed, non-TMZ and mono. The packed YUV format remains a test candidate.

The writer requires an active OTG0 with its master lock acknowledged. HUBP must
select VTG0 and acknowledge that lock. An earlier pending flip, triple buffering
or a separate surface lock causes refusal before any write. All 13 reads precede
the 13 writes. Address high precedes address low, which is last.
Repeated writes to one register use the result of the preceding planned write.
Thus the TMZ update cannot restore DCC bits from an older snapshot.

The helper does not change enables, power, lock selectors or locks. The owner
must keep residency, geometry, CNVC/CSC and DML/requestor state consistent.
Any attempted-write failure needs whole-transaction restoration before unlock.
The write count includes an operation whose callback failed after a hardware
write. Success proves neither latch nor scanout retirement. A hardware witness,
complete transaction/recovery and Windows DDI integration remain required.

### Transaction scheduling and recovery

The selected AMD `dcn201_pipe_control_lock` uses the timing generator's master
lock. DCN201 binds its normal lock/unlock functions to `optc1_lock` and
`optc1_unlock`. The lock selects the TG, sets the request and waits for its
acknowledgment. It is not a separate lock for each Windows plane.
The triple-buffer path adds VUPDATE keepout. It is a different protocol,
not an automatic solution for independent updates.

`driver/kmd/test/mpo_transaction_model.py` checks those source bindings and
tests an explicit double-buffered state model. In that model, a global lock
held across a boundary can delay an earlier accepted flip on the other plane.
This is a counterexample to the proposed scheduling policy, not a measured
DCN timing fault. The Microsoft hardware requirements above require separate
calls to retain independent update timing, as well as atomic multi-plane calls.

The owner must check pending work before taking the global lock for a later
request or coefficient upload. A candidate policy returns
[STATUS_RETRY](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddresswithmultiplaneoverlay3)
before changing hardware when the lock would delay accepted work. It preserves
the earlier flip in the model. Windows scheduling, starvation and independent
plane acceptance still need validation. A retry policy is not an HLK pass.
Preparation and coefficient staging must not hold the global lock across an
earlier accepted flip's boundary. All writers must share the ownership record.

The owner must retain the last coherent state and all old/new allocation
references before the first attempted write. A write error may follow a hardware
effect. Restoring only that register, or returning an error and unlocking, is
insufficient. The model finds mixed states at these failure boundaries.
Complete restoration before unlock works in the model, which assumes every
field is buffered. Actual register buffering, address triggers, disconnect
drain and rollback side effects remain unverified. Do not turn this model into
a generic MMIO undo log. If recovery fails, retain both allocation sets and
enter the display owner's quiesce/recovery path. Do not report completion.

Retirement needs evidence for the whole changed state, not only matching surface
addresses or a coefficient-bank observation. Same-buffer geometry/color changes
and plane disable requests are negative controls. A frame boundary and coherent
hardware observation must be associated with the submitted generation. The
hardware predicate for that association remains open.

The WDK defines PresentId as a per-plane identifier. Preserve its association
with LayerIndex. Do not compare identifiers from different planes to infer
completion. The
[plane structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_multiplane_overlay_plane3)
defines LayerIndex zero as the top plane. The internal MPC ZOrder uses the
opposite direction. The DDI adapter must map these explicitly, retain omitted
planes and report completion per plane. Neither that adapter nor the complete
transaction owner is implemented by this model.

### Disconnect and allocation retirement audit, 2026-10-22

The selected Linux path separates disconnect, drain and power-down. Paths
below are relative to `drivers/gpu/drm/amd/display/` at the revision above.

| Operation | Selected source | What it establishes |
|---|---|---|
| Disconnect | `dc/hwss/dcn201/dcn201_hwseq.c`, `dcn201_plane_atomic_disconnect` | Removes the MPCC and records disconnect pending. This is not completion |
| Wait before disable | `dc/hwss/dcn10/dcn10_hwseq.c`, `dcn10_plane_atomic_disable` | Waits for disconnect before disabling HUBP and DPP clocks |
| Disconnect observation | Same file, `dcn10_wait_for_mpcc_disconnect` | For an enabled TG, checks MPC idle before calling HUBP blank |
| MPC idle callback | `dc/dcn201/dcn201_mpc.c`, `dcn201_mpc_funcs` | Selects `mpc2_assert_idle_mpcc`, not the MPC1 implementation |
| MPC idle condition | `dc/mpc/dcn20/dcn20_mpc.c`, `mpc2_assert_idle_mpcc` | Accepts MPCC_DISABLED, otherwise polls MPCC_IDLE |
| HUBP drain | `dc/hubp/dcn10/dcn10_hubp.c`, `hubp1_set_blank` | Sets blank and TTU disable, then polls HUBP_NO_OUTSTANDING_REQ when the control register reads nonzero |

`dc/hwss/dcn201/dcn201_init.c` binds the disconnect, wait and disable callbacks
above. `dc/hubp/dcn201/dcn201_hubp.c` binds `hubp1_set_blank` and has no
`hubp_disconnect` callback. Do not infer an earlier TTU-disable operation from
the optional callback in the disconnect function.

These source sequences narrow the disable experiment. They do not prove that
MPCC idle alone releases every old surface reference. The HUBP drain is a
separate step. The zero-register exception in Linux is a power-gating workaround,
not permission to treat an uncertain MMIO read as successful retirement.
The imported void wait functions do not supply a checked Windows completion
result. A Windows adapter needs explicit timeout/error handling and retained
allocation references on failure. Long Linux polls do not belong in the
interrupt callback.

DCN201 also selects `hubp1_is_flip_pending`. That function checks the pending
bit and compares the earliest-in-use address with the requested address.
It returns false for a software-marked power-gated HUBP. It does not check
geometry, color, a Windows PresentId, or the complete disable/drain sequence.
Use it only within its established address-flip scope. L48 requires separate
hardware controls for attribute-only updates and disable retirement.

### CSC source-contract correction, 2026-10-22

The first audit resolved the function call but not its resource initializers.
`dpp201_cnv_setup` calls `dpp1_program_input_csc`. The latter uses the DCN10
debug field `CM_TEST_DEBUG_DATA_ID9_ICSC_MODE` to choose ICSC or COMA.
The DCN201 resource table expands `TF_REG_LIST_DCN20`, not
`TF_REG_LIST_DCN10`. It supplies the primary ICSC registers but no COMA
registers. Its shift and mask initializers omit both debug-field macros.
The omitted fields are present in the common structure prefix and have zero
values in these static objects. Thus the generic alternating-bank algorithm
alone does not establish an alternating-bank path for DCN201.

DCN20 has a different function, `dpp2_program_input_csc`. It uses ICSC_B and
the debug field `CM_TEST_DEBUG_DATA_ICSC_MODE` (shift 3, mask 0x18).
DCN10 uses shift 4 and mask 0x30. The DCN201 register header has ICSC_B names
but no COMA names. Its selected resource initializer and register structure
omit the separate DCN20 bank-B append list. Do not substitute ICSC_B for COMA
or copy the DCN10 debug mask into a DCN201 writer without further evidence.

`driver/kmd/test/dcn201_csc_contract.py` expands the original macros with
Clang. It reads the selected resource initializers and checks their fields
and common structure prefixes. Explicit DCN10 and DCN20 debug/append macros
are positive controls. This host check performs no MMIO and proves no hardware
limitation. The unvalidated COMA writer was removed before integration.
Linux wishlist L47 now requires a bank-status control and frame-boundary
observation before a hardware CSC bank writer is admitted.

### CNVC preparation and color arithmetic

`dcn_cnvc_color` captures the selected `dpp201_cnv_setup` function without
MMIO. It prepares format, alpha, zero expansion and the original input CSC
coefficients. RGB uses CSC bypass. The internal packed YUV candidate requires
an explicit 601 or 709 choice, each with full or limited range. Mismatched
format and color-space requests fail. Unknown values do not select a default.
BGRX disables pixel alpha. RGB10A2 writes all four two-bit alpha levels:
0, 85, 170 and 255. The owner must match these with HUBP and MPC state.

The capture boundary defers OBUF power and CSC bank operations. It does not
run the debug-index read or select a bank. The original DCN201 function ignores
its custom-matrix argument. This adapter does not expose that argument.
The selected DCN201 function table also sets `dpp_program_bias_and_scale`
to NULL. Do not assume that a generic BNS helper corrects input normalization.

The exact-rational host analysis compares the prepared matrices with the
[Microsoft conversion equations](https://learn.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering)
for limited-range 601 and 709. The full-range comparison uses an explicit
Y/255 and (chroma-128)/255 convention. Both comparisons model AMD input as
Cr,Y,Cb divided by 256, signed S2.13 coefficients, and clamped output times 255.
These input and output normalizations are hypotheses for the hardware path.

Across 1000 sampled input triples per matrix, that model gives maximum sampled
errors of 1.011658 output codes for both full-range matrices, 0.266112 for
601 limited and 0.286546 for 709 limited. Full-range neutral white is about
254, not 255. Each neutral ramp is monotonic. These are finite CPU results,
not bounds over all possible inputs and not hardware or HLK color passes.

Before YUV admission, measure neutral black/white and asymmetric chroma
patterns through the complete CNVC/CM/MPC path. Establish channel ordering,
expansion, clamp/rounding and any downstream normalization. Preserve the
original matrices until that evidence identifies which operation needs a
change. Active-bank observation, pending-selection ownership, no reuse before
latch, and atomic RGB/YUV transitions still require implementation and tests.

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
