# ADR 0021: separate board policy, chip mechanisms and adapter state

Status: Accepted, 2026-10-10.

This decision defines internal boundaries for future AMD GPU support.
It adds no PCI ID and makes no claim of support for a second chip or concurrent adapters.
Acceptance of this architecture does not mean that every boundary or gate is implemented.

## Context

The GPU PCI identity does not identify the surrounding board, fan controller or firmware memory layout.
A chip match alone must not authorize BC-250 board operations.
Likewise, a board name does not identify every register block or its version.

Windows gives each adapter a private context and its resources.
Shared global mappings cannot safely stand for independently active adapters.
An installer check cannot replace ownership checks inside the KMD.

The reviewed source baseline was `e3cbecf6b24fe1154042156168279a6daa2d86a4`.
The UMA provider at `434d2f427a69a527219d197f38686c892ac2784d` supplied the initial separation pattern.
That pattern alone does not qualify a firmware write transport.
This decision supplements [ADR 0006](0006-m3-display-only-miniport-first.md) and [ADR 0008](0008-full-wddm-behind-a-gate-in-stages.md).

## Decision

Keep one driver stack with three internal boundaries and the existing versioned KMD/UMD contracts.

| Boundary | Responsibility | Identity cannot imply |
|---|---|---|
| Board provider | Fan/EC protocol, firmware settings, power budget and permitted operating envelope | GPU register or packet compatibility |
| Chip description | IP versions, registers, firmware, packets, memory geometry and transition mechanisms | Permission to use another board's voltage or fan policy |
| Adapter and session | Ownership of mappings, devices, allocations, callbacks and queues | Separate ownership of a machine-wide controller |

### Board admission precedes hardware access

Select a board provider from checked PCI/subsystem and firmware identity.
Require a known BIOS tuple only where the operation depends on that firmware layout.
Unknown or malformed identity selects no provider for that operation.
Identity fields are compatibility evidence, not cryptographic attestation or caller authorization.

Apply admission before the first provider access, including identity-probe latch writes to the EC.
Then establish transport ownership. Matching SMBIOS text alone does not grant ownership of an LPC port range.
The provider must account for ACPI resources and competing access before using that transport.
A registry switch must not convert an unknown board into an admitted one.

Expose capabilities and per-feature reasons through one query used by the GUI and packaged tools.
Unsupported setters stay hidden. Display only values that the driver can read, with an explicit unavailable state for missing sensors.
Do not display unsupported sensors as zero temperature or zero RPM.

Keep the qualified BC-250 clock, voltage, power and thermal envelope in its provider.
The chip layer retains the SMU protocol and safe ordering of voltage and clock transitions.
Preserve existing values and operation sequences during this separation.
Preserve automatic fan hand-back on stop and partial initialization failure.

An unsupported board must receive no board-specific I/O.
If a startup path requires an unqualified platform operation, refuse it before hardware programming.
This decision does not promise a firmware-only fallback for such a path.

### Chip descriptions follow IP versions

Use immutable chip descriptions with runtime state under each adapter.
Derive topology and memory geometry from authoritative runtime sources where available.
Keep those values separate from templates, firmware versions and enabled CU masks.

Use the existing amdgpu shim and versioned blocks for graphics, SDMA, memory/VM, PSP, SMU and display.
Retain explicit initialization dependencies and per-instance completion state for failure unwind, stop, suspend and resume.
Unsupported reset operations return an explicit unsupported result.
An architecture label such as `gfx10` is not enough to select all registers and firmware.

Initially describe only the existing Cyan Skillfish implementation.
Preserve `SOC15_REG_OFFSET`, original register headers and upstream revision records.
Do not introduce guessed descriptors for untested chips or a general Linux compatibility layer.

### Adapter ownership is explicit

Attach each logical device, process, allocation and queue to its owning adapter.
Keep mutable adapter state under `MiniportDeviceContext`.
Select UMD sessions through the runtime LUID and the checked private contract.
Key standalone winsys caches by adapter identity. Do not substitute the first AMD adapter after explicit selection fails.

Before concurrent adapters are supported, reject a second active adapter inside the KMD.
The rejection must precede shared-state replacement and hardware access.
An installer-only restriction does not satisfy this rule.

Platform controllers, shared package files and Windows TDR policy can have a wider lifetime than one adapter.
Give each shared hardware resource one owner and safe references from its users.
Separate adapter objects must not become competing EC or SMU writers.
Control tools require explicit selection when several instances make a mutation ambiguous.

## Implementation and acceptance

The first complete operation covers fan/EC admission, the operating envelope, shared capabilities and the single-adapter guard.
Memory transport qualification remains a separate requirement.
New chip enablement and concurrent-adapter support remain separate acceptance work.

| Gate | Required result |
|---|---|
| Unknown or malformed board identity | Zero provider-specific accesses, including probe writes |
| Qualified BC-250 | The same permitted values and transport operation sequence as the baseline |
| Provider lifetime | Fan hand-back on normal stop and every partial initialization failure |
| Capabilities | GUI and packaged tools use the same ranges and unsupported reasons |
| Second synthetic adapter | Refusal before shared-state mutation or hardware access |
| Resource selection | No allocation, mapping or callback can switch to another adapter implicitly |
| Existing hardware | Package validation covers fan control, DPM, the GUI and correct rendering |

Host gates exercise production policy with fake transports and record every access.
Mutation checks must detect missing identity or ownership guards.
A passing host gate does not prove compatibility with an untested board.
Another chip requires its documentation, firmware provenance, hardware and positive controls before release admission.

## Alternatives and consequences

Expanding the INF alone would expose unqualified register and platform operations.
Gating only the GUI would leave startup and direct callers unchecked.
A universal abstraction built before a second chip exists would introduce speculative mechanisms.
Keep narrow providers around qualified operations instead.

The boundaries add explicit unsupported states and ownership requirements.
They do not establish support for other GPUs or correct recovery behavior by themselves.

## Contract sources

The review used WDK 10.0.26100.0 and these pinned primary sources.

- [AddDevice context](https://github.com/MicrosoftDocs/windows-driver-docs-ddi/blob/7515063cea4c9e98db6a92986c5b4ddb0463fd16/wdk-ddi-src/content/dispmprt/nc-dispmprt-dxgkddi_add_device.md).
- [StartDevice resources](https://github.com/MicrosoftDocs/windows-driver-docs-ddi/blob/7515063cea4c9e98db6a92986c5b4ddb0463fd16/wdk-ddi-src/content/dispmprt/nc-dispmprt-dxgkddi_start_device.md).
- [IP lifecycle](https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/include/amd_shared.h).
- [IP versions and instances](https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/amdgpu/amdgpu.h).
- [Chip selection](https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c).

PROVENANCE: Linux is GPL-2.0 overall, with MIT AMD driver portions. This document copies no implementation.
