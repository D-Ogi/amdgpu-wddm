# Clock ownership before full-WDDM startup

Status: M429 source preparation; no runtime ownership transfer or deployment.
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
encoded VID. It does not claim an exact analogue voltage. No rollback raises
frequency after partial downclock success. Missing transport, excessive
temperature, command failure or readback mismatch leaves readiness false.

`begin/end` must exclude every mailbox client for temperature check, commit
and both readbacks. `message` succeeds only after the actual transport and
firmware success response; a queued request is not success. The shim itself
performs no MMIO, mapping, allocation, locking implementation or async work.
Host tests prove its sequence and decision policy, not hardware transport.

## Runtime integration still required

1. Implement the native KMD mailbox backend from the pinned AMD SMU protocol,
   with named imported MP1 registers, bounded waits and response handling.
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
