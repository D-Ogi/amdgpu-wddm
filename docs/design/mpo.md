# Multiplane overlay

This is stage C of [display-modes.md](display-modes.md). It is a design. No code exists.

With multiplane overlay (MPO), DWM gives the driver more than one plane for a source: the desktop, and on top of it
a video or a game window. The display hardware blends the planes at scan-out. DWM then does not compose these
pixels with the 3D engine, and a window can get its own flips. The benefit is less GPU and memory work for each
frame, and a window that flips at its own rate.

## The DDIs

The KMD is compiled at WDDM 2.9. These are the DDIs of the "3" family that dxgkrnl calls:

| DDI | What the driver does |
| --- | --- |
| `DxgkDdiGetMultiPlaneOverlayCaps` | Returns the plane counts, the overlay caps, and the stretch and shrink factors of a source |
| `DxgkDdiGetPostCompositionCaps` | Returns the stretch and shrink factors of the post-composition step |
| `DxgkDdiCheckMultiPlaneOverlaySupport3` | Says if a set of planes and a post-composition step can be scanned out, without a register write |
| `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3` | Programs the planes of a source in one update |
| `DxgkDdiPostMultiPlaneOverlayPresent` | Optional. Lowers clocks after a configuration that needs less |

`DXGK_DRIVERCAPS` gets `SupportMultiPlaneOverlay` and `MaxOverlayPlanes`. The vsync report changes from
`CrtcVsync` to `CrtcVsyncWithMultiPlaneOverlay3`, which carries the present ID of each plane.

## The hardware

DCN 2.0.1 has four pipes (HUBP, DPP), five MPCCs, two OTGs and two OPPs (Linux `dcn201_resource.c`). Unit A uses
pipe 0 for its one display. Pipes 1 to 3 are free. Linux gives each stream at most one overlay plane
(`max_slave_planes` 1).

| Plane property | DCN 2.0.1 (Linux caps) | Proposed caps of this driver |
| --- | --- | --- |
| Planes for a source | 1 primary and 1 overlay | `MaxPlanes` 2, `MaxRGBPlanes` 2 |
| YUV planes | `nv12` and `p010` not supported | `MaxYUVPlanes` 0, `StretchYUV` 0 |
| Formats | ARGB8888 family, FP16 | The scan-out formats of `display_modes.h` |
| Upscale | 16 times for ARGB8888, none for FP16 | `MaxStretchFactor` 16.0. An FP16 plane must be 1:1 |
| Downscale | 4 times (`max_downscale_factor` 250) | `MaxShrinkFactor` 1.0 at first, 4.0 after the DCHUB step |
| Blending | Per-pixel alpha, global alpha | Opaque and alpha blend |
| Rotation | Only for swizzled surfaces | `Rotation` 0. The surfaces are linear |
| Mirror | Horizontal mirror in HUBP | `HorizontalFlip` 0 and `VerticalFlip` 0 at first |
| Filter | Polyphase taps | `BilinearFilter` 1, `HighFilter` 1 |
| Immediate flip | Flip type in HUBP | `Immediate` 0 until the vsync reports for each plane work |

No YUV plane means that MPO does not help video playback on this ASIC. MPO helps a game or a window of RGB pixels
over the desktop.

## The mapping

| WDDM | DCN |
| --- | --- |
| Plane 0 (the DWM primary) | Pipe 0: HUBP0, DPP0, MPCC0, as today |
| Overlay plane | Pipe 1: HUBP1, DPP1, MPCC1 |
| `LayerIndex` | The order of the MPCC tree: a higher layer is nearer the top |
| `SrcRect` | The HUBP viewport |
| `DstRect` and `ClipRect` | The DSCL recout and the scaler ratio (the stage A math for each pipe) |
| Blend: opaque | MPCC blend with the global alpha at full and per-pixel alpha off |
| Blend: alpha | MPCC per-pixel alpha |
| Post composition | The source to target scaling of the whole stream. DCN has no scaler after the blend, so the driver adds this step to the scaling of each plane (Linux does the same) |
| One update for all planes | `OTG_MASTER_UPDATE_LOCK` around the writes of every pipe, then one trigger |

Stage A puts pipe 0 through the scaler. Stage C uses the same plan and register sequence (`dcn_scale.c`) for each
pipe, with the pipe's register block.

## What DCHUB needs

Stage A writes no DCHUB request register. With two planes that is not safe: each pipe reads its own surface, and
the urgent and stutter watermarks, the DLG and TTU settings and the DET of each pipe depend on all planes. Linux
computes them with DML (`dml/dcn20`) and programs them in `hubp2_setup` and `hubbub2_program_watermarks`.

Stage C therefore needs:

1. DML for DCN 2.0.1 imported from Linux (MIT), not typed by hand, with a host test against Linux's numbers for
   the same configurations.
2. `CheckMultiPlaneOverlaySupport3` answers from DML: a configuration that DML refuses is not supported.
3. The DLG, TTU and watermark writes for each pipe, inside the same lock as the planes.

## The interaction with DirectFlip

M15.14 (independent flip) gives a fullscreen swap chain's buffer to `SetVidPnSourceAddress` for plane 0. The
handshake of that work is in [direct-flip-handshake.md](direct-flip-handshake.md). MPO changes these points:

- The scan-out admission (`Bc250ScanoutAdmit`) and the address checks hold for each plane, not only for plane 0.
- The flip state of each plane is separate: the address, the present ID, the pending flip and the vsync report.
  The present M15.14 code has one state for plane 0. Stage C makes it an array for each plane, and plane 0 keeps
  its present behaviour.
- With MPO, DWM can put a game in a window on an overlay plane and keep the desktop on plane 0. The independent flip
  of that window then goes through `SetVidPnSourceAddressWithMultiPlaneOverlay3`, not through
  `SetVidPnSourceAddress`.
- Stage C starts only after M15.14 is in main, and it does not change the M15.14 files until then.

## Steps

| Step | What | Go when |
| --- | --- | --- |
| C0 | Caps only: `GetMultiPlaneOverlayCaps` returns one plane, `CheckMultiPlaneOverlaySupport3` accepts plane 0 alone | The lab desktop and a game run as before, DWM logs no MPO error |
| C1 | DML import and its host test | The DML numbers equal Linux's numbers for unit A's configurations |
| C2 | The second plane, opaque, 1:1, RGB, with DML's DCHUB settings | A test client with an overlay swap chain flips on pipe 1, no underflow |
| C3 | Scaling and alpha on the second plane | The test client at several sizes. DWM uses the plane for a video in a window of RGB pixels |
| C4 | The vsync report and the flip state for each plane, immediate flips | A game in a window flips independently on the overlay plane |

Each step has its own switch, default off, until its lab trial passes. A trial is at most three minutes, a game
session at most 20 minutes.

## Open items

- The UMD DDIs of WDDM 1.3 (`pfnCheckMultiPlaneOverlaySupport`, `pfnPresentMultiplaneOverlay`) and the KMD DDIs of
  the "3" family: which ones a WDDM 2.9 driver needs for DWM to use MPO. To check against
  `multiplane-overlay-vidpn-presentation.md` and the HLK MPO tests before C0.
- The DPP clock for a downscaled plane. DML gives the value. The clock goes through the DENTIST (stage B, step B2).
