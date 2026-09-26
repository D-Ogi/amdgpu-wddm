# Linux GLX swapchain destruction wait (M521)

Same boot and graphics binaries as M520, Xorg PID5668. GDB and strace installed
only in the lab Debian build image. No driver, GPU, OS or Xorg reset this run.

Standalone multi-window control and direct five-predecessor replay both exit1
with failed pixels, without reaching the delayed debugger attachment. Both
are preceded by unchanged swapbuffers controls with pixel-pass/exit0.
These diagnostics do not replace the original timeout or count as new coverage.

Replaying exactly the six full007 names through the unchanged serial piglit
runner reproduces five passes and one45s timeout (runner exit3). During this
wait GDB attaches, records all thread stacks, and detaches before timeout.
The stack is diagnostic evidence; debugger timing may perturb the execution.

Observed dependency: main thread waits in zink_image_map -> zink_fence_wait ->
sync_flush -> util_queue_fence_wait. Zink queue worker zfq0 is inside
x11_swapchain_destroy -> thrd_join. The matching chain0x55ddc614cd10 event
worker blocks in xcb_wait_for_special_event. Its present queue worker is no
longer visible. This localizes the wait to Linux X11 swapchain teardown;
it does not establish a GPU hardware hang or a Windows WSI defect.

Local Mesa05e6c962 source: x11_swapchain_destroy sets negative status,
broadcasts the condition, wakes the present queue, then joins both managers.
The event manager can be outside that condition, blocked on an XCB special
event. Hypothesis: this wait lacks a teardown wake-up when the expected event
never arrives. Prove an interruptible wait/wake candidate with the unchanged
reproducer and positive control before resuming the full profile. No fix or
waiver is claimed. All workers terminal; Xorg remains running. Original
full003/006/007 outcomes remain authoritative and full acceptance remains open.
