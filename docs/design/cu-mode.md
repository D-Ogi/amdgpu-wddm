# CU mode: 24 or 40 compute units (KMD 0.7.174)

The BC-250 leaves the factory with 24 of its 40 compute units enabled: in each of the four shader arrays
(2 SE x 2 SA), WGPs 3 and 4 are marked inactive. The `bc250-40cu-unlock` reference showed that two
registers per shader array, written together, give all 40 to the dispatcher:

| Register (regcalc, BAR5 byte offset) | Field | Stock on unit A | 40 CU |
|---|---|---|---|
| `mmCC_GC_SHADER_ARRAY_CONFIG` (0x089BC) | `INACTIVE_WGPS` (31:16): what the driver enumerates | `0xFFF80000` | `0xFFE00000` |
| `mmSPI_PG_ENABLE_STATIC_WGP_MASK` (0x0935C) | `WGP_MASK` (15:0): where the SPI dispatches waves | `0x7` | `0x1F` |

Either register alone changes nothing measurable (the reference's four-state A/B test). Stock values: facts
M1(c) and M4 (M4 also records that the BIOS leaves the SPI mask at `0xFFFF` and that it reads `0x7` only after
amdgpu's init, with no host write to it: facts M17).

This change adds the mode as a driver setting, applied at device start. There is no live switch: the mode
changes on the next start of the driver, which normally means a reboot.

PROVENANCE: register names, values and the write point come from duggasco/bc250-40cu-unlock (GPL-2.0); no code was imported.

## Where it runs

`bc250_get_cu_tcc_info()` (driver/shim/bc250_gfx.c, stage 4 "constants") is the transcription of amdgpu's
`gfx_v10_0_get_cu_info()`. The reference writes there, and so do we. Just before the per-SA bitmap loop, the
shim calls `adev->gfx.cu_mode_hook` if one is installed. In replays and host tests the hook is NULL, so the
imported sequence is byte for byte what it was.

- **Policy and register sequence.** `driver/shim/bc250_cu_mode.c` is pure C with no Windows code, so the host
  test runs the same code the miniport runs.
- **Glue.** `driver/kmd/cumode.c` covers the registry, the boot guard, the escape and the caps patch.
- **IRQL constraint.** The stages run under `GartLock` (APC_LEVEL), where the registry cannot be reached.
  `startup.c` therefore brackets `GfxInitializeHardware`:
  - `CuModePrepare` runs at PASSIVE_LEVEL before it. It decides, makes the pending mark durable and installs
    the hook.
  - `CuModeFinish` runs after it, also on failure. It publishes what the registers read back and records the
    fallback.

The sequence, per shader array under `GRBM_GFX_INDEX`:

1. **Precondition.** Read `RLC_PG_CNTL` and `RLC_PG_ALWAYS_ON_WGP_MASK`; both are only read.
   - 40 is refused (`POWER_GATING`) when a power-gating enable of `RLC_PG_CNTL` is set:
     `GFX_POWER_GATING_ENABLE`, `DYN_PER_WGP_PG_ENABLE`, `STATIC_PER_WGP_PG_ENABLE` or
     `GFX_PIPELINE_PG_ENABLE` (masks from `gc_10_1_0_sh_mask.h`). With one on, power gating would decide on its
     own which WGPs have power; static per-WGP PG is the one that acts on `SPI_PG_ENABLE_STATIC_WGP_MASK`.
   - The other bits are not enables. KMD 0.7.174/175 refused on any non-zero value, as the reference measured 0
     under Linux. Unit A reads `0x00800000` (to40 of 2026-09-30, KMD 175): bit 23 alone, which our own
     `bc250_rlc_start()` sets on every RLC start (`gfx_v10_0_rlc_smu_handshake_cntl`: SMU handshake off, GFXOFF
     disabled; AMD's header calls it `RESERVED2`). From 0.7.176 the guard tests the enables only. The whole value
     is still logged and reported by the escape, with its enables next to it in the log.
   - At this stage `STATIC_PER_WGP_PG_ENABLE` alone is accepted (0.7.179). The constants stage runs before the RLC
     stage, as in amdgpu (`gfx_v10_0_hw_init()`: `constants_init`, then `rlc_resume`). On a cold start it meets
     the RLC the PSP started during the firmware load (fact M35), which leaves `RLC_PG_CNTL = 0x8`. E11 measured
     0x8 from the load through the constants stage and 0 after the RLC stage
     (`evidence/windows/2026-09-21-E11-run-001`, `sweep-GC-loaded` to `-s4`, then `-s5`).
   - Pre-driver sweeps under Linux read 0 (E21 `sweep-pre.log`, E03 `sweep-before`), so the firmware before
     the PSP load is not the source.
   - A warm device restart reloads nothing and meets our own `0x00800000`. 0.7.176 therefore took 40 on a warm
     restart (to40-1) and refused it with reason 6 at the next cold boot (06:14Z, 2026-09-30).
   - Any other enable at this stage is a state nobody has measured there, and 40 is still refused before
     anything is written.
   - **After the RLC stage** (`bc250_cu_mode_after_rlc()`, called at the end of `bc250_gfx_rlc_resume()`):
     amdgpu's "disable PG" write (`RLC_PG_CNTL = 0`, then our bit 23) has run. With 40 applied and any PG enable
     still on, every SA goes back to stock, the same verified way as a failed readback. The reason is
     `POWER_GATING`, and the durable fallback follows. The value is logged next to the stage value; the escape's
     `RlcPgCntl` stays the stage value.
   - Clearing the enables ourselves at the constants stage was not chosen. It would mean writing `RLC_PG_CNTL`
     under the running, PSP-started RLC before `rlc_stop`, a write amdgpu never makes.
   - The reference writes at the same point, so a cold boot under Linux meets the same state if Linux's PSP load
     behaves like ours (wishlist L16, unmeasured). Its `RLC_PG_CNTL = 0` is a `umr` reading after init.
2. **Read the entry values.** On the first start of a boot they are stock. A later start in the same boot
   takes stock from a volatile registry record (`Parameters\CuModeBoot`, gone at reboot), because the
   registers may still hold our own writes.
3. **Targets.** 24 is the stock pair.
   - 40 clears `INACTIVE_WGPS` and sets the SPI mask for every WGP of the SA, except WGPs named in the optional
     `CuDisableWgp`.
   - Only WGPs that stock leaves inactive may be named there, so 40 never ends below stock.
   - 40 is refused (`STOCK_UNEXPECTED`) unless the stock SPI mask names exactly the WGPs that CC leaves active.
4. **Write and read back.** Where a value differs from its target, write CC, then SPI (the reference's order),
   then read every SA back.
   - On a mismatch, restore stock on every SA and verify it (`READBACK`).
   - If stock does not read back either, the result is `RESTORE_FAILED` and the applied mode is 0 (unknown).
5. **Broadcast.** Leave `GRBM_GFX_INDEX` broadcasting.

`RLC_PG_ALWAYS_ON_WGP_MASK` is not written. The reference writes `0x1F` to it, but with every power-gating
enable off it should not matter (unit A reads `0x3` there). Its meaning across shader arrays is also not documented where we can
read it.

All five registers are in the Gfx allow table (`gen_regs.py`); `cu_mode_test.c` fails if the sequence touches
one that is not.

A retained-power resume reruns stages 1..8. The hook stays installed for the whole device start, so the
resume reapplies the mode that held at start, against this boot's stock.

## The CU count the UMD sees

`QueryAdapterInfo(UMDRIVERPRIVATE)` copies the measured 24 CU template, then `CuModePatchCaps` overwrites four
fields of its device block: `cu_active_number`, `cu_ao_mask`, `cu_bitmap[4][4]` and `cu_ao_bitmap[4][4]`.
Their offsets are in `umd_caps.h` and are checked against `offsetof` by `umd_caps_test.c`.

- **Source.** The values come from what the registers read back, computed the way `gfx_v10_0_get_cu_info()`
  computes them, including amdgpu's 32-bit wrap of `cu_ao_mask`.
- **Stock is unchanged.** For stock registers the result is byte for byte the template.
- **Both views.** Per SA the caps count the union of the CC view and the SPI view. If the two ever disagree
  (only after `RESTORE_FAILED`), the count is the larger one. Scratch is then sized for every CU that can
  receive a wave, never fewer.
- **No valid run.** If the stage never ran the hook (not 1002:13FE, bring-up failed), the template stays.

### Who consumes the CU count

| Consumer | Uses | Effect of a count lower than the SPI mask |
|---|---|---|
| RADV `radv_device.c:1709` `scratch_waves = MAX2(32 * num_cu, ...)` | scratch ring size | too little scratch: out-of-range writes, VM faults, TDR (0x116) |
| RADV `radv_shader.c:3039` max scratch waves, `radv_queue.c` scratch ring | per-queue scratch | as above |
| `ac_gpu_info.c:1247-1314` `num_cu`, `max_good_cu_per_sa`, `min_good_cu_per_sa` | tess/GS limits, `computeUnitsPerShaderArray` | wrong limits, not unsafe |
| `ac_gpu_info.c:2511` `ac_get_compute_resource_limits` | waves per SH | under-subscription |
| `radv_physical_device.c:2147` `activeComputeUnitCount`, max GFLOPS, perf counters, SPM | reporting | cosmetic |
| DXVK and vkd3d-proton engines | through RADV on the same WDDM winsys | as RADV |
| KMD `bc250_gfx.c:780` MQD `compute_static_thread_mgmt_se0..3 = 0xFFFFFFFF`, `bc250_dispatch.c` | all CUs the SPI allows | none: masks are all-ones |
| RADV winsys `spi_cu_en = ~0` (COMPUTE_STATIC_THREAD_MGMT) | all CUs | none |
| KMD `gfx.c` `inputs.max_cu_per_sh = 10` (now `BC250_MAX_CU_PER_SH`) | shim topology | none: it is the part's maximum |

The desktop's D3D UMD runs on the CPU (llvmpipe) and does not read the count. Nothing in the KMD sizes
anything by the active CU count; the dispatch masks are all-ones, so the SPI mask alone decides.

## Settings and the boot guard

All values are REG_DWORD under `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`. None is in the
INF; an absent `CuMode` means 24.

| Value | Written by | Meaning |
|---|---|---|
| `CuMode` | cumode tool; driver on fallback | 24 or 40. Anything else: stock, reason `INVALID_SETTING` |
| `CuDisableWgp` | cumode tool | with 40: WGPs kept masked, bit `sa * 5 + wgp`, stock-inactive WGPs only |
| `CuModePending` | driver | the encoded 40 request (`mode | disable << 8`) of a start nobody confirmed |
| `CuModeConfirmed` | driver | the encoded request a healthy start confirmed |
| `CuModeLastApplied`, `CuModeLastReason` | driver | what the last start did, readable without the adapter |

**Guard sequence for a 40 request.**

- `CuModePending` is written and flushed **before** the first register write. If that write fails, the start
  applies 24 (`REGISTRY`).
- A start that finds a mark concludes that the previous 40 start died, hung, lost power or was rebooted before
  anyone looked. It then:
  - applies 24,
  - writes `CuMode = 24`,
  - deletes the mark and the confirmation,
  - keeps the reason (`PENDING_UNCONFIRMED`).
- The same durable fallback follows a 40 that did not hold in hardware (`POWER_GATING`, `STOCK_UNEXPECTED`,
  `READBACK`, `RESTORE_FAILED`).
- 40 is never retried unasked: `cumode set 40` is the retry.

**Confirmation.** It needs the healthy milestone the deploy procedure already uses: start-health READY,
visible, completions, at least 60 s of it, fresh.

- A durable start-health CONFIRM confirms the CU mode as well.
- `cumode confirm` sends `BC250_CU_MODE_OP_CONFIRM` itself. That requires an administrator, the generation of
  a READ of this start, and the same milestone (`StartHealthIsReady`).
- Order: `Pending` is deleted first, then `Confirmed` is written. A crash between the two costs a
  reconfirmation, never a false fallback.
- A confirmation covers exactly one encoded request.

## The escape

`BC250_ESCAPE_RUN_CU_MODE` (22) with `BC250_ESCAPE_CU_MODE` (184 bytes, ABI 1) is an adapter-owned software
snapshot with no BAR access. It is dispatched next to start health, before the WDDM diagnostic gate.

- **Flags.** `NoAdapterSynchronization=1`, every other escape flag zero.
- **READ** is open to any caller and returns:
  - requested and applied mode, reason, counted CUs, disable mask, PCI id
  - `RLC_PG_CNTL` and `RLC_PG_ALWAYS_ON_WGP_MASK`
  - per SA: stock CC/SPI, CC/USER/SPI as read back, active WGPs
  - the start generation
  - flags `VALID`, `PENDING`, `CONFIRMED`, `STOCK_RECORD`, `CONSISTENT`, `WROTE`
- **CONFIRM** is as above.

## Safeguards, in one place

- **Device.** Only on PCI 1002:13FE (configuration dword 0); anything else leaves the registers and the caps
  alone (`NOT_THIS_DEVICE`).
- **Accepted values.** Only 24 and 40; the disable mask is limited to stock-inactive WGPs of the part's
  topology.
- **Readback.** Every write is read back; a mismatch restores stock.
- **Boot guard.** An unconfirmed 40 falls back to 24, durably, with the reason kept.
- **Scope of writes.** No clock, voltage or SMU access, no BIOS, SPI flash or NVRAM write. The only lasting
  state is the registry values above.

## Device restart instead of reboot

The history of a warm PnP restart on this driver:

- It hung the machine up to KMD 0.7.126 (facts M78, M101, M267, M330, M334, M402).
- Since 0.7.127, with the no-GFXOFF policy, it has worked (M404, M405, M457).
- KMD deploys use it routinely (live disable/enable with a health check).

The CU mode handles a warm restart: stock comes from the boot's record and the registers are rewritten.
Even so, `cumode restart-device` (DICS_PROPCHANGE) is opt-in and marked lab validation only; the default is
`cumode set 40` followed by a reboot. Once the lab plan below has shown the warm path, the lead may make it the
default.

## Lab validation plan (each step at most three minutes)

| Step | Action | Pass |
|---|---|---|
| 1 | Deploy KMD 0.7.174 by the usual live disable/enable; start-health CONFIRM | `cumode status`: applied 24, reason none, `valid`, no `wrote`; stock CC `0xFFF80000` and SPI `0x7` on all four SAs; no power-gating enable in `RLC_PG_CNTL` (0.7.176 and later: `0x00800000` passes). If stock SPI reads `0xFFFF` at stage 4, stop: 40 would be refused (`STOCK_UNEXPECTED`), and the write point needs rethinking |
| 2 | `bc250kmd_cli read 0x0935C` and `0x089BC` (SE0/SA0 after init); `cumode status` temperature | `0x7`, `0xFFF80000`; temperature below 85 C |
| 3 | `cumode set 40`, then reboot (or `cumode restart-device` for the warm-path trial) | the command returns 0; `CuMode` 40 |
| 4 | `cumode status` after the start | applied 40, counted 40, `PENDING`, `consistent`, `wrote`; CC `0xFFE00000`, SPI `0x1F` on all SAs; `bc250kmd_cli read 0x0935C` still `0x1F` after the full init |
| 5 | `RADV_DEBUG=info` run of the E14 compute client (`vkcompute`) | `num_cu = 40`; GPU hash equals CPU hash; no VM fault, no TDR |
| 6 | Temperature and plug power during a 60 s compute load | under 85 C; power rise recorded next to the 24 CU number |
| 7 | `cumode confirm --wait 120` | `CONFIRMED`; `CuModeConfirmed = 40`; `CuModePending` gone |
| 8 | `cumode set 24`, reboot, `cumode status` | applied 24, CC/SPI stock, counted 24, RADV `num_cu = 24`, compute hash exact |
| 9 | Guard check: `cumode set 40`, reboot, then reboot again without confirming | the second start applies 24, `CuMode` 24, last reason `PENDING_UNCONFIRMED` |
| Rollback | redeploy KMD 0.7.173 (13017eab); remove `CuMode*` values | 24 CUs, the pre-174 caps |

## Tests

- `driver/shim/test/run_cu_mode.ps1`, also a gate in `tools/quality/quick.ps1`, covers:
  - the setting and the boot guard
  - the register values
  - the CU bitmap for 24, 40 and mixed
  - the register sequence against a banked model: cold 24 without writes, cold 40, a disable mask,
    refusals, a stuck SPI (restore), a failed restore, warm restarts with and without the record, resume
    after power loss, topology
  - allow-table membership of every access
- `umd_caps_test.c`: the CU offsets and the stock template's CU fields.
- `firmware_metadata_test.c`: the caps patch runs after the template and the firmware section.
- `start_health_test.c`: a durable confirmation confirms the CU mode outside the spin lock, a failed one does
  not; `StartHealthIsReady` equals the CONFIRM milestone.

## Open questions

- Whether firmware later rewrites the SPI mask from CC during RLC start (M17 shows it changing without a host
  write). The write point is the reference's, where amdgpu with the patch reads `0x7` and leaves `0x1F`. Step 4
  reads it again after the full init.
- Whether CC + SPI alone give the full speed-up without `RLC_PG_ALWAYS_ON_WGP_MASK`. The reference's A/B test
  always wrote it. Step 5/6 throughput decides.
- Linux: `docs/linux-session-wishlist.md` asks for the stage-by-stage value of the SPI mask during amdgpu's init.
