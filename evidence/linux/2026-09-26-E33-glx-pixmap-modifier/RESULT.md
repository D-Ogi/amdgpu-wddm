# GLX texture-from-pixmap modifier export (M526)

PROVENANCE: Mesa05e6c962, MIT. Unchanged piglit0cc01423.
Full012 terminal39cases:30pass/8skip/1fail glx-tfp. Black probes and Zink
INVALID<->LINEAR error. Same unmodified executable pixel-passes on RadeonSI.
Default retained-reference total1050 outcomes remains separate and preserved.

GDB breakpoints show multiplanes_available=1 and screen->has_multibuffer=1,
but imported handle modifier0x00ffffffffffffff (DRM_FORMAT_MOD_INVALID),
stride256, offset0. Thus the error wording about missing DRI3 modifiers does
not establish that the client lacks the modern protocol.

RadeonSI si_texture.c computes an explicit modifier for resource parameter
queries when DBG(EXPORT_MODIFIER) is enabled. si_pipe.c defines the existing
AMD_DEBUG=export_modifier option. Restart only the RadeonSI Xorg server with
that option (10676 ->21335), same server binary and same OS/GPU instance.
No client patch or assumed conversion from INVALID to LINEAR.

Unchanged Zink glx-tfp now pixel-passes/exit0 using original installed RADV
FEC7C475 and Gallium1FA58014. A debugger verification independently observes
multiplanes=1 and modifier0x0 (LINEAR), stride256, offset0, normal test exit.
A serial swapbuffers control after both diagnostic processes terminate also
pixel-passes. Earlier launch of a control alongside debugger is not used as
proof of post-test ordering; the separate serial control supplies it.

Reference configuration now explicitly includes server AMD_DEBUG=export_modifier
and client keep_native_window_glx_drawable=true. Full013 starts all45159 names
again with identical installed tests/oracles and serial45s stop-on-nonpass.
Earlier configurations remain separate datasets; no cherry-picking to replace
failures. Full013 final outcome is not part of this checkpoint. No performance
claim and no full M12.1-M13.1 acceptance claim.
