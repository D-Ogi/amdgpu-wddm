# Hang recovery, stage 1: soft recovery in ResetEngine (KMD 0.7.216.17)

Every GPU hang on unit A so far has ended the same way: bugcheck 0x116. The trials behind the reports (147, 151,
153, 208, 245, 251) share one shape. A job's waves take no-retry GFXHUB faults, latch `MEM_VIOL` and stop, and the
end-of-pipe behind them never fires. The 500 ms submit watchdog closes node 0, so the desktop stops too. About
10 s later (`TdrDelay` 10 on the lab), dxgkrnl runs its per-engine TDR. `DxgkDdiResetEngine` refuses,
`DxgkDdiResetFromTimeout` fails, and the machine bugchecks. KMD 0.7.216.13 added stage 1 of M15.12, behind the
registry switch `HangRecoveryMode`: kill the waves, wait for the fence, and if it retires, report a successful
engine reset so that dxgkrnl removes only the guilty process's device. Lab trial D of 2026-10-07 never got to the
kill. A fence guard refused it because of a defect in this driver, which 0.7.216.16 removes (see "Lab trial D:
the fence bound is per node" below). Trial D1 of 0.7.216.16 got to the kill, but no kill reached the register.
The kill took the register path of the GART sequence, which 0.7.216.17 corrects (see "Lab trial D1" below).
Whether the kill drains the ring on gfx1013 is still an open question. Only a lab trial or the Linux measurement
(wishlist L43) can answer it.

## The switch

`HangRecoveryMode` is a REG_DWORD under the service's `Parameters` key, read once in `WddmStart`. Only the value
1 turns stage 1 on. With the switch on, the start logs `wddm: HangRecoveryMode 1: ...`. Absent or 0 leaves every
TDR DDI exactly as it is in 0.7.216.14, the driver of release 0.7.216.100-tester.20. The shim's kill helper is
compiled in but never called, no register of the new table entry is ever written, and the start logs nothing new.
Every install of the package writes 0. The INF writes it without NOCLOBBER, as it writes `EnableHangBugcheck`, so
an experiment that somebody left on does not survive a reinstall (`tools/quality/inf_gates.py`, `UNCONDITIONAL`).

## What ResetEngine does with the switch on

Stage 1 handles node 0 (3D/compute) only. Node 1, the paging node, keeps the refusal. dxgkrnl follows a
successful reset of a paging packet with an adapter-wide reset ("TDR changes in Windows 8", step 9), which on
this part is the 0x116 path anyway. For node 0, `Bc250WddmResetEngine` (`wddm.c`) does this:

1. It retires late fences (`WddmGpuFence`). Then, under `Lock`, it reads these values:
   - the head of node 0's completion queue, and whether its `Node` is the node the DDI names.
   - the OS fence of that job, which is the fence a reset would abort, and the VMID of that job.
   - the ring sequence of the tail job, the newest on the ring.
   - the last fence that the report DPC gave to dxgkrnl for the node of the reset: `LastReportedFence[node]`,
     through `Bc250HangNodeLastCompleted`. This value is per node. It is not the adapter-wide
     `LastCompletedFence` (see "Lab trial D" below).
2. It decides refusals that need no hardware first (`Bc250HangPreKillVerdict`, `hang_recovery.h`):
   - nothing of this node on the ring: verdict 3.
   - an abort fence outside `[last completed, last submitted]` of this node, or a node with no reported fence
     yet: verdict 4. Reporting a fence outside that range would be bugcheck 0x119 (parameter 0xA). The log
     then also gives the bound and the abort fence (`wddm: soft recovery fence guard: ...`).
   - a VMID a broadcast wave kill must not name: verdict 6 (see "The VMID" below).
3. Otherwise it writes the record's pending entry and flushes it (see "The record" below), and only then kills.
4. `GfxSoftRecover` (`gfx.c`) follows the shape of amdgpu's `amdgpu_ring_soft_recovery`, for at most
   `BC250_SOFT_RECOVER_US` (10 ms, amdgpu's value):
   - it holds `GartLock` and installs the GFX sequence as the register path, as `GfxSubmitIb` does.
   - it reads the fence of the newest sequence.
   - if that sequence has not retired, it issues `bc250_gfx_soft_recover_vmid`, waits 100 us and reads again.
   - if the register path refuses a kill, the loop reads the fence one last time and ends.

   The loop itself is `Bc250HangKillLoop` (`hang_recovery.h`), which the host test also runs. The ring runs in
   order, so when the newest sequence retires, every job on the ring has retired and the ring is idle. Verdicts: 5
   when the sequence retired before the first kill, 1 when it retired after kills, 2 when the time ran out or the
   register path refused a kill.
5. It writes the verdict and flushes it.
   - On 1 or 5, the reset succeeds. `LastAbortedFenceId` is the head job's fence. The queued jobs are dropped,
     the node-0 gates are reopened (`WatchdogFaulted`, `RefusalPending`, `RejectedPending`, `CompletionPending`,
     `CompletionRetries`, `PreemptionPending`), and node 0's last reported fence is advanced to the aborted one,
     as the engine-reset contract asks. Only node 0's values change (`SubmittedFence[0]`, `LastReportedFence[0]`).
     Then `WddmGfxHeadLocked` closes the queue's head state. It clears `HwPending`, cancels the submit watchdog
     and closes the ring-gap edge the histogram of 0.7.210 counts. `SoftRecoveries` counts the call, and the
     teardown summary prints it.
   - On 2, 3, 4 or 6, nothing is changed, and the call falls through to the refusal of 0.7.216.14, including the
     `StartHealthClose` of that revision and the `GfxRetireSignal` wake KMD 196 added.

The kill helper (`driver/shim/bc250_gfx.c`) transcribes amdgpu's `gfx_v10_0_ring_soft_recovery` (AMD MIT). Upstream
removed it in favour of queue resets, which this part does not have. It is one `mmSQ_CMD` write with
`CMD = SQ_IND_CMD_CMD_KILL`, `MODE = SQ_IND_CMD_MODE_BROADCAST`, `CHECK_VMID = 1` and `VM_ID` = the hung job's
VMID. The field masks come from `gc_10_1_0_sh_mask.h` and the enum values from `navi10_enum.h`. The RLC safe-mode
bracket is a no-op on this part (`cg_flags` = 0). `mmSQ_CMD` is the one register this feature adds to the GFX write
table. Its address comes from `gen_regs.py` and regcalc. The kill does not write `GRBM_GFX_INDEX`, because the
broadcast mode already addresses every SE, SH and CU. We deviate from upstream in one way: we wait 100 us between
kills, where amdgpu spins. The bound is the same 10 ms.

On success, `GfxSoftRecover` is the one place that clears the sticky `SubmitFailed`, and it does so only after the
fence has retired. It also clears `SubmitInFlight`, ends the DPM busy share and signals the retire waiters
(KMD 196: a submission held in `GfxSubmitWait` is waiting for exactly this gate). `DpmBusyEnd` is idempotent, so
this is harmless when the interrupt path got there first. StartHealth stays closed: the start did see a fault.
Rendering does not depend on StartHealth, but the custom DPM and CU-mode escapes do. Until the next device
restart they answer `STATUS_DEVICE_NOT_READY`, while the governor inside the KMD keeps running.

## The VMID

The first version of this design (KMD 0.7.194, on 0.7.193.1) named the kill's VMID with a constant,
`BC250_WDDM_VMID` = 1, because every WDDM job ran at VMID 1 and `gfx.c` refused a job of another page-table root
while one was in flight. **That assumption is gone.** KMD 0.7.214 added the VMID pool
(`vmid_pool.h`, `docs/design/gfx-submit-root-serialization.md`): a root gets a VMID of its own out of 1 and 3..15
and keeps it while it keeps submitting, and jobs of different roots - the game's and DWM's - are on the ring at
the same time on purpose. The constant no longer exists in the tree.

Stage 1 therefore takes the VMID from the hung job's own completion-queue entry (`BC250_GFX_COMPLETION::Vmid`,
recorded at the submit by KMD 214) and checks it with `Bc250KillVmidValid` before the kill: VMID 0 is the
GART/system domain and VMID 2 is SDMA paging's, so a broadcast `CHECK_VMID` kill on either would stop waves
that are not the hung application's, and a recorded 0 means the entry named no VMID at all. Either is verdict 6
and today's refusal, with nothing written to the hardware. `gfx.c` repeats the check before the register write,
because a broadcast wave kill is not something to issue on a VMID the caller could not name.

Only the hung job's VMID is killed, not every VMID on the ring. The GFX ring is in order: a job queued behind the
hung one has not started its waves, so it needs no kill and runs to completion once the head drains. This is also
why the drain is judged on the newest sequence retiring: it can only retire after every job before it did.

A VMID can hold several jobs, because the pool reuses a VMID for every job of the root it already holds. The
broadcast kill takes the waves of all of them. They belong to that one page-table root, which is the process whose
device dxgkrnl is about to put into the error state, so nobody else's work is cut short.

## The record

Each stage-1 call leaves its verdict in the registry, under
`<service key>\Parameters\HangRecovery`, a non-volatile subkey. `GuardRecordHangRecovery` (`guard.c`) writes it
and flushes it with `ZwFlushKey`. The log ring is about 1024 lines and lives in memory, so it does not survive the
0x116 that follows a refusal. The registry record does.

- Counters: `Attempts`, `Recovered` (verdicts 1 and 5), `NotDrained` (2) and `Refused` (3, 4 and 6). The driver
  never resets them.
- The last call: `LastVerdict`, `LastSeq`, `LastFence`, `LastKills`, `LastMicros`, `LastTime` (a UTC FILETIME,
  REG_QWORD) and `LastVersion`. Since 0.7.216.17, `LastKills` counts only the kills that reached the register. A
  verdict 2 with `LastKills` 0 is a refused kill, and the log ring then has `gfx: soft recovery kill refused`.

A call that kills writes twice: a pending entry (`LastVerdict` 0, `Attempts` + 1) before the first `SQ_CMD` write,
and its verdict before it returns. A refusal decided before any kill writes once. After every call that the
machine survives, `Attempts == Recovered + NotDrained + Refused`. If `Attempts` is one ahead and `LastVerdict` is 0,
the machine went down during the kill. `test/hang_recovery_test.c` checks this arithmetic.

The record is a subkey and not a set of values on `Parameters` because of the deploy kit. The kit compares the
value names of `Parameters` with its capture, and its recovery writes the captured values back. Neither looks at
subkeys. A write failure is logged and otherwise ignored: the record is evidence, never a gate.

## The contract, read again against today's code

Read from `ref/windows-driver-docs` (`display/tdr-changes-in-windows-8.md`,
`display/timeout-detection-and-recovery.md`) and `ref/ddi-display/d3dkmddi.md` (WDK 10.0.26100 declarations). Three
places where the contract and this driver have to be said out loud:

- **`DXGKARG_RESETENGINE` has no `LastCompletedFenceId` field.** The conceptual page says "the last completed
  fence ID should be set to the value of the `LastCompletedFenceId` member returned by the engine reset call", but
  the `d3dkmddi.h` declaration of WDK 10.0.26100 carries exactly three members: `NodeOrdinal`, `EngineOrdinal` and
  `LastAbortedFenceId`. The header wins (CLAUDE.md "References"). We therefore advance node 0's own
  `LastReportedFence` to the fence we report as aborted. That is the intent of that sentence, and the preemption
  notification later needs it, because it reads the same per-node value.
- **"Nothing remains in the physical adapter's hardware queue. The specified nodes are ready to accept new
  packets."** This is why the drain is the precondition of reporting success and why the queue is emptied and
  node 0's gates reopened before the return. A verdict of 2 cannot honour it, so it stays a failure.
- **Packets behind the aborted one are the scheduler's business.** "Render packets: the GPU scheduler assigns
  render packets new fence IDs and then resubmits them." Dropping the queue entries behind the head therefore
  loses no work. Since the VMID pool those later packets may belong to another process, which is a change from
  the 0.7.194 note: the kill does not touch their VMID, and the scheduler resubmits them.

## Lab trial D: the fence bound is per node

Trials A to D ran on unit A on 2026-10-07 with KMD 0.7.216.13 (SYS `BFFE3684`). A, B and C gave the expected
results. B is the positive control with the switch off: a long hang, the refusal, and 0x116. In trial D (switch on,
long hang), the log ring of the dump has these lines in sequence:

- `FENCE TIMEOUT seq 10012 fence 1186 vmid 12`.
- `hang record: verdict 4 ... kills 0`.
- `soft recovery verdict 4, refusing as before`.
- `ResetFromTimeout failed`, then bugcheck 0x116.

The record says `Attempts 1`, `Refused 1`, `LastVerdict 4`. Thus the fence guard refused before any kill. The
fence values in the dump show the cause:

| Value in the dump | Fence |
|---|---|
| `LastCompletedFence` (adapter-wide) | 9228, a node 1 (paging) fence |
| `LastReportedFence[0]` (node 0) | 1185 |
| fence of the hung node-0 job | 1186 |

Fence ids are counted per node, and on the lab the ids of node 1 run far ahead of the ids of node 0. The report DPC
writes `LastCompletedFence` at every delivered completion of either node. Thus it holds the newest report of any
node, which in trial D was a paging fence that completed after the hang. 0.7.216.13 read that field as the lower
bound of the 0x119 range. The guard then compared 1186 with 9228, `Bc250AbortedFenceValid(1186, 9228, 1186)` was
false, and a valid recovery was refused. This occurs each time paging work completes after a hang, so the defect
blocked every lab hang in practice.

0.7.216.16 makes these changes:

- ResetEngine reads the bound of the reset node only: `LastReportedFence[node]`, valid when
  `LastReportedValid[node]` is set (`Bc250HangNodeLastCompleted`, `hang_recovery.h`). This is the value that the
  report DPC last gave to dxgkrnl for that node, and the preemption report already uses it. A node with no
  reported fence fails the guard, because nothing then proves the range.
- A recovery writes only the state of node 0. 0.7.216.13 also wrote the hung fence into `LastCompletedFence`, which
  moved that field back below fences that node 1 had already reported.
- `LastCompletedFence` stays as a diagnostic value only. The summary line and the stop line of the log print it, and
  `tools/runcompare` reads the stop line. No code reads it as a bound.
- A verdict 4 also logs the bound and the abort fence, so the log alone tells which value refused the recovery.
- The host test models two nodes with interleaved fence ids: node 0 at 1185 and hung at 1186, node 1 at 9228 and
  the last to report. The kill must go ahead. The negative control `-SharedLastCompleted` puts back the read of
  0.7.216.13 at the read site of the test, and the test must then fail.

## Lab trial D1: the kill took the wrong register path

Trial D1 ran on unit A on 2026-10-07 with KMD 0.7.216.16 (SYS `B7DDED3A`), the switch on and a long hang. The
fence guard passed. The record says `LastVerdict 2`, `LastSeq 15096`, `LastFence 1539`, `LastKills 95` and
`LastMicros 10087`. Then `ResetFromTimeout` failed and the machine bugchecked with 0x116. The log ring of the dump
(`kmdlog.py` with the map of the build) has these lines in sequence:

- `FENCE TIMEOUT` of seq 15096 at VMID 4, with the register snapshot of the timeout.
- `hang record: verdict 0`, the pending entry.
- `gart: register 0x08DEC refused (0xC0000022), sequence stopped`, at the same time as the pending entry.
- `gfx: soft recovery of seq 15096 did not drain: 95 kill(s) of VMID 4 in 10087 us`.

`0x08DEC` is `GC.SQ_CMD` (regcalc), the register of the wave kill. The refusal names the GART sequence. All MMIO
of the shim goes through `adev->backend`, and each sequence checks the offset against its own table. `SQ_CMD` is
in the GFX table only. `GfxSoftRecover` did not install the GFX sequence, so the kill used the backend that was
installed, which was the GART sequence. The first kill was refused with `STATUS_ACCESS_DENIED`. A refusal stops
the sequence, so the next 94 writes were dropped. The loop counted all 95 as kills, but no kill reached the
register.

The snapshot of the timeout agrees with waves that nothing killed. It shows the client's spin and no fault:

| Register | Value | Fields that are set |
|---|---|---|
| `CP_STAT` | `0x80060000` | `ME_BUSY`, `QUERY_BUSY`, `CP_BUSY`. |
| `CP_BUSY_STAT` | `0x00040000` | `EOP_DONE_BUSY`. |
| `CP_STALLED_STAT2` | `0x00200000` | `EOPD_FIFO_NEEDS_SC_EOP_DONE`: the end-of-pipe waits for the shaders. |
| `GRBM_STATUS` | `0xA0403028` | `SPI_BUSY`, `GUI_ACTIVE`: waves are on the hardware. |
| `GCVM_L2_PROTECTION_FAULT_STATUS` | `0x00000000` | No VM fault. |

amdgpu's `amdgpu_ring_soft_recovery` (`ref/linux-src` v6.18, `amdgpu_ring.c`) does the same thing as our loop. It
writes `SQ_CMD` again and again for 10 ms and reads the fence between writes. No hardware step is missing from our
sequence. `CP_VMID_RESET` occurs only in the KIQ queue resets of `gfx_v10_0.c`, which is stage 2. The missing step
was in this driver: the register path of the write.

0.7.216.17 makes these changes:

- `GfxSoftRecover` runs as `GfxSubmitIb` runs. It holds `GartLock`, gets the device through `GartDevice`,
  installs the GFX sequence as `adev->backend` and starts it (`SequenceBegin`). After the loop, it puts the previous backend back.
  It takes `GartLock` in place of the `GfxAccess` reference, because a holder of that reference must not take
  `GartLock`.
- The loop is `Bc250HangKillLoop` in `hang_recovery.h`. A kill counts only if the sequence has no fault after it.
  The first refusal ends the loop after one last look at the fence. The verdict is then 2, and the log ring gets
  `gfx: soft recovery kill refused` with the register and the status.
- `gen_regs.py` names `SQ_CMD` (`BC250_REG_GC_SQ_CMD`), so that the host test can look it up in the generated
  tables.
- The host test runs `Bc250HangKillLoop` over a model of the register path with the generated GART and GFX
  tables. It checks that only the GFX table holds `SQ_CMD`. It also checks the trial-D1 drain through the GFX
  sequence and a refused kill that is not counted. Three negative controls must fail:
  - `-GartBackend` puts back the backend of 0.7.216.16 in the model.
  - `-CountRefusedKills` counts a refused kill in the loop.
  - `-NoBackendSwitch` removes the backend switch from the copy of `gfx.c` that the source check reads.
- `run_hang_recovery.ps1` reads `GfxSoftRecover` in `gfx.c` before the build. The function must take `GartLock`,
  install and start the GFX sequence, run the loop, and then put the backend back and release the lock, in that
  sequence.

## Why not in the 500 ms watchdog

Killing at watchdog time would cut the freeze from about 10 s to 0.5 s. It would also turn the hang into an
ordinary `DMA_COMPLETED`: the guilty application would keep its device and read garbage. Only a successful
`DxgkDdiResetEngine` lets dxgkrnl put the guilty device into the error state while the system device carries on.
The watchdog therefore still closes the ring and records its register snapshot, unchanged.

## Why there is no stage 2

The stage-2 candidates are a KIQ queue reset (`gfx_v10_0_reset_kgq`, which re-initialises the queue) and a
CP-only `GRBM_SOFT_RESET`. On this part both would be a second bring-up or a reset without a measured-good
sequence. The facts are against that:

- a second bring-up in one start fails at the KIQ (M44).
- amdgpu's own recovery hangs the machine (M53).
- a second init in one boot hangs unit A (M55).

If stage 1 does not drain on gfx1013, the next step is the Linux measurement (wishlist L43), not a blind queue
reset.

## Limits

- What the trial client (`tools/win/hangclient`) exercises is not the hang class of the reports. Its compute
  dispatch spins on live waves, which `KILL` stops. The waves of the real reports have already stopped on
  `MEM_VIOL`. The opcode is defined to stop them too, but that has not been measured on this part. A MEMVIOL
  fault cannot be formed safely through the checked system D3D12 runtime.
- When the ring holds several jobs (up to `BC250_GFX_PENDING_MAX`, 7), the drain waits for the newest, but the
  reset names the head as aborted. With the VMID pool open, the later jobs may belong to other processes. The
  scheduler resubmits them with new fence ids, and only the head's device goes into the error state.
- A job that ran past the 500 ms watchdog but completed before the TDR leaves nothing on the ring, so it gets
  verdict 3 and today's refusal. The contract would allow reporting the last completed fence as aborted, but our
  view of "last completed" can lag a pending report, and a wrong choice re-executes a packet.
- Reopening the node re-admits work onto hardware that has just faulted. Proof that the ring drained is the
  precondition, and the next job brings its own VM flush. A second hang is still bounded by dxgkrnl's
  `TdrLimitCount`.
- The host suite covers the decisions and the kill loop, never the kill on the hardware. `hang_recovery_test.c`
  holds the gate, both guards, the per-node bound of the fence guard, the pre-kill verdict, the kill loop over a
  model of the register path and the record's arithmetic. `run_hang_recovery.ps1` also reads the backend switch in
  `gfx.c`. It carries six negative controls that must fail: `-IgnoreFenceGuard`, `-IgnoreVmidGuard`,
  `-SharedLastCompleted`, `-GartBackend`, `-CountRefusedKills` and `-NoBackendSwitch`. Whether an `SQ_CMD` write
  that reaches the register drains the ring is measured only on the lab.
- The host test models the read site of `wddm.c` and the report DPC's writes. It does not compile `wddm.c`. A
  change of the read site in `wddm.c` that bypasses `Bc250HangNodeLastCompleted` is caught only by review.
