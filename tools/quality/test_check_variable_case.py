"""Self-test of the variable-case gate (tools/quality/check-variable-case.ps1).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

The first negative control is the defect this gate exists for, as it stood in tools/release/build-release-set.ps1:
the new parameter [string]$Report and the existing line $report = [ordered]@{...} in the same scope, so that the
table became a string. The second is the lab kit script of 2026-10-24, where foreach ($l in ...) overwrote $L. The
others are the forms that the scope rules must still find: a write in a ForEach-Object script block (the scope of its
caller), a $script: write in a function, a [ref] argument, and a write by a dot-sourced file.

The positive controls are the forms that must not be reported: one name in two functions, an environment variable
($env:LIB and $lib), a write in a child script block (& { }), a nested function, and reads under another spelling.
The last one is reported only with -Reads, as a WARNING, and the exit code stays 0.

The whole set runs in five calls of the gate, because each call starts PowerShell.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
GATE = REPO / "tools" / "quality" / "check-variable-case.ps1"
PWSH = shutil.which("pwsh")

# Each of these must give exactly the ERROR lines named by its pattern.
FAILS = {
    # tools/release/build-release-set.ps1 before main 504e6738.
    "typed-report": (r"""param([string]$Out, [string]$Report = '')
$report = [ordered]@{ schema = 1; rows = @() }
$report.rows += 'one'
""", r"written as \$Report \(\[string\] parameter, line 1\), \$report \(write, line 2\) in the top level of the file; "
     r"the other spelling writes into the typed variable \$Report"),
    # The lab kit script of 2026-10-24.
    "foreach-overwrites": (r"""$L = Get-Content 'lines.txt'
foreach ($l in 'a', 'b') { Write-Host $l }
Write-Host $L.Count
""", r"written as \$L \(write, line 1\), \$l \(write, line 2\)"),
    "typed-parameter-in-function": (r"""function Get-Rows([int]$Count) {
    $count = 'many'
    return $count
}
""", r"written as \$Count \(\[int\] parameter, line 1\), \$count \(write, line 2\) in function Get-Rows"),
    # A ForEach-Object script block runs in the scope of its caller.
    "foreach-object-block": (r"""param([string]$Name)
'a', 'b' | ForEach-Object { $name = $_ }
""", r"written as \$Name \(\[string\] parameter, line 1\), \$name \(write, line 2\) in the top level of the file"),
    # A $script: write in a function is a write of the top level of the file.
    "script-qualifier": (r"""$State = $null
function Set-State { $script:state = 'new' }
Set-State
""", r"written as \$State \(write, line 1\), \$state \(write, line 2\) in the top level of the file"),
    # A variable named $keys or $count is a key of the lint's own tables: the scope is still examined.
    "keys-variable": (r"""$keys = @('a')
$Path = 'x'
$path = 'y'
""", r"written as \$Path \(write, line 2\), \$path \(write, line 3\)"),
    "ref-argument": (r"""$Tokens = @()
$errors = $null
[void][Management.Automation.Language.Parser]::ParseInput('1', [ref]$tokens, [ref]$errors)
""", r"written as \$Tokens \(write, line 1\), \$tokens \(write, line 3\)"),
    "hardlink": (r"""New-Item -ItemType HardLink -Path 'b.txt' -Target 'a[1].txt'
""", r"ERROR hardlink New-Item -Target"),
}

# A dot-sourced file runs in the scope that dot-sources it.
LIB = r"""$Repo = Split-Path $PSScriptRoot
"""
DOT_SOURCE = r""". "$PSScriptRoot\lib.ps1"
$repo = 'elsewhere'
"""

# A read by a dot-sourced file is no write of the caller.
READ_LIB = r"""Write-Host $Item
"""
DOT_SOURCE_READ = r""". "$PSScriptRoot\read-lib.ps1"
$item = 1
"""

PASSES = {
    "two-functions": r"""function A { $Map = @{}; $Map['a'] = 1; $Map }
function B { $map = @{}; $map['b'] = 2; $map }
""",
    "environment-variable": r"""$lib = 'x.lib'
$env:LIB = 'C:\lib'
Write-Host $lib $env:LIB
""",
    "child-script-block": r"""$X = 1
& { $x = 2; Write-Host $x }
Start-Job -ScriptBlock { $x = 3 } | Out-Null
""",
    "nested-function": r"""function Outer {
    $Item = 1
    function Inner { $item = 2; $item }
    Inner
}
""",
    "member-and-index": r"""param([hashtable]$Table)
$table['a'] = 1
$table.b = 2
""",
    "global-and-script": r"""$Value = 1
function Set-Global { $global:value = 2 }
""",
}

# Reads only: the same variable for PowerShell, reported with -Reads as a WARNING.
READS = r"""param([string]$Out)
Write-Host $out
"""


def run(*args):
    out = subprocess.run([PWSH, "-NoProfile", "-File", str(GATE)] + list(args),
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    return out.returncode, out.stdout + out.stderr


@unittest.skipUnless(PWSH, "no PowerShell 7 in this environment")
class VariableCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="variable-case-")
        cls.fails = {name: cls.write(name + ".ps1", text) for name, (text, _) in FAILS.items()}
        cls.write("lib.ps1", LIB)
        cls.fails["dot-source"] = cls.write("dot-source.ps1", DOT_SOURCE)
        cls.passes = {name: cls.write(name + ".ps1", text) for name, text in PASSES.items()}
        cls.write("read-lib.ps1", READ_LIB)
        cls.passes["dot-source-read"] = cls.write("dot-source-read.ps1", DOT_SOURCE_READ)
        cls.reads = cls.write("reads.ps1", READS)
        cls.bad_code, cls.bad_out = run(*sorted(cls.fails.values()))
        cls.good_code, cls.good_out = run(cls.reads, *sorted(cls.passes.values()))
        cls.reads_code, cls.reads_out = run("-Reads", cls.reads)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, True)

    @classmethod
    def write(cls, name, text):
        path = os.path.join(cls.dir, name)
        with open(path, "w", encoding="utf-8", newline="\r\n") as fh:
            fh.write(text)
        return path

    def lines_of(self, out, path):
        return [line for line in out.splitlines() if line.startswith(path + ":")]

    def test_the_defects_this_gate_exists_for_fail(self):
        self.assertEqual(1, self.bad_code, self.bad_out)
        for name, (_, pattern) in sorted(FAILS.items()):
            lines = self.lines_of(self.bad_out, self.fails[name])
            self.assertEqual(1, len(lines), "%s: %s" % (name, self.bad_out))
            self.assertRegex(lines[0], pattern, name)
        self.assertIn("%d error(s)" % (len(FAILS) + 1), self.bad_out)

    def test_a_write_by_a_dot_sourced_file_is_a_write_of_the_caller(self):
        lines = self.lines_of(self.bad_out, self.fails["dot-source"])
        self.assertEqual(1, len(lines), self.bad_out)
        self.assertRegex(lines[0], r"written as \$Repo \(write by lib\.ps1, line 1\), \$repo \(write, line 2\)")

    def test_the_forms_that_must_not_be_reported(self):
        self.assertEqual(0, self.good_code, self.good_out)
        for name, path in sorted(self.passes.items()):
            self.assertEqual([], self.lines_of(self.good_out, path), name)

    def test_reads_are_a_warning_only(self):
        self.assertEqual([], self.lines_of(self.good_out, self.reads), self.good_out)
        self.assertEqual(0, self.reads_code, self.reads_out)
        lines = self.lines_of(self.reads_out, self.reads)
        self.assertEqual(1, len(lines), self.reads_out)
        self.assertIn("WARNING case one variable read as", lines[0])
        self.assertTrue(re.search(r"0 error\(s\), 1 warning\(s\)", self.reads_out), self.reads_out)

    def test_a_file_that_does_not_parse_is_an_error(self):
        path = self.write("broken.ps1", "param([string]$A\nWrite-Host $A\n")
        code, out = run(path)
        self.assertEqual(1, code, out)
        self.assertIn("ERROR parse", out)

    def test_a_missing_file_is_a_problem_and_not_a_pass(self):
        code, out = run(os.path.join(self.dir, "absent.ps1"))
        self.assertEqual(2, code, out)


if __name__ == "__main__":
    unittest.main()
