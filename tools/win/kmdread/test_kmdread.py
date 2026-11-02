"""Host tests of kmd-reader.ps1 (BD-051): the in-process reader against a fake driver, with no adapter and no lab.

test\\build-fake.ps1 compiles test\\fake_driver.c, which includes the unchanged bc250kmd_cli.c, into fake_cli.exe and a
fake bc250control.dll. So the texts of the CLI and of the reader come from the same driver answers, and each test
compares them byte for byte. The reader runs under Windows PowerShell 5.1 (the lab's shell) and, when it is present,
PowerShell 7.

    python -B -m unittest discover -s tools/win/kmdread -p "test_*.py"

BC250_TEST_TMP names the scratch directory (default: the system temp directory). The fake is built once per input hash.
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
READER = HERE / 'kmd-reader.ps1'


def find_kits():
    """The SDK NuGet packages: BC250_KITS, else toolchain\\nuget under BC250_ROOT or the first parent that has it."""
    if os.environ.get('BC250_KITS'):
        return Path(os.environ['BC250_KITS'])
    for base in ([Path(os.environ['BC250_ROOT'])] if os.environ.get('BC250_ROOT') else []) + list(REPO.parents):
        if (base / 'toolchain' / 'nuget').is_dir():
            return base / 'toolchain' / 'nuget'
    return Path('toolchain') / 'nuget'


KITS = find_kits()
TMP = Path(os.environ.get('BC250_TEST_TMP', tempfile.gettempdir())) / 'kmdread'
PS51 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
PWSH = shutil.which('pwsh')
SHELLS = [s for s in (str(PS51) if PS51.exists() else None, PWSH) if s]


def fake_inputs():
    names = [HERE / 'test' / 'fake_driver.c', HERE / 'test' / 'build-fake.ps1',
             REPO / 'tools' / 'win' / 'bc250kmd_cli' / 'bc250kmd_cli.c', REPO / 'driver' / 'kmd' / 'bc250kmd_escape.h']
    h = hashlib.sha256()
    for n in names:
        h.update(n.read_bytes())
    return h.hexdigest()[:16]


_FAKE = None


def fake():
    """The directory with fake_cli.exe and the fake bc250control.dll, built once per input hash."""
    global _FAKE
    if _FAKE:
        return _FAKE
    out = TMP / ('fake-' + fake_inputs())
    if not (out / 'bc250control.dll').exists() or not (out / 'fake_cli.exe').exists():
        if not PWSH:
            raise unittest.SkipTest('pwsh is needed to build the fake driver')
        r = subprocess.run([PWSH, '-NoProfile', '-File', str(HERE / 'test' / 'build-fake.ps1'), '-Kits', str(KITS),
                            '-Out', str(out)], capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            raise AssertionError('build-fake.ps1 failed:\n' + r.stdout[-3000:] + r.stderr[-3000:])
    _FAKE = out
    return out


def env(**extra):
    e = dict(os.environ)
    for k in [k for k in e if k.startswith('BC250_FAKE_')]:
        del e[k]
    e.update({k: str(v) for k, v in extra.items()})
    return e


def ps(shell, body, timeout=120, **extra):
    """Runs body under shell with the reader dot-sourced and loaded from the fake DLL."""
    script = TMP / ('case-%d-%d.ps1' % (os.getpid(), time.monotonic_ns()))
    script.write_text("$ErrorActionPreference = 'Stop'\n. '%s'\n$null = Initialize-KmdReader -Dll '%s'\n%s\n"
                      % (READER, fake() / 'bc250control.dll', body), encoding='utf-8-sig')
    try:
        r = subprocess.run([shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                           capture_output=True, timeout=timeout, env=env(**extra))
    finally:
        script.unlink()
    return r


def cli(*args, **extra):
    """The fake CLI's stdout bytes for args, decoded as the reader decodes text (Latin-1, line ends kept)."""
    r = subprocess.run([str(fake() / 'fake_cli.exe'), *args], capture_output=True, timeout=60, env=env(**extra))
    return r.returncode, r.stdout.decode('latin-1')


class Equivalence(unittest.TestCase):
    """The reader's text is the CLI's text for the same driver answers."""

    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        if not SHELLS:
            self.skipTest('no PowerShell')

    def test_clock_line_is_the_cli_line(self):
        for temp in (51234, 87001, 0):
            code, text = cli('clock', 'read', BC250_FAKE_TEMP_MC=temp)
            self.assertEqual(code, 0)
            for shell in SHELLS:
                r = ps(shell, "$c = Read-KmdClock\n[Console]::Out.Write($c.Ok.ToString() + '|' + $c.Line)",
                       BC250_FAKE_TEMP_MC=temp)
                self.assertEqual(r.returncode, 0, r.stderr.decode('latin-1'))
                self.assertEqual(r.stdout.decode('latin-1'), 'True|' + text.rstrip('\r\n'), shell)

    def test_health_line_is_the_cli_line(self):
        for flags, age in ((23, 1500), (16, 99000)):
            code, text = cli('health', 'read', BC250_FAKE_HEALTH_FLAGS=flags, BC250_FAKE_HEALTH_AGE_MS=age)
            self.assertEqual(code, 0)
            for shell in SHELLS:
                r = ps(shell, "[Console]::Out.Write((Read-KmdHealth))", BC250_FAKE_HEALTH_FLAGS=flags,
                       BC250_FAKE_HEALTH_AGE_MS=age)
                self.assertEqual(r.returncode, 0, r.stderr.decode('latin-1'))
                self.assertEqual(r.stdout.decode('latin-1'), text.rstrip('\r\n'), shell)

    def test_log_text_is_the_cli_text(self):
        # One page, several pages (64 lines a page), the end of the ring, past the end, and an empty ring.
        for total, start in ((150, 0), (150, 5), (150, 149), (150, 150), (150, 400), (300, 10), (0, 0)):
            code, text = cli('log', str(start), BC250_FAKE_LOG_TOTAL=total)
            self.assertEqual(code, 0)
            for shell in SHELLS:
                r = ps(shell, "[Console]::Out.Write((Read-KmdLog -From %d))" % start, BC250_FAKE_LOG_TOTAL=total)
                self.assertEqual(r.returncode, 0, r.stderr.decode('latin-1'))
                self.assertEqual(r.stdout.decode('latin-1'), text, '%s total %d from %d' % (shell, total, start))

    def test_a_refusal_is_a_loss_with_the_cli_line(self):
        code, text = cli('log', '0', BC250_FAKE_NOT_ADMIN=1)
        self.assertEqual(code, 3)
        for shell in SHELLS:
            r = ps(shell, "$t = Read-KmdLog -From 0\n[Console]::Out.Write(($null -eq $t).ToString() + '|' + (Get-KmdReaderLoss))",
                   BC250_FAKE_NOT_ADMIN=1)
            self.assertEqual(r.stdout.decode('latin-1'), 'True|' + text.rstrip('\r\n'), shell)


class Bounds(unittest.TestCase):
    """A hung escape is a loss inside the bound, and no second escape goes out while it is out."""

    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        if not SHELLS:
            self.skipTest('no PowerShell')

    def test_hung_escape(self):
        for shell in SHELLS:
            block = TMP / ('block-%d' % time.monotonic_ns())
            count = TMP / ('count-%d' % time.monotonic_ns())
            body = r"""
$c = Read-KmdClock                                    # one answered reading before the hang
Set-Content -LiteralPath '%(block)s' -Value 'x'
$sw = [Diagnostics.Stopwatch]::StartNew(); $a = Read-KmdClock -TimeoutMs 700; $t1 = $sw.ElapsedMilliseconds
$sw.Restart(); $b = Read-KmdLog -From 0 -TimeoutMs 700; $t2 = $sw.ElapsedMilliseconds; $lossB = Get-KmdReaderLoss
$sentWhileOut = (Get-Item -LiteralPath '%(count)s').Length
Remove-Item -LiteralPath '%(block)s'
Start-Sleep -Milliseconds 300
$d = Read-KmdClock
'{0}|{1}|{2}|{3}|{4}|{5}|{6}|{7}' -f $c.Ok, $a.Ok, $a.Reason, $t1, ($null -eq $b), $t2, $sentWhileOut, $d.Ok
'loss b: ' + $lossB
""" % {'block': block, 'count': count}
            r = ps(shell, body, BC250_FAKE_BLOCK_FILE=block, BC250_FAKE_COUNT_FILE=count)
            out = r.stdout.decode('latin-1').splitlines()
            self.assertEqual(r.returncode, 0, r.stderr.decode('latin-1'))
            c_ok, a_ok, a_reason, t1, b_null, t2, sent, d_ok = out[0].split('|')
            self.assertEqual((c_ok, a_ok, b_null, d_ok), ('True', 'False', 'True', 'True'), out)
            self.assertEqual(a_reason, 'no answer in 700 ms')
            self.assertLess(int(t1), 1500, 'the hung reading came back after its bound')
            self.assertLess(int(t2), 200, 'a reading waited although one was still out')
            self.assertIn('has not come back; nothing sent', out[1])
            self.assertEqual(int(sent), 2, 'escapes sent: the one before the hang and the hung one, nothing more')
            for p in (block, count):
                if p.exists():
                    p.unlink()


# ---- no process per reading: the job object of the shell counts every process it and its children start ----------

class JOBOBJECT_BASIC_ACCOUNTING_INFORMATION(ctypes.Structure):
    _fields_ = [('TotalUserTime', ctypes.c_int64), ('TotalKernelTime', ctypes.c_int64),
                ('ThisPeriodTotalUserTime', ctypes.c_int64), ('ThisPeriodTotalKernelTime', ctypes.c_int64),
                ('TotalPageFaultCount', wt.DWORD), ('TotalProcesses', wt.DWORD), ('ActiveProcesses', wt.DWORD),
                ('TotalTerminatedProcesses', wt.DWORD)]


def job_processes(job):
    info = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION()
    ok = ctypes.windll.kernel32.QueryInformationJobObject(job, 1, ctypes.byref(info), ctypes.sizeof(info), None)
    if not ok:
        raise OSError(ctypes.get_last_error(), 'QueryInformationJobObject')
    return info.TotalProcesses


class NoProcess(unittest.TestCase):
    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        if not SHELLS:
            self.skipTest('no PowerShell')

    def test_readings_start_no_process(self):
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.CreateJobObjectW.restype = wt.HANDLE
        k32.OpenProcess.restype = wt.HANDLE
        for shell in SHELLS:
            script = TMP / ('noproc-%d.ps1' % time.monotonic_ns())
            # A positive control first: one process start, which the job must count.
            script.write_text(
                "$ErrorActionPreference = 'Stop'\n. '%s'\n$null = Initialize-KmdReader -Dll '%s'\n"
                "$null = Read-KmdClock; $null = Read-KmdLog -From 0\n"
                "[Console]::Out.WriteLine('ready'); [Console]::Out.Flush(); $null = [Console]::In.ReadLine()\n"
                "$null = & '%s' clock read\n"
                "[Console]::Out.WriteLine('control'); [Console]::Out.Flush(); $null = [Console]::In.ReadLine()\n"
                "$ok = 0; for ($i = 0; $i -lt 40; $i++) { if ((Read-KmdClock).Ok) { $ok++ }; if (Read-KmdLog -From $i) { $ok++ } }\n"
                "[Console]::Out.WriteLine('done ' + $ok); [Console]::Out.Flush(); $null = [Console]::In.ReadLine()\n"
                % (READER, fake() / 'bc250control.dll', fake() / 'fake_cli.exe'), encoding='utf-8-sig')
            job = k32.CreateJobObjectW(None, None)
            self.assertTrue(job)
            p = subprocess.Popen([shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env(),
                                 creationflags=0x00000004)            # CREATE_SUSPENDED: in the job before it runs
            try:
                handle = k32.OpenProcess(0x1F0FFF, False, p.pid)
                self.assertTrue(k32.AssignProcessToJobObject(wt.HANDLE(job), wt.HANDLE(handle)))
                # Resume the main thread: NtResumeProcess resumes every thread of the suspended process.
                ctypes.WinDLL('ntdll').NtResumeProcess(wt.HANDLE(handle))
                k32.CloseHandle(wt.HANDLE(handle))
                self.assertEqual(p.stdout.readline().strip(), b'ready')
                ready = job_processes(job)
                p.stdin.write(b'\n'); p.stdin.flush()
                self.assertEqual(p.stdout.readline().strip(), b'control')
                control = job_processes(job)
                p.stdin.write(b'\n'); p.stdin.flush()
                self.assertEqual(p.stdout.readline().strip(), b'done 80')
                done = job_processes(job)
                p.stdin.write(b'\n'); p.stdin.flush()
                p.wait(timeout=60)
            finally:
                if p.poll() is None:
                    p.kill()
                    p.wait(timeout=30)
                for stream in (p.stdin, p.stdout, p.stderr):
                    stream.close()
                k32.CloseHandle(wt.HANDLE(job))
                script.unlink()
            self.assertEqual(control, ready + 1, 'the job did not count the control process start')
            self.assertEqual(done, control, '80 readings started %d processes' % (done - control))


class Loading(unittest.TestCase):
    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        if not SHELLS:
            self.skipTest('no PowerShell')

    def run_init(self, dll):
        script = TMP / ('init-%d.ps1' % time.monotonic_ns())
        script.write_text(". '%s'\ntry { $null = Initialize-KmdReader -Dll '%s'; 'loaded' } catch { 'refused: ' + $_.Exception.Message }\n"
                          % (READER, dll), encoding='utf-8-sig')
        try:
            r = subprocess.run([SHELLS[0], '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                               capture_output=True, text=True, timeout=120, env=env())
        finally:
            script.unlink()
        return r.stdout.strip()

    def test_missing_dll_is_refused(self):
        self.assertIn('refused: no bc250control.dll at', self.run_init(TMP / 'absent' / 'bc250control.dll'))

    def test_dll_without_the_exports_is_refused(self):
        kernel32 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'kernel32.dll'
        self.assertIn('has no export Bc250ClockControl', self.run_init(kernel32))


class ArmSampler(unittest.TestCase):
    """The HIP arm library takes the reader as its sampler when a kit stages kmd-reader.ps1 beside it."""

    ARMLIB = REPO / 'compute' / 'hip' / 'lab' / 'armlib.ps1'

    def setUp(self):
        TMP.mkdir(parents=True, exist_ok=True)
        if not SHELLS:
            self.skipTest('no PowerShell')

    def run_arm(self, shell, staged, **extra):
        stage = TMP / ('arm-%d-%d' % (os.getpid(), time.monotonic_ns()))
        stage.mkdir()
        try:
            shutil.copy2(self.ARMLIB, stage / 'armlib.ps1')
            if staged:
                shutil.copy2(READER, stage / 'kmd-reader.ps1')
            script = stage / 'case.ps1'
            script.write_text(
                "$ErrorActionPreference = 'Stop'\n"
                ". (Join-Path $PSScriptRoot 'armlib.ps1')\n"
                "$s = New-ArmDefaultSampler -TimeoutSec 5\n"
                "'kind=' + $script:ArmSamplerKind\n"
                "if ($script:ArmSamplerKind -like 'in-process *') {\n"
                "  $r = Invoke-ArmBoundedSample -Sampler $s -BudgetSec 4\n"
                "  'ok=' + $r.Ok + ' tctl=' + $r.Tctl + ' reason=' + $r.Reason\n"
                "}\n", encoding='utf-8-sig')
            e = dict(BC250_KMD_READER_DLL=fake() / 'bc250control.dll', **extra)
            r = subprocess.run([shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
                               capture_output=True, text=True, timeout=120, env=env(**e))
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_a_staged_reader_is_the_sampler(self):
        for shell in SHELLS:
            out = self.run_arm(shell, True, BC250_FAKE_TEMP_MC=61250)
            self.assertIn('kind=in-process ', out, shell)
            self.assertIn('ok=True tctl=61.25 ', out, shell)

    def test_the_band_check_holds_for_the_reader(self):
        out = self.run_arm(SHELLS[0], True, BC250_FAKE_TEMP_MC=250000)
        self.assertIn('ok=False', out)
        self.assertIn('outside 5 to 130 C', out)

    def test_without_the_reader_the_cli_sampler_stays(self):
        out = self.run_arm(SHELLS[0], False)
        self.assertIn('kind=cli, kmd-reader.ps1 is not staged beside armlib.ps1', out)


if __name__ == '__main__':
    unittest.main()
