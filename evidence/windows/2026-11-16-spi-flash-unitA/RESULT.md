# Unit A BIOS SPI flash: image identity and chip registers (2026-11-16)

Read with a CH341A programmer (3.3 V signals) on the J4004 header, unit A in S5 (PSU on, clean shutdown,
13.7 W at the wall). Tools: `tools/spiflash` (flashrom 1.2 for the image, `spi_regs.py` for the registers).
The image itself is not in this directory: it holds the UEFI variable store and AMD and AMI firmware. It is
kept outside the repository (`firmware/bios/spi-backup-unitA-20261116T003730Z/` in the workspace and on the
NAS). Its SHA-256 is in `SHA256SUMS`.

## Image

- 3 flashrom reads of 16777216 bytes, all with SHA-256 `09fb260f...2693103` (`reads.txt`, `SHA256SUMS`).
- Compared with the stock P3.00 image `BC250_3.00.ROM` (`diff-vs-stock-P3.00.json`, tool `img_diff.py`):
  99450 bytes differ, all in 0x00044A-0x01FEA0, which is the UEFI variable store region. The rest of the
  16 MiB is equal to P3.00.
- PSP directories and both SMU entries (type 8 and type 18, version 0.58.6.0) verify against the stock key
  `8b8df41e2a6649beb8f232cef3133650`, and all 8 single-byte negative controls reject
  (`offline-inspect.json`, tool `tools/spi-smu/offline.py` of branch `t39/spi-smu`).
- Result: unit A runs BIOS 3.00 with SMU firmware 0.58.6.0. The P5.00 image carries SMU 0.58.7.1.

## flashrom read and the status register

flashrom 1.2 calls the chip unlock hook before a read. The probe-only run after the reads
(`probe-verbose-after-reads.txt`) shows status register 0x40. The value before the first read is not known.
If block-protect bits were set, the reads cleared them (status register only).

## Chip registers

Three runs of `spi_regs.py` (read-class opcodes only, write enable latch 0 in every run):

| Run | MISO idle level | Directory |
|-----|-----------------|-----------|
| 00:50Z | 00 (no pull-up) | `chip-regs-20261116T005045Z` (first version: 256-byte SFDP, no extra registers) |
| 00:57Z | 00 (no pull-up) | `chip-regs-20261116T005707Z` |
| 01:16Z | FF (10 kOhm CH341A 3.3V to MISO) | `chip-regs-20261116T011621Z-pullup` |

The idle level is measured in the command phase, where the chip does not drive SO (`miso-float-check.txt`,
`miso-float-check-pullup.txt`).

- Same value with both idle levels, so driven by the chip: RDID `C2 20 18`, REMS `C2 17`, RES `17`, status
  register 0x40 (QE set, BP3-BP0 0, SRWD 0, WEL 0), configuration register 0x07, security register 0x00,
  SFDP (`sfdp.bin`, equal in both runs), secured OTP window (`otp-window.bin`, equal in both runs).
- Security register 0x00: WPSEL 0 (block-protect mode), secured OTP not factory-locked (bit 0), not locked
  down (LDSO, bit 1), no fail or suspend flags.
- Secured OTP (ENSO, READ 0-1023, EXSO): the 512-byte OTP is all FF, including the 16-byte factory ESN field.
  The OTP window differs from the main array (the first 1024 bytes of the image hold 65 FF bytes). After EXSO,
  READ at 0 equals the image again.
- Equal to the idle level in both runs (00, then FF), so the chip does not answer: RDLR, RDSPBLK, RDFBR,
  RDPASS, RDSPB, RDDPB. Datasheet rev 1.2 section 9-30 enables the lock, password and SPB/DPB commands only
  after WPSEL=1. It does not gate RDFBR, so its silence is not explained by rev 1.2.
- SFDP 1.6 with 3 parameter headers: JEDEC basic (16 dwords at 0x30), Macronix (4 dwords at 0x110, the same
  values as rev 1.2 Table 12), 4-byte address instructions (2 dwords at 0xC0, no 4-byte opcodes). Datasheet
  rev 1.2 Table 10 shows SFDP 1.0 with 2 headers, so this die is newer than rev 1.2 describes.

## Limits

- The 00:57Z run of `spi_regs.py` used the workspace script `scratch/t39/spi-tools/spi-regs.py`. Its SHA-256
  is in each `spi-regs.json`. `tools/spiflash` is the same sequence split into transport and sequence.
- The RDSPB/RDDPB address format (3 bytes and a dummy, against the 32-bit address of the figures) is not
  proven. It does not matter while the chip does not answer.
- No datasheet revision newer than 1.2 was found, so the full command set of this die is not known.
