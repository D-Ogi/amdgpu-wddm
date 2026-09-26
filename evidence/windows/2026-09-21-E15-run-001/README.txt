E15 run 001, 2026-09-21, unit A, Windows 11 Pro, bc250kmd 0.6.2.0 (source state: commit cdcd608, built from a clean
worktree of that commit), witness bc250rd. No kernel debugger session. Driven over SSH with
experiments/E15-compute-dispatch/e15_target.ps1. File name suffix is the target's local time. Firmware as in E10.
comparison.txt is experiments/E12-interrupts/compare.py (irq, rerun) over the logs named in it. Nothing here was edited
by hand. Unit A stayed between 67 and 71 C. Facts M57, M58, M59.

Boot 17:47 (after the Linux session of E13 boots 5 to 9; gates closed):
  install-plan-x-175016            0.6.2.0 installed, stage 61, MSI key present
  gate-plan-x-175049               eight gates opened, device restarted
  gart-enable-x-175100, psp-load-x-175106 (11 of 11 commands), ih-init-x-175112
  gfx-run-first-175126             stages 1 to 8 in one call, all rc 0; the 35 interrupt-source writes equal amdgpu's
  ih-state-first-175132
  fence-gfx-x-175138 (x3), fence-c0-x-175143 (x3)
  fence-s0-x-175159                THE SDMA RING TEST: 0xDEADBEEF arrives in 3 us (no vector: the test emits no trap)
  fence-s0-x-175205 (x3), fence-s1-x-175210 (x3), ih-state-sdma-175216   vectors client 8 and client 9, source 224, as
                                   under Linux
  fence-c0-x-175221                THE FIRST DISPATCH: 1 workgroup of 64 threads, fill 0x22222222, check 0, 27 us
  fence-c0-x-175227                16 workgroups (the whole 16 KB buffer, libdrm's own test), check 0, 28 us
  ih-state-dispatch-175232         one source 181 vector with ring id 4 per dispatch
  gfx-fini-undo-175251, ih-state-afterundo-175256   the undo with the IH ring on: 147 writes, 4 doorbells
  gfx-run-second-175302            THE SECOND BRING-UP PASSES (E12 run 002 failed here, M44): all eight stages rc 0,
                                   stage 6 in 313 us, every ring at a new GART address
  ih-state-second-175308           no UTCL2 (client 27) vector
  fence-gfx-x-175313 (x3), fence-kiq-x-175319, fence-c5-x-175330 (16 workgroups, check 0), gfx-state-second-175335
  fence-s1-x-175324, fence-s0-x-175356   THE DEFECT: both SDMA ring tests time out after 105 ms, the slot keeps 0xCAFEDEAD
  fence-s0-x-175401, fence-s1-x-175407   refused with STATUS_DEVICE_BUSY: the ring still owes the ring test's value
  ih-state-sdma2-175412
  sweep-*-sdma2-175832             witness: SDMA0_GFX_RB_RPTR = RB_WPTR = RB_RPTR_FETCH = 0x100, SDMA1 0xC0 - the FIRST
                                   run's final values (4 and 3 submissions of 16 dwords), RB_BASE the second run's
  gfx-fini-undo2-180104, sweep-*-afterfini2-180110   halted (F32_CNTL 1), pointers unchanged
  gfx-run-third-180147, sweep-*-third0-180153   third bring-up, swept BEFORE any submission: the writes of 0 to RB_RPTR
                                   and RB_WPTR did not take, 0x100 and 0xC0 again
  fence-s1-x-180230, sweep-*-third1-180236   ring test with doorbell value 0x40: nothing moves, SDMA1_GFX_STATUS 2 -> 3
  ih-state-final-180603            25 interrupts, 35 vectors, 0 overflows in the whole session
  gfx-fini-undo3-180609, ih-fini-final-180614, gate-plan-closed-180620   undone, gates closed, device restarted
