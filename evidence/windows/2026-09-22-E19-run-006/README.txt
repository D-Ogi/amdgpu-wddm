E19 run 006, 2026-09-22 01:43 to 01:47: H4, the content of a WDDM command buffer executed at VMID 1. bc250kmd 0.7.13
(commit 898706c, .sys sha256 prefix 9692e1996355551d), kmtprobe with --scratch (commit 636b1e7), boot 01:40:28 (a
fresh boot, not started by this session: the warm restart issued at about 01:27 never came up, compare facts M21).
Full table, EnableGpuVa = EnableGpuSubmit = 1, engine gates open. run-006-console.txt is the driving script's output
(scratch\tmp\e19_run6.sh); the other files are the target's own logs of the same steps.

  bring-up      gart enable 285 writes, psp load rc 0, ih init, gfx run 8: 462 writes, 16 doorbells, all 0
  control       fence gfx x1: 1 of 1, slot 0x1, 12 us
  H2 again      fence gfx x1 ib: IB 0x564000, 3 dwords, VMID 0, FETCHED; IH 1 -> 2
  read before   bc250kmd_cli read 30100 -> DEADBEEF   (the IB ring test's value: the read-back works and the register
                                                       holds something the next step does not write)
  H4 client     kmtprobe --submit --scratch BC250B01: PM4 C0017900 00000040 BC250B01 then NOPs, 64 bytes at GPU VA
                0x20000000 on context 0x400001C0, SubmitCommand STATUS_SUCCESS, monitored fence 1, submit=ok
  H4 driver     ring log 184-193: CreateContext flags 0x4; root 1:0x1FD729000 = physical 0x46DFF3000; `gfx: VMID 1 root
                0x46DFF3000 -> 0`; `fence 102 on the gfx ring: sequence 3, vmid 1, ... va 0x20000000, 64 bytes`;
                `hardware fence arrived, reporting fence 102`
  read after    bc250kmd_cli read 30100 -> BC250B01
  IH            2 -> 3 interrupts, 2 -> 3 vectors (client 20 source 181 ring 0, M79), 0 overflows
  table         no TDR, no watchdog line, no bugcheck, no live kernel report
  undo          gfx fini, ih fini, psp unload, gart restore: all 0; gate closed, stage 61, presents counting. 70 to 74 C.
