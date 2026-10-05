# Clock ownership before full-WDDM startup

Status: M440 adds the typed KMD request and CLI/monitor clients. Host tests and all builds pass; the owner remains offline until PnP/startup handover is implemented. No runtime ownership transfer.
See [startup requirements](wddm-startup.md), [ADR0014](../adr/0014-clocks-through-the-smu.md)
and factsM22,M243,M427. Production startup remains unaccepted.

## Two independent startup obligations

The M427 one-shot gate failed before the full table was selected, on registry
persistence. GuardConsumeSetting must retain its diagnostic durability rule.
Normal production selection can use the existing persistent mode2, but mode2
alone does not establish safe GPU startup: the current underclock task executes
in user mode and cannot establish ordering before early StartDevice.

The startup path must apply and verify its operating point before GART/PSP/GFX
activation, using a single mailbox owner shared by all control and telemetry
clients. Initial integration targets the already measured1000MHz/820mV, not a
new higher operating point. Settings, temperature and the actual encoded VID
readback belong in the startup report. No capability/admission publication may
precede successful clock preparation. A later CLI operation is not readiness.

## Prepared policy

`driver/shim/bc250_clock.c` calls the unchanged AMD
`cyan_skillfish_od_edit_dpm_table` extracted from the pinned MIT source. Its
settings are per-call state instead of AMD's file-global state. The wrapper
adds the project's1500MHz/900mV ceiling,85C check, and readback of frequency and
encoded VID. M437 first reads the current MHz/VID and confirms any required
voltage increase before the imported frequency-first commit; voltage reduction
follows frequency change. Its command-order controls and limits are in
[M437 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/clock-transition-order/RESULT.md).
It does not claim an exact analogue voltage. No rollback raises
frequency after partial downclock success. Missing transport, excessive
temperature, command failure or readback mismatch leaves readiness false.

`begin/end` must exclude every mailbox client for temperature check, commit
and both readbacks. `message` succeeds only after the actual transport and
firmware success response; a queued request is not success. The shim itself
performs no MMIO, mapping, allocation, locking implementation or async work.
Host tests prove its sequence and decision policy, not hardware transport.

## Prepared mailbox transport (M433)

`driver/shim/bc250_smu.c` uses five unchanged AMD mailbox functions, generated
MP1 offsets, caller-owner validation and callback-based IO. Firmware response
and transport success are both required. Polls have elapsed-time and iteration
bounds. The complete M429 transaction passes 607 model checks and WDK object
compilation; write-order and response-result mutations fail 94 and 23 checks.
See [source and limits](../../evidence/windows/2026-09-24-E27-m9-recovery/startup-smu-transport/RESULT.md).
This supplies the protocol layer only. Fresh/runtime initialization requires
lifecycle proof; the native owner must also enforce the command allow-list.

## Native owner and handover source (M434)

`driver/kmd/smu.c` now provides the kernel push-lock owner, MMIO/time bindings,
fixed clock preparation, paired reads and stop/join. All compile in the normal
WDK build; native source with real host threads passes 23135 checks, and an
unjoined-stop mutation fails four. New bc250rd builds permanently refuse their
old SMU IOCTL and contain no mailbox write path. Deployed binaries are unchanged.
See [evidence and limitations](../../evidence/windows/2026-09-24-E27-m9-recovery/native-smu-owner/RESULT.md).

The selected client route is a typed KMD escape, shared by CLI/overlay and the
same owner used for startup. Raw message forwarding and automatic legacy
fallback are excluded. M440 implements the typed dispatch/client route; PnP owner activation and startup wiring remain to be implemented.
M438 validates the named THM BAR sensor against Linux k10temp before amdgpu;
M439 validates the Windows READ_REG path against existing SMN reads in full
WDDM. These are measured-state sensor controls, not activation of this callback
in the native owner. Client/PnP handover and clock-before-engine ordering still
need hardware validation before native startup is enabled.

## Integration sequence (PnP path completed in M441)

1. Bind the tested M433 protocol to native KMD MMIO, time and temperature
   access, with a lifecycle-proven fresh/runtime choice.
   Preserve the kernel object's lifetime across startup/control calls and stop.
2. Establish exclusive ownership across KMD and the existing bc250rd instrument.
   A KMD-local lock does not serialize bc250rd's separate mailbox mapping/lock.
   The desired owner is KMD; existing tools must use its serialized control
   interface once installed. Legacy direct access must not silently resume when
   the KMD is temporarily unavailable during PnP. Complete this handover before
   running the new backend on hardware; preserve an explicit rollback path.
3. Wire clock preparation into GpuStartupInitialize before hardware activation,
   with report fields and host ordering controls. Preserve the same backend for
   authorized diagnostics and telemetry. Temperature enforcement during later
   workloads remains ADR0014 work; an initial check does not implement it.
4. Validate1000MHz/VID116 against the existing instrument without concurrent
   mailbox access, then one PnP startup/content control. Only after that prepare
   a production-mode OS-boot trial with documented USB recovery and captured
   clock-before-engine ordering. Disabling the old startup task follows proven
   ownership and startup behavior, not source compilation.

Do not add a sleep waiting for a user task, ignore a failed one-shot flush, claim
an initializer-ready flag without hardware readback, or call persistent mode2 a
measured fix before the actual boot and GPU-content controls pass.

## Typed clients and synchronization (M440)

Command19 has a versioned80-byte request with READ and SET operations, no raw
SMU message identifier. SET invokes the complete clock policy, including
thermal/limit checks, voltage-up staging and readback, under one owner lock.
READ returns paired MHz/VID/temperature through the same owner.

The local Microsoft D3DDDI_ESCAPEFLAGS reference and display guide
`threading-and-synchronization-second-level.md` say HardwareAccess requests
Level Two synchronization and idles the GPU. READ instead requires
HardwareAccess0/NoAdapterSynchronization1 and uses the embedded object's lock.
SET requires HardwareAccess1/NoAdapterSynchronization0. Both require the existing
administrator check. The object is initialized before AddDevice publication;
MmioStop closes/joins it before BAR unmap. At M440 no SmuOwnerStart callsite was active; M441 adds gated PnP activation.
This is not yet proof of PnP/power-transition behavior on the lab.

bc250kmd_cli exposes `clock read` and `clock set MHz mV`. Its native control DLL
exports the identical adapter-selection/request path to the C# monitor and the
bc250rd compatibility CLI. Both new clients contain no raw SMU IOCTL fallback.
The monitor polls in-process; it no longer spawns a clock subprocess or sends
independent frequency/voltage commands. The old unforced-voltage stock action
is removed; explicit bounded operating-point selection remains available.

Build the KMD CLI/DLL first; pass its bc250control.dll to the reader and monitor
builds with `-ControlDll`. Deploy these together only after native owner startup
is ready and the old reader writer is retired. The new reader SYS already
refuses the legacy mailbox IOCTL. M441 performs the coordinated handover after confirmed reader unload/refusal. An offline owner returns DEVICE_NOT_READY, not a legacy fallback.

## M441 measured integration and remaining lifecycle work

[Candidate136 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07136-native-smu/RESULT.md)
records clock readiness before GART/PSP/IH/GFX, native typed clients and old writer
retirement. Stop joins the owner before engine teardown. Adapter D1-D3 closes it;
monitor power notifications do not. Only a fresh StartDevice reopens it: this
is not full D0 restoration. Cold-boot ordering and hardware voltage-up/settling
remain unverified; the successful PnP trial began at1000MHz/VID116 already.
