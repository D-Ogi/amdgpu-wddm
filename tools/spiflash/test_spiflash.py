"""Host tests of the CH341A transport against a simulated programmer and flash (no USB)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ch341a  # noqa: E402


class FakeFlash:
    """Decodes the CH341A stream like the device does and answers like an SPI flash."""

    def __init__(self, image, sr=0x40):
        self.image, self.sr = image, sr
        self.outs, self.replies = [], bytearray()
        self.cs_low, self.cmd = False, bytearray()

    def out(self, data):
        self.outs.append(bytes(data))
        for k in range(0, len(data), ch341a.PKT):  # the device works in 32-byte packets
            pkt = data[k:k + ch341a.PKT]
            if pkt[0] == 0xAB:  # UIO stream, ends at 0x20
                for b in pkt[1:pkt.index(0x20)]:
                    if b & 0xC0 == 0x80:  # UIO_STM_OUT: bit 0 is CS
                        low = (b & 0x01) == 0
                        if low and not self.cs_low:
                            self.cmd = bytearray()
                        self.cs_low = low
            elif pkt[0] == 0xA8:
                if not self.cs_low:
                    raise AssertionError('SPI data with CS high')
                for b in pkt[1:]:
                    self.cmd.append(ch341a.rev(b))
                    self.replies.append(ch341a.rev(self._answer()))
            elif pkt[0] != 0xAA:
                raise AssertionError(f'unexpected packet type {pkt[0]:#x}')

    def _answer(self):
        c, n = self.cmd, len(self.cmd) - 1
        if n == 0:
            return 0x00  # command phase: SO high-Z, the idle level of this simulation
        if c[0] == 0x9F:
            return [0xC2, 0x20, 0x18][(n - 1) % 3]
        if c[0] == 0x05:
            return self.sr
        if c[0] == 0x03 and n >= 4:
            addr = c[1] << 16 | c[2] << 8 | c[3]
            return self.image[(addr + n - 4) % len(self.image)]
        return 0x00

    def inn(self, want, timeout_ms=0):
        got, self.replies = bytes(self.replies[:want]), self.replies[want:]
        return got

    def close(self):
        pass


class TransportTest(unittest.TestCase):
    def setUp(self):
        self.image = bytes((i * 7 + 3) & 0xFF for i in range(8192))
        self.flash = FakeFlash(self.image)
        self.dev = ch341a.CH341A(self.flash)

    def test_write_opcodes_never_reach_usb(self):
        before = len(self.flash.outs)
        for op in (0x06, 0x01, 0x02, 0x20, 0x52, 0xD8, 0x60, 0xC7, 0x2F, 0x2C, 0x17, 0x28, 0xE3, 0xE1, 0x68):
            with self.assertRaises(PermissionError):
                self.dev.xfer([op, 0, 0, 0], 1)
        self.assertEqual(len(self.flash.outs), before)

    def test_rdid_and_status(self):
        self.assertEqual(self.dev.xfer([0x9F], 3), bytes([0xC2, 0x20, 0x18]))
        self.assertEqual(self.dev.xfer([0x05], 1), bytes([0x40]))
        self.assertEqual(self.dev.last_command_phase, b'\x00')

    def test_read_across_packets(self):
        self.assertEqual(self.dev.read(0x100, 200), self.image[0x100:0x100 + 200])

    def test_one_packet_per_out_transfer(self):
        self.dev.read(0, 100)
        data_outs = [o for o in self.flash.outs if 0xA8 in o]
        for o in data_outs:
            start = ch341a.PKT if o.startswith(ch341a.CS_PACKET) else 0
            self.assertEqual(o[start], 0xA8)
            self.assertLessEqual(len(o) - start, ch341a.PKT)

    def test_bytes_are_bit_reversed_on_the_wire(self):
        self.dev.xfer([0x9F], 3)
        first = next(o for o in self.flash.outs if o.startswith(ch341a.CS_PACKET))
        self.assertEqual(first[ch341a.PKT + 1], ch341a.rev(0x9F))
        self.assertEqual(ch341a.rev(0x9F), 0xF9)


if __name__ == '__main__':
    unittest.main()
