# M9 production table selection and durable admission: source candidate 144

This sub-agent made no lab, registry, deployment, version or shared state changes.
Production sources frozen after WDK compilation; root owns final package/build.

## Reviewed policy and local evidence

Mode2 already selects the full table persistently. Mode1 remains a diagnostic
one-shot: admission requires both its closure write and closure flush to succeed.
M427's actual record did not distinguish WriteDword from ZwFlushKey failure;
0xC000014D alone did not establish the precise failing operation. Mode2 avoids
that diagnostic closure but previously could start GPU work without a durable
budget because GuardCheckAndCountStart ignored errors.

Local sources:
- kernel/using-a-handle-to-a-registry-key-object.md: registry changes are cached;
  closing or rereading a key does not establish disk persistence.
- kernel/surface-team-driver-development-best-practices.md:96-97: do not assume
  complete early-boot registry availability or another driver's load order.
- display/driverentry-of-display-miniport-driver.md: table registration occurs
  through DxgkInitialize in DriverEntry; a reinitialization callback is not a
  supported mechanism to replace a table already registered with dxgkrnl.
- kernel/writing-a-reinitialize-routine.md: callback follows successful entry and
  other drivers' initialization; this does not promise registry-hive durability.
- evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-pagefile-osboot/RESULT.md:
  diagnostic table selection failed at OS boot; later PnP transition succeeded.

The exact wdm/ntddk API pages were absent from the local DDI snapshot, so official
MS pages were checked online:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwflushkey
  Success establishes disk transfer; PASSIVE_LEVEL; a flush can cover a whole hive.
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioregisterbootdriverreinitialization
  Boot-driver callback runs after device enumeration/start, with successful entry
  required. Our INF is demand-start3; this is not an early table-selection fix.
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-ioregisterdriverreinitialization
  No guarantee that an arbitrary later callback makes a hive durably writable.

## Implemented bounded change

- WddmFullTableSelected is a pure getter for the latched DriverEntry choice. PnP
  does not consume the one-shot again or depend on Device.FullWddm (set later).
- GuardCheckAndCountStart(RequireDurable) checks open, count read, count write,
  and flush when the full table was selected. Only missing value permits an
  initial zero count; malformed/unreadable data does not refresh the budget.
- PnP checks that result before acquiring POST display ownership or mapping MMIO.
  Full-table hardware startup cannot proceed on an unconfirmed durable write.
- Existing display-only fallback policy is preserved explicitly. Exhausted
  budget still refuses either table. This is not a new durable guarantee for DDO.
- Mode1 closure logs distinguish write from flush errors. Count logs likewise
  identify open/read/write/flush. Cached increments remain conservative after a
  failed flush; they are not treated as durable admission or rolled back blindly.
- Mode2 is not enabled by code or this task. DriverEntry's GuardStage breadcrumbs
  remain best-effort and can still attempt registry I/O; no claim that all of
  DriverEntry has become read-only.

## Validation

Actual functions extracted: GuardConsumeSetting, GuardCheckAndCountStart,
WddmGateOpen, WddmFullTableSelected, and Bc250StartDevice's prefix through guard
admission. Registry mocks maintain distinct cached/durable values.

- host-positive.log:40 checks,0 failures, including production2 selection without
  closure writes, durable start, missing initial count, per-operation failure,
  exhausted budget, one-shot failure/success, pure getter and DDO recovery.
- host-negative.log: old ignored-error mutation produces5 expected failures out
  of40 checks, each admitting a full start despite an injected registry failure.
- wdk-compile.log: actual guard.c,wddm.c,pnp.c compile with /kernel /W4 /WX, WDK26100.

## Root's next positive control and remaining limits

Use deliberate production2 selection with this strict guard, then one preserved
cold-boot observation: retain separate open/read/write/flush statuses, counters,
selected table and completed GPU startup. OS-boot persistence is still unproved;
StartDevice is PASSIVE but not a documented guarantee of storage availability.
If durable admission is unavailable, refuse before hardware and investigate a
bounded later PnP retry from a working OS. Do not busy-wait in StartDevice, switch
DDI tables from a worker, replace flush by key reread, or admit GPU work and defer
its budget write until afterwards.

Separate positive-boot confirmation gap: current bc250mon auto-confirm requires
stage61, emitted only by PresentDisplayOnly. Full WDDM does not reach that path.
A successful full boot therefore needs an appropriate independently verified
full-table health/presentation confirmation; merely starting the overlay or
reaching StartDevice success must not automatically erase its failure budget.
The existing user-mode Confirm SetValue also does not force a flush, so a power
loss can conservatively retain an old count even after an in-memory reset.
