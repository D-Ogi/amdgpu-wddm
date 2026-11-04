"""Controls of the kmd-tidy gate (kmd_tidy.py): the parse of clang-tidy output, the line-free key, the ratchet,
and one real clang-tidy run on a planted dead store."""
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
import kmd_tidy  # noqa: E402

OUT = os.environ.get('BC250_TEST_OUT') or None


class ParseAndKey(unittest.TestCase):
    def test_both_diagnostic_formats(self):
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            src = Path(d) / 'a.c'
            src.write_text('int f(void)\n{\n    int x = 1;\n    return 0;\n}\n')
            text = (f'{src}:3:9: warning: Value stored to \'x\' during its initialization is never read '
                    f'[clang-analyzer-deadcode.DeadStores]\n'
                    f'{src}(3,9): warning: switch has 2 consecutive identical branches [bugprone-branch-clone]\n'
                    '    3 |     int x = 1;\n'
                    f'{src}:1:1: note: not a finding [some-check]\n')
            found = kmd_tidy.parse_output(text, Path(d), kmd_tidy.SourceLines())
            self.assertEqual([f['check'] for f in found],
                             ['clang-analyzer-deadcode.DeadStores', 'bugprone-branch-clone'])
            self.assertEqual({f['line'] for f in found}, {3})
            self.assertEqual(found[0]['file'], 'a.c')

    def test_key_survives_a_line_move_and_not_a_text_change(self):
        a = kmd_tidy.finding_key('Value stored to \'x\' is never read', '    x = 1;')
        self.assertEqual(a, kmd_tidy.finding_key('Value stored to \'x\' is never read', 'x = 1;  '))
        self.assertNotEqual(a, kmd_tidy.finding_key('Value stored to \'x\' is never read', '    x = 2;'))
        self.assertEqual(kmd_tidy.normalise('branch at line 12 is like line 30'),
                         'branch at line N is like line N')

    def test_header_finding_counts_once(self):
        row = {'file': 'h.h', 'line': 1, 'col': 1, 'check': 'c', 'message': 'm', 'key': 'k'}
        self.assertEqual(len(kmd_tidy.unique_findings([dict(row), dict(row)])), 1)

    def test_ratchet(self):
        counts = {('c', 'a.c', 'k1'): 2, ('c', 'a.c', 'k2'): 1}
        new, lower = kmd_tidy.compare(counts, {('c', 'a.c', 'k1'): 2, ('c', 'a.c', 'k3'): 1})
        self.assertEqual(set(new), {('c', 'a.c', 'k2')})
        self.assertEqual(set(lower), {('c', 'a.c', 'k3')})
        new, _ = kmd_tidy.compare({('c', 'a.c', 'k1'): 3}, {('c', 'a.c', 'k1'): 2})
        self.assertEqual(new, {('c', 'a.c', 'k1'): (3, 2)})
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            path = Path(d) / 'b.txt'
            kmd_tidy.write_baseline(path, counts, {('c', 'a.c', 'k1'): 'message'})
            self.assertEqual(kmd_tidy.read_baseline(path), counts)

    def test_translation_drops_outputs_and_keeps_inputs(self):
        entry = {'directory': 'P:/r', 'file': 'P:/r/a.c',
                 'arguments': ['C:/x/cl.exe', '/nologo', '/c', '/kernel', '/WX', '/Zi', '/O2', '/DX=1', '/IP:/inc',
                               '/FoP:/o/', '/FdP:/o/cl.pdb', '/std:c11', 'P:/r/a.c']}
        argv = kmd_clang.clang_cl_argv(entry, 'clang-cl.exe')
        for keep in ('/kernel', '/DX=1', '/IP:/inc', '/std:c11', '--target=x86_64-pc-windows-msvc'):
            self.assertIn(keep, argv)
        for drop in ('/nologo', '/c', '/WX', '/Zi', '/O2', '/FoP:/o/', '/FdP:/o/cl.pdb'):
            self.assertNotIn(drop, argv)
        self.assertEqual(argv[-1], str(Path('P:/r', 'P:/r/a.c')))
        self.assertTrue(any(a.startswith('/FI') and a.endswith('kmd_clang_prelude.h') for a in argv))


LLVM = kmd_clang.default_llvm_bin()


@unittest.skipUnless((LLVM / 'clang-tidy.exe').is_file(), 'the workspace LLVM (clang-tidy.exe) is required')
class RealClangTidy(unittest.TestCase):
    """A planted dead store fails the gate, the baseline admits it, a line move keeps it admitted, and a second
    dead store fails the gate again."""

    CLEAN = 'int f(int a)\n{\n    return a + 1;\n}\n'
    ONE = 'int f(int a)\n{\n    int x = a;\n    x = 2;\n    return a + 1;\n}\n'

    def run_gate(self, d, extra=()):
        argv = ['--database', str(d / 'compile_commands.json'), '--out', str(d / 'out'), '--match', r'unit\.c$',
                '--baseline', str(d / 'baseline.txt'), '--jobs', '1', *extra]
        with contextlib.redirect_stdout(io.StringIO()) as text:
            code = kmd_tidy.main(argv)
        return code, json.loads((d / 'out' / 'report.json').read_text()), text.getvalue()

    def test_dead_store_ratchet(self):
        with tempfile.TemporaryDirectory(dir=OUT) as tmp:
            d = Path(tmp)
            src = d / 'unit.c'
            (d / 'compile_commands.json').write_text(json.dumps(
                [{'directory': str(d), 'file': str(src), 'arguments': ['cl.exe', '/c', '/W4', str(src)]}]))
            src.write_text(self.CLEAN)
            code, report, text = self.run_gate(d)
            self.assertEqual((code, report['findings'], report['broken']), (0, 0, []), text)
            src.write_text(self.ONE)
            code, report, text = self.run_gate(d)
            self.assertEqual(code, 1, text)
            self.assertIn('clang-analyzer-deadcode.DeadStores', report['per_check'])
            code, report, text = self.run_gate(d, ['--accept'])
            self.assertEqual(code, 0, text)
            src.write_text('/* a comment above moves every line */\n\n' + self.ONE)
            code, report, text = self.run_gate(d)
            self.assertEqual(code, 0, text)
            src.write_text(self.ONE.replace('    return a + 1;', '    int y = a;\n    y = 3;\n    return a + 1;'))
            code, report, text = self.run_gate(d)
            self.assertEqual(code, 1, text)
            self.assertEqual(len(report['new']), 1)


if __name__ == '__main__':
    unittest.main()
