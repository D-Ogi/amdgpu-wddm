"""Raw SPI through a CH341A programmer (USB 1a86:5512, WinUSB or libusb driver), read-class opcodes only.

The transport refuses every opcode that is not on READ_OPCODES before any byte goes to USB. WREN (06h) is
not on the list, so the write enable latch of the flash stays 0 and the chip rejects every program, erase
and register write even if a byte is corrupted on the way.

Wire protocol (CH341A vendor protocol as flashrom 1.2 ch341a_spi.c drives it; reference only):
- bulk OUT 0x02, bulk IN 0x82, interface 0;
- the CH341A shifts LSB first, so every byte is bit-reversed in both directions;
- CS through the UIO stream (0xAB): OUT|0x37 = CS high, OUT|0x36 = CS low, DIR 0x40|mask, END 0x20;
- data through SPI stream packets: 0xA8 + up to 31 bytes; the device returns one byte per byte sent;
- the device stops taking OUT data when about 4 packets of reply wait unread, so one packet goes per OUT
  transfer and its reply is read before the next.
CS goes high after every command: ENSO/EXSO and the register commands take effect on the CS rising edge.
"""
import ctypes
import os
from pathlib import Path

VID, PID = 0x1A86, 0x5512
EP_OUT, EP_IN = 0x02, 0x82
TIMEOUT_MS = 1000
PKT = 32

# Read-class opcodes of the MX25L12873F datasheet rev 1.2, Table 5. Nothing else can leave the transport.
READ_OPCODES = {
    0x9F: 'RDID', 0x05: 'RDSR', 0x15: 'RDCR', 0x2B: 'RDSCUR', 0x5A: 'RDSFDP',
    0x03: 'READ', 0xB1: 'ENSO', 0xC1: 'EXSO',
    0x90: 'REMS', 0xAB: 'RES', 0x2D: 'RDLR', 0xA7: 'RDSPBLK', 0x16: 'RDFBR', 0x27: 'RDPASS',
    0xE2: 'RDSPB', 0xE0: 'RDDPB',
}

CS_PACKET = bytes([0xAB, 0x80 | 0x37, 0x80 | 0x37, 0x80 | 0x37, 0x80 | 0x36, 0x20]).ljust(PKT, b'\x00')
CS_HIGH = bytes([0xAB, 0x80 | 0x37, 0x80 | 0x37, 0x20])
STREAM_CONFIG = bytes([0xAA, 0x60 | 0x01, 0x00])           # 100 kHz class, single SPI
PINS_ON = bytes([0xAB, 0x80 | 0x37, 0x40 | 0x3F, 0x20])     # CS high, outputs enabled
PINS_OFF = bytes([0xAB, 0x80 | 0x37, 0x40 | 0x00, 0x20])    # CS high, outputs off


def rev(b):
    return int(f'{b:08b}'[::-1], 2)


def default_libusb():
    """libusb-1.0.dll: BC250_LIBUSB, else the MSYS2 tree under BC250_ROOT used for flashrom."""
    if os.environ.get('BC250_LIBUSB'):
        return Path(os.environ['BC250_LIBUSB'])
    root = Path(os.environ.get('BC250_ROOT', Path(__file__).resolve().parents[3]))
    return root / 'scratch' / 't39' / 'spi-tools' / 'root' / 'ucrt64' / 'bin' / 'libusb-1.0.dll'


class LibusbBackend:
    """Bulk transfers to the CH341A through libusb-1.0 (ctypes, no pip package)."""

    def __init__(self, dll=None):
        self.lib = L = ctypes.CDLL(str(dll or default_libusb()))
        L.libusb_init.argtypes = [ctypes.c_void_p]
        L.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        L.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
        L.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.libusb_close.argtypes = [ctypes.c_void_p]
        L.libusb_exit.argtypes = [ctypes.c_void_p]
        L.libusb_bulk_transfer.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_int,
                                           ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
        if L.libusb_init(None) != 0:
            raise RuntimeError('libusb_init failed')
        self.h = L.libusb_open_device_with_vid_pid(None, VID, PID)
        if not self.h:
            L.libusb_exit(None)
            raise RuntimeError('cannot open 1a86:5512 (jumper on 1-2 for SPI mode? WinUSB bound?)')
        rc = L.libusb_claim_interface(self.h, 0)
        if rc != 0:
            self._close()
            raise RuntimeError(f'claim interface 0 failed ({rc})')

    def out(self, data):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        n = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, EP_OUT, buf, len(data), ctypes.byref(n), TIMEOUT_MS)
        if rc != 0 or n.value != len(data):
            raise RuntimeError(f'bulk OUT failed rc={rc} sent={n.value}/{len(data)}')

    def inn(self, want, timeout_ms=TIMEOUT_MS):
        """One bulk IN transfer of up to want bytes; b'' on timeout."""
        buf = ctypes.create_string_buffer(want)
        n = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(self.h, EP_IN, buf, want, ctypes.byref(n), timeout_ms)
        return buf.raw[:n.value] if rc == 0 else b''

    def _close(self):
        if self.h:
            self.lib.libusb_close(self.h)
            self.h = None
        self.lib.libusb_exit(None)

    def close(self):
        self.lib.libusb_release_interface(self.h, 0)
        self._close()


class CH341A:
    """SPI transactions on a CH341A backend; read-class opcodes only."""

    def __init__(self, backend):
        self.usb = backend
        self.drained = self._drain()  # replies left over from an aborted run
        self.usb.out(STREAM_CONFIG)
        self.usb.out(PINS_ON)
        self.last_command_phase = b''

    def _drain(self):
        total = 0
        for _ in range(64):
            got = self.usb.inn(PKT, 100)
            if not got:
                break
            total += len(got)
        return total

    def _read_reply(self, count):
        got = bytearray()
        while len(got) < count:
            part = self.usb.inn(min(PKT - 1, count - len(got)))
            if not part:
                raise RuntimeError(f'bulk IN failed, got {len(got)}/{count}')
            got += part
        return bytes(got)

    def xfer(self, write, readcnt):
        """One SPI transaction: CS low, write bytes, read readcnt bytes, CS high."""
        if not write or write[0] not in READ_OPCODES:
            raise PermissionError(f'opcode {write[0] if write else None!r} is not a read-class opcode')
        stream = list(write) + [0xFF] * readcnt
        reply = bytearray()
        pending = CS_PACKET
        for i in range(0, len(stream), PKT - 1):
            chunk = stream[i:i + PKT - 1]
            self.usb.out(pending + bytes([0xA8]) + bytes(rev(b) for b in chunk))
            pending = b''
            reply += self._read_reply(len(chunk))
        self.usb.out(CS_HIGH)
        self.last_command_phase = bytes(rev(b) for b in reply[:len(write)])  # SO is high-Z here
        return bytes(rev(b) for b in reply[len(write):])

    def read(self, addr, n):
        return self.xfer([0x03, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF], n)

    def close(self):
        try:
            self.usb.out(PINS_OFF)
        finally:
            self.usb.close()


def open_programmer(dll=None):
    return CH341A(LibusbBackend(dll))
