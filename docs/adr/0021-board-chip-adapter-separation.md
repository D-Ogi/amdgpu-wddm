# ADR 0021: Separate board policy, chip mechanisms and adapter ownership

Status: Accepted, 2026-10-10.

## Context

A PCI identifier selects a GPU function. It does not identify the board's fan controller, power budget or firmware memory layout.
Two matching adapters also need separate mappings, queues and lifetimes.
An installer check cannot protect shared kernel state against a second adapter.

The source baseline is `e3cbecf6b24fe1154042156168279a6daa2d86a4`.
The board identity boundary began at `434d2f427a69a527219d197f38686c892ac2784d`.
The complete Windows board memory candidate is `9f5c7d591acc9734d29c424593297dd15dd15afc`.
That candidate includes host checks. It does not establish hardware acceptance of a firmware change.

## Decision

Keep one function driver and the existing KMD/UMD contracts.
Separate these three responsibilities:

| Boundary | Responsibility |
|---|---|
| Board provider | Positive identity, EC access, operating limits, fan control and firmware memory policy. |
| Chip mechanisms | Versioned registers, packets, firmware and IP lifecycle through the existing shim. |
| Adapter ownership | The Windows context that owns mappings, queues, allocations, callbacks and teardown. |

Keep the supported PCI identifiers and INF unchanged.
Do not add chip descriptions without the corresponding hardware and documented contracts.
This change does not establish support for another GPU or concurrent adapters.

## Board admission

Select the provider from PCI and SMBIOS identity before the first board operation.
Reuse the board memory identity checks, including their BIOS allowlist.
An unknown or malformed identity selects no provider.
SMBIOS identity is not cryptographic attestation and does not replace access checks.

Apply admission before EC probing. A read of an EC register also writes the address latch.
Do not use a registry switch to bypass admission.
Do not access BC-250 ports or send BC-250 control messages on an unknown board.

The provider owns the qualified clock, voltage, power and thermal limits.
The shim retains message encoding and the order of clock and voltage transitions.
Preserve the existing values and operation sequences on a qualified BC-250.
Return the fan to automatic mode on every exit that follows acquisition.
Keep the provider available until that hand-back finishes.

Expose limits and feature flags through one cached capability query.
The control app and packaged tools consume that query.
An absent or refused query does not authorize a default set of limits.
Unsupported fan, tuner and firmware memory controls have no setters.
Display read-only values only when the driver supplies valid readings.
The present full WDDM startup requires SMU setup. An unknown board can therefore fail startup.
This refusal does not establish a working display route on that board.

Board identity alone does not grant exclusive ownership of an EC window.
Keep the existing transport ownership and competing-monitor checks from [fan design](../design/fan.md).
Keep thermal and electrical requirements from [hardware limits](../hardware.md).

## Adapter lifetime

The current KMD has shared state, including its VidMm translation state.
Claim a single adapter slot before the first shared initialization in `AddDevice`.
Refuse a second context before it changes shared state or accesses hardware.
Keep the slot across stop and restart of the admitted context.
Release it only after that context's removal and successful hardware teardown.
Retain a refusal state if hardware ownership remains uncertain.

This restriction is stricter than allowing two stopped contexts and only one running adapter.
It prevents a late removal of an old context from destroying a new owner's state.
Concurrent adapters require a separate change that assigns each shared object to its actual owner.
Motherboard EC and SMU resources still require one physical owner.

Standalone RADV cache entries must use adapter identity.
An entry for one LUID must not satisfy a request for another LUID.
Keep the hosted winsys identity and callback ownership contract unchanged.

## Checks

| Check | Required result |
|---|---|
| Unknown or malformed identity | No EC port access or board control message. |
| Qualified BC-250 | The previous values and transport sequence remain unchanged. |
| Two synthetic adapters | The second cannot initialize, overwrite or tear down the first context's state. |
| Partial initialization and stop | Fan hand-back completes before provider removal. |
| Capability consumers | Missing capabilities disable setters without fabricated sensor values. |
| Negative controls | Removing admission or ownership checks makes the host tests fail. |

Retain regressions for voltage ordering, fan hand-back, thermal control and allocation ownership.
Compare the same production operations through fake transports, including each register or message access.
Host checks do not establish operation on an untested board.
Hardware acceptance uses the exact candidate and existing fan, DPM and application checks.

## Sources

The Microsoft DDI reference revision is `7515063cea4c9e98db6a92986c5b4ddb0463fd16`.
The declarations are from WDK `10.0.26100.0`.

- [AddDevice context and PDO contract](https://github.com/MicrosoftDocs/windows-driver-docs-ddi/blob/7515063cea4c9e98db6a92986c5b4ddb0463fd16/wdk-ddi-src/content/dispmprt/nc-dispmprt-dxgkddi_add_device.md).
- [StartDevice resources](https://github.com/MicrosoftDocs/windows-driver-docs-ddi/blob/7515063cea4c9e98db6a92986c5b4ddb0463fd16/wdk-ddi-src/content/dispmprt/nc-dispmprt-dxgkddi_start_device.md).
- [Linux IP lifecycle](https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/include/amd_shared.h).
- [Linux IP descriptions](https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/amdgpu/amdgpu.h).

PROVENANCE: Linux is GPL-2.0 overall, with MIT AMD driver portions. This ADR copies no implementation.

## Alternatives

An expanded INF without these boundaries exposes unqualified hardware operations.
A GUI-only gate leaves startup and direct callers unprotected.
A universal chip abstraction without another measured device would encode assumptions before contracts exist.
Keep the boundary narrow and add implementations only when their contracts and positive controls are available.
