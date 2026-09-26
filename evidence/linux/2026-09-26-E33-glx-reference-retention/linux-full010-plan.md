# Full Linux piglit reference with drawable retention

M523/M524 isolation establishes pixel-preserving native GLX drawable retention.
Use original installed RADVFEC7C475 and Gallium1FA58014, original upstream
piglit0cc01423, same hardware/clocks and RadeonSI Xorg. Explicit
keep_native_window_glx_drawable=true; no cleanup patch, polling patch or oracle
change. Full45159-case inventory restarts from its beginning. Earlier full003,
006 and007 remain a separate default-configuration dataset, not merged into
this run. Runtime keeps serial45s timeout and stops on any non-pass/skip.
Known FBConfig warning and GLX cleanup failure remain results, not waived.
Expected: make-current no longer loses back-buffer contents; multi-window may
report its GLXBadWindow cleanup failure. Any timeout requires fresh diagnosis.
This configuration is not a measured performance improvement.
