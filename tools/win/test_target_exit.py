"""Host tests for the exit codes of `target.py run` and `ps` (BD-267).

The lab's sshd runs each remote command line as `powershell.exe -c "<command line>"` (Windows PowerShell 5.1).
That shell ends with 1 for any failed native call, so a remote exit 3 or 9 arrived here as 1. The fake remote
below runs the command line through the local Windows PowerShell 5.1 the same way; no network, no lab. Run:
    python -m unittest discover -s tools/win -p "test_target*.py"
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import target  # noqa: E402

POWERSHELL = shutil.which("powershell.exe")


class FakeRemote(target.Target):
    """A Target whose ssh runs the command line through the local sshd shell stand-in."""

    def __init__(self, tmp_dir):
        self.cfg = {"tmp_dir": tmp_dir}
        self.sent = []

    def push(self, local_paths, remote_dir=None, timeout=300):
        remote_dir = remote_dir or self.cfg["tmp_dir"]
        for path in local_paths:
            shutil.copy(path, remote_dir)

    def ssh(self, remote_command, timeout=120, binary=False, stdin=None, options=()):
        self.sent.append(remote_command)
        return subprocess.run([POWERSHELL, "-NoProfile", "-c", remote_command], capture_output=True,
                              text=True, timeout=timeout)


class CommandLines(unittest.TestCase):
    """The command lines alone, on every host."""

    def test_run_script_ends_with_the_exit_pass(self):
        t = target.Target.__new__(target.Target)
        t.cfg = {"tmp_dir": "C:\\BC250\\tmp"}
        sent = []
        t.push = lambda paths, remote_dir=None, timeout=300: None
        t.ssh = lambda command, timeout=None: sent.append(command)
        t.run_script("lab/x.ps1", ["-Mode", "a;b"])
        self.assertEqual(sent, ['powershell -NoProfile -ExecutionPolicy Bypass -File "C:\\BC250\\tmp\\x.ps1" '
                                '-Mode "a;b"; exit $LASTEXITCODE'])

    def test_inline_status_starts_on_its_own_line(self):
        import base64
        line = target.encoded_command("Get-Date # a comment")
        self.assertTrue(line.startswith("powershell -NoProfile -EncodedCommand "))
        self.assertTrue(line.endswith("; exit $LASTEXITCODE"))
        script = base64.b64decode(line.split()[3]).decode("utf-16-le")
        self.assertTrue(script.startswith("Get-Date # a comment\n$bc250Ok = $?"))


@unittest.skipUnless(POWERSHELL, "needs Windows PowerShell as the sshd shell stand-in")
class ExitCodes(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bc250-target-exit-", dir=os.environ.get("BC250_TEST_TMP"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.remote = FakeRemote(self.dir)

    def script(self, name, text):
        path = os.path.join(self.dir, "local-" + name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        os.makedirs(os.path.join(self.dir, "remote"), exist_ok=True)
        return path

    def ps(self, text, args=()):
        path = self.script("kit.ps1", text)
        return self.remote.run_script(path, args, remote_dir=os.path.join(self.dir, "remote"))

    def test_ps_passes_the_script_exit_code(self):
        for code in (0, 2, 3, 9):
            with self.subTest(code=code):
                done = self.ps(f"Write-Output 'VERDICT {code}'\nexit {code}\n")
                self.assertEqual(done.returncode, code)
                self.assertIn(f"VERDICT {code}", done.stdout)

    def test_ps_without_the_pass_loses_the_code(self):
        # Positive control of the stand-in: the old command line gives 1 for a script exit 3.
        path = self.script("kit.ps1", "exit 3\n")
        remote = os.path.join(self.dir, "remote")
        self.remote.push([path], remote)
        old = f'powershell -NoProfile -ExecutionPolicy Bypass -File "{remote}\\local-kit.ps1"'
        self.assertEqual(self.remote.ssh(old).returncode, 1)

    def test_ps_passes_arguments_with_a_semicolon(self):
        done = self.ps("param($Expect)\nif ($Expect -eq 'a;b') { exit 0 } else { exit 4 }\n", ["-Expect", "a;b"])
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_run_passes_a_native_exit_code(self):
        self.assertEqual(self.remote.run_status("cmd /c exit 3").returncode, 3)
        self.assertEqual(self.remote.run_status("cmd /c exit 9").returncode, 9)

    def test_run_passes_an_explicit_exit(self):
        self.assertEqual(self.remote.run_status("Write-Output x; exit 2").returncode, 2)

    def test_run_keeps_zero_for_a_cmdlet_only_command(self):
        done = self.remote.run_status("Get-Date | Out-Null; Write-Output ok")
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stdout.strip(), "ok")

    def test_run_keeps_one_for_a_failed_cmdlet(self):
        missing = os.path.join(self.dir, "no-such-file")
        done = self.remote.run_status(f"Get-Item -LiteralPath '{missing}'")
        self.assertEqual(done.returncode, 1)

    def test_run_judges_by_the_last_statement(self):
        # As before: an earlier failed native call does not fail a command whose last statement succeeds.
        self.assertEqual(self.remote.run_status("cmd /c exit 5; Write-Output after").returncode, 0)

    def test_run_with_a_trailing_comment(self):
        self.assertEqual(self.remote.run_status("cmd /c exit 7 # the end").returncode, 7)

    def test_run_returns_stdout(self):
        self.assertEqual(self.remote.run("Write-Output 'line one'").strip(), "line one")


if __name__ == "__main__":
    unittest.main()
