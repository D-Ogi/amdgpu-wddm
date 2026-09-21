E11 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.5.3.0, 0.5.4.0, then 0.5.5.0 (imports from
mainline v6.18), witness bc250rd with the 5355-offset allow-list. No kernel debugger attached. Driven over SSH with
experiments/E11-gfx-bringup/e11_target.ps1. File name suffix is the target's local time. sweep-plan-<tag>-*.txt are
the script's own lines of each sweep phase. Firmware as in E10 (linux-firmware commit
2b8daaf611fbade74f26a5b58ec1defe6a02f5e0, not in this repository). The target was rebooted before each of the four
parts (one PSP load per boot, facts M35). comparison.txt is experiments/E11-gfx-bringup/compare_run001.sh's output.

attempt-0.5.3/   boot 11:21. H1 and H2 held, PSP load as in E10. "gfx plan 5" stopped in stage 3: the GRBM CAM probe
                 writes a pattern and reads it back through an alias, and a plan executes no write. Nothing was
                 executed. Led to 0.5.4: a plan answers a read of a register it has planned a write to.
attempt-0.5.4/   boot 13:03. The plan ran through stage 5; compare.py showed every line after write 17 shifted by one:
                 the golden setting GCMC_VM_CACHEABLE_DRAM_ADDRESS_END was missing from the comparison's trace filter
                 AND from the driver's generated register table (both excluded GCMC_*), so a run would have stopped
                 with a refused write in stage 2. Nothing was executed. Led to 0.5.5.
(this directory) boot 13:10, bc250kmd 0.5.5.0, the run proper:
  install-plan-x-131143.txt        0.5.5.0 installed, gates closed by the INF, stage 61
  gfx-state-closed-131155.txt      H1: refused, STATUS_DEVICE_NOT_READY (the state query as well: H1 said it would answer)
  gate-plan-x-131200.txt           five gates opened, device restarted
  sweep-*-before, before2          control sweeps (GC, MMHUB, MP0, NBIO)
  gart-enable-x-131324.txt         E09's sequence, 285 writes
  gfx-run-nopsp-131330.txt         H2: refused, 0xC0000184 STATUS_INVALID_DEVICE_STATE, nothing written
  psp-load-x-131336.txt            E10's load, 11 of 11 commands, status 0
  sweep-*-loaded
  gfx-plan-x-131418.txt            H3: 203 writes planned for stages 1 to 5, none executed
  sweep-*-afterplan                H3: nothing moved
  gfx-run-s1 .. s7, sweep-*-s1 .. s7   H4 to H8: one stage per call, a sweep after each
  gfx-fini-fini-132058.txt, sweep-*-fini     H9a
  gate-plan-afterfini-132156.txt, gfx-state-afterrestart-132215.txt, sweep-*-restarted   device restart after the undo
  gate-plan-end-132311.txt         gates closed
boot-2/          boot 13:23, bc250kmd 0.5.5.0 still installed: gates, GART, PSP load, all seven stages in ONE call
                 (gfx-run-all), sweep "running", then H10: device restart while the engines ran
                 (gate-plan-whilerunning), state and sweep of the new driver instance, gates closed.
H9b (a second run after the undo) was not attempted: predicted to fail before the run (README of the experiment).
