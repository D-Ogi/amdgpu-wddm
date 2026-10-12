import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('decoder', Path(__file__).with_name('decode-dispatch-journal.py'))
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


def fixture(slot_index=0, identity=1, state=4):
    raw = bytearray(d.TOTAL)
    struct.pack_into('<6I7Q', raw, 0, 0x4a504948, 1, d.TOTAL, d.SLOT_BYTES, 32,
                     (slot_index + 1) % 32, identity, 1, 1, 0, 0, 0, 0)
    record = bytearray(152)
    struct.pack_into('<4I3Q8I4IQ', record, 0, 152, 2, 8, 1, 0x1000, 0x2000, 0x3000,
                     7, 3, 2, 64, 1, 1, 16, 0, 96, 104, 112, 0, 42)
    record[96:98] = b'k\0'
    struct.pack_into('<Q', record, 104, 0x4010)
    struct.pack_into('<4I3Q', record, 112, 0, 0, 1, 1, 0x4010, 0x4000, 0x100)
    upload = bytearray(80) + record
    struct.pack_into('<8I4Q4I', upload, 0, 0x30353242, 35, 0xffffffff, 0, 0, 1,
                     len(upload), 1, 0x5000, 0x6000, 7, 0, 0, 0, 0, 0)
    start = d.HEADER + slot_index * d.SLOT_BYTES
    struct.pack_into('<14Q8I', raw, start, 2, identity, 3, 4, 5, 6, 7, 8, 9, 10,
                     0x5000, 0x6000, 7, 0x7000, state, len(upload), 5876, 900, 800, 5, 256, 0)
    raw[start + d.SLOT_PREFIX:start + d.SLOT_PREFIX + len(upload)] = upload
    return raw, start


class DecoderTests(unittest.TestCase):
    def test_complete(self):
        raw, _ = fixture()
        got = d.decode(raw)
        self.assertFalse(got['warnings'])
        slot = got['slots'][0]
        self.assertEqual((slot['sequence'], slot['fence_value'], slot['process_id']), (800, 7, 5876))
        self.assertEqual(slot['records'][0]['symbol'], 'k')
        self.assertEqual(slot['records'][0]['grid'], [7, 3, 2])
        self.assertEqual(slot['records'][0]['bindings'][0]['classification'], 'known')

    def test_empty_symbol(self):
        raw, start = fixture()
        at = start + d.SLOT_PREFIX + 80
        struct.pack_into('<I', raw, at + 4, 1)
        raw[at + 96] = 0
        self.assertIn('record limits', d.decode(raw)['slots'][0]['error'])

    def test_missing_kernarg_address(self):
        raw, start = fixture()
        struct.pack_into('<Q', raw, start + d.SLOT_PREFIX + 80 + 32, 0)
        self.assertIn('kernarg VA', d.decode(raw)['slots'][0]['error'])

    def test_upload_alignment(self):
        for offset in (32, 40):
            with self.subTest(offset=offset):
                raw, start = fixture()
                raw[start + d.SLOT_PREFIX + offset] |= 1
                self.assertIn('alignment', d.decode(raw)['slots'][0]['error'])

    def test_times_and_counters(self):
        raw, _ = fixture()
        struct.pack_into('<7Q', raw, 24, 1, 2, 3, 4, 5, 6, 7)
        got = d.decode(raw)
        self.assertEqual([got[k] for k in ('next_context', 'uploads', 'overwritten', 'refused', 'completed', 'cancelled')], [2, 3, 4, 5, 6, 7])
        self.assertEqual([got['slots'][0][k] for k in ('uploaded_100ns', 'bound_100ns', 'submitted_100ns', 'finished_100ns')], [7, 8, 9, 10])

    def test_replay_fields(self):
        raw, start = fixture(state=2)
        struct.pack_into('<BBH', raw, start + 144, 0, 1, 2)
        slot = d.decode(raw)['slots'][0]
        self.assertTrue(slot['valid'])
        self.assertTrue(slot['replay_pending'])
        self.assertEqual(slot['attempts'], 2)
        self.assertEqual(slot['sequence'], 800)  # Prior attempt, not new execution.
        raw[start + 148] = 1
        self.assertIn('reserved', d.decode(raw)['slots'][0]['error'])

    def test_replay_flag(self):
        raw, start = fixture()
        raw[start + 145] = 2
        self.assertIn('reserved', d.decode(raw)['slots'][0]['error'])

    def test_wrap(self):
        raw, _ = fixture(31, 33)
        other, start = fixture(0, 34)
        raw[start:start + d.SLOT_BYTES] = other[start:start + d.SLOT_BYTES]
        struct.pack_into('<I', raw, 20, 1)
        struct.pack_into('<Q', raw, 24, 34)
        self.assertEqual([x['id'] for x in d.decode(raw)['slots']], [33, 34])

    def test_cancelled_not_completed(self):
        raw, _ = fixture(state=7)
        slot = d.decode(raw)['slots'][0]
        self.assertTrue(slot['valid'])
        self.assertEqual(slot['state'], 'cancelled')

    def test_odd_generation(self):
        raw, start = fixture()
        struct.pack_into('<Q', raw, start, 3)
        slot = d.decode(raw)['slots'][0]
        self.assertFalse(slot['valid'])
        self.assertNotIn('records', slot)
        self.assertIn('torn', slot['error'])

    def test_corrupt_binding(self):
        raw, start = fixture()
        raw[start + d.SLOT_PREFIX + 80 + 112 + 16] ^= 1
        self.assertIn('binding value', d.decode(raw)['slots'][0]['error'])

    def test_missing(self):
        raw, _ = fixture()
        with self.assertRaisesRegex(ValueError, 'missing'):
            d.decode(raw[:-1])

    def test_wrong_layout(self):
        raw, _ = fixture()
        struct.pack_into('<I', raw, 4, 2)
        with self.assertRaisesRegex(ValueError, 'layout'):
            d.decode(raw)

    def test_nonzero_padding(self):
        raw, start = fixture()
        raw[start + d.SLOT_PREFIX + 80 + 99] = 1
        self.assertIn('padding', d.decode(raw)['slots'][0]['error'])

    def test_slot_binding(self):
        raw, start = fixture()
        struct.pack_into('<Q', raw, start + 10 * 8, 0xdead)
        self.assertIn('slot/upload', d.decode(raw)['slots'][0]['error'])


if __name__ == '__main__':
    unittest.main()
