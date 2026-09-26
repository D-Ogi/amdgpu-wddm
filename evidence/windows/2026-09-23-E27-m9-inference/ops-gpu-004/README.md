# GPU operator and inference contrasts, run004

Unit A, boot 2026-09-23T02:27:01, KMD 0.7.56.1, llama.cpp b9564. All 27 selected Vulkan0 comparisons pass: 10 CPY, 6 MUL_MAT and 11 SOFT_MAX. The same selectors passed the CPU control. These shapes do not cover the whole inference graph.

CPU16 produces the expected opening about Lily. Baseline GPU emits repeated s; disabling FP16 emits repeated #; disabling graph optimization and fusion together also emits repeated #. All four inference processes exit 0. None of the contrasts fixes correctness. No performance acceptance is claimed.

Full-mode entry and all explicit engine/fence controls completed. Display-only restored and UnconfirmedStarts confirmed 0 at 02:34:33. Temperature samples are retained. The owner reported a black desktop after this boot before the test; restarting DWM restored the visible desktop, as confirmed by the owner. That observation does not establish a root cause.
