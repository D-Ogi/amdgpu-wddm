# Hang recovery, stage 1: soft recovery in ResetEngine (KMD 0.7.194)

Every GPU hang on unit A so far has ended the same way: bugcheck 0x116. The trials behind the reports (147, 151,
153, 208, 245, 251) share one shape. A job on the single WDDM hardware VMID (`BC250_WDDM_VMID`, 1) takes no-retry
GFXHUB faults, its waves latch `MEM_VIOL` and halt, and the end-of-pipe behind them never fires. The 500 ms
submit watchdog closes node 0, so the desktop stops too, and about 10 s later (`TdrDelay` 10 on the lab) dxgkrnl
runs its per-engine TDR. `DxgkDdiResetEngine` refuses, `DxgkDdiResetFromTimeout` fails, and the machine
bugchecks. KMD 194 adds stage 1 of M15.12, behind the registry switch `HangRecoveryMode`: kill the waves, wait
for the fence, and if it retires, report a successful engine reset so that dxgkrnl removes only the guilty
process's device. None of this has run on the lab yet. Whether the kill drains the ring on gfx1013 is an open
question, and only a lab trial or the Linux measurement (wishlist L38) can answer it.

## The switch

`HangRecoveryMode` is a REG_DWORD under the service's `Parameters` key, read once in `WddmStart`. Only the value
1 turns stage 1 on; with the switch on, the start logs `wddm: HangRecoveryMode 1: ...`. Absent or 0 leaves
every TDR DDI as it was in 0.7.193.1. The shim's kill helper is compiled in but never called, and the start logs
nothing new.

## What ResetEngine does with the switch on

Stage 1 handles node 0 (3D/compute) only. Node 1, the paging node, keeps the refusal: dxgkrnl follows a
successful reset of a paging packet with an adapter-wide reset ("TDR changes in Windows 8", step 9), which on
this part is the 0x116 path anyway. For node 0, `Bc250WddmResetEngine` (`wddm.c`) does this:

1. It retires late fences (`WddmGpuFence`). Then, under `Lock`, it reads:
   - whether a node-0 job is still queued;
   - the head job's OS fence, which is the fence a reset would abort;
   - the tail job's ring sequence, the newest on the ring;
   - `LastCompletedFence`.
2. It decides refusals that need no hardware first (`Bc250HangPreKillVerdict`, `hang_recovery.h`):
   - nothing on the ring: verdict 3;
   - an abort fence outside `[last completed, last submitted]`: verdict 4. Reporting such a fence would be
     bugcheck 0x119.
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
     the node-0 gates are reopened (`WatchdogFaulted`, `RefusalPending`, `PreemptionPending`), and our completed
     fence is advanced to the aborted one, as the engine-reset contract asks. `SoftRecoveries` counts it.
   - On 2, 3 or 4, nothing is changed, and the call falls through to the refusal of 0.7.193.1.

The kill helper (`driver/shim/bc250_gfx.c`) transcribes amdgpu's `gfx_v10_0_ring_soft_recovery` (AMD MIT; upstream
removed it in favour of queue resets, which this part does not have). It is one `mmSQ_CMD` write with
`CMD = SQ_IND_CMD_CMD_KILL`, `MODE = SQ_IND_CMD_MODE_BROADCAST`, `CHECK_VMID = 1` and `VM_ID = 1`. The field
masks come from `gc_10_1_0_sh_mask.h` and the enum values from `navi10_enum.h`. The RLC safe-mode bracket is a
no-op on this part (`cg_flags` = 0). `mmSQ_CMD` is the one register this feature adds to the GFX write table; its
address comes from `gen_regs.py` and regcalc. The kill does not write `GRBM_GFX_INDEX`, because the broadcast mode
already addresses every SE, SH and CU. We deviate from upstream in one way: we wait 100 us between kills, where
amdgpu spins. The bound is the same 10 ms.

On success, `GfxSoftRecover` is the one place that clears the sticky `SubmitFailed`, and it does so only after the
fence has retired. It also clears `SubmitInFlight` and ends the DPM busy share. `DpmBusyEnd` is idempotent, so
this is harmless when the interrupt path got there first. StartHealth stays closed: the start did see a fault.
Rendering does not depend on StartHealth, but the custom DPM and CU-mode escapes do. Until the next device
restart they answer `STATUS_DEVICE_NOT_READY`, while the governor inside the KMD keeps running.

## The record

Each stage-1 call leaves its verdict in the registry, under
`<service key>\Parameters\HangRecovery`, a non-volatile subkey. `GuardRecordHangRecovery` (`guard.c`) writes it
and flushes it with `ZwFlushKey`. The log ring is about 1024 lines and lives in memory, so it does not survive the
0x116 that follows a refusal. The registry record does.

- Counters: `Attempts`, `Recovered` (verdicts 1 and 5), `NotDrained` (2) and `Refused` (3 and 4). The driver
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

## Why not in the 500 ms watchdog

Killing at watchdog time would cut the freeze from about 10 s to 0.5 s. It would also turn the hang into an
ordinary `DMA_COMPLETED`: the guilty application would keep its device and read garbage. Only a successful
`DxgkDdiResetEngine` lets dxgkrnl put the guilty device into the error state while the system device carries on.
The watchdog therefore still closes the ring and records its register snapshot, as in 193.

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

- What the trial client exercises is not the hang class of the reports. Its compute dispatch spins on live waves,
  which `KILL` terminates. The waves of the real reports are already halted on `MEM_VIOL`; the opcode is defined
  to terminate them too, but that has not been measured on this part. A MEMVIOL fault cannot be formed safely
  through the validated system D3D12 runtime.
- When the ring holds several jobs from the same root (up to `BC250_GFX_PENDING_MAX`, 7), the drain waits for the
  newest, but the reset names the head as aborted. dxgkrnl may resubmit the later jobs ("Packets unaffected by
  engine reset") although the kill loop may have cut them short. They belong to the process whose device is put
  into the error state.
- A job that ran past the 500 ms watchdog but completed before the TDR leaves nothing on the ring, so it gets
  verdict 3 and today's refusal. The contract would allow reporting the last completed fence as aborted, but our
  view of "last completed" can lag a pending report, and a wrong choice re-executes a packet.
- Reopening the node re-admits work onto hardware that has just faulted. Proof that the ring drained is the
  precondition, and the next job brings its own VM flush. A second hang is still bounded by dxgkrnl's
  `TdrLimitCount`.
