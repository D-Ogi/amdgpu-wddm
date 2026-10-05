# M386 - Prepare source-derived SDMA scheduler timeout probe

Goal: distinguish timeout-driven PER_QUEUE SDMA reset from debugfs full reset.
Use the upstream libdrm deadlock_tests.c amdgpu_deadlock_sdma memory-poll pattern,
not malformed commands. Prepare one task, positive control releases poll before
submission; diagnostic mode releases from a CPU thread after bounded delay.
Append a write marker to prove packet execution; a signaled fence alone is not
proof of data execution after recovery. No hardware run in preparation step.

Generate packet words from original AMD navi10 SDMA macros; reuse E13 generated
DRM UAPI layouts and Session BO/context ownership. Query DMA available rings and
IB alignment; keep all backing until wait completes, release word in finally.
Report recovered/cancelled job separately from marker PASS. New context/post
reset byte control required in a later hardware plan before accepting recovery.

Before a Linux trial: source-matched trace preparation, exact module/package
identity, actual reset masks/debug flags, first-load SDMA byte control, bounded
lockup_timeout policy matching libdrm's documented test setup. No debugfs full
reset and no uninstrumented reload. This preparation does not authorize a claim
that a delayed poll will necessarily trigger reset or that the reset will work.
