E19 run 001, 2026-09-21 23:53 to 23:56: M6's bring-up under the full WDDM table. bc250kmd 0.7.12 (commit 24c9ba5,
.sys sha256 prefix f34f0988f12561fd, the build of E18 run 003 - no new package), boot 23:42:20.

Gates: EnableFullWddm 1 (one shot), EnableMmio, EnableVram, EnableVramWrite, EnableGpuVa, EnableGart, EnablePsp,
EnableGfx, EnableIh = 1. run-console.txt is the driving script's filtered output (scratch\tmp\e19_run1.sh); the
per-step files are the target's own logs.

  gart enable            sequence result 0, 285 writes
  psp load               every command rc 0 status 0
  ih init                21 register writes, ring ENABLED
  gfx run 8              stages 1 to 8 all rc 0, 462 register writes, 16 doorbells
  fence gfx x3           3 of 3 read back, 34 us in all
  fence c0 x16 dispatch  check 0: every dword as asked
  ih state               3 interrupts, 4 vectors, 0 overflows
  full table meanwhile   102 presents, 102 virtual submissions completed in software, no TDR; vidmm 280643 entries
  gfx fini, ih fini      result 0 (147 writes and 4 doorbells; 9 writes)
  gate closed            display-only table, stage 61, presents counting. 72.6 to 75.8 C.
