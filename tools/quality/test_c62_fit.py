"""The C62 analysis must reproduce its own published numbers from the staged evidence tree.

Audit finding F2 (2026-10-10): `evidence/windows/2026-10-07-c62-ident/ident/ident-fit.py` looks for its input
in its own folder plus `ident` again, and `grid/fit.py` in its own folder plus `runs`. Neither path exists in
the published layout, so both run to an empty result, and both write their output back into `evidence/`. The
numbers of facts M828 and M829 are right; the scripts as published cannot produce them again.

Those two files stay as the round published them, because `evidence/` is immutable and the package's own
sha256.txt covers them (CLAUDE.md rule 6). `tools/quality/c62/` holds the same arithmetic with the input and
the output as arguments, and this test runs those reproducers against the staged evidence tree and compares
their results with the recorded ones. It also checks that a run writes nothing into `evidence/`.

    python -m unittest discover -s tools/quality
    python -m unittest tools.quality.test_c62_fit     # from the repository root
"""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
C62 = REPO / 'evidence' / 'windows' / '2026-10-07-c62-ident'
IDENT_FIT = HERE / 'c62' / 'ident_fit.py'
GRID_FIT = HERE / 'c62' / 'grid_fit.py'


def tree_hashes(root):
    """Every file of the package and its SHA-256, so that a write into evidence cannot pass unseen."""
    out = {}
    for path in sorted(p for p in root.rglob('*') if p.is_file()):
        out[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def run(script, arguments):
    """The script, with the interpreter that runs this test, started from the repository root so that a
    default path cannot come out of the current directory. Isolated mode is not used: ident_fit.py needs
    numpy, which lives in the user site directory here."""
    return subprocess.run([sys.executable, str(script)] + arguments, cwd=str(REPO),
                          capture_output=True, text=True)


class C62Scripts(unittest.TestCase):
    def setUp(self):
        if not C62.is_dir():
            self.skipTest('the C62 evidence package is not in this tree')
        self.before = tree_hashes(C62)

    def tearDown(self):
        self.assertEqual(self.before, tree_hashes(C62), 'a C62 script wrote into evidence/, which is immutable')

    def test_ident_fit_reproduces_its_recorded_json(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest('numpy is not available to this interpreter')
        done = run(IDENT_FIT, [])
        self.assertEqual(done.returncode, 0, done.stderr)
        got = json.loads(done.stdout)
        recorded = json.loads((C62 / 'ident' / 'ident-fit.json').read_text())
        self.assertEqual(sorted(got), ['fan100', 'fan50'], 'the default input folder is not the published one')
        self.assertEqual(got, recorded)
        # The numbers fact M828 states, read out of the result and not out of the prose.
        self.assertEqual(got['fan100']['samples'], 56)
        self.assertEqual(got['fan50']['samples'], 45)
        self.assertAlmostEqual(got['fan100']['gpu_c']['tau_s'], 14.4, places=3)
        self.assertAlmostEqual(got['fan100']['gpu_c']['R_C_per_W'], 0.259, places=3)
        self.assertAlmostEqual(got['fan50']['gpu_c']['tau_s'], 19.0, places=3)

    def test_ident_fit_writes_only_where_it_is_told(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest('numpy is not available to this interpreter')
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / 'ident-fit.json'
            done = run(IDENT_FIT, ['--out', str(out)])
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(json.loads(out.read_text()), json.loads(done.stdout))

    def test_grid_fit_reproduces_its_recorded_json_and_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            outJson = pathlib.Path(tmp) / 'fit.json'
            outMd = pathlib.Path(tmp) / 'fit.md'
            done = run(GRID_FIT, ['--out-json', str(outJson), '--out-md', str(outMd)])
            self.assertEqual(done.returncode, 0, done.stderr)
            got = json.loads(outJson.read_text())
            recorded = json.loads((C62 / 'grid' / 'fit.json').read_text())
            self.assertEqual(sorted(got), ['f0-g0-c0', 'f0-g0-c4', 'f0-g25-c0', 'f0-g25-c4'],
                             'the default input folder is not the published one')
            self.assertEqual(got, recorded)
            self.assertEqual(outMd.read_text(encoding='utf-8'),
                             (C62 / 'grid' / 'fit.md').read_text(encoding='utf-8'))
        # The numbers fact M829 states.
        self.assertEqual([got[c]['mhz_mean'] for c in sorted(got)], [988, 984, 963, 1030])
        self.assertEqual([got[c]['mhz_last30'] for c in sorted(got)], [800, 800, 800, 800])
        self.assertEqual([got[c]['smu_w_last30'] for c in sorted(got)], [89.5, 84.3, 84.7, 82.5])
        self.assertEqual([got[c]['wall_w_mean'] for c in sorted(got)], [163.1, 159.9, 163.2, 171.4])
        self.assertEqual([got[c]['first_step_s'] for c in sorted(got)], [10.471, 10.493, 12.259, 12.275])


if __name__ == '__main__':
    unittest.main()
