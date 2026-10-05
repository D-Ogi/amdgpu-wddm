# Index buffer binding after DISCARD

Hypothesis: replacing a bound index buffer's VkBuffer on DISCARD leaves
ctx->index_buffer caching the same pipe_resource, so the next draw skips the
required bind. A flush or forced batch-start state masks the stale binding.

The eighth state case initializes each2048-byte backing store completely:
all indices are zero except the current32-byte region, which moves each draw.
It keeps the same ID3D11Buffer and format, with nonzero binding offset/BaseVertex.
The previous backing therefore supplies degenerate indices at the new position.
WARP111 and CPU112 are positive controls; GPU113 is the negative control.

Clear the context's cached index-buffer identity when invalidate_buffer replaces
that resource's backing object. No per-draw flush/wait or broad state emission.
Apply index-discard.patch after idle-discard.patch and other existing hosted
changes, verifying exact pre/post hashes. Candidate051 UMD49AFB21F, control114
EXE7492F85A, hosted ICD3508416F. Require all64 state images to match both positive
controls, including both submission patterns. Sharing/Present and bounded DWM
validation remain required before promotion. M591 records this control only.

PROVENANCE: Mesa (https://gitlab.freedesktop.org/mesa/mesa), MIT.
