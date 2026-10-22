"""Falsification controls using serialized results from the actual C probe."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import re
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('latch_analysis', HERE / 'analyze-dcn-latch.py')
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)
MODEL_RECORDS = None


class SourceBindings(unittest.TestCase):
    def test_named_amd_masks(self):
        header = (HERE.parents[2] / 'third_party/linux-amdgpu/dcn_2_0_1_sh_mask.h').read_text()
        for name, value in (
            ('OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK', analysis.LOCK_REQUEST),
            ('OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK', analysis.LOCK_ACK),
            ('OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK', analysis.LOCK_SELECT),
            ('HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK', analysis.ADDRESS_HIGH),
            ('HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK', analysis.FLIP_PENDING),
        ):
            match = re.search(r'^#define\s+' + name + r'\s+(0x[0-9a-fA-F]+)L?\s*$', header, re.M)
            self.assertIsNotNone(match, name)
            self.assertEqual(int(match[1], 16), value)

    def test_duplicate_keys_rejected(self):
        with self.assertRaises(ValueError):
            json.loads('{"exit_code":1,"exit_code":0}', object_pairs_hook=analysis.unique_object)


class EvidenceControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if MODEL_RECORDS is None:
            raise unittest.SkipTest('Run run_latch_probe.ps1 for the actual C model records')
        rows = [json.loads(line) for line in MODEL_RECORDS.read_text(encoding='utf-8-sig').splitlines()]
        if len(rows) != 9 or [r['model'] for r in rows] != [0, 0, 0, 1, 1, 1, 2, 2, 2]:
            raise ValueError('Expected nine actual-core model records')
        cls.models = [[r['records'] for r in rows[start:start+3]] for start in (0, 3, 6)]

    def capture(self, arm=2, model=0):
        return copy.deepcopy(self.models[model][arm])

    def test_actual_core_models(self):
        for i, expected in enumerate(('H1-compatible', 'H2-compatible')):
            result = analysis.compare(self.models[i])
            self.assertTrue(result['eligible'], result['reasons'])
            self.assertEqual(result['conditional_pattern'], expected)
            for field in ('hardware_contract_verified', 'latch_proven', 'retirement_proven'):
                self.assertFalse(result[field])

    def test_deferred_acceptance_falsifies_unique_h1_interpretation(self):
        # This C device model permits an already accepted B to latch under a
        # lock (H2). Its trigger submission has not yet accepted B when the
        # later lock arrives. Both controls pass, yet its record looks like H1.
        result = analysis.compare(self.models[2])
        self.assertEqual(self.models[0], self.models[2])
        self.assertTrue(result['eligible'], result['reasons'])
        self.assertEqual(result['conditional_pattern'], 'H1-compatible')
        self.assertFalse(result['hardware_contract_verified'])

    def test_every_truncated_packet(self):
        for count in range(120):
            with self.subTest(count=count):
                c = self.capture(); c[1]['raw_dwords'] = c[1]['raw_dwords'][:count]
                with self.assertRaises(ValueError): analysis.decode(c)

    def test_invalid_dword_types(self):
        for value in (True, -1, 1 << 32, 1.0, '1', None):
            c = self.capture(); c[1]['raw_dwords'][20] = value
            with self.subTest(value=value), self.assertRaises(ValueError): analysis.decode(c)

    def test_missing_or_extra_records(self):
        for c in (self.capture()[:2], self.capture()+[{'type': 'end'}]):
            with self.assertRaises(ValueError): analysis.decode(c)

    def test_failed_statuses_cannot_classify(self):
        for name, offset in (('nt_status', 7), ('measurement_status', 20), ('unlock_status', 21),
                             ('restore_status', 22), ('end_status', 23)):
            c = self.capture(); c[1][name] = c[1]['raw_dwords'][offset] = 0xC000022D
            with self.subTest(name=name): self.assertFalse(analysis.decode(c)['eligible'])

    def test_summary_cannot_hide_raw_failure(self):
        c = self.capture(); c[1]['raw_dwords'][22] = 0xC0000184
        with self.assertRaises(ValueError): analysis.decode(c)

    def test_every_sample_bracket(self):
        for sample in range(4):
            for word, bit in ((9, 1), (1, analysis.LOCK_SELECT), (8, analysis.LOCK_SELECT),
                              (7, analysis.LOCK_REQUEST), (5, 1), (10, 1),
                              (11, 1), (12, 1), (13, 1), (14, 1)):
                c = self.capture(); c[1]['raw_dwords'][30+sample*16+word] ^= bit
                with self.subTest(sample=sample, word=word): self.assertFalse(analysis.decode(c)['eligible'])

    def test_old_abi_and_reserved_words_rejected(self):
        for version in (1, 2):
            c = self.capture()
            c[1]['raw_dwords'][4] = version
            self.assertFalse(analysis.decode(c)['eligible'])
        c = self.capture()
        c[1]['raw_dwords'][8] = 1
        self.assertFalse(analysis.decode(c)['eligible'])

    def test_unlock_observation_corruption(self):
        for offset, value in ((94, 0), (96, 0), (97, 0), (97, 1002),
                              (98, analysis.LOCK_ACK), (99, 0), (100, 1),
                              (101, 1), (102, 0xFFFFFFFF), (104, 0xFFFFFFFF),
                              (106, 1), (108, 1)):
            c = self.capture(); c[1]['raw_dwords'][offset] = value
            with self.subTest(offset=offset): self.assertFalse(analysis.decode(c)['eligible'])

    def test_clear_pending_rejects_old_address_race(self):
        for arm in (1, 2):
            c = self.capture(arm=arm)
            c[1]['raw_dwords'][46+6] &= ~analysis.FLIP_PENDING
            self.assertFalse(analysis.decode(c)['eligible'])
        c = self.capture(arm=0)
        c[1]['raw_dwords'][46+6] = 0
        self.assertTrue(analysis.decode(c)['eligible'])

    def test_unused_high_register_bits_not_address_bits(self):
        c = self.capture()
        for sample in range(4):
            for word in (3, 5): c[1]['raw_dwords'][30+sample*16+word] |= 0xABCD0000
        self.assertTrue(analysis.decode(c)['eligible'])

    def test_counter_wrap_is_only_word_movement(self):
        c = self.capture()
        for sample in range(4):
            value = 0xFFFFFFFF if sample < 2 else 0
            for word in (0, 9): c[1]['raw_dwords'][30+sample*16+word] = value
        self.assertTrue(analysis.decode(c)['eligible'])
        for sample in range(4):
            for word in (0, 9): c[1]['raw_dwords'][30+sample*16+word] = 0
        self.assertFalse(analysis.decode(c)['eligible'])

    def test_h2_does_not_mean_already_b_when_armed(self):
        c = self.capture(model=1); w = c[1]['raw_dwords']
        w[46+4] = w[46+14] = w[28]; w[46+3] = w[46+5] = w[46+15] = w[29]
        self.assertFalse(analysis.decode(c)['eligible'])

    def test_failed_controls_invalidate_discriminator(self):
        for arm in (0, 1):
            captures = copy.deepcopy(self.models[1])
            w = captures[arm][1]['raw_dwords']
            # Valid-looking raw boundary with the opposite control address.
            offset = 26 if arm == 0 else 28
            w[62+4] = w[62+14] = w[offset]
            w[62+3] = w[62+5] = w[62+15] = w[offset+1]
            result = analysis.compare(captures)
            self.assertFalse(result['eligible'])
            self.assertIsNone(result['conditional_pattern'])

    def test_mixed_versions_and_reordered_intervals(self):
        captures = copy.deepcopy(self.models[0])
        c = captures[2]; c[0]['expected_version'] += 1
        c[1]['raw_dwords'][3] += 1; c[1]['raw_dwords'][6] += 1
        self.assertTrue(analysis.decode(c)['eligible'])
        self.assertFalse(analysis.compare(captures)['eligible'])
        captures = copy.deepcopy(self.models[0])
        captures[2][1]['begin_qpc'] = captures[1][1]['begin_qpc']
        self.assertFalse(analysis.compare(captures)['eligible'])

    def test_qpc_failure_or_timeout(self):
        c = self.capture(); c[1]['end_qpc'] = c[1]['begin_qpc']-1
        with self.assertRaises(ValueError): analysis.decode(c)
        c = self.capture(); c[1]['end_qpc'] = c[1]['begin_qpc']+31*c[0]['qpc_frequency']
        self.assertFalse(analysis.decode(c)['eligible'])

    def test_fake_client_verdict(self):
        c = self.capture(); c[-1]['hardware_verdict'] = 'H2'
        with self.assertRaises(ValueError): analysis.decode(c)

    def test_boolean_schema_rejected(self):
        c = self.capture(); c[0]['schema'] = True
        with self.assertRaises(ValueError): analysis.decode(c)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--model-records', type=Path, required=True)
    args = parser.parse_args()
    MODEL_RECORDS = args.model_records
    unittest.main(argv=['test_dcn_latch_analysis'], verbosity=2)
