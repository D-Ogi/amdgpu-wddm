E06 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, test signing on, no kernel debugger attached.
Instrument: our own miniport driver/kmd (bc250kmd 0.3.1.0) at commit "driver/kmd: review before first load ...",
built by driver/kmd/build.ps1, test-signed. Driven from the lab PC over SSH with
experiments/E05-display-only-owns-device/e05_target.ps1 -Package C:\BC250\e06 -InfName bc250kmd.inf, and a
small script that runs tools/win/bc250kmd_cli (stages, info) and dumps the Parameters key (kmd-*.txt).

Order of events (file name suffix is the target's local time, HHMMSS):
  state-before-080054.txt             Basic Display on the GPU, 1920x1200
  sweep-GC-before-080101.log          bc250rd GC sweep, 4512 registers (allow-list without the side-effect
  sweep-GC-before2-080133.log         registers of facts M25); the pair is the noise floor: 9 registers
  install-080500.txt, state-*-080500  pnputil /add-driver /install: device switches to bc250kmd, no problem code,
                                      1920x1200 kept, no reboot
  kmd-after-install-080531.txt        LastStage 61, history 10 20 30 31 32 33 34 39 50 60 61, D3DKMTEscape with
                                      BC250_ESCAPE_GET_INFO returns STATUS_SUCCESS and the driver's data
  sweep-GC-after-080550.log           sweep under bc250kmd
  comparison.txt                      compare.py: 0 registers outside the noise set
  cycle-080651.txt, state-disabled, state-reenabled, kmd-after-cycle-080708.txt
                                      disable/enable: comes back OK, stage 61, UnconfirmedStarts 1 (the monitor
                                      confirms once per boot)
  state-after-reboot-080939.txt, kmd-after-reboot-080946.txt
                                      restart: the driver starts by itself with UnconfirmedStarts 1 -> 2 (the edge
                                      of the budget), the monitor's confirmation brings it to 0 about a minute later
  rollback-081041.txt, state-after-rollback-081041.txt
                                      pnputil /delete-driver /uninstall /force: Basic Display is back, OK
  install-081053.txt, state-*-081053, kmd-after-reinstall-081106.txt
                                      installed again, stage 61, escape OK; the driver stays installed
  screen-after-reinstall.jpg          half-scale capture through bc250mon. A capture shows what the compositor
                                      holds; the owner, at the screen, reported that the picture fills the panel.
  sweep-interleaved.log               side measurement, taken before the install: every GC register followed by a
                                      read of GRBM_READ_ERROR. The error value was already latched from the sweep
                                      before it and did not change, so this log does not say which read faults.

Sweep logs are UTF-16 (PowerShell redirect); compare.py reads both encodings. Untouched since capture.
