# WGL replacement glFlush validation

PROVENANCE: Mesa MIT; piglit MIT. Unit A, KMD151/RADV986B691F.
Full quick005 stopped after1225cases:1060pass/164skip/1fail. The original
large-array scratch case passed in this full run. The first failure was
spec@!opengl 1.0@gl-1.0-beginend-coverage: only glflush failed, reporting
GL_NO_ERROR where GL_INVALID_OPERATION was required. No GPU hang/reset occurred;
registration restored and health remained generation589506402/epoch5.

Mesa main/context.c deliberately replaces Flush in no-op dispatch tables on
Windows to suppress implicit flushes by the system opengl32 loader during
procedure lookup. The standalone Mesa opengl32 wrapper does not issue those
implicit calls (stw_wgl.c delegates directly to DrvGetProcAddress).
The WGL frontend already distinguishes native ICD use by callbacks.pfnGetDhglrc.

The patch restores the validating OutsideBeginEnd Flush handler in BeginEnd
and HWSelectModeBeginEnd tables only when that callback is absent (Mesa's
replacement). Native system-loader ICD behavior is unchanged by the branch;
that native path has not been runtime-tested here. Patch replay against exact
upstream reproduces the built source. New Zink library1DAF0B60, oldD18E2372.

024:unchanged gl-1.0-beginend-coverage, all subtests including glFlush pass,
process exit0. 025:unchanged primitive-restart DISABLE_VBO, pass/exit0; this
covers the lazy procedure-lookup pattern motivating the old workaround.
Both use canonical RADV986B691F and keep the same healthy GPU generation/epoch.
Baseline Vulkan registration restored after each. No version override or skip.

Earlier isolated023 inherited RADV_DEBUG=shaders,shaderstats and hit its45s
instrumentation deadline while printing shader dumps. It was killed; task was
Ready, no test process remained, baseline restored and GPU health unchanged.
024 disables only that debug output and retains the same45s limit. This is not
a relaxed timeout or a pass for023. Its local raw logs remain in scratch/m12.

Full quick must be rerun with the new library. Full Linux parity, native loader
validation and M12.1-M13.1 acceptance remain open.
