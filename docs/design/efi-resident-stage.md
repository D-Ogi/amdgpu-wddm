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
| edk2, `MdeModulePkg/Core/RuntimeDxe/Runtime.c` | the same commit | what the firmware converts at `SetVirtualAddressMap` |
| edk2, `SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.c` | the same commit | the sequence of the db and dbx checks, and the sequence of the Authenticode digest |
| ACPI Specification | 6.5 | the table header, the AML encodings, `_CST` and `_CSD` |
| Microsoft, "Windows Authenticode Portable Executable Signature Format" | version 1.0, 2008 | the Authenticode digest and the signature structure |
| Microsoft, "PE Format" | learn.microsoft.com, read 2026-11-13 | the PE headers and the attribute certificate table |
| Microsoft, "Secure boot" (OEM guidance), `windows-hardware/manufacture/desktop/secure-boot-landing` | `ms.date` 2026-03-10, repository commit `3abfddf4` | what Windows requires of `SecureBoot` and `SetupMode` |
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
| `secure-boot/` | module `secure-boot` | the image check and the reported mode |
| `secure-boot/rt/` | runtime driver `bc250-sbvar.efi` | the Secure Boot variables in RAM, for the reported mode only |
| `lib/` | shared library | the UEFI header, the small C library, text and device paths, SMBIOS, SHA-256 |
| `lib/acpi/` | shared ACPI table library | the root table walk, the AML name scan, the AML writer |

### The interface between the core and a module

`stage/src/stage.h` is the whole interface. A module is one `struct stage_module`: a name, an off switch and four
functions.

| Function | When the core calls it |
|---|---|
| `run` | once, before Windows starts. This is the work of the module |
| `gate` | once, with the bytes of the next image. The answer is pass, bind or deny |
| `loaded` | after the firmware loaded that image, with the status |
| `unwind` | when the image did not start or returned |

A module calls the core for a console line, the board identity, the ACPI root tables, a file beside the stage and
pool memory. A module never calls another module, and it never includes a header of another module. The shared
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
| The check in the Security2 protocol | a pointer into the stage image | the firmware protocol | no. Boot services end, and nothing calls the protocol later |
| The installed ACPI tables | ACPI memory of the firmware | the firmware. `InstallAcpiTable` copies the table | yes, as data. The operating system reads them |
| `bc250-sbvar.efi`, in the reported mode only | runtime services code and data | the firmware loads it as a runtime driver | yes, as code. It serves the variable calls |

So the default start leaves no code of this project in memory. The only resident data are the ACPI tables, and
the firmware owns them. Resident code exists only when the operator asks for `sb.reported=1`.

The runtime driver holds one static block with the variables. The block has offsets and no pointer. The driver
keeps three pointers to the variable functions of the firmware, and it converts them in the event group of
`SetVirtualAddressMap`. It leaves the conversion of the runtime services table to the firmware, as the edk2
runtime core does. The driver refuses to start when the firmware did not load it into runtime services code.

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
| `sb.reported=1` | 0 | serve the Secure Boot variables to the operating system. Needs `sb.mode=enforce` |

## The failure policy

Every module fails open: when something is wrong, the module prints the reason, does nothing more, and Windows
starts as it does without the stage.

- A board that is not a BC-250: the core unlock and the ACPI module do nothing and say so.
- A table, a file or a protocol that is not there or is not sound: the module says which one, and stops.
- A built table that fails its own check: it is not installed.
- A defect in the inputs of the Secure Boot module in `enforce` mode: the mode becomes `audit`, with the reason.

One case does not fail open, and the operator asks for it. In `sb.mode=enforce`, an image that is not allowed does
not start. The stage then returns `EFI_ACCESS_DENIED`, and the firmware goes on with its boot order.

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
| The names and the argument count of the stubs | M966. A stub is built only for a name that the DSDT in RAM contains and that no table in RAM defines |

No number in an installed table is a number of this project. A processor that the rule does not cover gets no
scope, and the console says how many were left out.

### The identification

The module acts only when each of these checks passes. On any other input it prints what it found and installs
nothing.

1. The board is a BC-250: the PCI function and the SMBIOS table pass the identity check of the driver.
2. An SSDT has the header of the stock CPU table (OEM ID `AMD`, OEM table ID `AMD CPU`) and a correct checksum.
3. Every byte of that table has the stock form: scopes `\_PR.C000` and up without a gap, each with the `_CST` of
   the digest of M965 and a `_CSD` of the stock form, and nothing else.
4. No table defines `C000`, and no processor has `_CST` in another table.
5. Each enabled processor of the MADT has exactly one `Processor` object in the DSDT.
6. For the stubs: the DSDT has the header of the stock DSDT (OEM ID `ALASKA`, OEM table ID `A M I `).

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
   signed: forbidden. A certificate on the way that is in `db`, or that a `db` certificate signed: allowed.
3. The digest of the image is in `db`: allowed.
4. In all other cases the image is not allowed.

The databases are files beside the stage: `db.esl` and `dbx.esl`. Their format is the `EFI_SIGNATURE_LIST` of the
UEFI `db` variable, so standard tools can make them. `tools/efi/secure-boot/tools/mkesl.py` makes them too. The
module reads two entry types: an X.509 certificate and a SHA-256 digest. It counts every other entry on the
console.

The cryptography is in the stage: SHA-256, DER, X.509, PKCS #7 and RSA signature verification with keys of 2048
to 4096 bits. The stage has no private key and no signing function. The module does not check certificate dates,
key usage or time stamps. A PC firmware does not check dates for boot images either, and the stage has no trusted
clock. The only revocation is `dbx`.

### Where the check sits

The PI Specification defines the hook. The firmware core calls `FileAuthentication` of
`EFI_SECURITY2_ARCH_PROTOCOL` for each image that `LoadImage` loads, with the bytes of the image.
`EFI_ACCESS_DENIED` from that call means that the core does not load the image.

The module puts its own function into that protocol and keeps the function of the firmware. Its function gives
the verdict and then calls the function of the firmware. So the check covers the Windows Boot Manager, the
runtime driver, and every other image that something loads through `LoadImage` before ExitBootServices. At the
unwind, the module puts the function of the firmware back.

The module does not rely on the hook alone. Before the start, the core gives the bytes of the Windows Boot Manager
to the gate of the module. In `enforce` mode the gate answers "bind": the core then loads the image from that
buffer, so the bytes that start are the bytes that the gate verified. After the load, the module prints how many
times the firmware called the hook. Zero says that this firmware core does not call Security2, and the gate was
the only check.

### Point (a): what reports "Secure Boot on" to Windows

Windows reads two UEFI variables of the global vendor GUID through the variable services. The Microsoft OEM
guidance says: "Variables must be set to SecureBoot=1 and SetupMode=0". The UEFI Specification defines both as
variables with the attributes boot service access and runtime access, which makes them volatile. The platform
firmware creates them. `PK`, `KEK`, `db` and `dbx` are non-volatile authenticated variables.

The firmware of the BC-250 has no Secure Boot code. It has none of the six variables, and Windows says "Variable
is currently undefined". Three ways exist to give Windows the variables.

| Way | NVRAM write | Decision |
|---|---|---|
| `SetVariable` with the non-volatile attribute | yes | not allowed without explicit consent. Not used |
| `SetVariable` with only the volatile attributes | not known. The AMI variable code has a list of names that it handles in a special way, and `SecureBoot`, `PK` and `KEK` are in it. What it stores for these names is not measured | not used. The stage calls `SetVariable` nowhere |
| A runtime driver that answers `GetVariable` for these names from RAM | no. The driver has no write path of its own | used, in the reported mode only |

So the design works from volatile state. `bc250-sbvar.efi` puts three functions into the runtime services table,
in front of the functions of the firmware.

- `GetVariable` and `GetNextVariableName` answer for the names of the store, and pass every other name on.
- `SetVariable` returns `EFI_WRITE_PROTECTED` for a name of the store. For every other name it passes the call of
  the operating system on, unchanged. The stage and the driver start no write.

The store holds `SecureBoot` = 1, `SetupMode` = 0, and `PK`, `KEK`, `db` and `dbx` from the files beside the stage.
Each variable has only the two volatile attribute bits. No variable claims to be non-volatile or authenticated.
One more variable, `Bc250StageSecureBoot` under a vendor GUID of this project, says in plain text where the
variables come from. The module does not start the reported mode when the firmware has one of the six variables.

### Point (b): the conflict with the test-signed driver

The driver of this project is test-signed. Windows does not allow its test-signing mode while Secure Boot is on
(study source S6 of the Secure Boot study of 2026-11-13). So a start with `SecureBoot` = 1 gives a Windows that
refuses the driver.

For that reason the report is a separate mode, and it is off by default. The default mode verifies and reports
only. Windows then reads no Secure Boot variable, as it does without the stage, and the driver loads.

### What each mode gives, and what it does not give

| Mode | What the user gets | What the user does not get |
|---|---|---|
| `sb=0` | nothing changes | no check |
| `sb.mode=audit` (default) | a console line with the verdict and the digest of each image that the stage starts. Nothing is refused | no protection. Windows reports Secure Boot as not available |
| `sb.mode=enforce` | an image that `db` does not allow, or that `dbx` forbids, does not start through the stage | no report to Windows. No check of what the Windows Boot Manager loads later. Windows makes those checks only when it reads `SecureBoot` = 1 |
| `sb.mode=enforce sb.reported=1` | the same check, and Windows reads `SecureBoot` = 1. The Windows loader then enforces its own signature rules on the kernel and the boot drivers | no test-signed driver: the driver of this project does not load. No firmware Secure Boot. No protected variables: Windows cannot write `dbx` updates, and it logs that |

In every mode the root of trust is a set of files on the EFI System Partition that nobody signed: the stage
itself, `db.esl` and `dbx.esl`. The firmware starts the stage without a check. So no mode protects against an
attacker who can write to the disk or change the boot order. Such an attacker replaces the stage or the database.
The check protects against an accident, such as a damaged or exchanged boot manager file. It is not a security
boundary.

### The limit: nothing for a remote party

The stage has no code that makes a statement for a remote party. It makes no TPM quote, no TPM event log, no
measurement and no attestation. The TPM is out of scope. The stage does not copy the keys or the name of another
vendor: the operator supplies every database.

The reported mode changes what the local Windows reads. A program in that Windows that sends the Secure Boot
state to a server sends the value that the stage served. The stage cannot prevent that. The design therefore
keeps the mode off by default, labels the variables with their origin, and does not hide the missing
authentication attribute from a program that reads the attributes. A request to hide that origin, or to satisfy a
remote check, is outside this design.

## The test plan

### Host tests

`pwsh tools\efi\stage\build.ps1` builds the two images and the host tests and runs the tests. The tests need no
firmware.

| Part | What the test checks |
|---|---|
| shared library | SHA-256 against the vectors of the standard. The AML writer against the AML name scan |
| core | the load options: claimed, not understood, dropped |
| core unlock | the tests of [the core unlock page](efi-core-unlock.md), unchanged |
| ACPI module, unit A | the plan and both tables from the ACPI dump of unit A outside the repository. The test says SKIP when the dump is not there |
| ACPI module, changed tables | 16 processors in pairs and not in pairs. A shorter stock table, one changed byte and a wrong checksum. A table that defines the target, and a second run |
| ACPI module, result | a name scan of each built table, its length field and its checksum. The ACPICA disassembler reads each built table as a second parser |
| Secure Boot, images | a chain of three keys made at run time. Each certificate of the chain is the `db` entry in turn. Changed image bytes, a changed signature and another signer. A certificate without the CA flag, and two signatures. `dbx` by digest and by certificate |
| Secure Boot, robustness | each byte of a signed image changed in turn, and each length of the file. No change of a signed byte is allowed |
| Secure Boot, real image | optional: a real signed UEFI image must pass under the issuer of its signer. This is the positive control on bytes that this project did not make |
| variable store | the status and size rules of `GetVariable` and `GetNextVariableName`, against a model of the firmware |

The test keys are made at each run and are never written to a file. The repository holds no private key.

### Lab steps

The lab runs the UEFI bindings, which the host cannot test. Each step starts the stage from the GRUB of the lab
stick. No step makes a firmware boot entry. The steps are in the report of the work package and in the README.

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
| The AMI runtime core relocates the runtime driver and converts the table at `SetVirtualAddressMap` | not measured. Row L56 tests it under Linux first |
| Windows accepts `PK`, `KEK` and `db` without the non-volatile and authenticated attributes | not measured |
| The signature check on real images | tested on the host with one real image when the build gets one |
