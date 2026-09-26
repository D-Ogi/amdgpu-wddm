# M474: shared surface raster block extent

Unit A, 2026-09-25. KMD147 unchanged; UMD CB1480EC67DB4DC22193E65AAB158680D00063332077205CE307B4474B1986C6 deployed.

## Cause
Two startup crashes reached llvmpipe shade_quads through lp_rast_linear_rect_fallback.
The owner-authorized private full dump confirms a1428x33 RGBA8 shared surface,
stride5712, CPU map extent192512 (page-rounded188496 logical bytes), versus
llvmpipe layer/sample stride205632 (36rows). At y32,x256 the third row load accesses
offset195232, outside that map. The address is16-byte aligned: this is an extent
error, not a misaligned vmovdqa or a missing VC dependency.
Private dump and detailed application-memory analysis remain outside the repository.

## Change
Owned pitch is aligned to64 bytes, or256 for primary scanout, and backing height
to4rows. Imports preserve allocation pitch/size rather than recomputing tight
storage. Storage metadata rotates with the underlying allocation. GDI USER_MEMORY
imports require complete4x4 raster blocks. Visible dimensions remain unchanged.
Apply mesa-surface-padding.patch after the existing E26 main/cache/diagnostic patches.

## Validation
- Build succeeds with Mesa f9a2d34a / LLVM23.1.2.
- Exact shared pixels both directions,64x32,1428x33,1366x35,1x1,67x65: zero mismatches.
- Event completion queries,307200green pixels,60present calls, native exit0.
- Loaded UMD hash matches candidate; KMD healthflags15 after test.
- Notepad survives15seconds on the new UMD instead of crashing after3-4seconds.
  The immediate process MainWindowHandle observation was false; that first value
  alone does not prove an interactive window. A subsequent monitor enumeration
  confirms a visible Notepad1430x838 window and a window screenshot is retained
  privately. No new Notepad crash event in the observed interval.
- Temporary full-dump configuration and reproduction task removed.
- Driver refresh initially checked DWM too early; later independent loaded-module
  check confirmed the candidate. No Windows reboot.

These are bounded regression controls. Not a long-duration stability result,
formal certification, or M10 FIFO/presentation acceptance.
