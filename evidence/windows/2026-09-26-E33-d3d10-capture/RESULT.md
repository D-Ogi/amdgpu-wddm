# D3D10 Instancing capture

PROVENANCE: DirectX-SDK-Samples MIT; DXVK zlib; Mesa MIT.

Unit A, unchanged KMD151 and Mesa8B5EC055. Diagnostic005 proves the failing
capture source is1080x720, R8G8B8A8_UNORM_SRGB (29), single sample, default
usage and render-target binding. QueryInterface succeeds; direct D3DX PNG
save still returns80004005. This does not isolate D3DX's internal failing call.

Candidate006 copies the final resolved frame to a CPU-readable UNORM staging
texture, maps/unmaps it to verify readback, then asks D3DX to encode that
texture. No rendering, shader, scene, frame count or resolution changes.
The copy preserves encoded pixel bits; it does not apply a gamma conversion.
Both staging creation and Map succeed; RowPitch4320 matches1080 RGBA pixels.

Run006 completes660 ordered CPU submission rows in14780ms, exit0, and all
five required modules are witnessed. The1080x720 PNG visibly contains the
island, tree, grass and sky. Full image and hash are retained here. GPU health
retains generation560773206/epoch5; registry baseline restored without reboot.
Capture timing is outside the CPU render-submit metric. Neither that metric
nor this capture run establishes performance acceptance.

The complete four-file patch replays onto07e3eaa1 and matches compiled source
with normalized LF. The tested manifest retains the prior base patch hash plus
a diagnostic description; patch-replay.json binds the complete final patch to
the exact tested executable hash. No dependency or driver promotion occurred.

All four Direct3D generations now have a complete application capture run
(M497-M500), but Linux/Proton image and performance comparisons remain open.
Full Vulkan CTS, sparse completion, OpenGL and OpenCL acceptance remain open.

Reference gap: local driver/DDI and DirectX-Specs files did not contain the
D3D10 application CopyResource contract. Microsoft's API reference confirms
compatible format groups and byte-copy semantics without format conversion:
https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-copyresource
Consulted2026-09-26. No claim that all D3DX sRGB captures fail is made.
