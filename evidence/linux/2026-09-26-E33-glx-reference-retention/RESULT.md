# Explicit Linux GLX reference configuration (M524)

Same boot and Xorg10676, no resets. Original RADVFEC7C475, Gallium1FA58014.
After M523 Xorg recovery, swapbuffers passes with retention both unset and true.
Separately running the cleanup-corrected diagnostic on Zink, then RadeonSI,
produces two pixel-passes, each followed by a passing Zink swapbuffers control.
Unchanged upstream multi-window with retention=true then reports GLXBadWindow;
its subsequent swapbuffers control also passes. Therefore neither cleanup
error alone nor those passing diagnostics reproduce the prior presentation
assertion. M523 negative control is retained; causality remains unresolved.

For full reference, select Mesa's existing keep_native_window_glx_drawable=true
explicitly. M523 explains its lifetime semantics and default failures. Use
original installed graphics binaries and unchanged installed upstream tests.
No polling or cleanup patch enters this reference. Start full010 from the
beginning of all45159 names; do not merge earlier default-configuration results
as if measured with this setting. Keep serial45s timeout and stop-on-nonpass.
Known warning/failure outcomes remain recorded, not skipped or changed.

Checkpoint: worker10926 is live,561 completed (427pass/134skip), inventory45159.
This is only a running snapshot, not completion. Historical full003/006/007
remain a separate dataset. Full Windows/Linux comparison and M12.1-M13.1 open.
