import unittest
from types import SimpleNamespace
from offline import digest, diff_ranges, patch, rebuild, rebuild_parsed, directory_coverage


class OfflineTests(unittest.TestCase):
    def setUp(self):
        self.image = bytes(range(32))
        self.entry = {'body_range': [8, 24]}
        self.edit = {'offset': 10, 'before': '0a', 'after': 'ff'}

    def apply(self, edits=None, identity=None):
        return patch(self.image, self.entry, edits or [self.edit], identity or digest(self.image))

    def test_patch_preserves_surroundings(self):
        result = self.apply()
        self.assertEqual(diff_ranges(self.image, result), [[10, 11]])
        self.assertEqual(self.image[10], 10)

    def test_identity(self):
        with self.assertRaises(ValueError):
            self.apply(identity='0' * 64)

    def test_expected_bytes(self):
        with self.assertRaises(ValueError):
            self.apply([{**self.edit, 'before': '00'}])

    def test_boundary_and_signature_exclusion(self):
        for offset in (-1, 7, 24, 32):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.apply([{**self.edit, 'offset': offset}])

    def test_size_change_and_noop(self):
        for after in ('', 'ffff', '0a'):
            with self.subTest(after=after), self.assertRaises(ValueError):
                self.apply([{**self.edit, 'after': after}])

    def test_overlap(self):
        with self.assertRaises(ValueError):
            self.apply([self.edit, self.edit])

    def test_rebuild_preserves_gaps(self):
        self.assertEqual(rebuild(self.image, [(8, 12), (16, 24)]), self.image)

    def test_rebuild_rejects_invalid_ranges(self):
        for spans in ([(8, 12), (11, 24)], [(-1, 3)], [(4, 33)], [(4, 4)]):
            with self.subTest(spans=spans), self.assertRaises(ValueError):
                rebuild(self.image, spans)

    def test_diff_end_and_separated_ranges(self):
        self.assertEqual(diff_ranges(b'abcde', b'xbcdy'), [[0, 1], [4, 5]])
        with self.assertRaises(ValueError):
            diff_ranges(b'a', b'ab')


class DirectoryCoverageTests(unittest.TestCase):
    def setUp(self):
        self.entry = SimpleNamespace(type=8, size=16, offset=0xff000020,
                                     entry_offset=0, address_mode=0, file_offset=lambda: 32)
        self.directory = SimpleNamespace(entries=[self.entry], files=[], HEADER_SIZE=16,
                                         magic=b'$PSP', address_mode=0, get_address=lambda: 0)

    def parsed(self, size=16, type_=8):
        class File:
            type = type_

            def get_address(self):
                return 32

            def __len__(self):
                return size
        self.directory.files.append(File())

    def record(self):
        return directory_coverage(self.directory, 64)[0]

    def test_dropped_record_remains_visible(self):
        rec = self.record()
        self.assertEqual(rec['coverage_status'], 'missing')
        self.assertEqual(rec['raw_location'], 0xff000020)
        self.assertEqual(rec['normalized_offset'], 32)
        self.assertEqual(rec['record_offset'], 16)
        self.assertEqual(rec['unparsed_ranges'], [[32, 48]])
        self.assertIsNone(rec['parsed_range'])

    def test_complete_and_shorter_file(self):
        self.parsed(8)
        rec = self.record()
        self.assertEqual(rec['coverage_status'], 'partial')
        self.assertEqual(rec['parsed_range'], [32, 40])
        self.assertEqual(rec['unparsed_ranges'], [[40, 48]])
        self.directory.files.clear()
        self.parsed()
        self.assertEqual(self.record()['coverage_status'], 'complete')

    def test_out_of_range_or_empty_record_is_not_missing(self):
        for size in (0, 33):
            self.entry.size = size
            with self.subTest(size=size):
                self.assertEqual(self.record()['coverage_status'], 'unsupported')

    def test_larger_or_ambiguous_parsed_file_is_not_complete(self):
        self.parsed(17)
        self.assertEqual(self.record()['coverage_status'], 'unsupported')
        self.directory.files.clear()
        self.parsed()
        self.parsed()
        self.assertEqual(self.record()['reason'], 'multiple-parsed-files')

    def test_inline_fuse_is_not_an_image_extent(self):
        self.entry.type, self.entry.size = 0x0b, 0xffffffff
        rec = self.record()
        self.assertEqual(rec['coverage_status'], 'special')
        self.assertIsNone(rec['declared_range'])
        self.assertEqual(rec['unparsed_ranges'], [])

    def test_bios_records_and_directory_references_are_not_firmware_extents(self):
        self.directory.magic = b'$BHD'
        self.parsed()
        self.assertEqual(self.record()['coverage_status'], 'unsupported')
        self.assertIsNone(self.record()['declared_range'])
        self.directory.magic, self.entry.type = b'$PSP', 0x40
        self.assertEqual(self.record()['coverage_status'], 'special')

    def test_slot_normalization_is_not_claimed_as_proven(self):
        self.directory.address_mode = self.entry.address_mode = 3
        self.parsed()
        rec = self.record()
        self.assertEqual(rec['normalized_offset'], 32)
        self.assertEqual(rec['coverage_status'], 'unsupported')
        self.assertEqual(rec['reason'], 'slot-relative-offset-not-established')

    def test_normalization_failure_remains_visible(self):
        def bad_offset():
            raise ValueError('unsupported layout')
        self.entry.file_offset = bad_offset
        rec = self.record()
        self.assertEqual(rec['reason'], 'offset-normalization-failed')
        self.assertEqual(rec['normalization_error'], 'ValueError')
        self.assertEqual(rec['raw_location'], 0xff000020)


class ParsedRebuildTests(unittest.TestCase):
    class Component:
        def __init__(self, offset, data):
            self.offset, self.data = offset, data

        def get_address(self):
            return self.offset

        def get_bytes(self):
            return self.data

        def __len__(self):
            return len(self.data)

    class Entry(Component):
        pass

    def setUp(self):
        self.image = b'gapHDRbodySIGtail'
        self.entry = self.Entry(3, b'HDRbodySIG')
        self.entry.type = 8
        self.entry.encrypted = self.entry.compressed = False
        self.entry.header = self.Component(3, b'HDR')
        self.entry.body = self.Component(6, b'body')
        self.entry.signature = self.Component(10, b'SIG')

    def test_round_trip_preserves_opaque_data(self):
        rebuilt, records = rebuild_parsed(self.image, [self.entry])
        self.assertEqual(rebuilt, self.image)
        self.assertEqual([c['name'] for c in records[0]['components']], ['header', 'body', 'signature'])

    def test_extraction_corruption_is_not_hidden_by_original_slices(self):
        self.entry.body.data = b'Body'
        with self.assertRaisesRegex(ValueError, 'differs'):
            rebuild_parsed(self.image, [self.entry])

    def test_component_gap_and_overlap(self):
        for offset in (5, 7):
            self.entry.body.offset = offset
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'non-contiguous'):
                rebuild_parsed(self.image, [self.entry])

    def test_missing_tail(self):
        self.entry.signature.data = b'SI'
        with self.assertRaisesRegex(ValueError, 'cover'):
            rebuild_parsed(self.image, [self.entry])

    def test_duplicate_entries_and_empty_set(self):
        for entries in ([self.entry, self.entry], []):
            with self.subTest(count=len(entries)), self.assertRaises(ValueError):
                rebuild_parsed(self.image, entries)

    def test_encoded_entry_refused(self):
        for flag in ('encrypted', 'compressed'):
            setattr(self.entry, flag, True)
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, 'encoded'):
                rebuild_parsed(self.image, [self.entry])
            setattr(self.entry, flag, False)


if __name__ == '__main__':
    unittest.main()
