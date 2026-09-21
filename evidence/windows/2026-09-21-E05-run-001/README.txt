E05 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, test signing on, KDNET debugger attached.
Instrument: Microsoft KMDOD sample, unmodified (Windows-driver-samples @ 3c3fb49), our INF matching only
PCI\VEN_1002&DEV_13FE, built by experiments/E05-display-only-owns-device/build.ps1, test-signed.
Driven from the lab PC over SSH with experiments/E05-display-only-owns-device/e05_target.ps1.

Order of events (file name suffix is the target's local time, HHMMSS):
  state-before-070859.txt            Basic Display on the GPU, 1920x1200
  sweep-GC-before-070904.log         bc250rd GC sweep, 4537 registers     (first sweep since boot)
  sweep-GC-before2-070944.log        the same again, nothing changed in between: noise floor
  state-before-install / install-071019.txt / state-after-install-071019.txt
                                     pnputil /add-driver /install: device switches to the sample, no problem
                                     code, 1920x1200 kept, no reboot
  sweep-GC-after-071043.log          sweep under the sample
  cycle-071245.txt, state-disabled, state-reenabled
                                     pnputil /disable-device, /enable-device: comes back OK
  state-after-reboot-071457.txt      after a restart: the sample starts by itself, OK, 1920x1200
  rollback-071515.txt, state-after-rollback-071515.txt
                                     pnputil /delete-driver /uninstall /force: Basic Display is back, OK
  comparison.txt                     compare.py over the three sweeps
  screen-*.jpg                       half-scale captures taken through bc250mon (re-encoded from PNG, quality 70).
                                     A capture shows what the desktop compositor holds, not what the monitor
                                     shows: the owner, who was using the screen, is the witness for the panel.
                                     screen-4 shows only a quarter of the desktop: after the rollback Windows
                                     chose 200 % scaling and the already running monitor process was not aware
                                     of the new DPI.

Sweep logs are UTF-16 (PowerShell redirect); compare.py reads both encodings. Untouched since capture.
The debugger log of the session stays outside the repo (P:\BC-250\scratch\kd); it shows no bugcheck.
