# The picture on the start screen

While Windows starts, this board draws a spinning ring on black and nothing else. The firmware does publish an
ACPI BGRT table, so it claims that it drew a logo, but no logo reaches the screen. This page describes how the
kernel-mode driver draws the project mascot there instead, what the Windows contract allows, and where this
design follows that contract. The startup blank hides the drawing until Windows requests a visible source.

Code: `driver/kmd/boot_logo.h`, `driver/kmd/boot_logo.c`, `driver/kmd/boot_logo_asset.inc` (generated),
`DisplayDrawBootLogo` in `driver/kmd/display.c`, the call in `driver/kmd/pnp.c`. Host test:
`driver/kmd/test/boot_logo_test.c`, run by `driver/kmd/test/run_boot_logo.ps1` and by `tools/quality/quick.ps1`
as `boot-logo`. Setting: `BootLogo`, in the control application on the Display page.

## What draws the start screen, and until when

1. The UEFI Graphics Output Protocol sets the panel's native timing and resolution, and hands the operating
   system a frame buffer to scan out. Windows does not reset the timing: it uses the buffer the firmware handed over,
   which removes one screen flash. (`ref/windows-driver-docs`, `display/seamless-state-transitions-in-wddm-1-2-and-later.md`,
   "Transition from firmware to operating system".)
2. The boot loader and the kernel draw the logo and the ring into that same buffer.
3. The operating system starts the display miniport driver through `DxgkDdiStartDevice`. The driver calls
   `DxgkCbAcquirePostDisplayOwnership` and is told the exact geometry of the buffer the firmware left:
   `DXGK_DISPLAY_INFORMATION` with `Width`, `Height`, `Pitch`, `ColorFormat` and `PhysicAddress`
   (`ref/ddi-display/d3dkmdt.md`, `d3dkmdt.h` line 1474, WDK 10.0.26100).
4. The driver keeps the output black while it owns startup. Windows renders the first frame, then requests source visibility.

Step 3 to step 4 is the window this feature draws in, behind the hardware blank. The driver already maps that buffer there
(`DisplayMapFramebuffer`) and already writes to it on two other paths: `Bc250PresentDisplayOnly` for the
display-only desktop, and `Bc250SystemDisplayWrite` for the bug check screen.

## Where this design stands against the contract

The [PnP start contract](https://learn.microsoft.com/windows-hardware/drivers/display/plug-and-play--pnp--start-and-stop-cases)
requires black output during `DxgkDdiStartDevice`, regardless of the surface contents. Sync signals must remain active.
Windows later requests source visibility after it renders the first frame. Local reference: windows-driver-docs revision `110f60ea`, with WDK 10.0.26100.

The start now checks that OTG0 is active and OTG1 is inactive before it writes display state.
It blanks the pipe after BAR mapping and checks the blank again after connector admission.
`StartVisibilityPending` keeps internal surface restoration or connector changes from exposing pixels before the OS visibility request.
The hardware blank applies to CPU presentation too. A framebuffer clear alone does not satisfy the contract.
An OS request for TRUE releases the pending state only after hardware confirmation succeeds.
A flip, timer or logo draw cannot release it. A failed start restores display ownership before BAR5 is unmapped.

The blank color follows the active output encoding, including RGB with limited range and the supported YCbCr matrices.
The driver reads the formatter and the standard AMD CSC matrix. Two frame-counter increments separate the bank selection read from coefficient inspection.
An unknown matrix, changing format, held update lock or stalled timing refuses blanking.
A refusal before any display write releases MMIO without changing the inherited pipe.
Once a display write succeeds, failed startup keeps MMIO mapped through display restoration.
The color selection and blank acknowledgement share a 100 ms polling budget. Host models do not establish the color or timing on a physical monitor.

`BootLogo` remains on by default. The driver draws behind the blank, so a visible mascot before the first OS frame is no longer promised.
HLK tests must use the shipped default. Disabling the logo only for certification would not test this behavior.

The earlier seven lab boots measured the behavior before this change. Their first visibility callback requested TRUE and found the pipe already visible.
The earlier hardware path did not require a change in `SourceVisible` to write blanking. The missing operation was the blank during startup.
Those boots do not establish the behavior of the new startup sequence.

The earlier logo reached the panel in both modes, and the camera caught it in each of the three boots that drew with a
camera running (M901). The mascot stands above the ring of dots, as a white silhouette on the dark screen, in
the frames between the boot screen and the black that follows it:

- **1920x1080, anchor 350**: 2 frames, `0102` and `0103` of the `handoff` burst.
- **1920x1080, anchor 700**: 1 frame, `0108` of the `anchor` burst, and 34 camera rows lower on the panel than
  at anchor 350, which is the setting working on the hardware and not only in the host test.
- **800x600 scaled**: 2 frames, `0089` and `0090` of the `scaled` burst, where her silhouette stands in the
  place of the Windows flag, which is in the three frames before it.
- **`BootLogo` 0**: no such frame anywhere in that burst. That is the control.

What ends it is the open question. In every boot the screen goes black in the next frame and stays black for
2 to 5 s, and the OS-choice menu of `bootim.exe` comes after that black, not before it. The black may be this
driver's own first present, which has nothing of its own in the surface yet, or a clear by something else. One boot argues
against the first present: the `anchor` window from the draw to the first flip is 1453 ms, which is 5 camera
frames, and she is in one of them. The scanout line below is what answers this, by saying what the display was
fetching while the picture was in the surface.

Two related requirements apply:

- **Device.Graphics.WDDM12.Display.PnpStopStartSupport** in the Hardware Lab Kit tests the requirement above.
  Host tests check startup ordering and failure cleanup. Native and inherited scaled modes still need lab validation with the shipped default.
- A driver that starts as an upgrade of another WDDM driver is handed a running desktop, not a start screen.
  A documented signal, not a behaviour, excludes that case: `DxgkCbAcquirePostDisplayOwnership`
  says that an uninitialized `TargetId` "typically occurs after a system boot", and that "in the case of a
  driver-to-driver upgrade, the previous driver will have provided the target identifier"
  (`ref/ddi-display/dispmprt.md`). `DisplayDrawBootLogo` draws only when `Post.TargetId` is
  `D3DDDI_ID_UNINITIALIZED`.

What this design does **not** touch, and will not: the firmware, the SPI flash, NVRAM, the EFI system partition,
the boot entries and the BGRT table. The two usual ways to put a logo on a start screen -- a modified firmware
logo, or a loader that rewrites BGRT in the HackBGRT manner -- are both out of scope for that reason.

## The picture

`tools/win/amdgpu_wddm_control/art/nagi/sheet-welcome@256.png`, the mascot the control application draws as
`01-wave` and the installer draws on its first page (that directory's `PROVENANCE.md`). The generated file
records the picture's SHA-256, so a substituted picture stands out in a diff.

`tools/win/make_boot_logo_asset.py` turns it into C source: transparent margins trimmed, fitted into a 160-pixel
box, colours reduced to a palette of at most 256 straight-alpha BGRA entries, and each row coded as runs of
(count, index) pairs ending in a zero count. Index 0 is the transparent colour, so the margins of the figure
cost two bytes a row. The result today is 123 x 160 pixels in 22172 bytes, against a budget of 48 KiB that both
the generator and the host test enforce.

The generator is **not** run by the build. The repository holds its output, the driver decodes no PNG, and the build
stays reproducible (`/Brepro`, `docs/design/reproducible-builds.md`). Re-run it only to change the picture.

## Placing and drawing

Every coordinate is in the firmware's surface, never in the monitor's timing. Where the firmware scaled a
smaller surface up to a larger timing -- the BD-155 case, an 800x600 surface on a 1920x1200 panel -- the display
pipe scales what this code draws together with the rest of the picture, so the mascot keeps the same share of
the screen and needs no special case.

- Enlargement: `Height / 600`, brought inside 1 to 3, then lowered until the picture fits. At 800x600 it is 1,
  at 1920x1200 it is 2, at 3840x2160 it is 3. Nearest neighbour, whole numbers only.
- Across: centred.
- Down: the centre of the picture sits at `BootLogoAnchorPermille` thousandths of the height, 350 by default, so
  the mascot stands above the ring rather than under it. The driver brings the value inside 100..900. It is a registry
  value and not a constant, so the lab can try other heights against a photograph without a rebuild.
- **The ring keeps its place** (owner, 2026-10-14: the mascot goes in the logo area above the ring, never over
  it). The whole picture lives in the top `BC250_LOGO_BOTTOM_PERMILLE` thousandths of the surface, 550 today.
  The band wins over the anchor: an anchor that asks for a lower place moves the picture up, never down over the
  ring. The enlargement drops until the picture fits the band, and a picture that cannot fit the band at
  enlargement 1 refuses the call and draws nothing. At the lab's mode, 1920x1200, the picture is 246 x 320 at
  (837, 260), so its last row is 580 of 1200 and the band line is row 660.
- Blending: source over destination. A fully opaque pixel goes straight into the frame buffer with no read back, a
  fully transparent one changes nothing, and only the anti-aliased edge of the figure reads and blends. The
  destination at this moment is black, so the result is the figure on black.

This code touches no pixel outside the placed rectangle. An asset that fails its own self-check, or a surface
the picture does not fit in, draws no pixel at all.

## Settings

Under `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`. The driver reads them once at device start.

| Value | Type | Default | Meaning |
|---|---|---|---|
| `BootLogo` | REG_DWORD | absent, which means on | 1 or absent draws the picture, 0 does not |
| `BootLogoAnchorPermille` | REG_DWORD | absent, which means 350 | the vertical centre of the picture, in thousandths of the surface's height, clamped to 100..900 |

On is the release default, so the control application writes 0 to switch the picture off and **removes** the
value to switch it back on. A removed value reads as absent, which is the default, so no later install can undo
the person's choice by writing the default over it. This is the rule `FanLoadBoost` already follows.

In the control application the switch is on the Display page, in the card "Start screen"
(`src/BootLogoSetting.cs`, `src/MainForm.Display.cs`), with its search entry and its explanation in the four
interface languages.

The installer offers the same choice (owner, 2026-10-14): a box on the plan screen of the setup window, checked when
the screen opens, beside the box for the Nagi wallpaper, and `-BootLogo` or `-NoBootLogo` on the command line.
A run that states no answer writes nothing, so a reinstall or an upgrade keeps the answer of the installation before
it, and `uninstall.cmd` removes the value. `Get-BootLogoPlan` in `tools/release/installer/common.ps1` is the whole
rule, and it is pure, so `tools/release/test-engine-units.ps1` holds every case of it.

## What the driver writes to its log

`DisplayDrawBootLogo` writes one line a start, and the draw writes a second one:

```
boot logo: drawn on 1920x1080 anchor 350, 13133 pixels, 52532 bytes, 4582 us
boot logo: scanout 0x2729B0000 inherited 0xC0000000 otg master 1 blank 0 control 0x00000000
boot logo: skipped, target 2424833 is a driver handoff, not a boot
boot logo: off by setting (BootLogo 0)
boot logo: scanout not read at the OTG control, status 0xC0000001
```

The draw line comes after `DisplayLogFramebufferSample`, which is why the picture is drawn after that sample
as well: the sample is evidence of what the firmware left in the buffer, and it must keep reading the firmware's
pixels, not ours.

The **off by setting** line exists because of the `off` trial of 2026-10-14: its log holds no `boot logo:` line
at all, and a reader cannot tell a driver whose owner turned the picture off from a driver in which this code
never ran. Every path now says what it did.

The **scanout** line is read after the draw, from three registers this driver already reads elsewhere
(`HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS` and its `_HIGH`, `OTG0_OTG_CONTROL`, `OTG0_OTG_BLANK_CONTROL`). It
answers the question the camera cannot: whether the surface the picture is in is the surface the display is
fetching at that moment, and whether the pipe is blanked while it does so. A failed read prints the step that
failed and the status and no value, because a zero address beside a zero blank state would read as "scanning
out nothing", which would be a conclusion about registers that were never read. The `inherited` field is
`Post.PhysicAddress`. The existing `display inherited: ... surface` field is `SurfaceConfig`, a format
register, and carries no address, which is why this line was needed at all.

## What the lab measured, 2026-10-14

Seven boots of unit A on a KMD built from this feature (`evidence/windows/2026-10-14-boot-logo-trials`, M901).
The window the picture is in the surface, from the draw to the first flip, was 984, 1153, 1453 and 951 ms in the
four boots that drew. `DxgkCbAcquirePostDisplayOwnership` to the end of the window was 1174, 1347, 1648 and
1141 ms. The draw itself took 4582 to 4669 us for 52532 bytes, which is 10.7 to 10.9 MiB/s through the
write-combining mapping of the inherited surface. The camera caught the mascot on the panel in one or two
frames of each of the three boots whose burst ran, so her time there is between about 0.3 and 0.9 s of that
window. The handoff refusal fired on a device restart, with
`TargetId` 2424833 (`0x250001`), the child this driver owns, against `D3DDDI_ID_UNINITIALIZED` on every cold
boot: the discriminator works on the hardware and not only in the documentation.

Historical observations for **BD-173**, the ownership-boundary gap:

- The first `SetVidPnSourceVisibility` of a boot arrives **with the first flip**, not during `StartDevice`:
  2.283 to 2.774 s after the driver loaded, against a draw at 1.321 to 1.348 s. There is no visibility call at
  all in the draw window on that measured build. The startup blank now closes that visibility window.
  The first OS visibility request releases it. Leaving the inherited image visible until a flip would violate the startup contract.
- That first call reports `previous 1` in three of the native boots and `previous 0` in the fourth and in the
  scaled boot. The value does not follow the mode, the connector route (both `previous` values occur on the full
  route) or what the camera saw, and both values leave `blanked 0` before and after the call, so neither changed
  what reached the panel. Startup and connector admission now keep `SourceVisible` FALSE until the OS requests otherwise.

How to read the **camera**, because the first reading of these same frames was wrong. It delivers about 3.3
frames a second during a boot screen, so one frame is about 290 to 306 ms against a window of about 1 s, and it
catches the mascot in one or two frames of each boot. Her time on the panel is therefore between about 0.3 and
0.9 s, which the frame interval alone cannot narrow. The camera's automatic exposure lifts a dark screen until
the panel and the room read the same, so **a brightness mean of a region says nothing here**: the first pass
compared the logo region with a control region beside it, found 123 against 103, and read that as nothing on
the screen. What survives the exposure is saturation and shape, because everything a boot screen draws is white
on black: the Windows flag is a compact square, the mascot is a tall ragged silhouette, the ring is a small dot
low on the panel, and the OS-choice menu is a wide band. `scratch/boot-nagi/lab/screen-shapes.py` reads the
bursts that way, by masking the room from a frame whose panel is black and naming the lit shapes of every other
frame.

## Tests

`driver/kmd/test/boot_logo_test.c` (the `boot-logo` check of `tools/quality/quick.ps1`) decodes and repaints
every case with an independent pixel oracle and compares the whole surface, so the decoder, the placement and
the blend are each checked against something that is not the code under test. It also holds: the padding of a
pitch wider than the width stays untouched, a refused call writes no byte, a damaged asset refuses before any
pixel, and the shipped asset verifies, stays inside its size budget and fits every surface the lab hands over,
including the BD-155 one. Every case also holds the band rule, over every anchor from 0 to 1000 in steps of 25:
the last row of the picture is always above the line the ring owns. A second compile of the same source uses the kernel flag set, `/W4 /WX`.

The same script pins the **form of the five log lines** above, which live in `display.c` and no host test
compiles, because a support file is read by a person and by the trial analyser: a renamed field is a broken
tool. One of the checks is that the failed-read line prints the step and the status and nothing else. The
widths of the lines are the `guardlog-width` gate's business, not this one's. The longest of them, the scanout
line, is 124 characters at its worst case against the ring's limit of 159.
