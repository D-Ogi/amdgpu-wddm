# tools/spiflash

Reads the BIOS SPI flash of the BC-250 through the J4004 header with a CH341A programmer: a full image with
flashrom, and the chip registers, SFDP, protection bits and secured OTP with an own raw-SPI transport. No
tool here can write the flash. Measured state of unit A:
[M965](../../docs/facts/hardware.md#m965), [M966](../../docs/facts/hardware.md#m966).

| File | Use |
|------|-----|
| `spi_read.ps1` | Image backup: probe with `-V` (records the status register), then 3 flashrom reads, equal hashes, optional NAS copy |
| `ch341a.py` | CH341A raw-SPI transport (libusb through ctypes, no pip package), read-class opcodes only |
| `spi_regs.py` | Registers, SFDP, lock, fast boot, password, SPB/DPB, secured OTP. RDID and a READ against the backup gate it |
| `miso_float_check.py` | Negative control: the MISO level while the chip does not drive SO |
| `test_spiflash.py` | Host tests against a simulated programmer: `python -m unittest discover -s tools/spiflash` |
| `winusb/` | WinUSB binding for the CH341A (inbox driver, own INF, local self-signed catalog) |
| `j4004-wiring.svg` | Wiring diagram |

## The flash and J4004 on unit A

- Chip `BIOS_A1`: Macronix MX25L12873F, 3 V, 16 MiB, JEDEC ID `C2 20 18`. flashrom 1.2 has no entry for it.
  Use `-c "MX25L12835F/MX25L12845E/MX25L12865E"` (same ID, same read commands).
- J4004, top view, the white triangle marks pin 1:

  ```
  top:     5 GND    6 CLK    7 MOSI   8 (strap, leave open)
  bottom:  1 VCC    2 CS#    3 MISO   4 (no pin)
  ```

  The layout is from project-ariel `bc250_manual.md`. It agrees with unit A (no pin at position 4) and gave
  a correct read. A table that puts SCLK on pin 4 is wrong for this board.
- WP# and HOLD# are tied high on the board. Pin 1 (VCC) stays open: the board powers the flash from the
  standby rail.

![J4004 wiring](j4004-wiring.svg)

## Procedure

1. Install the WinUSB binding once (elevated): `pwsh -File tools/spiflash/winusb/install_winusb.ps1 -WorkDir <dir outside the repo>`.
2. CH341A jumper on pins 1-2 (SPI mode, USB ID `1a86:5512`). On pins 2-3 the device is a UART (`1a86:5523`) and
   flashrom reports "Couldn't open device 1a86:5512".
3. The CH341A must give 3.3 V signals. The common black board gives 5 V on its data lines until it is modified
   (pin 28 lifted and connected to the 3.3 V regulator output).
4. Shut down unit A cleanly and leave the PSU on (S5, about 14 W at the wall). The flash is then powered and the
   chipset SPI controller is idle.
5. Connect the programmer to USB first, then the 5 wires (CS, MISO, GND, CLK, MOSI to J4004 pins 2, 3, 5, 6, 7).
   An unpowered CH341A gets power through its inputs from the board.
6. Backup: `pwsh -File tools/spiflash/spi_read.ps1 -OutRoot <firmware dir> -NasRoot <NAS dir>`.
7. Registers: `python -I tools/spiflash/spi_regs.py <read1.bin> <new dir>`.
8. Remove the wires, then do an AC cycle and a boot test.

## Traps

- **flashrom `-r` is not read-only.** flashrom 1.2 `prepare_flash_access()` calls the chip unlock hook before a
  read. For this chip definition the hook sends WREN+WRSR when a block-protect bit is set. `spi_read.ps1`
  records the status register with a probe-only `-V` run first.
- **Undriven MISO.** Without a pull-up, the CH341A read `00` on unit A while the chip did not drive SO. With
  10 kOhm from the CH341A 3.3V pin to MISO it read `FF`. A reply equal to the idle level is "no answer", not a
  register value. `miso_float_check.py` shows the idle level, and `spi_regs.py` runs without and with the
  pull-up classify each reply.
- **The CH341A stalls with queued packets.** It stops taking OUT data when about 4 SPI packets of reply wait
  unread (bulk OUT timeout). `ch341a.py` sends one packet per transfer and reads the reply before the next.
- **Fast boot register.** RDFBR (`16h`) does not answer on unit A, although datasheet rev 1.2 does not gate it.
  The die reports SFDP 1.6, newer than rev 1.2 describes, so its command set can differ.
- **The image is not for a repository.** It holds the UEFI variable store and AMD and AMI firmware. It stays in
  the workspace `firmware\` directory and on the NAS. The facts carry only hashes, versions and register values.

PROVENANCE: CH341A vendor protocol constants as used by flashrom 1.2 `ch341a_spi.c` (GPL-2.0-or-later),
reference only, no code copied. MX25L12873F datasheet rev 1.2 (Macronix) for the opcodes.
