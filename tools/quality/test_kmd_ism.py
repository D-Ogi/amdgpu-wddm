"""Controls of the kmd-ism detector (kmd_ism.py): a clean explicit state machine gives no candidate, the same
machine with loose flags gives one candidate of each kind, and the ratchet and the positive controls hold."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kmd_clang  # noqa: E402
import kmd_ism  # noqa: E402

OUT = os.environ.get('BC250_TEST_OUT') or None
FIXTURES = Path(__file__).resolve().parent / 'kmd_ism_fixtures'
LLVM = kmd_clang.default_llvm_bin()


def detect(source):
    with tempfile.TemporaryDirectory(dir=OUT) as d:
        argv = ['--files', str(FIXTURES / source), '--own', 'kmd_ism_fixtures', '--out', d, '--no-baseline',
                '--jobs', '1']
        with contextlib.redirect_stdout(io.StringIO()):
            code = kmd_ism.main(argv)
        report = json.loads((Path(d) / 'report.json').read_text(encoding='utf-8'))
    return code, report


@unittest.skipUnless((LLVM / 'clang-cl.exe').is_file(), 'the workspace LLVM (clang-cl.exe) is required')
class Fixtures(unittest.TestCase):
    def test_negative_control_explicit_enum(self):
        code, report = detect('clean_state.c')
        self.assertEqual(code, 0)
        self.assertEqual(report['broken'], [])
        self.assertGreater(report['events'], 5, 'the fixture was read: it has field accesses')
        self.assertEqual([c['key'] for c in report['candidates']], [])

    def test_mutant_loose_flags(self):
        code, report = detect('loose_flags.c')
        self.assertEqual(code, 0)
        self.assertEqual(sorted(c['key'] for c in report['candidates']), sorted([
            'cluster _DEVICE.Active _DEVICE.Training',
            'cluster _DEVICE.Active _DEVICE.Faulted',
            'cluster _DEVICE.Faulted _DEVICE.Training',
            'dead _DEVICE.LastError',
            'sticky _DEVICE.Faulted',
            'int-state _DEVICE.Step',
            'switch LinkName _LINK_KIND',
        ]))


class Ratchet(unittest.TestCase):
    def test_baseline_round_trip(self):
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            path = Path(d) / 'b.txt'
            kmd_ism.write_baseline(path, {'dead A.x', 'cluster A.a A.b'})
            self.assertEqual(kmd_ism.read_baseline(path), {'dead A.x', 'cluster A.a A.b'})
            self.assertTrue(path.read_text().startswith('#'))

    def test_controls_are_in_the_baseline(self):
        """Every positive control is a candidate of the day the baseline was written, so a gate run on an
        unchanged driver has each of them. A control outside the baseline would be a control nobody saw."""
        controls = kmd_ism.read_controls(kmd_ism.CONTROLS)
        self.assertGreaterEqual(len(controls), 3)
        baseline = kmd_ism.read_baseline(kmd_ism.BASELINE)
        for key, why in controls:
            self.assertIn(key, baseline, why)
            self.assertTrue(why)

    def test_analysis_rules_on_plain_facts(self):
        """The rules without clang: a flag set outside start and cleared only at start is sticky; an enum that
        start, stop and transitions write is not."""
        records = {'_D': {'file': 'd.h', 'line': 1, 'union': False, 'fields': {
            'Faulted': {'class': 'flag', 'type': 'BOOLEAN', 'record': None, 'target': None, 'line': 2},
            'Phase': {'class': 'enum', 'type': 'PHASE', 'record': None, 'target': None, 'line': 3}}}}

        def ev(field, kind, value, func):
            return {'field': '_D.' + field, 'kind': kind, 'value': value, 'callee': None, 'func': func,
                    'func_file': 'd.c', 'static': False, 'file': 'd.c', 'line': 1, 'root': None, 'whole': None}
        events = [ev('Faulted', 'write', '1', 'OnError'), ev('Faulted', 'write', '0', 'DevStart'),
                  ev('Faulted', 'read', None, 'Usable'), ev('Phase', 'write', 'PhaseA', 'Go'),
                  ev('Phase', 'read', None, 'Usable')]
        functions = {n: {'name': n, 'file': 'd.c', 'line': 1, 'static': False, 'calls': [], 'switches': [],
                         'key': n} for n in ('OnError', 'DevStart', 'Usable', 'Go')}
        candidates, _ = kmd_ism.analyse(records, {}, {'_PHASE': {'PhaseA': 0}}, functions, events, {'_D'})
        self.assertEqual([c['key'] for c in candidates], ['sticky _D.Faulted'])


if __name__ == '__main__':
    unittest.main()
