# Hang recovery, stage 1: soft recovery in ResetEngine (KMD 0.7.216.13)

Every GPU hang on unit A so far has ended the same way: bugcheck 0x116. The trials behind the reports (147, 151,
153, 208, 245, 251) share one shape. A job's waves take no-retry GFXHUB faults, latch `MEM_VIOL` and halt, and the
end-of-pipe behind them never fires. The 500 ms submit watchdog closes node 0, so the desktop stops too, and about
10 s later (`TdrDelay` 10 on the lab) dxgkrnl runs its per-engine TDR. `DxgkDdiResetEngine` refuses,
`DxgkDdiResetFromTimeout` fails, and the machine bugchecks. This revision adds stage 1 of M15.12, behind the
registry switch `HangRecoveryMode`: kill the waves, wait for the fence, and if it retires, report a successful
engine reset so that dxgkrnl removes only the guilty process's device. None of this has run on the lab yet.
Whether the kill drains the ring on gfx1013 is an open question, and only a lab trial or the Linux measurement
(wishlist L38) can answer it.

## The switch

`HangRecoveryMode` is a REG_DWORD under the service's `Parameters` key, read once in `WddmStart`. Only the value
1 turns stage 1 on; with the switch on, the start logs `wddm: HangRecoveryMode 1: ...`. Absent or 0 leaves every
TDR DDI exactly as it was in 0.7.216.12: the shim's kill helper is compiled in but never called, no register of
the new table entry is ever written, and the start logs nothing new.

## What ResetEngine does with the switch on

Stage 1 handles node 0 (3D/compute) only. Node 1, the paging node, keeps the refusal: dxgkrnl follows a
successful reset of a paging packet with an adapter-wide reset ("TDR changes in Windows 8", step 9), which on
this part is the 0x116 path anyway. For node 0, `Bc250WddmResetEngine` (`wddm.c`) does this:

1. It retires late fences (`WddmGpuFence`). Then, under `Lock`, it reads:
   - the head of node 0's completion queue, and whether its `Node` is the node the DDI names;
   - that job's OS fence, which is the fence a reset would abort, and its VMID;
   - the tail job's ring sequence, the newest on the ring;
   - `LastCompletedFence`.
2. It decides refusals that need no hardware first (`Bc250HangPreKillVerdict`, `hang_recovery.h`):
   - nothing of this node on the ring: verdict 3;
   - an abort fence outside `[last completed, last submitted]`: verdict 4. Reporting such a fence would be
     bugcheck 0x119 (parameter 0xA);
   - a VMID a broadcast wave kill must not name: verdict 6 (see "The VMID" below).
3. Otherwise it writes the record's pending entry and flushes it (see "The record" below), and only then kills.
4. `GfxSoftRecover` (`gfx.c`) follows the shape of amdgpu's `amdgpu_ring_soft_recovery`, for at most
   `BC250_SOFT_RECOVER_US` (10 ms, amdgpu's value):
   - it reads the fence of the newest sequence;
   - if that sequence has not retired, it issues `bc250_gfx_soft_recover_vmid`, waits 100 us and reads again.

   The ring runs in order, so when the newest sequence retires, every job on the ring has retired and the ring is
   idle. Verdicts: 5 when the sequence retired before the first kill, 1 when it retired after kills, 2 when the
   time ran out.
5. It writes the verdict and flushes it.
   - On 1 or 5, the reset succeeds. `LastAbortedFenceId` is the head job's fence. The queued jobs are dropped,
     the node-0 gates are reopened (`WatchdogFaulted`, `RefusalPending`, `RejectedPending`, `CompletionPending`,
     `CompletionRetries`, `PreemptionPending`), our completed fence is advanced to the aborted one as the
     engine-reset contract asks, and `WddmGfxHeadLocked` closes the queue's head state: it clears `HwPending`,
     cancels the submit watchdog and closes the ring-gap edge the histogram of 0.7.210 counts. `SoftRecoveries`
     counts the call, and the teardown summary prints it.
   - On 2, 3, 4 or 6, nothing is changed, and the call falls through to the refusal of 0.7.216.12, including that
     revision's `StartHealthClose` and the `GfxRetireSignal` wake KMD 196 added.

The kill helper (`driver/shim/bc250_gfx.c`) transcribes amdgpu's `gfx_v10_0_ring_soft_recovery` (AMD MIT; upstream
removed it in favour of queue resets, which this part does not have). It is one `mmSQ_CMD` write with
`CMD = SQ_IND_CMD_CMD_KILL`, `MODE = SQ_IND_CMD_MODE_BROADCAST`, `CHECK_VMID = 1` and `VM_ID` = the hung job's
VMID. The field masks come from `gc_10_1_0_sh_mask.h` and the enum values from `navi10_enum.h`. The RLC safe-mode
bracket is a no-op on this part (`cg_flags` = 0). `mmSQ_CMD` is the one register this feature adds to the GFX write
table; its address comes from `gen_regs.py` and regcalc. The kill does not write `GRBM_GFX_INDEX`, because the
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
GART/system domain and VMID 2 is SDMA paging's, so a broadcast `CHECK_VMID` kill on either would terminate waves
that are not the hung application's, and a recorded 0 means the entry named no VMID at all. Either is verdict 6
and today's refusal, with nothing written to the hardware. `gfx.c` repeats the check before the register write,
because a broadcast wave kill is not something to issue on a VMID the caller could not name.

Only the hung job's VMID is killed, not every VMID on the ring. The GFX ring is in order: a job queued behind the
hung one has not launched its waves, so it needs no kill and runs to completion once the head drains. This is also
why the drain is judged on the newest sequence retiring: it can only retire after every job before it did.

## The record

Each stage-1 call leaves its verdict in the registry, under
`<service key>\Parameters\HangRecovery`, a non-volatile subkey. `GuardRecordHangRecovery` (`guard.c`) writes it
and flushes it with `ZwFlushKey`. The log ring is about 1024 lines and lives in memory, so it does not survive the
0x116 that follows a refusal. The registry record does.

- Counters: `Attempts`, `Recovered` (verdicts 1 and 5), `NotDrained` (2) and `Refused` (3, 4 and 6). The driver
  never resets them.
- The last call: `LastVerdict`, `LastSeq`, `LastFence`, `LastKills`, `LastMicros`, `LastTime` (a UTC FILETIME,
  REG_QWORD) and `LastVersion`.

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
  `LastAbortedFenceId`. The header wins (CLAUDE.md "References"). We therefore advance our own
  `LastCompletedFence` to the fence we report as aborted, which is the intent of that sentence and what the
  preemption notification later needs.
- **"Nothing remains in the physical adapter's hardware queue. The specified nodes are ready to accept new
  packets."** This is why the drain is the precondition of reporting success and why the queue is emptied and
  node 0's gates reopened before the return. A verdict of 2 cannot honour it, so it stays a failure.
- **Packets behind the aborted one are the scheduler's business.** "Render packets: the GPU scheduler assigns
  render packets new fence IDs and then resubmits them." Dropping the queue entries behind the head therefore
  loses no work. Since the VMID pool those later packets may belong to another process, which is a change from
  the 0.7.194 note: the kill does not touch their VMID, and the scheduler resubmits them.

## Why not in the 500 ms watchdog

Killing at watchdog time would cut the freeze from about 10 s to 0.5 s. It would also turn the hang into an
ordinary `DMA_COMPLETED`: the guilty application would keep its device and read garbage. Only a successful
`DxgkDdiResetEngine` lets dxgkrnl put the guilty device into the error state while the system device carries on.
The watchdog therefore still closes the ring and records its register snapshot, unchanged.

## Why there is no stage 2

The stage-2 candidates are a KIQ queue reset (`gfx_v10_0_reset_kgq`, which re-initialises the queue) and a
CP-only `GRBM_SOFT_RESET`. On this part both would be a second bring-up or a reset without a measured-good
sequence. The facts are against that:

- a second bring-up in one start fails at the KIQ (M44);
- amdgpu's own recovery hangs the machine (M53);
- a second init in one boot hangs unit A (M55).

If stage 1 does not drain on gfx1013, the next step is the Linux measurement (wishlist L38), not a blind queue
reset.

## Limits

- What the trial client (`tools/win/hangclient`) exercises is not the hang class of the reports. Its compute
  dispatch spins on live waves, which `KILL` terminates. The waves of the real reports are already halted on
  `MEM_VIOL`; the opcode is defined to terminate them too, but that has not been measured on this part. A MEMVIOL
  fault cannot be formed safely through the validated system D3D12 runtime.
- When the ring holds several jobs (up to `BC250_GFX_PENDING_MAX`, 7), the drain waits for the newest, but the
  reset names the head as aborted. With the VMID pool open, the later jobs may belong to other processes; the
  scheduler resubmits them with new fence ids, and only the head's device goes into the error state.
- A job that ran past the 500 ms watchdog but completed before the TDR leaves nothing on the ring, so it gets
  verdict 3 and today's refusal. The contract would allow reporting the last completed fence as aborted, but our
  view of "last completed" can lag a pending report, and a wrong choice re-executes a packet.
- Reopening the node re-admits work onto hardware that has just faulted. Proof that the ring drained is the
  precondition, and the next job brings its own VM flush. A second hang is still bounded by dxgkrnl's
  `TdrLimitCount`.
- The host suite covers the decisions, never the kill: `hang_recovery_test.c` fixes the gate, both guards, the
  pre-kill verdict and the record's arithmetic, and `run_hang_recovery.ps1` carries two negative controls
  (`-IgnoreFenceGuard`, `-IgnoreVmidGuard`) that must fail. The 10 ms kill loop and the `SQ_CMD` write have no
  host test at all and are measured only on the lab.
