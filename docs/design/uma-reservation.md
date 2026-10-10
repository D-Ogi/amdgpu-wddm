# BC-250 board memory reservation in Windows

Status: partial implementation. On a recognized BC-250 board, the app shows the
active reservation and why changes are unavailable. Other boards get a read-only
VRAM value from the Windows segment statistics. The driver refuses every
set and restore request. Only an explicit administrator diagnostic on a recognized
board can read CMOS through HAL.

## Implemented boundary

`RUN_BOARD_MEMORY` is command 32, operation ABI 2, with a 128-byte record.
ABI 1 was an unreleased prototype. The DLL and app reject it. READ uses only
`NoAdapterSynchronization`. On the BC-250 it returns the complete GC/NBIO memory range
captured during VRAM start, separate from the smaller application segment.
The snapshot is atomic and becomes unavailable at VRAM stop. READ does not
access CMOS, registers, firmware, the registry or a disk.

SET and RESTORE are reserved operations. The driver returns `STATUS_NOT_SUPPORTED`
for an administrator and never calls a hardware transport. Unknown sizes, ABI
versions, flags, operations and nonzero reserved fields are refused. There is
no registry switch that enables writes. The control DLL first reads capabilities.
It sends no hardware escape when write support is absent.

## Board selection

This operation is a board capability. It is not a generic way to resize memory
on AMD GPUs. A provider supplies query, set, restore and the read probe. Provider
zero means unsupported. Provider one is the BC-250 ABL block. A failed identity
query or an unknown board selects no provider. No CMOS probe runs in that case.

Selection requires all of these values:

| Source | Required value |
|---|---|
| PCI vendor and device | `1002:13FE`, ordinary device header |
| PCI subsystem vendor and device | `1022:0000` |
| SMBIOS Type 2 manufacturer and product | `ASRock`, `AMD BC-250` |
| SMBIOS Type 0 vendor | `American Megatrends Inc.` |
| BIOS version and date | P2.00: 11/09/2021, P3.00: 12/09/2021, P5.00: 05/03/2022 |

The PCI and P3.00 board strings come from the
[saved unit A inventory](../../evidence/linux/2026-09-21-E01-recon/dmidecode.txt)
and its [PCI report](../../evidence/linux/2026-09-21-E01-recon/lspci-gpu-vvv.txt).
P2.00 and P5.00 are known firmware-image identities. Their inclusion does not
claim a Windows CMOS test on those versions. Modified firmware can keep the same
version strings. This match identifies the board. It does not authenticate firmware.

The driver captures identity at device start and clears selection at stop. PCI
reads must return the requested byte count. The SMBIOS parser checks lengths,
string indices, table termination and duplicate identity records. Missing or
malformed data selects no provider. The raw table is not logged.

The [PCI callback contract](https://learn.microsoft.com/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkcb_read_device_space)
and WDK 10.0.26100.0 define the configuration read. The local WDK also declares
`AuxKlibGetSystemFirmwareTable`. The missing narrative contract was checked against
the [Microsoft reference](https://learn.microsoft.com/windows-hardware/drivers/ddi/aux_klib/nf-aux_klib-auxklibgetsystemfirmwaretable)
on 2026-10-10: initialize AuxKlib first, query at PASSIVE_LEVEL, and parse the raw
SMBIOS header. The [enumeration reference](https://learn.microsoft.com/windows-hardware/drivers/ddi/aux_klib/nf-aux_klib-auxklibenumeratesystemfirmwaretables)
identifies RSMB table zero. Both pages are dated 2023-03-13.

In ABI 2, the support bit is `0x10`. At byte 80 the query returns the provider ID,
followed by two allowed MiB values and a restart requirement. Unsupported replies
contain none of those capabilities or any board memory data. A supported BC-250
returns choices 8192 and 12288, with restart required. Support for the board does
not imply write permission: the write capability stays clear in this stage.

The app has separate active and next-start values. An unavailable next-start
value is not shown as zero or as a pending change. The initial choices are
8 and 12 GiB. More VRAM leaves less RAM for Windows. On the measured
12 GiB configuration, Windows has about 3.8 GiB RAM. A setting would take
effect after a restart. The app does not restart Windows as part of this action.
Help explains that a physical CMOS clear restores the board default and also
resets the boot order.

## Transport prerequisite

The WDK 10.0.26100.0 `ntddk.h` declarations of `HalGetBusDataByOffset` and
`HalSetBusDataByOffset` exempt `Cmos` from their obsolete-API recommendation.
However, the current [HAL reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-halgetbusdatabyoffset)
says that only `PCIConfiguration` is supported. The
[legacy VideoPort reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/video/nf-video-videoportgetbusdata)
describes ECMOS as bus 1 and SlotNumber as its start location. That reference
does not establish a WDDM transport or its serialization with firmware.

The HAL Cmos interface is selected under the project's WDK-header precedence
rule. Its mapping to this board's extended CMOS remains unmeasured. A private
mutex, raised IRQL or WDDM HardwareAccess flag does not establish exclusion
against HAL, ACPI or SMM. The candidate uses no raw ports. The diagnostic compares
two fixed parameter variants without admitting either as a write route.

Reference revisions: WDK 10.0.26100.0, Microsoft driver DDI checkout
`7515063cea4c9e98db6a92986c5b4ddb0463fd16`, public HAL page checked 2026-10-10.

## Read-only HAL diagnostic

`bc250kmd_cli bc250-board-memory-probe` calls command 32, operation 3, with a separate
128-byte record. It needs a selected BC-250 provider, an administrator,
HardwareAccess alone, PASSIVE_LEVEL and a
started, awake adapter. It performs six `HalGetBusDataByOffset(Cmos, ...)`
calls and returns raw counts and bytes:

| Bus | SlotNumber | Offset | Length | Purpose |
|---:|---:|---:|---:|---|
| 1 | 0x90 | 0 | 28 | Legacy VideoPort slot interpretation |
| 1 | 0 | 0x90 | 28 | Offset interpretation |
| 0 | 0, 2, 4, 0x0D separately | 0 | 1 each | RTC control |

The diagnostic never reads RTC register 0x0C, whose interrupt flags have a read
side effect. The [Linux RTC handler](https://github.com/torvalds/linux/blob/10dd1a736d557e310a77117832874729a0175d57/drivers/rtc/rtc-cmos.c)
uses that read to clear IRQ status. It clears bytes beyond a short return count. An oversized count
is an error. DONE means only that the calls returned. Zero or short counts are
not accepted as a memory profile. The GUI never calls this operation.

The operator must compare a complete block with the Linux control: signature,
checksum, timing bytes and intended UMA size. A valid RTC read alone cannot
establish the extended CMOS mapping. Firmware concurrency still requires a
separate assessment before writes are enabled.

## Block policy prepared for a future transport

PROVENANCE: fanoush/bc250_memcfg, MIT, revision `222420fb`, block layout,
signatures and checksum. [Upstream source](https://github.com/fanoush/bc250_memcfg).

The portable `bc250_uma` policy works on a 28-byte block through injected
callbacks. Host tests use an array. No production hardware caller exists.

- Accept `$ABL`, `APCB` and valid-checksum `CMSB`. Reject unknown and error
  signatures, bad checksums and an invalid existing size.
- Existing sizes must be 16 MiB aligned, at least 256 MiB and below 14336 MiB.
  Targets are limited to 8192 and 12288 MiB.
- Read the full block twice. Require equal reads and the caller's expected
  snapshot. A request for the current value performs no writes.
- Preserve timing bytes. Change only UMA, checksum and signature. Invalidate
  the signature before the update and publish its commit byte last.
- Check the full block. On failure, attempt bounded rollback of only those
  fields. Distinguish a checked restoration from an unknown state. Do not
  publish a valid signature after a failed rollback write.

The caller must own the transport and keep a durable backup before calling the
policy. Stable reads do not provide exclusion from firmware. The multi-byte
update is not atomic against power loss.

## Work required before writes can be enabled

1. Establish the supported transport, address mapping and serialization.
2. Check the board selection and the exact firmware revision on the target.
   The identity checks are implemented. A matching tuple does not qualify writes.
3. Add a flushed, checked backup owned by the driver and a bounded restore
   operation. Do not accept arbitrary replacement bytes from a client.
4. Apply effective caller authorization, device lifetime exclusion, stale-state
   checks and a persistent refusal after an unconfirmed rollback.
5. Check the exact Windows artifact: read, set 12 GiB, read back, restart and
   compare active geometry and Windows RAM. Then restore 8 GiB and check again.
   The lab operator owns this bounded validation. It has not run.

## Host checks

`driver/kmd/test/run_uma.ps1` compiles the real portable policy and checks valid
states, stale and unstable observations, allowed bytes, no-op requests and
injected failures. Compiled checksum, stale-state and no-op mutants must fail
runtime checks. These tests do not check platform arbitration or POST.

The native control DLL test suite and the app's pure tests cover the ABI and
the unavailable state. The board tests cover foreign device, subsystem, board and
BIOS identities, malformed records and capture failures. Mocked HAL calls must
remain zero for unsupported identities. The KMD build compiles the actual handlers.
The app build uses `-NoSmoke`. Host checks do not open the GUI or a GPU.
