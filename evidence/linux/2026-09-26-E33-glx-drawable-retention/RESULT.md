# GLX native drawable lifetime and test cleanup (M523)

PROVENANCE: Mesa05e6c962 (MIT), piglit0cc01423 (MIT).
Same Linux boot as M522; initially Xorg7618, later Xorg-only recovery10676.
Diagnostic tests are separate executables, never replacements for installed
upstream test cases. Every modified test retains the original pixel probes.

Immediate one-pixel readback after drawing gives the correct color in all8
windows and GL_NO_ERROR. Later probes after context switches read black.
Adding glFinish after each draw still fails. Combined with M522 correct50x50
viewport, this points to drawable lifetime rather than missing shader output.

Mesa src/glx/dri_common.c releaseDrawable drops native X-window drawables at
refcount0 by default. keep_native_window_glx_drawable routes these through a
live-window/zombie set. src/util/driconf.h describes this explicit option;
xmlconfig.c supports its environment override. This is a configuration change,
not equivalent to the default baseline or a license to rewrite its results.

With keep_native_window_glx_drawable=true and diagnostic RADV86DAD711,
unchanged glx-make-current pixel-passes/exit0. Unchanged multi-window instead
reaches GLXBadWindow at X_GLXDestroyWindow, without black-probe output.
The test creates raw X windows via XCreateWindow but calls glXDestroyWindow.
A separate diagnostic copy changes only its two cleanup calls to XDestroyWindow.
With retention=true that copy pixel-passes/exit0 on BOTH Zink using original
FEC7C475 (no bounded-wait patch) and native RadeonSI. Thus this control does not
require the timed-poll candidate. Installed upstream tests remain unchanged.
Original failures/timeout stay authoritative until a recorded profile policy
handles configuration and upstream-test defects; no full-suite waiver here.

Post-control swapbuffers using original FEC7C475 with retention=true aborts134
in kopper_acquire. Thus successful diagnostic pixel probes do not establish
healthy subsequent presentation. Preserve this negative control. Restarting
only Xorg7618 ->10676 restores unchanged swapbuffers pixel-pass/exit0 on the
original ICD without retention override. No GPU/OS reset. All workers terminal. The diagnostic X11 patch is reversed in the build source;
source matches its saved original byte-for-byte. Rebuild succeeds but binary
comparison differs (98201E38 vs installed FEC7C475), so no identical-binary
claim or deployment: FEC7C475 remains the tested installed baseline. The saved
86DAD711 diagnostic path remains available but is not promoted.

Next: establish explicit GLX reference configuration and preserve historical
outcomes when continuing full inventory. Upstream test cleanup and teardown
hang require separate tracked treatment. M12.1-M13.1 acceptance remains open.
