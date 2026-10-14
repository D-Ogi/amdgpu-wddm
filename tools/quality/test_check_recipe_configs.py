"""Self-test of the recipe-configs gate (tools/quality/check-recipe-configs.ps1).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Every control runs the gate over a copy of this repository's own tools\\build: the recipe scripts, their configs
files and common.ps1, which sets Set-StrictMode -Version 3.0. A negative control changes one line of that copy back
to the form a defect had, so the controls follow the scripts as they change, and a control whose line is gone fails
and says so instead of passing.

The first negative control is the defect this gate exists for: the release flavour read release_options whether or
not the config had the key, no vkd3d config has it, and a release build of the vkd3d engine stopped before meson
ran. The second is the same trap after the compile, in the version stamp's read of the ships map, which a per-app
build reached only after ninja: the resolve part of the gate cannot reach that line and the reads part must. The
others are the ways the gate could pass without having checked anything: a config the script's -Config set
refuses, a -Config name with no config, a release flavour that never took its release path, a script whose option
resolution has no end the gate knows, and a configs file that no script reads.

The positive controls are the tree as it is, with the resolved option sets read back, and two other ways to look a
key up before it is read.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
GATE = REPO / "tools" / "quality" / "check-recipe-configs.ps1"
BUILD = REPO / "tools" / "build"
PWSH = shutil.which("pwsh")
OUT = os.environ.get("BC250_TEST_OUT") or None

# The lines the negative controls revert, as the scripts carry them now, and the forms the defects had.
RELEASE_GUARDED = "if ($forkFlavour -eq 'release' -and $recipeConfig.PSObject.Properties['release_options']) {"
RELEASE_UNGUARDED = "if ($forkFlavour -eq 'release') {"
RELEASE_READ = "@($recipeConfig.release_options"
SHIPS_GUARDED = "if ($recipeConfig.PSObject.Properties['ships'] -and"
SHIPS_UNGUARDED = "if ($recipeConfig.ships -and"
ANCHOR = "$options = Add-ReproducibleMesonOptions $options $Source $Build -Arch $Arch"
FLAVOUR_PROBE = "-SimpleMatch \"'amdgpu-wddm-lab-flavour'\""
# The stamp's lookup and read of the ships map, as one if; the nested form below must pass as well.
SHIPS_BLOCK = re.compile(r"if \(\$recipeConfig\.PSObject\.Properties\['ships'\] -and "
                         r"(\$recipeConfig\.ships\.PSObject\.Properties\.Name -contains \$t)\) \{(\r?\n)(\s*)"
                         r"(\$names = @\(\$recipeConfig\.ships\.\$t\))(\r?\n)(\s*)\}")


@unittest.skipUnless(PWSH, "no PowerShell 7 in this environment")
class RecipeConfigs(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="recipe-configs-", dir=OUT)
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.repo = os.path.join(self.dir, "repo")
        self.build = os.path.join(self.repo, "tools", "build")
        os.makedirs(self.build)
        for p in BUILD.iterdir():
            if p.is_file() and p.suffix in (".ps1", ".json"):
                shutil.copy2(p, self.build)

    def read(self, name):
        with open(os.path.join(self.build, name), encoding="utf-8", newline="") as fh:
            return fh.read()

    def write(self, name, text):
        with open(os.path.join(self.build, name), "w", encoding="utf-8", newline="") as fh:
            fh.write(text)

    def edit(self, name, old, new):
        text = self.read(name)
        self.assertEqual(1, text.count(old), "%s no longer carries %r once: update this control" % (name, old))
        self.write(name, text.replace(old, new))

    def line_of(self, name, needle):
        for number, line in enumerate(self.read(name).splitlines(), 1):
            if needle in line:
                return number
        self.fail("%s has no line with %r" % (name, needle))

    def run_gate(self, repo=None):
        out = os.path.join(self.dir, "out")
        r = subprocess.run([PWSH, "-NoProfile", "-File", str(GATE), "-Repo", repo or self.repo, "-Out", out],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        return r.returncode, r.stdout + r.stderr, out

    def test_the_tree_as_it_is_passes_and_resolves_both_flavours(self):
        code, text, out = self.run_gate()
        self.assertEqual(0, code, text)
        self.assertIn("; 0 problem(s)", text)
        with open(os.path.join(out, "resolved.json"), encoding="utf-8") as fh:
            resolved = json.load(fh)
        mesa = json.loads(self.read("mesa-configs.json"))["configs"]
        for config in ("llvmpipe-umd", "zink-umd"):
            extra = mesa[config]["release_options"]
            self.assertTrue(extra, config)
            release = resolved["build-mesa.ps1 %s release" % config]
            lab = resolved["build-mesa.ps1 %s lab" % config]
            for option in extra:
                self.assertIn(option, release, config)
                self.assertNotIn(option, lab, config)
            self.assertIn("-Damdgpu-wddm-lab-flavour=true", lab)
            self.assertNotIn("-Damdgpu-wddm-lab-flavour=true", release)
        # every config of every configs file, in both flavours
        for script, configs in (("build-mesa.ps1", "mesa-configs.json"), ("build-dxvk.ps1", "dxvk-configs.json"),
                                ("build-vkd3d.ps1", "vkd3d-configs.json")):
            for config in json.loads(self.read(configs))["configs"]:
                for flavour in ("release", "lab"):
                    self.assertIn("%s %s %s" % (script, config, flavour), resolved)

    def test_the_defect_this_gate_exists_for_fails(self):
        self.edit("build-vkd3d.ps1", RELEASE_GUARDED, RELEASE_UNGUARDED)
        line = self.line_of("build-vkd3d.ps1", RELEASE_READ)
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        for config in ("per-app", "ddi-engine", "ddi-engine-lto"):
            self.assertIn("build-vkd3d.ps1:%d: -Config %s (release) does not resolve: The property 'release_options' "
                          "cannot be found" % (line, config), text)
            self.assertIn("build-vkd3d.ps1:%d: $recipeConfig.release_options with -Config %s: The property "
                          "'release_options' cannot be found" % (line, config), text)
        self.assertNotIn("(lab) does not resolve", text)
        self.assertNotIn("build-mesa.ps1", text)
        self.assertNotIn("build-dxvk.ps1", text)

    def test_the_same_trap_after_the_compile_fails(self):
        self.edit("build-dxvk.ps1", SHIPS_GUARDED, SHIPS_UNGUARDED)
        line = self.line_of("build-dxvk.ps1", SHIPS_UNGUARDED)
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("build-dxvk.ps1:%d: $recipeConfig.ships with -Config per-app: The property 'ships' cannot be "
                      "found" % line, text)
        # the config with a ships map reads it, and the resolve part never reaches the stamp
        self.assertNotIn("-Config ddi-engine:", text)
        self.assertNotIn("does not resolve", text)

    def test_other_lookups_before_the_read_pass(self):
        self.edit("build-dxvk.ps1", SHIPS_GUARDED, "if ($recipeConfig.PSObject.Properties.Name -contains 'ships' -and")
        text = self.read("build-vkd3d.ps1")
        nested, count = SHIPS_BLOCK.subn(r"if ($recipeConfig.PSObject.Properties['ships']) {\2\3if (\1) { \4 }\5\6}", text)
        self.assertEqual(1, count, "build-vkd3d.ps1 no longer carries the stamp's lookup of ships: update this control")
        self.write("build-vkd3d.ps1", nested)
        code, text, _ = self.run_gate()
        self.assertEqual(0, code, text)

    def test_a_config_the_script_refuses_fails(self):
        data = json.loads(self.read("dxvk-configs.json"))
        data["configs"]["extra"] = data["configs"]["per-app"]
        self.write("dxvk-configs.json", json.dumps(data, indent=2))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("build-dxvk.ps1: -Config extra (release) does not resolve: Cannot validate argument on parameter "
                      "'Config'", text)

    def test_a_config_name_without_a_config_fails(self):
        data = json.loads(self.read("vkd3d-configs.json"))
        del data["configs"]["per-app"]
        self.write("vkd3d-configs.json", json.dumps(data, indent=2))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("-Config per-app (release) does not resolve: The property 'per-app' cannot be found", text)

    def test_a_release_flavour_that_never_takes_its_release_path_fails(self):
        self.edit("common.ps1", FLAVOUR_PROBE, "-SimpleMatch \"'amdgpu-wddm-lab-flavour-renamed'\"")
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("build-vkd3d.ps1: -Config ddi-engine (release) resolved the none", text)
        self.assertIn("build-vkd3d.ps1: -Config ddi-engine (lab) does not resolve in common.ps1:", text)
        self.assertIn("declares no amdgpu-wddm-lab-flavour option", text)

    def test_a_resolution_without_a_known_end_fails(self):
        self.edit("build-mesa.ps1", ANCHOR, "$options = @($options)")
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("build-mesa.ps1: no statement of its body assigns the result of Add-ReproducibleMesonOptions", text)

    def test_a_configs_file_no_script_reads_fails(self):
        self.write("orphan-configs.json", json.dumps({"schema": 1, "configs": {}}))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("orphan-configs.json: no recipe script of tools\\build reads it", text)

    def test_the_repository_passes(self):
        code, text, _ = self.run_gate(str(REPO))
        self.assertEqual(0, code, text)
        self.assertIn("; 0 problem(s)", text)


if __name__ == "__main__":
    unittest.main()
