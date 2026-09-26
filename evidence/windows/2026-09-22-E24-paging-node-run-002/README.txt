E24 run 002, 2026-09-22 10:36-10:47: bc250kmd 0.7.24 (f14c11c) on unit A, owner at the monitor. The control run
for run 001: the same bring-up with the paging node gate CLOSED, and one escape call per stage so that the console
names the stage that dies.

Result: stages 1 to 5 returned (doorbell aperture, golden registers, GRBM CAM probe, constants, RLC); stage 6, the
CP, never returned and the machine wedged exactly as in run 001 - no output, both addresses dead, monitor black,
no bugcheck, no TDR. The owner's power button was the way back.

What it proves is not what it was written to prove: the paging node was closed, so the node is not the cause. This
run was the SECOND bring-up of its boot (E22 run 004 had done gart, psp and ih at 09:58 in the same boot), which
is exactly the shape of fact M78. Run 003, from a fresh boot with the node open, then completed all eight stages.
