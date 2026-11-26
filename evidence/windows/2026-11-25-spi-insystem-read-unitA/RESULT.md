# Unit A BIOS SPI flash read in system through the flash protocol of the firmware (2026-11-25)

One start of the EFI stage (`tools/efi/stage`, sources of 5496c8f75) on unit A, with the job `read` of its flash
module. The stage read the 16 MiB SPI flash of the board through the flash protocol of the firmware, before Windows
started. This is step M1 of [the flash page](../../../docs/design/p6-flash.md). The start wrote nothing to the
flash, to a firmware variable or to the boot configuration. The image is not in this directory: it holds the UEFI
variable store and AMD and AMI firmware.

## The start

| Time (UTC) | Event | File |
| --- | --- | --- |
| 18:57:05 | The Windows start before it: route cks, `secure_boot_confirm=True`, HLKSvc and HlkCampaignGuard Running | `bootinfo-before.txt` |
| 20:05:54 | Clean shutdown of that Windows | `restart.txt` |
| 20:07:09 | Standby power at the plug: 10.3 W in two samples | `restart.txt` |
| 20:07:16 to 20:07:24 | The plug off, then on | `restart.txt` |
| 20:08:04 | The stage wrote the result file, the log and the image file on the stick (time of the files) | `session.log` |
| 20:08:28 | Windows started on the persistent route of the stick | `bootinfo-after.txt` |

The stick started the stage copy `\efi\wckfl\bc250-stage.efi` (2349032 bytes, SHA-256 `42a29581...e96fa`) with
`unlock=0 tpm=0 acpi=0 iommu=0 sb=0 flash=read flash.reset=1 log=line`. The flash module was the only module that
was on. The signature of the stage copy is the signature of the image key of the project.

## Result

1. The firmware had the flash protocol `755B6596-6896-4BA3-B3DD-1C629FD1EA88` when the stage ran
   (`bc250-stage.log`: `flash: job read, flash protocol at 0x7E213C58`).
2. `Read` gave the whole image: `flash: read 16777216 of 16777216 bytes`, in 256 pieces of 64 KiB.
3. The SHA-256 of the image file is one value on the unit, in the result file and on the host:
   `f44cabeafe04f86f7159ab02d54bafd59f86e77e64f303c17db5ae7320c1fd08` (`session.log`, `bc250-flash.result`).
4. With the variable store read as zeros, the image has the SHA-256
   `0817b85481ef1aa3e8d1971e1d6a33152bf637867f8619e5a01ae5c5a276a28e`. The programmer read behind the SPI write
   of 2026-11-19 (image SHA-256 `73da918f...e596cc`) has the same value (`identity.txt`).
5. 0 bytes differ outside the variable store: the 16646144 bytes of 0x020000 to 0xFFFFFF are equal, the PSP
   directories and the PEI volume included. 35751 bytes differ inside the store (0x000000 to 0x01FFFF).
6. The stock image P3.00 of the workspace, read the same way, has the SHA-256 `62411d52...f10ad`. 1449125 bytes
   differ outside the store, so the image of unit A is not the stock image (`identity-vs-stock.txt`).
7. The volumes, from the result file. The store volume is 131072 bytes, used to 0x20000. The outer volume is
   3276800 bytes, used to 0xC23647. The PEI volume is 2097152 bytes, used to 0x1000000. The compressed stream
   of the outer volume starts at 0xAE00B5: 1324434 bytes for 4354064 plain bytes.
8. The 1952185 bytes behind the image file of the outer volume (0xC23647 to 0xDFFFFF) all read FF. The first
   whole free sector of 4096 bytes is at 0xC24000 (`volumes.txt`, `free_space.py`).
9. `ResetSystem(EfiResetCold)` from the stage restarted the board (`flash: restarting the board`). Windows
   started about 24 s after the time of the files, on the persistent route, with `secure_boot_confirm=True`.

The result file ends with `dump yes` and `status ok`.

## Limits

- n = 1 unit, one start, one image (the image of the SPI write of 2026-11-19).
- `Erase`, `Write` and `EraseWrite` were not called. The write functions of the protocol are not measured.
- The time of the read is not known: the log has no time. The files have a time 40 s after the plug came on,
  the delay of the menu included.
- Secure Boot was read in Windows before and after the start, not in the start itself.
- The variable store was not examined. Which variables changed in the six days between the two reads is not
  known.
