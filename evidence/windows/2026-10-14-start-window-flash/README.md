# Start window flash: the window's own surface against DWM's frames (lab, 2026-10-14)

Date: 2026-10-14. Run 1 09:20:50-09:23:28Z, run 2 09:53:23-09:56:15Z (lab clock, which was about 9.5 s ahead of the
development PC in run 2). Unit A, KMD rc4 of tester.31 (SYS B0E01A86,
read on the lab before each run), 40 CU, the GPU at 500 MHz in every DPM sample of both runs (no clock change). The kit
did not record the Windows build. Visual report: the countdown window of the logon task (`start-window.ps1`, "Nagi
start check") flashes now and then while its text counts down.

## Method

The kit is `tools/win/app-launch` (`measure-flash.ps1`, `flash-watch.exe`, `poll-repro.ps1`, `dpm-sample.ps1`):
run 1 at commit 8cb7205f, run 2 at the content of commit 7358e281 (`flash-watch.exe` SHA-256 D0B16AD3). Each case
starts the window script the way the logon task does (`powershell.exe -NoProfile -WindowStyle Hidden ...`) from a
task in the console session. `flash-watch.exe` records each frame that DWM composes over the window's rectangle
(Desktop Duplication, 8,34, 488x39) and over the overlay's rectangle. A flash is a content that stayed less than
60 ms between two other contents. `poll-repro.ps1` repeats the read-only calls of the start confirmation's poll every
5 s, and `dpm-sample.ps1` reads the DPM governor once a second, on the same QPC clock.

| Case | Window | Poll | Length |
|---|---|---|---|
| run 1 a | installed (`start-window.ps1` of the package, a WinForms Label whose Text changes once a second) | yes | 45 s |
| run 1 b | installed | no | 45 s |
| run 1 c | fixed (commit 84f35234: the form draws each frame in one buffered piece, no Label) | yes | 45 s |
| run 2 a | installed, and `-Surface` | yes | 150 s |

With `-Surface`, a second thread of `flash-watch.exe` reads the window's own surface through its window DC (GetDC,
BitBlt) about every 2 ms. GDI answers that read from the window's redirection surface, between two GDI calls on it.
A camera on the development PC recorded the window's place on the monitor at 30 frames a second (`flash-cam.py`,
crop inside ffmpeg, only the mean colour of 10x10 cells kept).

## Results

| | run 1 a | run 1 b | run 1 c | run 2 a |
|---|---|---|---|---|
| text changes (countdown steps) | 45 | 44 | 44 | 150 |
| partial pictures in DWM's frames | 8 | 2 | 0 | 12 |
| partial pictures in the window's surface | - | - | - | 125 |

1. Every partial picture except one is the same image, hash 4aabba0c843e6455 in both runs. It is the navy background
   with text only at x 7..99, y 5..22 of the 488x39 window. The complete text is at x 7..458, y 5..29. The exception
   (run 2, +126.963 s, hash 6a7c7ca3e8b3ef0e) has the complete first line (x 7..458) and no second line (y 5..22).
2. Run 2 made 72,041 surface reads in 150 s: median gap 2.08 ms, 99th percentile 2.16 ms, largest 7.44 ms. The
   slowest read took 1.19 ms. Self-check: all 150 regular contents of DWM's frames were also read from the surface,
   with the same hash.
3. The window's surface held the partial picture 4aabba0c843e6455 at 125 of the 150 text changes, each time for one
   read (2 ms, at most 4 ms).
4. Of the 12 partial pictures in DWM's frames of run 2, 9 were in the surface 1.0 to 3.0 ms before DWM's present.
   2 have the same hash, and no read fell into that state (largest read gap around them 2.14 ms). The one with hash
   6a7c7ca3e8b3ef0e is a later state of the same paint. It lasted less than one read gap, and the surface read the
   first state 2.0 ms before it.
5. The surface reads did not change the rate significantly: 12 of 150 in run 2 against 8 of 45 in run 1 a (two-sided
   Fisher p = 0.09). The fixed window had none: 0 of 44 against 8 of 45 with the same poll (one-sided Fisher
   p = 0.003).
6. No flash of the window was inside a step of the poll (0 of 8 in run 1 a, 0 of 21 in run 2). The steps take 2.0 %
   and 1.5 % of the time. The DPM governor did not change the clock in either run.
7. Camera, run 1: in case a the camera recorded 7 one-frame dips of the green level of the text cells. Each was
   0.484 to 0.504 s after a partial picture of DWM's frames, on the lab clock against the PC clock (a constant offset).
   There was no dip without a partial picture. In case b, one dip was 0.513 s after the other-content flash. Case c
   had no dip. Run 2: the camera found the window period (149.8 s), but its text cells did not resolve the flashes
   (6 dips, none within 0.3 s of a flash). ffmpeg dropped frames in that recording.
8. The close of the window, run 1 b and run 2 a. The window's rectangle shows, for one composed frame (30.3 ms and
   27.6 ms), a Windows Terminal window. The PowerShell icon and the tab title "C:\Windows\System32\Windo..." identify
   it: the console of `powershell.exe`, which Windows put in Windows Terminal. 8 frames of its close fade over the
   desktop follow (230 ms). The surface of the window never showed it. In run 1 a and c, DWM's frames have nothing
   at the close.
9. The overlay's rectangle (run 1, `analysis/overlay-run1.txt`, 89 bitmaps). In every case a darker area appears
   behind the 88 % opaque overlay before the window: a first change at -0.68 s (b) and -0.64 s (c) with a 200 ms
   fade-in, then a second change at -0.27 s (b) and -0.32 s (c) that stays. It goes away at the close with a 200 ms
   fade (b +44.696 to +44.902 s, a +45.080 to +45.280 s). At the end of c the overlay recorded no change. The darker
   pixels are at screen x 988..1668, y 27..883. `analysis/overlay-run1-difference.png` is the difference of two
   frames of case b against the brightest value of each pixel, 8 times stronger, at half size: window title bars and
   covered parts of the wallpaper.

## Conclusion

The partial picture is a state of the window's own surface during each change of the Label's text. DWM composes it
when a composition falls into that state, which lasts about 1.7 ms of each change (estimated from 125 of 150 changes
seen at a 2.1 ms read period). DWM's copy, the present and the scanout show the same pixels and add no state of their own.
The cause is the way the window paints itself, not the driver.

When the logon task starts the script, the console of `powershell.exe` is a Windows Terminal window on this unit, and
`-WindowStyle Hidden` does not close it: it is on the screen at the close of the start window. The overlay's numbers
agree with a Terminal window that stays open for the whole life of the script.

This run does not show:

- which call of the Label path writes the partial picture
- whether the Terminal window is visible all the time, because no capture covered the full screen
- the surface of the fixed window (run 1 c has DWM's frames only, 44 changes)
- the Windows build.

Runs 3 to 5 below answer the first and the third item. The console window has its own page:
[start window console](../2026-10-14-start-window-console/README.md).

## Runs 3 to 5: the fixed window's surface, and the cause of the partial picture

Date: 2026-10-14, 11:09:30-11:15:39Z on the development PC clock, in one lab slot after the console proof. Unit A,
KMD rc4 of tester.31 (SYS B0E01A86, native mode, read on the lab at 11:07Z), 500 MHz in every DPM sample.

The kit is `tools/win/app-launch`: `flash-watch.exe` SHA-256 E308E0BA (it gives the arguments after `--` to the window
script), `measure-flash.ps1` with `-FixedKind` and `-FixedApp`, and the window script `label-window.ps1` as run
(`scripts/label-window-run45.ps1`, SHA-256 00DCDA08). Every case read the window's surface (`-Surface`) as in run 2.

`label-window.ps1` makes a window with the shape of rc4's window: a borderless navy form, one control with Dock Fill,
Consolas 9 in cyan, the rc4 text and countdown, and a 250 ms timer. It draws the text as Nagi Control App does
(visual styles, TextRenderer). SteadyLabel is a Label subclass. Its window has the WinForms class instead of STATIC
(`scripts/SteadyLabel-run5.cs`). Run 5 loaded it from a control application build with SHA-256 DEB1BE6A, which was
made for this test only. No release of the control application has SteadyLabel.

| Run | Case | Window | Length |
|---|---|---|---|
| run 3 | c | the window of commit 84f35234: one form that draws each frame in one buffered piece, no child control | 150 s |
| run 4 | c | `label-window.ps1 -Kind Label`: a WinForms Label | 60 s |
| run 5 | c | `label-window.ps1 -Kind Steady`: a SteadyLabel | 60 s |

| | run 3 | run 4 | run 5 |
|---|---|---|---|
| surface reads | 71,654 | 28,739 | 28,085 |
| changes of the full text in the surface | 149 | 59 | 59 |
| changes with the partial picture in the surface between the two texts | 0 | 46 | 55 |
| partial pictures in DWM's frames while the window was open | 0 | 0 | 3 |

1. Runs 4 and 5 have one partial picture, hash 69b00b13dfdbc8af: navy, with text only at x 7..99, y 5..22 of the
   488x39 window, the same place as hash 4aabba0c843e6455 of runs 1 and 2 (that window drew with GDI+, these with
   TextRenderer). It lasts about 2 ms: the read before it and the read after it are 1.96 and 1.92 ms away (median).
2. The fixed window has no partial picture in 149 changes. This shows that the fix of commit 84f35234 works in
   the window's own surface, not only in DWM's frames. The last read of runs 3 and 5 (hash f707b8a381a645e3) is the window at its
   close.
3. DWM's frames show the Terminal window's close of M899 at the close of runs 3 (8 frames) and 4 (10 frames).
4. SteadyLabel gives the same partial picture as the Label. The window class of the control is not the cause, and the
   inference of run 2 (the STATIC window's own repaint on `WM_SETTEXT`) is wrong.

The cause, on the development PC with hidden windows (`scripts/label-bounds.ps1`, output `analysis/label-bounds.txt`):
a subclass of the Label's window records each `WM_WINDOWPOSCHANGING`. At every change of the text of a Label with
Dock Fill and AutoSize off, the Label first gets the size 100x23 and then its full size again (SWP flags 0x0016, so no
move). That is `Label.AdjustSize`, which `Label.OnTextChanged` calls. With AutoSize off it sets the size that the
program asked for. A docked control keeps the size that it had before the dock: 100x23, the default size of a Label.
The layout of the form then docks the control again. On the screen, the form paints the area that the small Label
uncovers with navy, without a buffer, and the small Label keeps the top left 100x23 pixels of its old text. That
corner of the text ("Nagi start ch" and the top of "Nothing to d") is the same at every countdown value, so the
partial picture has the same hash at every change. The next paint at the full size shows the new text. The pixels
agree (`scripts/partial-vs-ref.py`, `analysis/partial-vs-ref.txt`): in runs 2, 4 and 5, the partial picture has the
reference picture's pixels in all 2,300 pixels of the top left 100x23 and navy in all 16,732 other pixels.

The same trace for the layout of the control application's live labels (AutoSize on, a maximum width, in a
FlowLayoutPanel or a TableLayoutPanel cell): a text of the same size gives no window position, and a text of a new size
gives one, which is the final size. Those labels do not go through a wrong size.

## Files

| File | Content |
|---|---|
| `run1/measure-flash.txt`, `run2/measure-flash.txt` | standard output of `measure-flash.ps1` |
| `run1/<case>-log.txt`, `run2/a-log.txt` | the log of `flash-watch.exe`: targets, one line per flash, summary |
| `run1/<case>-frames.csv`, `run2/a-frames.csv` | the frames of each target (window and overlay, and the changes of the surface) |
| `run1/<case>-polls.csv`, `run2/a-polls.csv`, `*/dpm.csv` | the poll steps and the DPM samples |
| `run2/a-surface.csv.gz` | one row per surface read: QPC before and after, hash |
| `run1/*.png`, `run2/*.png` | the window's flash bitmaps (`<case>-window-<n>`), its reference (`<case>-ref`) and the first surface flash (`a-surface-0`). They are BMPs of the lab converted to PNG with the same pixels and the same base name |
| `analysis/` | the outputs of the scripts below. `overlay-run1.txt` names a PNG path on the lab, which is `overlay-run1-difference.png` here |
| `scripts/` | the analysis scripts as run: `run1-facts.py`, `surface-vs-dda.py`, `overlay-console.ps1`, `flash-cam.py`, `camflash.py`, `camflash-run2.py`. `surface-vs-dda.py` reads `a-surface.csv`: unpack the gzip first. `overlay-console.ps1` ran on the lab before the deletion of the overlay bitmaps |
| `run3/`, `run4/`, `run5/` | the same files for case c of each run (`c-*`), with the surface reads as `c-surface.csv.gz`. Every surface flash of a run has the same hash, so each run keeps one surface bitmap (`c-surface-0`) |
| `analysis/surface-runs-run345.txt`, `analysis/change-count-run345.txt` | the states of the surface between two texts (`scripts/surface-runs.py`) and the count of changes with the partial picture (`scripts/change-count.py`) |
| `analysis/label-bounds.txt` | the window positions of a Label at a text change, in three layouts (`scripts/label-bounds.ps1`, Windows PowerShell 5.1 on the development PC, no window shown) |
| `analysis/partial-vs-ref.txt` | the partial picture against the reference picture, pixel for pixel (`scripts/partial-vs-ref.py`) |
| `scripts/label-window-run45.ps1`, `scripts/SteadyLabel-run5.cs` | the window script of runs 4 and 5 as run, and the source of the SteadyLabel class that run 5 loaded |

Not kept: the overlay bitmaps (89 in run 1, 36 in run 2, 3.2 MB each), deleted on the lab after the analysis, and the
camera CSVs (cell colours, 21 MB each), which stay outside the repository.
