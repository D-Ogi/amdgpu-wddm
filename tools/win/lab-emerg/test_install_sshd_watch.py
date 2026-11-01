"""Host tests of the watch count in install-sshd-watch.ps1 (BD-051).

In Windows PowerShell 5.1 a function's @() result unrolls on return: with exactly one match the caller gets a bare CIM
instance, and its .Count is empty, not 1. bd051-k1 (2026-11-01) read one running watch as none: the install printed
"watch processes " with no count, and the uninstall wrote no stop file and ended no process.

The test takes Get-WatchProcess, Get-WatchCount and Stop-Watch out of the script with the PowerShell parser and runs
them under the local Windows PowerShell 5.1, the engine of the lab, against a hidden stand-in process (Start-Sleep).
The stand-in's command line holds a unique token, and the test sets the watch pattern to that token, so no other
process can match. No task is registered, no service is touched, no lab and no network. Run:
    python -m unittest discover -s tools/win/lab-emerg -p "test_*.py"
"""
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
INSTALL = HERE / 'install-sshd-watch.ps1'
POWERSHELL = shutil.which('powershell.exe')

# The harness defines the three functions from the script's own text and the script variables that they read.
HARNESS = r'''
param([Parameter(Mandatory)][string]$Install, [Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'
# The token comes from the environment: on this command line it would make the harness match (and stop) itself.
$Token = $env:BC250_WATCH_TOKEN
if (!$Token) { throw 'BC250_WATCH_TOKEN missing' }
$ast = [Management.Automation.Language.Parser]::ParseFile($Install, [ref]$null, [ref]$null)
$defs = $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)
foreach ($name in 'Get-WatchProcess', 'Get-WatchCount', 'Stop-Watch') {
    $d = @($defs | Where-Object { $_.Name -eq $name })
    if ($d.Count -ne 1) { throw "function ${name}: $($d.Count) definitions" }
    . ([scriptblock]::Create($d[0].Extent.Text))
}
$watchPattern = [regex]::Escape($Token)
$watchDir = $Dir
$stopFile = Join-Path $Dir 'stop'
$stopWaitSeconds = 2
$r = [ordered]@{}
$r.bare = [string](Get-WatchProcess).Count
$r.count_before = Get-WatchCount
$sw = [Diagnostics.Stopwatch]::StartNew()
Stop-Watch
$r.stop_seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
$r.count_after = Get-WatchCount
$r.stop_file = Test-Path -LiteralPath $stopFile
'HARNESS ' + ($r | ConvertTo-Json -Compress)
'''


@unittest.skipUnless(POWERSHELL, 'needs Windows PowerShell 5.1, the engine of the lab')
class WatchCount(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='bc250-install-watch-', dir=os.environ.get('BC250_TEST_TMP')))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        (self.tmp / 'harness.ps1').write_text(HARNESS, encoding='utf-8')

    def stand_in(self, token):
        p = subprocess.Popen([POWERSHELL, '-NoProfile', '-Command', f'Start-Sleep -Seconds 60 # {token}'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self.addCleanup(lambda: p.poll() is None and p.kill())
        return p

    def harness(self, token):
        done = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                               str(self.tmp / 'harness.ps1'), '-Install', str(INSTALL),
                               '-Dir', str(self.tmp / 'sshd-watch')],
                              capture_output=True, text=True, timeout=90,
                              env=dict(os.environ, BC250_WATCH_TOKEN=token),
                              creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        lines = [line for line in done.stdout.splitlines() if line.startswith('HARNESS ')]
        self.assertTrue(lines, done.stdout + done.stderr)
        return json.loads(lines[-1][len('HARNESS '):])

    def test_one_watch_is_counted_and_stopped(self):
        token = 'bc250-watch-count-' + uuid.uuid4().hex
        p = self.stand_in(token)
        time.sleep(1.5)  # the stand-in is in Win32_Process before the harness looks
        r = self.harness(token)
        # The positive control of the defect: the bare count of one match is empty on this engine.
        self.assertEqual(r['bare'], '')
        self.assertEqual(r['count_before'], 1)
        self.assertTrue(r['stop_file'])  # the old Stop-Watch returned before this
        self.assertEqual(r['count_after'], 0)
        self.assertIsNotNone(p.wait(10))

    def test_no_watch_returns_at_once(self):
        r = self.harness('bc250-watch-count-none-' + uuid.uuid4().hex)
        self.assertEqual((r['count_before'], r['count_after'], r['stop_file']), (0, 0, False))
        self.assertLess(r['stop_seconds'], 2)

    def test_no_bare_count_of_a_function_result(self):
        # Every count of the watch goes through Get-WatchCount (or an @() at the call).
        for path in sorted(HERE.glob('*.ps1')):
            code = [line for line in path.read_text(encoding='utf-8-sig').splitlines()
                    if not line.lstrip().startswith('#')]
            text = '\n'.join(code)
            with self.subTest(file=path.name):
                self.assertIsNone(re.search(r'(?<!@)\(\s*Get-WatchProcess\b[^)]*\)\s*\.Count', text))


if __name__ == '__main__':
    unittest.main()
