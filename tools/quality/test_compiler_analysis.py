"""Controls for the Clang static-analysis path of the package gate."""
import importlib.util
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).with_name('msvc_analysis.py'))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class ClangAnalysisTests(unittest.TestCase):
    def test_baseline_is_bound_to_exact_source_and_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory:
            cwd = Path(directory)
            source = cwd / 'unit.cpp'
            source.write_text('int f() { return 0; }')
            diagnostic = dict(check_name='core.NullDereference', description='null', issue_context='f',
                              issue_hash_content_of_line_in_context='abc', location=dict(file=0, line=1, col=2))
            report = cwd / 'report.plist'
            report.write_bytes(plistlib.dumps(dict(files=['unit.cpp'], diagnostics=[diagnostic])))
            row = {k: v for k, v in diagnostic.items() if k != 'location'}
            row.update(file_sha256=analysis.digest(source), line=1, col=2, reason='Reviewed test fixture')
            baseline = dict(translation_unit_sha256=analysis.digest(source), diagnostics=[row])
            self.assertTrue(analysis.clang_report_passed(0, b'warning: null', report, baseline, cwd, source))
            self.assertFalse(analysis.clang_report_passed(0, b'warning: null\nwarning: ignored', report, baseline, cwd, source))
            row['check_name'] = 'another.Check'
            self.assertFalse(analysis.clang_report_passed(0, b'warning: null', report, baseline, cwd, source))
            row['check_name'] = 'core.NullDereference'
            source.write_text('int f() { return 1; }')
            self.assertFalse(analysis.clang_report_passed(0, b'warning: null', report, baseline, cwd, source))

    @unittest.skipUnless(os.name == 'nt' and os.environ.get('BC250_ROOT'), 'Windows workspace compiler required')
    def test_real_clang_accepts_clean_and_detects_null_dereference(self):
        compiler = Path(os.environ['BC250_ROOT']) / 'toolchain/llvm-23.1.4-win64/bin/clang-cl.exe'
        self.assertTrue(compiler.is_file())
        with tempfile.TemporaryDirectory() as directory:
            cwd = Path(directory)
            for label, source_text, expected in (
                ('clean', 'int f(int x) { return x + 1; }', True),
                ('null', 'int f(void) { int *p = 0; return *p; }', False)):
                source = cwd / (label + '.c')
                source.write_text(source_text)
                report = cwd / (label + '.plist')
                command = analysis.clang_analysis_command(compiler, [str(compiler), '/c', str(source)], report)
                run = subprocess.run(command, cwd=cwd, capture_output=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertEqual(analysis.clang_report_passed(run.returncode, run.stdout + run.stderr, report), expected)
                if not expected:
                    self.assertIn(b'core.NullDereference', report.read_bytes())

    def test_preserves_build_inputs_and_redirects_outputs(self):
        original = ['clang-cl.exe', '-DTHING=1', '-Iinclude', '/clang:-march=x86-64-v3',
                    '/W4', '/c', '/showIncludes', '/Fooriginal.obj', '/Fdoriginal.pdb', 'unit.cpp']
        command = analysis.clang_analysis_command('clang-cl.exe', original, Path('report.plist'))
        for item in ('-DTHING=1', '-Iinclude', '/clang:-march=x86-64-v3', '/W4', 'unit.cpp'):
            self.assertIn(item, command)
        for item in ('/c', '/showIncludes', '/Fooriginal.obj', '/Fdoriginal.pdb'):
            self.assertNotIn(item, command)
        self.assertEqual(command[-5:], ['--analyze', '-Xclang', '-analyzer-output=plist-multi-file', '-o', 'report.plist'])

    def test_requires_clean_explicit_analyzer_report(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / 'analysis.plist'
            self.assertFalse(analysis.clang_report_passed(0, b'', report))
            for diagnostics in (None, [{'description': 'null dereference'}], []):
                data = {} if diagnostics is None else {'diagnostics': diagnostics}
                report.write_bytes(plistlib.dumps(data))
                self.assertEqual(analysis.clang_report_passed(0, b'', report), diagnostics == [])
            self.assertFalse(analysis.clang_report_passed(1, b'', report))
            self.assertFalse(analysis.clang_report_passed(0, b'warning: ignored argument', report))


if __name__ == '__main__':
    unittest.main()
