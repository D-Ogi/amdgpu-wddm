"""Offline controls for package workspace selection and status-map profiles."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "tools/quality/kmd-build-context.ps1"
def default_test_output(repo, environment):
    if environment.get("BC250_TEST_OUT"):
        output = Path(environment["BC250_TEST_OUT"]).resolve()
    else:
        if environment.get("BC250_ROOT"):
            workspace = Path(environment["BC250_ROOT"]).resolve()
        else:
            common = subprocess.run(
                ["git", "-C", str(repo), "rev-parse", "--git-common-dir"],
                capture_output=True, text=True, check=True, env=environment, timeout=15)
            common_path = Path(common.stdout.strip())
            if not common_path.is_absolute():
                common_path = repo / common_path
            workspace = common_path.resolve().parent.parent
        if not (workspace / "toolchain/nuget").is_dir():
            raise RuntimeError("Cannot infer test workspace; set BC250_ROOT or BC250_TEST_OUT")
        output = workspace / "scratch/quality/kmd-package-tests"
    if output.is_relative_to(repo.resolve()):
        raise RuntimeError("Package test output must be outside the source repository")
    return output


BASE = default_test_output(ROOT, os.environ.copy())
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
        self.env = os.environ.copy()
        self.env.pop("BC250_ROOT", None)
        for key in list(self.env):
            if key.startswith("GIT_CONFIG_") or key in ("GIT_CONFIG", "GIT_DIR", "GIT_COMMON_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
                del self.env[key]
        for level in ("GLOBAL", "SYSTEM"):
            config = self.base / (level.lower() + ".gitconfig")
            config.write_text("", encoding="utf-8")
            self.env["GIT_CONFIG_" + level] = str(config)
        self.env["GIT_CONFIG_NOSYSTEM"] = "1"
        self.env["TEMP"] = self.env["TMP"] = str(self.base)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True, capture_output=True, env=self.env)

    def resolve(self, explicit="", environment="", kits=None, legacy=False, helper=HELPER, nonlocal_fixture=False):
        env = self.env.copy()
        if environment:
            env["BC250_ROOT"] = str(environment)
        if legacy:
            script = "$repo=" + ps_quote(self.repo) + "; $QualityWorkspace=''; "
            script += "if(!$QualityWorkspace){$QualityWorkspace=if($env:BC250_ROOT){$env:BC250_ROOT}else{Split-Path $repo}}; "
            script += "@{workspace=$QualityWorkspace;source='legacy'} | ConvertTo-Json -Compress"
        else:
            script = ". " + ps_quote(helper) + "; Resolve-Bc250PackageContext -Repo " + ps_quote(self.repo)
            script += " -Kits " + ps_quote(kits or self.kits) + " -QualityWorkspace " + ps_quote(explicit)
            script += " | ConvertTo-Json -Compress"
        if nonlocal_fixture:
            # Git 2.27 lacks GIT_CONFIG_GLOBAL/SYSTEM overrides. Include the isolated
            # fixtures at command scope; --local must ignore this non-local scope too.
            # Select-Object -First 1: a PATH with two git.exe entries (Git for Windows ships cmd\git.exe and
            # mingw64\bin\git.exe) makes Get-Command return both, and .Source on the array joins the paths.
            command = "$script:FixtureGit=(Get-Command git -CommandType Application | Select-Object -First 1).Source; function git { & $script:FixtureGit "
            for level in ("SYSTEM", "GLOBAL"):
                command += "-c " + ps_quote("include.path=" + self.env["GIT_CONFIG_" + level]) + " "
            script = command + "@args }; " + script
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
        subprocess.run(["git", "-C", str(self.repo), "config", "--local", "bc250.workspace", "../../.."], check=True, env=self.env)
        self.check_resolution(self.resolve(), "git:bc250.workspace")

    def test_global_and_system_config_are_ignored(self):
        for level in ("GLOBAL", "SYSTEM"):
            subprocess.run(["git", "config", "--file", self.env["GIT_CONFIG_" + level],
                            "bc250.workspace", str(self.base / (level + "-wrong"))],
                           check=True, env=self.env)
        self.check_resolution(self.resolve(nonlocal_fixture=True), "Kits")
        old = self.base / "helper-with-global-config.ps1"
        old.write_text(HELPER.read_text(encoding="utf-8-sig").replace(
            "config --local --get bc250.workspace", "config --get bc250.workspace"), encoding="utf-8-sig")
        with self.assertRaises(AssertionError):
            self.check_resolution(self.resolve(helper=old, nonlocal_fixture=True), "Kits")

    def test_default_outputs_use_workspace_without_writing_repo(self):
        env = self.env.copy()
        env.pop("BC250_TEST_OUT", None)
        env["BC250_ROOT"] = str(self.workspace)
        expected = self.workspace / "scratch/quality/kmd-package-tests"
        self.assertEqual(default_test_output(self.repo, env), expected)
        self.assertFalse(expected.exists())
        env.pop("BC250_ROOT")
        main_repo = self.workspace / "main-checkout"
        main_repo.mkdir(exist_ok=True)
        subprocess.run(["git", "init", "-q", str(main_repo)], check=True, capture_output=True, env=env)
        self.assertEqual(default_test_output(main_repo, env), expected)
        with mock.patch("subprocess.run", return_value=subprocess.CompletedProcess(
                [], 0, stdout=str(main_repo / ".git") + "\n")) as query:
            self.assertEqual(default_test_output(self.repo, env), expected)
            self.assertIn("--git-common-dir", query.call_args.args[0])
        self.assertFalse((self.repo / "build").exists())
        env["BC250_TEST_OUT"] = str(self.repo / "build")
        with self.assertRaises(RuntimeError):
            default_test_output(self.repo, env)

    def test_actual_quick_rejects_mixed_kits_in_both_profiles(self):
        other = self.base / "other-kits"
        other.mkdir(exist_ok=True)
        for selected in ("Full", "KmdPackage"):
            out = self.base / ("refused-" + selected)
            run = subprocess.run(["pwsh", "-NoProfile", "-File", str(ROOT / "tools/quality/quick.ps1"),
                                  "-Workspace", str(self.workspace), "-RepoRoot", str(ROOT),
                                  "-Kits", str(other), "-Out", str(out), "-Profile", selected],
                                 capture_output=True, text=True, env=self.env, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("inconsistent compiler", run.stderr)
            self.assertNotIn(" PASS ", run.stdout)
            self.assertFalse(out.exists())

    def test_invalid_explicit_does_not_fallback(self):
        self.assertNotEqual(self.resolve(self.base / "missing").returncode, 0)

    def test_invalid_environment_does_not_fallback(self):
        self.assertNotEqual(self.resolve(environment=self.base / "missing").returncode, 0)

    def test_invalid_config_does_not_fallback(self):
        subprocess.run(["git", "-C", str(self.repo), "config", "--local", "bc250.workspace", "missing"], check=True, env=self.env)
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
        receipt = json.loads((self.base / "full/result.json").read_text())
        self.assertTrue(receipt["reason"].startswith("Full requires"))

    def test_package_records_difference_without_touching_artifact(self):
        out = self.base / "package"
        self.assertEqual(profile.evaluate_outputs("KmdPackage", self.repo, self.outputs, out), 0)
        receipt = json.loads((out / "result.json").read_text())
        self.assertEqual(receipt["tracked_output_freshness"], "profile-exclusion")
        self.assertTrue(receipt["reason"].startswith("Package checks"))
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
        self.assertIn('-Profile "%BC250_QUALITY_PROFILE%" -Kits "%BC250_QUALITY_KITS%\\."', cmd)


class BuildFlavourTests(unittest.TestCase):
    """Refuse conflicting input before discovery; keep the compiler and resource on one flavour.

    Refusal and command export execute the real build script. To check the resource without compiling the
    driver, a fixture executes its production prefix through rc-header, ending before the first native tool.
    Only the script's relocated source directory is replaced. No flavour logic is copied into the test.
    Both supported PowerShell hosts run the same controls in the existing quality-controls gate.
    """
    def setUp(self):
        self.base = BASE / ("build-flavour-" + self._testMethodName)
        self.base.mkdir(parents=True, exist_ok=True)
        self.build = ROOT / "driver/kmd/build.ps1"
        self.text = self.build.read_text(encoding="utf-8-sig")
        if os.environ.get("BC250_ROOT"):
            workspace = Path(os.environ["BC250_ROOT"])
        else:
            common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--git-common-dir"],
                                    check=True, capture_output=True, text=True, timeout=15)
            common_path = Path(common.stdout.strip())
            if not common_path.is_absolute():
                common_path = ROOT / common_path
            workspace = common_path.resolve().parent.parent
        self.kits = workspace / "toolchain/nuget"
        self.assertTrue(self.kits.is_dir(), str(self.kits))
        self.hosts = [(name, shutil.which(name)) for name in ("powershell.exe", "pwsh.exe")]
        for name, path in self.hosts:
            self.assertIsNotNone(path, name + " is required")
        self.env = os.environ.copy()
        self.env["TEMP"] = self.env["TMP"] = str(self.base)
        self.env["PYTHONDONTWRITEBYTECODE"] = "1"

    def run_build(self, host, name, tokens, build=None, existing=False, missing_tools=False):
        case = self.base / (Path(host).stem + "-" + name)
        case.mkdir(parents=True, exist_ok=True)
        out = case / "output"
        if existing:
            for relative in ("obj/keep.obj", "obj/cl.pdb", "obj/keep.ilk", "bc250kmd.pdb", "bc250kmd.map",
                             "package/bc250kmd.sys", "compile_commands.json"):
                path = out / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"previous build must remain intact\n")
        before = {p.relative_to(out): p.read_bytes() for p in out.rglob("*") if p.is_file()} if out.exists() else None
        command = "& " + ps_quote(build or self.build) + " -Kits " + ps_quote(self.kits) + " -Out " + ps_quote(out)
        wrapper = case / "run.ps1"
        wrapper.write_text("$ErrorActionPreference='Stop'\n" + command + " " + tokens + "\n", encoding="utf-8-sig")
        env = self.env.copy()
        if missing_tools:
            env["ProgramFiles(x86)"] = str(case / "no-tool-discovery-here")
        run = subprocess.run([host, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(wrapper)],
                             capture_output=True, text=True, env=env, timeout=30)
        (case / "result.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        return run, out, before

    def assert_refused(self, run, out, before, message):
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(message, run.stderr)
        self.assertNotIn("compile", run.stdout.lower())
        if before is None:
            self.assertFalse(out.exists(), "refusal created the output directory")
        else:
            after = {p.relative_to(out): p.read_bytes() for p in out.rglob("*") if p.is_file()}
            self.assertEqual(after, before, "refusal changed an existing build")

    def test_actual_conflict_refuses_before_discovery_or_cleanup_in_all_modes(self):
        for _, host in self.hosts:
            for mode in ("", "-CompileOnly", "-ExportCommandsOnly"):
                for index, pair in enumerate(("-Flavour release -Lab", "-Flavour lab -Lab:$false")):
                    for existing in (False, True):
                        name = (mode or "package") + str(index) + str(existing)
                        with self.subTest(host=host, mode=mode, pair=pair, existing=existing):
                            run, out, before = self.run_build(host, name, pair + " " + mode, existing=existing, missing_tools=True)
                            self.assert_refused(run, out, before, "Conflicting -Flavour and -Lab")

    def test_actual_lab_package_requires_version_before_discovery(self):
        for _, host in self.hosts:
            for index, tokens in enumerate(("-Flavour lab", "-Lab")):
                with self.subTest(host=host, tokens=tokens):
                    run, out, before = self.run_build(host, str(index), tokens, existing=True, missing_tools=True)
                    self.assert_refused(run, out, before, "needs -PackageVersion for a package build")

    def check_commands(self, out, expected):
        commands = json.loads((out / "compile_commands.json").read_text(encoding="utf-8-sig"))
        self.assertGreater(len(commands), 50)
        for command in commands:
            self.assertEqual(command["arguments"].count("/DAMDGPU_WDDM_LAB_FLAVOUR=1"),
                             int(expected == "lab"), command["file"])

    def test_actual_export_keeps_alias_and_flavour_compiler_flags(self):
        variants = (("", "release"), ("-Flavour release", "release"), ("-Lab:$false", "release"),
                    ("-Flavour lab", "lab"), ("-Lab", "lab"), ("-Flavour LAB -Lab", "lab"),
                    ("-Flavour release -Lab:$false", "release"))
        for _, host in self.hosts:
            for index, (tokens, expected) in enumerate(variants):
                with self.subTest(host=host, tokens=tokens):
                    run, out, _ = self.run_build(host, str(index), tokens + " -ExportCommandsOnly")
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.check_commands(out, expected)
                    self.assertFalse((out / "obj/version-stamp.h").exists())
                    self.assertFalse((out / "package/bc250kmd.sys").exists())

    def test_production_compile_prefix_keeps_version_stamp_and_flags_together(self):
        marker = '    Invoke-Tool (Join-Path $sdk "bin\\$KitVersion\\x64\\rc.exe")'
        self.assertEqual(self.text.count(marker), 1)
        prefix = self.text[:self.text.index(marker)] + "}\n"
        here = "$here = Split-Path -Parent $MyInvocation.MyCommand.Path"
        self.assertEqual(prefix.count(here), 1)
        prefix = prefix.replace(here, "$here = " + ps_quote(self.build.parent))
        fixture = self.base / "production-prefix.ps1"
        fixture.write_text(prefix, encoding="utf-8-sig")
        variants = (("", "release"), ("-Flavour lab", "lab"), ("-Lab", "lab"),
                    ("-Flavour lab -Lab", "lab"), ("-Lab:$false", "release"))
        for _, host in self.hosts:
            for index, (tokens, expected) in enumerate(variants):
                for versioned in (False, True):
                    with self.subTest(host=host, tokens=tokens, versioned=versioned):
                        version = " -PackageVersion 0.7.216.102-tester.31" if versioned else ""
                        run, out, _ = self.run_build(host, str(index) + str(versioned), tokens + " -CompileOnly" + version, build=fixture)
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.check_commands(out, expected)
                        header = out / "obj/version-stamp.h"
                        self.assertEqual(header.exists(), versioned)
                        if versioned:
                            self.assertEqual("#define BC250_VER_LAB" in header.read_text(), expected == "lab")
                        self.assertFalse((out / "package/bc250kmd.sys").exists())
        # The post-link byte gate must consume that same canonical value, not the compatibility alias.
        self.assertIn("'--flavour' $Flavour '--kmd'", self.text)
        self.assertNotIn("if ($Lab) { $clFlags", self.text)
        # The original fault must fail the same compiler-flag assertion even though rc-header still says lab.
        legacy = self.base / "independent-lab-prefix.ps1"
        legacy.write_text(prefix.replace("if ($Flavour -eq 'lab') { $clFlags", "if ($Lab) { $clFlags"), encoding="utf-8-sig")
        for _, host in self.hosts:
            run, out, _ = self.run_build(host, "negative-independent-lab", "-CompileOnly -Flavour lab "
                                        "-PackageVersion 0.7.216.102-tester.31", build=legacy)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("#define BC250_VER_LAB", (out / "obj/version-stamp.h").read_text())
            with self.assertRaises(AssertionError):
                self.check_commands(out, "lab")


class QuickCommandTests(unittest.TestCase):
    def test_actual_cmd_to_native_pwsh_argv(self):
        base = BASE / "quick-native-argv"
        base.mkdir(parents=True, exist_ok=True)
        current = (ROOT / "tools/quality/quick.cmd").read_text(encoding="utf-8-sig")
        old = current
        for variable in ("BC250_WORKSPACE", "BC250_QUALITY_OUT", "BC250_QUALITY_KITS"):
            old = old.replace('"%' + variable + '%\\."', '"%' + variable + '%"')
        old = old.replace('if defined BC250_QUALITY_REPO set "BC250_QUALITY_REPO=%BC250_QUALITY_REPO%\\."', '')
        self.assertNotEqual(old, current, "legacy forwarding mutation must change the production CMD")
        env = os.environ.copy()
        env["TEMP"] = env["TMP"] = str(base)
        for label, trailing, command_text in (("plain", False, current), ("trailing", True, current),
                                               ("legacy-plain", False, old), ("legacy-trailing", True, old)):
            case = base / label
            case.mkdir(exist_ok=True)
            workspace = case / "workspace with spaces"
            kits = workspace / "toolchain/nuget"
            kits.mkdir(parents=True, exist_ok=True)
            (workspace / "scratch/tmp").mkdir(parents=True, exist_ok=True)
            repo = case / "source with spaces"
            repo.mkdir(exist_ok=True)
            output = case / "output with spaces"
            copied = case / "quick.cmd"
            copied.write_text(command_text, encoding="ascii")
            (case / "quick.ps1").write_text(
                'param([string]$Workspace,[string]$Out,[string]$RepoRoot,[string]$Profile,[string]$Kits)\n'
                '@{workspace=$Workspace;out=$Out;repo=$RepoRoot;profile=$Profile;kits=$Kits} | '
                'ConvertTo-Json | Set-Content -LiteralPath $env:BC250_ARGV_RECEIPT\n', encoding="ascii")
            suffix = "\\" if trailing else ""
            wrapper = case / "invoke.cmd"
            wrapper.write_text('@echo off\ncall "' + str(copied) + '" "' + str(workspace) + suffix
                               + '" "' + str(output) + suffix + '" "' + str(repo) + suffix
                               + '" KmdPackage "' + str(kits) + suffix + '"\nexit /b %errorlevel%\n',
                               encoding="ascii")
            receipt_path = case / "argv.json"
            if receipt_path.exists():
                receipt_path.unlink()
            env["BC250_ARGV_RECEIPT"] = str(receipt_path)
            run = subprocess.run(["cmd", "/d", "/c", str(wrapper)], capture_output=True, text=True,
                                 env=env, timeout=40)
            (case / "result.log").write_text(run.stdout + run.stderr, encoding="utf-8")
            def check_argv():
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertTrue(receipt_path.exists(), run.stdout + run.stderr)
                receipt = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
                self.assertEqual(receipt["profile"], "KmdPackage")
                for key, expected in (("workspace", workspace), ("out", output), ("repo", repo), ("kits", kits)):
                    self.assertEqual(Path(receipt[key]).resolve(), expected.resolve(), key)
            if label == "legacy-trailing":
                with self.assertRaises(AssertionError):
                    check_argv()
            else:
                check_argv()

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
