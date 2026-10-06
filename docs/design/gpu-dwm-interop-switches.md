# GPU DWM interop switches (KMD 0.7.181)

Two registry switches decide whether the desktop compositor's Blt presents run on the GPU:

- `EnableGpuPresentBlit` turns a Blt present into one GPU copy. `DxgkDdiPresent` builds a BGP1 record and
  `SubmitCommandVirtual` executes it.
- `EnableCddDwmInterop` sets `DXGK_DRIVERCAPS.PresentationCaps.DriverSupportsCddDwmInterop`.

Both were diagnostic and default off up to KMD 0.7.180. DWM050 on KMD 170 ran with both set to 1. KMD 181
makes them the default, as point 4 of section 2 of the GPU DWM promotion note asks. An absent value now reads
as 1. An explicit 0 still turns a switch off. The driver also closes both switches by itself after a boot that
died with the path in use, the way DPM falls back to fixed-lab (`docs/design/dpm.md`).

None of this has run on the lab yet.

## What the switches gate

| Switch | Consumer | Effect when 1 |
|---|---|---|
| `EnableGpuPresentBlit` | `Bc250WddmPresent` (wddm.c) | a Blt present calls `WddmBuildGpuPresent` instead of the CPU `BlitGate` path |
| `EnableGpuPresentBlit` | `SubmitCommandVirtual` BGP1 admission (wddm.c) | a BGP1 record is executed; with the switch at 0 it is refused with `STATUS_INVALID_PARAMETER` |
| `EnableGpuPresentBlit` | `wddm_allocation_identity.inc` | `OpenAllocation` binds the opened handle to its `CreateAllocation` object |
| `EnableCddDwmInterop` | `WddmDriverCaps` (wddm.c, DXGKQAITYPE_DRIVERCAPS) | reports the interop cap and `MaxTextureWidthShift`/`MaxTextureHeightShift` 2 |
| `EnableCddDwmInterop` (or `EnableHandleIdentityProbe`) | `WddmObservePresent` (wddm.c) | records the first 16 Blt presents' handles (diagnostic, no GPU work) |
| either | `WddmInteropUse` (wddm.c, new) | counts the DDI device and marks the session (below) |

`WddmBuildGpuPresent` still accepts only the four 8-bit BGRA/RGBA formats (`WddmLinearColorFormat`). A 10-bit or
FP16 Blt source gets `STATUS_GRAPHICS_CANNOTCOLORCONVERT`, even though KMD 173 admits composed A2B10G10R10
surfaces at `CreateAllocation`. This is a known limit for HDR, not a change of this revision.

## Start-latched

Both switches are read once, in `WddmStart`, before the adapter state is published. This did not change:

- dxgkrnl queries DRIVERCAPS at adapter start and keeps the answer. A later change of the cap would not reach
  the compositor.
- The identity binding happens only at `OpenAllocation`. Turning `EnableGpuPresentBlit` on in the middle of a
  start would leave the allocations opened before it unbound, and the Present path would refuse them.

A change takes effect at the next adapter start: a driver restart, a device disable and enable, or a reboot.

## The decision at start

`interop_policy.c` decides; `interop.c` reads the registry and carries the decision out. The policy is plain C
and `driver/kmd/test/interop_policy_test.c` runs it against an independent oracle over every input combination,
plus the life of the switches across simulated boots (quality gate `interop-policy`).

| Input | Result |
|---|---|
| a switch absent | requested (default 1) |
| a switch 1 | requested |
| a switch 0 | not requested |
| a switch holding any other value | both closed for this start, reason 2 (invalid-setting) |
| a switch not a REG_DWORD, or unreadable | both closed for this start, reason 5 (registry) |
| `InteropSession` present, no record of this boot | unclean: see below |
| `InteropSession` present, record of this boot | stale: cleared, nothing closes |

After an unclean boot, if either switch is not explicitly 0, the start:

1. writes `EnableGpuPresentBlit` = 0 and `EnableCddDwmInterop` = 0, flushed;
2. writes `InteropClosedReason` = 4 (unclean);
3. deletes `InteropSession`, but only if steps 1 and 2 reached the disk. Otherwise the marker stays, and the
   next start closes again;
4. runs with both switches closed and logs one line that names the reason:

```
interop: the last boot died with the GPU DWM path in use (marker of boot N, this boot M): EnableGpuPresentBlit and EnableCddDwmInterop closed, reason 4 (unclean), durable status 0x00000000
```

The next start finds both switches at 0. It runs with reason 1 (not-requested) and reports
`InteropClosedReason` 4 with the flag `CLOSED_BY_DRIVER`. The operator opens them again by writing 1 to both, or
by deleting both values, and restarting. The first start that runs with them open deletes `InteropClosedReason`.

The tester release installer reads the same record (BD-069, `tools/release/installer/common.ps1`
`$script:DriverClosures`). A switch at 0 with `InteropClosedReason` present is the driver's own closure, not a
setting of the tester: `install.cmd -Repair` writes 1 to both switches again and deletes the record, while every
other install keeps the closure and names it, with its remedy, in its report. A switch at 0 without the record
stays as the tester set it.

Reason numbers follow `enum bc250_dpm_reason` where the meaning is the same: 0 none, 1 not-requested,
2 invalid-setting, 4 unclean, 5 registry, 7 not-run.

## The session marker

The marker has to say "the machine went down while the path was in use", not "the driver started".

- dxgkrnl does not stop the adapter at shutdown. A marker written at start would therefore outlive every clean
  reboot and close the switches each time.
- The marker is written instead at the first Blt present of a DDI device while either switch is open
  (`WddmInteropUse` in wddm.c, then `InteropUserBegin`). That device is counted.
- When the last counted device is destroyed (`Bc250WddmDestroyDevice`, then `InteropUserEnd`), the marker is
  deleted, and `InteropLastEnd` is set to 2.
- An orderly adapter stop (`Bc250StopDevice`, then `InteropStop`) also deletes it, and sets `InteropLastEnd` to 1.
- A system sleep or shutdown deletes it when it begins, while the registry is still up (KMD 0.7.198, BD-059):
  - the `\Callback\PowerState` notification `PO_CB_SYSTEM_STATE_LOCK` with Argument2 0, sent before the system
    set-power IRP, sets `InteropLastEnd` to 3 (system-power);
  - the adapter's `DxgkDdiSetPowerState` to D1-D3 for a system action (`PowerActionSleep` to
    `PowerActionShutdownOff`) is the second hook and sets 4 (adapter-d3) if the marker is still there, for example
    after a failed delete in the callback.

  The counted devices stay counted. Until the system is back in S0 (Argument2 1, or the adapter's D0) a new device
  marks nothing. On the way back the marker is written again if devices are still counted: after a resume DWM
  presents on without a new first present.
- The volatile subkey `InteropBoot` (value `Marked` = 1) is written before the marker. The configuration manager
  drops it at every reboot. A marker found together with it was left by an earlier start of the same boot, for
  example after a failed stop, and is not taken as a dead machine.

The marker's value is `KUSER_SHARED_DATA.BootId`, which is reported only. The stale/unclean decision uses the
volatile record, not the BootId.

Up to KMD 0.7.197 no power transition touched the marker, on the expectation that DWM's devices are destroyed
before the registry shuts down. The lab refuted it (BD-059): a clean `shutdown /r` from boot 168 left the marker
with no end recorded, and boot 169 closed both switches as unclean. DWM's DDI devices are not destroyed at a
clean restart, so the session now ends at the power transition instead.

What stays detected: a hang, a 0x116 bugcheck or a power cut with the path in use comes before any power
transition, so the marker is still there at the next start, and the switches close. What is given up: a power
loss while the machine is asleep (S3) or hibernated, with DWM alive, now reads as clean; the path is not in use
in S3/S4, and the marker comes back at the resume.

The transitions are `bc250_interop_session_step` in `interop_policy.c`, plain C; the host test checks every
sequence of up to five events, each with the registry write succeeding or failing, against independent rules,
plus nine lifecycles on the simulated registry (a clean restart through either hook, a failed delete retried, a
device that begins during the shutdown, a death with the path in use, sleep and resume).

One planned restart on the lab has to show `previous end system-power` (or `adapter-d3`) and no `unclean`, and one
AC cut with DWM on the path has to show `unclean` and `CLOSED_BY_DRIVER`.

## Registry values

All are REG_DWORD under `Services\bc250kmd\Parameters`.

| Value | Written by | Meaning |
|---|---|---|
| `EnableGpuPresentBlit` | operator; the driver writes 0 after an unclean boot | absent = 1 since 0.7.181 |
| `EnableCddDwmInterop` | operator; the driver writes 0 after an unclean boot | absent = 1 since 0.7.181 |
| `InteropSession` | driver | this boot's BootId while a counted device is alive |
| `InteropClosedReason` | driver | why the driver wrote both switches 0; deleted when they open again |
| `InteropLastState` | driver, every full start | effective bits, plus requested bits shifted left by 8 (bit 1 blit, bit 2 cdd) |
| `InteropLastReason` | driver, every full start | the start's reason |
| `InteropLastEnd` | driver | how the last session ended: 1 device stop, 2 last user gone, 3 system power transition (0.7.198), 4 adapter down for a system action (0.7.198) |

The INF writes none of them. An install therefore keeps whatever the operator set, and a fresh install runs
with both switches on.

## Reading the state

`BC250_ESCAPE_RUN_INTEROP` (25, `BC250_ESCAPE_INTEROP`, ABI 1, 104 bytes) returns the snapshot. It has one
operation, READ, and follows the DPM and CU-mode pattern:

- `NoAdapterSynchronization` = 1 and every other escape flag 0;
- open to every caller;
- no BAR access, under the start-health reader rundown;
- `Reserved` zero in and out.

There is no write operation. The switches are registry state and stay so.

```
bc250kmd_cli interop
driver 0x000700B5, requested blit+cdd, effective blit+cdd, reason none, generation ...
settings EnableGpuPresentBlit=absent(1) EnableCddDwmInterop=absent(1)
session marked, users 1, marks 1, unmarks 0, mark failures 0, last end none, previous end system-power, boot 57, marker found 0
power callback registered
```

The last line comes from 0.7.198 on. `NOT registered` means `\Callback\PowerState` was not available at load and
only the adapter's D3 ends a session at a clean restart; `, system power transition under way (no mark)` means a
sleep or shutdown has begun and the system is not back in S0. Snapshot flags: `POWER_CALLBACK` 2048, `DOWN` 4096.

`log summary` adds one `interop summary:` line. The existing `wddm: CDD interop%u GPU Present gate%u identity
probe%u` line is unchanged, because `interop.ps1` parses it.

## Deployment note

The deployment kit's Configure step writes both switches as explicit 0 (`Set-DurablePresentGates 0`), and its
CPU verification requires explicit 0s. Deploying 0.7.181 through the kit unchanged therefore keeps the CPU
desktop. It also lets one planned restart check the marker's clean path with the path closed, before a GPU trial
opens it. The kit's list of driver-written Parameters has to learn the `Interop*` names before a restore of
captured Parameters can bring back a stale `InteropSession`.

> Nie chwal dnia przed zachodem słońca. - Do not praise the day before sunset: the default is on in the code,
> and on the lab only after the restart above.
