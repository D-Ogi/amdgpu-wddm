"""Self-test of the script-variables gate (tools/quality/check-unassigned.ps1).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

The negative control is the defect this gate exists for, as it stood in the tree. The BD-167 branch hoisted
the control application's source list into $appSources and read it in both compiles, the shipped one and the
release flavour's lab twin; this train compiles the shipped exe from its output directory with relative names
and listed the sources inline on that line. Keeping our line and taking the branch's second compile left
$appSources written nowhere, so the lab twin compiled no file at all: "error CS5001: no suitable static
Main". No gate of the suite runs that build, which is why the gate is here and why the control is the real
pair of lines rather than an invented one.

The positive controls are the forms that must not be reported, each of which produced a false alarm while the
gate was written: a param block with a [Parameter()] attribute, Tee-Object -Variable, Set-Variable, a foreach
variable, a scriptblock parameter and an automatic variable.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
GATE = REPO / "tools" / "quality" / "check-unassigned.ps1"
PWSH = shutil.which("pwsh") or shutil.which("powershell")

# The two compiles of the control application's build, with the assignment lost: $appSources is read twice
# and written nowhere.
LOST_ASSIGNMENT = r"""param([string]$Out)
$csFlavour = @(if ($Lab) { '/define:AMDGPU_WDDM_LAB_FLAVOUR' })
& $csc @csFlavour "/out:amdgpu_wddm_control.exe" @appSources
& $csc /define:AMDGPU_WDDM_LAB_FLAVOUR "/out:$app" @appSources
"""

# The same build with the list assigned once, which is what the tree carries now.
KEPT_ASSIGNMENT = r"""param([string]$Out, [switch]$Lab)
$csc = 'csc.exe'
$app = "$Out\twin.exe"
$csFlavour = @(if ($Lab) { '/define:AMDGPU_WDDM_LAB_FLAVOUR' })
$appSources = @((Get-ChildItem "$here\src\*.cs").FullName | Sort-Object)
$here = $PSScriptRoot
& $csc @csFlavour "/out:amdgpu_wddm_control.exe" @appSources
& $csc /define:AMDGPU_WDDM_LAB_FLAVOUR "/out:$app" @appSources
"""

# Forms that must pass: each one was a false alarm of an earlier regular-expression version of this gate.
PASSES = {
    "parameter-attribute": r"""param([Parameter(Mandatory)][string]$Kits, [switch]$NoSmoke)
Write-Host "$Kits $NoSmoke"
""",
    "tee-object-variable": r"""$exe = 'x.exe'
& $exe --selftest | Tee-Object -Variable selftest | Out-Null
$selftest | Select-Object -Last 1
""",
    "set-variable": r"""Set-Variable rate 3
Write-Host $rate
""",
    "foreach-and-automatic": r"""foreach ($arch in 'x64', 'x86') { Write-Host $arch }
'a', 'b' | ForEach-Object { Write-Host $_ }
Write-Host $PSScriptRoot $LASTEXITCODE
""",
    "scriptblock-parameter": r"""$job = { param($name, $out) Write-Host "$name $out" }
& $job 'a' 'b'
""",
    "error-variable": r"""Get-Item missing -ErrorAction SilentlyContinue -ErrorVariable problem
if ($problem) { Write-Host 'no' }
""",
}


def run(*args):
    out = subprocess.run([PWSH, "-NoProfile", "-File", str(GATE)] + list(args),
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    return out.returncode, out.stdout + out.stderr


@unittest.skipUnless(PWSH, "no PowerShell in this environment")
class ScriptVariables(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="unassigned-")
        self.addCleanup(shutil.rmtree, self.dir, True)

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        with open(path, "w", encoding="utf-8", newline="\r\n") as fh:
            fh.write(text)
        return path

    def test_the_defect_this_gate_exists_for_fails(self):
        code, out = run(self.write("lost.ps1", LOST_ASSIGNMENT))
        self.assertEqual(1, code, out)
        self.assertIn("$appSources is read and never written", out)
        self.assertIn("1 with a variable that is never written", out)

    def test_the_fixed_build_passes(self):
        code, out = run(self.write("kept.ps1", KEPT_ASSIGNMENT))
        self.assertEqual(0, code, out)

    def test_the_forms_that_must_not_be_reported(self):
        for name, text in sorted(PASSES.items()):
            code, out = run(self.write(name + ".ps1", text))
            self.assertEqual(0, code, "%s: %s" % (name, out))

    def test_a_file_that_does_not_parse_is_a_problem(self):
        code, out = run(self.write("broken.ps1", "param([string]$A\nWrite-Host $A\n"))
        self.assertEqual(1, code, out)
        self.assertIn("does not parse", out)

    def test_no_file_given_is_a_problem_and_not_a_pass(self):
        code, out = run()
        self.assertEqual(2, code, out)

    def test_every_recipe_of_the_repository_passes(self):
        code, out = run("-Recipes", "-Repo", str(REPO))
        self.assertEqual(0, code, out)
        self.assertIn("0 with a variable that is never written", out)


if __name__ == "__main__":
    unittest.main()
