# E12: interrupts under Windows: the IH ring, the first interrupt, fences (milestone M6)

State: **part A run (2026-09-21, run 001): all four hold. Parts B and C run (run 002): the IH ring and fences work, a second bring-up failed (M44; fixed in E15).**

## Why

M5 (E11, facts M36) showed that the GPU executes what we put into its rings, found by polling a register. Everything
after that (fences, a scheduler, paging) needs the GPU to tell the CPU that it is done. On this part that is the
interrupt controller's IH ring (`navi10_ih.c`): a ring in GTT memory the hardware writes 32-byte vectors into, a
write-back slot for its write pointer, a doorbell for our read pointer, and one interrupt line or message to the
host. amdgpu sets it up on unit A at 0.252832 to 0.252845 s of its init (E03 trace, MEASURED: 19 accesses, 11
registers: `IH_RB_*`, `IH_DOORBELL_RPTR`, `NBIO.INTERRUPT_CNTL`, `INTERRUPT_CNTL2`, `BIF_IH_DOORBELL_RANGE`), and
unmasks its sources later (1.560 to 1.562 s: `CP_INT_CNTL_RING0`, `CP_ME1_PIPEn_INT_CNTL`, `CPC_INT_CNTL`,
`SDMAn_CNTL`, the doorbell self-ring aperture; the host replay of `driver/shim` already reproduces those 35 writes).

What is not known (and no Linux trace can tell): whether dxgkrnl connects an interrupt for a **display-only**
miniport at all, whether it honours `MessageSignaledInterruptProperties` for one, and whether `DxgkCbQueueDpc` works
there. Under bc250kmd 0.5.x the device has a line interrupt resource (IRQ 52, read from the target on 2026-09-21)
and the function's PCI command register has INTx disabled (`0x0406`). The function has MSI (1 of 4 messages) and
MSI-X (3) capabilities; amdgpu used them (E01 `lspci-gpu-vvv.txt`).

## Part A: what Windows assigns, and silence (bc250kmd 0.6.0, no GPU register touched)

0.6.0 adds the INF section for message-signalled interrupts, an interrupt routine that counts every call and takes
none as ours while the IH ring is off, a gate `EnableIh` (default 0, closed by every install, needs `EnableGfx`) and
`bc250kmd_cli ih state`, which answers with the gate closed as well (it reports Windows' side only).

- A1. 0.6.0 installs and starts as 0.5.5 did (stage 61, picture), before and after a reboot of the target.
- A2. After the reboot the device's interrupt resource is a message (`ih state`: "message (MSI)"; the driver log
  line "interrupt resource 1: message"). Before the reboot either answer is acceptable and is recorded: whether
  a disable/enable is enough to switch is one of the things measured.
- A3. With no source enabled the interrupt routine is not called: `InterruptCount` stays 0 over at least two
  minutes that include presents, a sweep and a device restart. (On the line interrupt a non-zero count would not
  be alarming, the line can be shared; on a message it would mean the GPU signals without being asked.)
- A4. E11's sequence (GART, PSP load, `gfx run 7`, `gfx fini`) still passes on this build, and the count is still
  0 afterwards: the CP raises nothing while its enable bits are clear and the IH ring is off.

Stop rule (review of 0.6.0): in part A the interrupt routine claims nothing, which is harmless on a message and a
livelock on a level-triggered line that something asserts. So `ih state` is read before anything else touches the
GPU: if it says "line", part A ends there (the answer is recorded, A3 and A4 are not run in that boot, and E11's
sequence is never run on a line interrupt with this build). The `msi key` line of the script's state says whether
the INF's `.HW` section ran at all: "ABSENT" means the install was not a real install, not that Windows refused.

What would refute: the device does not start with the MSI section (Windows falls back to Basic Display: the
failsafe of ADR 0006); the count moves without a source enabled.

## Part B: the IH ring (needs the shim's `bc250_ih_*`)

`ih plan` / `ih init` / `ih fini`, registers through `g_MmioIhAllow` (generated from the trace window above).

- B1. `ih plan`: the planned writes equal amdgpu's 15 in offset, order and value, except the address registers
  (`IH_RB_BASE`, `IH_RB_BASE_HI`, `IH_RB_WPTR_ADDR_LO/HI`: our GTT addresses; `INTERRUPT_CNTL2`: our dummy page).
- B2. `ih init`: executed as planned; `IH_RB_CNTL` reads back enabled as in Linux after init; still no interrupt
  (no source is unmasked yet); no protection fault bit.
- B3. `ih fini`: ring disabled, memory returned, and nothing arrives afterwards.

## Part C: the first interrupt and a fence (needs the shim's interrupt stage and fence emit)

- C1. With the IH ring on and E11's bring-up done, the interrupt stage (the trace's 35 writes of 1.56 s) executes
  equal to the trace except the self-ring aperture base (our BAR2 address, `0xD0000000` on this machine as well).
- C2. A fence on the gfx ring (amdgpu's `RELEASE_MEM`/`WRITE_DATA` with `INT_SEL`): the fence value appears in our
  GTT slot, exactly one vector arrives with the CP end-of-pipe source of the gfx ring, the interrupt routine count,
  the DPC count and the vector count each go up by one, and the ring's read pointer catches up with the write
  pointer. The same on one compute ring, with that ring's id in the vector.
- C3. A hundred fences in a row: a hundred vectors, no overflow, none lost.
- C4. Device restart with everything running: no interrupt after the stop, no bugcheck, memory rule of `gpumem.c`
  kept (the IH ring's pages go back only with the ring disabled).

## Safety

Part A touches no GPU register. The interrupt routine and the DPC touch only the device context and one dxgkrnl
callback; the IH object is freed only when no interrupt can be connected (device removal; a restart reuses it). From part B on the
new risk is an interrupt storm (a source that keeps asserting because nobody acknowledges it): the DPC advances the
read pointer for every vector whether it knows the source or not, the CP sources are edge-like (one vector per
event), and `ih fini` / the device stop disable the ring first. A storm would show as a busy CPU on the target, not
as a hang of the bus; the target is rebooted freely. The IH ring and its write-back slot are GTT pages under
`gpumem.c`'s rule.

## Procedure

`e12_target.ps1` (E11's script plus the `EnableIh` gate and the `ih` phase), logs under `C:\BC250\e12\out`.
Part A: `install` -> `ih state -Tag installed` -> reboot -> `ih state -Tag boot` -> `gate -On 1` -> `ih state`
-> `sweep before` -> wait two minutes -> `ih state -Tag quiet` -> E11's sequence -> `ih state -Tag aftergfx`
-> `gate -On 0` -> reboot.

Part B (bc250kmd 0.6.1, its own boot): `install` -> `gate -On 1` -> `sweep before` (now with OSSSYS) -> `gart enable`
-> `ih plan` (`compare.py ih`) -> `sweep afterplan` -> `ih init` (`compare.py ih`) -> `ih state` -> `sweep init` -> wait
a minute -> `ih state -Tag quiet` -> `ih fini` -> `ih state` -> `sweep fini` -> `ih init` again -> `ih fini`.

Part C (same boot or the next): `gart enable` -> `psp load` -> `ih init` -> `gfx run -Stage 7` -> `ih state` ->
`gfx run -Stage 8` (`compare.py irq`) -> `ih state -Tag sources` -> `fence -Op gfx -NoInt` -> `ih state` (the control:
the value, no vector) -> `fence -Op gfx` -> `ih state` -> `fence -Op c0`, `c5`, `kiq` with `ih state` after each ->
`fence -Op gfx -Count 100` -> `ih state` -> `gate -On 1` (a device restart with everything running, C4) -> `ih state`
-> `gate -On 0` -> reboot.

## Result

### Part A, run 001 (2026-09-21, bc250kmd 0.6.0.0, `evidence/windows/2026-09-21-E12-run-001/`)

- A1 held: 0.6.0 installs and starts (stage 61, picture) straight after the install and after a reboot.
- A2 held, and more: Windows assigns a **message interrupt (MSI)** to the display-only miniport as soon as the
  package is installed (`pnputil /add-driver /install` restarts the device; vector 0x80), no reboot needed; the
  same after the reboot (vector 0x51) and after every device restart (0x70). The INF's `.HW` section had run
  (`MSISupported 1`, `MessageNumberLimit 1` under the device's hardware key).
- A3 held: the interrupt routine was called 0 times over a sweep of four IP blocks, more than two minutes of
  presents (87 -> 276) and two device restarts.
- A4 held: E11's sequence passes on this build (355 + 69 writes, all seven stages rc 0, 93-write undo), and the
  count is 0 with the engines running and after the undo: the CP raises nothing while the IH ring is off.
- Extra, for E11's H9b: the whole bring-up a second time in the same boot. See E11's README.

Not answered by part A: whether `DxgkCbQueueDpc` works for a display-only miniport (no interrupt arrived, so none
was queued). Part C answers it.

### Parts B and C, run 002 (2026-09-21, bc250kmd 0.6.1.0 = commit 9a716a5, `evidence/windows/2026-09-21-E12-run-002/`)

Reviewed a third time before the install (no blocker; the DPC drain at stop, a single-consumer gate in the DPC and a
total time budget for the fence escape went in on the reviewer's advice). Linux reference taken the same day: E13.

- **B1, B2, B3 held.** `ih plan` and `ih init`: 15 writes, 12 equal to amdgpu's, three address registers with our
  addresses, nothing else; `IH_RB_CNTL` bit 31 reads back cleared as modelled. Ring enabled, 0 interrupts over a sweep
  of five IP blocks and a minute; `ih fini` returns the memory; a second init and fini in the same device start work
  (its first write is a read-modify-write over our own earlier value, the one expected difference).
- **C1 held.** Stage 8: 35 writes, equal to amdgpu's 35 in offset, order and value (`compare.py irq`, which now leaves
  out of the trace side one pipe selection that amdgpu makes without writing anything).
- **C2 held: the first interrupt** (facts M43). Control without the interrupt bit: value, no interrupt. With it: one
  routine call, one DPC, one vector, `client 20 source 181 ring 0`, `src_data[0] = 0x80000000`, as under Linux.
  `DxgkCbQueueDpc` works for a display-only miniport (part A's open question). Compute pipe 0 queue 0 -> ring id 4,
  pipe 1 queue 1 -> 21, pipe 3 queue 0 -> 7; KIQ -> `source 178`, ring id 9. All as E13 measured under Linux.
- **C3 held.** 100 fences: 100 values, 100 vectors, 1082 us, none lost, no overflow.
- **C4 held.** Device restart with ring, sources and engines running: no bugcheck, silence afterwards. Two notes: the
  driver refuses a bring-up after a device restart (it does not take the earlier start's PSP load as its own), and the
  first `ih init` after such a stop was followed, once, by a message without a vector.
- **Not held: a second bring-up in the same device start** (facts M44). Stage 6 times out at the KIQ ring test and the
  GPU page-faults at the first run's KIQ ring: the MEC resumes the old queue from state that the `CP_HQD_*` registers
  do not reset. M39's success was an accident of equal addresses. The fault itself came in through our IH ring as a
  UTCL2 vector and the witness confirmed it, and the undo after it worked. The prediction attached to the
  `cp_hqd_pq_rptr = 0` deviation was not reached; the deviation is under review together with the teardown.
- Seen on the way (facts M45): the KIQ's unmaps during the undo raise `source 181` vectors with `src_data[0] = 1` and
  ring ids that are not the unmapped queues' own.

Next: a teardown that dequeues the KIQ's HQD while the MEC runs (shim), then the second bring-up again with the IH
ring holding its pages, then the SDMA ring test and fence (the shim's emitter exists as a patch), then a compute
dispatch.
