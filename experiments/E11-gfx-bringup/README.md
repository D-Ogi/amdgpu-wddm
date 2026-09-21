# E11: RLC, CP, KIQ, queues, ring tests and SDMA under Windows (milestone M5, second part)

State: **run 001 done on unit A, 2026-09-21. M5's criterion is met: PM4 packets written by the CPU into rings in
GART memory executed; every ring test passed. H2 to H8, H9a and H10 hold, H1 holds except one sub-claim, H9b was
not attempted in run 001 and was settled in E12 run 001 (a second bring-up in the same boot works, facts M39).**

## Why

M5's criterion is a PM4 packet, written by the CPU into a ring, changing `SCRATCH_REG0`. E10 put the firmware
into the GPU (facts M34) and showed that the PSP already starts the RLC and releases SDMA while doing so
(facts M35); the CP is still halted (`CP_ME_CNTL = 0x15000000`, `CP_MEC_CNTL = 0x50000000`). What amdgpu did
next on unit A is in the E03 trace (MEASURED): the doorbell aperture at 0.038 s, then between 0.5496 s and
0.551 s about 590 accesses: golden registers, the GRBM CAM probe, constants (`SH_MEM_CONFIG` and bases per
VMID, `GDS_*`, `SPI_GDBG_*`), RLC stop, clear-state buffer, RLC start, the KIQ's MQD and HQD registers,
`CP_MEC_CNTL = 0`, the KIQ ring test (`SCRATCH_REG0` `0xCAFEDEAD -> 0xDEADBEEF`), eight compute queues and the
gfx queue mapped through the KIQ, `CP_ME_CNTL = 0`, the clear-state stream, nine more ring tests, both SDMA
rings.

The code: `driver/shim/bc250_gfx.c`, `bc250_sdma.c`, `bc250_ring.c`, `bc250_nbio.c`: amdgpu's
`gfx_v10_0_hw_init()` and `sdma_v5_0_hw_init()` transcribed against AMD's imported, unmodified tables and
structures (golden settings, clear state, MQD layouts, PM4), because `gfx_v10_0.c` itself needs about ninety
kernel functions (driver/amdgpu-import/PROVENANCE.md). A host test replays it against the trace. The driver
part is `driver/kmd/gfx.c` and `gpumem.c` (bc250kmd 0.5.3): gate `EnableGfx` (default 0, closed by every
install, needs the four gates before it), one escape with PLAN, RUN to a stage, FINI, STATE; registers through
`g_MmioGfxAllow` only (generated: the 249 registers amdgpu itself read or wrote in these steps).

Differences from amdgpu that are known beforehand:

- **Addresses.** amdgpu's MQDs and clear-state buffer were in low VRAM (`0xF4008CD000`...), which under
  Windows is the firmware's framebuffer. Ours come from a pool 32 MB to 8 MB below the end of VRAM. Rings and
  write-back slots are system memory behind the GART, as amdgpu's, at other GART offsets. Every register that
  carries such an address differs from the trace by exactly that; the host test has the list.
- **Start state.** amdgpu's sequence started from the state its own earlier init steps had left; ours starts
  from the state after E10's load (sweep `loaded` of E10). Read-modify-write results can differ where the
  start state differs; the host test's second mode (reads answered from that sweep) says where.
- **TLB flushes.** amdgpu entered its rings into the GART when it allocated them, long before this step, and
  flushed VMID 0 of both hubs after each bind (E03 trace, 0.2495 to 0.2528 s: 22 binds, nothing else in that
  window). Our allocations happen at the head of the first `gfx run`, so the same flushes appear there, before
  stage 1's first write. `compare.py` takes them out and checks their values by themselves. A plan binds
  nothing and so flushes nothing. The PTE values themselves have no counterpart in the trace (it holds no
  table content): the first check of `bc250_gart_bind()` on hardware is the first ring test.
- **Interrupts** (the trace's block at 1.56 s) are left out: there is no IH ring yet (M6).

## Stages

1 doorbell aperture, 2 golden registers, 3 GRBM CAM probe, 4 constants, 5 RLC, 6 CP (KIQ, MEC, queues, CP
start, ten ring tests), 7 SDMA. The run goes one stage at a time with a sweep after each.

## Hypotheses

- H1. Closed gate: `gfx state` answers, `gfx plan`/`run` are refused with `STATUS_DEVICE_NOT_READY`.
- H2. `gfx run` before `psp load`: refused (`STATUS_INVALID_DEVICE_STATE`), nothing written.
- H3. `gfx plan 5` (**no register write, no doorbell write**): the planned writes of stages 1 to 5 equal the
  trace's in offset, order and value, except the registers on the host test's address list and the
  differences its second mode predicted. Sweeps before and after equal.
- H4. Stage 1: `RCC_DOORBELL_APER_EN` reads 1 through the witness, nothing else moves.
- H5. Stages 2 to 4: the executed writes equal the plan; the written registers read back as written through
  the witness and equal Linux's values after init (E03 `sweep-after-init`) where those do not carry addresses.
- H6. Stage 5: the RLC stops and starts again: `RLC_CNTL = 1`, `RLC_STAT` not busy, `RLC_CSIB_*` hold our
  buffer, `RLC_PG_CNTL = 0`. No protection fault bit.
- H7. Stage 6, **M5's criterion**: every ring test passes (KIQ, gfx queue, eight compute queues): return
  code 0, and the sequence's own reads show `SCRATCH_REG0` going `0xCAFEDEAD -> 0xDEADBEEF`. Afterwards
  `CP_ME_CNTL` and `CP_MEC_CNTL` have no halt bit, `GRBM_STATUS` shows an idle CP, no protection fault bit in
  either hub (the rings and write-back slots are GART memory: this is also the first functional proof of M4's
  page table), picture undisturbed.
- H8. Stage 7: both SDMA rings programmed as in the trace (`SDMAn_GFX_RB_CNTL = 0x80840016`, doorbell
  offsets `0x800`/`0x850`). The sequence has no SDMA ring test (amdgpu's needs a fence interrupt path, M6):
  a passed stage 7 says the registers were programmed, not that the engines execute.
- H9. `gfx fini`. **Split in two after the host test measured it** (`driver/shim/test/run_gfx.ps1`,
  2026-09-21).
  - H9a, the undo itself: engines halted again (`CP_ME_CNTL = 0x15000000`, `CP_MEC_CNTL = 0x50000000`,
    `RLC_CNTL = 0`, `SDMAn_F32_CNTL` bit 0), memory given back, and nothing touched that is outside the
    generated register table. The host test confirms both: the teardown touches 18 registers and every one
    is already named by a window the table is generated from. Expected to hold.
  - H9b, a second `gfx run 7` without a new PSP load: **expected to fail, and where.**
    `bc250_gfx_hw_fini()` now unmaps the client queues through the KIQ as upstream does, but nothing unmaps
    the KIQ's own HQD - upstream has no such step, and its only `CP_HQD_ACTIVE = 0` is the SR-IOV write at
    `gfx_v10_0.c:7029`. So the second run finds `CP_HQD_ACTIVE` set, writes `CP_HQD_DEQUEUE_REQUEST = 1`
    and polls a MEC that fini halted, because `cp_compute_enable(true)` runs after `kiq_resume`
    (`gfx_v10_0.c:7208` from `:7243`, against `:7239`). Stage 6 returns a timeout. Upstream never notices
    because it does not look at the poll's outcome.
    Attempt H9b only to confirm the predicted failure, and only once `CP_HQD_DEQUEUE_REQUEST` (0x0C884) is
    in the generated table: it is the one register a second run touches that no trace window names, so
    without it the sequence faults before it ever reaches the timeout. A declared deviation that would make
    the re-run work has been proposed and is not in the tree; if one is taken, H9b is rewritten around it
    before the run.
- H10. Device restart while running: the driver halts the engines by itself before it unloads the PSP and
  restores the GART; the new instance reports no stage done.

What would refute: a planned write outside the predicted differences; a ring test timing out, other than
H9b's predicted one; a protection fault bit; a hang (GRBM_STATUS busy bits that stay); the picture
disturbed; engines not halted after fini.

## Safety

Every register write is one amdgpu made on this unit at this point of its init, in the same order, to a GPU
in the state amdgpu had it in (GART enabled, firmware loaded by the PSP), with the declared kinds of
difference (above). The display engine (DCN) is not touched: no DMU register is in the table, the firmware's
scan-out keeps running from the framebuffer, which our memory stays clear of (checked at start). The new risk
of this step is the GPU as a bus master writing into system memory: read pointer write-back and fence slots
live in GTT pages. Those pages are non-paged contiguous memory owned by the driver, entered into the GART by
AMD's PTE format; they are given back to Windows only after the engines read halted, and leaked on purpose
otherwise (`gpumem.c`). A wrong PTE or a wrong ring address shows up as a protection fault bit or a ring test
timeout, not as a stray write: VMID 0 translates only what the table holds, everything else goes to the dummy
page.

If the machine hangs: power cycle. The gates stay as the script left them, but open gates only allocate at
start; every sequence runs on command only.

## Procedure

`e11_target.ps1`, one phase per call, logs under `C:\BC250\e11\out`. Fresh boot of the target (one PSP load
per boot, facts M35). `install` -> `gfx state -Tag closed` (H1) -> `gate -On 1` -> `sweep before`,
`sweep before2` -> `gart enable` -> `gfx run -Stage 1 -Tag nopsp` (H2) -> `psp load` -> `sweep loaded` ->
`gfx plan -Stage 5` -> `sweep afterplan` (H3; compare before going on) -> `gfx run -Stage 1`, `sweep s1` (H4)
-> stages 2, 3, 4 with a sweep each (H5) -> stage 5, sweep (H6) -> stage 6, sweep (H7) -> stage 7, sweep (H8)
-> `gfx fini`, sweep (H9a) -> `gfx run -Stage 7 -Tag second`, sweep (H9b, the predicted timeout; skip
it unless `CP_HQD_DEQUEUE_REQUEST` is in the table) -> `gate -On 1` again (H10), sweep ->
`gate -On 0` -> reboot the target.

## Result

Run 001, 2026-09-21, unit A, bc250kmd 0.5.5.0 (after two attempts that executed nothing, below). Evidence:
`evidence/windows/2026-09-21-E11-run-001/` (`README.txt` there maps files to hypotheses, `comparison.txt` is
`compare_run001.sh`'s output). All MEASURED.

- **Two defects found by the plan, before any write was executed.** On 0.5.3 the plan stopped in stage 3: the
  CAM probe reads back, through an alias, a pattern it has just written, and a plan writes nothing. Since 0.5.4
  a plan answers a read of a register it has planned a write to with the planned value (one declared alias,
  `VGT_ESGS_RING_SIZE_UMD` -> `VGT_ESGS_RING_SIZE`, from the trace). On 0.5.4 the comparison of the plan showed
  every line after write 17 shifted by one: the golden setting `GCMC_VM_CACHEABLE_DRAM_ADDRESS_END` was missing
  from the comparison's trace filter and from the driver's register table (both excluded `GCMC_*`). A run would
  have been stopped by the driver's own table in stage 2. The step is 355 writes, not 354.
- H1 holds for plan and run (`STATUS_DEVICE_NOT_READY`). The state query is refused as well with a closed gate,
  where H1 said it would answer: there is no GFX object to ask then.
- H2 holds: `0xC0000184`, nothing written.
- H3 holds: 203 planned writes, 201 equal to the trace in offset, order and value, the other two are
  `RLC_CSIB_ADDR_HI/LO` (our clear-state buffer at MC `0xF5FE00A000`). No register moved across the plan except
  free-running ones (`SDMAn_STATUS2_REG`, the PSP's `C2PMSG_60/81`, clocks).
- H4 holds: `RCC_DOORBELL_APER_EN` 0 -> 1, nothing else. The same call made the driver's 23 GART binds (rings,
  write-back slots, EOP buffers: 0x121000 bytes of system memory, plus 0xB000 bytes of VRAM for MQDs and the
  clear-state buffer): 69 TLB flush writes, values as amdgpu's, every flush acknowledged.
- H5 holds: stages 2 to 4 executed as planned (34, 3 and 157 writes); 27 and 33 registers moved to Linux's
  after-init value, none to anything else except the free-running ones. **The GRBM CAM alias holds under
  Windows** (stage 3 returned 0: the pattern written to `VGT_ESGS_RING_SIZE_UMD` read back through
  `VGT_ESGS_RING_SIZE`).
- H6 holds: `RLC_CNTL = 1`, `RLC_STAT = 0`, `RLC_PG_CNTL = 0` (it was 8 after the PSP load), `RLC_CSIB_LENGTH =
  0x3B5`, no protection fault bit.
- **H7 holds, M5's criterion.** Stage 6: 62 register writes and 14 doorbells in 349 us, return code 0, i.e. the
  KIQ ring test, the eight compute rings and the gfx ring each saw `SCRATCH_REG0` go `0xCAFEDEAD -> 0xDEADBEEF`
  by a `SET_UCONFIG_REG` packet the CPU had written into a ring. The witness afterwards: `SCRATCH_REG0 =
  0xDEADBEEF`, `CP_ME_CNTL = 0`, `CP_MEC_CNTL = 0`, `CP_STAT = 0`, `CP_RB0_RPTR = CP_RB0_WPTR = 0x500` (the
  clear-state stream and the ring test consumed), `GRBM_STATUS = 0x00003028` (idle), no protection fault bit in
  either hub, 593 registers at Linux's after-init value, the rest addresses, ring positions and running
  counters. The picture kept presenting throughout. The rings, the write-back slots and the KIQ's packets live
  in system memory behind the GART: this is the functional proof of M4's page table, of `bc250_gart_bind()`'s
  PTE format and of "bus address = CPU physical address" on this machine, and the compute MQDs (VRAM pool) were
  accepted by the CP through `MAP_QUEUES`.
- H8 holds: `SDMAn_GFX_RB_CNTL` reads `0x80841017` (written `0x80840016`, then enable), doorbell offsets `0x800`
  and `0x850`, `SDMAn_F32_CNTL = 0`. `SDMAn_CNTL` reads `0x000400E2` where Linux has `0x000400E3`: bit 0 is the
  trap enable of the interrupt block we leave out. No SDMA ring test exists, so nothing here says that the SDMA
  engines execute.
- The whole step: the 355 executed writes of the seven calls are 341 equal to the trace and 14 address
  registers with our addresses, none other; the same in boot 2, where all seven stages ran in one call (about
  620 us of stage time, amdgpu's window is about 1 ms).
- H9a holds: `gfx fini` executed 93 writes and 4 KIQ doorbells (the queue unmaps and the ring test after each),
  return code 0; `CP_ME_CNTL = 0x15000000`, `CP_MEC_CNTL = 0x50000000`, `SDMAn_F32_CNTL = 1`, `RLC_CNTL = 0`; all
  memory given back to Windows (engines read halted). New observation: with the engines halted this way
  `GRBM_STATUS` reads `0xA0003028` and `GRBM_STATUS2` `0x30000008` (busy bits of the CP front ends stay set),
  also after the device restart; a reboot of the target clears them.
- H9b not attempted in run 001 (predicted to fail, above). Settled later the same day in E12 run 001 with the
  declared deviation in the tree (`driver/amdgpu-import/PROVENANCE.md`: `CP_HQD_ACTIVE = 0` instead of a dequeue
  request when the MEC is halted): run, undo, run again, undo in one boot, all seven stages rc 0 both times, all
  ring tests pass, no register outside the table. The second run writes 358 registers against 355; the edit script
  is `evidence/windows/2026-09-21-E12-run-001/comparison.txt` (facts M39). Open: stage 6 takes 5222 us in the second
  run against 349 us in the first.
- H10 holds (boot 2): device restart with all engines running: the new instance reports no stage done and no
  memory held, engines halted, `GCVM_CONTEXT0_CNTL` back at the firmware's `0x007FFE80`, no fault bit, machine
  and picture fine.
- Not needed, as it turned out: any SMU message from us. The CP runs its ring tests with the SMU as the firmware
  left it.
