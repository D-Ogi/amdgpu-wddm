# Standard GDI texture layout control

Source: parent d6845cc123be3e32c8a86106333a254b31ae7af4 plus the changed-file
SHA256 manifest in this directory. Consumer: Mesa UMD
a0ad8af5ea46b90400d947d82d056f3bd58278c5, Resource.cpp OpenResource.

Standard GDI TEXTURE allocations now retain logical dimensions while using
256-byte pitch alignment and four-row allocation padding. The host test checks
the independent UMD acceptance conditions for widths 1-129 and heights 1-9,
plus zero and overflow rejection. Result: 4727 checks, zero failures; kernel-flag
compilation also passes. The surface-layout test is now a mandatory quick gate.

All 12 quick gates pass through tools/quality/quick.cmd. The earlier direct
quick.ps1 invocation failed at the ABI compilation gate without the wrapper's
compiler environment; that failed log is retained alongside the passing run.

These are development-host checks. No new lab deployment, GDI interop capability,
Present callback execution, or G0 acceptance follows from this result. The local
KMD154 package predates this change. Staging placement, A8 interpretation,
shared backing identity and cache ordering remain separate requirements.

Logs are copied unchanged. No private identifiers were removed.
