fbdump runs 001-003, 2026-09-22 ~06:30, bc250kmd 0.7.22 (4c233e3, clean worktree build, .sys sha256 prefix
9d137aa9dc44fec8) display-only on unit A, owner absent. The scanout dump escape reads HUBP0's surface address from the
DCN registers and copies the scanned-out surface in 64-row bands (commit e13546e).
run 001 (not kept separately): EnableMmio alone -> refused "band not inside VRAM or the firmware framebuffer" (the POST
  framebuffer Windows hands the driver is the BAR alias, the DCN address is the system physical 0x270000000; the
  carve-out range is known only with EnableVram).
run 002: EnableMmio + EnableVram -> the device start was refused: CM_PROB_FAILED_POST_START, the driver's own
  UnconfirmedStarts guard (2 starts since the install without the overlay's confirmation, which needs ~30 s of a
  healthy desktop; the restarts came 8 s apart). run-002-console-refused-start.txt, run-002-diag.txt.
run 003: UnconfirmedStarts reset by hand, same gates -> 19 bands, 1920x1200, 9216000 bytes in 396 ms, address
  0x270000000..; scanout-003-half.png is `mon.py scanout` (half scale, pure-Python PNG) of the same instrument taken
  right after. Gates closed again by the script.
scanout-003-half.png: the overlay's panel (top right) is blanked before commit because its System section prints
  the lab's address; the rest of the frame is the raw read-back.
