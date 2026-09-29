import copy
import importlib.util
import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout

_spec = importlib.util.spec_from_file_location('check_caps', os.path.join(os.path.dirname(__file__), 'check-caps.py'))
check_caps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_caps)


def entry(size, align, requested=0, samples=1, total=None):
    e = {'GetResourceAllocationInfo': {'SizeInBytes': size, 'Alignment': align},
         'desc': {'Alignment': requested, 'SampleCount': samples}}
    if total is not None:
        e['GetCopyableFootprints'] = {'TotalBytes': total}
    return e


GOOD = {'device': {'allocations': {
    'buffer_64k': entry(65536, 65536, total=65536),
    'rgba8_1920x1200_rt': entry(9895936, 65536, total=9216000),
    'rgba8_1920x1200_rt_msaa4': entry(41943040, 4194304, samples=4),
    'small_rgba8_64_align4k': entry(16384, 4096, requested=4096, total=16384),
    'small_rgba8_256_rt_msaa4_align64k': entry(1179648, 65536, requested=65536, samples=4),
    'small_rgba8_256_align4k_too_large': entry('UINT64_MAX', 65536, requested=4096, total='UINT64_MAX'),
}}}


def with_answer(name, size, align):
    doc = copy.deepcopy(GOOD)
    doc['device']['allocations'][name]['GetResourceAllocationInfo'] = {'SizeInBytes': size, 'Alignment': align}
    return doc


class CheckCapsTest(unittest.TestCase):
    def test_good_document_passes(self):
        self.assertEqual(check_caps.check(GOOD), [])

    def test_wrapped_negative_sizes_fail(self):
        # Trial 090: every size serialized as the negated alignment.
        failures = check_caps.check(with_answer('rgba8_1920x1200_rt', -65536, 65536))
        self.assertEqual(failures, ['rgba8_1920x1200_rt: SizeInBytes -65536 is not a positive size'])

    def test_buffer_must_be_exact(self):
        failures = check_caps.check(with_answer('buffer_64k', 131072, 65536))
        self.assertEqual(failures, ['buffer_64k: 131072 / 65536, expected exactly 65536 / 65536'])

    def test_default_and_requested_alignment(self):
        self.assertEqual(check_caps.check(with_answer('rgba8_1920x1200_rt_msaa4', 41943040, 65536)),
                         ['rgba8_1920x1200_rt_msaa4: Alignment 65536, expected 4194304'])
        self.assertEqual(check_caps.check(with_answer('small_rgba8_64_align4k', 65536, 65536)),
                         ['small_rgba8_64_align4k: Alignment 65536, expected 4096'])

    def test_misaligned_and_non_power_of_two(self):
        self.assertEqual(check_caps.check(with_answer('rgba8_1920x1200_rt', 9895937, 65536)),
                         ['rgba8_1920x1200_rt: SizeInBytes 9895937 is not a multiple of Alignment 65536'])
        self.assertEqual(check_caps.check(with_answer('rgba8_1920x1200_rt', 9895936, 65535)),
                         ['rgba8_1920x1200_rt: Alignment 65535 is not a power of two'])

    def test_sentinel_must_stay_invalid(self):
        failures = check_caps.check(with_answer('small_rgba8_256_align4k_too_large', 262144, 4096))
        self.assertEqual(failures, ['small_rgba8_256_align4k_too_large: SizeInBytes 262144, expected UINT64_MAX'])

    def test_missing_entries_and_section(self):
        doc = copy.deepcopy(GOOD)
        del doc['device']['allocations']['buffer_64k']
        self.assertEqual(check_caps.check(doc), ['buffer_64k: absent'])
        self.assertEqual(check_caps.check({'device': {}}), ['device.allocations: absent or empty'])

    def test_bool_is_not_a_size(self):
        self.assertEqual(check_caps.check(with_answer('rgba8_1920x1200_rt', True, 1)),
                         ['rgba8_1920x1200_rt: SizeInBytes True is not a positive size'])

    def test_main_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            good, bad = os.path.join(d, 'good.json'), os.path.join(d, 'bad.json')
            for p, v in ((good, GOOD), (bad, with_answer('buffer_64k', -65536, 65536))):
                with open(p, 'w', encoding='utf-8') as f:
                    json.dump(v, f)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(check_caps.main(['check-caps.py', good]), 0)
                self.assertEqual(check_caps.main(['check-caps.py', good, bad]), 1)
                self.assertEqual(check_caps.main(['check-caps.py', os.path.join(d, 'none.json')]), 2)
                self.assertEqual(check_caps.main(['check-caps.py']), 2)


if __name__ == '__main__':
    unittest.main()
