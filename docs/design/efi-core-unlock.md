# The EFI core unlock

The code: [`tools/efi/core-unlock/`](../../tools/efi/core-unlock/). The build and the installation steps:
[the README of the stage](../../tools/efi/stage/README.md). The route it uses:
[the core mask in the tuner page](tuner.md) and the facts M961 and M962.

The core unlock is the first module of the EFI stage, `bc250-stage.efi`. [The stage page](efi-resident-stage.md)
describes the core, the other modules and the load options of all modules. In this page, "the application" is the
stage with this module.

## What it is

The BC-250 is sold with 6 of its 8 CPU cores enabled. The Windows driver enables the other two with queue 3
message `BC250_CPU_MSG_STORE_FF`, and the cores arrive at the next warm restart (M961). A cold boot gives the stock
mask back. So with the driver alone, every cold boot needs one extra Windows restart for 8 cores.

The core unlock sends the same message from the UEFI stage, before Windows starts. It then resets the
board, and the firmware starts with 8 cores. The next start of the application sees 16 logical processors and
starts the Windows Boot Manager. The application changes no firmware setting and writes no NVRAM variable.

## The parts

| File | Job | Tested on the host |
|---|---|---|
| `src/cu_decide.c` | the decision table below, a pure function of four observations | yes, every combination |
| `src/cu_run.c` | the steps of one start, over a table of operations | yes, with recorded operations |
| `src/cu_smu.c` | the message, over the shim's transport and policy | yes, against a model of queue 3 |
| `src/cu_options.c` | the load options of this module, read from the options of the stage | yes |
| `src/cu_msr.c` | the register list of the report and the CPUID check before each read | yes, with a model of CPUID |
| `src/cu_report.c` | the text of the report | yes, with the stored tables of unit A |
| `src/report_uefi.c` | the UEFI binding of the report: MP Services, the register reads, GOP, the report file | no, the lab runs it |
| `src/cu_module.c` | the UEFI binding of the module: the operations and the reset | no, the lab runs it |
| `../lib/acpi/efl_acpi.c` | shared: the FADT reset register and the MADT processor count | yes, with synthetic tables |
| `../lib/acpi/efl_aml.c` | shared: the name scan of a DSDT or SSDT for the report | yes, with the stored tables of unit A |
| `../lib/efl_smbios.c` | shared: the SMBIOS table in the form the driver's identity check reads | yes, with the driver's check |
| `../lib/efl_text.c` | shared: the device paths and the console lines | yes |
| `../stage/src/main.c` | the core of the stage: the board identity and the start of Windows | no, the lab runs it |

The application shares four files with the driver and copies none of them. `driver/shim/bc250_smu.c` is the SMU
transport. `driver/shim/bc250_cpu.c` holds the allowlist, the argument check and the mailbox registers of queue 3.
`driver/kmd/board_identity.c` identifies the board. `driver/shim/include/bc250_mmio_bar.h` names the register BAR.
That last header is new: the index and the length of the BAR were local to `driver/kmd/mmio.c` before.

## The decision table

The application reads four things: the board identity, the logical processor count, the marker file and a key.
The count goes through the driver's own rule, `bc250_cpu_mask_in_force`. So 16 is the full mask and 12 is the
stock mask.

| Board | Count | Marker | Key | Action | Console line |
|---|---|---|---|---|---|
| not a BC-250 | any | not read | not read | start Windows | not a BC-250 |
| BC-250 | 16 | none, or unreadable | not read | start Windows | all 8 cores run |
| BC-250 | 16 | attempt, failed or other | not read | delete the marker, start Windows | the cores run |
| BC-250 | 12 | none | pressed | start Windows | skipped |
| BC-250 | 12 | none | not pressed | write the marker, send the message, reset | unlock |
| BC-250 | 12 | attempt | not read | mark the attempt failed, start Windows | the reset did not keep the mask |
| BC-250 | 12 | failed or other | not read | start Windows | an earlier attempt failed |
| BC-250 | 12 | unreadable | not read | start Windows | the marker cannot be read |
| BC-250 | other or not known | any | not read | start Windows | the count is not 12 or 16 |

The key window opens only on the unlock row, for 1.5 s. Every other start costs no wait. The load option `key=0`
switches the window off. The default is the window, so a person at the board can stop the unlock. A board with
no person at it uses `key=0`: a key that the firmware reports by itself then cannot stop the unlock.

## The loop guard

The guard is a file on the EFI System Partition, in the directory of the application. Its name is
`core-unlock.mark`. The application never calls `SetVariable`, and its UEFI header gives no typed way to call it.

1. Before the message, the application writes the marker with the state "attempt" and reads it back. A marker that
   does not read back stops the unlock: no message and no reset.
2. The application never resets twice in a row. A start that finds "attempt" and 12 logical processors writes
   "failed" and starts Windows.
3. A "failed" marker stops every later attempt. A start with 16 logical processors deletes it, and an operator can
   delete it by hand.
4. When the message fails, or the reset returns, the application deletes the marker. No reset happened, so the
   next start must not count a failed attempt.

## The message

`src/cu_smu.c` sends at most two messages, in the order of the driver. It holds no offset, message number or status
code of its own.

| Step | What | Source |
|---|---|---|
| 1 | The allowlist and the argument check admit both messages, or nothing is sent | `bc250_cpu_message_allowed`, `bc250_cpu_argument_allowed` in `driver/shim/bc250_cpu.c` |
| 2 | The three registers of queue 3 | `bc250_cpu_queue3` in `driver/shim/bc250_cpu.c`, from the generated `driver/shim/generated/smu_registers.h` |
| 3 | The transport for queue 3, with the poll bound and the "already active" flag of the driver | `bc250_smu_init_queue`, called as `driver/kmd/smu.c` calls it |
| 4 | A temperature read of `THM_TCON_CUR_TMP` | the decode of `Temperature` in `driver/kmd/smu.c` |
| 5 | The getter `BC250_CPU_MSG_READ_CPU_MV` with argument 0, which proves that queue 3 answers | `driver/kmd/cpu.c`, the read stage before every setter |
| 6 | The answer must be inside `BC250_CPU_PLAUSIBLE_MIN_MV` to `BC250_CPU_PLAUSIBLE_MAX_MV` | `driver/shim/include/bc250_board_envelope.h` |
| 7 | A stall of `BC250_CPU_MESSAGE_GAP_MS` | the first queue 3 traffic of a driver start |
| 8 | A second temperature read. No reading, or a reading at `BC250_CLOCK_HOT_MC` or above, stops here | the hot gate of `CpuMessageGated` in `driver/kmd/smu.c` |
| 9 | The store `BC250_CPU_MSG_STORE_FF` with the argument `BC250_CPU_SMN_CORE_ENABLE_MASK` | `CpuSendFullCoreMask` in `driver/kmd/cpu.c` |
| 10 | The response must be OK | `smu_cmn_wait_for_response` in `driver/amdgpu-import/smu_mailbox.inc` |

Each message is the AMD send order of `smu_mailbox.inc`. The transport polls the response register until the
earlier command is done. Then it clears the response, writes the argument and writes the command. Then it polls
the response again.

The argument check admits one argument for the store, the named register. It never admits 0, which hangs the
firmware. The register BAR is identified twice, as `driver/kmd/mmio.c` does it: by the BAR in the configuration
header, and by a memory resource of `BC250_BAR5_LENGTH` at that address. The application does not turn on the
memory decode of the GPU function. When the decode is off, it sends nothing.

Boot services have no monotonic clock that every firmware has. The clock of the transport is therefore the sum of
the stalls that it asked for. The transport also bounds each poll by its iteration count, so each poll ends.

## The reset

The method comes from the load options of the boot entry. The default is `reset=warm`.

| Option | Method |
|---|---|
| `reset=warm` | the runtime service `ResetSystem` with `EfiResetWarm` |
| `reset=fadt` | the reset register of the ACPI FADT, written with the FADT reset value |
| `reset=none` | no reset. The message goes out and Windows starts. The marker is not written |

The application reads the FADT at run time. It takes the register from the XSDT, or from the RSDT when the RSDP is
of revision 0. The table checksum must be right, and the flag `RESET_REG_SUP` must be set. The register must be 8
bits wide at bit offset 0, in memory space or in I/O space. A register in PCI configuration space is refused.

## The board identity

The application calls the driver's own `BoardIdentitySelect`. It reads the first 64 bytes of the configuration
header of each PCI function, and it builds the 'RSMB' form of the SMBIOS table that Windows gives the driver. The
SMBIOS 3.0 entry point comes first, and the 2.1 entry point is the fallback. Exactly one PCI function must match.

The identity has no CPU check. The repository holds no CPUID family or model for this part.

## The processor count

The count is the number of enabled processors that the MP Services protocol of the firmware reports. When the
firmware has no MP Services protocol, the count is the number of enabled Local APIC entries in the ACPI MADT. When
the MADT has no Local APIC entry, the count is the number of enabled x2APIC entries. A processor that the MADT
lists in both forms therefore counts once.

## The report

The load option `report=1` adds a read-only report. The default is `report=0`. The report runs before the decision
and does not feed it. It writes one file, `core-unlock-report.txt`, beside the image, on the volume of the marker.
The file replaces the report of an earlier start. When the write fails, the application writes a console line and
goes on. The report also goes to the console.

| Part | What it reads | Source of the layout |
|---|---|---|
| processors | the processor count and each processor of MP Services, and each processor entry of the MADT with its APIC ID and flags | PI Specification 1.8 volume 2, ACPI 6.5 section 5.2.12 |
| ACPI tables | the signature, OEM ID, OEM table ID, revision, length and checksum of each table of the XSDT, and of the DSDT of the FADT | ACPI 6.5 section 5.2 |
| ACPI names | per DSDT and SSDT, the processor names and the thermal names below | ACPI 6.5 chapters 8, 11 and 20 |
| FADT | the flags, `P_LVL2_LAT`, `P_LVL3_LAT`, `RESET_REG` and `RESET_VALUE` | ACPI 6.5 section 5.2.9 |
| registers | on each enabled processor: `HWCR`, `PStateCurLim`, `PStateCtl`, `PStateStat`, `PStateDef[0]` to `PStateDef[7]`, `CStateBaseAddr` | AMD PPR 56176 revision 3.06 |
| graphics | the current mode and every mode of each GOP instance: resolution, pixel format, scan line | UEFI 2.10 section 12.9 |

The processor names are `_CST`, `_CSD`, `_PSS`, `_PCT`, `_PPC`, `_PSD`, `_CPC` and `_LPI`. The report lists each
processor object, each processor device and each scope that defines one of them. The thermal names are `_TZ_`,
`_TMP`, `_CRT`, `_PSV`, `_AC0`, `_FST` and the fan ID `PNP0C0B`. The report gives the count of each.

The ACPI names part is a name scan, not an AML interpretation. It follows the opcodes that open a scope (`Scope`,
`Device`, `Processor`, `ThermalZone`, `PowerResource`) and the two that define a name (`Name`, `Method`). It skips
method bodies and data (buffers, packages, strings, integers), and it executes nothing. A name counts for the scope
whose opening encloses its definition. The report says this in its text. The thermal and fan counts are counts of
the bytes of the name in the table, definitions and references alike.

The register part reads with RDMSR only. The image has no WRMSR instruction. Before each read, `src/cu_msr.c`
checks CPUID: the vendor must be `AuthenticAMD` and the family must be 17h. `PStateCurLim`, `PStateCtl` and
`PStateStat` also need CPUID Fn8000_0007_EDX[HwPstate]. The application processors run the sample through MP
Services `StartupThisAP`, one at a time, and the procedure calls no UEFI service. Without MP Services, the report
reads the bootstrap processor only.

The register numbers and fields come from the PPR of family 17h model 71h. The repository holds no CPUID family or
model of the BC-250, so the report prints the family and model that CPUID gives. These items are open:

- The C-state configuration registers are not in the public PPR, so the report does not read them.
- The PPR is not of the BC-250's own model. A field of the list can mean another thing on the BC-250.

The stored ACPI tables of unit A (`evidence/linux/2026-09-21-E01-recon-binaries/acpi`) are the input of the host
tests. On them the scan finds 16 `Processor` objects in the DSDT, and 12 scopes `\_PR_.C000` to `\_PR_.C00B` with
`_CST` and `_CSD` in one SSDT. That is the state of one capture with 12 logical processors. The tables of a start
with 16 logical processors are not captured.

## What is measured and what is not

Nothing of this application has run on a board.

| Question | State |
|---|---|
| Does queue 3 answer before the operating system starts? | not measured |
| Does the BIOS leave the GPU function's memory decode on? | not measured |
| Which reset keeps the mask: `reset=warm`, `reset=fadt` or neither? | not measured. A Windows restart keeps it (M961) |
| Does the firmware have the MP Services protocol, and what does it count with 6 and with 8 cores? | not measured |
| Does the MADT list 12 or 16 enabled processors in each state? | not measured |
| Can the application write to the EFI System Partition? | not measured |
| How long does a start take with the unlock row, and with the other rows? | not measured |
| With 16 logical processors, do the ACPI tables give the four new processors `_CST`? | not measured. `report=1` reads it |
| Does `StartupThisAP` run the register sample on every enabled processor? | not measured |
| Does the firmware load an unsigned application with Secure Boot on? | not measured. With Secure Boot on, it should refuse the image and go on with its boot order |
| Does a Fast Startup session or a hibernated session resume when the processor count changed since it was saved? | not measured. See the next section |

## Hibernation and Fast Startup

Windows saves its kernel session at a hibernation, and with Fast Startup also at each shutdown. The saved session
holds the processor count of the start that made it. The application runs before the Windows Boot Manager reads
that session, so two cases need a measurement before the installation on a system that has either feature on:

- The session was saved with 16 logical processors and the next start is cold. The application sends the message
  and resets, and the resume then finds 16 processors again. A start on which the application does not act (a
  failed mark, a pressed key, a refused message) resumes that session on 12 processors.
- The session was saved with 12 logical processors, for example at the shutdown directly after the installation,
  and the next start comes up with 16.

Until both cases have evidence, the installation steps tell the user to make one restart after the installation
and not to use hibernation, and the tool's README says so.

## The installation

The installation puts `bc250-stage.efi` in its own directory on the EFI System Partition. It adds one firmware
boot entry for it, first in the boot order, and leaves the Windows Boot Manager entry in place. Explicit consent
covers this one entry and no other NVRAM variable. The removal deletes the entry and the directory. The README of
the tool gives the steps.
