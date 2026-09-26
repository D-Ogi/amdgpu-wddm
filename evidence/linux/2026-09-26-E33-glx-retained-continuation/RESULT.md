# Retained-drawable reference continuation (M525)

Same explicit reference as M524: original RADVFEC7C475/Gallium1FA58014,
keep_native_window_glx_drawable=true, unchanged upstream piglit0cc01423.
Full010 ends1003cases:861pass/141skip/1warn. Its48 FBConfig diagnostic lines
are identical to full003; extra stderr declares retention override. Raw results
are preserved, including warning. It is not converted to pass.

Exact disjoint remainder full011 ends8cases:7pass/1fail. Previously failing
make-current now passes. Multi-window fails at GLXBadWindow/X_GLXDestroyWindow,
matching the isolated invalid cleanup diagnosis M523/M524, not a timeout or
black-pixel failure. Post-run unchanged swapbuffers passes with retention=true.
No Xorg, GPU or OS restart was needed for these reference segments.

Together1011cases:868pass/141skip/1warn/1fail. Prepare exact44148 remaining
names with piglit_remaining, preserving both non-pass outcomes as required
inputs to final comparison. Original default-config runs remain separate.
Full012 starts that remainder with the identical reference configuration and
45s stop-on-nonpass runner; its final outcome is not part of this checkpoint.
Neither reviewed stop is a waiver, and full GL or M12.1-M13.1 acceptance is
not complete. No diagnostic cleanup/poll patch is used by the reference.
