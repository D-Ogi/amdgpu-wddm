# Scaled flip of plane 0

This is BD-223. It is the first part of stage C of [display-modes.md](display-modes.md) and of
[mpo.md](mpo.md): one plane, plane 0, that is smaller than the source mode and that the display hardware scales.
The code is in the lab flavour only, behind the switch `LabScaledPlane` (default 0). The release flavour does
not change.

## The problem

A game whose flip-model swap chain is smaller than the panel mode does not get independent flip. The lab example
is Cyberpunk 2077 at 1920x1080 on a 1920x1200 panel. DWM composes the chain and stretches it with the 3D engine.
The time from present to display is 49 ms, and the frames judder. At the native size the same game gets
independent flip (M15.14, [direct-flip-handshake.md](direct-flip-handshake.md)).

The KMD gives Windows no plane that can scale. `SetVidPnSourceAddress` takes only a surface of the source mode's
size (`scanout_admit.h`, `BC250_SCANOUT_GEOMETRY`). Windows can flip a smaller chain only through the
multiplane-overlay (MPO) DDIs, which say where the plane goes on the screen.

## The contract

The sources are the WDK 10.0.26100 headers (`shared/d3dkmddi.h`, `km/dispmprt.h`) and the Microsoft pages
`display/multiplane-overlay-vidpn-presentation.md` and `display/multiplane-overlay-hardware-requirements.md`
(windows-driver-docs, staging 110f60ea).

The KMD reports `WDDMVersion` 2.0 (ADR 0019 B1). The MPO DDIs of that version are the "2" family. Both are in the
WDDM 2.0 block of `DRIVER_INITIALIZATION_DATA`, before `SetVideoProtectedRegion`, so `WddmCheckReserved` accepts
them. The "3" family of [mpo.md](mpo.md) is after that block and needs a later `WDDMVersion`.

| Item | What the lab flavour does |
| --- | --- |
| `DXGK_DRIVERCAPS` | `SupportMultiPlaneOverlay` TRUE and `MaxOverlayPlanes` 1 (the count includes the primary) when bit 0 is on |
| `DxgkDdiCheckMultiPlaneOverlaySupport2` | PASSIVE_LEVEL. `Supported` TRUE only for the plane that this document admits and that DML fits into the clocks that run |
| `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2` | Up to DIRQL. Programs plane 0 through the same transaction as `SetVidPnSourceAddress` |
| `SetVidPnSourceAddress` | "The driver should disable all non-primary overlay planes": there is no other plane. The primary keeps its shape when `hAllocation` is NULL |
| VSync report | `CRTC_VSYNC` by default. With bit 1, `DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY` with one `DXGK_MULTIPLANE_OVERLAY_VSYNC_INFO` for layer 0 |
| `SetVidPnSourceVisibility` FALSE | Blanks the pipe, as today. That disables plane 0, the only plane |

The register writes of one flip post at one vertical blank: the shape and the address are written inside one
`OTG_MASTER_UPDATE_LOCK` (`dcn.c` `DcnFlipWriteSequence`). That is the atomic-post rule of the hardware
requirements page.

## The switch

`LabScaledPlane` is a REG_DWORD under the driver's Parameters key. The lab KMD reads it once at `DriverEntry`, and
`WddmStart` latches it. It is open only while the DirectFlip handshake is on for that start, because the plane
needs the flip path and the scan-out rule.

The switch is not a setting of the state catalog (`driver/state/catalog.json`), for the same reason as
`LabGdiHwAccel`. A new catalog entry changes the catalog identity, and `DriverStateInit` refuses a KMD image whose
catalog is not the catalog of the installed package. A lab arm changes only the KMD image, so the switch must not
change the catalog. The start log has a line for the read: `lab: scaled plane selector ...`.

| Bit | Meaning |
| --- | --- |
| 0 | The MPO caps and the Check answer. Without this bit, Check says "not supported" and the caps stay as before |
| 1 | The VSync goes to the OS as the MPO type |
| 2 | `MaxOverlayPlanes` 2 in place of 1. A lab question only: Check still refuses a second plane |

The start log has `wddm: scaled plane 0x<value> (LabScaledPlane 0x<value>, handshake <0|1>)`.

## What the KMD admits

`dcn_plane.c` (`Bc250PlanePrepare`) decides without register access or floating point. `wddm.c` translates the
DDI's plane into `BC250_PLANE_REQUEST`. Each refusal has a verdict number, which the log and the summary carry
(`enum bc250_plane_verdict`, append only).

| Rule | Verdict | Why |
| --- | --- | --- |
| One plane, layer 0, enabled | `count`, `layer`, `disabled` | This stage has pipe 0 only |
| The source mode is the timing's active area | `source-mode` | A source mode that stage A scales would need the post-composition step, and DCN has no scaler after the blend |
| No mirror, rotation, alpha blend, interlace or stereo | `mirror`, `rotation`, `blend`, `interlaced`, `stereo` | Not programmed here |
| sRGB colour space (`RGB_FULL_G22_NONE_P709`) | `color-space` | No input CSC is programmed |
| Stretch quality BILINEAR or HIGH | `stretch-quality` | The 4-tap polyphase upscale filter meets both |
| ARGB8888, ABGR8888 when the plane formats are open, no 10-bit | `format` | The rows of the scan-out rule (`bc250_scanout_caps.h`). DCN 2.0.1 cannot scale FP16 |
| The allocation passes the scan-out rule at its own size | `scanout` | `Bc250ScanoutAdmit`. The flip checks residency and the address again |
| 64x64 to 16383x16383 | `surface` | The plane caps of Linux `dcn201_resource.c`, and the 14-bit viewport fields |
| The pitch of the committed pipe | `pitch` | See below |
| `SrcRect` is the whole allocation | `src-crop` | A crop needs the viewport start and the init phases of a clipped source |
| `DstRect` inside the source mode, `ClipRect` contains `DstRect` | `dst-bounds`, `clip` | The `ClipRect` text says "relative to the DstRect". A clip that cuts the plane is refused, so both readings give the same answer |
| 1:1 or up, at most 16 times | `downscale`, `upscale` | `max_upscale_factor` 16000. Downscale needs DPP clock and DCHUB changes |
| The scaler plan | `scaler` | `Bc250ScalerPlanRecout` (`dcn_scale.c`) |
| DML fits the scaled pipe into the clocks that run | `dml`, `clocks` | The flip changes no clock. The DENTIST belongs to the commit |

A plane of the source mode's size at the origin is the identity plane. It is the full-size flip of today and needs
no DML.

### The pitch rule

The flip writes the viewport, the scaler and the MPCC, not the request (RQ) and delivery (DLG, TTU) registers. The
commit programmed those for the pipe's pitch (`connector_link.c`). So a plane must have that pitch. In practice the
plane then has the source mode's width, and the scaling is vertical. The lab case passes: 1920x1080 at pitch 7680
on the 1920x1200 timing is a vertical ratio of 1080:1200. A narrower chain, for example 1280x720 on 1920x1080, is
refused with `pitch` and DWM composes it as before. To remove this limit, the flip must program RQ, DLG and TTU
from the plane's DML plan inside the same lock. That is a later step.

### DML

Check runs DML201 (`ConnectorLinkPlaneDml`) at PASSIVE_LEVEL under `Connector.Operation`, with the floating-point
state saved. The input is the committed timing, output, lanes and clocks of the link, with the plane's pitch,
viewport, recout and taps. The budget is the clocks that run: the firmware's clocks for an inherited link, the
commit's DML clocks for an applied one. For the lab case the host test gives DISPCLK 156007 kHz and DPPCLK
154462 kHz, the same as the full-size pipe.

DML cannot run in the flip DDI, which can be at DIRQL. Check therefore keeps the last four approved shapes in a
table (`WddmPlaneApprove`), each with the mode stamp that was current before DML ran. The flip DDI plans the
plane again (integer only), and programs it only when the table has the same shape under the same stamp
(`WddmPlaneApproved`, a sequence count for each entry, no lock). Every program of the pipe by a mode increments
the stamp (`DcnPlaneShapeModeset`), so an approval does not survive a mode change. A plane that is not in the
table is refused with `STATUS_INVALID_PARAMETER` and counted as `unapproved`.

## The flip

`SetVidPnSourceAddressWithMultiPlaneOverlay2` converts plane 0 into a `DXGKARG_SETVIDPNSOURCEADDRESS` (segment,
address, allocation, contexts, flags, duration) and calls `WddmFlipEntry` with the shape. That is the entry of
`SetVidPnSourceAddress`: the same two entries (above and below DISPATCH_LEVEL), the same transaction, the same
writer rules and the same E5 path ([vidpn-flip.md](vidpn-flip.md) section 7.2). The transaction changes in four
points:

1. The surface checks use the shape's source size.
2. A shape needs an allocation and an open flip gate. Otherwise the answer is `STATUS_NOT_SUPPORTED`.
3. A different shape is a change, also at the same address. A full-size flip after a scaled one is a change.
4. A flip with no allocation keeps the current shape (`DcnPlaneShapeCurrent`), because its surface is the same.

`DcnFlipSourceAddressShaped` checks the shape against the source mode (`DcnPlaneShapeAdmit`) before any write.
In the acknowledged lock it writes the shape before the address (`DcnPlaneShapeLocked`, which calls
`Bc250ScalerProgramLocked`). A shape that equals the one on the pipe is not written again, so a run of game frames
writes the scaler once. Every write sequence without a shape puts the 1:1 shape back first, so the next desktop
flip, a restore or a writer always ends a scaled shape in its own update. A failed shape write counts the pipe as
scaled with no known shape, so the next flip writes its shape again.

A shape change at the same address is retired by the pending bit of that address, which the hardware clears at
the VUPDATE that also takes the shape, because both are in one lock. The BD-223 transaction audit of 2026-10-22
notes that an equal address does not prove new attributes in general. The lab check therefore compares the
shape counters with the flips, and a later step reads the scaler back.

The CPU copy of the present path (`DcnScanoutMapping`) is refused while the plane is scaled. Its copy has the
source mode's size and would run past the smaller allocation.

## Deviations from the Microsoft requirements

The HLK MPO tests are not expected to pass at this stage. These are the known gaps:

| Requirement | This stage |
| --- | --- |
| At least one plane can shrink and stretch | Stretch only. Shrink needs DPP clock and DCHUB work |
| More than one plane | Plane 0 only (`MaxOverlayPlanes` 1) |
| Any `SrcRect` | The whole allocation only |
| Any pitch of the plane | The pitch of the committed pipe only |
| MPO immediate flip | Not in WDDM 2.0. `FlipImmediate` follows the M15.14 rules of plane 0 |
| `ClipRect` that cuts `DstRect` | Refused (`clip`) |

Each refusal is a "not supported" answer of Check, so DWM composes that case as before. Only a plane that Check
approved reaches the hardware.

## What else must exist before DWM uses the plane

The KMD part alone does not give a game the scaled flip. The user-mode half is the contract in
[mpo.md](mpo.md):

- The UMD router refuses the DXGI 1.3 MPO entries (`pfnGetMultiplaneOverlayCaps`,
  `pfnPresentMultiplaneOverlay`) in `front-dxgi.cpp`. DWM asks the UMD caps before it asks the KMD. The front
  publishes them while the caps trailer has `BC250_SCANOUT_CAPS_MPO`, which the lab KMD sets with bit 0.
- The shells ask for scan-out only for a swap chain of a source mode's size. `Bc250ScanoutAdmit` refuses an
  application allocation that did not ask for scan-out (`not-requested`). `bc250_scanout_primary_rule` admits a
  1920x1080 chain on the 1920x1200 mode while the trailer has `MPO` (`bc250_scanout_mpo_geometry`). The shells
  must call it with the new header before such a chain is a scan-out candidate.

Until both lanes exist, the lab trial of this stage measures the caps, the Check calls that DWM makes, and the
absence of a regression. It cannot measure the Cyberpunk case.

## Host tests

| Test | What it proves |
| --- | --- |
| `dcn_scale_test.c` (run_modeset.ps1) | `Bc250ScalerPlanRecout` and `Bc250ScalerProgramLocked`: the register file equals the one of `Bc250ScalerProgram` for the same plan, and nothing is written outside a held lock |
| `dcn_plane_test.c` (run_modeset.ps1) | Every verdict, the lab case, and the DML verdict against the budget, with DML201 itself |
| `dcn_flip_test.c` (run_dcn_flip.ps1) | `DcnFlipSourceAddressShaped`: the plane's size, the admission before any write, the shape inside the lock before the address, and the refusals |
| `vidpn_flip_test.c` (run_vidpn_flip.ps1) | `PlaneShapeFlips` at both levels: the shape as a change, the kept shape of a NULL allocation, the end of a shape, and the refusals. Three negative controls: `shape-not-a-change`, `null-drops-shape`, `shape-without-allocation` |
| `connector_link_test.c` | The commit forgets the plane's shape before its scaler step |

The lab-flavour code of `wddm.c` (the DDI translation, the approval table and the MPO VSync report) compiles in
both flavours and has no host test yet.

## The lab plan

The lab trials are in the BD-223 hand-over to the lead (local). In short: a three-minute check with the switch at
0, 1 and 3 (the caps, the Check log lines, no regression of the desktop and of independent flip at the native
size, the shape counters, no HUBP underflow), and a Cyberpunk 2077 session of at most 1200 s after the UMD lanes
exist.
