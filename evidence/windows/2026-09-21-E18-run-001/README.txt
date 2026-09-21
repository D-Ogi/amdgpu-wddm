E18 run 001, 2026-09-21: bc250kmd 0.7.10 (commit a5befa3), UMD stub package 0.7.10.1, EnableGpuVa = 0 (plan)
===========================================================================================================

Package frozen by tools/packagecheck (package-manifest.txt, PASS). bc250kmd.sys sha256 prefix 7460df514c983f51.
Nothing is written by this build with the gate closed; the run reads what VidMm sends.

  23:25:18  install, gates closed, stage 61, confirmed.
  23:26:02  gate -Full 1 -GpuVa 0. Full table up as in E16 run 009 (stage 50, presents and fences, no TDR).
  23:26:33  ring-b1, 23:26:39 ring-summary5 (with the vidmm summary).

What the ring says (ring-b1-232633.log):
  lines 38-46   1028 UpdatePageTable calls before the first root: UpdateMode 0 = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL,
                level 0, start 0, count 512; PageTableAddress read as CpuVirtual is a pointer that advances by 4096
                per call (printed by this build as "table <low dword>:0x0", e.g. 2146361344 = 0x7FEEE000; the high dword is not in this log). 0.7.10
                refuses them as calls ("bad calls 1028"): it only knew the GPU_PHYSICAL mode it declares.
  line 47       SetRootPageTable 1:0x0 for the system paging process.
  lines 67-84   the CDD process, GPU_PHYSICAL mode: level 0 table 1:0x1FD732000, level 1 table 1:0x1FD733000 whose
                entry 0 is raw Flags 0x21 (Valid, Segment 1) / PageAddress 0x1FD732; level 2 table 1:0x1FD734000 entry
                0 = 0x21 / 0x1FD733; level 3 table 1:0x1FD735000 entry 0 = 0x21 / 0x1FD734; and line 125:
                SetRootPageTable 1:0x1FD735000. So PageAddress is a page frame number (offset >> 12), level 0 is the
                leaf and level 3 is the root.
  line 72       first system memory entry: raw Flags 0x1 (Valid, Segment 0) / PageAddress 0x2607D4.
  flags         0x3 = Repeat | InitialUpdate with an invalid entry (clear a range), 0x2 = InitialUpdate.
  summary       level 0: 36 calls 14395 entries 9002 valid; level 1: 3 / 520 / 9; level 2: 1 / 512 / 1; level 3:
                1 / 512 / 1. Segment 0 named by 2 valid entries, segment 1 by 9011. All 9011 valid entries refused
                (rc -22): the translator was configured for byte addresses and refuses what does not fit, as designed.
