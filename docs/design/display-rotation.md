# Display rotation

BD-247. Windows Settings offers four display orientations: landscape, portrait, landscape (flipped) and portrait
(flipped). Before this change the driver offered only the identity rotation, so Settings offered landscape alone.
This document gives the route that the driver uses for the other three orientations, what it refuses, and the lab
check. The code is `driver/kmd/display_rotation.h` and its callers in `display.c`, `display_inherited_vidpn.h`
and `wddm.c`.

## The contracts

The references are the WDK 10.0.26100 headers (`ref/ddi-display`, `d3dkmdt.md` and `d3dkmddi.md`) and the
conceptual guides in `windows-driver-docs` at `110f60ea`:

- `display/supporting-rotation-in-a-display-miniport-driver.md`: the DirectX graphics kernel subsystem uses
  non-rotated surface dimensions to create the shared primary surface. The source mode of a rotated path keeps
  the physical width and height.
- `display/optimized-screen-rotation-support.md`: smooth rotation needs two modes with the same width, height and
  stride that differ only in orientation, and `DxgkDdiUpdateActiveVidPnPresentPath` must accept a change of the
  path rotation (`DXGK_DRIVERCAPS.SmoothRotation`, which the driver already sets).
- `display/supporting-path-independent-rotation.md`: the offset values (`D3DKMDT_VPPR_IDENTITY_OFFSET90` to
  `D3DKMDT_VPPR_ROTATE270_OFFSET270`) are for clone, where one source feeds paths of different orientations.
- `d3dkmddi.md`, `DXGK_PRESENTFLAGS.Rotate`: a Blt present asks the driver to rotate the source onto the
  destination.
- `D3DKMDT_VIDPN_PRESENT_PATH_ROTATION`: the angles count counter-clockwise, as GDI counts them (`DMDO_90` is
  portrait).

The Microsoft KMDOD sample (`bdd.cxx`, `bltfuncs.cxx`) uses the same model. Its source mode is not swapped, its
flips carry no rotation, and it rotates only a Blt with `Flags.Rotate`.

## The route

Three routes were possible:

1. **Scan-out rotation in HUBP.** DCN 2.0.1 can rotate in the plane, but only a swizzled surface. A linear surface
   cannot have a vertical source scan (Linux v6.18 `display_mode_vba_20v2.c`,
   `SourceFormatPixelAndScanSupport`), and 90 and 270 degrees are a vertical scan. The primaries and the scan-out
   chains of this driver are linear ([mpo.md](mpo.md), the `Rotation` row). This route is not possible.
2. **A rotating copy by the GPU.** The GPU present producer (`WddmBuildGpuPresent`) copies row spans with CP DMA.
   It cannot transpose a rectangle. A compute copy could, but it is new work, and the compositor does not need
   it (route 3).
3. **Rotation by the compositor.** DWM is rotation-aware. On a rotated path it draws its content already rotated
   into the shared primary, which has the physical size. A flip of that primary then needs no rotation, and the
   plane scans it out as at identity.

The driver uses route 3. It does these things:

| Item | What the driver does |
| --- | --- |
| Path support | `BC250_ROTATION_SUPPORT_FILL`: `Identity`, `Rotate90`, `Rotate180`, `Rotate270` and `Offset0`, on the connector route, the inherited route and the legacy modeset route. No other offset: a full miniport that offers one on the primary path gets bugcheck 0x113 subtype 0x1C (E16 run 007). |
| Path admission | `Bc250RotationAngle`: the identity, the three rotations and the values that pin no rotation are admitted. Each offset value is refused with `STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED` or `IsVidPnSupported` FALSE. The source mode and the timing are the same as at identity. |
| Commit and smooth rotation | Each route records the committed angle in `BC250_DEVICE.Rotation` (`DisplayRecordRotation`, one guard-log line for each change). `DxgkDdiUpdateActiveVidPnPresentPath` accepts a change of rotation and programs nothing for it. |
| Primary description | `DxgkDdiDescribeAllocation` gives the rotation of the committed path in `Rotation`. The `d3dkmddi.h` comment is "Rotation for display mode". The refresh is that of the committed mode. Before this change the rotation was `D3DDDI_ROTATION_IDENTITY` on every path. In the first lab trial (rot1), dxgkrnl set the source to not visible after each rotated commit. It did not set the source visible again. A primary that does not agree with the mode can cause this (E26/M147 for the refresh) |
| Hardware caps | `DXGK_VIDPN_HW_CAPABILITY.DriverRotation` is 1 on the full table: the rotation is done by software, not by the scan-out hardware. |
| Bugcheck screen | `DxgkDdiSystemDisplayEnable` returns the logical size (width and height swap at 90 and 270 degrees). `DxgkDdiSystemDisplayWrite` maps each logical pixel onto the physical frame (`Bc250RotationWriteBlock`). |
| Cursor | No change. The driver has no hardware cursor (`MaxPointerWidth` 0), so the OS draws the cursor into the rotated desktop. |

## What the driver refuses

| Request | Answer | Reason |
| --- | --- | --- |
| Path rotation with an offset (5 to 16) | Refused at enumeration, at `IsSupportedVidPn`, at commit and at `UpdateActiveVidPnPresentPath` | One path, no clone |
| Any rotation on the display-only table (`FullWddm` 0) | Refused. The support states `Identity` only | `Bc250PresentDisplayOnly` copies rows unrotated |
| `DxgkDdiPresent` Blt with `Flags.Rotate` | `STATUS_INVALID_PARAMETER`, counted in `RotatedPresentsRefused` and logged for the first calls | Both present producers copy rows. Before BD-247 the GPU producer refused the same flags, and with both producers off the call was dropped with success |
| DirectFlip of an application chain while the path is rotated | The scan-out caps trailer clears `BC250_SCANOUT_CAPS_DIRECT_FLIP` | The plane does not rotate. The shells then compose the chain (rule "gated"), and DWM rotates it with the desktop |

The KMD flip admission (`scanout_admit.h`) is not changed. A refusal there of a flip that the compositor asked to
scan out blanks the output, so the gate is in the trailer, before the compositor asks.

## Full-screen applications

A full-screen chain on a rotated path is composed: the trailer closes DirectFlip, so DWM draws the chain into the
rotated primary. The application keeps its own back buffer size, and the compositor does the turn. Independent flip
(M15.14) is therefore not available while the display is rotated. To get it back, the flip path would need a chain
that the application drew already rotated, or a rotating copy (route 2).

The D3D11 shell passes the DXGI Blt `Rotate` value to its engine (`umd/dxvk/ddi-blt.cpp`). Whether the engine turns
the copy is not verified. The lab check below tells if a windowed D3D11 present on a rotated desktop is correct.

## Coordination with the scaled overlay plane (BD-223)

The MPO caps must keep `Rotation` 0 ([mpo.md](mpo.md)), and `DxgkDdiCheckMultiPlaneOverlaySupport3` must refuse
a plane with a rotation in its attributes, also when the path is rotated. A rotated overlay plane needs a swizzled
surface or a rotating copy, as route 1 and route 2 above.

## Host tests

| Gate | What it checks |
| --- | --- |
| `display-rotation` | `display_rotation_test.c` on the production header. It checks the angle table, the support macro and the logical size. It checks the pixel map (corners, bijection on odd sizes, inverse turn) and the rotated bugcheck copy against the map |
| `display-rotation-controls` | Seven negative controls on a copy of the header, each of which must fail. They are: clockwise portrait, an offset admitted, rotation on the display-only table, `Offset90` stated, size not swapped, stride not clipped, 180 recorded as 270 |
| `display-timing` | The inherited route: admission of the three rotations, refusal of the offsets, the recorded angle at commit and at `UpdateActiveVidPnPresentPath`, no pipe write. `DxgkDdiDescribeAllocation` gives the path rotation (1 to 4) and the unrotated size |
| `display-timing-old-describe-rotation` | Negative control: `DxgkDdiDescribeAllocation` with the old `D3DDDI_ROTATION_IDENTITY`. It must fail |
| `connector-modes` | `ConnectorPathValid` for the three rotations, the offsets and the display-only table |
| `post-display` | The rotated bugcheck screen: size, pixel map and clipping at 90, 180 and 270 degrees |

## Lab check (three minutes)

The first trial (rot1) used `CDS_FULLSCREEN`, and Windows set the mode back when the script exited, after about 3.5 s.
Each commit was accepted, and the pictures were taken after the return to landscape. The second trial (rot2) holds
each orientation for 20 s with `CDS_UPDATEREGISTRY` and always returns to landscape at the end. The guard log gives
each visibility request that changes the state, for the first 1024 calls. One trial on unit A with the KMD of this
change:

1. Install or swap the KMD. Check the SHA256 of the loaded KMD.
2. For each of portrait, landscape (flipped), portrait (flipped) and landscape, in this order:
   - set the orientation with `ChangeDisplaySettingsEx` (`DMDO_90`, `DMDO_180`, `DMDO_270`, `DMDO_DEFAULT`, with the
     width and height swapped at 90 and 270 degrees) from a script that runs in the user's session.
   - hold the orientation for 20 s. During the hold take `mon.py screenshot` and a camera frame, and read the
     visibility, present and scan-out trailer lines of the guard log.
   - call `SetCursorPos` to a known logical point (for example 100, 200), read `GetCursorPos`, and click a test
     window at that point. The window must get the click at the same client coordinates.
3. In portrait, before the return to landscape, run the full-screen flip client for 20 s
   (`tools/win/d3d12queue/scanout-trial.ps1 -Seconds 20 -Experiment scanout-flip`). Expected: the client presents
   each frame, its picture in a screenshot is turned with the desktop, and the scan-out counters of application
   buffers do not move (the chain is composed).
4. Read the guard log. Expected: one `display: rotation` line for each change, no `Present refused a rotated Blt`
   line (if there are some, CDD or a shell asked for a rotated Blt, which is the follow-up of route 2), and the
   scan-out trailer without DirectFlip while rotated.
5. Expected failures: none. A bugcheck 0x113, a TDR or a black screen fails the trial. The last step returns to
   landscape.

Result of rot2 (2026-10-29, lab KMD of this change, 24 CU, held 30 s or more):

| Orientation | Commit | Visibility after the commit | Camera | Trailer |
| --- | --- | --- | --- | --- |
| 90 | `display: rotation 0 -> 90 degrees (connector commit)` | FALSE, then TRUE 296 ms after the commit, TRUE to the end of the hold | Desktop turned, panel lit | `direct-flip 0 rotation 90` |
| 180 | `90 -> 180` | TRUE 260 ms after the commit, FALSE and TRUE again 1 s later, TRUE to the end | Desktop upside down | No new line: the answer stays `direct-flip 0` |
| 270 | `180 -> 270` | TRUE 258 ms after the commit | Desktop turned the other way | No new line: the answer stays `direct-flip 0` |
| 0 | `90 -> 0` | TRUE 274 ms after the commit | Landscape | `direct-flip 1 rotation 0` |

The pointer check passed at each angle. No `Present refused a rotated Blt` line, and no bugcheck, TDR or live kernel
report. The full-screen client step did not run: `scanout-trial.ps1` needs `controller.ps1` next to it on the lab,
and the kit did not push it. The guard log has no `DescribeAllocation` value of a rotated path, because the driver
logs only the first calls of that DDI, and those are at the start.

## Follow-up

- A rotating compute copy for `Flags.Rotate`, if the lab finds that CDD or the shells send it.
- The DWM user-mode driver (Zink `d3d10umd`) refuses a DXGI `Blt` with a rotation (`E_NOTIMPL`). In rot2 DWM did
  not need it: the rotated desktop was on the panel. Keep it in mind if another compositor path gives a black screen.
- The full-screen flip client in portrait (step 3), with `controller.ps1` pushed next to `scanout-trial.ps1`.
- DirectFlip on a rotated path, for a chain that its application drew already rotated. This needs a way for the
  shells to identify such a chain.
