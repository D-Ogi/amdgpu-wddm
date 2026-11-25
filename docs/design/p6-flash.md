# The flash job: P6 from the Installer

The code: [`tools/efi/flash/`](../../tools/efi/flash/). The job is a module of [the EFI stage](efi-resident-stage.md).
The build and the load options: [the README of the stage](../../tools/efi/stage/README.md).

## What it is

The Installer of the product brings a unit from the stock firmware to the P6 image. For that, something writes the
16 MiB SPI flash of the board. The lab writes it through a programmer on header J4004, with the board off. A user
has no programmer.

The flash job is the way without a programmer. The stage runs before Windows, so it can read the image, and after the
measurements of this page it writes it.

| Part | State |
|---|---|
| Job `read`: the image through the firmware, and two files beside the stage image | built. The host tests pass. MEASURED on unit A on 2026-11-25: [M973](../facts/hardware.md#m973) |
| Job `test`: each function of the protocol on one free sector and on the next sector, for the measurement M2 | built. The host tests pass. Not measured on a unit |
| Job `apply`: a package writes the image | the design in this page. Not built |

## The sources

| Source | Revision | Used for |
|---|---|---|
| The update kit of the firmware vendor for this board, outside the repository. Its image is the stock P5.00 of the workspace | the kit of 2022, read 2026-11-25 | the vendor flashes this board with no programmer |
| The code of the stock firmware P3.00: the module `FlashDriver` and the module list of the image | read 2026-11-25, one reader | the flash protocol of the firmware |
| edk2, `MdePkg/Include/Pi/PiFirmwareVolume.h` and `MdePkg/Include/Pi/PiFirmwareFile.h` | commit `556dcfb466e9f05affeaff881d738807f8156d3c` | the firmware volume header, the file header, the file states and the guided section, as the headers state the PI Specification |
| Fact M965 | 2026-11-16 | the chip and the bytes that are the unit's own |
| The read of the image of unit A through the job `read`, `evidence/windows/2026-11-25-spi-insystem-read-unitA/` | 2026-11-25 | what `Read` gives, and the image of unit A |

The edk2 code is a reference only. The stage copies no line of it.

## The facts

### The vendor flashes this board with no programmer

The update kit of the vendor has a UEFI shell and the EFI firmware update utility of AMI, version 5.14.02. The
script of the kit runs the utility on the new image with the options that program the main BIOS, the boot block, the
NVRAM and the non-critical blocks. The board restarts at the end. So the stock firmware has a service that writes its
own flash while the board runs.

The utility is the software of AMI and is under the licence of AMI. The product does not ship it.

### The stock firmware has the flash service

The stock image P3.00 has these modules for the flash. In DXE: `FlashDriver`, `SmiFlashDxe`, `ReFlash`,
`CapsuleRuntimeDxe` and `EsrtDxe`. In SMM: `FlashDriverSmm`, `SmiFlash` and `Ofbd`. In PEI: `CapsulePei` and
`Recovery`. The files that differ between P3.00 and P5.00 do not include them, and P6 keeps them.

`ReFlash` is the Setup page "Flash update" of AMI. The stock Setup does not show the page.

### The flash protocol

`FlashDriver` installs the protocol `755B6596-6896-4BA3-B3DD-1C629FD1EA88`. The protocol has six functions:

| Slot | Call | Result |
|---|---|---|
| 0 | `Read(Address, Length, Buffer)` | gives `Length` bytes of the image |
| 1 | `Erase(Address, Length)` | erases. `Address` and `Length` must be multiples of 0x1000, or the result is `EFI_UNSUPPORTED` |
| 2 | `Write(Address, Length, Buffer)` | programs `Length` bytes, with no erase |
| 3 | `EraseWrite(Address, Length, Buffer)` | erases and programs, per 0x1000 bytes |
| 4 | `Open()` | starts a session |
| 5 | `Close()` | ends a session |

`Address` is the address of the flash in the ROM window of the processor. The first byte of the image is at
`0xFF000000`. The functions mask the two legacy interrupt controllers for the time of each call.

The six functions come from the code of the stock module, read by one reader. On unit A, the protocol was there
when the stage ran, and `Read` gave the whole image: outside the variable store, each byte equals the programmer
read of the same unit ([M973](../facts/hardware.md#m973)). `Erase`, `Write` and `EraseWrite` are not measured.

### The image

| Offset range | Content | The bytes are the unit's own |
|---|---|---|
| `0x000000-0x01FFFF` | a firmware volume with the NVAR variable store | yes |
| `0x020000-0xADFFFF` | the PSP directories and entries | no. Measured on one unit |
| `0xAE0000-0xDFFFFF` | the outer firmware volume with the LZMA-compressed nested volume of the DXE and SMM modules | no. Measured on one unit |
| `0xE00000-0xFFFFFF` | the outer firmware volume with the nested volume of the PEI modules, and the reset vector | no. Measured on one unit |

Each volume starts with a pad file for the extended header of the volume. In the stock image P3.00, the store volume
is 0x20000 bytes and full. The outer volume is 0x320000 bytes: its image file ends at `0x141E7A` of the volume, and
the space behind it is free. The PEI volume is 0x200000 bytes with one image file to its end.

The image of unit A is not the stock image: on 2026-11-25 its image file of the outer volume ends at `0xC23647`,
and the 1952185 bytes behind it read FF to the end of the volume. The first whole free sector is at `0xC24000`.

The erase block is 0x1000 bytes. P6 L1 changes 324 blocks in the outer volume of a stock P3.00 image and nothing
else.

The name of a base is the SHA-256 of the image in which the variable store reads as zeros. A unit and the image
file of its firmware have the same name when the variable store is their only difference. The stock image P3.00
has the name `62411d52...`. The image of unit A on 2026-11-25 has the name `0817b854...`.

## The job in the stage

The job is a module of the stage, and not a second UEFI application, for three reasons. The Installer puts the
stage on the EFI system partition of every unit. The stage has the console, the log, the volume and the board
identity. With Secure Boot on, a second application is a second signed image.

The module is the first in the order of the modules. Without the load option `flash=<job>` the module does nothing
and says nothing. `flash=0` turns the module off. With `probe=1`, the module that is off says whether the firmware
has the flash protocol.

### Job read

1. The module finds the flash protocol. The firmware does not have it: the module says so and writes no image.
2. The module allocates 16 MiB of pages and reads the image in pieces of 64 KiB.
3. The module computes the digest of the image, the name of the base and the digest of the variable store. It reads
   the three volumes.
4. The module writes `bc250-flash.dump` and `bc250-flash.result` beside the stage image, and frees the pages.
5. With `flash.reset=1`, the module writes the log and restarts the board.

The job writes nothing to the flash. An image file of an earlier job does not stay when a job fails.

`flash.reset=1` is for a start that must not go on to Windows: a measurement with the other modules off, and each
job that writes. Use `log=line` with it, so that the log is on the volume before the restart.

Measured on unit A (2026-11-25, one start, Secure Boot on in the Windows before and after): the job read
16777216 bytes and wrote the three files. The reset restarted the board, and Windows started on the persistent
route of the stick about 24 s after the time of the files ([M974](../facts/hardware.md#m974)).

The result file has one `key value` line for each item and a line feed at the end of each line. Its last line is the
status line.

| Key | Value |
|---|---|
| `format` | 1 |
| `job` | the name of the job |
| `sha256`, `base`, `store` | the three digests, in lower-case hex |
| `volume <name>` | the bytes and the used end of the store, the outer and the PEI volume. `absent` when the header is not sound |
| `stream <name>` | where the compressed stream of a volume starts, its bytes and the plain bytes. `absent` when the first file is not an LZMA image file |
| `image-bytes` | the bytes that the job read |
| `dump` | `yes` when the image file is complete |
| `read-failed-at` | the image offset where a read failed |
| `error` | the reason of a failure |
| `status` | `ok` or `failed`. This line is the last |

### Job test

The job is for the measurement M2. It calls each function of the protocol on one sector S that the load option
`flash.sector=<hex>` names, and on the next sector N. The module reads the image first. Both sectors must be behind
the last file of the outer volume: for any other sector the job writes nothing.

| Step | Call | The read after it must show |
|---|---|---|
| 1 | `Open` | |
| 2 | `Read` S | each byte `FF`: S was erased before the test |
| 3 | `Erase` S | |
| 4 | `Read` S | each byte `FF` |
| 5 | `Write` S | |
| 6 | `Read` S | the first pattern |
| 7 | `EraseWrite` S | |
| 8 | `Read` S | the second pattern, the complement of the first |
| 9 | `Write` N | |
| 10 | `Read` N | the third pattern |
| 11 | `Erase` S | |
| 12 | `Read` S | each byte `FF` |
| 13 | `Read` N | the third pattern: the erase of S left N |
| 14 | `Write` 4 bytes of S at `0x28` | |
| 15 | `Read` S | each byte `FF`, but for the four bytes `_FVH` |
| 16 | `Erase` S | |
| 17 | `Read` S | each byte `FF` |
| 18 | `Erase` N | |
| 19 | `Read` N | each byte `FF` |
| 20 | `Close` | |
| 21 | `Read` S | each byte `FF`, outside the session |
| 22 | `Read` N | each byte `FF`, outside the session |

Step 13 measures that an erase of one sector leaves the next sector. Step 14 is the write of the signature of a
volume header into an erased first sector. The job apply needs that write after the first sector of the outer
volume ("The signature of a volume").

When a call fails, or a read shows other bytes, the job goes on only with the steps that erase both sectors again
and end the session: from step 5 on, these are the steps 16 to 22. Before step 5, these are the steps 20 to 22. A
failed `Open` ends the job.

After the test the module reads the image again. The result file has the keys of the job `read` for the image
before the test, then these keys:

| Key | Value |
|---|---|
| `sector` | the image offset of S |
| `sector-place`, `next-sector-place` | `free`, or why S or N is not free |
| `test <number> <call>` | `status`, the status of the call. `ms`, its time in milliseconds, or `?` when the processor has no constant time stamp counter. Then `as-expected` or `not-as-expected` |
| `error` | `test step <number> not as expected` |
| `sha256-after`, `base-after`, `store-after` | the three digests of the image after the test |
| `diff-in-sectors`, `diff-in-store`, `diff-elsewhere` | the bytes that changed between the two reads, in the two sectors, in the variable store and in the rest of the image |

The job ends with `status ok` when each step was as expected, no byte outside the two sectors and the variable
store changed, and the name of the base is the same before and after the test. The image after the test goes into
the image file.

### Job apply (the design)

The job does not exist in the code. These are the steps that the measurements must make safe.

1. The Installer puts a package beside the stage image and starts the stage with `flash=apply`.
2. The stage reads the image, as in the job `read`, and finds the name of the base. The package does not name the
   base: the stage writes nothing and says so.
3. The stage verifies the signature of the package.
4. For each block of 0x1000 bytes in the package: when the block on the flash is different, the stage gives the block
   to `EraseWrite`. Then it reads the block and compares it. The first block of the outer volume comes last, and the
   stage programs `_FVH` after it: see "The signature of a volume" below.
5. The stage reads the whole image again and compares the name of the base with the target in the package.
6. The stage writes the result file and restarts the board.

A package names one base and one target, and holds the blocks that differ between them. A power loss in the time of
the write needs a programmer. This is a rule of the product (2026-11-25): a recovery without a programmer is not a
condition.

#### The signature of a volume

A call that starts at the first byte of a volume that the protocol knows has a second effect. The protocol
invalidates `_FVH` in the flash, takes `_FVH` out of the buffer of the caller, and programs it back only when a
later call ends at the end of that volume. A job that gives the first block of the outer volume in a call of one
block must program `_FVH` itself, as its last write. A write test must use a sector that is not the first of a
volume. Which volumes the protocol knows on unit A is not read.

### What the job never writes

- The variable store.
- A unit whose image is not a base that the package names.
- A package without a sound signature.
- A byte that the package does not name.
- In the job `test`: any sector that is not behind the last file of the outer volume.

### The second way: a USB stick

The same stage with the same load option starts from a USB stick that the Installer makes. This way is for a unit
whose EFI system partition has no room or takes no write.

## The measurements on unit A

| Step | What | State | Writes to the flash |
|---|---|---|---|
| M1 | the job `read`. The dump is compared with a read through the programmer | MEASURED 2026-11-25: 0 bytes differ outside the variable store ([M973](../facts/hardware.md#m973)) | none |
| M2 | the job `test` on one sector of the free space of the outer volume | the job is built. Not measured | one sector |
| M3 | the P6 image through the job `apply` on a unit with the stock firmware. The result is compared with the image | not measured | the blocks of P6 L1 |

Each step needs a written plan, a backup through the programmer and one operator.

## Open questions

1. Does the protocol write when Secure Boot is on? `Read` works with Secure Boot on in the starts before and after.
2. Does the recovery path of the stock firmware (`Recovery`) restore an image from a USB stick? A unit with a
   half-written outer volume would then need no programmer.
3. Has another unit bytes of its own outside the variable store? Measured on one unit only.
4. How does the Installer start the job? A boot entry can carry no load option that the Installer writes.
5. The time of the write. The estimate is one minute for the 324 blocks of P6 L1.
6. Which volumes does the protocol know on a unit? The `_FVH` bookkeeping of "The signature of a volume" comes
   from the code. Step M3 measures it on the outer volume.
