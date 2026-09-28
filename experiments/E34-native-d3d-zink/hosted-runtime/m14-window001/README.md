# Native M14 window control (not yet run)

Hypothesis: the measured native M14 renderer also creates a system DXGI
FLIP_DISCARD swap chain, presents three frames of each frame scene with S_OK,
and preserves the corresponding runtime011 image checksums. Both CPU and GPU
are checked against their own frozen offscreen references, not against each
other. Same-GPU per-app equivalence is separately measured in M739.

Use a fresh C:\BC250\m14\window001 stage and independent SYSTEM supervisor
with the runtime001 180-second bound/170-second restore reserve. Capture,
Install, Restore and Verify stay in session0. Cpu/Gpu use the M740-validated
bounded-child --active-console path; scripts log inside that session, because
cross-session handle inheritance is prohibited. A positive session receipt and
job-empty receipt are required before restoration. Failures must retain logs
and restore the CPU UMD through the existing verified file transaction.

Router selects only this exact client's path, explicit GPU flag and60s enable
lease. DWM remains CPU171. Use shell/runtime011, engine253A, ICDC0CE and frozen
client84C328CD; no application-local D3D DLLs. Client uses a visible64x64 window,
8 draws,2 fill layers,4 shader variants,3 frames,zero warmup,20s deadline.
The debugger has25s and each interactive phase38s. Shader scene retains its
separate256x256 texture. Copy for image validation occurs before final Present;
that deliberate validation readback is not evidence of a no-copy presenter.

Stage scripts derive from m14-runtime001 with literal identity replacement;
use this directory's overrides plus the updated invoke-bounded.ps1 and helper
from kmd168-transition. Preserve manifest hashes for every artifact. Build
router and selection tests with build.ps1. Run scene-gate positive/negative
fixtures; verify all staged scripts parse before admission. No measured
performance claim is allowed from this debugger/three-frame trial.
