# Unit A BIOS SPI flash read through a Pico H with serprog firmware (2026-11-18)

Read on the J4004 header with a Raspberry Pi Pico H that runs the libreboot 26.11rev1 `serprog_pico.uf2`
(pico-serprog), unit A in S5 (PSU on, clean shutdown, 13.6 W at the wall). Host: Windows, flashrom 1.2,
programmer string `serprog:dev=COM6:115200,spispeed=2M`. Pins: CS GP5, SCK GP2, MOSI GP3, MISO GP4
(`tools/spiflash/j4004-pico-h-wiring.svg`). The image is not in this directory: it holds the UEFI variable
store and AMD and AMI firmware. Only read operations were made. No write through the Pico was tried.

## Result

- Three full reads of 16777216 bytes have the SHA-256 `0b0dfd52...fa0ed28c` (`SHA256SUMS`): two reads of
  `spi_read.ps1` at 19:48Z (`reads.txt`) and one earlier read of a verbose flashrom run at 19:46Z.
- The probe reports the firmware name `pico-serprog`, an SPI clock of 1953125 Hz for the requested 2 MHz, RDID
  `C2 20 18` and status register 0x40 (`probe-verbose.txt`). This is the status value of
  [M966](../../../docs/facts/hardware.md#m966).
- Compared with the last CH341A read of the same chip (16:31Z of the same day, the unit was started and shut
  down between the two reads): 299 bytes differ, in 0x001330-0x001332 and 0x01BEC3-0x01C0E5. Both ranges are
  in the UEFI variable store (0x000000-0x01FFFF). The other 16646144 bytes are equal.

## DTR on the serial port

- With the default port settings, the probe passes and the first array read fails: flashrom asks for 65536
  bytes in one serprog SPI operation and reports "invalid response 0x00 from device (to command 0x13)"
  (`read-dtr-off.err.txt`, `read-dtr-off.log.txt`). Three runs gave this result, with two USB cables.
- After `mode COM6: dtr=on rts=on` (`port-setup.txt`) each full read completes, in about 120 s.
- flashrom 1.2 sets only baud, data bits, parity and stop bits on Windows (`serial.c`, `serialport_config`).
  It does not assert DTR.

## Limits

- The cause inside the firmware is not established. A stale send buffer of the USB CDC stack while DTR is off
  fits the result, but the TinyUSB source was not read.
- n = 1 unit, one Pico, one host. Signal levels and edges at the header were not measured.
- A program, an erase and a restore through this programmer were not tried.
- The release binary of the firmware was used. That it was built from the published source was not checked.
