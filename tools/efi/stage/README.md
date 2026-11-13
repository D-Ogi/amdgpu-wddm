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

The script writes `bc250-stage.efi`, `bc250-sbvar.efi`, `SHA256SUMS` and the host test to
`P:\BC-250\scratch\build\efi-stage`. Give `-Out <directory>` for another place. Give `-SkipTests` to build the
images alone. Give `-SignedImage <file>` to add a real signed UEFI image to the Secure Boot tests.

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
| `acpi.idle=0` | 1 | do not install the idle state table |
| `acpi.stubs=0` | 1 | do not install the method stub table |
| `sb.mode=audit` | yes | report the verdict on each image, and refuse none |
| `sb.mode=enforce` | no | do not start an image that the databases do not allow |
| `sb.reported=1` | 0 | make Windows read "Secure Boot on". This needs `sb.mode=enforce` |

The stage counts each option that no module understands and prints the count.

## The files beside the stage

Put these files in the directory of `bc250-stage.efi`. Only the stage itself is necessary.

| File | Used by | Content |
|---|---|---|
| `db.esl` | Secure Boot module | the allowed certificates and image digests |
| `dbx.esl` | Secure Boot module | the forbidden certificates and image digests |
| `PK.esl`, `KEK.esl` | reported mode | the content of the `PK` and `KEK` variables that Windows reads |
| `bc250-sbvar.efi` | reported mode | the runtime driver |
| `core-unlock.mark` | core unlock | the loop guard. The stage writes it |

## Make the databases

`tools\efi\secure-boot\tools\mkesl.py` makes a database file. It needs only Python.

1. Get the certificate that signs your boot manager, as a DER or PEM file.
2. Run `python tools\efi\secure-boot\tools\mkesl.py --out db.esl --cert <certificate> --image bc250-sbvar.efi`.
3. To allow one image by its digest, add `--image <file>` to that command.
4. To forbid one image, run `python tools\efi\secure-boot\tools\mkesl.py --out dbx.esl --image <file>`.

The option `--digest <file>` prints the Authenticode SHA-256 digest of an image and writes nothing.

## Start it one time from GRUB

This procedure writes no NVRAM variable.

1. Copy `bc250-stage.efi` to `\EFI\BC250\` on the EFI System Partition of the Windows disk.
2. Add a menu entry to `grub.cfg` on the stick. Set `root` to that partition in the entry.
3. In the entry, write `chainloader /EFI/BC250/bc250-stage.efi unlock=0 sb=0`.
4. Start the board from the stick and select the entry.
5. Read the console lines of the stage.
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

## The reported mode

WARNING: With `sb.reported=1`, Windows refuses its test-signing mode. The driver of this project is test-signed
and does not load in that start. Use this mode only on an installation that does not need the driver.

The mode does not make the board secure. The stage and its databases are files that nobody signed. Read
"What each mode gives, and what it does not give" in the design page before you use it.

## The marker

The core unlock keeps its loop guard in `core-unlock.mark`, beside the image. A marker with the word `failed` stops
every later attempt. Remove the marker to try again, for example after a change of the reset method.
