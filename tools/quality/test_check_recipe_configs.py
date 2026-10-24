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
resolution has no end the gate knows, a configs file that no script reads, a variant with a misspelt key, and a
variant without an entry for one compiler.

The positive controls are the tree as it is, with the resolved option sets read back; the Mesa sets as recorded for
each compiler of build-mesa.ps1, and its variants, which add to the sets and change nothing else; and two other ways
to look a key up before it is read.
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
ANCHOR = "$options = Add-ReproducibleMesonOptions $options $Source $Build -Arch $Arch -Compiler $Compiler"
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
            release = resolved["build-mesa.ps1 %s release msvc" % config]
            lab = resolved["build-mesa.ps1 %s lab msvc" % config]
            for option in extra:
                self.assertIn(option, release, config)
                self.assertNotIn(option, lab, config)
            self.assertIn("-Damdgpu-wddm-lab-flavour=true", lab)
            self.assertNotIn("-Damdgpu-wddm-lab-flavour=true", release)
        # every config of every configs file, in both flavours, with each compiler of the three scripts, which has no
        # default
        for script, configs, compilers in (("build-mesa.ps1", "mesa-configs.json", (" msvc", " clang-cl")),
                                           ("build-dxvk.ps1", "dxvk-configs.json", (" msvc", " clang-cl")),
                                           ("build-vkd3d.ps1", "vkd3d-configs.json", (" msvc", " clang-cl"))):
            for config in json.loads(self.read(configs))["configs"]:
                for flavour in ("release", "lab"):
                    for compiler in compilers:
                        self.assertIn("%s %s %s%s" % (script, config, flavour, compiler), resolved)
                    self.assertNotIn("%s %s %s" % (script, config, flavour), resolved)
        # BD-180: lab warning capture must survive the LTO recipe, while every release option stays intact, with
        # either compiler.
        for compiler in ("msvc", "clang-cl"):
            release = resolved["build-vkd3d.ps1 ddi-engine-lto release %s" % compiler]
            lab = resolved["build-vkd3d.ps1 ddi-engine-lto lab %s" % compiler]
            release_c = next(option for option in release if option.startswith("-Dc_args="))
            lab_c = next(option for option in lab if option.startswith("-Dc_args="))
            self.assertIn("-DVKD3D_NO_DEBUG_MESSAGES", release_c.split(), compiler)
            self.assertNotIn("-DVKD3D_NO_DEBUG_MESSAGES", lab_c.split(), compiler)
            expected_lab = [option.replace(" -DVKD3D_NO_DEBUG_MESSAGES", "") for option in release]
            self.assertEqual(expected_lab, [option for option in lab if option != "-Damdgpu-wddm-lab-flavour=true"],
                             compiler)

    def test_the_engine_sets_resolve_per_compiler(self):
        # The msvc set of the shipped engine stays the set every engine up to tester.32 was built with, in its order;
        # clang-cl spells LTO, the instruction set and the linker map its own way and changes nothing else.
        code, text, out = self.run_gate()
        self.assertEqual(0, code, text)
        with open(os.path.join(out, "resolved.json"), encoding="utf-8") as fh:
            resolved = json.load(fh)
        arguments = ("-Dc_args=", "-Dcpp_args=", "-Dc_link_args=", "-Dcpp_link_args=")

        def value(options, key):
            found = [x.split("=", 1)[1] for x in options if x.startswith("-D%s=" % key)]
            self.assertEqual(1, len(found), key)
            return found[0]

        recorded = ["-Dbuildtype=release", "-Denable_tests=false", "-Denable_ddi_engine=true", "-Db_vscrt=mt",
                    "-Db_ndebug=true", "-Db_lto=true", "-Db_lto_mode=default",
                    "-Dc_args=/arch:AVX2 -DVKD3D_NO_DEBUG_MESSAGES", "-Dcpp_args=/arch:AVX2", "-Dcpp_link_args=/MAP",
                    "-Ddebug=true"]
        m = resolved["build-vkd3d.ps1 ddi-engine-lto release msvc"]
        c = resolved["build-vkd3d.ps1 ddi-engine-lto release clang-cl"]
        # msvc: the recorded set, each argument option with the reproducible-build switches at its end
        self.assertEqual(len(recorded) + 1, len(m))
        for want, got in zip(recorded, m):
            self.assertTrue(got == want or (want.startswith(arguments) and got.startswith(want + " /Brepro")), got)
        self.assertEqual("/Brepro /PDBALTPATH:%_PDB%", value(m, "c_link_args"))
        # clang-cl: the same options, LTO in its ThinLTO mode
        plain = [x for x in m if not x.startswith(arguments + ("-Db_lto_mode=",))]
        self.assertEqual(plain, [x for x in c if not x.startswith(arguments + ("-Db_lto_mode=",))])
        self.assertEqual("thin", value(c, "b_lto_mode"))
        self.assertEqual(["-march=x86-64-v3", "-mtune=znver2", "-DVKD3D_NO_DEBUG_MESSAGES",
                          "/clang:-ffp-contract=off", "/Brepro"], value(c, "c_args").split()[:5])
        self.assertEqual(["-march=x86-64-v3", "-mtune=znver2", "/clang:-ffp-contract=off", "/Brepro"],
                         value(c, "cpp_args").split()[:4])
        for key in ("c_args", "cpp_args"):
            words = value(c, key).split()
            self.assertFalse([w for w in words if w.startswith(("/arch:", "-march=znver", "-march=native", "/FC",
                                                                "/d1trimfile:"))], key)
        # meson's sanity check gives the link options to clang-cl as compile options: no unknown /option
        self.assertEqual("-map /Brepro -pdbaltpath:%_PDB% -pdbsourcepath:X:\\mesa-build", value(c, "cpp_link_args"))
        self.assertEqual("/Brepro -pdbaltpath:%_PDB% -pdbsourcepath:X:\\mesa-build", value(c, "c_link_args"))
        # the configs without compiler_options build one set with either compiler
        for config in ("per-app", "ddi-engine"):
            for flavour in ("release", "lab"):
                m = resolved["build-vkd3d.ps1 %s %s msvc" % (config, flavour)]
                c = resolved["build-vkd3d.ps1 %s %s clang-cl" % (config, flavour)]
                self.assertEqual([x for x in m if not x.startswith(arguments)],
                                 [x for x in c if not x.startswith(arguments)], config)

    def test_compiler_options_without_an_entry_for_one_compiler_fail(self):
        data = json.loads(self.read("vkd3d-configs.json"))
        del data["configs"]["ddi-engine-lto"]["compiler_options"]["clang-cl"]
        self.write("vkd3d-configs.json", json.dumps(data, indent=2))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("-Config ddi-engine-lto (release, clang-cl) does not resolve", text)
        self.assertIn("-Config ddi-engine-lto has no compiler_options for -Compiler clang-cl (it has msvc)", text)
        self.assertNotIn("(release, msvc) does not resolve", text)

    def test_the_defect_this_gate_exists_for_fails(self):
        self.edit("build-vkd3d.ps1", RELEASE_GUARDED, RELEASE_UNGUARDED)
        line = self.line_of("build-vkd3d.ps1", RELEASE_READ)
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        for config in ("per-app", "ddi-engine", "ddi-engine-lto"):
            for compiler in ("msvc", "clang-cl"):
                self.assertIn("build-vkd3d.ps1:%d: -Config %s (release, %s) does not resolve: The property "
                              "'release_options' cannot be found" % (line, config, compiler), text)
            self.assertIn("build-vkd3d.ps1:%d: $recipeConfig.release_options with -Config %s: The property "
                          "'release_options' cannot be found" % (line, config), text)
        self.assertNotIn("(lab, ", text)
        self.assertNotIn("build-mesa.ps1", text)
        self.assertNotIn("build-dxvk.ps1", text)

    def test_system_icd_resolves_static_crt_in_both_flavours(self):
        # BD-178: test the real builder's resolved arguments, including its flavour logic.
        # Omitting b_vscrt selected Meson's dynamic CRT even though default_library was static.
        code, text, out = self.run_gate()
        self.assertEqual(0, code, text)
        with open(os.path.join(out, "resolved.json"), encoding="utf-8") as fh:
            resolved = json.load(fh)
        for config in ("radv-system", "radv-mt"):
            for flavour in ("release", "lab"):
                for compiler in ("msvc", "clang-cl"):
                    for variants in ("", " +release+lto+isa"):
                        name = "build-mesa.ps1 %s %s %s%s" % (config, flavour, compiler, variants)
                        options = resolved[name]
                        self.assertEqual(["-Db_vscrt=mt"], [x for x in options if x.startswith("-Db_vscrt=")], name)
                        self.assertIn("-Db_ndebug=true", options, name)
        mesa = json.loads(self.read("mesa-configs.json"))["configs"]
        self.assertEqual(["vulkan_radeon.dll"], mesa["radv-system"]["ships"]["src/amd/vulkan/vulkan_radeon.dll"])

    def test_the_mesa_sets_resolve_as_recorded_and_variants_only_add(self):
        # The shipped sets stay as recorded; -Compiler names the compiler and -Variant adds named
        # changes. No variant may touch an option it does not name.
        code, text, out = self.run_gate()
        self.assertEqual(0, code, text)
        with open(os.path.join(out, "resolved.json"), encoding="utf-8") as fh:
            resolved = json.load(fh)
        mesa = json.loads(self.read("mesa-configs.json"))
        arguments = ("-Dc_args=", "-Dcpp_args=", "-Dc_link_args=", "-Dcpp_link_args=")

        def plain(options):
            return [x for x in options if not x.startswith(arguments)]

        def value(options, key):
            found = [x.split("=", 1)[1] for x in options if x.startswith("-D%s=" % key)]
            self.assertEqual(1, len(found), key)
            return found[0]

        def get(config, flavour, compiler, variants=""):
            return resolved["build-mesa.ps1 %s %s %s%s" % (config, flavour, compiler, variants)]

        for config, entry in mesa["configs"].items():
            for flavour in ("release", "lab"):
                recorded = list(entry["options"])
                if flavour == "lab":
                    recorded.append("-Damdgpu-wddm-lab-flavour=true")
                recorded += entry.get("%s_options" % flavour, [])
                msvc = get(config, flavour, "msvc")
                clang = get(config, flavour, "clang-cl")
                # no variant: the recorded set, with either compiler
                self.assertEqual(recorded, plain(msvc), config)
                self.assertEqual(recorded, plain(clang), config)
                for key in ("c_args", "cpp_args"):
                    self.assertTrue(value(msvc, key).startswith("/Brepro /FC /d1trimfile:"), config)
                    words = value(clang, key).split()
                    self.assertEqual(["/clang:-ffp-contract=off", "-gline-tables-only", "/Brepro"], words[:3], config)
                    self.assertNotIn("/FC", words, config)
                    self.assertFalse([w for w in words if w.startswith("/d1trimfile:")], config)
                    self.assertTrue([w for w in words if w.startswith("/clang:-fmacro-prefix-map=")], config)
                    # lld-link hashes the PDB into the GUID: the PDB must not name the build directory
                    self.assertTrue([w for w in words if w.startswith("/clang:-fdebug-prefix-map=")], config)
                # meson's sanity check gives the link options to clang-cl as compile options: no unknown /option
                for key in ("c_link_args", "cpp_link_args"):
                    self.assertEqual("/Brepro /PDBALTPATH:%_PDB%", value(msvc, key), config)
                    self.assertEqual("/Brepro -pdbaltpath:%_PDB% -pdbsourcepath:X:\\mesa-build", value(clang, key), config)
                # each variant alone changes its own options and nothing else
                for compiler, base in (("msvc", msvc), ("clang-cl", clang)):
                    isa = get(config, flavour, compiler, " +isa")
                    self.assertEqual(plain(base), plain(isa), config)
                    lto = get(config, flavour, compiler, " +lto")
                    self.assertEqual(plain(base), [x for x in plain(lto) if not x.startswith(
                        ("-Db_lto=", "-Dallow-broken-lto=", "-Db_lto_mode="))], config)
                    release = get(config, flavour, compiler, " +release")
                    self.assertEqual("-Dbuildtype=release", release[0], config)
                    self.assertEqual(plain(base)[1:], [x for x in plain(release)[1:] if x not in (
                        "-Ddebug=true", "-Db_ndebug=false")], config)
                # all variants: release with the PDB, LTO and the x86-64-v3 level, in each compiler's spelling
                m = get(config, flavour, "msvc", " +release+lto+isa")
                c = get(config, flavour, "clang-cl", " +release+lto+isa")
                for options in (m, c):
                    self.assertEqual("release", value(options, "buildtype"), config)
                    self.assertEqual("true", value(options, "debug"), config)
                    self.assertEqual("true", value(options, "b_lto"), config)
                    # Mesa's meson.build refuses LTO without this switch.
                    self.assertEqual("true", value(options, "allow-broken-lto"), config)
                    if "-Db_ndebug=true" in recorded:
                        self.assertEqual("true", value(options, "b_ndebug"), config)
                    else:
                        # debugoptimized left NDEBUG undefined; a release buildtype alone would define it
                        self.assertEqual("false", value(options, "b_ndebug"), config)
                self.assertEqual("default", value(m, "b_lto_mode"), config)
                self.assertEqual("thin", value(c, "b_lto_mode"), config)
                self.assertEqual([x for x in plain(m) if not x.startswith("-Db_lto_mode=")],
                                 [x for x in plain(c) if not x.startswith("-Db_lto_mode=")], config)
                for key in ("c_args", "cpp_args"):
                    self.assertEqual(["/arch:AVX2", "/Brepro"], value(m, key).split()[:2], config)
                    words = value(c, key).split()
                    self.assertEqual(["/clang:-ffp-contract=off", "-gline-tables-only", "-march=x86-64-v3",
                                      "-mtune=znver2", "/Brepro"], words[:5], config)
                    self.assertFalse([w for w in words if w.startswith(("-march=znver", "-march=native"))], config)
        # The lab flavour of the two desktop UMDs keeps its asserts (docs/build.md, BD-167).
        for config in ("llvmpipe-umd", "zink-umd"):
            self.assertEqual("false", value(get(config, "lab", "msvc", " +release"), "b_ndebug"), config)
            self.assertEqual("true", value(get(config, "release", "msvc", " +release"), "b_ndebug"), config)

    def test_a_misspelt_variant_key_fails(self):
        data = json.loads(self.read("mesa-configs.json"))
        data["variants"]["isa"]["msvc"]["cargs"] = data["variants"]["isa"]["msvc"].pop("c_args")
        self.write("mesa-configs.json", json.dumps(data, indent=2))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("-Config radv-system (release, msvc, +isa) does not resolve in common.ps1:", text)
        self.assertIn("variants.isa.msvc: unknown key 'cargs'", text)
        self.assertNotIn("(release, clang-cl, +isa) does not resolve", text)
        self.assertNotIn("(release, msvc) does not resolve", text)

    def test_a_variant_without_an_entry_for_one_compiler_fails(self):
        data = json.loads(self.read("mesa-configs.json"))
        del data["variants"]["lto"]["clang-cl"]
        self.write("mesa-configs.json", json.dumps(data, indent=2))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("-Config radv-system (release, clang-cl, +lto) does not resolve in common.ps1:", text)
        self.assertIn("variants.lto has no entry for -Compiler clang-cl (it has msvc)", text)
        self.assertNotIn("(release, msvc, +lto) does not resolve", text)

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
        for compiler in ("msvc", "clang-cl"):
            self.assertIn("build-dxvk.ps1: -Config extra (release, %s) does not resolve: Cannot validate argument on "
                          "parameter 'Config'" % compiler, text)

    def test_a_config_name_without_a_config_fails(self):
        data = json.loads(self.read("vkd3d-configs.json"))
        del data["configs"]["per-app"]
        self.write("vkd3d-configs.json", json.dumps(data, indent=2))
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("-Config per-app (release, msvc) does not resolve: The property 'per-app' cannot be found", text)

    def test_a_release_flavour_that_never_takes_its_release_path_fails(self):
        self.edit("common.ps1", FLAVOUR_PROBE, "-SimpleMatch \"'amdgpu-wddm-lab-flavour-renamed'\"")
        code, text, _ = self.run_gate()
        self.assertEqual(1, code, text)
        self.assertIn("build-vkd3d.ps1: -Config ddi-engine (release, msvc) resolved the none", text)
        self.assertIn("build-vkd3d.ps1: -Config ddi-engine (lab, msvc) does not resolve in common.ps1:", text)
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
