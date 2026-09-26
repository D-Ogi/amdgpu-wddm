# CPU WSI acquisition rotation (M514)

PROVENANCE: Mesa (MIT), piglit (MIT), revisions in the preceding M503/M512/M513 evidence.

Full piglit006 stopped at 1267 terminal cases: 1102 pass, 164 skip, one
45-second timeout in `spec@!opengl 1.0@gl-1.0-swapbuffers-behavior`.
All four previously repaired cases passed. GPU generation589506402/epoch5
remained ready; the runner restored the baseline Vulkan registration.

Diagnostic026 adds only flushed stage messages to the upstream test. It
reaches the back-buffer read after successful swap and front-buffer read.
Diagnostic027 additionally logs Zink AcquireNextImageKHR calls: initial
images0/1 are acquired, but back readback repeatedly reacquires image0.
CPU WSI always selects the first idle image while Zink cycles to last_dt_idx.
Both diagnostic runs stop at the same45-second guard; neither is a pass.
The large diagnostic stderr is losslessly gzip-compressed.

The candidate rotates the first CPU idle-image search index under the existing
acquire_mutex. Only idle images are returned; DXGI retains its prior order.
Probe028 uses the unchanged executable pulled from full001, production Zink
1DAF0B60 (without acquisition logging), and candidate RADV B365C281.
It exits0 with the upstream pixel oracle pass and reports a true buffer swap.
Before/after health stays generation589506402/epoch5; registration is restored.

This is a measured fix for this integration case, not full OpenGL/Vulkan
conformance or Linux parity. Full007 is a separate ongoing regression run.
The patch is incremental on our existing Win32 WSI source, not pristine Mesa.
No secrets, full memory dumps, or firmware are included.
