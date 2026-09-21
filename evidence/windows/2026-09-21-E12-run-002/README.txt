E12 run 002 (parts B and C), 2026-09-21, unit A, Windows 11 Pro 22631.3880, bc250kmd 0.6.1.0 (source state: commit
3c1373d), witness bc250rd. No kernel debugger session. Driven over SSH with experiments/E12-interrupts/e12_target.ps1.
File name suffix is the target's local time. Firmware as in E10. comparison.txt is experiments/E12-interrupts/compare.py
(ih, irq, rerun) over the logs named in it. Nothing here was edited by hand. The Linux reference is E13
(evidence/linux/2026-09-21-E13-reference-2/, facts M40).

Boot 15:08 (cold, after the Linux session of E13; bc250kmd 0.6.0.0, gates closed):
  install-plan-x-151908            0.6.1.0 installed, stage 61, MSI key present
  gate-plan-x-151918               eight gates opened, device restarted; vector 0x80
  sweep-*-before                   control sweeps (GC, MMHUB, MP0, NBIO, OSSSYS)
  gart-enable-x-152011             285 writes
  ih-plan-x-152017                 B1: 15 planned writes, equal to amdgpu's except three address registers
  ih-init-x-152028, ih-state-x-152034   B2: executed (15 + 6 TLB flush writes), ring ENABLED, 0 interrupts
  sweep-*-init, ih-state-quiet-152124   still 0 interrupts, 0 vectors
  ih-fini-x-152130, sweep-*-fini   B3: ring off (3 writes + 6 flush writes), memory returned
  ih-init-x-152212, ih-fini-x-152218, ih-state-x-152230   B3: a second init and fini in the same device start. The first
                                   write differs from the trace (403101A0 for 10610080): a read-modify-write over our
                                   own earlier value instead of the firmware's. comparison.txt marks it; it is expected.
  psp-load-x-152244                11 of 11 commands
  ih-init-x-152250, gfx-run-x-152255 (stages 1 to 7), ih-state-x-152306   0 interrupts with the engines running
  gfx-run-x-152312                 C1: stage 8, 35 writes, equal to amdgpu's 35 (one selection without a write left out)
  ih-state-sources-152317          0 interrupts after the sources are enabled
  fence-gfx-x-152330 (no interrupt bit), ih-state-x-152335   C2 control: value 1 arrives, 0 interrupts, 0 vectors
  fence-gfx-x-152341, ih-state-x-152346   C2: THE FIRST INTERRUPT. 1 routine call, 1 DPC, 1 vector: client 20 source 181
                                   ring 0, src_data[0] 80000000 (as under Linux)
  fence-c0 / ih-state-c0           ring id 4
  fence-c5 / ih-state-c5           ring id 21
  fence-kiq / ih-state-kiq         client 20 source 178 ring id 9
  fence-gfx-x-152433 (x100), ih-state-hundred-152438   C3: 100 of 100 values, 1082 us, 100 vectors (104 in all), 102
                                   interrupts (two DPCs took two vectors), 0 overflows
  gate-plan-x-152456, ih-state-restart-152515   C4: device restart with everything running: no bugcheck, ring off, 0 calls
  gart-enable-x-152528, ih-init-x-152541, gfx-run-x-152553, fence-c3-x-152606   refused (0xC0000184): after a device
                                   restart the driver does not take the PSP load of the earlier start as its own
  ih-state-second-152618, ih-state-odd-152646   one interrupt routine call NOT taken as ours and no vector, once, after
                                   this ih init (the first one after a device stop with running engines)
  ih-fini-x-152734, ih-init-x-152746, ih-state-reinit-152758, ih-fini-x-152811   it does not repeat
Boot 15:29 (restart; vector 0x51):
  ih-state-boot2-153037, gart-enable-x-153043, psp-load-x-153049, ih-init-x-153054
  gfx-run-x-153100                 stages 1 to 8 in one call: 459 writes, 14 doorbells, all rc 0, stage 6 takes 351 us
  fence-c3-x-153106 (x10), ih-state-first-153111   10 vectors with ring id 7 (me 1 pipe 3 queue 0), 9 interrupts
  gfx-fini-x-153123, ih-state-afterfini-153128   undo with the IH ring on: 97 writes, 4 doorbells; 8 more source 181
                                   vectors arrive during the unmaps (18 in all), src_data[0] = 1, ring ids 20, 21, 22, 7 twice
  gfx-run-x-153134                 THE SECOND BRING-UP FAILS: stage 6 rc -62 after 164974 us (KIQ ring test), 320 writes,
                                   2 doorbells. All GART addresses differ from the first run's (the IH ring holds its pages)
  fence-*-1531xx                   refused
  ih-state-second-153156, ih-state-failed-153224   one new vector: client 27 (UTCL2) source 0, src_data 00000444 00000050:
                                   a GPU page fault, VMID 0, page 0x444000 = inside the FIRST run's KIQ ring (base 0x442000)
  gfx-state-x-153230
  sweep-*-fault-153411             witness: GCVM_L2_PROTECTION_FAULT_STATUS 000008B0, ADDR_LO32 00000444, CP_MEC_CNTL 0,
                                   GRBM_STATUS A0003028, GRBM_STATUS2 10000008
  gfx-fini-x-153448, ih-state-afterfault-153454, ih-fini-x-153500, gfx-state-x-153505   the undo works after the fault:
                                   93 writes, 4 doorbells, all memory returned
  gate-plan-x-153521               gates closed, device restarted. The target was rebooted afterwards.
