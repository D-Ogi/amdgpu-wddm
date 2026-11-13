# The EFI stage

The code: [`tools/efi/`](../../tools/efi/). The build and the installation steps:
[the README of the stage](../../tools/efi/stage/README.md). The first module has its own page:
[the EFI core unlock](efi-core-unlock.md).

## What it is

`bc250-stage.efi` is one UEFI application for the BC-250. The firmware starts it before Windows. It does three
jobs in sequence, and then it starts the Windows Boot Manager.

1. The core unlock enables the two CPU cores that the board is sold without.
2. The ACPI module installs corrected ACPI tables in RAM.
3. The Secure Boot module verifies the images that the stage starts.

The stage writes no NVRAM variable, no CMOS byte and no flash. Its UEFI header has no typed `SetVariable`. Every
change that it makes is in RAM and is gone at the next restart.

## The sources

Each contract in this page comes from one of these sources. The revision is the one that was read.

| Source | Revision | Used for |
|---|---|---|
| edk2, `MdePkg/Include` (`Uefi/UefiSpec.h`, `Guid/GlobalVariable.h`, `Guid/ImageAuthentication.h`, `Protocol/AcpiTable.h`, `Protocol/AcpiSystemDescriptionTable.h`, `Protocol/Security2.h`) | commit `8628578dacd38b327d4f9acf820c6914f4b3a48f` | the interfaces and GUIDs of the UEFI Specification 2.10 and of the PI Specification, as the headers state them |
| edk2, `MdeModulePkg/Core/Dxe/Image/Image.c` | the same commit | when the firmware core calls the Security2 protocol, and what each status does |
| edk2, `MdeModulePkg/Universal/SecurityStubDxe/SecurityStub.c` | the same commit | the Security2 call without a file buffer |
| edk2, `SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.c` | the same commit | the sequence of the db and dbx checks, and the sequence of the Authenticode digest |
| ACPI Specification | 6.5 | the table header, the AML encodings, `_CST` and `_CSD` |
| Microsoft, "Windows Authenticode Portable Executable Signature Format" | version 1.0, 2008 | the Authenticode digest and the signature structure |
| Microsoft, "PE Format" | learn.microsoft.com, read 2026-11-13 | the PE headers and the attribute certificate table |
| Microsoft, "Secure boot" (OEM guidance), `windows-hardware/manufacture/desktop/secure-boot-landing` | `ms.date` 2026-03-10, repository commit `3abfddf4` | what Windows requires of `SecureBoot` and `SetupMode` |
| Microsoft, "The TESTSIGNING boot configuration option", `windows-hardware/drivers/install/the-testsigning-boot-configuration-option` | `ms.date` 2025-04-16, repository commit `16aec560` | the test-signing mode and Secure Boot |
| Microsoft, "Virtualization-based Security (VBS)", `windows-hardware/design/device-experiences/oem-vbs` | `ms.date` 2023-03-17, repository commit `d4e5a03b` | what VBS and memory integrity require of the platform |
| Microsoft, "Implementing processor power management for Windows 7" (archived) | `ms.date` 2017-06-01, repository commit `d950d3ea` | what Windows requires of processor objects, `_CST`, `_CSD`, `_PSS`, `_PCT` and `_PPC` |
| RFC 5280, RFC 2315, RFC 8017, FIPS 180-4, ITU-T X.690 | the published texts | certificates, SignedData, RSA signature verification, SHA-256, DER |

The site of the UEFI Forum refused the download of the specification text on 2026-11-13 (HTTP 403). So this page
names the UEFI and PI interfaces as the edk2 headers state them, and it gives no section number of those two
specifications. The edk2 code is a reference only. The stage copies no line of it.

## The parts

The stage has a core, three modules and a shared library. Each directory is one part.

| Directory | Part | Job |
|---|---|---|
| `stage/` | core | the console, the load options, the volume of the stage, the board identity, the start of Windows |
| `core-unlock/` | module `core-unlock` | the two CPU cores, with the warm reset |
| `acpi-repair/` | module `acpi` | the corrected ACPI tables |
| `secure-boot/` | module `secure-boot` | the image check |
| `lib/` | shared library | the UEFI header, the small C library, text and device paths, SMBIOS, SHA-256 |
| `lib/acpi/` | shared ACPI table library | the root table walk, the AML name scan, the AML writer |

### The interface between the core and a module

`stage/src/stage.h` is the whole interface. A module is one `struct stage_module`: a name, an off switch and five
functions.

| Function | When the core calls it |
|---|---|
| `run` | once, before Windows starts. This is the work of the module |
| `probe` | in place of `run`, when the module is off and `probe=1` is set. It says which protocols the firmware has for the module, and it changes nothing |
| `gate` | once, with the bytes of the next image. The answer is pass, bind or deny |
| `loaded` | after the firmware loaded that image, with the status |
| `unwind` | when the image did not start or returned |

A module calls the core for a console line, the board identity, the ACPI root tables, a file beside the stage and
pool memory. A module that restarts the board first calls the core to write the log. A module never calls another module, and it never includes a header of another module. The shared
library holds only code that has no job of its own. The core holds no job of a module.

Each module keeps its decisions in pure C files that need no firmware. One file per module is the UEFI binding.
The host tests build the pure files and run them against models and stored tables.

## The sequence of one start

1. The core reads the load options and opens the volume of the stage.
2. The core unlock runs. When it sends the unlock message, it resets the board and does not return.
3. The ACPI module runs.
4. The Secure Boot module runs: it reads its databases and puts its check into the firmware.
5. The core reads the next image and asks the gate of each module.
6. The core loads and starts the next image.

The next image is the Windows Boot Manager. The load option `next=` names another image on the same volume. The
lab uses it to start the boot loader of the stick, so that Linux can read the result of the stage.

The core unlock is first for two reasons. A reset discards every table that the ACPI module installed, so work
before the reset is lost. The ACPI module must also see the processor count that Windows gets. After the reset,
the firmware starts the stage again, the core unlock finds all cores, and the other two modules run.

## What stays in memory after ExitBootServices

| Item | Memory type | Owner | Stays |
|---|---|---|---|
| The stage image, its data and its pool memory | loader code and loader data | the stage | no. The operating system takes this memory back |
| The Security2 protocol of the stage | a structure in the stage image, on the handle of the firmware | the stage | no. Boot services end, and nothing calls the protocol later |
| The installed ACPI tables | ACPI memory of the firmware | the firmware. `InstallAcpiTable` copies the table | yes, as data. The operating system reads them |

So no start leaves code of this project in memory. The only resident data are the ACPI tables, and the firmware
owns them.

## The load options

An option is `name=value`. An option that no module understands is counted on the console and changes nothing.

| Option | Default | Result |
|---|---|---|
| `unlock=0` | 1 | the core unlock module does not run |
| `acpi=0` | 1 | the ACPI module does not run |
| `sb=0` | 1 | the Secure Boot module does not run |
| `next=<path>` | `\EFI\Microsoft\Boot\bootmgfw.efi` | the image that the stage starts after its work, on the volume of the stage |
| `reset=warm`, `reset=fadt`, `reset=none` | `warm` | the reset of the core unlock |
| `report=1` | 0 | the read-only report of the core unlock |
| `acpi.idle=0` | 1 | no idle state table |
| `acpi.stubs=0` | 1 | no method stub table |
| `sb.mode=audit`, `sb.mode=enforce` | `audit` | report each verdict, or refuse an image that is not allowed |
| `acpi.apply=0` | 1 | the ACPI module builds and checks its tables, and installs and removes none |
| `probe=1` | 0 | a module that is off says which protocols the firmware has for it |
| `log=0`, `log=1`, `log=line` | `1` | the log file: none, written before the next image starts, or written after each line |

### The log file

The console of the stage is on the screen of the board only. So the core writes the console lines to
`bc250-stage.log` beside the stage image. It uses the file protocol of the volume, as the core unlock does for
its marker. The new lines go behind the lines of earlier starts. A file above 256 KiB starts again.

The core keeps in memory only the lines that are not yet in the file, at most 16 KiB. A line that finds this
memory full causes a write first. So the log of one start has no bound. The first build of the log kept the
whole start in that memory, and the first lab run with `report=1` lost each line after 16 KiB.

The text of the read-only report of the core unlock goes to the console and to its own file,
`core-unlock-report.txt`. The log gets one line that names that file.

- `log=1` writes the lines before the next image starts, before a module restarts the board, and before the
  stage returns. It also writes when the memory for the lines is full.
- `log=line` writes after each line. Use it for a first start on a firmware: if the firmware stops in a call, the
  file ends at the line before that call.
- `log=0` writes no file.

If the volume does not take the file, the console says so one time, and the start continues.

## The failure policy

Every module fails open: when something is wrong, the module prints the reason, does nothing more, and Windows
starts as it does without the stage.

- A board that is not a BC-250: the core unlock and the ACPI module do nothing and say so.
- A table, a file or a protocol that is not there or is not sound: the module says which one, and stops.
- A built table that fails its own check: it is not installed.
- An ACPI table set that the module cannot read whole: the module installs nothing and says so.

`sb.mode=enforce` does not fail open, and the operator asks for it. An image that is not allowed does not start.
A defect in the inputs of the mode also refuses the image, with the reason on the console. The mode does not
become `audit`, because a start after a silent change of the mode looks like a verified start. The stage then
returns `EFI_ACCESS_DENIED`, and the firmware goes on with its boot order.

One more case does not return to the firmware. When the next image did not start, the stage puts the Security2
protocol of the firmware back. When the firmware refuses that, the stage prints the reason, waits 10 seconds and
restarts the board. A return would leave a protocol that points into freed memory.

## The ACPI module

### The defects

The stock firmware has two defects in its ACPI tables. Both are facts.

- The CPU idle table targets objects that do not exist (M965). Its 12 scopes are `\_PR.C000` to `\_PR.C00B`. The
  DSDT declares the processors as `\_PR.P000` to `\_PR.P00F` (M966). So the table does not load, 12 lookups fail
  (M47), and no processor has `_CST`.
- The DSDT calls the methods `APTS`, `AWAK` and `AFN7`, and no table defines them (M966). Each call aborts its
  caller with `AE_NOT_FOUND`. The callers are the sleep and wake paths and three brightness methods.

The firmware has no P-state objects, and this module adds none. See "What the module leaves out".

### What the module installs

The module installs at most two tables through `EFI_ACPI_TABLE_PROTOCOL`. The firmware copies each table into its
own ACPI memory and adds it to the root tables.

| Table | OEM ID, OEM table ID | Content |
|---|---|---|
| Idle states | `BC250`, `CPUIDLE` | one scope for each enabled processor, on the `Processor` object that the DSDT declares, with `_CST` and `_CSD` |
| Method stubs | `BC250`, `AMLSTUB` | an empty method with one argument for each of the three names that has no definition |

After the idle table is in the root tables, the module removes the stock CPU table. It finds the key of that
table through `EFI_ACPI_SDT_PROTOCOL`. When that protocol is not there, the stock table stays and still fails to
load, which is the state without this module.

### Where each value comes from

| Value | Source |
|---|---|
| The `_CST` object | a copy of the bytes of the firmware's own `_CST`, from the stock table in RAM at run time. The module checks that copy against the digest of M965 |
| The `_CSD` object of a processor that has a stock scope | a copy of the firmware's bytes of that scope |
| The `_CSD` object of a processor after the stock scopes | built: `{ 6, 0, position / 2, 0xFE, 2, 0 }`. The module builds it only when each stock scope obeys the same rule. The two processors of the pair must also have neighbour APIC IDs in the MADT at run time |
| The target of a scope | the `Processor` object of the DSDT whose processor ID is the processor UID of the MADT entry, read at run time |
| The names of the stubs | M966. A stub is built only for a name that the DSDT in RAM contains and that no table in RAM defines |
| The argument count of a stub | the call sites of the DSDT in RAM. AML does not encode the count of a call. A name gets a stub with one argument only when one `ArgN` or `LocalN` operand follows the name at each place. That is the form of each call in the stock DSDT (M966). The count is not a measured fact |

No number in an installed table is a number of this project. A processor that the rule does not cover gets no
scope, and the console says how many were left out.

### The identification

The module acts only when each of these checks passes. On any other input it prints what it found and installs
nothing.

1. The board is a BC-250: the PCI function and the SMBIOS table pass the identity check of the driver.
2. An SSDT has the header of the stock CPU table (OEM ID `AMD`, OEM table ID `AMD CPU`) and a correct checksum.
3. Every byte of that table has the stock form: scopes `\_PR.C000` and up without a gap, each with the `_CST` of
   the digest of M965 and a `_CSD` of the stock form, and nothing else.
4. The table set is whole. The root table lists each table, each one maps with a correct checksum, and each
   DSDT, SSDT and PSDT scans to its end. A table that the module cannot read can define a name a second time.
5. No table of that set defines `C000`, and no processor has `_CST` or `_CSD` in a table.
6. Each enabled processor of the MADT has exactly one `Processor` object in the DSDT.
7. For the stubs: the DSDT has the header of the stock DSDT (OEM ID `ALASKA`, OEM table ID `A M I `).

The firmware writes parts of the DSDT at each start, so a digest of the whole DSDT cannot identify it. The whole
CPU table has a digest for the capture with 12 logical processors. The console says if the table in RAM has that
digest or only the stock content.

### What Windows requires, and what the module leaves out

The Microsoft processor power management document gives these rules. Windows wants the processor objects in one
namespace, `\_PR` or `\_SB`, not both. The idle table keeps them in `\_PR`, where the DSDT declares them. Windows
wants `_CSD` with the coordination type 0xFE. The firmware's objects have it. When an object fails the checks of
Windows, Windows does not use that control and logs an event.

The same document says that processor performance states need `_PSS`, `_PCT` and `_PPC`. The module writes none of
them. A `_PSS` package needs the frequency, the power and the control value of each state. No value of this kind
is a fact for this board: the firmware's own package holds only 0xFFFF (M966). The control encoding of `_PCT` for
this CPU needs an AMD source that this project does not have. The measurement is row L55 of
[the Linux wishlist](../linux-session-wishlist.md).

The module also does not change the FADT, the MADT or the DSDT, and it does not add processor objects. The DSDT
already declares 16.

## The Secure Boot module

### What it verifies

The module gives a verdict on a PE image against two databases. `db` allows and `dbx` forbids. The sequence is
the one of the UEFI image check.

1. The Authenticode SHA-256 digest of the image is in `dbx`: forbidden.
2. For each Authenticode signature of the image that verifies: walk from the certificate of the signer up through
   the certificates of the signature. A certificate on the way that is in `dbx`, or that a `dbx` certificate
   signed: forbidden. `dbx` can name a certificate whole, or by the SHA-256 digest of its signed part. A certificate on the way that is in `db`, or that a `db` certificate signed: allowed.
3. The digest of the image is in `db`: allowed.
4. In all other cases the image is not allowed.

The databases are files beside the stage: `db.esl` and `dbx.esl`. Their format is the `EFI_SIGNATURE_LIST` of the
UEFI `db` variable, so standard tools can make them. `tools/efi/secure-boot/tools/mkesl.py` makes them too. The
module reads three entry types: an X.509 certificate, a SHA-256 image digest, and the SHA-256 digest of the
signed part of a certificate with a revocation time. It counts every other entry on the console. An entry with a
revocation time forbids at every time, because the stage does not read time stamps. That is stricter than the
UEFI rule.

When the firmware has `db` and `dbx` variables, the module reads them with `GetVariable` and adds their lists to
the lists of the files. The BC-250 firmware has none. A database that is not well formed allows no image.

### The inputs of the enforce mode

`sb.mode=enforce` starts an image only when each of these conditions is true.

- `db.esl` is beside the stage and is well formed.
- `dbx.esl` is beside the stage and is well formed. A file with one empty list forbids nothing, and
  `mkesl.py --empty` makes it. A file that is not there is a defect, because a failed copy looks the same.
- Each `db` and `dbx` variable of the firmware that exists reads and is well formed.
- `dbx` has no entry of a type that the stage does not read. Such an entry can forbid the image.

With a defect, the module prints the reason and refuses the next image.

The cryptography is in the stage: SHA-256, DER, X.509, PKCS #7 and RSA signature verification with keys of 2048
to 4096 bits. The stage has no private key and no signing function. The module does not check certificate dates,
key usage or time stamps. A PC firmware does not check dates for boot images either, and the stage has no trusted
clock. The only revocation is `dbx`.

### Where the check sits

The PI Specification defines the hook. The firmware core calls `FileAuthentication` of
`EFI_SECURITY2_ARCH_PROTOCOL` for each image that `LoadImage` loads, with the bytes of the image.
`EFI_ACCESS_DENIED` from that call means that the core does not load the image.

The module does not write into the protocol structure of the firmware, which can be in read-only memory. It
installs a structure of its own in the place of that one, with `ReinstallProtocolInterface` on the handle that
carries the protocol. The edk2 core then reads the protocol again, because a new installation signals its
notification. The function of the module gives the verdict and then calls the function of the firmware. The
module prints one console line before this step, so a firmware that stops there leaves a trace. So the check covers the Windows Boot Manager and every other image that something loads through `LoadImage` before ExitBootServices. At the
unwind, the module installs the structure of the firmware again.

An image that the stage starts can install a Security2 protocol of its own and return without an unwind. The
function of that image can then call into the stage. The module finds that at its unwind, says so, and restarts
the board as the failure policy tells. An image for `next=` must put the protocol back before it returns.

The module does not rely on the hook alone. Before the start, the core gives the bytes of the Windows Boot Manager
to the gate of the module. In `enforce` mode the gate answers "bind": the core then loads the image from that
buffer, so the bytes that start are the bytes that the gate verified. After the load, the module prints how many
times the firmware called the hook. Zero says that this firmware core does not call the new structure, and the
gate was the only check.

### Point (a): what reports "Secure Boot on" to Windows

Windows reads two UEFI variables of the global vendor GUID through the variable services. The Microsoft OEM
guidance says: "Variables must be set to SecureBoot=1 and SetupMode=0". The UEFI Specification defines both as
variables with the attributes boot service access and runtime access, which makes them volatile. The platform
firmware creates them. `PK`, `KEK`, `db` and `dbx` are non-volatile authenticated variables.

The firmware of the BC-250 has no Secure Boot code. It has none of the six variables, and Windows says "Variable
is currently undefined". Three ways exist to give Windows the variables.

| Way | NVRAM write |
|---|---|
| `SetVariable` with the non-volatile attribute | yes. Not allowed without explicit consent |
| `SetVariable` with only the volatile attributes | not known. The AMI variable code has a list of names that it handles in a special way, and `SecureBoot`, `PK` and `KEK` are in it. What it stores for these names is not measured |
| A runtime driver that answers `GetVariable` for these names from RAM | no |

So a report without an NVRAM write is possible in principle, through the third way. The stage does not do it.
Point (b) gives the reason.

### Point (b): the conflict with the test-signed driver

The driver of this project is test-signed. Windows loads a test-signed kernel driver only in its test-signing
mode, which is the boot option `testsigning`. While Windows reads Secure Boot as on, it refuses that option. The
Microsoft page of the option says that Secure Boot must be off before the option is set. `bcdedit` answers "The
value is protected by Secure Boot policy". So a start with `SecureBoot` = 1 gives a Windows that does not load the
driver of this project.

### The reported mode is not built

A mode that served `SecureBoot` = 1 from RAM was studied and is not in the stage. The project rule of 2026-11-13
is: no Secure Boot emulation while Windows refuses the test-signed driver under it. Point (b) shows that Windows
refuses it. Such a mode would also give no protection, and a program that sends the Secure Boot state to a server
would send a value that the firmware did not make.

One condition opens the topic again: a production-signed driver, which Windows loads without the test-signing
mode. The work then starts from point (a), with a lab test of the third way and a new decision by the owner.

### What each mode gives, and what it does not give

| Mode | What the user gets | What the user does not get |
|---|---|---|
| `sb=0` | nothing changes | no check |
| `sb.mode=audit` (default) | a console line with the verdict and the digest of each image that the stage starts. Nothing is refused | no protection. Windows reports Secure Boot as not available |
| `sb.mode=enforce` | an image that `db` does not allow, or that `dbx` forbids, does not start through the stage | no report to Windows. No check of what the Windows Boot Manager loads later. Windows makes those checks only when it reads `SecureBoot` = 1 |

In every mode the root of trust is a set of files on the EFI System Partition that nobody signed: the stage
itself, `db.esl` and `dbx.esl`. The firmware starts the stage without a check. So no mode protects against an
attacker who can write to the disk or change the boot order. Such an attacker replaces the stage or the database.
The check protects against an accident, such as a damaged or exchanged boot manager file. It is not a security
boundary.

### The limit: nothing for a remote party

The stage has no code that makes a statement for a remote party. It makes no TPM quote, no TPM event log, no
measurement and no attestation. The TPM is out of scope. The stage does not copy the keys or the name of another
vendor: the operator supplies every database.

The stage gives no Secure Boot variable to the operating system. In each mode, Windows reads the same Secure Boot
state as without the stage. A request to make Windows or a remote check read another state is outside this
design.

## SVM and IOMMU

The owner put the Setup options "SVM Mode" and "IOMMU" into tester.39, as the way to VBS and memory integrity.
The owner changes them by hand in the Setup menu of unit A. The stage does not set them. This section lists what
the tables and the stage must give for that work.

| Point | Status |
|---|---|
| The firmware gives no IVRS table with the options at their defaults | MEASURED, M13, M47. The ACPI dump of unit A has 15 tables, and IVRS is not one of them |
| The firmware gives no WSMT table | read from the same dump: WSMT is not one of the 15 tables. Windows names WSMT as a missing requirement of VBS (M870) |
| The firmware gives an IVRS table when "IOMMU" is on, and the table is correct | open measurement. Copy the tables with the option on, and read the IVRS device entries against the PCI devices, the IOAPIC and the HPET |
| The CPU table and the DSDT are the same with the two options on | open measurement. The ACPI module identifies the CPU table by its content. A changed table makes the module do nothing and say so |
| The MADT has the two unlocked processors as enabled, with correct APIC IDs | open measurement, row L54. The hypervisor starts each processor of the MADT |
| SVM is on for the two unlocked cores after the warm reset | open measurement. Read the SVM disable bit of the `VM_CR` register on each of the 8 processors, with the option on |
| VBS starts with SVM and IOMMU on and without Secure Boot | open measurement. The Microsoft VBS page lists Secure Boot as a requirement. The stage does not report Secure Boot |
| The memory of the stage does not disturb the start of the hypervisor | open measurement. The stage keeps only loader memory, and the firmware owns the installed tables |

The stage must not add a WSMT table. That table is a statement about the SMM code of the firmware, and this
project cannot examine that code. The stage also builds no IVRS table: a wrong IVRS table can stop the start or
damage data, and the firmware has its own builder. When the IVRS table of the firmware has a defect, the defect
goes into the facts first, and a correction is a new decision.

## The test plan

### Host tests

`pwsh tools\efi\stage\build.ps1` builds the image and the host tests and runs the tests. The tests need no
firmware.

| Part | What the test checks |
|---|---|
| shared library | SHA-256 against the vectors of the standard. The AML writer against the AML name scan |
| core | the load options: claimed, not understood, dropped |
| core, log | the log file against the console of the model: before the next image starts, after each line, behind an earlier start, and before a restart. A console of more than 60 KiB with `log=line` and with `log=1`. A volume that takes no write or stops in the middle, and a file above its bound. A line for the console only |
| core unlock | the tests of [the core unlock page](efi-core-unlock.md), unchanged |
| ACPI module, unit A | the plan and both tables from the ACPI dump of unit A outside the repository. The test says SKIP when the dump is not there |
| ACPI module, changed tables | 16 processors in pairs and not in pairs. A shorter stock table and one changed byte. A table that defines the target, and a second run |
| ACPI module, table set | a wrong checksum in a definition block, and a block that does not scan. A root table that did not give each table. In each case nothing is built |
| ACPI module, result | a name scan of each built table, its length field and its checksum |
| ACPI module, namespace | ACPICA loads each built table with the stock DSDT and the other tables. Each scope must be on a `Processor` object, no name can have two definitions, and the DSDT must have no method without a definition. Two controls come first: the missing target of the stock CPU table, and a table that is loaded twice |
| Secure Boot, images | a chain of three keys made at run time. Each certificate of the chain is the `db` entry in turn. Changed image bytes, a changed signature and another signer. A certificate without the CA flag, and two signatures. `dbx` by digest and by certificate |
| Secure Boot, robustness | each byte of a signed image changed in turn, and each length of the file. No change of a signed byte is allowed |
| Secure Boot, databases | the digest of a certificate in `dbx`, and a database that is not well formed. An unknown entry type and an empty list. Each defect of the enforce inputs |
| Secure Boot, real images | necessary: the Windows Boot Manager and the Windows loader of the build PC must pass under the issuer of their signer. This is the positive control on bytes and signatures that this project did not make. The build fails without them |
| UEFI bindings | the real entry point of the stage against a model of the firmware in the test. The start from the path and from the verified buffer. A refused image and each defect of the enforce inputs. An image that the firmware does not start. The hook and its unwind, with a firmware that does not take its protocol back. The installation of the tables and the removal of the stock table |
| Secure Boot, fixtures | the signature check of Windows reads the signed fixture and refuses the changed fixture. This is the control of the fixtures, by code that this project did not write |

The test keys are made at each run and are never written to a file. The repository holds no private key. The
host tests run with the address sanitizer of the compiler, so a read outside a buffer stops the test.

### Lab steps

The lab runs the UEFI bindings on the real firmware. The host runs them only against a model, which follows the
edk2 behaviour and not the AMI firmware. Each step starts the stage from the GRUB of the lab
stick. No step makes a firmware boot entry. [The README of the stage](../../tools/efi/stage/README.md) gives the GRUB
entry.

Each step uses `log=line` and reads `bc250-stage.log` in Windows after the start. No person reads the screen.

1. Copy `bc250-stage.efi` to `\EFI\BC250\` on the EFI System Partition of the Windows disk. Start it with
   `unlock=0 acpi=0 sb=0 probe=1`. Read the log: the core runs, no module runs, the probe lines give the presence
   of the four protocols, and Windows starts.
2. Start it with `unlock=0 sb=0 acpi.apply=0`. Read the plan of the ACPI module. Then start it with
   `unlock=0 sb=0`. Read the lines of the ACPI module. In Windows, examine the processor devices, the idle
   states and the ACPI errors of the System log.
3. Put `db.esl` beside the stage. Start it with `unlock=0 acpi=0`. Read the verdict lines and the count of hook
   calls.
4. Put `dbx.esl` beside the stage. Start it with `unlock=0 acpi=0 sb.mode=enforce`. Windows must start. Then put
   the digest of the boot manager into `dbx.esl`: the stage must refuse the boot manager, and the firmware goes on
   with its boot order.
5. Start it with all modules, `reset=warm` and `report=1`.

## What is measured and what is not

| Statement | Status |
|---|---|
| The defects of the stock tables | MEASURED, M965, M966, M47 |
| The built tables are well formed and hold the firmware's objects | tested on the host, with two parsers |
| The firmware has `EFI_ACPI_TABLE_PROTOCOL` and `EFI_ACPI_SDT_PROTOCOL`, and accepts the tables | not measured |
| Windows and Linux use the idle states of the new table | not measured. Row L56 |
| The effect of the stock defects on Windows | not measured |
| The tables and the APIC IDs of a start with 8 cores, and on BIOS P5.00 | not measured. Row L54 |
| The AMI firmware core calls Security2 for `LoadImage` | not measured. The module prints the count at each start |
| `LoadImage` from a buffer with a device path starts the Windows Boot Manager on this firmware | not measured |
| The signature check on real images | tested on the host with one real image when the build gets one |

## Open questions

Each question has the measurement that answers it.

1. Does the AMI firmware core call Security2 at `LoadImage`, and does the firmware have the two ACPI protocols?
   Lab steps 1 and 3 answer it: the stage prints the presence of the protocols and the count of hook calls.
2. Does `LoadImage` from a verified buffer with a device path start the Windows Boot Manager on this firmware?
   Lab step 4 answers it.
3. Are the MADT and the CPU table the same after a start with 8 cores, and on BIOS P5.00? Row L54 answers it.
4. Is the I/O address of the C2 state in the stock `_CST` the address that the CPU uses? The report of lab step 5
   reads the C-state base address register, and the two values must agree.
5. What are the real P-state frequencies and voltages? Row L55 answers the frequencies. The `_PCT` contract needs
   an AMD source.
6. Do Windows and Linux use the idle states of the new table? Row L56 answers it for Linux, and lab step 2 for
   Windows.

The question about a runtime driver at `SetVirtualAddressMap` went away with the reported mode.