# Native M14 window control (not yet run)

Hypothesis: the measured native M14 renderer also creates a system DXGI
FLIP_DISCARD swap chain, presents three frames of each frame scene with S_OK,
and preserves the corresponding runtime011 image checksums. Both CPU and GPU
are checked against their own frozen offscreen references, not against each
other. Same-GPU per-app equivalence is separately measured in M739.

Use a fresh C:\BC250\m14\window002 stage and independent SYSTEM supervisor
with the runtime001 180-second bound/170-second restore reserve. Capture,
Install, Restore and Verify stay in session0. Cpu/Gpu controllers also remain
SYSTEM for the privileged temperature query. They launch only interactive-client
through the M740-validated bounded-child --active-console path, nested inside
the outer phase Job. User-side scripts own logs and set their process environment;
no privileged telemetry runs in the user session. Parent polls STOP/temperature
and kills the helper on failure; nested Job closure kills its client tree. The
outer phase receipt must still prove every descendant stopped before restoration.
A positive user-session and inner job-empty receipt is required on success.
This fixes window001's measured telemetry access-denied admission failure.

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
