E10 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.5.1.0 then 0.5.2.0 (imports from mainline
v6.18), witness bc250rd with the 5355-offset allow-list. No kernel debugger attached. Driven over SSH with
experiments/E10-psp-firmware-load/e10_target.ps1. File name suffix is the target's local time.
sweep-plan-<tag>-*.txt are the script's own lines of each sweep phase. Firmware: linux-firmware commit
2b8daaf611fbade74f26a5b58ec1defe6a02f5e0, not in this repository; sizes and SHA-256 in firmware-plan-x-110353.txt.

  firmware-plan-x-110353.txt      the eight firmware files on the target (equal to the host's copies)
  install-plan-x-110402.txt       0.5.1.0 installed, six gates closed by the INF, stage 61
  psp-plan-closed-110416.txt      H1: refused, STATUS_DEVICE_NOT_READY
  gate-plan-x-110425.txt          four gates opened, device restarted
  sweep-*-before, before2         control sweeps (GC, MMHUB, MP0)
    -- a review of 0.5.1 came back at this point; 0.5.2.0 has its fixes (fence poll that sleeps, firmware path in
       the global DOS device directory, two defensive checks). Nothing but plan-closed had been run on 0.5.1. --
  install-plan-v052-110700.txt    0.5.2.0 installed, gates closed again by the INF
  psp-plan-closed-110709.txt      H1 again
  gate-plan-x-110715.txt          gates opened, device restarted
  sweep-*-before3                 third control sweep
  psp-plan-plan-110805.txt        H2: 4 register writes planned, none executed, 11 commands planned
  sweep-*-afterplan               H2: nothing changed
  psp-load-nogart-110904.txt      H3: refused, 0xC0000184 STATUS_INVALID_DEVICE_STATE, nothing written
  gart-enable-e10-110909.txt      E09's sequence, 285 writes
  sweep-*-gart                    control for the load
  psp-load-load-110956.txt        H4: 15 writes executed, 11 of 11 commands, every status 0
  sweep-*-loaded                  H4, H5
  psp-unload-unload-111045.txt    H6: DESTROY_TMR and ring stop, 2 writes
  sweep-*-unloaded
  psp-load-second-111143.txt      H6: second load, 11 of 11 again
  sweep-*-second
  gate-plan-h7-111245.txt         H7: device restarted while loaded, no unload command
  psp-plan-afterstop-111304.txt   the new driver instance: no ring, no TMR
  sweep-*-afterstop               H7: C2PMSG_64 = 0x80030000, C2PMSG_67 = 0xC0: the driver unloaded by itself
  gate-plan-close-111407.txt      gates closed, device restarted
  sweep-*-afterreboot             after a warm reboot of the target (11:21): SDMA halted again, PSP mailbox zero
  comparison.txt                  experiments/E10-psp-firmware-load/compare_run001.sh over the above (made on the
                                  lab PC)

All logs from the target are UTF-16. Untouched since capture.
