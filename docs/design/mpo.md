# Multiplane overlay

This is stage C of [display-modes.md](display-modes.md) and the contract between the KMD and the user-mode drivers
for BD-223. The target is the Microsoft MPO minimum and the HLK MPO tests. The first part is code in the lab
flavour: one scaled plane 0 through the WDDM 2.0 MPO DDIs ([scaled-flip.md](scaled-flip.md)). The release flavour
reports no MPO caps until the full minimum is implemented and its lab gates pass.

With multiplane overlay (MPO), DWM gives the driver more than one plane for a source: the desktop, and on top of it
a video or a game window. The display hardware scales and blends the planes at scan-out. DWM then does not compose
these pixels with the 3D engine, and a window can get its own flips. A game chain smaller than the mode can also
flip independently on plane 0, because the plane scales it to the mode.

## Lab status

Scaled MPO is not active. In tester.34 `LabScaledPlane` is 0 by default, and the release flavour reads no selector
and has no MPO DDI. The lab results of 2026-10-29 on unit A, with the lab KMD of stage S2b:

| Arm | UMD rows | Native chain | 1280x720 STRETCH chain |
| --- | --- | --- | --- |
| Selector 0 | Installed tester.34 | Scans out (1498 of 1556 flips) | Composed. No MPO caps, as expected |
| Selector 1 or 3 | Installed tester.34 | Composed. DWM restarts its composition about every 0.4 s | Composed |
| Selector 0, 1 or 3 | Router 9fd42f83 and zink 088a1588 | Scans out (1499 of 1557, 1500 of 1546) | Composed. No `CheckMultiPlaneOverlaySupport2` call |
| Selector 5 or 7 (`MaxOverlayPlanes` 2) | Router 9fd42f83 and zink 088a1588 | Scans out (1497 of 1551, 1499 of 1574) | Composed. No `CheckMultiPlaneOverlaySupport2` call |

- The tester.34 router front refuses every DXGI overlay entry. With the MPO caps of the KMD, that refusal makes DWM
  start its composition again and again: every flip is a mode-change flip, and `SetVisibility` comes 1297 times in
  222 s. Thus the KMD caps must not go out without the user-mode half of [The KMD/UMD contract](#the-kmdumd-contract).
- With the user-mode half, the caps are harmless: one DWM per start, VSync flips, no DWM event, no underflow. The
  front publishes WDDM 1.3 (`level=wddm1_3 hosted_mpo=1`), and it refuses no entry.
- dxgkrnl does not call `DxgkDdiCheckMultiPlaneOverlaySupport2` in any arm. The first-call log of the Check is
  empty, and the check and set counters stay 0. `MaxOverlayPlanes` counts the primary surface, so 1 gives DWM no
  overlay plane. But `MaxOverlayPlanes` 2 also gives no Check call, so the KMD plane count alone is not the gate.
  The plan of plane 0 alone (step S1 in [Steps](#steps)) does not reach the KMD.
- The BD-258 device churn (9 to 14 `CreateDevice` calls a second) was present in every arm, the control arms
  included. The native scan-out passed with it, so it does not explain the missing Check.

The gates that can stop DWM before the Check, in the order of the next lab work (after tester.34):

1. The caps answer of the front. In arms 5 and 7 the caps trailer was `0x0000000b`, without
   `BC250_SCANOUT_CAPS_MPO_PLANE1`. Thus `pfnGetMultiplaneOverlayCaps` and `pfnGetMultiplaneOverlayGroupCaps` said
   1 plane while the KMD said `MaxOverlayPlanes` 2. The front logs a refusal but not an answer, so the log does not
   tell if DWM asked these entries. The next arm makes the two counts agree and logs each caps answer once.
2. The format support of the chain. The D3D12 engine never reports `D3D12DDI_FORMAT_SUPPORT_MULTIPLANE_OVERLAY`
   (`format-list.py` excludes it with `DISPLAY`). The hosted D3D11 UMD of DWM must report it for the formats of
   plane 0 as well. That is not checked yet.
3. YUV in the group caps. The Microsoft minimum asks for one plane with YUV formats and both BT.601 and BT.709
   ([multiplane-overlay-hardware-requirements.md](https://learn.microsoft.com/windows-hardware/drivers/display/multiplane-overlay-hardware-requirements)).
   The group caps report RGB only. That is step S4.

When DWM makes the Check, the KMD log keeps its first calls (`wddm: MPO check n <planes> surf <size> ... dst ...`).
If DWM sends the game chain over the desktop plane, plane 0 can take a set in which an opaque top plane covers the
whole output and the plane below it is not visible. That is a change of the Check and of the flip.

## The route

The KMD reports `WDDMVersion` 2.0 (ADR 0019). It therefore uses the WDDM 1.3/2.0 MPO model. dxgkrnl calls the KMD
DDIs of the "2" family, and the DXGI runtime calls the DXGI 1.3 entries of the compositor's UMD:

| Part | DDI | What it does here |
| --- | --- | --- |
| KMD | `DXGK_DRIVERCAPS.SupportMultiPlaneOverlay`, `MaxOverlayPlanes` | `MaxOverlayPlanes` counts the primary. 1 is plane 0 alone, and DWM then makes no Check call ([Lab status](#lab-status)) |
| KMD | `DxgkDdiCheckMultiPlaneOverlaySupport2` | Decides a set of planes at PASSIVE_LEVEL, with no register write. The only authority for a plane configuration |
| KMD | `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2` | Programs the planes of the source in one OTG update |
| KMD | VSync type `DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY` | Reports the address of each plane at the vsync |
| UMD | `pfnGetMultiplaneOverlayCaps`, `pfnGetMultiplaneOverlayGroupCaps` | Caps for DXGI, from the contract header. Advisory |
| UMD | `pfnPresentMultiplaneOverlay` | One MPO present for all planes |

At DXGI 1.3 the `pfnCheckMultiplaneOverlaySupport` slot is `pfnReserved1` (`dxgiddi.h`), so the UMD does not check
a configuration. The runtime asks the KMD. The kernel-only route of the "3" family (`CheckMultiPlaneOverlaySupport3`,
post-composition caps) needs `WDDMVersion` 2.2 or later and is out of scope.

## The KMD/UMD contract

### The caps trailer

The KMD publishes the MPO state in the scan-out caps trailer (`driver/contract/bc250_scanout_caps.h`). The trailer
keeps version 1 and 24 bytes. Two flags are new:

| Flag | Value | Meaning |
| --- | --- | --- |
| `BC250_SCANOUT_CAPS_MPO` | `0x8` | This start reports `SupportMultiPlaneOverlay` and answers the two MPO DDIs. Set only with `BC250_SCANOUT_CAPS_DIRECT_FLIP` |
| `BC250_SCANOUT_CAPS_MPO_PLANE1` | `0x10` | The KMD also scans out an overlay plane on pipe 1 (`MaxOverlayPlanes` 2). No KMD sets it yet |

The lab KMD sets `MPO` while `LabScaledPlane` bit 0 is on (the bits are in [scaled-flip.md](scaled-flip.md)).
`LabScaledPlane` bit 2 reports `MaxOverlayPlanes` 2 to dxgkrnl as a lab question, but it does not set
`MPO_PLANE1`, because no second pipe scans out. The release KMD sets neither flag.

### What the UMD reports

The compositor's front (`driver/umd/router`) publishes the WDDM 1.3 interface (`D3DWDDM1_3_DDI_SUPPORTED`) and a
DXGI 1.3 table only while the trailer has `MPO`. Without the flag, the front stays the D3D11.1 front with a DXGI 1.2
table, as before BD-223. The caps come from constants in the same header, not from the trailer:

| Field | Value |
| --- | --- |
| `DXGI_DDI_MULTIPLANE_OVERLAY_CAPS.MaxPlanes` | 2 with `MPO_PLANE1`, else 1 |
| `NumCapabilityGroups` | 1 |
| Group `NumPlanes` | `MaxPlanes` |
| Group `MaxStretchFactor` | `BC250_MPO_MAX_STRETCH_MILLI` / 1000 (16.0) |
| Group `MaxShrinkFactor` | `BC250_MPO_MAX_SHRINK_MILLI` / 1000 (4.0 from step S2) |
| Group `OverlayCaps` | `RGB`, `BILINEAR_FILTER` and `HIGH_FILTER`. `YUV` only when a KMD stage admits a YUV plane |
| Group `StereoCaps` | 0 |

The caps are advisory. The KMD check refuses every configuration outside its stage, and DWM then composes. A KMD
that changes a limit changes the constant in the same commit. `scanout_admit_test.c` fails the build when the
constants and `dcn_plane.h` disagree.

### The present

- No MPO DDI carries private driver data. The KMD reads the plane attributes that dxgkrnl gives it
  (`DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES2`). The UMD maps no attribute.
- `pfnPresentMultiplaneOverlay` makes one `pfnPresentMultiplaneOverlayCb` call for the present. The entries of
  `AllocationInfo` (`DXGIDDI_MULTIPLANE_OVERLAY_ALLOCATION_INFO`, dxgiddi.h) are the enabled planes only, in the
  order of the runtime's plane array: `PresentAllocation` is the KMD allocation of the plane's source resource,
  and `SubResourceIndex` is the plane's own subresource index. `AllocationInfoCount` is the number of enabled
  planes ("the number of allocations in the array", dxgiddi.h). A disabled plane names no resource, so it has no
  entry.
- The front does not call the hosted `pfnPresent` before the MPO callback. That is a second present to dxgkrnl. The
  hosted driver gets an MPO present entry that does the work of its `Present` for every enabled plane: the flush,
  the order on the present fence, and the allocation on demand (`Bc250EnsureSurface`). Then it makes the one MPO
  callback.

### The scan-out surface

A plane's allocation is a scan-out allocation as for an independent flip:

- Its creator asked for scan-out: the BC2A `UMD_BLOB_A_SCANOUT` flag with the v3 geometry, or an LB7A surface whose
  E26R record has `BC250_SURFACE_RESOURCE_SCANOUT`.
- It is linear, in the local segment, at a 4 KiB aligned address (`Bc250ScanoutAdmit`).
- Its format is a `SCANOUT_PRIMARY` row of the shared table (`bc250_scanout_format_admitted`).
- Its pitch is `bc250_scanout_primary_pitch(width, bytes_per_pixel)`: the row rounded up to 256 bytes.

The shells decide a scan-out primary with `bc250_scanout_primary_rule` (`bc250_scanout_primary.h`). With `MPO` set,
the rule also admits a chain that is not a mode when `bc250_scanout_mpo_geometry` holds: at least
`BC250_MPO_MIN_DIMENSION` (64) in each direction, not larger than the mode, and the mode at most 16 times the chain
in each direction. `SetVidPnSourceAddress` still refuses such a primary (`BC250_SCANOUT_GEOMETRY`). It reaches the
plane only through the MPO flip, after the KMD check approved its shape.

From step S2 the KMD takes a plane at any pitch: the flip programs the plane's own RQ, DLG and TTU registers and its
global sync from the DML plan that Check kept ([scaled-flip.md](scaled-flip.md), "The display plan of a plane").
Stage S1 refused 1280x720 on the 1920x1200 mode with verdict `pitch`. Step S2 admits it when DML fits it into the
clocks that run and its VSTARTUP and VUPDATE offset are the ones of OTG0 (verdict `global-sync` otherwise). The
shells have no pitch rule of their own.

Known limit of tester.34: a D3D11 borderless or windowed flip-model chain is a `DISPLAYABLE_SURFACE` without
`pPrimaryDesc`, and the surface record refuses `SCANOUT` without `PRIMARY` (`surface_resource_private.h`). Such a
chain cannot reach a plane, and DWM composes it. A D3D12 chain always has a `PRIMARY` heap. Tester.34 keeps the
record rule as it is: MPO is a lab-flavour feature and is off by default there. A record rule for displayable
surfaces (scan-out with a displayable bit in place of `PRIMARY`, with no video present source) is work for a later
release.

### When UNSUPPORTED ends

The UMD answers anything other than "not supported" only while the trailer has `MPO`. It has no step logic of its
own. The steps change what the KMD check admits and the constants of the header.

## The plane attributes

`WddmPlaneRequestOf` (`driver/kmd/wddm.c`) maps the attributes of a plane to the request that `dcn_plane.c` decides:

| `DXGK_MULTIPLANE_OVERLAY_ATTRIBUTES2` | Request | Present stage |
| --- | --- | --- |
| `SrcRect` | `Src` | The whole allocation only (`src-crop`) |
| `DstRect` | `Dst` | Inside the source mode, from a quarter of the source size to 16 times it in each direction (`dst-bounds`, `downscale`, `upscale`) |
| `ClipRect` | `Clip` | Must not cut `DstRect` (`clip`) |
| `Flags.HorizontalFlip`, `Flags.VerticalFlip` | `Mirror` | Refused |
| `Rotation` | `Rotated` | Identity only |
| `Blend.AlphaBlend` | `AlphaBlend` | Opaque only |
| `VideoFrameFormat` | `Interlaced` | Progressive only |
| `StereoFormat`, `StereoFlipMode`, the stereo frame fields | `Stereo` | Mono only |
| `ColorSpaceType` | `NotSrgb` | `RGB_FULL_G22_NONE_P709` only |
| `StretchQuality` | `StretchQualityKnown` | Bilinear or high: the polyphase filter meets both |
| The allocation's description | `SurfaceWidth`, `SurfaceHeight`, `SurfacePitch`, `PlaneFormat` | A scan-out allocation (`scanout`), any pitch from step S2 (stage S1: the committed pitch, `pitch`) |

Every refusal has a numbered verdict in `dcn_plane.h` (`BC250_PLANE_*`), and `dcn_plane_test.c` drives each one. The
DML step (`ConnectorLinkPlaneDml`) refuses a pipe that needs more DISPCLK or DPPCLK than the mode programmed.

## The hardware

DCN 2.0.1 has four pipes (HUBP, DPP), five MPCCs, two OTGs and two OPPs (Linux `dcn201_resource.c`). Unit A uses
pipe 0 for its one display. Pipes 1 to 3 are free.

| Plane property | DCN 2.0.1 (Linux `plane_cap`) | Microsoft MPO minimum | This driver |
| --- | --- | --- | --- |
| Planes for a source | 1 primary and 1 overlay | 2 | 1 now, 2 at step S3 |
| YUV | `nv12` and `p010` false | NV12 on one plane, BT.601 and BT.709, both ranges | Open: see below |
| Upscale | 16 times for ARGB8888 | Stretch | 16 times |
| Downscale | 4 times (`max_downscale_factor` 250) | Shrink | 4 times from step S2, if DML fits the DPP clock |
| Blending | Per-pixel alpha, global alpha | Premultiplied alpha | Opaque now, alpha at step S3 |
| Filter | Polyphase taps | Bilinear or better | The polyphase upscale filter |
| Clipping | Viewport and recout | Source and destination clipping | Whole surface now, crop and clip at step S3 |
| Rotation, mirror | Swizzled surfaces only, HUBP mirror | Not required | Refused |

Linux's DCN 2.0.1 resource refuses NV12 and P010, while DCN 2.0 admits them. The reason is not recorded in the Linux
source. The Microsoft minimum needs a YUV plane. The driver admits YUV only after a lab measurement finds a correct
NV12 plane on unit A (colour bars in each matrix and range). Until then `OverlayCaps` has no `YUV` bit, and the HLK
YUV tests are a known failure.

## Corrections from the requirements audit

The requirements audit ([multiplane-overlay-requirements.md](multiplane-overlay-requirements.md)) found these
points. They apply to every step:

- Windows `LayerIndex` 0 is the top plane. Keep it separate from the hardware pipe number, the desktop role and
  the order of the MPC tree.
- DCN 2.0.1 selects the HUBP1 surface functions through HUBP201, not through the generic HUBP2 surface path.
  Resolve each callback before its sequence is imported.
- A lock of the whole OTG is not proof of independent timing between the updates of two planes. A later update
  must not delay a flip of another plane that was accepted before it.
- An equal surface address is not proof that a new geometry, colour or plane disable took effect. The retirement
  of a flip needs the state of the whole transaction.

## The flip

- The scan-out admission and the address checks hold for each plane.
- One `OTG_MASTER_UPDATE_LOCK` holds the writes of all planes: the scaler, the viewport and the address of each
  pipe latch at one update.
- A mode set clears the plane shapes and the check approvals (`DcnPlaneShapeModeset`, the mode stamp).
- The flip state of each plane is separate: the address, the present ID, the pending flip and the vsync report.
  The present code has one state for plane 0. Step S3 makes it an array.

## Step S3: plane 1 on pipe 1

### The Linux path of DCN 2.0.1

`dcn201_init.c` sets `program_pipe` to `dcn10_program_pipe`. So DCN 2.0.1 programs a plane through the DCN 1.0
functions, not through `dcn20_program_pipe`. For each pipe of the tree, from the top:

1. The top pipe only: `optc1_program_global_sync`, then `set_vtg_params` and `blank_pixel_data`.
2. A full update: `dcn10_enable_plane`. `power_on_plane_resources` does nothing on DCN 2.0.1: the
   `dcn_2_0_1_offset.h` header has no `DC_IP_REQUEST_CNTL` and no `DOMAIN*_PG` register (regcalc: not found). Then
   `hubp_clk_cntl` (`HUBP_CLOCK_ENABLE`) and `opp_pipe_clock_control` of OPP0.
3. `dcn10_update_dchubp_dpp`, in this order:
   - `dpp_dppclk_control` and the DCCG DTO of the DPP (`update_dpp_dto`)
   - `hubp_vtg_sel` (OTG0), `hubp_setup` and `hubp_setup_interdependent`
   - `dcn10_update_dpp` (the format) and `update_mpcc` (`dcn201_update_mpcc`)
   - the scaler and `mem_program_viewport`
   - `hubp_program_surface_config` and the address
   - the HUBP unblank of a visible tree.

The firmware leaves HUBP1 blanked on VTG 1 (facts, `display.yaml`). The kit-s3p probe reads what the running KMD
leaves on pipe 1 (the HUBP and DPP clock enables, the DPPCLK1 DTO, VTG_SEL, the MPCC links). It is the positive
control for this sequence.

### What S3 adds to the driver

- The enable of pipe 1 at the first two-plane flip, in steps 2 and 3 above, inside the OTG0 lock. The disable at the
  last two-plane flip: `dcn201_plane_atomic_disconnect` (MPCC1 out of the tree), then the HUBP1 blank and the clocks
  off after the latch.
- MPCC1 above MPCC0 in the OPP0 tree, with per-pixel premultiplied alpha (`dcn_mpc.c`).
- The DML201 plan of the two planes (the grouped requestors of `bc250_dcn201_pipe.c`). The DPP clock DTO of each
  pipe is from the plan, and the DISPCLK must fit as for one plane.
- The check of two planes: `LayerIndex` order, the source crop and the clip.
- The flip state of each plane: the address, the present ID and the vsync report.
- `MPO_PLANE1` and `MaxOverlayPlanes` 2 in the lab flavour.

## Steps

| Step | What | Go when |
| --- | --- | --- |
| S1 | Plane 0 scaled up, RGB, opaque, whole surface, the committed pitch. Lab flavour, `LabScaledPlane` | Code in place. Lab: the 3-minute check of [scaled-flip.md](scaled-flip.md), then a game below the mode flips independently |
| S2 | Plane 0 at any scan-out pitch (RQ, DLG and TTU from DML at the flip) and downscale to 4 times (DPP clock) | 1280x720 and 2560x1440 chains on the 1920x1200 mode flip, no underflow |
| S3 | Plane 1 on pipe 1: RGB, premultiplied alpha, source crop and clip, the MPCC tree, the flip state per plane, `MPO_PLANE1` | A test client with an overlay chain flips on pipe 1. DWM uses the plane for a window |
| S4 | NV12 on plane 1, BT.601 and BT.709, both ranges, if the lab measurement admits it | Colour bars correct in each matrix and range. The HLK MPO jobs |
| S5 | The caps in the release flavour | The lead's lab gates pass on the release bytes |

A trial is at most three minutes, a game session at most 20 minutes.

## Prior work

Branch `feat/bd223-scaled-flip` (2026-10-22, base before integrate/tester34) holds host-tested helpers for steps S2
to S4. None of them is called by a DDI or admits hardware. They are ported onto this line with their author kept:

- Scaler geometry, taps, line-buffer capacity and coefficient banks (`dcn_geometry.c`, `dcn_coeff.c`).
- Two-plane MPCC blending (`dcn_mpc.c`) and input colour and alpha (`dcn_cnvc_color.c`).
- The linear and NV12 surface layout and its locked write (`dcn_surface_layout.c`, `dcn_surface_write.c`).
- DML201 runs for two planes, grouped requestors and mixed NV12 and RGB (`dml201_group_test.c`,
  `dml201_mpo_test.c`, `dml201_nv12_test.c`).
- A bounded lab latch probe for the update lock (`dcn_latch_probe.c`) and the requirements matrix
  (`multiplane-overlay-requirements.md`).
