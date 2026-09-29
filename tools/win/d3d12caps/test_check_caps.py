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


def entry(size, align, requested=0, samples=1, total=None, fmt='R8G8B8A8_UNORM', width=64):
    e = {'GetResourceAllocationInfo': {'SizeInBytes': size, 'Alignment': align},
         'desc': {'Alignment': requested, 'SampleCount': samples, 'Format': fmt, 'Width': width}}
    if samples == 1:  # the tool records footprints for single-sampled cases only
        e['GetCopyableFootprints'] = {'TotalBytes': size if total is None else total}
    return e


def plausible(name):
    """A passing answer for any inventory case, shaped like the real 091 answers."""
    if name == check_caps.SENTINEL:
        return entry('UINT64_MAX', 65536, requested=4096, total='UINT64_MAX')
    if name.startswith('buffer_'):
        width = {'buffer_64k': 65536, 'buffer_16m_uav': 16 << 20, 'buffer_256_align64k': 256}[name]
        return entry(max(width, 65536), 65536, requested=65536 if name.endswith('align64k') else 0, fmt='UNKNOWN',
                     width=width)
    if name.endswith('_align4k'):
        return entry(16384, 4096, requested=4096)
    if 'msaa4' in name:
        requested = 65536 if name.endswith('align64k') else 0
        return entry(1179648 if requested else 41943040, requested or 4194304, requested=requested, samples=4)
    return entry(9895936, 65536, total=9216000)


GOOD = {'device': {'allocations': {name: plausible(name) for name in check_caps.INVENTORY}}}


def with_answer(name, size, align):
    doc = copy.deepcopy(GOOD)
    doc['device']['allocations'][name]['GetResourceAllocationInfo'] = {'SizeInBytes': size, 'Alignment': align}
    return doc


class CheckCapsTest(unittest.TestCase):
    def test_good_document_passes(self):
        self.assertEqual(len(GOOD['device']['allocations']), 52)
        self.assertEqual(check_caps.check(GOOD), [])

    def test_wrapped_negative_sizes_fail(self):
        # Trial 090: every size serialized as the negated alignment.
        failures = check_caps.check(with_answer('rgba8_1920x1200_rt', -65536, 65536))
        self.assertEqual(failures, ['rgba8_1920x1200_rt: SizeInBytes -65536 is not a positive size'])

    def test_buffer_must_be_exact(self):
        failures = check_caps.check(with_answer('buffer_64k', 131072, 65536))
        self.assertEqual(failures, ['buffer_64k: 131072 / 65536, expected exactly 65536 / 65536'])

    def test_buffer_smaller_than_its_width_fails(self):
        # Codex 817: an aligned positive answer below the buffer's width passed before.
        failures = check_caps.check(with_answer('buffer_16m_uav', 65536, 65536))
        self.assertEqual(failures, ['buffer_16m_uav: SizeInBytes 65536 is smaller than the buffer width 16777216'])

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

    def test_every_inventory_case_is_required(self):
        doc = copy.deepcopy(GOOD)
        del doc['device']['allocations']['rgba16f_128x128x128_3d_uav']
        self.assertEqual(check_caps.check(doc), ['rgba16f_128x128x128_3d_uav: absent'])
        self.assertEqual(check_caps.check({'device': {}}), ['device.allocations: absent or empty'])

    def test_single_sampled_footprints_are_required(self):
        doc = copy.deepcopy(GOOD)
        del doc['device']['allocations']['buffer_16m_uav']['GetCopyableFootprints']
        self.assertEqual(check_caps.check(doc),
                         ['buffer_16m_uav: GetCopyableFootprints.TotalBytes None is not a positive size'])

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
