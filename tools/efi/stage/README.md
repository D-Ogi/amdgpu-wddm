# The EFI stage

`bc250-stage.efi` is a UEFI application for the BC-250. It runs before Windows and then starts the Windows Boot
Manager. [The design page](../../../docs/design/efi-resident-stage.md) gives the modules, the modes and the open
questions. [The core unlock page](../../../docs/design/efi-core-unlock.md) gives the decision table of the first
module.

## Build

1. Open pwsh 7 in the repository root.
2. Run `pwsh tools\efi\stage\build.ps1`.
3. Read the last lines. The line `bc250-stage host tests exit code 0` must be there.
4. Look for lines that start with `SKIP`. Each one names an input that the tests did not have.

The script writes `bc250-stage.efi`, `SHA256SUMS` and the host test to
`P:\BC-250\scratch\build\efi-stage`. Give `-Out <directory>` for another place. Give `-SkipTests` to build the
image alone. The Secure Boot tests read two signed images of the Windows installation of the build PC as controls.
`-SignedImage <file>` and `-SecondSignedImage <file>` name other ones. The build fails when a control is not there.

## Load options

| Option | Default | Result |
|---|---|---|
| `unlock=0` | 1 | the core unlock does not run |
| `acpi=0` | 1 | the ACPI module does not run |
| `sb=0` | 1 | the Secure Boot module does not run |
| `next=<path>` | the Windows Boot Manager | the image that the stage starts after its work |
| `reset=warm` | yes | the reset after the unlock message is `ResetSystem` with `EfiResetWarm` |
| `reset=fadt` | no | the reset after the unlock message is the reset register of the ACPI FADT |
| `reset=none` | no | no reset. The cores come at the next warm restart of Windows |
| `report=1` | 0 | write the read-only report to `core-unlock-report.txt` beside the image |
| `key=0` | 1 | no key window before the unlock. Use it on a board with no person at it |
| `acpi.idle=0` | 1 | do not install the idle state table |
| `acpi.stubs=0` | 1 | do not install the method stub table |
| `acpi.apply=0` | 1 | build and check the tables of the ACPI module, and install and remove none |
| `probe=1` | 0 | a module that is off says which protocols the firmware has for it. It changes nothing |
| `log=0` | 1 | write no log file |
| `log=line` | no | write the log file after each console line, not only before the next image starts |
| `sb.mode=audit` | yes | report the verdict on each image, and refuse none |
| `sb.mode=enforce` | no | do not start an image that the databases do not allow. A defect in `db.esl` or `dbx.esl` refuses each image |
| `sb.windows=1` | no | with `sb.mode=enforce`: Windows reads Secure Boot as on, with the four databases of the kit |
| `sb.windows=off` | no | on a firmware that has Secure Boot on: Windows reads Secure Boot as off for this start. The module does not check in that start |
| `flash=read` | no job | the flash module reads the firmware image of the board and writes it beside the stage image ([the flash page](../../docs/design/p6-flash.md)) |
| `flash.reset=1` | 0 | with a flash job: the stage restarts the board after the job, and Windows does not start in this boot |

The stage counts each option that no module understands and prints the count.

## The files beside the stage

Put these files in the directory of `bc250-stage.efi`. Only the stage itself is necessary.

| File | Used by | Content |
|---|---|---|
| `db.esl` | Secure Boot module | the allowed certificates and image digests |
| `dbx.esl` | Secure Boot module | the forbidden certificates and image digests |
| `core-unlock.mark` | core unlock | the loop guard. The stage writes it |
| `bc250-stage.log` | core | the console lines of each start. The stage writes it. `log=0` stops that |
| `bc250-flash.dump` | flash job `read` | the 16 MiB firmware image of the board. The stage writes it |
| `bc250-flash.result` | flash job | the digests, the volumes and the status of the job. The stage writes it |

## Make the databases

`tools\efi\secure-boot\tools\mkesl.py` makes a database file. It needs only Python.

1. Get the certificate that signs your boot manager, as a DER or PEM file.
2. Run `python tools\efi\secure-boot\tools\mkesl.py --out db.esl --cert <certificate>`.
3. To allow one image by its digest, add `--image <file>` to that command.
4. To forbid one image, run `python tools\efi\secure-boot\tools\mkesl.py --out dbx.esl --image <file>`.
5. To forbid a certificate, add `--revoke-cert <certificate>` to that command.
6. To forbid nothing, run `python tools\efi\secure-boot\tools\mkesl.py --out dbx.esl --empty`. The file has one list with the image digest of 32 zero bytes: no image has that
   digest. Do not make a dbx file without an entry: the Windows Boot Manager of 26300 does not start when the stage
   serves it with `sb.windows=1` (BD-386).

`sb.mode=enforce` needs the two files. `sb.mode=audit` works without them.

The option `--digest <file>` prints the Authenticode SHA-256 digest of an image and writes nothing.

## Start it one time from GRUB

This procedure writes no NVRAM variable.

1. Copy `bc250-stage.efi` to `\EFI\BC250\` on the EFI System Partition of the Windows disk.
2. Add a menu entry to `grub.cfg` on the stick. Set `root` to that partition in the entry.
3. In the entry, write `chainloader /EFI/BC250/bc250-stage.efi unlock=0 sb=0`.
4. Start the board from the stick and select the entry.
5. Read the console lines of the stage on the screen, or read `\EFI\BC250\bc250-stage.log` after the start.
6. Remove one off switch for each later start, and read the lines again.

To read the result under Linux, put the stage and the boot loader of the stick on one partition. Then add
`next=<path of that boot loader>` to the options.

## Start it one time from a UEFI shell

This procedure writes no NVRAM variable.

1. Copy `bc250-stage.efi` to `\EFI\BC250\` on the EFI System Partition of the Windows disk.
2. Start the UEFI shell of the board.
3. Change to the file system of that partition.
4. Type `\EFI\BC250\bc250-stage.efi reset=warm`.
5. Read the console lines before the reset.

## Install it as a boot entry

WARNING: A boot entry and a change of the boot order are NVRAM writes. Get explicit consent for both before
step 3.

1. Start Linux from the lab stick.
2. Copy `bc250-stage.efi` to `EFI/BC250/` on the EFI System Partition of the Windows disk.
3. Run `efibootmgr --create --disk <disk> --part <partition> --label "BC-250 stage" --loader '\EFI\BC250\bc250-stage.efi' --unicode 'reset=warm'`.
4. Run `efibootmgr` and record the number of the new entry.
5. Make sure that the new entry is first in `BootOrder` and that the Windows Boot Manager entry stays in it.
6. Start Windows and restart it one time. The session that Windows keeps for Fast Startup then has the processor
   count of the next start.

CAUTION: Do not use hibernation while the entry is installed. A resume of a session that was saved with a different
processor count is not measured (`docs/design/efi-core-unlock.md`, "Hibernation and Fast Startup").

To change an option, remove the entry and create it again with the new options.

## Remove it

1. Start Linux from the lab stick.
2. Run `efibootmgr --bootnum <number> --delete-bootnum`.
3. Remove the directory `EFI/BC250/` on the EFI System Partition.

## What the Secure Boot module is not

The module does not make the board secure, and it does not make Windows report Secure Boot. The stage and its
databases are files that nobody signed. Read "What each mode gives, and what it does not give" in the design page.

## The marker

The core unlock keeps its loop guard in `core-unlock.mark`, beside the image. A marker with the word `failed` stops
every later attempt. Remove the marker to try again, for example after a change of the reset method.
