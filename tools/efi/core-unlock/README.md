# core-unlock

A UEFI application for the BC-250. It enables the two CPU cores that the board is sold without, before Windows
starts. Then it starts the Windows Boot Manager. The design, the decision table and the open questions are in
[the design page](../../../docs/design/efi-core-unlock.md).

## Build

1. Open pwsh 7 in the repository root.
2. Run `pwsh tools\efi\core-unlock\build.ps1`.
3. Read the last line. It must say `exit code 0`.

The script writes `core-unlock.efi`, `SHA256SUMS` and the host test to `P:\BC-250\scratch\build\efi-core-unlock`.
Give `-Out <directory>` for another place. Give `-SkipTests` to build the image alone.

## Load options

| Option | Result |
|---|---|
| `reset=warm` | the reset after the message is `ResetSystem` with `EfiResetWarm`. This is the default |
| `reset=fadt` | the reset after the message is the reset register of the ACPI FADT |
| `reset=none` | no reset. The cores come at the next warm restart of Windows |
| `report=1` | before the decision, write the read-only report to `core-unlock-report.txt` beside the image and show it on the console. The default is `report=0` |

The report does not change the decision. To get a report without the message, press a key in the key window: the report is written before that window opens.

## Run it once from a UEFI shell

This way writes no NVRAM variable.

1. Copy `core-unlock.efi` to `\EFI\BC250\` on the EFI System Partition of the Windows disk.
2. Start the UEFI shell of the board.
3. Change to the file system of that partition.
4. Type `\EFI\BC250\core-unlock.efi reset=warm`.
5. Read the console lines before the reset.

## Install it as a boot entry

WARNING: A boot entry and a change of the boot order are NVRAM writes. Get explicit consent for both before
step 3.

1. Start Linux from the lab stick.
2. Copy `core-unlock.efi` to `EFI/BC250/` on the EFI System Partition of the Windows disk.
3. Run `efibootmgr --create --disk <disk> --part <partition> --label "BC-250 core unlock" --loader '\EFI\BC250\core-unlock.efi' --unicode 'reset=warm'`.
4. Run `efibootmgr` and record the number of the new entry.
5. Make sure that the new entry is first in `BootOrder` and that the Windows Boot Manager entry stays in it.

6. Start Windows and restart it one time, so that the session Windows keeps for Fast Startup has the same processor
   count as the next start.

CAUTION: Do not use hibernation while the entry is installed. A resume of a session that was saved with a different
processor count is not measured (`docs/design/efi-core-unlock.md`, "Hibernation and Fast Startup").

To change the reset method, remove the entry and create it again with the new option.

## Remove it

1. Start Linux from the lab stick.
2. Run `efibootmgr --bootnum <number> --delete-bootnum`.
3. Remove the directory `EFI/BC250/` on the EFI System Partition.

## The marker

The application keeps its loop guard in `core-unlock.mark`, beside the image. A marker with the word `failed` stops
every later attempt. Remove the marker to try again, for example after a change of the reset method.
