# Hang detector and progress records (KMD 0.7.172)

Twice on 2026-09-29 unit A stopped serving user mode less than a second after a game loaded our user-mode
driver. There was no bugcheck, no 0x133, no TDR and no dump, and the power draw stayed
steady for minutes. The cause is not known. KMD 172 adds three things so that the next occurrence leaves
evidence: a quota on the paging drain, lock-free progress records, and a test-only detector that turns such a
state into a bugcheck with a dump. None of this has run on the lab yet. It fixes nothing by itself and is not
claimed to.

## Paging drain quota

A source review on 2026-09-29 found that `WddmGpuFencePaging` has no bound at DISPATCH_LEVEL. One caller retires at most one
hardware packet per pass, but when another processor publishes the next head between two passes, the loop in
the first caller keeps going for as long as work keeps arriving.

- `PAGING_DRAIN_QUOTA` (`driver/kmd/paging_drain.h`) is 8 retirements per invocation. 8 is well above the one
  packet an invocation retires in the uncontended case, so single-caller behaviour is unchanged.
- At the quota the drain stops with exit reason `Quota` and calls `WddmRequeuePagingDrain`. Under `Lock`, and
  only if the device is not stopping, that arms `PagingDrainTimer` with a relative due time of -1, the earliest
  expiry it can ask for. The timer queues `PagingDrainDpc`, which calls the drain again.
- The requeue is explicit because a quota exit can leave the head queued with nothing in flight. In that state
  no interrupt would ever come to pick it up again.
- A timer is used, not `KeInsertQueueDpc`, because a DPC queued from a DPC can run in the same DPC drain.
- What is implemented is exactly this: an invocation returns after 8 retirements, and one continuation is
  requested by timer. Two questions are left to measurement:
  - whether ordinary threads get processor time between invocations
  - how long the continuation takes under load

  The quota bounds retirements per invocation. It does not bound the time spent in a lock, a nested submission,
  MMIO or logging, and it does not by itself exclude a sustained stream of short DPCs.
- Completion reporting is unchanged: every retirement still calls `WddmQueueReport`.
- `WddmSuspendRetained` and `WddmStop` cancel the timer and remove the DPC, together with the other timers.

## Progress records

`g_Bc250Progress` (`driver/kmd/progress.h`, defined in `hang.c`) is one static, preallocated record of 1856
bytes. It is initialised at compile time, so its signature is present from image load on.

- Header:
  - `Signature` is the qword `0x4752503035324342`, the bytes "BC250PRG".
  - `Version` is 2. Version 1 had no `Input` and no `RecentSeq`; it was never deployed.
  - The header also holds `Size`, `KmdVersion` and `SiteCount`.
- Writers use interlocked operations only: no lock, no allocation, no register access, no log. The recorders are
  therefore safe at any IRQL, including the ISR.
- Each site in `Sites[]` records:
  - `Entries` and `Exits`
  - `LastCpu`
  - `Value`, a site-specific value written at exit
  - `Input`, the call's argument, written at entry before `Entries` counts it (0 for sites without one)
  - `LastEntryTime` and `LastExitTime`, from `KeQueryInterruptTime`, 100 ns units. This clock includes time
    spent asleep.
- A site with `Entries != Exits` was inside that code when the dump was taken. Counters can wrap.
- Fields and `HangOpenSites` (bugcheck parameter 4) are sampled independently. They are not a coherent snapshot
  of one invocation:
  - With a call stalled inside, `Value` still describes an earlier completed call.
  - With concurrent callers, `Input` may belong to a later entry than the stalled one.
  - Neither `Value`, `Input` nor `LastCpu` names the open call without further evidence.
- The sites, in `BC250_PROGRESS_SITE_ID` order:

  | Site | Input at entry | Value at exit |
  |---|---|---|
  | `Isr` | none | message number |
  | `DeviceDpc` | none | none |
  | `IhDpc` | none | passes |
  | `PagingDrain` | none | exit reason |
  | `PagingSubmit` | none | sequence |
  | `GfxSubmit` | none | sequence |
  | `VmFlush` | none | result |
  | `ReportDpc` | none | none |
  | `SubmitWatchdogDpc` | none | none |
  | `PagingWatchdogDpc` | none | none |
  | `PagingDrainDpc` | none | none |
  | `VSyncDpc` | none | none |
  | `BuildPagingBuffer` | operation | operation |
  | `SubmitCommand` | fence | fence |
  | `SubmitCommandVirtual` | fence | fence |

  The software VSync DPC has its own site. The hardware vsync interrupt goes through the ISR and `DeviceDpc`.
- `Drain`:
  - `Iterations` and `Retired` are totals.
  - `QuotaExits` counts quota exits.
  - `LastIterations` and `MaxIterations` describe single invocations.
  - `LastExit` is the last exit reason: 1 stopping, 2 idle, 3 refused, 4 quota.
- `Ih`:
  - `Vectors`, `Requeues` and `Handoffs`
  - `Recent[16]` with `RecentSeq[16]`: the last 16 vectors, best-effort. `RecentNext` counts reserved slots and
    can run ahead of what was written. Each slot's sequence is cleared, the slot written, then the sequence
    (the 1-based vector number) published. Trust a slot only when its `RecentSeq` is nonzero, and order the
    slots by it. A crash mid-write leaves that slot at 0, so the tail can be partial.
  - `BySource[256]`, counts per source id
- `Watch` is the detector's own state:
  - `Armed` and `Active`
  - `LimitSeconds`
  - `Heartbeat` and `HeartbeatTime`
  - `Checks` and `CheckTime`
  - `Fired`

  `HeartbeatTime` and `CheckTime` come from `KeQueryUnbiasedInterruptTime`, which excludes time asleep. The site
  times come from `KeQueryInterruptTime`, which includes it. The two clocks drift apart after every sleep, so a
  decoder must keep them separate and never subtract one from the other.

To find the record in a kernel dump, either use `dt bc250kmd!g_Bc250Progress` with the exact build's PDB, or
search memory for the qword `4752503035324342`. Bugcheck parameter 2 is its address.

## Detector (test only)

The detector is armed from `Services\bc250kmd\Parameters`:

- `EnableHangBugcheck` (REG_DWORD). Only the value 1 arms it. The default is 0, and every install writes 0.
- `HangBugcheckSeconds` (REG_DWORD), 5..60. The default is 10, and any value outside the range also gives 10.

Both values are read at device start (full WDDM only). To arm the detector, set the value, then restart the
device or reboot. The value stays set until an install closes it or it is written back to 0. With 0, no thread,
timer or callback is created.

When armed, the detector has three parts:

- **Heartbeat.** A system thread at priority 8, the base priority of an ordinary process's threads, increments
  `Watch.Heartbeat` every 500 ms. It stands for "an ordinary thread still gets a processor", which is what SSH
  and the desktop lacked.
- **Check.** A periodic 1 s timer DPC compares the heartbeat.
  - If the heartbeat has not moved for `HangBugcheckSeconds` while checks kept running, the DPC calls
    `KeBugCheckEx`.
  - A gap between checks as long as the limit starts a new window instead of counting as stale. This covers
    sleep, a debugger break and similar pauses.
  - Nothing before the call takes a lock, touches MMIO, allocates or logs.
- **Dump pages.** A `KbCallbackAddPages` reason callback adds three ranges to kernel and complete dumps: the
  progress record, the log ring (`GuardLogDumpRegion`) and the log cursor. Both are static data in the image,
  so a kernel dump normally holds them anyway. The callback makes it explicit.

The bugcheck is `0xBC250BAD`, with these parameters:

| Parameter | Meaning |
|---|---|
| 1 | `0x4752503035324342`, the signature |
| 2 | address of `g_Bc250Progress` |
| 3 | heartbeat age in milliseconds |
| 4 | low 32 bits: bitmask of the sites entered and not left (bit = site id). High 32 bits: record version |

### Lifetime

- **Start.** `HangDetectorStart` runs at the end of start, after `Started = TRUE` and before `StageStartDone`.
  It sets up in this order:
  1. the thread
  2. the dump ranges
  3. the callback
  4. the timer
  5. `Active = 1`, last
- **Stop.** `HangDetectorStop` is the first call in `Bc250StopDevice` and in `Bc250RemoveDevice`, and it runs in
  `Bc250Unload` before `GuardCleanup`. It is idempotent. It tears down in this order:
  1. `Active = 0`
  2. cancel the timer, remove the queued DPC, then `KeFlushQueuedDpcs`
  3. signal the thread and wait for it to exit
  4. deregister the callback

  Nothing remains that can run the check once stop returns.
- **Power.** Leaving D0 pauses the check (`Active = 0`). A successful return to D0 resumes it with a fresh window,
  so the heartbeat age does not include the time spent asleep. A failed transition down stays paused.
- **Pause is a flag, not a barrier.** A check DPC that already read `Active = 1` can still reach `KeBugCheckEx`
  after `HangDetectorPause` returns. Only `HangDetectorStop` closes the detector, through its DPC flush. A pause
  that excluded such a trip would need a synchronized disarm; this diagnostic accepts the narrow window.
- dxgkrnl serializes PnP and power calls, so start, stop, pause and resume do not race each other.

### Limits

- **The check DPC must run.** If every processor is held at DISPATCH_LEVEL or above (a spin on a lock that
  every CPU wants, or an interrupt storm on all of them), the timer never fires and the detector cannot fire.
  Only the keyboard crash or an NMI can produce a dump then.
- **It detects thread starvation, not deadlock.** A thread blocked on one of our mutexes leaves the heartbeat
  running. Conversely, a trip is not proof of a GPU deadlock.
- **False positives are possible.** A legitimate long stretch of DISPATCH_LEVEL work on every processor, or
  priority-8 starvation by real-time threads, fires the detector. This is why it is test only and off by default.
- **Dumps.** The added ranges are for kernel and complete memory dumps. A small (mini) dump is not guaranteed to
  contain them. Parameter 2 still gives the address.
- **Boot loops are bounded.** A hang during start-up that the detector turns into a bugcheck counts against
  `UnconfirmedStarts`. At the start budget the driver refuses to start, and Windows uses Basic Display.
- **The recorders are not free.** The ISR and every DPC gain a few interlocked operations each.

## Validation

The quota is tested in the paging queue host test. `experiments/E27-m9-inference/generate-paging-queue-test.py`
extracts the actual code from `wddm.c`:

- the drain
- the requeue
- the watchdog and drain DPCs
- the stop drain
- admission

`driver/kmd/test/run_paging_queue.ps1` builds that code with the real `paging_private.c` parser; gate
`paging-queue` in `tools/quality/quick.ps1` runs it. The earlier ownership and order checks pass unchanged. The
second caller is the actual drain, run again from the report hook between two passes of the first
caller. The new cases are:

- **Success path.** A single caller retires one job and publishes the next, with no yield and no requeue.
- **Quota with a job waiting.** A second caller publishes seven times and then stops. The first caller yields at
  8 with the ninth job queued and nothing in flight. The requeue is armed at -1, and the drain DPC publishes the
  head. All 12 jobs then retire in FIFO order.
- **Sustained interleaving.** The second caller never stops. The invocations retire 8, 8 and 4 of 20 jobs, with
  two requeues, in FIFO order.
- **Boundary.** A stop that lands during the quota's last report still gives a quota exit, but no requeue.

Three mutations of the generated code are negative controls; each fails as expected:

- `--drop-quota`, the 171 drain. Gate `paging-queue-no-quota` runs this one.
- `--drop-requeue`
- `--late-slot-release`

`driver/kmd/test/run_hang_progress.ps1` (gate `hang-progress`) checks the pure parts:

- the quota helper boundary
- the seconds range
- the heartbeat moving, stale and gap cases
- the signature bytes

The host harness is deterministic. It does not show that the detector fires on hardware, what a real dump
contains, or what caused the hangs described above.
