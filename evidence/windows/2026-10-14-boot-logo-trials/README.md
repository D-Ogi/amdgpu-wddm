# The boot-screen mascot on unit A: seven boots, 2026-10-14

Seven trials on unit A (Windows 11 Pro 22631) with a KMD built from the boot-logo feature on the BD-169 base
`cb4c3420`, SHA-256 `CB49F836638B4ED7E4AE42BBED9306E3939422FE3A810DBBA326A006D75C6D0D`, 1029008 bytes, deployed
by hash into the installed `tester.31` candidate package (rc4) and swapped back to rc4
(`B0E01A869298CF85E14827B4CBD6840DD8C7955374FB9E4E1698A640FFB9760E`) at the end. The design is
`docs/design/boot-logo.md`; the fact is M901.

Each trial restarted the lab, read the driver's ring log, the start record, the inherited-pipe readouts, the
device status and the settings, and ran a camera burst over the boot screen. The GPU stayed at the lab
operating point; no trial exceeded three minutes, the longest being the restore at 2 min 27 s; the lab was
restored and verified by the operator and independently by the lead at 11:07Z.

| Trial | UTC | Setting | Surface | Draw | Window to the first flip |
|---|---|---|---|---|---|
| `appears` | 10:18:37 | defaults | 1920x1080 | drawn, anchor 350, 4638 us | 984 ms |
| `handoff` | 10:48:04 | defaults | 1920x1080 | drawn, anchor 350, 4582 us | 1153 ms |
| `anchor` | 10:53:25 | `BootLogoAnchorPermille` 700 | 1920x1080 | drawn, anchor 700, 4669 us | 1453 ms |
| `handoff2` | 10:57:03 | defaults, device restart only | 1920x1080 | refused: `target 2424833 is a driver handoff, not a boot` | none |
| `off` | 10:58:39 | `BootLogo` 0 | 1920x1080 | no draw, and no line in the log | none |
| `scaled` | 11:01:52 | `bcdedit graphicsresolution 800x600` | 800x600 | drawn, anchor 350, 4639 us | 951 ms |
| `restore` | 11:04:28 | rc4 KMD back | 1920x1080 | the rc4 driver has no boot logo | none |

All seven PASS: the loaded KMD hash matched the intended one at every boot, the device read OK with no Code 43,
and the desktop came up each time.

## What the picture costs and how long it is in the surface

The draw writes 13133 pixels, 52532 bytes, in 4582 to 4669 us: 10.7 to 10.9 MiB/s through the write-combining
mapping of the inherited surface. The window from the draw to the first `SetVidPnSourceAddress` flip was 984,
1153, 1453 and 951 ms in the four boots that drew. From `DxgkCbAcquirePostDisplayOwnership` (stage 33) to the
end of that window: 1174, 1347, 1648 and 1141 ms.

Every boot ends its window with a flip, and no boot took the display-only present path, so the picture is
replaced by the operating system through this driver and not by a second writer.

## Was it on the panel

Yes, in both modes, and the camera caught it in every boot that drew with a burst running. The mascot is a
white silhouette above the ring of dots, in the frames between the boot screen and the black that follows.

| Burst | Mode | Frames with her | Interval | Her frames over the window |
|---|---|---|---|---|
| `handoff` | 1920x1080, anchor 350 | 0102, 0103 | 286.1 ms | 2 of 4.0 |
| `anchor` | 1920x1080, anchor 700 | 0108 | 288.6 ms | 1 of 5.0 |
| `scaled` | 800x600 | 0089, 0090 | 305.9 ms | 2 of 3.1 |
| `off` | 1920x1080, `BootLogo` 0 | none in 360 frames | 306.6 ms | the control |
| `appears` | 1920x1080, anchor 350 | no burst: the camera wrote 0 frames | - | - |

One or two frames each, so her time on the panel is between about 0.3 and 0.9 s, which the frame interval
alone cannot narrow further. In the `anchor` burst she stands 34 camera rows lower on the panel than in the
`handoff` one, which is `BootLogoAnchorPermille` 700 against 350 working on the hardware. In the `scaled` burst
her silhouette stands where the Windows flag is in the three frames before it, which is where the owner saw her.

The `appears` burst is empty because of the camera defect of that morning: the burst ran `bash`, which on this
PC is WSL and not Git Bash, so ffmpeg never started. The trial itself is unaffected.

**What ends her time on the panel is open.** In every boot the next frame is black, the screen stays black for
2 to 5 s, and the OS-choice menu of `bootim.exe` comes after that black and not before it. The black may be
this driver's own first present, which has nothing of its own in the surface yet, or a clear by something else. The `anchor`
boot argues against the first present: its draw-to-flip window is 1453 ms, five camera frames, and she is in
one of them. The next round settles it with the `boot logo: scanout` line, which reads the primary surface
address, the OTG master bit and the OTG blank state at the draw moment.

The driver never blanks in that window: the first `SetVidPnSourceVisibility` of every boot asks for `visible 1`,
arrives with the first flip (2.283 to 2.774 s after the load, against a draw at 1.321 to 1.348 s), and reads
`hardware 1 blanked 0` before and `blanked 0` after. The driver's own blank is calls 5 and 6 at about 14.17 s,
long after the desktop is up.

The first call reports `previous 1` in three native boots and `previous 0` in the fourth and in the scaled boot.
The value follows neither the mode, nor the connector route, nor the sighting, and both values leave `blanked 0`
on each side of the call, so neither changed what the panel showed. It is recorded as an unexplained difference
between otherwise identical starts, a lead for BD-173, not a cause.

## How to read the camera, and how not to

The first pass over these same bursts reported that the mascot did not appear in the native boots. That was
wrong, and the way it went wrong is worth keeping. The camera's automatic exposure lifts a dark boot screen
until the panel and the room read the same, so **the mean brightness of a region carries no signal**: the logo
region read 123 against 103 for a control region beside it, and that difference was read as the absence of a
picture. What survives the exposure is saturation and shape, because everything a boot screen draws is white
on black. The Windows flag is a compact square, the mascot is a tall ragged silhouette with her hair on top,
the ring of dots is a small blob low on the panel, and the OS-choice menu is a wide band.

`scratch/boot-nagi/lab/screen-shapes.py` reads a burst that way: it takes a frame whose panel is black, masks
everything saturated in it as the room, and then names the lit shapes of every other frame by area and aspect.
On these bursts it gives the state of the screen frame by frame, and the mascot frames of the table above are
its output, confirmed by eye on the frames themselves.

The frames stay in the operator's scratch directory and are not kept here. The camera also sees the owner's
room, so every frame was cropped at capture.

## Two readouts the trials produced for other work

- **BD-169 on hardware, independently of its own arm.** The `scaled` boot is a BD-155 case and took the
  inherited-only route: `connector: inherited-only child 2424833 source 800x600 pitch 3328 target 1920x1080
  pixel 174500000 support 4`, `inherited gate: 0 none word 00000000`, one source mode, device OK, no Code 43,
  desktop at 800x600. The four native boots took the full route, `connector-link: open pipe support F status
  00000000 valid 7FFFFF bottom 00000000 control FFFF0461`, and committed generation 1 with 1 attempt, 1
  completed, 0 refused.
- **Native against scaled at the draw.** The inherited-pipe readouts are identical in the two modes except
  where the geometry differs: `otg1 80000310`, `hubp 000F100A`, `mpcc FFFF0461` and the plane format
  `argb8888 config 0x00000008 crossbar 0x00E40000` in both; `viewport 00000000/04380780 pitch 0000077F taps
  00000000` native against `viewport 00000000/02580320 pitch 0000033F taps 00000011` scaled. The handoff boot
  differs in `hubp 100F010A`, the running desktop's plane, which is what a handoff is.

## Trial records

The operator's directories, one per trial, hold the driver ring log, the start record, the before and after
readouts, the settings, the restart record, the boot wait and the camera frames:
`scratch/boot-nagi/lab/<trial>-<timestamp>/` on the development PC, outside this repository. The analyser that
produced the windows above is `scratch/boot-nagi/lab/analyse-start.py`, which keys on the `boot logo:` lines
whose form `driver/kmd/test/run_boot_logo.ps1` pins.
