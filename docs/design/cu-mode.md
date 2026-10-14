# CU mode: 24 or 40 compute units

## In short

- The GPU of the BC-250 has 40 compute units (CUs). The board leaves the factory with 24 of them enabled.
- The GPU has four shader arrays with five WGPs each, and one WGP holds two CUs. In each shader array, the
  firmware marks WGPs 3 and 4 inactive. That is 4 x 3 x 2 = 24 CUs.
- Our kernel driver can enable all 40. When the driver starts, it writes two registers in each shader array
  and reads them back.
- The unlock does not flash the BIOS. It does not write the SPI flash, CMOS or NVRAM, and it does not change a
  clock or a voltage. The only lasting state is one registry value. Remove the value, restart, and the GPU
  has 24 CUs again.
- The release default is 24, the number the board's firmware switches on (owner, 2026-10-14. tester.30 shipped
  40, BD-077). You choose any even number from 24 to 40, and that choice is written into the key together with
  `CuModeChosen`, the record that every later install reads as your own choice.
- A count above 24 must reach a healthy desktop. If it does not, the next start goes back to 24 by itself and
  records why. The record is `CuModeClosedReason`. With 24 as the release default, an install writes that same 24
  anyway, so there is nothing for `install.cmd -Repair` to reopen there.

## How to turn on 40 CUs

A release install writes `CuMode = 24` by itself (the table of
`tools/release/installer/registry-defaults.json`), so a fresh install starts on the firmware's own 24 CUs and
asks for no confirmation. The two ways to choose the number by hand:

- **When you install:** `install.cmd -CuMode 40` for all of them, or `install.cmd -CuMode 24`. The installer
  writes `CuModeChosen` with the number, so the next upgrade keeps it.
- **After the install:** open the amdgpu-wddm control app, go to the Graphics page, select the number of cores
  (24 to 40 in twos) or pick the pairs yourself on the map, and restart Windows when the app offers it.

After the restart, the start confirmation task (the small blue window after logon) confirms the 40 CU start
once the desktop has run healthy for one minute. Do not end that task.

To check the result:

- A Vulkan application started with `RADV_DEBUG=info` prints `num_cu = 40`.
- `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters` holds `CuModeLastApplied = 40` and
  `CuModeConfirmed`.
- `CuModeLastReason` tells why a start applied 24. The reasons are in the "Settings and the boot guard" section
  below.

To go back to 24, select "Standard (24)" on the same page, or write `CuMode = 24` by hand, and restart. The page
writes the number out as `CuMode = 24`. It does not delete the value: a deleted value is a key that the release
installer never configured, which it then writes its own default over (BD-077).

If one WGP of your board does not work, `CuDisableWgp` keeps it masked while the others run (see the settings
table below). Only the WGPs that the firmware disables are accepted there. The Graphics page writes that mask
itself, either from the count you select (the default set of that count, in the table below) or from the map of
pairs you pick yourself. One pair is one WGP, that is two CUs. The page never says "WGP" to the person.

The lab unit first applied 40 CUs on a cold boot on 2026-09-30 (KMD 0.7.179.1: 40 counted on all four shader
arrays). The release validation installs with `-CuMode 40` and checks the applied mode before it measures.
Each game and LLM result that we publish states the CU count it ran with.

## How it works

The `bc250-40cu-unlock` reference showed that two registers per shader array, written together, give all 40
to the dispatcher:

| Register (regcalc, BAR5 byte offset) | Field | Stock on unit A | 40 CU |
|---|---|---|---|
| `mmCC_GC_SHADER_ARRAY_CONFIG` (0x089BC) | `INACTIVE_WGPS` (31:16): what the driver enumerates | `0xFFF80000` | `0xFFE00000` |
| `mmSPI_PG_ENABLE_STATIC_WGP_MASK` (0x0935C) | `WGP_MASK` (15:0): where the SPI dispatches waves | `0x7` | `0x1F` |

Either register alone changes nothing measurable (the reference's four-state A/B test). Stock values: facts
M1(c) and M4 (M4 also records that the BIOS leaves the SPI mask at `0xFFFF` and that it reads `0x7` only after
amdgpu's init, with no host write to it: facts M17).

The mode is a driver setting, applied at device start. It has been in the kernel driver since 0.7.174, and a
cold boot applies 40 since 0.7.179 (the power-gating check below). Every tester release is newer than that, so
every tester release has it. There is no live switch: the mode changes on the next start of the driver, which
normally means a reboot.

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
     restart (to40-1) and refused it with reason 6 at the next cold boot (06:14Z, 2026-09-30); 0.7.178 refused
     it the same way at the 07:26Z cold boot.
   - **Measured on 0.7.179.1 (2026-09-30, unit A):** warm restart to40-3 applied 40 (counted 40, `num_cu = 40`,
     confirmed); the planned cold boot at 08:22Z then logged "applied 40 CUs (40 counted), reason 0, consistent 1,
     wrote 1, RLC_PG_CNTL 0x00000008 (PG enables 0x8), after the RLC stage 0x00800000 (PG enables 0x0)", CC
     `0xFFE00000` and SPI `0x1F` on SA0-3, `CuModeLastApplied` 40, with DPM active from the same start. The
     stage value 0x8 and the post-RLC 0 are the E11 values; the after-RLC hook undid nothing.
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
INF; an absent `CuMode` means 24. The release installer writes `CuMode = 24` at an install, so the value is
absent only on a machine that this installer never configured (`tools/quality/inf_gates.py` `NOT_IN_INF` holds
the name, because the INF must not clobber the boot guard's own writes).

| Value | Written by | Meaning |
|---|---|---|
| `CuMode` | cumode tool, release installer, control app, driver on fallback | 24 or 40. Anything else: stock, reason `INVALID_SETTING` |
| `CuDisableWgp` | cumode tool, control app | with 40: WGPs kept masked, bit `sa * 5 + wgp`, stock-inactive WGPs only |
| `CuModeChosen` | control app, release installer | the encoded request a person asked for (`mode \| disable << 8`). The driver neither reads nor writes it. The installer reads it to tell a number a person chose from a number a release applied |
| `CuModePending` | driver | the encoded 40 request (`mode \| disable << 8`) of a start nobody confirmed |
| `CuModeConfirmed` | driver | the encoded request a healthy start confirmed |
| `CuModeLastApplied`, `CuModeLastReason` | driver | what the last start did, readable without the adapter |
| `CuModeClosedReason` | driver on fallback | 0.7.216.62: the reason the driver itself wrote `CuMode` 24. `PersistFallback` writes it, a start that reads a `CuMode` other than 24 deletes it, a start that reads 24 (or no value) leaves it alone |

**A count between 24 and 40** (owner, 2026-10-14). The hardware switches CUs on in pairs (one WGP), so the
counts are 24 to 40 in twos. Every count above 24 is `CuMode = 40` with a `CuDisableWgp` mask. 24 is the stock
path with no mask at all. The default set of a count takes the highest free WGP of shader array 0, then of 1, 2
and 3, then the next one down, so the cores stay as even across the four arrays as the step allows
(`CuMode.DefaultOrder`, `CuMode.DefaultDisable`):

| Cores | `CuMode` | `CuDisableWgp` | Cores per shader array |
|---|---|---|---|
| 40 | 40 | absent | 10, 10, 10, 10 |
| 38 | 40 | `0x00010` | 8, 10, 10, 10 |
| 36 | 40 | `0x00210` | 8, 8, 10, 10 |
| 34 | 40 | `0x04210` | 8, 8, 8, 10 |
| 32 | 40 | `0x84210` | 8, 8, 8, 8 |
| 30 | 40 | `0x84218` | 6, 8, 8, 8 |
| 28 | 40 | `0x84318` | 6, 6, 8, 8 |
| 26 | 40 | `0x86318` | 6, 6, 6, 8 |
| 24 | 24 | removed | 6, 6, 6, 6 |

A person who knows which pair of their board is broken picks the pairs themselves on the map of the Graphics
page, which writes any mask inside `0xC6318` (the eight stock-inactive WGPs of this board). The window reads the
board's own stock set from the CU READ snapshot (`StockCc`), so it never offers a pair the driver would refuse
with `INVALID_DISABLE`. On a board whose stock set is not this one, the map is read-only and the counts use the
documented set.

**The record of a person's choice** (`CuModeChosen`, owner 2026-10-14). The release default is 24 and tester.30's
was 40, and `Release\AppliedDefaults` records what the previous installer wrote, not who decided it: a 40 a
person chose in the control application and the 40 tester.30 applied are the same registry state (both end in a
confirmed start, so `CuModeConfirmed` is 40 in both, and tester.30 never wrote a mask). Nothing else tells them
apart, so the upgrade needs its own record:

- The control application writes `CuModeChosen` on every run of the CU setter (step `S1c`), and so does
  `install.cmd -CuMode`. "Reset to defaults" removes it, which hands the number of cores back to the installed
  release.
- The installer reads it before the `update` and `kept` rules (`Get-UserChoice`, decision `chosen`). The record
  has to describe `CuMode` and `CuDisableWgp` exactly as they are before the install. A record that does not (a
  driver fallback moved `CuMode`, or somebody edited the key) is stale and decides nothing. Nothing has to clear
  a stale record: one value stands for one (mode, mask) pair, so it matches again only when the key holds what
  that person asked for.
- A driver closure keeps its precedence over the record, because the driver's act is the later one.
- The upgrade from tester.30 to tester.31 takes a chosen 40 away once, by design: tester.30 wrote no record, so
  its 40 reads as the release default it was. The release notes say so, and the number is one click away.

**Guard sequence for a 40 request.**

- `CuModePending` is written and flushed **before** the first register write. If that write fails, the start
  applies 24 (`REGISTRY`).
- A start that finds a mark concludes that the previous 40 start died, hung, lost power or was rebooted before
  anyone looked. It then:
  - applies 24,
  - writes `CuModeClosedReason` and then `CuMode = 24`,
  - deletes the mark and the confirmation,
  - keeps the reason (`PENDING_UNCONFIRMED`).
- The same durable fallback follows a 40 that did not hold in hardware (`POWER_GATING`, `STOCK_UNEXPECTED`,
  `READBACK`, `RESTORE_FAILED`).
- 40 is never retried unasked: `cumode set 40` is the retry.

**The record of a fallback** (0.7.216.62, BD-077). While the release default was 40, a 24 in the key was either
the fallback's or the tester's, and the release installer had to tell them apart. `CuModeClosedReason` does that:

- Only `PersistFallback` writes it, and the record goes in before the 24 it describes. A start that dies between
  the two writes leaves a record next to a `CuMode` that still asks for 40, and the next start deletes the record
  itself.
- No later start overwrites it. A start that reads `CuMode` 24, or no value at all, leaves it alone
  (`bc250_cu_decide`: the 24 branch returns before `clear_closed`).
- A start that reads any other `CuMode` deletes it (`clear_closed`): the tester, the control application or
  `install.cmd -Repair` wrote over the fallback.
- Two writers delete the record themselves, because their own 24 is not the driver's: the control application
  when the Graphics page selects "Standard (24)" (step `remove-closed` of the setter), and the installer when
  `-CuMode` names a number on the command line. Both are the tester's word about that very value, so the record
  of the driver's fallback is stale. Without them a chosen 24 would read as a closure at every later install,
  which would report the tester's own number as a step back of the driver and offer to take it away.
- The control application reads the record as well: next to a stored 24 it shows "Standard (24) is set, but the
  driver chose that number, not you" with the way back, instead of calling that 24 a choice of the user.
- `CuModeLastReason` cannot do this work. Every start overwrites it, and the reasons of a fallback are not the
  driver's alone: a 24 start whose stock restore does not read back writes `RESTORE_FAILED` there as well. The
  installer therefore reads no legacy record for `CuMode`, and a 24 from a driver older than 0.7.216.62 stays the
  tester's value (`tools/release/README.md`). `DpmLastReason` is sound for `DpmMode` because no start but a
  fallback writes its three reasons.
- The installer's side of the same contract is `$script:DriverClosures` in
  `tools/release/installer/common.ps1`: an install keeps the 24 and names it, `install.cmd -Repair` writes the
  release default again and deletes the record. With the release default back at 24 (owner, 2026-10-14) that
  entry is inert: `Get-DriverClosure` returns nothing where the default equals the closed value, because an
  install writes that same 24 anyway. The entry stays for a release whose default is 40 again, and
  `tools/release/test-cu-mode-installer.ps1` holds it to both cases.

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
- **Boot guard.** An unconfirmed 40 falls back to 24, durably, with the reason kept and recorded in
  `CuModeClosedReason`.
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

This is the original plan, kept as written. The cold-boot result on KMD 0.7.179.1 is in the "Where it runs"
section above.

| Step | Action | Pass |
|---|---|---|
| 1 | Deploy KMD 0.7.174 by the usual live disable/enable; start-health CONFIRM | `cumode status`: applied 24, reason none, `valid`, no `wrote`; stock CC `0xFFF80000` and SPI `0x7` on all four SAs; no power-gating enable in `RLC_PG_CNTL` (0.7.176 and later: `0x00800000` passes). If stock SPI reads `0xFFFF` at stage 4, stop: 40 would be refused (`STOCK_UNEXPECTED`), and the write point needs rethinking |
| 2 | `bc250kmd_cli read 0x0935C` and `0x089BC` (SE0/SA0 after init); `cumode status` temperature | `0x7`, `0xFFF80000`; temperature below 87 C (85 C before 2026-10-01) |
| 3 | `cumode set 40`, then reboot (or `cumode restart-device` for the warm-path trial) | the command returns 0; `CuMode` 40 |
| 4 | `cumode status` after the start | applied 40, counted 40, `PENDING`, `consistent`, `wrote`; CC `0xFFE00000`, SPI `0x1F` on all SAs; `bc250kmd_cli read 0x0935C` still `0x1F` after the full init |
| 5 | `RADV_DEBUG=info` run of the E14 compute client (`vkcompute`) | `num_cu = 40`; GPU hash equals CPU hash; no VM fault, no TDR |
| 6 | Temperature and plug power during a 60 s compute load | under 87 C (85 C before 2026-10-01); power rise recorded next to the 24 CU number |
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
- The host side of the count and the map (owner, 2026-10-14):
  - `tools/win/amdgpu_wddm_control/test/CuTests.cs` (`CuCount`): the count-to-mask table above, the stock set
    read from a snapshot and the boards this page cannot map, a hand-picked map and every map the driver would
    refuse, the record `CuModeChosen` and the plan that removes it, and the sentence that names the cores of
    each group. The G-PLAN expectations of `build.ps1` hold the step order of 24, 30 and 40 and the refusal of
    `--cu-map` without a reading of the board.
  - `tools/release/test-cu-mode-installer.ps1` and `tools/release/test-registry-defaults.ps1`: the release
    default 24, the upgrade rule, a stale record, the inert closure, and the record `install.cmd -CuMode`
    writes.
- The partial path has **not** run on hardware yet. Every count between 24 and 40 is new on the lab. The KMD
  side of it (`bc250_cu_targets` per shader array, the caps patch from the readback) is unchanged since
  0.7.216.64 and is covered by `run_cu_mode.ps1` "a disable mask" alone.

## Open questions

- Whether firmware later rewrites the SPI mask from CC during RLC start (M17 shows it changing without a host
  write). The write point is the reference's, where amdgpu with the patch reads `0x7` and leaves `0x1F`. Step 4
  reads it again after the full init.
- Whether CC + SPI alone give the full speed-up without `RLC_PG_ALWAYS_ON_WGP_MASK`. The reference's A/B test
  always wrote it. Step 5/6 throughput decides.
- Linux: `docs/linux-session-wishlist.md` asks for the stage-by-stage value of the SPI mask during amdgpu's init.
