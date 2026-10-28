# TDR recovery S1: the adapter reset of an idle GPU

Status: implemented, not measured on the lab. This page is stage S1 of the
[TDR recovery plan](tdr-recovery-plan.md), with the S0 tools that its trials need. It builds on
[hang recovery](hang-recovery.md), which owns the soft engine reset of nodes 0 and 2.

Before S1, `DxgkDdiResetFromTimeout` failed every time, so every adapter-wide reset ended in bug check
0x116. S1 lets the call succeed in one case: no engine of this driver has work. In every other case the
call fails as before. S1 resets no hardware.

## Sources

| Source | Revision |
|---|---|
| Microsoft driver documentation, `ref/windows-driver-docs` | `110f60ea` |
| DDI reference, `ref/windows-driver-docs-ddi` | `7515063` |
| WDK and SDK headers | 10.0.26100 (`toolchain/nuget`) |
| Linux amdgpu idle tests, `driver/amdgpu-import/reference` | v6.18 |

## 1. The contract

The pages, in `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/` and
`ref/windows-driver-docs/windows-driver-docs-pr/`:

| Page | What it requires |
|---|---|
| `nc-d3dkmddi-dxgkddi_resetfromtimeout.md` | The function "guarantees that the GPU is not writing or reading any memory by the time that DxgkDdiResetFromTimeout returns". Any status other than `STATUS_SUCCESS` makes the operating system bug check |
| `debugger/bug-check-0x116---video-tdr-failure.md` | 0x116 is that bug check: the recovery failed. Parameter 3 is the status that the driver returned |
| `debugger/bug-check-0x117---video-tdr-timeout-detected.md` | 0x117 is the live dump of a timeout that recovered. The machine does not stop |
| `display/thread-synchronization-and-tdr.md` | No other driver thread runs during `DxgkDdiResetFromTimeout`. Interrupts and DPCs can run. After the call, VidMm cleans up. It sends a `TRANSFER` of size 0 for each evicted allocation ("the content was lost during the reset"). It sends `UNMAP_APERTURE_SEGMENT` for each aperture mapping. The driver "shouldn't access the GPU during the preceding calls unless absolutely necessary". Then `DxgkDdiRestartFromTimeout` follows |
| `nc-d3dkmddi-dxgkddi_restartfromtimeout.md` | The OS calls it after the recovery is complete. The driver can access the GPU again. Success or a bug check, as above |
| `display/tdr-changes-in-windows-8.md` | At an adapter-wide reset, the last completed fence of each node advances to the last submitted fence of that time ("For adapter-wide reset ..."). A failed `DxgkDdiResetEngine` is promoted to an adapter reset (TdrReason 9) |
| `nc-d3dkmddi-dxgkddi_resetengine.md`, `nc-d3dkmddi-dxgkddi_querydependentenginegroup.md` | The engine path. S1 does not change it, except for the start witness of a node 0 refusal (section 4) |

## 2. What S1 does

### 2.1 The switch

`Parameters\AdapterRecoveryMode`, a DWORD in the state catalog, read at the device start:

| Value | Behaviour |
|---|---|
| 0 | `DxgkDdiResetFromTimeout` fails every time, as before S1 (0x116) |
| 1 | Success when every witness is idle |
| 2, absent, any other value | The bounded wave kill of [hang recovery](hang-recovery.md) stage 1 on nodes 0 and 2 first, then the same witnesses |

`HangRecoveryMode` 0 also turns S1 off. The decisions are in `driver/kmd/adapter_reset.h`, a header
without kernel types, so that the host test reads the same code.

The new setting changes the catalog identity. A driver of this version therefore needs its own INF and
state migration: put it on a machine with the installer, not with a copy of `bc250kmd.sys` alone. A copy
alone stops the state store at `DriverStateInit` (`STATUS_REVISION_MISMATCH`), and the record of section 3
is then not written.

### 2.2 The order of `DxgkDdiResetFromTimeout`

1. Write the pending record (`LastPath` 2, `LastVerdict` 0), flushed, before any other step.
2. Close every node: `WatchdogFaulted`, the three submit watchdog timers, and the ring gates
   (`GfxSubmitClose`, `GfxComputeClose`, `GfxPagingSubmitClose`). None of these closes adds a health
   fault. The start witness gets the pending-reset mark (BD-120).
3. Retire the fences that arrived late.
4. Start the reset on every node in one critical section, or on none
   (`Bc250AdapterResetBeginAll`). A report in flight or a submission inside the driver gets 500 ms, the
   budget of `WddmResetAdmit`.
5. Mode 2: the wave kill on each IB node whose queue head has a VMID that a kill may name.
6. Read the witnesses (section 2.3).
7. Commit in one critical section: drop the completion queues, advance each node's fence, clear the
   pending state, reopen node 1. Return success.

Any busy witness, at step 4, 6 or 7, stops the attempt. The call then undoes the start of the reset, writes verdict 10
with the busy mask, and takes the path of every earlier version: the sticky closes and
`STATUS_UNSUCCESSFUL`.

### 2.3 The witnesses

All must be idle. Busy bits: `BC250_ADAPTER_BUSY_*` in `adapter_reset.h`.

| Witness | Idle when | Source |
|---|---|---|
| GFX ring (node 0) | The fence slot reached the newest sequence that the driver wrote | `GfxRingsRetired` |
| Compute ring (node 2) | The same, for slot 11 | `GfxRingsRetired` |
| Paging ring (node 1) | No packet in flight, or its fence arrived | `GfxRingsRetired` |
| `mmGRBM_STATUS.GUI_ACTIVE` | 0 | amdgpu `gfx_v10_0_is_idle` |
| `mmCP_STAT.CP_BUSY` | 0 | |
| `mmCP_CPC_STATUS.CPC_BUSY` | 0 | |
| `mmSDMA0_STATUS_REG.IDLE` | 1 | amdgpu `sdma_v5_0_is_idle` |
| `mmSDMA0_GFX_RB_RPTR(_HI)` and `mmSDMA0_GFX_RB_WPTR(_HI)` | Equal, both halves | only while node 1 has its ring |
| Paging queue | Empty, with no packet in flight | commit, under the lock |

The registers are read three times, about 1 ms apart, and every sample must be idle. A register that
`MmioRead` refuses is busy (`UNREAD`), and so is a device that reads all ones. Every register is on the
read list of `regs.generated.h`. `mmCP_CPC_STATUS` was added to the named reads of `gen_regs.py` for
this. The host test checks the read list.

The GFX ring pointers (`mmCP_RB0_RPTR`, `mmCP_RB0_WPTR`) are not a witness. Node 0 is a kernel queue that
the KIQ maps, and what those registers say about such a queue is not measured on this part. The fence of
the newest sequence is the witness instead.

### 2.4 The fence advance

Each node's last reported fence becomes its last submitted fence, so does the scheduler boundary that a
later engine reset and a later preemption report use (`Bc250AdapterResetCommitNode`). A node that has
never submitted keeps the fence that it last reported.

### 2.5 The window between the two DDIs

The render nodes (0 and 2) stay closed until `DxgkDdiRestartFromTimeout`. A submission that arrives on
them is held back, never sent, and counted. At the restart:

| Submissions in the window | Outcome (`Bc250AdapterWindowOutcome`) |
|---|---|
| None | The node opens |
| Only empty ones (no command bytes, no UMD record, no present) | The node opens and the newest one completes in software |
| One with commands | The node stays closed. Its fence never completes, so its next timeout is the old 0x116 |

The paging node (1) opens again at the end of the successful `DxgkDdiResetFromTimeout`, because the
cleanup is paging work. An unmap that the driver held back would leave the GART and the view of VidMm
apart.

`DxgkDdiBuildPagingBuffer` returns success with nothing built for a `TRANSFER` of size 0. Before S1 this
call never came, and `WddmPreparePhysicalTransfer` refused it: that refusal would have been bug check
0x10E (`paging_ddi_status.h`).

Every DDI in the window is logged by name, up to 64 lines (`tdr window:`), so that the first lab trial
shows what dxgkrnl does there.

### 2.6 `DxgkDdiRestartFromTimeout`

After a successful S1 reset it reopens the render nodes (`GfxReopenAfterAbort`,
`GfxComputeReopenAfterAbort`), reopens the start witness with node 0 (`StartHealthRecovered`), adds 1 to
`AdapterRestarts`, and logs the DPM and fan summary. Otherwise it does what it did before: the sticky
close of the start witness.

## 3. The record

`Parameters\HangRecovery` (`guard.c`) has two new values:

| Value | Meaning |
|---|---|
| `LastPath` | 1: `DxgkDdiResetEngine` wrote the record. 2: `DxgkDdiResetFromTimeout` wrote it |
| `AdapterRestarts` | `DxgkDdiRestartFromTimeout` calls that ended a successful S1 reset |

Two new verdicts (`hang_recovery.h`):

| Verdict | Meaning | Counters |
|---|---|---|
| 9 `ADAPTER_RESET` | Every witness idle, success returned. `LastFence` is node 0's advanced fence, `LastKills` the kills, `LastMicros` the time of the whole call | Recovered |
| 10 `ADAPTER_BUSY` | A witness was busy, failure returned. `LastSeq` is the busy mask | Refused |

The pending record counts the attempt, so `Attempts == Recovered + NotDrained + Refused` holds after
every finished call on both paths. A call that the machine did not survive leaves `Attempts` one ahead,
with `LastVerdict` 0 and `LastPath` 2.

## 4. The engine path

With `AdapterRecoveryMode` on, a `DxgkDdiResetEngine` refusal on node 0 keeps the pending-reset mark of
the start witness (`StartHealthCloseKeepPendingReset`), as a refusal on node 1 always did. The scheduler
follows the refusal with `DxgkDdiResetFromTimeout`. When that call proves the engines idle, the restart
reopens the witness. When it does not, it closes the witness for good, as the refusal did before S1.

## 5. Recovery of the desktop path, DPM and the fan

The interop closure (`InteropClosedReason` unclean) and the DPM closure (`DpmMode` 0,
`DpmClosedReason` 4) happen at the next start after an unclean end. A recovered TDR is not an unclean
end, so neither runs. The DPM governor and the fan curve run on their own timers, and S1 does not stop
them: the restart prints their summary as evidence. The desktop route reopens with node 0 and the start
witness. Whether DWM keeps its hosted device across a recovered adapter reset is not measured. The lab
plan checks it.

## 6. The S0 tools

| Tool | Purpose |
|---|---|
| `tools/win/tdrclient` (`bc250tdr.exe`) | A controlled TDR without a hang: FORCETDR (adapter) or ENGINETDR on one node, through `D3DKMT_ESCAPE_TDRDBGCTRL`. It needs `TdrTestMode` 1 (`ref/ddi-display/d3dkmthk.md`, the table of `D3DKMT_ESCAPETYPE`) |
| `tdr window:` log lines | Every DDI between the two timeout DDIs |
| `HangRecovery\LastPath`, verdicts 9 and 10 | Which path ran, and why it refused |

Not done in this stage: the snapshot that `DxgkDdiCollectDbgInfo` copies, and the Vulkan hang client
(event wait, unmapped access). The plan keeps both in S0.

## 7. Tests

`driver/kmd/test/run_adapter_reset.ps1` (in `tools/quality/quick.ps1`) builds the host test over a copy
of `adapter_reset.h`. It checks the switch, each witness alone, every sample, the all-ones device, the
all-or-nothing start of the reset, the fence advance, the commit and restart, the window outcome, the record counters,
and the read list. Two source checks read `wddm.c`: the order of the witness register table, and that the
one success return of `WddmAdapterReset` comes after the witnesses and the busy test. Six negative
controls each fail by a check: `-LastSampleOnly`, `-IgnoreSdmaIdle`, `-PartialBegin`,
`-CompleteNonEmptyWindow`, `-ReorderWitnessTable` and `-CommitBeforeWitness`.

Three existing suites cover the parts of S1 that they compile from `wddm.c`:

| Suite | What it checks for S1 |
|---|---|
| `run_paging_build.ps1` | A `TRANSFER` of size 0 returns success with nothing built, journalled or published, in the window and outside it. A `TRANSFER` with a size is built as before |
| `run_recovery_protocol.ps1` | A node-0 refusal keeps the pending-reset mark when `AdapterRecoveryMode` is on, and clears it when the mode is off. The control `-RefusalIgnoresAdapterRecovery` fails by a check |
| `run_paging_preemption.ps1` | The submit wrapper with the window helper, outside the window |

## 8. Open points

- The duration of `DxgkDdiResetFromTimeout`: up to 500 ms of admission wait, the kill (at most 10 ms
  per node) and 2 ms of samples. Its effect on the scanout is not measured.
- What dxgkrnl sends in the window, and whether the paging node there gets the eviction notices only.
- Whether the lab's dxgkrnl accepts the TDR escape, and in which data form.
- The positive control for "S1 refuses a busy GPU" is a planned 0x116 (plan section 7, decision 2).

Ostrożności nigdy za wiele - one can never be too careful: the success return needs a proof, and the
failure return stays the default.
