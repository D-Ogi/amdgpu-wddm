"""Negative control: the MISO level the CH341A reads while the flash does not drive SO.

During the command phase of a transaction the flash keeps SO in high impedance, so the bytes read there are
the idle level of the line. A register reply equal to that level is not a register value. Run once without
and once with a 10 kOhm pull-up from the CH341A 3.3V pin to MISO: a reply that follows the idle level is
"no answer", a reply that stays the same is driven by the chip.

Usage: python -I tools/spiflash/miso_float_check.py [--libusb DLL]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ch341a  # noqa: E402

PROBES = ((0x9F, 3), (0x05, 1), (0x2D, 2), (0x16, 4))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--libusb', type=Path)
    dev = ch341a.open_programmer(ap.parse_args().libusb)
    try:
        for op, n in PROBES:
            data = dev.xfer([op], n)
            print(f'{ch341a.READ_OPCODES[op]:7s} command-phase {dev.last_command_phase.hex()} data {data.hex()}')
        data = dev.read(0x900000, 4)
        print(f'READ    command-phase {dev.last_command_phase.hex()} data {data.hex()}')
    finally:
        dev.close()


if __name__ == '__main__':
    main()
