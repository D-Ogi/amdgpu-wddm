# Linux multi-window timeout and Xorg recovery (M520)

Same graphics artifacts and unit as M517-M519. Full007 executes five passes
then times out at45s in glx-multi-window-single-context. Process is terminated
by the runner; SSH remains responsive and no GPU ring timeout/reset/fault
appears in the filtered kernel check. Clock1000MHz, GPU64C.

Those checks do not establish rendering health: the unchanged swapbuffers
positive control after the timeout aborts in Zink kopper_acquire with the
acquisition-retry assertion, exit134. Preserve this negative control.

Only Xorg is terminated and relaunched with the same RadeonSI server command.
No amdgpu unload/reload/recover, OS restart or plug operation occurs. The same
swapbuffers control then exits0 with correct pixel probes and true swapping.
This demonstrates recovery of this presentation control, not GPU reset.

On the recovered session the unchanged multi-window executable with native
RadeonSI returns fail/exit1, black probes across windows, without hitting its
45s timeout. Native failure and Zink timeout are different outcomes; neither
is a pass or accepted equivalence. The profile remains stopped. Next localize
the Zink wait; the exact-remainder tool deliberately rejects timeout resumes.
