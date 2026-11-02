"""Tests of the spawn-loop ratchet (BD-051), tools/quality/spawn_loop_lint.py.

The positive control is kmdlog-stream.ps1 as it was at 930f075e: it started bc250kmd_cli twice a round, every
0.55 s, inside the ssh session of trial 427. The lint must count that loop, and must count none in the tools that
now read the KMD in-process. Run:
    python -m unittest discover -s tools/quality -p "test_spawn_loop_lint.py"
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import spawn_loop_lint as lint  # noqa: E402

BEFORE_THE_FIX = '930f075e'
TMP = os.environ.get('BC250_TEST_TMP') or None


def has_ps51():
    return lint.PS51.exists()


@unittest.skipUnless(has_ps51(), 'Windows PowerShell 5.1 is needed for the PowerShell rule')
class PowerShellRule(unittest.TestCase):
    CASES = {
        'cli-poll.ps1': ("$cli = 'C:\\x\\bc250kmd_cli.exe'\n"
                         "while ($true) { $t = & $cli clock read; Start-Sleep -Seconds 1 }\n", 1),
        'no-wait.ps1': ("$cli = 'C:\\x\\bc250kmd_cli.exe'\n"
                        "foreach ($a in 1..3) { & $cli clock read }\n", 0),
        'via-function.ps1': ("function Run-Bounded($exe) { $p = Start-Process $exe -PassThru; $p.WaitForExit(1000) }\n"
                             "function Read-Clock { Run-Bounded 'cli.exe' }\n"
                             "for ($i = 0; $i -lt 9; $i++) { Read-Clock; Start-Sleep -Milliseconds 500 }\n", 1),
        'scriptblock.ps1': ("function Repeat([scriptblock]$Body) { while ($true) { & $Body; Start-Sleep 1 } }\n", 0),
        'in-process.ps1': ("while ($true) { $x = [IO.File]::ReadAllText('a'); Start-Sleep 1 }\n", 0),
        'nested.ps1': ("foreach ($a in 1..3) {\n"
                       "  while ($true) { & ping.exe -n 1 localhost; Start-Sleep 1 }\n"
                       "  Start-Sleep 1\n}\n", 1),
        'allowed.ps1': ("# spawn-loop-ok: one start after a confirmed stall only\n"
                        "while ($true) { Start-Process notepad.exe; Start-Sleep 1 }\n", 0),
    }

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=TMP)
        root = Path(cls.tmp.name)
        cls.paths = {}
        for name, (text, _) in cls.CASES.items():
            p = root / name
            p.write_text(text, encoding='utf-8')
            cls.paths[name] = p
        cls.result = lint.scan(list(cls.paths.values()))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_each_case_counts_what_it_should(self):
        for name, (_, want) in self.CASES.items():
            entry = self.result[str(self.paths[name])]
            self.assertEqual(entry['error'], '', name)
            self.assertEqual(len(lint.counted(entry)), want, '%s: %s' % (name, entry['loops']))

    def test_the_allow_comment_keeps_its_reason(self):
        loops = self.result[str(self.paths['allowed.ps1'])]['loops']
        self.assertEqual([l['allowed'] for l in loops], ['one start after a confirmed stall only'])

    def test_a_nested_loop_is_counted_once(self):
        loops = self.result[str(self.paths['nested.ps1'])]['loops']
        self.assertEqual([l['line'] for l in loops], [2])


@unittest.skipUnless(has_ps51(), 'Windows PowerShell 5.1 is needed for the PowerShell rule')
class TheRealTools(unittest.TestCase):
    SWITCHED = ['tools/win/lab-runner/kmdlog-stream.ps1', 'tools/win/lab-runner/game-runtime.ps1',
                'tools/win/kmdread/kmd-reader.ps1', 'compute/hip/lab/armlib.ps1',
                'tools/win/kmd-deploy/ops/confirm-current.ps1', 'tools/win/kmd-deploy/ops/fresh-dwm.ps1',
                'tools/win/train-validate/lab/stop-record.ps1']

    def test_the_stream_of_trial_427_is_counted(self):
        old = subprocess.run(['git', '-C', str(REPO), 'show', BEFORE_THE_FIX + ':tools/win/lab-runner/kmdlog-stream.ps1'],
                             capture_output=True)
        if old.returncode != 0:
            self.skipTest('commit %s is not in this clone' % BEFORE_THE_FIX)
        with tempfile.TemporaryDirectory(dir=TMP) as d:
            p = Path(d) / 'kmdlog-stream.ps1'
            p.write_bytes(old.stdout)
            entry = lint.scan([p])[str(p)]
        self.assertGreaterEqual(len(lint.counted(entry)), 1, entry)

    def test_the_switched_tools_start_no_process_in_a_loop(self):
        paths = [REPO / r for r in self.SWITCHED]
        result = lint.scan(paths)
        for p in paths:
            entry = result[str(p)]
            self.assertEqual(entry['error'], '', p)
            self.assertEqual(lint.counted(entry), [], p)


class ShellRule(unittest.TestCase):
    def loops(self, text):
        with tempfile.TemporaryDirectory(dir=TMP) as d:
            p = Path(d) / 'case.sh'
            p.write_text(text, encoding='utf-8')
            return lint.sh_loops(p)

    def test_a_loop_that_reaches_the_lab_each_round_counts(self):
        self.assertEqual(len(self.loops('while true; do\n  ssh lab hostname\n  sleep 5\ndone\n')), 1)

    def test_a_function_and_a_variable_that_hold_the_lab_tool_count(self):
        text = ('emerg="$here/../lab-emerg.py"\n'
                'read_dpm() {\n  python "$emerg" ps -c x\n}\n'
                'while [ 1 ]; do\n  line=$(read_dpm)\n  sleep 30\ndone\n')
        loops = self.loops(text)
        self.assertEqual(len(loops), 1)
        self.assertEqual(loops[0]['starts'][0]['what'], 'read_dpm()')

    def test_a_local_loop_does_not_count(self):
        self.assertEqual(self.loops('for f in *.txt; do\n  wc -l "$f"\n  sleep 1\ndone\n'), [])

    def test_the_allow_comment_applies(self):
        loops = self.loops('# spawn-loop-ok: once a minute, no ssh\nwhile true; do\n  inp.py shot\n  sleep 60\ndone\n')
        self.assertEqual([l['allowed'] for l in loops], ['once a minute, no ssh'])


@unittest.skipUnless(has_ps51(), 'Windows PowerShell 5.1 is needed for the PowerShell rule')
class Ratchet(unittest.TestCase):
    LOOP = "while ($true) { & ping.exe -n 1 localhost; Start-Sleep 1 }\n"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=TMP)
        self.root = Path(self.tmp.name)
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        (self.root / 'tool.ps1').write_text(self.LOOP, encoding='utf-8')
        subprocess.run(['git', '-C', str(self.root), 'add', 'tool.ps1'], check=True)
        self.baseline = self.root / 'baseline.txt'

    def tearDown(self):
        self.tmp.cleanup()

    def gate(self, *extra):
        return lint.main(['--root', str(self.root), '--baseline', str(self.baseline), *extra])

    def test_a_new_loop_fails_without_a_baseline_line(self):
        self.assertEqual(self.gate(), 1)

    def test_the_baseline_admits_what_it_names_and_no_more(self):
        self.assertEqual(self.gate('--update-baseline'), 0)
        self.assertIn('1\ttool.ps1', self.baseline.read_text(encoding='utf-8'))
        self.assertEqual(self.gate(), 0)
        (self.root / 'tool.ps1').write_text(self.LOOP + self.LOOP.replace('localhost', '127.0.0.1'), encoding='utf-8')
        self.assertEqual(self.gate(), 1)

    def test_kit_mode_refuses_any_counted_loop(self):
        self.assertEqual(lint.main(['--files', str(self.root / 'tool.ps1')]), 1)


if __name__ == '__main__':
    unittest.main()
