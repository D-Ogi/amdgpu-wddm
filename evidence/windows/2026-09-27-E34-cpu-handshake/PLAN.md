# CPU handshake-only negative control

Pinned c0edf93/0DC75F8C probe, accepted only with --handshake-only --no-open.
Baseline exact164/UMD8279/ICDCF39, same DWM/boot before and after. Two sequential
641x479 windows, no-paint then GDI-paint. No source/context/Present/resource open.
Record DWM answer and classification, child process identity, exact arguments,
health and temperature, DxgKrnl/Kernel-Process ETW. Expected CPU answers are
adapter-not-found or GDI-blit-required, but retain any actual answer without
relabeling it a GPU success. Each child <=45s, task <=3min. No driver transition.
All children terminal before task removal and archive. STOP honored. GPU matched
observation follows separately, using reviewed baseline UMD first.
