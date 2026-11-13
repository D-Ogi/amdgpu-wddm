# A reading of the CPU objects in the stock ACPI tables of unit A

Date of the reading: 2026-11-13. No hardware was touched. The input is the set of ACPI tables that the recon
session of 2026-09-21 stored in [`../2026-09-21-E01-recon-binaries/acpi/`](../2026-09-21-E01-recon-binaries/acpi/)
(unit A, BIOS P3.00, 6 cores enabled, Linux `/sys/firmware/acpi/tables`).

## Method

1. The tables were disassembled with the ACPICA compiler `iasl -d`, version 20260930.
2. The digests are SHA-256, made with Python `hashlib` over the stored bytes.
3. The positive control for the disassembler is the header of each table. The length in the header is the size
   of the file, for `SSDT1.aml` (1560) and for `DSDT.aml` (24208).

## The files here

| File | Content |
|---|---|
| `SSDT1.dsl` | the full disassembly of `SSDT1.aml` |
| `dsdt-excerpts.txt` | the lines of the disassembly of `DSDT.aml` that this reading uses, with their line numbers |

## What the tables say

### SSDT1: the CPU idle table

| Item | Value |
|---|---|
| Header | signature `SSDT`, revision 1, length 1560, OEM ID `AMD`, OEM table ID `AMD CPU`, OEM revision 1 |
| SHA-256 of the table | `2f0c27555723a8ecda6a968059f66bdf082e63194d31898792ac71cb1f86a4d1` |
| Content | 12 `Scope` blocks of 127 bytes each, and nothing else: 36 + 12 x 127 = 1560 |
| The scope names | `\_PR.C000` to `\_PR.C00B` |
| `_CST` in each scope | 79 bytes, the same in all 12 scopes. SHA-256 `0a4b37466742ce2c7788010c913e9c46268a920dd93624e92c8d8a2582742e3c` |
| `_CST` content | 2 states. C1: a `FFixedHW` register (bit width 2, bit offset 2, address 0), latency 1, power 0. C2: a `SystemIO` register at 0x414 (width 8), latency 0x190, power 0 |
| `_CSD` in each scope | `Package { 6, 0, domain, 0xFE, 2, 0 }`. The domain of scope k is k / 2 (0, 0, 1, 1, ... 5, 5). The integers are not in the shortest AML form: the first two have a byte prefix and the other four have a 32-bit prefix |

No table of the set defines an object `C000` to `C00B`. The disassembler lists the 12 names as `External`.

### DSDT: the processor objects

| Item | Value |
|---|---|
| Header | signature `DSDT`, revision 2, length 24208, OEM ID `ALASKA`, OEM table ID `A M I ` |
| SHA-256 of the stored table | `ab88560016b5313425e9868f34a2b9b900bcf66d9d359de38d4e5fca789a22a6` |
| Processor objects | 16 `Processor` objects in `\_PR`: `P000` to `P00F`, with the processor IDs 1 to 16. Each has the processor block address 0x810 and the block length 6 |
| Content of each processor object | a name `TYPE` and a method `_PDC`. No `_CST`, `_CSD`, `_PSS`, `_PCT`, `_PPC` or `_PSD` |
| `_PDC` | loads a table from the operation region `\_PR.SSDT` when `TBLD` is 0 and `NPSS` is not 0. The stored value of `TBLD` is 4 |
| `\_PR.SSDT` | an operation region at the address 0xFFFF0000 with the length 0xFFFF |
| `\_PR.APSS` | a package of 10 packages. Each of the 60 integers is 0xFFFF |

The firmware writes parts of the DSDT at each start, so the digest of the DSDT is the digest of this capture
only.

### DSDT: three methods without a definition

| Method | Called from | Arguments at the call |
|---|---|---|
| `APTS` | `\_SB.PCI0.NPTS` (one call) | 1 |
| `AWAK` | `\_SB.PCI0.NWAK` (one call) | 1 |
| `AFN7` | three `_BCM` methods of `LCD` devices | 1 |

No table of the set defines `APTS`, `AWAK` or `AFN7`. The disassembler says "Unknown method, guessing 1
arguments" for each.

### APIC and FACP

| Item | Value |
|---|---|
| MADT processors | 16 Local APIC entries with the processor IDs 1 to 16. The IDs 1 to 12 are enabled, with the APIC IDs 0 to 5 and 8 to 13. The IDs 13 to 16 are not enabled, and their APIC ID is 0 |
| FADT | C2 latency 0x65, C3 latency 0x3E9 |

## What this reading does not show

- It does not show what Windows does with these tables. The Windows-side effect is not measured.
- It does not show the tables of a start with 8 cores. The capture has 6 cores.
- It gives no P-state value. The firmware's P-state package holds only the value 0xFFFF.
