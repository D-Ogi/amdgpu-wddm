E15 run 002, 2026-09-21, unit A, Windows 11 Pro, bc250kmd 0.7.0.0 (source state: commit 32df400, built from a clean
worktree of that commit; the EnableFullWddm gate closed), witness bc250rd. No kernel debugger session. Driven over SSH
with experiments/E15-compute-dispatch/e15_target.ps1 -Package C:\BC250\e15b, one script for the whole run
(P:\BC-250\scratch\tmp\e15b_run.sh: it stops at the first step that does not end with exit code 0; none did). File
name suffix is the target's local time. Nothing here was edited by hand. 69 to 71 C. Fact M60.

Boot 18:28 (a restart for this run, so that both SDMA engines start with their pointers at 0):
  install-plan-x-*                 0.7.0.0 installed, stage 61
  gate-plan-x-*, gart-enable-*, psp-load-*, ih-init-*
  Three times, tags first, second, third, inside ONE device start:
    gfx-run-<tag>-*                stages 1 to 8, all rc 0. Stage 7 now ends with both SDMA ring tests (2 more doorbells
                                   than 0.6.2: 16); in the second and third run it also carries the adoption's reads
    sweep-*-<tag>-before-*         witness, before any submission of ours beyond stage 7's ring tests
    fence-s0 / fence-s1 (test)     ring tests: 0xDEADBEEF arrives, every time
    fence-s0 x3, fence-s1 x2, fence-gfx x3, fence-c0 x16 dispatch (check 0, every time)
    sweep-*-<tag>-after-*
    ih-state-<tag>-*
    gfx-fini-undo-<tag>-*          147 writes, 4 doorbells, result 0 (so: both engines halted with rptr == wptr)
  ih-fini-final-*, gate-plan-closed-*

The SDMA pointers (SDMAn_GFX_RB_RPTR = RB_WPTR = RB_RPTR_FETCH in every sweep), bytes:
                 first-before  first-after  second-before  second-after  third-before  third-after
  SDMA0              0x40         0x140         0x180          0x280         0x2C0        0x3C0
  SDMA1              0x40         0x100         0x140          0x200         0x240        0x300
They count on across the undo and the new ring base (0x55E0 / 0x6860 / 0x7AE0 for SDMA0): each bring-up adopted the
value the previous one ended with, and its first submission (stage 7's ring test, 0x40 bytes) moved the engine.

Two vectors nobody ordered, both from SDMA0 (client 8), one per re-init, none in the first bring-up and none from SDMA1:
  between the first and the second run's fences:  source 244 (SDMA_DOORBELL_INVALID in irqsrcs_sdma0_5_0.h), data 00000280
  between the second and the third run's fences:  source 0, data 00000500
Neither stopped anything: every submission after them completed. Where exactly they are raised (the undo, the un-halt,
the adoption's WPTR write, the first doorbell) is not separated by this run; ih state was read once per bring-up.
