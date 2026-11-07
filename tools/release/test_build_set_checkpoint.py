"""Crash/retry checks of the production scheduler on local stub recipes, never a driver build."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

import build_release_set
from test_build_release_set import Workspace, TOOL, OUT
from test_provenance import remove_tree


@unittest.skipUnless(shutil.which("pwsh") and os.name == "nt", "Windows pwsh scheduler")
class Checkpoints(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="set-checkpoint-", dir=OUT)
        self.ws = Workspace(self.tmp)
        self.process = None
        self.log = None
        self.orphans = []

    @staticmethod
    def stop_worker(worker):
        # Only the process identity this fixture captured, never a PID that may have been recycled.
        command = (f"$p=Get-Process -Id {int(worker['pid'])} -ErrorAction SilentlyContinue; "
                   f"if ($p -and $p.StartTime.ToUniversalTime().Ticks -eq {int(worker['start_ticks'])}) "
                   f"{{ & taskkill.exe /PID {int(worker['pid'])} /T /F; exit $LASTEXITCODE }}")
        subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, check=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            subprocess.run(["taskkill.exe", "/PID", str(self.process.pid), "/T", "/F"],
                           capture_output=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            self.process.wait(timeout=15)
        if self.log is not None:
            self.log.close()
            self.log = None
        for worker in self.orphans:
            self.stop_worker(worker)
        self.orphans.clear()

    def tearDown(self):
        self.stop()
        remove_tree(self.tmp)

    def start(self, manifest, fail_stop=False):
        self.ws.write(manifest)
        self.log = open(Path(self.tmp) / "scheduler.log", "wb")
        entry = TOOL
        if fail_stop:
            entry = self.ws.root / 'refuse-stop.ps1'
            entry.write_text("function taskkill.exe { $global:LASTEXITCODE = 5; 'fixture stop denied' }\n"
                             f"& '{TOOL}' @args -BuildTimeout 3\nexit $LASTEXITCODE\n")
        self.process = subprocess.Popen([
            "pwsh", "-NoProfile", "-File", str(entry), "-Manifest", str(self.ws.manifest_path),
            "-Out", str(self.ws.out), "-Root", str(self.ws.root), "-Work", str(self.ws.work),
            "-SizeGuard", str(self.ws.guard_dir / "size-guard.py"), "-MaxParallel", "1"],
            stdout=self.log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW,
            env=self.ws.child_environment())

    def wait_for(self, predicate):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                report = self.ws.report()
                if predicate(report):
                    return report
            except FileNotFoundError:
                pass
            # A partial JSON document is deliberately NOT swallowed: every observable receipt must be complete.
            if self.process.poll() is not None:
                self.fail((Path(self.tmp) / "scheduler.log").read_text(errors="replace"))
            time.sleep(.05)
        self.fail("scheduler did not reach expected checkpoint within 90 s")

    def test_kill_retains_completed_sibling_and_excludes_running_row(self):
        manifest = self.ws.manifest([self.ws.row("a", sleep=0), self.ws.row("b", sleep=45)])
        self.start(manifest)
        report = self.wait_for(lambda r: {b["id"]: b["status"] for b in r["builds"]} ==
                               {"a": "ok", "b": "running"})
        self.stop()
        self.assertEqual(set(build_release_set.previous_builds(self.ws.out)), {"a"})
        a = next(b for b in report["builds"] if b["id"] == "a")
        self.assertTrue(Path(a["log_dir"], "result.json").is_file())
        self.assertTrue(a["definition"])
        for output in a["outputs"]:
            self.assertEqual(build_release_set.sha256_file(output["artifact"]), output["sha256"])

    def test_retry_revokes_old_success_before_delete_and_preserves_attempt_logs(self):
        manifest = self.ws.manifest([self.ws.row("a", sleep=0)])
        code, output = self.ws.run(manifest, "-MaxParallel", "1")
        self.assertEqual(code, 0, output)
        old = self.ws.report()["builds"][0]
        old_log = Path(old["log_dir"])
        old_result = (old_log / "result.json").read_bytes()
        self.start(self.ws.manifest([self.ws.row("a", sleep=45, extra=("-Fail",))]))
        report = self.wait_for(lambda r: r["builds"][0]["status"] == "running")
        self.stop()
        self.assertEqual(build_release_set.previous_builds(self.ws.out), {})
        self.assertNotEqual(report["builds"][0]["log_dir"], str(old_log))
        self.assertEqual((old_log / "result.json").read_bytes(), old_result)
        code, output = self.ws.run(manifest, "-MaxParallel", "1")
        self.assertEqual(code, 0, output)
        self.assertEqual((old_log / "result.json").read_bytes(), old_result)
        self.assertNotEqual(self.ws.report()["builds"][0]["log_dir"], str(old_log))

    def test_parent_only_death_refuses_retry_until_its_exact_worker_exits(self):
        manifest = self.ws.manifest([self.ws.row("a", sleep=45)])
        self.start(manifest)

        def worker_started(report):
            row = report["builds"][0]
            return (row["status"] == "running" and
                    Path(row["log_dir"], "worker.json").is_file() and
                    (self.ws.events / "a.start").is_file())

        report = self.wait_for(worker_started)
        worker_path = Path(report["builds"][0]["log_dir"], "worker.json")
        worker = json.loads(worker_path.read_text())
        self.orphans.append(worker)
        plan = (self.ws.out / "plan.json").read_bytes()
        receipt = (self.ws.out / "release-report.json").read_bytes()
        command = (f"$p=Get-Process -Id {self.process.pid}; "
                   f"if ($p.StartTime.ToUniversalTime().Ticks -ne {int(worker['parent_start_ticks'])}) "
                   "{ throw 'fixture parent identity changed' }; Stop-Process -Id $p.Id -Force")
        subprocess.run(["pwsh", "-NoProfile", "-Command", command], capture_output=True, check=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)
        self.process.wait(timeout=15)
        self.log.close()
        self.log = None
        code, output = self.ws.run(manifest, "-MaxParallel", "1")
        self.assertNotEqual(code, 0, output)
        self.assertIn("Previous worker for a is still running", output)
        self.assertEqual((self.ws.out / "plan.json").read_bytes(), plan)
        self.assertEqual((self.ws.out / "release-report.json").read_bytes(), receipt)
        self.stop_worker(worker)
        self.orphans.clear()
        code, output = self.ws.run(self.ws.manifest([self.ws.row("a", sleep=0)]), "-MaxParallel", "1")
        self.assertEqual(code, 0, output)

    def test_late_worker_registers_then_refuses_wrong_parent_identity_before_work(self):
        log_dir = self.ws.root / "late-worker"
        work = self.ws.root / "untouched-work"
        work.mkdir()
        sentinel = work / "sentinel"
        sentinel.write_bytes(b"must remain")
        job = self.ws.root / "late-job.json"
        job.write_text(json.dumps({"build": {"dirs": {"work": str(work)}}, "log_dir": str(log_dir),
                                   "parent_pid": os.getpid(), "parent_start_ticks": 1}))
        result = subprocess.run(["pwsh", "-NoProfile", "-File", str(TOOL), "-Job", str(job)],
                                capture_output=True, text=True, timeout=30, env=self.ws.child_environment(),
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("this worker refuses to start", result.stdout + result.stderr)
        self.assertTrue((log_dir / "worker.json").is_file())
        self.assertFalse((log_dir / "result.json").exists())
        self.assertEqual(sentinel.read_bytes(), b"must remain")

    def test_failed_timeout_stop_preserves_worker_receipt_and_live_shared_tree(self):
        manifest = self.ws.manifest([self.ws.row('a', sleep=90)])
        self.start(manifest, fail_stop=True)
        report = self.wait_for(lambda r: r['builds'][0]['status'] == 'running' and
                               (self.ws.events / 'a.start').is_file())
        row = report['builds'][0]
        worker = json.loads(Path(row['log_dir'], 'worker.json').read_text())
        self.orphans.append(worker)
        plan = json.loads((self.ws.out / 'plan.json').read_text(encoding='utf-8-sig'))
        trees = [Path(t['path']) for t in plan['shared_trees']]
        self.assertTrue(trees)
        before = {str(t): (t / 'stub.ps1').read_bytes() for t in trees}
        self.process.wait(timeout=45)
        self.log.close()
        self.log = None
        self.assertNotEqual(self.process.returncode, 0)
        log = (Path(self.tmp) / 'scheduler.log').read_text(errors='replace')
        self.assertIn('taskkill exit 5', log)
        self.assertIn('not confirmed', log)
        after = self.ws.report()['builds'][0]
        self.assertEqual(after['status'], 'running')
        self.assertEqual(after['log_dir'], row['log_dir'])
        for tree in trees:
            self.assertEqual((tree / 'stub.ps1').read_bytes(), before[str(tree)])
        code, output = self.ws.run(manifest, '-Plan', timeout=30)
        self.assertNotEqual(code, 0, output)
        self.assertIn('Previous worker for a is still running', output)
        self.stop_worker(worker)
        self.orphans.clear()

    def test_missing_worker_receipt_requires_new_guard_schema(self):
        # Invoke the real startup guard without a build or tool-context scan.
        script = self.ws.root / "startup-guard.ps1"
        script.write_text("""param([string]$ToolPath, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($ToolPath,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'production source does not parse' }
foreach ($name in 'Read-Json','Test-ExactProcess','Assert-NoPreviousWorker') {
    $function=$ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] },$true) |
        Where-Object Name -eq $name
    if (@($function).Count -ne 1) { throw "missing production function $name" }
    . ([scriptblock]::Create($function.Extent.Text))
}
Assert-NoPreviousWorker $OutputDirectory
""")
        self.ws.out.mkdir()
        receipt = {"builds": [{"id": "a", "status": "running", "log_dir": str(self.ws.out / "logs/a")}]}
        report_path = self.ws.out / "release-report.json"
        for schema, allowed in ((None, False), (0, False), (1, True)):
            if schema is not None:
                receipt["worker_guard_schema"] = schema
            report_path.write_text(json.dumps(receipt))
            before = report_path.read_bytes()
            result = subprocess.run(["pwsh", "-NoProfile", "-File", str(script), "-ToolPath", str(TOOL),
                                     "-OutputDirectory", str(self.ws.out)], capture_output=True, text=True,
                                    timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode == 0, allowed, result.stdout + result.stderr)
            if not allowed:
                self.assertIn("legacy running receipt without worker identity", result.stdout + result.stderr)
            self.assertEqual(report_path.read_bytes(), before)

    def test_second_scheduler_cannot_replace_the_active_plan(self):
        manifest = self.ws.manifest([self.ws.row('a', sleep=45)])
        self.start(manifest)
        self.wait_for(lambda r: r['builds'][0]['status'] == 'running')
        before = (self.ws.out / 'plan.json').read_bytes()
        code, output = self.ws.run(manifest, '-Plan', timeout=30)
        self.assertNotEqual(code, 0, output)
        self.assertIn('Output directory is busy', output)
        self.assertEqual((self.ws.out / 'plan.json').read_bytes(), before)

    def test_default_retry_keeps_actual_completed_build_and_rebuild_is_explicit(self):
        manifest = self.ws.manifest([self.ws.row('a', sleep=0)])
        code, output = self.ws.run(manifest)
        self.assertEqual(code, 0, output)
        first = self.ws.report()['builds'][0]
        code, output = self.ws.run(manifest)
        self.assertEqual(code, 0, output)
        self.assertIn('automatic (recommended)', output)
        kept = self.ws.report()['builds'][0]
        self.assertTrue(kept['kept'])
        self.assertEqual(kept['log_dir'], first['log_dir'])
        code, output = self.ws.run(manifest, '-Rebuild')
        self.assertEqual(code, 0, output)
        self.assertIn('RECOMMENDED: omit -Rebuild', output)
        rebuilt = self.ws.report()['builds'][0]
        self.assertFalse(rebuilt.get('kept', False))
        self.assertNotEqual(rebuilt['log_dir'], first['log_dir'])


if __name__ == "__main__":
    unittest.main()
