# BC-250 board memory reservation in Windows

Status: implemented and host-tested, awaiting hardware acceptance. The BC-250
provider checks its fixed extended-CMOS route at startup and before each change.
There is no manual probe receipt, special build or registry switch that enables
writes. Passing host tests does not establish that Windows writes or the next
POST succeed on the board.

## App and request boundary

`RUN_BOARD_MEMORY` is command 32, operation ABI 2, with a 128-byte record.
ABI 1 was an unreleased prototype. The DLL and app reject it. READ uses only
`NoAdapterSynchronization` and returns a cached snapshot. It does not access
CMOS, registers, firmware, the registry or a disk. The active value is the complete
GC/NBIO memory range captured at VRAM start, not the smaller application segment.
The snapshot becomes unavailable at stop.

The BC-250 card appears only for a supported provider. Other boards get a
read-only VRAM value from Windows segment statistics. Support alone does
not permit a change: SET and RESTORE require the write capability, an administrator,
HardwareAccess, PASSIVE_LEVEL and a started, awake adapter. Unknown sizes, ABI
versions, operations, flags and nonzero reserved fields are refused. The DLL
checks capabilities before it sends a hardware escape.

The choices are 8192 and 12288 MiB. The app separates active and next-start values,
explains the RAM tradeoff and asks for confirmation through the elevated helper.
An unknown next-start value is not shown as zero or as a pending change. The helper
binds confirmation to the observed block and capabilities. Changed state requires
new confirmation. A successful change takes effect after restart. The app does
not restart Windows automatically. No running VidMm segment is resized.

## Board and backup identity

This is a board capability, not a generic memory setting for AMD GPUs. Provider
zero means unsupported. Provider one identifies the BC-250 ABL block. Selection
requires all of these values:

| Source | Required value |
|---|---|
| PCI vendor and device | `1002:13FE`, ordinary device header |
| PCI subsystem vendor and device | `1022:0000` |
| SMBIOS Type 2 manufacturer and product | `ASRock`, `AMD BC-250` |
| SMBIOS Type 0 vendor | `American Megatrends Inc.` |
| BIOS version and date | P2.00: 11/09/2021, P3.00: 12/09/2021, P5.00: 05/03/2022 |

The PCI and P3.00 strings come from the
[saved unit A inventory](../../evidence/linux/2026-09-21-E01-recon/dmidecode.txt)
and its [PCI report](../../evidence/linux/2026-09-21-E01-recon/lspci-gpu-vvv.txt).
P2.00 and P5.00 are known firmware-image identities. Their inclusion does not
claim a Windows write test on those versions. Modified firmware can retain these
strings. Matching them does not authenticate firmware.

Identity is captured at start and cleared at stop. The PCI read must be complete.
The SMBIOS parser checks lengths, strings, table termination and duplicate records.
Write support also requires SMBIOS 2.6 or later with exactly one Type 1 UUID.
Missing, all-zero and all-FF UUIDs are refused. The protected backup binds the raw
16 UUID bytes to the known BIOS identity and transport identifier. Raw SMBIOS data
and machine identifiers are not logged.

The [PCI callback contract](https://learn.microsoft.com/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkcb_read_device_space)
and WDK 10.0.26100.0 define the configuration read. The
[AuxKlib reference](https://learn.microsoft.com/windows-hardware/drivers/ddi/aux_klib/nf-aux_klib-auxklibgetsystemfirmwaretable)
requires initialization and PASSIVE_LEVEL. The
[enumeration reference](https://learn.microsoft.com/windows-hardware/drivers/ddi/aux_klib/nf-aux_klib-auxklibenumeratesystemfirmwaretables)
identifies RSMB table zero. Both pages are dated 2023-03-13.

## Runtime transport check

The WDK 10.0.26100.0 `ntddk.h` declarations of `HalGetBusDataByOffset` and
`HalSetBusDataByOffset` exempt `Cmos` from the obsolete-API recommendation. The
[HAL page](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-halgetbusdatabyoffset)
says that only `PCIConfiguration` is supported. Neither source defines how this
WDDM caller selects the board's extended bank. The
[legacy VideoPort reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/video/nf-video-videoportgetbusdata)
describes ECMOS as bus 1, but that does not establish the HAL mapping on x64.

The tested HAL forms did not return an ABL block. Offline inspection of the
matching Windows kernel, version 10.0.22621.6199, shows that its exported Cmos
branches ignore BusNumber and Offset. They use SlotNumber on ports 0x70/0x71.
This is a finding about that binary, not a Windows ABI guarantee.

The provider now uses the fixed index/data ports 0x72/0x73 and indices 0x90..0xAB
from the upstream board tool. The transport identifier is 3. Legacy identifiers
1 and 2 are refused for reads and writes. They remain reserved so an old backup
cannot be accepted as a backup of this transport. There is no caller-selected
port or arbitrary index. These platform ports are not GPU BARs.
This route is admitted only after the board and firmware identity checks above.

`BoardMemoryDetect` requires two identical complete reads and matching repeated
byte reads. The signature and checksum must be valid. The size must be 8192 or
12288 MiB. The full block must still match after a separate RTC control.

The RTC control uses the HAL's SlotNumber on bus 0. It reads seconds and registers
A, B and D, checks the valid bit, rejects update-in-progress and SET, and handles
BCD or binary seconds. Two stable samples must advance by one second, including
59 to zero. The loop requests at most twelve 125 ms waits. This bounds
iterations and requested delay, not HAL execution time or scheduler latency.
It does not prove extended-bank selection. The fixed-port transport is separate.

RTC register C is never read. Its interrupt flags have a read side effect, as in
the [Linux RTC handler](https://github.com/torvalds/linux/blob/10dd1a736d557e310a77117832874729a0175d57/drivers/rtc/rtc-cmos.c).
A failed check clears write support. Startup and mutation paths run the check.
The ordinary GUI READ only returns the snapshot. The separate administrator
`bc250kmd_cli bc250-board-memory-probe` remains a read-only diagnostic. Its raw
counts and bytes do not enable writes and require no manual acceptance step.

Reference revisions: WDK 10.0.26100.0, Microsoft driver DDI checkout
`7515063cea4c9e98db6a92986c5b4ddb0463fd16`, public HAL page checked 2026-10-10.

The index/data pair uses the WDK's `READ_PORT_UCHAR` and `WRITE_PORT_UCHAR`.
All production callers hold one driver-wide transaction lock. The normal RTC
HAL route uses another port pair. This lock excludes concurrent transactions in
this driver. It does not establish exclusion of firmware, AML or another driver.
No private HAL lock or reserved ACPI lock interface is called. Port instructions
have no success status. Repeated reads and transaction read-back detect errors.

## Durable backup and transaction

The driver stores `Backup`, an 88-byte `REG_BINARY` record, under
`HKLM\SYSTEM\CurrentControlSet\Control\amdgpu-wddm-board-memory`.
This nonvolatile location is outside the replaceable driver-service key and
survives a driver reinstall. SYSTEM owns the key. Its protected ACL grants SYSTEM
full access and Administrators read access. The driver checks ownership and ACL
on access. The record includes identity, mapping, the original 28 bytes, a pending
marker, checksum and reserved fields. A checksum detects corruption. It is not
a substitute for access control.

The first change saves the original block, flushes the key and reads the record
back. Later changes cannot replace that original. Before any CMOS data write, the driver
sets the pending marker, flushes and verifies it. A pending record at startup,
invalid record, identity mismatch or unconfirmed rollback blocks further changes.
A completed transaction clears pending only after another checked full read and
a flushed, verified record update. A crash can therefore leave a deliberate
lockout even if the bytes happen to be valid.

PROVENANCE: fanoush/bc250_memcfg, MIT, revision `222420fb`, block layout,
signatures and checksum. [Upstream source](https://github.com/fanoush/bc250_memcfg).

The transaction accepts `$ABL`, `APCB` and valid-checksum `CMSB`. It compares the
current full block with the snapshot approved in the app. A request for the current value
performs no writes. It preserves the timing bytes and permits changes only to
signature, checksum and memory-size bytes. It invalidates the signature, checks
read-back, writes the payload and checksum, and commits the signature last.
Full read-back must match. On failure it attempts bounded rollback and distinguishes
a verified restoration from an unknown state. RESTORE uses the exact saved block.
A client cannot supply replacement bytes. The transport also refuses writes to
timing bytes independently of the transaction policy.

The provider's serialization does not prove exclusion of ACPI or
SMM. Sampled stable reads cannot exclude a later firmware change. The multi-byte
transaction is not atomic against power loss, and a valid read-back is not proof
of the next POST. The lab operator must still test 12 GiB, restart, compare active
geometry and Windows RAM, then restore 8 GiB and check again. This hardware
acceptance has not run. A physical CMOS clear is a manual recovery action at the lab. It
restores board defaults and can reset boot order.

## Host checks

The gates compile production sources with mocked hardware or registry boundaries:

- `run_uma.ps1`: block validation, stale state, permitted bytes, commit order,
  rollback, no-op and runtime mutation controls.
- `run_uma_transport.ps1`: fixed ports and indices, byte/block equivalence,
  PASSIVE_LEVEL, timing-write refusal, dropped writes and rollback.
- `run_board_memory_detect.ps1`: legacy HAL refusal, RTC control, checksum,
  unchanged block and negative controls for omitted checks.
- `run_board_memory_service.ps1`: backup ordering, persistent pending state,
  restore and failure lockout.
- `run_board_memory_store.ps1`: registry record, identity, ACL, flush and read-back.
- `run_board_memory_write.ps1`: the joined request, lifecycle, transaction and
  reply paths with injected failures.
- Board identity and qualification tests: foreign or malformed identities,
  duplicate records and missing or sentinel UUIDs.

The DLL and app tests cover strict ABI parsing, capability checks, confirmation
state and unavailable behavior. The KMD build compiles the real handlers. The app
build uses `-NoSmoke`. These host checks do not open the GUI or a GPU. They do not
measure firmware concurrency, persistence through an actual
power failure or successful POST.
