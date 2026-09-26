# M440 - Typed SMU client path, still offline on the lab

2026-09-24, source/host only. No lab call, package installation or native mailbox
activation. Unit A remains on M439135. DEV135 SYS SHA256
`DEFFB5281DF207748E0F190E40C4E7432E63F347BBB14E41444DAE6B46CE3CFB`
was built and signed but NEVER installed. Other artifacts are identified in
smu-typed-build-identities.json. Dirty worktree over bed764da5192be7132d646be0e6331c1edeb30fd;
exact source bytes are retained, including the generic public PCI identifiers.

## Implementation

- Versioned80-byte command19 with READ and SET operations, no raw SMU opcode.
  READ returns MHz/VID/temperature from one owner transaction; SET invokes the
  complete M437 policy with limits, temperature, direction-aware voltage staging
  and readback. Outputs clear on refusal; operation NTSTATUS is explicit.
- READ requires HardwareAccess0/NoAdapterSynchronization1; SET requires
  HardwareAccess1/NoAdapterSynchronization0. Both use the existing admin check.
  This avoids requesting GPU-wide idling for periodic reads. Performance and
  PnP/power behavior remain unmeasured. Local MS D3DDDI_ESCAPEFLAGS and the
  second-level threading guide establish that HardwareAccess idles the GPU;
  NoAdapterSynchronization's no-wake usage is shown in signaling-cpu-event-from-kmd.md.
- Owner storage is embedded in BC250_DEVICE, initialized before publication in
  AddDevice; MmioStop closes/joins it before BAR unmap. No SmuOwnerStart callsite
  is enabled, so new requests remain offline until the actual handover.
- bc250kmd_cli offers clock read/set. The same adapter lookup/request code is
  exported in bc250control.dll for direct monitor P/Invoke and the reader CLI.
  New monitor uses paired telemetry and atomic SET; the old unforced-voltage
  stock action is removed. Explicit bounded clock.set remains.
- Reader CLI clock/clock-check use the DLL; raw smu passthrough is removed.
  New reader SYS already refuses the legacy mailbox IOCTL (M434). No client
  fallback exists. Build client/DLL first, then reader/monitor with -ControlDll;
  package the DLL beside each executable. Do not deploy them independently.

## Validation

- Actual smu.c with mocked kernel primitives and real concurrent host threads:
  30675 checks,0 failures. Four threads each run50 SET/READ pairs. Existing
  stop joins a deliberately held transaction before making the mapping offline.
  Typed1000/820 and1500/900 model transitions preserve readbacks and staging;
  the latter is host input, not an authorized new lab operating point.
- Removing the SET synchronization requirement yields2 failures. The negative
  source/log are preserved; the production source retains the requirement.
- Native header layout and the compiled C# structure agree:80 bytes and18
  field offsets. Built DLL export/invalid-argument checks occur before adapter
  lookup: no development-PC hardware call, overlay or resident process started.
- Full WDK, CLI/DLL, reader SYS/CLI and monitor builds pass, including existing
  stage-table checks. First compile failures are preserved: command macro/type
  naming collision, host NT_SUCCESS/constant-expression fixture issues and
  explicit bitfield-to-BOOLEAN conversion. None was a hardware experiment.
  The first managed-layout observer selected Marshal.SizeOf(object) on a Type;
  using an instance corrected the observer. Final layout comparison is retained.

## Remaining integration

Activate the owner only after the deployed legacy writer is removed, wire fixed
startup1000MHz/820mV before GART/PSP/GFX, report its readiness and close it across
PnP/power teardown. Replace clients/startup task as one reviewed handover, then
validate sensor/clock ordering, one PnP and GPU content before an OS-boot test.
This source path does not prove live exclusivity, startup readiness or clock
settling. BD-005/024/027 remain in progress; full M9 is not achieved.

Reference snapshots: MS DDI7515063cea4c9e98db6a92986c5b4ddb0463fd16,
conceptual docs110f60eaf2ac5836e644d320c1e92c1011f2af5e. All archived material is
project source/build output; no firmware or private unit data is included.
