# Exact KMD162 diagnostic build

Clean isolated source52c92ce4d3e079dce35e80efbbfe00b25279d6e7, parent exact161
6c93d74. Only wddm.c diagnostics from0e540dd and INF/escape version fields differ.
No unrelated main-branch fence-ledger changes imported. KMD0.7.162.1,
escape0x000700A2. Gate behavior remains diagnostic/default0.

Full build,13 quality gates, link, stack gate, catalog generation and test signing
pass. Largest fixed stack frame3992 bytes remains GfxPagingBuildUpdate; existing
stack warnings remain in build.log. SYS SHA256:
89A7AE285ABD34C17F825D51CBD3C27ACA7BA7CEF764122CB35A274C9BCC78F8.
Both packages contain the identical signed SYS. Every package file was independently
rehashed against the eligible source manifest; worktree still clean after build.
Artifact: scratch/g0-hosted/kmd162-final001/package-umd.

Not staged or deployed. Exact161/40F7916F remains installed with CPU desktop,
GPU Present0/interop0. Before runtime: prepare exact161 rollback, validate target
manifest and transition scripts, preserve the current boot/session and use the
new predicates to identify the M652 rejection. No G0 acceptance claimed.
