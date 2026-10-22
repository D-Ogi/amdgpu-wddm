"""Offline evidence controls. These tests do not open an adapter."""
import copy
import importlib.util
from pathlib import Path
import re
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('observe_analysis', HERE / 'analyze-dcn-observe.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def capture():
    d = dict.fromkeys(analysis.FIELDS, 0)
    d.update(Magic=0x30353242, Command=20, Version=0x700D8, AbiVersion=1,
             RegisterCount=22, ValidMask=(1 << 22)-1, PrimaryAddressLow=4096,
             EarliestInUseLow=4096, PrimaryAddressHigh=4, EarliestInUseHigh=4)
    return [dict(type='header', schema=1, samples=2, expected_version=0x700D8,
                 qpc_frequency=1000, atomic=False, hardware_access=True),
            dict(type='sample', index=0, escape_status=0, begin_qpc=100, end_qpc=101, data=d),
            dict(type='sample', index=1, escape_status=0, begin_qpc=350, end_qpc=352, data=copy.deepcopy(d)),
            dict(type='end', exit_code=0)]


class EvidenceControls(unittest.TestCase):
    def test_positive_capture_is_not_retirement(self):
        rows = capture()
        rows[2]['data']['OtgFrameCount'] = 15
        rows[2]['data']['PrimaryAddressLow'] = rows[2]['data']['EarliestInUseLow'] = 8192
        result = analysis.summarize(rows)
        self.assertTrue(result['capture_complete'])
        self.assertEqual(result['distinct_frame_words'], 2)
        self.assertEqual(result['distinct_primary_word_pairs'], 2)
        self.assertEqual(result['max_escape_ms'], 2)
        self.assertFalse(result['latch_proven'])
        self.assertFalse(result['retirement_proven'])

    def test_torn_candidate_remains_unproven(self):
        rows = capture()
        rows[1]['data']['EarliestInUseHigh'] = 5
        result = analysis.summarize(rows)
        self.assertEqual(result['clear_with_unequal_address_words'], [0])
        self.assertFalse(result['latch_proven'])
        rows[1]['data']['FlipControl'] = 0x100
        result = analysis.summarize(rows)
        self.assertEqual(result['clear_with_unequal_address_words'], [])
        self.assertEqual(result['pending_samples'], [0])

    def test_software_generation_change_excludes_candidate(self):
        for before, after in ((0, 2), (3, 3)):
            rows = capture()
            rows[1]['data'].update(SequenceBefore=before, SequenceAfter=after, EarliestInUseLow=8192)
            result = analysis.summarize(rows)
            self.assertEqual(result['software_change_samples'], [0])
            self.assertEqual(result['clear_with_unequal_address_words'], [])

    def test_wrong_contract_or_partial_read(self):
        for field, value in [('Magic', 0), ('Command', 19), ('Status', 2), ('NtStatus', 0xC0000022),
                             ('Version', 0x700D7), ('AbiVersion', 2), ('RegisterCount', 21), ('ValidMask', 7)]:
            with self.subTest(field=field):
                rows = capture()
                rows[1]['data'][field] = value
                result = analysis.summarize(rows)
                self.assertFalse(result['capture_complete'])
                self.assertEqual(result['invalid_samples'], [0])
        rows = capture()
        rows[1]['escape_status'] = 0xC0000022
        self.assertFalse(analysis.summarize(rows)['capture_complete'])

    def test_incomplete_or_failed_tail(self):
        rows = capture()
        self.assertFalse(analysis.summarize(rows[:-1])['capture_complete'])
        self.assertFalse(analysis.summarize(rows[:2] + rows[-1:])['capture_complete'])
        rows[-1]['exit_code'] = 1
        self.assertFalse(analysis.summarize(rows)['capture_complete'])

    def test_bad_input(self):
        for mutate in (lambda r: r[0].update(qpc_frequency=0),
                       lambda r: r[0].update(samples=0),
                       lambda r: r[0].update(atomic=True),
                       lambda r: r[1]['data'].update(FlipControl=-1),
                       lambda r: r[1]['data'].pop('TimingControl'),
                       lambda r: r[2].update(begin_qpc=99),
                       lambda r: r.insert(2, dict(type='end', exit_code=0))):
            rows = capture()
            mutate(rows)
            with self.assertRaises(ValueError):
                analysis.summarize(rows)

    def test_source_abi_binding(self):
        repo = HERE.parents[2]
        header = (repo / 'driver/kmd/bc250kmd_escape.h').read_text()
        body = header.split('typedef struct _BC250_ESCAPE_DCN_OBSERVE {', 1)[1].split('}', 1)[0]
        names = re.findall(r'unsigned long ([^;]+);', body)
        self.assertEqual(analysis.FIELDS, [name.strip() for group in names for name in group.split(',')])
        client = (HERE / 'dcn_observe.cpp').read_text()
        printed = client.split('static const char *fields[] = {', 1)[1].split('};', 1)[0]
        self.assertEqual(analysis.FIELDS, re.findall(r'"([A-Za-z0-9]+)"', printed))
        for name, expected in [('BC250_ESCAPE_MAGIC', 0x30353242), ('BC250_ESCAPE_OBSERVE_DCN', 20),
                               ('BC250_DCN_OBSERVE_ABI', 1), ('BC250_DCN_OBSERVE_REG_COUNT', 22),
                               ('BC250_ESCAPE_STATUS_DONE', 0)]:
            value = re.search(r'^#define ' + name + r' (0x[0-9A-Fa-f]+|[0-9]+)u?\b', header, re.M).group(1)
            self.assertEqual(int(value, 0), expected)
        masks = (repo / 'third_party/linux-amdgpu/dcn_2_0_1_sh_mask.h').read_text()
        for name, expected in [
            ('HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK', analysis.SURFACE_FLIP_PENDING),
            ('HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK', analysis.ADDRESS_HIGH),
            ('HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK', analysis.ADDRESS_HIGH)]:
            value = re.search(r'^#define ' + name + r'\s+(0x[0-9A-Fa-f]+)', masks, re.M).group(1)
            self.assertEqual(int(value, 0), expected)


if __name__ == '__main__':
    unittest.main()
