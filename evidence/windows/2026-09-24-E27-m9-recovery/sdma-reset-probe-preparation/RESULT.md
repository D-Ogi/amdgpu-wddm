# M386 - SDMA timeout probe prepared, not run

Upstream6.18.52 amdgpu_debugfs_reset_work explicitly sets NEED_FULL_RESET and
calls amdgpu_device_gpu_recover with no timed-out job. Reading amdgpu_gpu_recover
therefore does not exercise the timeout-driven per-queue path identifiedM385.
The inspected sdma_reset_mask attribute is read-only capability reporting.

Prepared sdma-reset-probe.py using existing E13 generated DRM ABI and BO/context
lifetime helpers. One valid SDMA memory poll, CPU-controlled release and a trailing
write marker. A pre-released mode is the positive byte/fence control. Delayed
release is bounded to5seconds; trace must establish the real timeout and chosen
recovery branch. Submission latency can consume the release delay; no reset
claim follows merely from requesting a delay or observing a fence.

PROVENANCE: libdrm deadlock_tests.c (MIT) supplies the poll pattern; original AMD
navi10_sdma_pkt_open.h (MIT) supplies exact packet field macros. A native C
generator compiled with W4/WX emits poll/write/NOP fields. Source headers,
upstream test, generated packet constants, probe and E13 dependencies are pinned
here with hashes. No hand-entered MMIO address or malformed instruction.

Host validation: Python syntax, required DRM DMA/IB-size fields,10low/high-address
and alignment fixtures pass. Fixtures check poll addresses, payload layout,
trailing marker and NOP padding for4/16/32/64/256byte alignments. This validates
construction only, not Linux submission, CPU/GPU coherence or reset success.

The probe never equates fence completion with reset acceptance: marker success
only means this job executed. Cancelled/recovered jobs are distinct outcomes.
A new context and known-pattern post-reset workload remain required. Before
hardware use, prepare exact reset entry/return tracing and record runtime module,
reset masks, debug policy and lockup timeout. Do not run this probe as an
unobserved stress loop or use manual full reset as its substitute.

No lab action. Current119 initialized Windows session retained;120 undeployed.
The hardware trial and full M9 are unfinished.
