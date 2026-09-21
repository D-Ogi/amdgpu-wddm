E12 run 001 (part A), 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.6.0.0 (shim with the declared
deviation of driver/amdgpu-import/PROVENANCE.md for a re-init over a halted MEC), witness bc250rd. No kernel debugger
attached. Driven over SSH with experiments/E12-interrupts/e12_target.ps1. File name suffix is the target's local time.
Firmware as in E10. comparison.txt is experiments/E12-interrupts/compare.py rerun plus the last lines of E11's
compare.py writes for the first run.

Boot 13:29 (bc250kmd 0.5.5.0, gates closed, nothing run in this boot):
  install-plan-x-134609.txt        0.6.0.0 installed with pnputil /add-driver /install, stage 61, MSI key present
  ih-state-installed-134619.txt    A2: message (MSI), vector 0x80, straight after the install, no reboot
Boot 13:46:
  ih-state-boot-134828.txt         A1, A2: message (MSI), vector 0x51, interrupt routine called 0 times
  gate-plan-x-134834.txt           six gates opened, device restarted (vector 0x70 from here on)
  ih-state-gated-134845.txt
  sweep-*-before                   control sweeps (GC, MMHUB, MP0, NBIO)
  ih-state-quiet-135141.txt        A3: 0 calls after the sweep and more than two minutes (presents 87 -> 276)
  gart-enable-a, psp-load-a, gfx-run-a     A4: E11's sequence, 355 + 69 writes, all seven stages rc 0
  ih-state-afterrun-135210.txt     0 calls with the engines running
  gfx-fini-a-135222.txt            93 writes, 4 doorbells
  ih-state-aftergfx-135228.txt     0 calls
  gfx-run-rerun-135233.txt         E11 H9b: the whole bring-up AGAIN in the same boot: rc 0 in all stages, 358 + 69
                                   writes, no refused register; stage 6 takes 5222 us (349 us in the first run)
  gfx-state-rerun-135248.txt
  gfx-fini-rerun-135253.txt        93 writes, 4 doorbells
  sweep-*-after
  ih-state-end-135335.txt          0 calls
  gate-plan-x-135341.txt           gates closed, device restarted
  ih-state-closed-135359.txt       0 calls
The target was rebooted afterwards (boot 13:54): message (MSI), vector 0x51, 0 calls, stage 61.
