E07 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.4.1.0 (commit "ADR 0007 and the first
bring-up step ..."), witness bc250rd with the 5355-offset allow-list. No kernel debugger attached.
Driven over SSH with experiments/E07-first-register-write/e07_target.ps1 (and the E05 script for the sweeps).
File name suffix is the target's local time.

  install-x-082344.txt            package installed, gates closed by the INF, stage 61 via the new stage 35
  probe-closed-082354.txt         H1: every register escape refused, STATUS_DEVICE_NOT_READY, "not mapped"
  gate-read-082407.txt            EnableMmio = 1, device restarted
  probe-readgate-082418.txt       H2: six registers identical through bc250kmd and bc250rd; GRBM_GFX_CNTL (on no
                                  list) refused with STATUS_ACCESS_DENIED; a write refused (gate closed)
  gate-write-082439.txt           EnableMmioWrite = 1, device restarted
  sweep-GC-before / before2       noise floor
  write-cafedead-082557.txt       H3: SCRATCH_REG0 = CAFEDEAD, SCRATCH_REG1 = 0BC25001 written through the escape,
                                  read back by the driver and, independently, by bc250rd
  sweep-GC-after-082603.log, comparison.txt
                                  H4: exactly those two registers differ outside the noise set
  probe-writegate-082641.txt      with the write gate open: a write to GRBM_SCRATCH_REG0 (not on the write table)
                                  refused with STATUS_ACCESS_DENIED, value unchanged
  write-restore-082647.txt        H5: both registers back to 0, witnessed
  gate-close-082653.txt, probe-closed-again-082704.txt
                                  gates closed, device restarted, escapes refused again

Sweep logs are UTF-16 (PowerShell redirect). Untouched since capture.
