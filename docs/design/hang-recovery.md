# Hang recovery, stage 1: soft recovery in ResetEngine (KMD 0.7.216.27)

Every GPU hang on unit A so far has ended the same way: bugcheck 0x116. The trials behind the reports (147, 151,
153, 208, 245, 251) share one shape. A job's waves take no-retry GFXHUB faults, latch `MEM_VIOL` and stop, and the
end-of-pipe behind them never fires. The submit watchdog closes node 0, so the desktop stops too. About
10 s later (`TdrDelay` 10 on the lab), dxgkrnl runs its per-engine TDR. `DxgkDdiResetEngine` refuses,
`DxgkDdiResetFromTimeout` fails, and the machine bugchecks. KMD 0.7.216.13 added stage 1 of M15.12, behind the
registry switch `HangRecoveryMode`: kill the waves, wait for the fence, and if it retires, report a successful
engine reset so that dxgkrnl removes only the guilty process's device. Lab trial D of 2026-10-07 never got to the
kill. A fence guard refused it because of a defect in this driver, which 0.7.216.16 removes (see "Lab trial D:
the fence bound is per node" below). Trial D1 of 0.7.216.16 got to the kill, but no kill reached the register.
The kill took the register path of the GART sequence, which 0.7.216.17 corrects (see "Lab trial D1" below).
In trial D1 of 0.7.216.17 the kill drained the ring, and stage 1 passed. The desktop then stayed black, because
DWM spun in the hosted UMD when it destroyed its lost device. The KMD did not cause that. The hosted UMD gets the
change (see "Lab trial D1 of 0.7.216.17" below). With that change (zink b25) the same trial recovered and the desktop
stayed, but DWM still lost its device and made a new one. The cause is a 10 s CPU wait in the hosted ICD, not the
KMD (see "Lab trial D1 of 0.7.216.17 with zink b25" below). From 0.7.216.18 stage 1 is on by default.

BD-114 of 2026-10-10 found a second way into the same bugcheck, and this time the driver's own watchdog started
it. 0.7.216.27 answers it in two places: the watchdog itself ("The private submit watchdog" below) and a verdict
that was a refusal and is now a reported reset (verdict 7, "The record"). The hang class above is unchanged.

## The switch

`HangRecoveryMode` is a REG_DWORD under the service's `Parameters` key, read once in `WddmStart`. From 0.7.216.18
stage 1 is a finished feature and is on by default, as the release-train rule asks (owner, 2026-10-05): absent
means 1, and the INF writes 1 with NOCLOBBER, the value of `tools/release/installer/registry-defaults.json`. The
start logs `wddm: HangRecoveryMode 1: ...`.

0 is the switch-off. The start then logs `wddm: HangRecoveryMode 0: ...`, and every TDR DDI is exactly as it is in
0.7.216.14, the driver of release 0.7.216.100-tester.20: the shim's kill helper is compiled in but never called,
and no register of the new table entry is ever written. Any other value is on, as for `EnableVmidPool`. With
NOCLOBBER a reinstall of the driver package keeps an operator's 0, and so does the release installer: it writes
the default only where the value is absent or still holds what the previous installer wrote (rule `kept` in
`tools/release/installer/common.ps1`). A machine that had tester.20, whose INF did not name the value, gets 1.

From 0.7.216.13 to 0.7.216.17 the switch was an experiment: off by default, and every install wrote 0 without
NOCLOBBER, as for `EnableHangBugcheck`. No release carried those drivers.

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

- Counters: `Attempts`, `Recovered` (verdicts 1, 5 and 7), `NotDrained` (2) and `Refused` (3, 4 and 6). The
  driver never resets them.
- The last call: `LastVerdict`, `LastSeq`, `LastFence`, `LastKills`, `LastMicros`, `LastTime` (a UTC FILETIME,
  REG_QWORD) and `LastVersion`. Since 0.7.216.17, `LastKills` counts only the kills that reached the register. A
  verdict 2 with `LastKills` 0 is a refused kill, and the log ring then has `gfx: soft recovery kill refused`.

A call that kills writes twice: a pending entry (`LastVerdict` 0, `Attempts` + 1) before the first `SQ_CMD` write,
and its verdict before it returns. An outcome decided before any kill writes once - a refusal, or verdict 7 - and
counts its own attempt in that one write, because no pending entry counted one for it. After every call that the
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

## Lab trial D1 of 0.7.216.17: DWM spun when it destroyed its lost device

Trial D1 ran again on 2026-10-07 with KMD 0.7.216.17 (SYS `4E033FB2`). Stage 1 passed. The log ring has
`seq 8756 retired after 1 kill(s) of VMID 12 waves in 0 us` and `SOFT RECOVERED, aborted fence 1143` at 136.473 s.
The client got `DXGI_ERROR_DEVICE_HUNG`, the DWM process stayed the same, and event 4101 was logged with no
bugcheck. After the recovery the desktop stayed black:

- At 136.601 s dxgkrnl set source 0 to not visible (`display visibility: call 9 ... visible 0`). Nothing set it
  visible again.
- The GPU was idle (`dpm busy 0`).
- One DWM thread used 100 % of a core for more than 10 minutes.
- When the thread was stopped (DWM killed), the new DWM set the source visible and the desktop came back.

A full user dump of DWM, read with `cdb -z` and the PDB of the deployed hosted UMD (`bc250d3d_zink.dll` SHA-256
`D9C4DF68`, Mesa `amdgpu-wddm/b19-hosted-umd` at `7eb7861d`), shows the cause:

| Item in the dump | Value |
|---|---|
| Spinning thread | `zink_context_destroy+0x574`, `zink_context.c` line 250, the walk to the end of the screen's free list of batch states |
| Callers | d3d10umd `DestroyDevice`, D3D11 device release, dwmcore `ReleaseSwapChain` / `EnsureSwapChain` / `CheckOcclusionState`, `CComposition::PreRender` |
| `ctx->is_device_lost`, `screen->device_lost` | both true |
| `ctx->bs` | equal to `ctx->last_batch_state`, and `bs->next` points to `bs` |
| Hosted state | `device_lost` and `submission_failed` true. Progress fence 0x33B of 0x33B, Present fence 0x338 of 0x338. No fence at `UINT64_MAX`. |

DWM had declared its hosted device lost. DWM then releases such a device to make a new one. A failed submit marks
the zink batch state lost, and `flush_batch` then starts no new batch, so `ctx->bs` stays on `ctx->batch_states`.
The destroy of upstream zink appends that list to the screen's free list and then appends `ctx->bs` a second time.
That makes `bs->next == bs`, and the walk never ends. Upstream Mesa has the same code (`ref/mesa`).

Three conclusions:

- **The KMD needs no change for this defect.** The GPU and the KMD recovered: every fence of DWM in the dump has
  completed, and a new DWM drew the desktop at once. KMD 0.7.216.17 stays the stage-1 candidate.
- **The UMD change is in zink.** The Mesa branch `amdgpu-wddm/hang-recovery-zink` returns the current batch state
  only if neither list of the context holds it (`zink_bc250_batch_list.h`). Its host test
  (`zink/tests/bc250_batch_list.py`) holds the D1 shape, and the negative control without the guard spins on it.
- **Why DWM declared its device lost was open here.** The next section answers it. The dump shows the result but
  not the path. An innocent device is not put into the error state by an engine reset. Two paths of the hosted UMD can declare the loss without that:
  a runtime callback that returned a device-lost result, or the 10 s CPU bound of the Present-idle wait
  (`Bc250WaitPresentIdle`) while DWM's work waited about 14 s behind the hang (122.35 s to 136.47 s). The same
  branch keeps the first cause in the hosted state (`lost_reason`, `lost_op`, `lost_hr`, `lost_tick`) and names it
  in the error line. The next D1 run tells which path it was. If it is the 10 s bound, that bound must be longer
  than the TDR of the lab, or the wait must poll the device state, because the OS reports a real loss itself.

The packets that wait behind the hung one are a consequence of the contract. The drain runs them on the
hardware, and the scheduler then gives them new fence ids and submits them again. Thus such a packet can run twice.
In D1 these were DWM's packets, and the second run did no harm, but a packet that is not idempotent can give a
wrong frame once.

## Lab trial D1 of 0.7.216.17 with zink b25: the desktop stays, DWM still lost its device

Trials C4, D1 and D2 ran again on 2026-10-07 from 15:26Z, with KMD 0.7.216.17 and the hosted UMD of the branch
above (zink b25, `bc250d3d_zink.dll` SHA-256 `7C4E5E7E`, Mesa `amdgpu-wddm/hang-recovery-zink` at `12133799`).
Evidence: `evidence/windows/2026-10-07-hang-recovery-b25/`.

- **Stage 1 passed again.** The client submitted one dispatch at 15:27:27.428. Its wait ended after 14590.4 ms with
  `DXGI_ERROR_DEVICE_HUNG`. The log ring has `seq 8654 retired after 1 kill(s) of VMID 12 waves in 1011 us` and
  `ResetEngine node 0: SOFT RECOVERED, aborted fence 1123`. The record has verdict 1, 1 kill, 1011 us.
- **The desktop stayed.** DWM kept its process (PID 1876). The KMD log reports source 0 visible 5.3 s after the
  recovery (`display visibility: call 11 ... visible 1`), and DWM used 0.022 of a core over 5 s. D2, a short client
  after D1, completed with `S_OK`.
- **DWM still lost its hosted device and made a new one.** Its diagnostic log has these lines, in this order:

  ```
  async wait event: 0x2
  BC250 hosted op=36 count=1 hr=80004001
  GetDeviceState: 0xC00000BB
  MESA: error: ZINK: vkQueueSubmit failed (VK_ERROR_DEVICE_LOST)
  BC250 hosted device lost: reason=5 (stop) op=4294967295 hr=00000000 tick=99671, SetErrorCb
  ```

The cause is in the hosted ICD of the desktop route (`payload/desktop/amdgpu_wddm_radv.dll`, Mesa
`amdgpu-wddm/radv-wddm2-hosted-main` at `48546c73`), not in the KMD:

1. A queue of the winsys reuses its gather slots in a ring. Before it reuses a slot, `cs_submit` waits on the CPU
   until the job that last used the slot has retired (`vk_wddm2_fence_wait`). DWM's jobs were queued behind the
   hung job, so that wait did not end. The winsys waited once, for 10 s. The wait ended with `VK_TIMEOUT` (the
   `0x2`), and the winsys returned `VK_ERROR_DEVICE_LOST` for the submission.
2. RADV marks the queue lost. On that path it asks for a page-fault report (`radv_queue_handle_fault_state`), and
   the winsys asks the host for `GetDeviceState` (host operation 36). The hosted device has no kernel device of its
   own, so the hosted UMD answers `E_NOTIMPL`, which goes back as `STATUS_NOT_SUPPORTED` (`0xC00000BB`). This is a
   report on the way to the loss, not its cause.
3. zink saw `VK_ERROR_DEVICE_LOST` from `vkQueueSubmit` and stopped the hosted device (reason 5, `stop`). The hosted
   UMD called `pfnSetErrorCb` with `D3DDDIERR_DEVICEREMOVED`. DWM then released the device and made a new one,
   which the b25 destroy change lets it do.

The engine reset came 14.6 s after the hang started (`TdrDelay` 10 and the reset), longer than the 10 s wait. The
contract says what happens to such a job: after a successful `DxgkDdiResetEngine` the scheduler gives the render
packets that waited behind the hung one new fence ids and submits them again, and only the device of the hung
packet goes into the error state ("TDR changes in Windows 8", "Packets unaffected by engine reset" and step 9,
`windows-driver-docs` at `110f60ea`). So DWM's job would have retired. The 10 s bound turned an innocent wait into
a loss.

The change is in the user-mode layers, and the KMD keeps 0.7.216.17's recovery:

- **Hosted ICD** (`amdgpu-wddm/hang-recovery-icd-hosted` at `9ddfcbe2`, on `48546c73`). `vk_wddm2_fence_wait` waits
  in 1 s slices up to 120 s. After each slice it reads the fence. It stops early only when the device is lost: the
  fence reads `UINT64_MAX`, the host reports the loss, or a kernel device (not a hosted one) is not in the
  `D3DKMT_DEVICEEXECUTION_ACTIVE` state (`D3DKMT_DEVICEEXECUTION_STATE` in `d3dkmthk.h` of WDK 10.0.26100 and in
  `windows-driver-docs-ddi` at `7515063`). A wait longer than one slice logs `radv/wddm2: fence F value V pending
  after 1000 ms, device active: waiting on` and then `... completed after N ms of wait`. The winsys host test has
  three new cases (`innocent_wait`, `lost_during_wait`, `wait_bound`). With the old bound (`BC250_TEST_OLD_BOUND=1`:
  one slice, then a loss) `innocent_wait` fails, as the negative control must. The release ICD line gets the same
  commit (`amdgpu-wddm/hang-recovery-icd` at `154050d5`, on `b19-icd` `31844893`), where all 45 cases pass.
- **Hosted UMD** (zink b26, `amdgpu-wddm/hang-recovery-zink` at `ea876500`). The Present-idle wait
  (`Bc250WaitPresentIdle`) had the same single 10 s bound. It now waits in 1 s slices up to 120 s through
  `bc250_slice_wait.h`, and between slices it asks `Bc250HostStatus` for a loss and reads the Present fence. The
  dispatcher no longer logs the `GetDeviceState` answer as an error. Host test `d3d10umd/tests/bc250_slice_wait.py`
  with a negative control that ends the wait after one slice.

The 120 s bound is far above one TDR cycle. A job that is still pending on a live device after 120 s is reported as
lost, as before.

## Why the kill is not in the submit watchdog

Killing at watchdog time would cut the freeze from about 10 s to the watchdog's own budget. It would also turn the
hang into an ordinary `DMA_COMPLETED`: the guilty application would keep its device and read garbage. Only a
successful `DxgkDdiResetEngine` lets dxgkrnl put the guilty device into the error state while the system device
carries on. The watchdog therefore still only closes the ring and records its register snapshot.

## The private submit watchdog

Node 0 has a watchdog of its own, a `KTIMER` and a DPC in `wddm.c`, because this part has no working GPU reset
(facts M53) and a fence that never arrives has to be noticed by somebody. Until 0.7.216.26 it was one constant,
`BC250_WDDM_SUBMIT_TIMEOUT_MS` 500, counted from the moment the packet was written to the ring. BD-114 shows what
that cost. A dense 512-token LLM prefill submits packets whose own execution time is 400 ms or more. The
watchdog fired on one of them. `GfxSubmitFail` closed node 0. The next submission was refused, which latched
`RefusalPending[0]`. That flag blocks the `DXGK_INTERRUPT_DMA_PREEMPTED` acknowledgement for ever. The OS then
waited out its whole `TdrDelay` on an unacknowledged preempt request. `DxgkDdiResetEngine` found nothing of node 0
on the ring, because the accused packet had completed successfully in the meantime, and refused with verdict 3.
`Bc250WddmResetFromTimeout` then returned `STATUS_UNSUCCESSFUL`, which is bugcheck parameter 3 verbatim. Every
step after the first is unconditional, in both captured boots. **A false trip of this watchdog is not a lost
frame. It is a bugcheck.** The analysis is `scratch/bd114/ANALYSIS.md` (local, not in this repository). The
decisions are `driver/kmd/submit_watchdog.h` and the host suite is `test/submit_watchdog_test.c`.

### The budget is a setting, and it is never shorter than the OS's

The OS measures execution time itself and owns recovery: "The GPU scheduler ... detects when the GPU takes more
than the permitted amount of time to execute a particular task ... The preempt operation has a 'wait' timeout,
which is the actual TDR timeout" (`display/timeout-detection-and-recovery.md`), and `TdrDelay` "specifies the
number of seconds that the GPU can delay the preempt request from the GPU scheduler ... 2 seconds is the default
value" (`display/tdr-registry-keys.md`). A private watchdog shorter than that budget replaces a mechanism that
works with one that bugchecks.

`SubmitWatchdogMs` is a REG_DWORD under the service's `Parameters` key, read once in `WddmStart` the way
`HangRecoveryMode` is. The INF does not write it, so the shipping case is the absent value, and the absent value
is `TdrDelay` plus a 2 s margin: 12 s on the lab, where the GUI writes `TdrDelay` 10, and 4 s on a machine that
has no `TdrDelay` at all. `TdrDelay` is read from Windows' own
`HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers` through `GuardReadGraphicsSetting`, which opens that
key for `KEY_READ` alone. An operator's own value is taken as asked, clamped to five minutes, and **raised to
`TdrDelay` if it is shorter** - a value of 500 here would put BD-114 back, so it cannot be set. The start says
which of the three happened:

    wddm: submit watchdog 12000 ms (TdrDelay 10000 ms, setting 0, default), looks for progress every 250 ms

Node 1 (paging) gets the same budget. Its hang class is not BD-114's, but its refusal path is the same one-way
door to a 0x116, and the argument does not care which ring the packet is on. Two bounds keep a constant of their
own and are deliberately NOT the watchdog's budget. `BC250_WDDM_HOLD_DEADLINE_MS` (500 ms) bounds a CPU wait
inside `SubmitCommandVirtual`, on a dxgkrnl worker thread, and letting it grow to a ten-second budget would block
that thread for ten seconds. `BC250_WDDM_STOP_DRAIN_MS` (600 ms) bounds the drain of `WddmStop`, which must not
wait out a long budget. A held submission that runs out of its 500 ms still closes the node, so that is a
remaining instance of the same class, and it is in the "Limits" list below.

### The budget is re-armed while progress is observed

The budget alone is a floor, not an answer. The old watchdog had one input, wall clock, so it could not tell a
healthy job from a dead one at all. The DPC now looks for progress every `min(budget / 4, 250)` ms and re-arms
itself, and what it looks at is a 64-bit token mixed out of the things that move while a node-0 packet is healthy:
the submission fence slot the command processor writes, the ring read pointer `CP_RB0_RPTR`, and the `CP_IB1` and
`CP_IB2` base and size pairs. It fires only when that token has stood still for the whole budget **while the
checks were watching it**: a gap of a budget or more between two checks means nobody was looking, and a token that
nobody watched stand still is not evidence of a hang. That is the rule `Bc250HangWatchCheck` (`progress.h`)
already uses for the heartbeat, and it means a fire needs at least three looks, not two.

Every uncertain reading is read as progress, and that direction is chosen on purpose. The `CP_IB*` family is
banked by `GRBM_GFX_INDEX`, which this driver must not write, so the bank is whatever the shim left selected, and
those registers describe where the command processor is now rather than the head job. Both limits can only make
the token change when the head made no progress, which re-arms the budget - and a watchdog that is too patient
merely lets the OS's own recovery arrive first, which is the mechanism that works.

### The deadline belongs to the job at the head

`job.Deadline` was stamped at the ring write, so a packet behind six others on this seven-deep queue could have
its whole budget consumed before it began - the q27 arm of the two dumps. It is stamped when the job becomes the
head now (`WddmGfxHeadLocked`), and a job that already carries a deadline keeps it, which is the rule that
function has always stated: appending work must not extend a running job's watchdog. What a push behind a running
head may move is the moment the watchdog next looks, because the look is a set cadence and not a fresh budget.
The queue is seven deep, so a deferral costs at most six ticks of detection latency. In exchange the function
keeps the property it had before BD-114 - it leaves the timer armed whenever a head exists - instead of making the
watchdog depend on an unbroken chain of DPC self-re-arms.

**One honest correction to the analysis.** Once the staleness window is armed at the head change, the stamp no
longer changes WHEN the watchdog fires: the window restarts at every head change either way. What the stamp
changes is the deadline a queued job carries, the second (redundant) condition of the fire, and the two measured
numbers below. It is kept for all three, not for a fire it would prevent.

The timer is re-armed inside `wddm->Lock`, and that is not a style choice. `WddmStop` and `WddmSuspendRetained`
set `Stopping`, cancel this timer and take back its queued DPC in one critical section, on the strength of "from
here nothing of ours arms a timer". A re-arm outside the lock could read `Stopping` as FALSE, lose the race to
that critical section and call `KeSetTimer` after the cancel, the `KeRemoveQueueDpc` and the `KeFlushQueuedDpcs` -
on a `KTIMER` and a `KDPC` that live inside the `BC250_WDDM` allocation `WddmStop` then frees.
`run_submit_watchdog.ps1` gates that order with `-RearmOutsideLock`.

### The timeout line says what it measured

The line printed the budget itself, so "after 500 ms" meant "after at least 500 ms, by an unknown amount", and the
offline analysis of two bugchecks could bound a packet's duration but never measure one. It now reports three
measured times, and the `wddm summary` carries the two high-water marks, so a workload can be priced against the
budget without a bugcheck to read it from:

    wddm: timeout measured: head 3250 ms, queued 2400 ms, stale 3000 ms, budget 12000 ms
    wddm summary: submit watchdog 12000 ms (TdrDelay 10000 ms), 4821 checks, 4820 re-armed
    wddm summary: submit watchdog high water: head 412 ms, queue wait 2208 ms

### Verdict 7: the last reported fence named as aborted

The situation BD-114 ends in - nothing of node 0 on the ring, because the accused packet completed between the
watchdog and the TDR - is verdict 3, a refusal, after which `ResetFromTimeout` fails and the machine goes down.
The contract asks for the other answer in exactly this situation (`display/tdr-changes-in-windows-8.md`): "A
special situation can occur when a packet is completed on the GPU between steps 3 and 7. In this case, the driver
should set **LastAbortedFenceId** to the fence ID of the last completed packet if there are no packets in the
hardware queue from the driver's point of view. From the scheduler's point of view, it appears that such a packet
was aborted."

The objection in the "Limits" list of 0.7.216.18 - "our view of 'last completed' can lag a pending report, and a
wrong choice re-executes a packet" - is answered by guards rather than waved away, and every one of them is a
condition the report DPC already tracks (`Bc250HangAbortReportedFence`, `hang_recovery.h`):

- nothing of this node on the ring. Only then is there no packet whose completion we would be pre-empting.
- `CompletionPending[node] == 0`. A pending completion is exactly the lag the objection names: a fence the
  hardware has produced and dxgkrnl has not been told about. With one outstanding, "last reported" is not the
  last completed and the fence named would be wrong.
- `LastReportedValid[node]`, and that fence inside the engine-reset contract's range against the last fence
  dxgkrnl submitted on this node. Out of range is bugcheck 0x119. The lower bound holds by construction, because
  the fence named is the node's own `LastReportedFence`. Only the upper bound can refuse.

The upper bound needed a value this driver did not keep. `SubmittedFence[]` carries the fence of a completion
waiting to be reported, not the newest submission, so `LastSubmittedFence[]` was added and is written in both
submit DDI wrappers - `DxgkDdiSubmitCommand` as well as `DxgkDdiSubmitCommandVirtual` - with the wrap-aware
comparison `bc250_fence_reached` uses. A node-0 context that submitted through the non-virtual DDI would otherwise
leave that value behind the fence the report DPC has already published, and the guard would refuse a report the
contract asks for.

A refusal by any guard leaves verdict 3 and today's behaviour, byte for byte. On a report, the recovered path is
the one verdicts 1 and 5 already take, and it also reopens node 0 in `gfx.c` through `GfxReopenAfterAbort`. That
is not cosmetic: the watchdog's `GfxSubmitFail` left a sticky refusal there, and reopening node 0 in `wddm.c`
alone would admit submissions that `gfx.c` then refuses, which closes the node again on the next packet.
`GfxReopenAfterAbort` does the tail of `GfxSoftRecover`'s drained path and nothing else - there was no kill, so
there is no sequence to stop - and it refuses unless the ring is provably idle.

Expected effect: the guilty device goes into the error state, the application sees a device loss, and the machine
stays up. BD-114 becomes a lost `llama-bench` run instead of a bugcheck. It does not stop the trip, so it is not a
substitute for the budget or the progress window.

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
- A job that ran past the watchdog but completed before the TDR leaves nothing on the ring. Until 0.7.216.26
  that was verdict 3 and a refusal. It is verdict 7 and a reported reset now, under three guards (see "Verdict 7"
  above). The objection that stood here - our view of "last completed" can lag a pending report, and a wrong
  choice re-executes a packet - is what the `CompletionPending[node] == 0` guard answers. A refusal by any guard
  still leaves verdict 3.
- `BC250_WDDM_HOLD_DEADLINE_MS` is still 500 ms, and a held submission that runs out of it still closes node 0
  through `GfxSubmitFail`, which is the first step of the BD-114 chain. It is kept short on purpose, because it
  bounds a CPU wait on a dxgkrnl worker thread. The hold is measured in microseconds in practice (the histogram
  of sessions 313 and 314), so a 500 ms hold is already a hang of something else. It is still the one instance of
  the class the budget change does not remove.
- The progress token cannot name the head job. The `CP_IB*` registers are banked by `GRBM_GFX_INDEX` and describe
  where the command processor is now, so a token that moves while the head stands still re-arms the budget. That
  is the conservative direction - the OS fires first - but it means the watchdog's reaction to a hang behind
  healthy work on the same ring is bounded only by the OS's own TDR.
- Reopening the node re-admits work onto hardware that has just faulted. Proof that the ring drained is the
  precondition, and the next job brings its own VM flush. A second hang is still bounded by dxgkrnl's
  `TdrLimitCount`.
- The watchdog's host suite models `WddmGfxHeadLocked` and the DPC. It does not compile `wddm.c` either.
  `run_submit_watchdog.ps1` therefore reads `wddm.c` by text for the registry reads, the stamp, the re-arm under
  the lock, the measured log line, the aborted-fence call and the two submit wrappers, and it carries eight
  negative controls that must fail: `-FlatFiveHundred`, `-NoTdrFloor`, `-IgnoreProgress`, `-StampAtRingWrite`,
  `-ReportWithPendingCompletion`, `-NoFenceRange`, `-LogTheConstant` and `-RearmOutsideLock`.
- The host suite covers the decisions and the kill loop, never the kill on the hardware. `hang_recovery_test.c`
  holds the gate, both guards, the per-node bound of the fence guard, the pre-kill verdict, the kill loop over a
  model of the register path and the record's arithmetic. `run_hang_recovery.ps1` also reads the backend switch in
  `gfx.c`. It carries six negative controls that must fail: `-IgnoreFenceGuard`, `-IgnoreVmidGuard`,
  `-SharedLastCompleted`, `-GartBackend`, `-CountRefusedKills` and `-NoBackendSwitch`. Whether an `SQ_CMD` write
  that reaches the register drains the ring is measured only on the lab.
- The host test models the read site of `wddm.c` and the report DPC's writes. It does not compile `wddm.c`. A
  change of the read site in `wddm.c` that bypasses `Bc250HangNodeLastCompleted` is caught only by review.
