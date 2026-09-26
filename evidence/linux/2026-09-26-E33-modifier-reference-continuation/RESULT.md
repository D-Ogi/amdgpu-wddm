# Explicit-modifier reference continuation (M527)

M526 reference configuration unchanged: server RadeonSI AMD_DEBUG=export_modifier,
client keep_native_window_glx_drawable=true, original RADVFEC7C475 and
Gallium1FA58014, unchanged installed upstream piglit0cc01423.

Full013 ends1003cases:861pass/141skip/1warn. Its48 FBConfig warning lines
match full010. Full014 exact continuation ends8cases:7pass/1fail. The failure
is the same multi-window GLXBadWindow at X_GLXDestroyWindow with empty stdout,
not black probes or timeout. The subsequent swapbuffers control pixel-passes.
No reset or configuration change between these segments.

Retain all1011 outcomes (868pass/141skip/1warn/1fail), including both stops.
The exact44148 remaining names are launched as full015 in the same reference
configuration. Earlier configurations remain separate. Full015 final result
is not included here. No full OpenGL or M12.1-M13.1 acceptance claim.
