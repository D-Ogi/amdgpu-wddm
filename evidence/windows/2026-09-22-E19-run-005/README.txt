E19 runs 002 to 005, 2026-09-22 00:34 to 01:19: stage C of ADR 0008. bc250kmd 0.7.13 (commit ab8f864, .sys sha256
prefix 9692e1996355551d, package-umd 0.7.13.1), full WDDM table, EnableGpuVa = EnableGpuSubmit = 1, engine gates open.
The *-console.txt files are the driving scripts' filtered output (scratch\tmp\e19_run2.sh .. e19_run5.sh); the other
files are the target's own logs of the same steps, selected by time.

Run 002 (00:34, boot 23:42:20 - the boot of run 001): installed 0.7.13 with the gates closed (M74), gate, `gart enable`,
  `psp load`, `ih init` all returned 0; the unit stopped answering inside `gfx run 8` and never came back by itself.
  The next boot (00:56:04, not started by this session) holds no BugCheck 1001 event: a hard hang, not a bugcheck. No ring
  log survived it. This was the second bring-up of that boot, after two device restarts, and run 001's undo had left
  out `psp unload` and `gart restore`.
Run 003 (boot 00:56:04): the same bring-up passed (462 writes, 16 doorbells, as run 001). Its control step was wrong
  twice over and both refusals are the driver's, before any ring write: fence mode `test` exists for the SDMA rings
  only, and `C:\BC250\e16` still held a bc250kmd_cli older than 0.7.13 whose BC250_ESCAPE_FENCE is shorter than the
  driver's (sha256 prefix C984DC1EE268779B; the 0.7.13 one, 499F9D4EFAE986D4, had gone to `e16-umd` only). With the
  0.7.13 cli in place: plain fence 1 of 1, then `fence gfx 1 ib` FETCHED. Undo complete, all 0.
Run 004 (boot 01:05:48, a warm restart): bring-up, control and IB as run 003. `kmtprobe --submit` refused its own
  arguments (`bad --timeout`: the option is in seconds, the script passed 20000) and submitted nothing: the IH counters
  are the same before and after it. That output was overwritten by run 005's; the refusal is quoted here from the
  console. Undo complete, all 0.
Run 005 (boot 01:13:10, a warm restart): bring-up, control and IB as before, then `kmtprobe --submit`:

  control    fence gfx x1                  1 of 1 read back, slot 0x1, 12 us
  H2         fence gfx x1 ib               IB 0x564000, 3 dwords, VMID 0: FETCHED (the scratch register took the
                                           value), fence 0x2/0x2; IH 1 -> 2 interrupts, 1 -> 2 vectors
  H3 client  kmtprobe --submit             CreateContextVirtual, command buffer at GPU VA 0x20000000, 0x40 bytes
                                           (PACKET3 NOP), SubmitCommand STATUS_SUCCESS, monitored fence reached 1,
                                           submit=ok, all steps passed
  H3 driver  ring log lines 184 to 193     CreateContext flags 0x4; root page table 1:0x1FD729000 = physical
                                           0x46DFF3000; `gfx: VMID 1 root 0x46DFF3000 -> 0`; `fence 100 on the gfx ring:
                                           sequence 3, vmid 1, ... va 0x20000000, 64 bytes`; `hardware fence arrived,
                                           reporting fence 100`
  H3 IH      ih state before / after       2 -> 3 interrupts, 2 -> 3 vectors, 0 overflows
  table      meanwhile                     CDD's presents (size 0) completed in software; no TDR, no watchdog line
  undo       gfx fini, ih fini, psp unload, gart restore: all 0; gate closed, stage 61, presents counting. 71 to 75 C.

What run 005 does not show: the command buffer held NOPs, which leave no trace. That the CP got past the
INDIRECT_BUFFER packet at VMID 1 and that no second vector arrived is what was measured; that the buffer's bytes were
fetched through VidMm's page tables is not demonstrated by it (H4).
