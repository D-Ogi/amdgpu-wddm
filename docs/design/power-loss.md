# A failed way down when the power goes all the same (BD-188 D-2)

Status: implemented, not measured on the lab. The witness is a HYPOTHESIS until lab row A3 passes (section 9). The
"power kept" branch has no positive control on unit A (A4 is skipped): it is untested on hardware.

`DxgkDdiSetPowerState` "should never fail" (dispmprt.md), and the bus takes the power after the call whatever it
returns. Before D-2, the first failed step of the retained way down (`GpuSetPowerRetained`, `power.c`) ended the save:
the later steps did not run and the phase went to FAILED. At D0 nothing was restored, the first job met a chip without
firmware, and its TDR ended in bug check 0x116. D-1 (`WddmSuspendRetained`, `power_drain.h`) removed the most frequent
cause of a failed step 1. D-2 removes the class: a failed step no longer stops the save when the power goes.

## 1. The rule

| Way down | Result | D0 |
|---|---|---|
| Every step passes | SUSPENDED (as before) | Normal restore. The witness is read and logged only |
| A step fails, the power goes (section 2) | SUSPENDED_UNSAVED. The later steps still run | Section 4 |
| A step fails, the power can stay (D1 or D2 without a sleep action) | FAILED (as before) | Refused (as before) |

`BC250_POWER_SUSPENDED_UNSAVED` is phase 5 (`power_phase.h`). The call returns the first failure, as before, so the
display half in `pnp.c` behaves as it did for a failed way down. `Device->RetainedDownFailed` holds the failed steps
(bit k-1 for step k).

## 2. When the power goes

`Bc250PowerLossExpected` (`power_loss.h`): a D3 target, or the action `PowerActionSleep` (S1-S3) or
`PowerActionHibernate` (S4). dxgkrnl sends D3 only when it gives the adapter to the bus. The driver cannot tell D3hot
from D3cold at that time, so the witness at D0 decides what really happened.

## 3. The forced way down

The steps keep their order. Each step that fails is recorded, and the next step runs:

| Step | After a failure |
|---|---|
| 1 WDDM | `WddmPauseForPowerLoss`: the pause of `WddmSuspendRetained` (timers and DPCs off, `Stopping`, `RetainedPowerPause`), with every pending record kept. It records the busy mask in `PowerLostWork` and, for nodes 0 and 2, the newest ring sequence in `PowerLostSeq`. It completes nothing, frees nothing and moves no fence |
| 2 IH | Runs as before |
| 3 GFX | Runs as before. A refused stop sets `Failed`, `GpuStopUnconfirmed` and now `PowerHaltFailed` |
| 4 PSP | Runs as before. Its own precondition refuses it when the GFX is not suspended or the stop is unconfirmed. Without memory for the PSP report, this step fails and the other steps still run |
| 5 GART | Runs as before. Its own precondition refuses it in the same cases |

power.c calls functions. It sets no flag of another block.

The call returns a failure, so `pnp.c` would put the committed display shape back at once, as after a failed way down.
After an unsaved way down it does not (`Bc250PowerDownRepairsDisplay`, `power_phase.h`): the engines are down, and a display
writer would act on a suspended device. The D0 restores the display.

## 4. The witness

The last act of every way down whose power goes, saved or unsaved, writes `BC250_POWER_WITNESS_PATTERN` into the
witness register and reads it back. Only a read-back that matches arms the witness. At D0, before the SMU starts and
before any restore step, the driver reads the register once:

| Read | Verdict |
|---|---|
| 0 (the power-on value) | Power lost |
| The pattern | Power kept |
| Any other value, a refused read, or a witness that was not armed | No proof |

Then the driver writes 0, so that a later D0 cannot read a stale pattern.

The register is per-IP data (condition f): `asic_ip.h` gives the GC version of the device by PCI id, and
`Bc250PowerWitnessFor` maps GC 10.1 to `BC250_POWER_WITNESS_GC_SCRATCH_REG1`. The shim resolves the id with
`SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG1)` (`bc250_gfx_power_witness`), and the access goes through the GFX register
table (`gen_regs.py`). A device without a row has no witness, so its verdict is always "no proof".

### 4.1 Producers of SCRATCH_REG1

Checked with `git grep` on 2026-11-09 (lane l7, base 9632e4a6f plus the l7 changes):

- KMD and shim: no code writes `mmSCRATCH_REG1` or its offset. The register headers, `gen_regs.py` (E07 control and
  now the witness row), `regs.generated.h` (the raw MMIO write escape of the lab flavour, BD-167), `tools/win/bc250rd`
  (read lists), facts M30 and experiment E07 (a manual write with `bc250kmd_cli`) name it. Only a person who uses the
  lab-flavour raw write escape can write it. The kit README forbids raw register writes during the power rows.
- Ring and IB tests: the PM4 ring tests write `SCRATCH_REG0` with `SET_UCONFIG_REG`. `SCRATCH_REG1` (0x2041) is below
  `PACKET3_SET_UCONFIG_REG_START` (0xC000), so no `SET_UCONFIG_REG` can reach it. The SDMA tests write memory.
- The UMD tree of the release (Mesa 64c082d37c4): only the register database names it. No code uses it.
- amdgpu (`gfx_v10_0.c`) names `scratch_reg1` only for the RLCG register access of an SR-IOV VF. This part runs on bare
  metal.
- Linux evidence: after amdgpu's full init the register reads 0 (E03, E21).
- Not provable with grep: the RLC, CP, SDMA and PSP firmware. Lab row A3 is the positive control.

## 5. D0 after an unsaved way down

1. Phase RESTORING. The witness verdict (section 4).
2. Power kept or no proof: FAILED, as before. Nothing restores over a chip that can still hold state.
3. Power lost: each block takes the software state of a successful retained suspend, through its own function, and
   logs what it changed: `IhPowerLost`, `GfxPowerLost`, `PspPowerLost`, `GartPowerLost`. `GfxPowerLost` clears
   `GpuStopUnconfirmed`: the only place outside a start that clears it, and only with the witness. The witness proves the
   chip state, but not that the OS stopped calling the ISR. `IhPowerLost` therefore calls the IH quiet only when interrupt
   admission is closed, and `GfxPowerLost` clears `GpuStopUnconfirmed` only when the IH is quiet. With open admission the
   restore refuses, and the D0 is FAILED. `GfxPowerLost` clears `Failed` only when the retained stop set it
   (`PowerHaltFailed`). A fence that a ring owes is dropped: every ring starts
   empty. Jobs in flight stay in flight (`Gfx->PowerLostWork`).
4. The normal restore runs, with the normal failure rule: a failed restore step is FAILED.
5. `WddmResumeRetained` accepts the work that the forced pause recorded in place of "idle"
   (`GfxSubmitRestoredWithLostWork` for the engines). It queues the report DPC and the paging drain that the pause
   removed. No fence moves (condition c).
6. Phase ACTIVE.

## 6. The TDR after D0

The lost jobs never complete: their ring contents went with the power. dxgkrnl times them out.

- `ResetEngine`: a head job at or below `PowerLostSeq` of its node is refused before any kill
  (`Bc250HangPowerLost`, verdict `POWER_LOST` 13). The restored engine holds no wave and no queue state of it.
- `ResetFromTimeout` (mode 2): unchanged. The rings read unretired, so `Bc250AdapterQueueResetPlan` resets the queues.
  Each queue reset proves the engine with a ring test and writes the newest sequence into its fence slot. The commit
  advances every node to its last submitted fence (the adapter-reset contract, the only place where fences move) and
  clears `PowerLostSeq`.
- Modes 0 and 1 have no queue reset, so the refusal and 0x116 stay. Those modes are the opt-outs.

## 7. Host checks

`power_coordinator_test.c` (generated from `power.c`):

- Saved way down: the witness is logged.
- Power kept: FAILED, no restore.
- Power lost after a failed step 1 or step 3: the power-lost state, then the full restore in order.
- A failed restore stays FAILED.
- No proof (other value, refused read, not armed, no witness row): FAILED.
- D1 without a sleep action: FAILED at the first step.
- No memory for the PSP report: step 4 fails. At a hibernate the other steps run and the D0 after a power loss
  restores. At D1 without a sleep action the way down is FAILED.

`retained_power_test.c` (generated from `wddm.c`): the forced pause keeps every record and fence and records the lost
sequences. The resume accepts only that work.

`power_loss_test.c` with source checks of `wddm.c`:

- The pure rules.
- ResetEngine refuses before any kill.
- No fence moves in the pause or the resume.
- The commit clears `PowerLostSeq`.
- `IhPowerLost` calls the IH quiet only with closed interrupt admission, and `GfxPowerLost` clears `GpuStopUnconfirmed`
  only with a quiet IH.
- `pnp.c` repairs the display only by `Bc250PowerDownRepairsDisplay`.

Negative controls in `tools/quality/quick.ps1`, each must fail: `power-coordinator-forced-d1`,
`-witness-ignored`, `-failed-restore-active`, `-lost-before-witness`, `power-loss-no-rule`,
`power-loss-fence-at-resume`, `power-loss-ih-quiet-always`, `power-loss-repair-after-unsaved`.

## 8. Risks

1. The witness is a hypothesis until A3 passes.
2. A full restore after a failed GFX stop relies on the chip being at its power-on state. Only the witness proves it.
3. The forced pause keeps records of work that no hardware holds through D3. Only the TDR of section 6 ends them.

## 9. Lab rows

The l7 kit runs A3 first. A3 writes a verdict file, and A5-A7 refuse to run unless it says "witness 0 at D0".

| Row | What |
|---|---|
| A3 | Positive control, power lost: a normal hibernate with RTC wake. Expect the witness armed at the way down and 0 at D0 |
| A4 | Positive control, power kept: needs a D3 without power loss, which unit A does not give. Skipped. Untested |
| A5 | `LabPowerDownFault` 1 and a hibernate: SUSPENDED_UNSAVED, witness 0, full restore, ACTIVE, a working desktop |
| A6 | `LabPowerDownFault` 3 and a hibernate: the same, with `GpuStopUnconfirmed` cleared by the witness |
| A7 | A job pending at the way down, then a hibernate: the TDR after D0 recovers (4101), no 0x116 |

## 10. Where the code is

| Part | File |
|---|---|
| The rules, the witness ids and the pattern | `driver/kmd/power_loss.h`, `asic_ip.h` |
| The forced way down, the witness, the D0 decision | `driver/kmd/power.c` |
| The phase | `driver/kmd/power_phase.h` |
| The forced pause, the resume with lost work, the ResetEngine refusal, the commit | `driver/kmd/wddm.c` |
| The power-lost state of each block | `driver/kmd/ih.c`, `gfx.c`, `psp.c`, `gart.c` |
| The witness access | `driver/kmd/gfx.c` (`GfxPowerWitnessRead`, `GfxPowerWitnessWrite`), `driver/shim/bc250_gfx.c` |
| The PCI id and the witness of the device | `driver/kmd/pnp.c` |
