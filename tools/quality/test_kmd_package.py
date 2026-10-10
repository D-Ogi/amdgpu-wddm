"""Offline controls for package workspace selection and status-map profiles."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools/quality/kmd-build-context.ps1"
BASE = Path(os.environ.get("BC250_TEST_OUT", ROOT / "build/kmd-package-tests")).resolve()
BASE.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location("kmd_status_profile", ROOT / "tools/quality/kmd_status_profile.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def ps_quote(value):
    return "'" + str(value).replace("'", "''") + "'"


class PackageContextTests(unittest.TestCase):
    def setUp(self):
        self.base = BASE / ("package-context-" + self._testMethodName)
        self.base.mkdir(parents=True, exist_ok=True)
        self.workspace = self.base / "workspace with spaces"
        self.kits = self.workspace / "toolchain/nuget"
        self.kits.mkdir(parents=True, exist_ok=True)
        self.repo = self.workspace / "scratch/review/wt"
        self.repo.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True, capture_output=True)
        self.env = os.environ.copy()
        self.env.pop("BC250_ROOT", None)
        self.env["TEMP"] = self.env["TMP"] = str(self.base)

    def resolve(self, explicit="", environment="", kits=None, legacy=False):
        env = self.env.copy()
        if environment:
            env["BC250_ROOT"] = str(environment)
        if legacy:
            script = "$repo=" + ps_quote(self.repo) + "; $QualityWorkspace=''; "
            script += "if(!$QualityWorkspace){$QualityWorkspace=if($env:BC250_ROOT){$env:BC250_ROOT}else{Split-Path $repo}}; "
            script += "@{workspace=$QualityWorkspace;source='legacy'} | ConvertTo-Json -Compress"
        else:
            script = ". " + ps_quote(HELPER) + "; Resolve-Bc250PackageContext -Repo " + ps_quote(self.repo)
            script += " -Kits " + ps_quote(kits or self.kits) + " -QualityWorkspace " + ps_quote(explicit)
            script += " | ConvertTo-Json -Compress"
        return subprocess.run(["pwsh", "-NoProfile", "-Command", "$ErrorActionPreference='Stop'; " + script],
                              capture_output=True, text=True, env=env, timeout=30)

    def check_resolution(self, run, origin):
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout.lstrip("\ufeff"))
        self.assertEqual(Path(result["workspace"]), self.workspace)
        self.assertEqual(result["source"], origin)

    def test_nested_worktree_uses_kits(self):
        self.check_resolution(self.resolve(), "Kits")
        old = self.resolve(legacy=True)
        with self.assertRaises(AssertionError):
            self.check_resolution(old, "Kits")

    def test_explicit_precedes_environment(self):
        self.check_resolution(self.resolve(self.workspace, self.base / "missing"), "QualityWorkspace")

    def test_environment(self):
        self.check_resolution(self.resolve(environment=self.workspace), "BC250_ROOT")

    def test_repository_config_relative_to_repo(self):
        subprocess.run(["git", "-C", str(self.repo), "config", "bc250.workspace", "../../.."], check=True)
        self.check_resolution(self.resolve(), "git:bc250.workspace")

    def test_invalid_explicit_does_not_fallback(self):
        self.assertNotEqual(self.resolve(self.base / "missing").returncode, 0)

    def test_invalid_environment_does_not_fallback(self):
        self.assertNotEqual(self.resolve(environment=self.base / "missing").returncode, 0)

    def test_invalid_config_does_not_fallback(self):
        subprocess.run(["git", "-C", str(self.repo), "config", "bc250.workspace", "missing"], check=True)
        self.assertNotEqual(self.resolve().returncode, 0)

    def test_mismatched_kits_refused(self):
        other = self.base / "other/toolchain/nuget"
        other.mkdir(parents=True, exist_ok=True)
        run = self.resolve(self.workspace, kits=other)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("inconsistent compiler", run.stderr)

    def test_nonstandard_kits_cannot_guess_parent(self):
        other = self.base / "kits"
        other.mkdir(exist_ok=True)
        self.assertNotEqual(self.resolve(kits=other).returncode, 0)


class StatusProfileTests(unittest.TestCase):
    def setUp(self):
        self.base = BASE / ("status-profile-" + self._testMethodName)
        self.base.mkdir(parents=True, exist_ok=True)
        self.repo = self.base / "repo"
        self.source = self.repo / "docs/status/status.json"
        self.source.parent.mkdir(parents=True, exist_ok=True)
        self.source.write_text("old commit snapshot\n", encoding="utf-8")
        self.outputs = {self.source: "new code snapshot\n"}

    def test_full_stale_fails_and_does_not_rewrite(self):
        self.assertEqual(profile.evaluate_outputs("Full", self.repo, self.outputs, self.base / "full"), 1)
        self.assertEqual(self.source.read_text(), "old commit snapshot\n")

    def test_package_records_difference_without_touching_artifact(self):
        out = self.base / "package"
        self.assertEqual(profile.evaluate_outputs("KmdPackage", self.repo, self.outputs, out), 0)
        receipt = json.loads((out / "result.json").read_text())
        self.assertEqual(receipt["tracked_output_freshness"], "profile-exclusion")
        self.assertEqual(receipt["different_tracked_outputs"], ["docs/status/status.json"])
        self.assertEqual((out / "generated/docs/status/status.json").read_text(), "new code snapshot\n")
        self.assertEqual(self.source.read_text(), "old commit snapshot\n")

    def test_fresh_full_passes(self):
        self.assertEqual(profile.evaluate_outputs("Full", self.repo, {self.source: self.source.read_text()},
                                                  self.base / "fresh"), 0)

    def test_invalid_profile_fails(self):
        with self.assertRaises(ValueError):
            profile.evaluate_outputs("skip-all", self.repo, self.outputs, self.base / "bad")

    def test_actual_generator_errors_still_fail_both_profiles(self):
        env = os.environ.copy()
        env["TEMP"] = env["TMP"] = str(self.base)
        for selected in ("Full", "KmdPackage"):
            run = subprocess.run(["python", str(ROOT / "tools/quality/kmd_status_profile.py"),
                                  "--repo", str(ROOT), "--kits", str(self.base / "missing-kits"),
                                  "--out", str(self.base / selected), "--profile", selected],
                                 capture_output=True, text=True, env=env, timeout=40)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("not found", run.stderr)

    def test_build_and_quick_keep_gates_wired(self):
        build = (ROOT / "driver/kmd/build.ps1").read_text(encoding="utf-8-sig")
        quick = (ROOT / "tools/quality/quick.ps1").read_text(encoding="utf-8-sig")
        cmd = (ROOT / "tools/quality/quick.cmd").read_text(encoding="utf-8-sig")
        self.assertIn("$repo 'KmdPackage' $Kits", build)
        self.assertIn("Fast quality gates failed; KMD package not built", build)
        self.assertIn("Source/artifact identity verification failed", build)
        self.assertIn("[string]$Profile='Full'", quick)
        self.assertIn("kmd_status_profile.py", quick)
        self.assertIn("-s \"$repo\\tools\\docs\"", quick)
        self.assertIn("Check 'release-provenance'", quick)
        self.assertIn("Check 'kmd-contract'", quick)
        self.assertIn('-Profile "%BC250_QUALITY_PROFILE%" -Kits "%BC250_QUALITY_KITS%"', cmd)


class QuickCommandTests(unittest.TestCase):
    def test_actual_cmd_forwards_profile_and_kits(self):
        base = BASE / "quick-command"
        fake = base / "fake-bin"
        fake.mkdir(parents=True, exist_ok=True)
        workspace = base / "workspace"
        kits = workspace / "toolchain/nuget"
        kits.mkdir(parents=True, exist_ok=True)
        (workspace / "scratch/tmp").mkdir(parents=True, exist_ok=True)
        (fake / "pwsh.cmd").write_text(
            "@echo off\necho STUB_PROFILE=%BC250_QUALITY_PROFILE%\n"
            "echo STUB_KITS=%BC250_QUALITY_KITS%\necho STUB_ARGS=%*\nexit /b 0\n",
            encoding="ascii")
        wrapper = base / "invoke.cmd"
        wrapper.write_text(
            '@echo off\nset "PATH=' + str(fake) + ';%PATH%"\n'
            'call "' + str(ROOT / 'tools/quality/quick.cmd') + '" "' + str(workspace)
            + '" "' + str(base / 'out') + '" "' + str(ROOT) + '" KmdPackage "' + str(kits)
            + '"\nexit /b %errorlevel%\n', encoding="ascii")
        env = os.environ.copy()
        env["TEMP"] = env["TMP"] = str(base)
        run = subprocess.run(["cmd", "/d", "/c", str(wrapper)], capture_output=True, text=True,
                             env=env, timeout=40)
        (base / "result.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("STUB_PROFILE=KmdPackage", run.stdout)
        self.assertIn("STUB_KITS=" + str(kits), run.stdout)
        self.assertIn('-Profile "KmdPackage" -Kits "' + str(kits) + '"', run.stdout)

class BuildOrderGateTests(unittest.TestCase):
    def test_actual_order_gate_and_legacy_negative(self):
        for negative in (False, True):
            out = BASE / ("ordinal-negative" if negative else "ordinal-positive")
            command = ["python", str(ROOT / "tools/quality/test_kmd_reproducible.py"), "--out", str(out)]
            if negative:
                command.append("--negative-control")
            env = os.environ.copy()
            env["TEMP"] = env["TMP"] = str(BASE)
            run = subprocess.run(command, capture_output=True, text=True, env=env, timeout=90)
            (BASE / ("ordinal-negative.log" if negative else "ordinal-positive.log")).write_text(
                run.stdout + run.stderr, encoding="utf-8")
            self.assertEqual(run.returncode, 1 if negative else 0, run.stdout + run.stderr)
            if negative:
                self.assertIn("FAIL CHECK", run.stdout)


if __name__ == "__main__":
    unittest.main()
