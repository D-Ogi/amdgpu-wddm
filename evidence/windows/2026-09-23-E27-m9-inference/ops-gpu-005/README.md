# Run005: synchronization contrasts and node samples

Unit A, boot 2026-09-23T02:37:26, KMD 0.7.56.1, upstream llama.cpp b9564. CPU16 correct; baseline GPU repeats s; disabling async, disabling host-visible video-memory selection, or both repeats #. Every process exits 0. The memory-selection flag may be inert on UMA; no actual staging-path change is established.

Upstream llama-eval-callback built with the same dynamic backend setup reads each node. CPU and GPU runs print the same CPU GET_ROWS embedding samples and sum -3.354056. The first GPU RMS_NORM norm-0 differs: CPU sum -4.806195, GPU sum 1.954104. The next MUL and MUL_MAT print the same GPU samples/sum as norm-0 despite distinct operations. This locates the first observed divergence, not its cause: input upload, execution and readback are not separated yet. Callback reads synchronize execution.

Display-only restored, UnconfirmedStarts 0 at 02:41:58. No acceptance or fix is claimed. Raw callback output is retained; the trace stream repeats it.
