# Hardware pointer

Before this change the driver reported no hardware pointer (`DXGK_DRIVERCAPS.MaxPointerWidth` 0), so dxgkrnl drew
the mouse pointer into each frame of the desktop. This document gives the route that the driver uses to show the
pointer on the DCN 2.0.1 cursor of pipe 0, what it refuses, and its life cycle. The code is
`driver/kmd/dcn_cursor_shape.h` (the conversion and the placement, plain C) and `driver/kmd/dcn_cursor.c` (the
surfaces, the registers and the life cycle), with hooks in `connector_link.c`, `modeset.c`, `display.c`, `dcn.c`,
`pnp.c`, `connector_buffer.c` and `wddm.c`.

## The contracts

The references are the WDK 10.0.26100 headers (`d3dkmddi.h`) and the DDI pages of `windows-driver-docs-ddi`:

- `DXGK_POINTERFLAGS`: Monochrome (0x1), Color (0x2), MaskedColor (0x4). A colour pointer is always A8R8G8B8.
- `DXGKARG_SETPOINTERSHAPE`: a Monochrome shape is a 1 bpp AND mask of Width x Height, then a 1 bpp XOR mask of
  the same size, each row Pitch bytes. A Color shape is 32 bpp ARGB. A MaskedColor shape is 32 bpp ARGB with the
  mask in the alpha byte: 0 replaces the screen pixel with the RGB value, 0xFF XORs the RGB value into the screen
  pixel. The page says that `XHot` and `YHot` "can be ignored by drivers that control hardware".
- `DXGKARG_SETPOINTERPOSITION`: X and Y are the column and the row from the top left of the source. The
  `d3dkmddi.h` comment says that they give the top left pixel of the pointer image. With `Visible` 0 the driver
  ignores X and Y.
- `DxgkDdiSetPointerShape` and `DxgkDdiSetPointerPosition` never run at the same time. Either one can run at the
  same time as any other DDI. The shape DDI runs at PASSIVE_LEVEL and is pageable. The position DDI must not wait
  or allocate.

The DCN programming comes from AMD DC (MIT): `hubp2_cursor_set_attributes`, `hubp2_cursor_set_position`,
`hubp2_get_lines_per_chunk`, `dpp2_set_cursor_attributes`, `dpp1_set_cursor_position`, `mpc1_cursor_lock` and
`dcn10_set_cursor_position`. The negative position rule comes from `amdgpu_dm` `get_cursor_position`.

## Decisions

| Item | Decision | Reason |
| --- | --- | --- |
| Size | `MaxPointerWidth` and `MaxPointerHeight` 256 | DCN 2.0 takes 256: `CURSOR_WIDTH` is 9 bits, `CURSOR_PITCH` has a 256 setting, `hubp2_get_lines_per_chunk` has a row for 129 to 256, and the DCN 2.0 caps give `max_cursor_size` 256. The 8-bit hot spot fields cover a 256 pixel image. A pointer of 64 would send every larger accessibility pointer back to dxgkrnl. |
| Formats | Monochrome, Color, MaskedColor | All three convert to one 32-bit surface (below). |
| Color | `CURSOR_MODE` 3, straight alpha | Windows gives straight (not premultiplied) ARGB. Setting bit 1 selects mode 2 (premultiplied) for the A/B check. |
| Monochrome, MaskedColor | Straight alpha when no pixel inverts the screen. Else `CURSOR_MODE` 1 (`COLOR_1BIT_AND`) | Most system pointers have no XOR pixel and need no AND-mask semantics of the hardware. In mode 1 the alpha byte is the AND bit (0xFF keeps the screen pixel) and the RGB bytes are the XOR value. |
| Position | X and Y are the top left pixel | The header comment and the `XHot` remark. Setting bit 2 subtracts the hot spot instead, for the A/B check. |
| Negative position | `CURSOR_X_POSITION` 0, and the part outside goes into `CURSOR_HOT_SPOT_X` (the same for Y) | The register fields are unsigned. This is the rule of `amdgpu_dm` and `dcn10_set_cursor_position`. |
| Edges | The cursor is disabled when it is fully outside the viewport | `hubp2_cursor_set_position`. |
| Rotation | The driver turns the image and maps the position into the physical frame | The scan-out pipe does not rotate (`docs/design/display-rotation.md`). DWM draws the turned desktop into a primary of the physical size, so a pointer in the logical frame must turn the same way. The turn is that of `Bc250RotationMapPixel`. |
| Scaled plane | A multiplane overlay plane that `dcn.c` scales (`DcnPlaneShape`, `BC250_SCALING_PLANE`) maps the source position into the plane surface. The source-mode scaler of `modeset.c` changes only the destination offset ratio. | `dcn10_set_cursor_position` and `hubp2_cursor_set_position`. |
| Source | Source 0 only | The driver has one pipe. |

## Surfaces and tear

`DcnCursorPrepare` (StartDevice, after `ConnectorBufferPrepare` and before `WddmStart`) puts two surfaces of
256 KiB each after the connector's handoff buffer, aligned to 64 KiB, below `BC250_VRAM_TOP_RESERVED`. It moves
`DisplayReservationEnd` past them, so no VidMm segment includes them. It also allocates one nonpaged copy of the
input shape (256 KiB). If any step fails, the caps report no hardware pointer and dxgkrnl draws the pointer.

A shape change draws into the surface that the registers do not name. That surface can still be in use for one frame
after the previous change, until VUPDATE latches the address. The driver waits until
`CNVC_CUR0_CURSOR0_CONTROL.CUR0_UPDATE_PENDING` reads 0, or 100 ms passed since the change, or the cursor is off.
The wait is 120 ms at most, in 1 ms steps outside the spin lock. A timeout is counted and the draw continues.

Each register batch runs inside the cursor update lock (`CUR_VUPDATE_LOCK_SET0`), so the hardware latches all of a
batch at one VUPDATE. The lock is released on every path.

## Registers

All writes go through `MmioDcnWriteEx` with the names on `g_MmioDcnWriteAllow` (`gen_regs.py`
`DCN_CURSOR_WRITE_REGISTERS`). `check_dcn_write_sites.py` reads each site, which includes the wrapper that reads, changes and writes back one register,
`CursorUpdate`.

| Register | Fields that the driver writes |
| --- | --- |
| `CURSOR0_0_CURSOR_SURFACE_ADDRESS`, `_HIGH` | The physical address of the surface |
| `CURSOR0_0_CURSOR_SIZE` | Width and height of the image (swapped at 90 and 270 degrees) |
| `CURSOR0_0_CURSOR_CONTROL` | `CURSOR_ENABLE`, `CURSOR_MODE`, `CURSOR_2X_MAGNIFY` 0, `CURSOR_PITCH` 2 (256), `CURSOR_LINES_PER_CHUNK` |
| `HUBPREQ0_CURSOR_SETTINGS` | `CURSOR0_DST_Y_OFFSET` 0, `CURSOR0_CHUNK_HDL_ADJUST` 3 |
| `CURSOR0_0_CURSOR_POSITION`, `_HOT_SPOT`, `_DST_OFFSET` | The placement |
| `CNVC_CUR0_CURSOR0_CONTROL` | `CUR0_ENABLE`, `CUR0_MODE`, `CUR0_EXPANSION_MODE` 0, `CUR0_ROM_EN` 0 |
| `CNVC_CUR0_CURSOR0_FP_SCALE_BIAS` | Scale 0x3C00 (FP16 1.0), bias 0 |
| `CUR_VUPDATE_LOCK_SET0` | `CUR_VUPDATE_LOCK_SET` 1, then 0 |
| `HUBPREQ0_DCN_CUR0_TTU_CNTL0`, `_CNTL1` | The cursor TTU of the DML plan, only on the inherited timing (below) |

`CNVC_CUR0_CURSOR0_COLOR0` and `COLOR1` (the monochrome colours) are on the write list but the driver does not
write them: it does not use mode 0. `CURSOR0_0_CURSOR_MEM_PWR_STATUS` is on the read list for the lab readback.

## The DML plan

AMD's `amdgpu_dm` plans each RGB pipe with a 256-wide 32-bit cursor. The connector (`connector_link.c` `Plan`)
does the same when the hardware pointer is on (`DcnCursorPlanWidth`). A timing that DML refuses with the cursor is
planned again without it. That mode is still offered, without a hardware pointer on it (the shape DDI refuses
shapes there). `Bc250Dcn201PipeApply` writes the cursor TTU of the plan with the rest of the requestor. For the
inherited timing (the firmware's pipe), the connector does not program the requestor, so `DcnCursorPipeReady`
writes the two cursor TTU registers from the plan, while the cursor is off.

## Life cycle

| Event | What the cursor does |
| --- | --- |
| Start | `DcnCursorPrepare` allocates. The caps report a pointer only on the full WDDM table with DCN writes and VidPN flips. A pipe with a cursor in its plan must also have become ready (`EverReady`). |
| Mode set (connector) | `Apply` calls `DcnCursorPipeGone` before the OTG goes off and `DcnCursorPipeReady` at the end. The stored shape is drawn again for the new pipe, at the last position. |
| Mode set (legacy route), rotation | `ModesetCommit` and `DisplayRecordRotation` call `DcnCursorRefresh`: a new angle draws the image again, else the cursor is placed again. |
| D3 and the return to D0 | `ConnectorLinkSuspend` calls `DcnCursorPipeGone`. The resume applies the mode (connector route) or calls `DcnCursorPipeReady` on the inherited pipe. |
| Detach | `DisableOwned` calls `DcnCursorPipeGone`. |
| Bugcheck screen | `Bc250SystemDisplayEnable` calls `DcnCursorOffQuiet` (any IRQL, no lock, no log) before it restores the firmware surface. |
| TDR, reset | `Bc250ResetDevice` calls `DcnCursorOffQuiet`. |
| Handoff of the firmware surface, stop | `Bc250StopDeviceAndReleasePostDisplayOwnership`, `Bc250StopDevice` and `DcnStop` call `DcnCursorStop`: the cursor goes off, and nothing programs it again until the next start. One summary line goes to the guard log. |
| Unload | `ConnectorBufferRelease` calls `DcnCursorRelease`: stop, unmap, free. |

`DcnCursorSetShape` refuses a shape when no pipe is ready (`STATUS_DEVICE_NOT_READY`) or when the shape is out of
range (`STATUS_NOT_SUPPORTED`): more than 256 pixels, a pitch that does not hold one row, a reserved or second
format flag, or more than 256 KiB of input. A refusal is logged (the first 32 lines). The intent is that dxgkrnl
then draws the pointer itself.

## Locks and IRQL

The order is: the connector's transaction, then `ShapeLock` (a fast mutex), then `Lock` (a spin lock). The shape DDI
takes `ShapeLock` and `Lock`. The position DDI takes `Lock` only, writes the registers and returns. It does no wait,
no allocation and no log line, and it always returns `STATUS_SUCCESS`: a register failure is counted
(`BatchFailures`). `DcnCursorPipeGone` takes `Lock` only, so a mode change never waits for a shape. The VRAM draw runs
under `ShapeLock` only.

## The setting

`EnableHardwarePointer` is a REG_DWORD in the Parameters key of the driver service
(`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`). The driver reads it once in `DriverEntry` and does
not write it. When the value is absent, cannot be read or has another type, the driver uses 1. A change takes effect
at the next start of the driver. The useful values are 0, 1, 3, 5 and 7:

| Bit | Meaning |
| --- | --- |
| 0 | The hardware pointer. 0: the caps report no pointer and the DML plans have no cursor, as before this change. |
| 1 | Color pointers as premultiplied alpha (A/B check of the alpha contract). |
| 2 | X and Y are the hot spot position (A/B check of the position contract). |

The value is not a setting of the state catalog. A catalog entry changes the catalog identity, and then every release
component that pins the catalog must be built again. A later catalog entry can replace the value. The KMD log line
`cursor: setting EnableHardwarePointer <n>` shows the value of each start.

## Gates

| Gate | What it checks |
| --- | --- |
| `dcn-cursor` | `dcn_cursor_test.c` on the production header. It checks the verdicts and the three formats (monochrome with an odd width and a padded pitch, with an inverting pixel). It checks the hot spot rule and the placement: negative clamp, edges, the 8-bit hot spot and the destination offset. It checks a scaled plane with a viewport origin, and the rotated image against `Bc250RotationMapPixel`. |
| `dcn-cursor-controls` | 11 mutations of the header. Each one must fail the test. |
| `dcn-write-sites` | Each cursor write names a register on the write list. |

## Open points

These points have no evidence on this disk. Lab trials of the pointer decide them:

1. The alpha of Color pointers: straight (mode 3) or premultiplied (mode 2). Setting 1 against 3.
2. The meaning of the alpha byte in mode 1: the driver uses 0xFF as "keep the screen and XOR the RGB value". A
   monochrome I-beam on a dark and a light background shows it.
3. The position: top left (setting 1) or hot spot (setting 5). An arrow at a known `SetCursorPos` point shows it.
4. The rotated frame: the driver assumes that X and Y are logical (desktop) coordinates on a rotated path.
5. The software fallback after a refused shape: no DDI page states that dxgkrnl then draws the pointer.
6. A flip that changes the overlay plane shape does not place the cursor again until the next position call.
7. FP16 and HDR planes: the scale 0x3C00 is right for an SDR plane only.
