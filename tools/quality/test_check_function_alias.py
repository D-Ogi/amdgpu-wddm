"""Self-test of the function-alias gate (tools/quality/check-function-alias.ps1).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

The first negative control is the defect this gate exists for: the rc3 FINAL-VERIFY row of 2026-11-09 defined
'function Cli', the built-in alias cli (Clear-Item) ran in its place, and the DPM cap was never set. The others are a
filter, another case, a scope modifier, and an alias of one edition only (sc in 5.1, gerr in 7). The positive controls
are names that only contain an alias name, a class method, and a variable.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
GATE = REPO / "tools" / "quality" / "check-function-alias.ps1"
LIST = REPO / "tools" / "quality" / "powershell-builtin-aliases.txt"
PWSH = shutil.which("pwsh")

# Each file must give exactly one ERROR line that matches its pattern.
FAILS = {
    "rc3-cli": ("function Cli([string[]]$A) { & bc250kmd_cli.exe @A }\nCli state set DpmMaxMHz 1800\n",
                r":1: ERROR alias function Cli: the built-in alias cli \(Clear-Item\) runs in its place"),
    "upper-case": ("function CLS { 'x' }\n", r"ERROR alias function CLS: the built-in alias cls \(Clear-Host\)"),
    "filter": ("filter ls { $_ }\n", r"ERROR alias filter ls: the built-in alias ls \(Get-ChildItem\)"),
    "scope-modifier": ("function script:gc { 'x' }\n", r"ERROR alias function script:gc: the built-in alias gc"),
    "only-in-5.1": ("function sc { 'x' }\n", r"ERROR alias function sc: the built-in alias sc \(Set-Content\)"),
    "only-in-7": ("function gerr { 'x' }\n", r"ERROR alias function gerr: the built-in alias gerr \(Get-Error\)"),
    "nested": ("function Outer {\n    function select { 'x' }\n    select\n}\n",
               r":2: ERROR alias function select: the built-in alias select \(Select-Object\)"),
}
# These must give no ERROR line.
PASSES = {
    "renamed": "function Invoke-KmdCli([string[]]$A) { & bc250kmd_cli.exe @A }\nInvoke-KmdCli state set DpmMaxMHz 1800\n",
    "contains-alias": "function Get-Cli { 'x' }\nfunction cli2 { 'x' }\nfunction Select-Rows { 'x' }\n",
    "class-method": "class Tool { [string] cli() { return 'x' } }\n",
    "variable": "$cli = 'bc250kmd_cli.exe'\n& $cli state\n",
}


@unittest.skipUnless(PWSH, "pwsh is not on PATH")
class FunctionAliasGate(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="function-alias-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_gate(self, files, *extra):
        r = subprocess.run([PWSH, "-NoProfile", "-File", str(GATE), *map(str, files), *extra],
                           capture_output=True, text=True, timeout=300)
        return r.returncode, r.stdout + r.stderr

    def write(self, cases):
        paths = {}
        for name, text in cases.items():
            p = self.tmp / f"{name}.ps1"
            p.write_text(text, encoding="utf-8")
            paths[name] = p
        return paths

    def test_the_list_is_the_built_in_set(self):
        lines = [l for l in LIST.read_text(encoding="utf-8").splitlines() if l and not l.startswith("#")]
        names = [l.split("\t")[0] for l in lines]
        self.assertEqual(names, sorted(names))
        self.assertEqual(names, [n.lower() for n in names])
        table = {l.split("\t")[0]: l.split("\t")[1] for l in lines}
        for name, definition in (("cli", "Clear-Item"), ("ls", "Get-ChildItem"), ("sc", "Set-Content"),
                                 ("gerr", "Get-Error"), ("%", "ForEach-Object")):
            self.assertEqual(table.get(name), definition, name)

    def test_planted_functions(self):
        fails, passes = self.write({k: v[0] for k, v in FAILS.items()}), self.write(PASSES)
        code, out = self.run_gate([*fails.values(), *passes.values()])
        self.assertEqual(code, 1, out)
        errors = [l for l in out.splitlines() if " ERROR " in l]
        self.assertEqual(len(errors), len(FAILS), out)
        for name, (_, pattern) in FAILS.items():
            hits = [l for l in errors if l.startswith(str(fails[name]))]
            self.assertEqual(len(hits), 1, f"{name}: {out}")
            self.assertRegex(hits[0], pattern)
        for name, path in passes.items():
            self.assertFalse([l for l in errors if l.startswith(str(path))], f"{name}: {out}")
        code, out = self.run_gate(passes.values())
        self.assertEqual(code, 0, out)

    def test_no_list_is_exit_2(self):
        code, out = self.run_gate(self.write({"renamed": PASSES["renamed"]}).values(), "-Aliases",
                                  str(self.tmp / "absent.txt"))
        self.assertEqual(code, 2, out)
        self.assertIn("no alias list", out)

    def test_a_file_that_does_not_parse_is_an_error(self):
        code, out = self.run_gate(self.write({"broken": "function Ok {\n"}).values())
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r"ERROR parse ")


if __name__ == "__main__":
    unittest.main()
