# Native D3D11 resize/lifetime control

Hypothesis: the M747 runtime-backed DISPLAYABLE surfaces survive three
ResizeBuffers calls and repeated flip-discard identity rotation with exact
content, through both CPU and native hosted GPU routes.

Use the window006 independent SYSTEM supervisor and active-console launcher,
with fresh resize001 identities, exact frozen engine253A/ICDC0CE and UMD96615291.
Replace the client with the resize-scene build. Each phase has its existing
bounded debugger/Job/deadline; overall <=180 seconds with independent CPU171
restoration, thermal/STOP monitoring, and verified process-tree closure.
CPU control must pass first. Only then run the same native GPU client.

Expected success: twelve successful Presents and exact per-frame colors across
64x64,65x33,127x79,64x64; CPU/GPU hashes identical, restoration/postflight pass.
Any API failure, wrong pixel, missing step, timeout or cleanup uncertainty fails.
The synchronized readback is intentionally a correctness instrument; this does
not claim performance or no-copy Present or independently measured composition.

Result: M748 passes on CPU and GPU; all24 pulled images match exact expected colors. CPU171 restored43.030s, task Missing. See evidence/windows/2026-09-28-E34-m14-resize001.
