E09 run 001, 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.4.3.0 (imports from mainline v6.18,
commit "M4: import gfxhub_v2_0 and mmhub_v2_0 ..."), witness bc250rd with the 5355-offset allow-list.
No kernel debugger attached. Driven over SSH with experiments/E09-gart-enable/e09_target.ps1. File name
suffix is the target's local time. sweep-<tag>-*.txt are the script's own lines of each sweep phase.

  install-x-095845.txt          package installed, five gates closed by the INF, stage 61
  plan-closed-095857.txt        H1: refused, STATUS_DEVICE_NOT_READY
  gate-x-095907.txt             EnableMmio, EnableVram, EnableGart = 1, device restarted
  sweep-*-before / before2      control: firmware state and noise set (GC and MMHUB)
  plan-x-100027.txt             H2: 285 writes planned, none executed, table/scratch/dummy addresses
  sweep-*-afterplan             H2: nothing changed
  enable-x-100146.txt           H3: sequence result 0, 285 writes executed, no refused register
  sweep-*-enabled               H3: state through the witness
  restore-x-100314.txt          H4: 282 writes (the snapshot; protocol registers are not written back)
  sweep-*-restored              H4
  enable-second-100354.txt      enabled again, then
  gate-x-100400.txt             the device restarted with GART enabled: the driver restores at stop
  sweep-*-afterstop, plan-afterstop-100445.txt
                                H5: firmware state; the new driver instance holds no snapshot and is not enabled
  gate-x-100533.txt, plan-closed-again-100544.txt
                                gates closed, device restarted, command refused again
  comparison.txt                experiments/E09-gart-enable/compare.py over the above (made on the lab PC)

All logs from the target are UTF-16. Untouched since capture.
