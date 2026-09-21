# E09: GART and VM context 0 under Windows, with AMD's own code (milestone M4)

State: **run 001 done: H1, H2, H3, H5 hold, H4 holds with a residue** (2026-09-21). Milestone M4 reached.
Evidence: `evidence/windows/2026-09-21-E09-run-001/`.

## Why

M4's exit criterion: the page table programmed as `gmc_v10_0` does it, the hub registers reading back like the
Linux baseline, no fault status bit set. Everything needed is in place:

- The recipe: amdgpu's GART step on this very unit, 285 register writes to the two VM hubs plus the TLB
  flushes (E03 trace, `tools/trace/extract_phase.py`).
- The code: `gfxhub_v2_0.c` and `mmhub_v2_0.c` imported unmodified from mainline v6.18 (ADR 0002), compiled
  against `driver/shim`, driven by `driver/shim/bc250_gmc.c` in `gmc_v10_0_gart_enable()`'s order. A host
  replay test feeds that code the firmware state recorded on unit A and gets amdgpu's 285 writes back, offset
  for offset and value for value (`driver/shim/test`).
- The starting point: the firmware leaves the same VM state under Windows as under Linux, and the VRAM
  carve-out is reachable by physical address (E08, facts M31).

The driver part is `driver/kmd/gart.c` (bc250kmd 0.4.3): the kernel backend of the shim plus one escape with
three operations. Registers are reached through `g_MmioGartAllow` only, a table generated from the trace
itself: what amdgpu wrote in this step and the two acknowledge registers it polled, 287 offsets. Gate
`EnableGart`, default 0, closed by every install; needs `EnableMmio` and `EnableVram`.

Memory, as amdgpu laid it out on this unit: the page table in the top 2 MB of VRAM (MC `0xF5FFE00000`, physical
`0x46FE00000`, 1 MB for 512 MB of GART at GPU address 0, all entries invalid = 0), the scratch page in the
second MB of that window (amdgpu: wherever its allocator put it), the dummy page in system memory
(`MmAllocateContiguousMemorySpecifyCache`, kept for as long as the GPU knows its address).

## Hypotheses

- H1. Closed gate: `gart plan` is refused with `STATUS_DEVICE_NOT_READY`; the driver starts as before.
- H2. `gart plan` (gates open, **no write executed, no protocol register read**): the driver reports 285 writes
  whose offsets and order equal the first 285 writes of amdgpu's trace, and whose values equal amdgpu's except
  in exactly four registers that carry our addresses instead of amdgpu's: `GCMC/MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB`
  (= our scratch page's physical address >> 12) and `GCVM/MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32` (= our
  dummy page >> 12). The page table base is the same as amdgpu's because the table is in the same place.
  GC and MMHUB sweeps before and after the plan differ only by noise: a plan changes nothing.
- H3. `gart enable`: sequence result 0 (both engine-17 invalidations acknowledged inside amdgpu's poll budget,
  MMHUB semaphore taken and released), no refused register, 285 writes. Afterwards, read through the
  independent witness `bc250rd`: every register of the step holds either the value written or the value
  Linux's sweep after init shows for it (self-clearing bits), apart from the four address registers, which
  hold ours; `GCVM_L2_PROTECTION_FAULT_STATUS` and `MMVM_L2_PROTECTION_FAULT_STATUS` are 0; outside the
  step's registers the GC and MMHUB sweeps differ from the control only by noise. The display keeps running
  (stage 61, presents counting, picture unchanged): its scan-out address lies inside the system aperture,
  which both before and after maps VRAM untranslated.
- H4. `gart restore`: every register of the step reads what it read before the experiment; sweeps equal the
  control outside noise.
- H5. Enabled and then the device is stopped (disable/enable) without a restore command: the driver restores
  by itself at stop; after the restart the registers are in the firmware's state.

What would refute: a planned write that amdgpu did not make (or a missing one, or another order); an
acknowledge that never comes; a fault status bit; any register outside the table changing; the picture
disturbed.

## Safety

Every write is one amdgpu made on this unit, in the same order, with the firmware's scan-out running at the
time (E03: simpledrm showed the console until 124.5 s, GART was enabled at 123.2 s). The values that differ
point at memory we own. The plan runs first and is compared before anything is executed; a register outside
the generated table stops the sequence at once. The firmware's values are held for `restore`, and the driver
restores by itself when the device stops. Reading the MMHUB semaphore register acquires it (facts M25): only
`enable` does that, releases it on every path, and nothing else reads it (it is on no sweep list).
If the machine hangs: power cycle. The gates stay as the script left them, but an open `EnableGart` only
allocates one page at start; the sequence runs on command only.

## Procedure

`e09_target.ps1`, one phase per call, logs under `C:\BC250\e09\out`: `install` -> `plan -Tag closed` (H1) ->
`gate -On 1` -> `sweep before`, `sweep before2` -> `plan` -> `sweep afterplan` (H2; host: `compare.py plan`) ->
`enable` -> `sweep enabled` (H3; host: `compare.py state`) -> `restore` -> `sweep restored` (H4) -> `enable` ->
`gate -On 1` again (device restart with GART enabled) -> `sweep afterstop` (H5) -> `gate -On 0`.

## Result (run 001)

| | Outcome |
|---|---|
| H1 closed gate | **holds**: refused with `STATUS_DEVICE_NOT_READY` |
| H2 plan | **holds**: 285 planned writes, same offsets and order as amdgpu's first 285, same values except the four predicted address registers, which hold exactly the computed values (scratch `0x46FF00`, dummy page `0x26FFFA`); the 66 further writes of the trace are repeated engine-17 invalidations; sweeps after the plan equal the control |
| H3 enable | **holds**: sequence result 0 (both invalidations acknowledged, MMHUB semaphore taken and released), 285 writes, none refused. Through the witness: 272 registers as written, 8 address registers with our values, 2 (`GCVM/MMVM_L2_CNTL2`, self-clearing invalidate bits) as Linux shows them after init; both `L2_PROTECTION_FAULT_STATUS` = 0; nothing outside the step changed except what the hardware itself does in response, and that is what Linux shows after init too: `SDMA0/1_UTCL1_INV0` `0x800 -> 0x01000800`, engine-17 request `0x00F80001`, acknowledge bit 0. Display alive throughout (stage 61, presents counting) |
| H4 restore | **holds with a residue**: all 282 restorable registers of the step read as the firmware left them. Six registers keep the record that an invalidation ran (last request and acknowledge of engine 17 in both hubs, `SDMA0/1_UTCL1_INV0`): status that cannot be written back. The hypothesis said "every register"; that was too strong |
| H5 restore at stop | **holds**: device stopped with GART enabled, restarted: firmware state, same residue; the new driver instance reports not enabled, no snapshot |

`compare.py` had to learn two things during the run, both visible in `comparison.txt`: a clock register
(`RLC_REFCLOCK_TIMESTAMP_MSB`) ticks about once a minute and can be missed by a control pair, so the known
clocks and counters of earlier noise sets are named; and registers that change as the hardware's own response
are compared with Linux's sweep after init instead of being counted as foreign changes.

What this does not show: that the GPU translates through the table. Nothing can issue a GPU virtual address
before a ring runs (M5); the first GART mapping that carries real traffic will be the ring buffer itself.
