"""Controls for the WinForms label gate (winforms_labels.py, fact M898).

The negative controls are the shapes that must fail: the start window of tester.31 rc4 as it was written (a docked
Label, AutoSize off), the other ways PowerShell and C# write the same layout, and C# inside a PowerShell here-string.
The positive controls are the same windows with AutoSize on, an Anchor instead of a Dock, Dock None, or the layout
in a comment. The real tree is checked too: it must pass, and the two scripts of the start window of 84f35234
(start-window.ps1 and start-confirm.ps1) must make no WinForms window any more (the window is gone, 2026-10-22).
"""
import os
from pathlib import Path
import tempfile
import unittest

from winforms_labels import check, scan_text

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]

# The window of tester.31 rc4, as tools/release/installer/start-window.ps1 had it before 84f35234.
RC4 = """Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$form = New-Object Windows.Forms.Form
$form.BackColor = [Drawing.Color]::Navy
$label = New-Object Windows.Forms.Label
$label.Dock = 'Fill'; $label.ForeColor = [Drawing.Color]::Cyan; $label.Font = New-Object Drawing.Font('Consolas', 9)
$form.Controls.Add($label)
"""


def ps(text):
    return scan_text(Path("x.ps1"), text)


def cs(text):
    return scan_text(Path("x.cs"), text)


class PowerShell(unittest.TestCase):
    def test_rc4_window_fails(self):
        self.assertEqual(ps(RC4), [(4, "$label", "'Fill'")])

    def test_autosize_on_passes(self):
        self.assertEqual(ps(RC4 + "$label.AutoSize = $true\n"), [])

    def test_dock_none_passes(self):
        self.assertEqual(ps(RC4.replace("'Fill'", "'None'")), [])

    def test_no_dock_passes(self):
        self.assertEqual(ps(RC4.replace("$label.Dock = 'Fill'; ", "$label.Anchor = 'Top, Bottom, Left, Right'; ")), [])

    def test_property_hashtable_fails(self):
        text = "Add-Type -AssemblyName System.Windows.Forms\n$l = New-Object System.Windows.Forms.Label -Property @{ Text = 'x'; Dock = 'Top' }\n"
        self.assertEqual(ps(text), [(2, "$l", "'Top'")])
        self.assertEqual(ps(text.replace("Dock = 'Top'", "Dock = 'Top'; AutoSize = $true")), [])

    def test_static_new_and_enum_value_fail(self):
        text = "Add-Type -AssemblyName System.Windows.Forms\n$t = [Windows.Forms.LinkLabel]::new()\n$t.Dock = [Windows.Forms.DockStyle]::Left\n"
        self.assertEqual(ps(text), [(2, "$t", "[Windows.Forms.DockStyle]::Left")])

    def test_comments_do_not_count(self):
        text = RC4.replace("$label.Dock = 'Fill'; ", "") + "# $label.Dock = 'Fill'\n<#\n$label.Dock = 'Fill'\n#>\n"
        self.assertEqual(ps(text), [])

    def test_script_without_winforms_is_not_read(self):
        self.assertIsNone(ps("$label = 'Fill'\n"))

    def test_csharp_in_a_here_string_fails(self):
        text = ("Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @'\n"
                "class W : System.Windows.Forms.Form { W() { Controls.Add(new System.Windows.Forms.Label { Dock = System.Windows.Forms.DockStyle.Fill }); } }\n"
                "'@\n")
        self.assertEqual(ps(text), [(2, "new Label", "DockStyle.Fill")])


class CSharp(unittest.TestCase):
    def test_docked_initializer_fails(self):
        text = "var l = new Label { Text = text, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter };\n"
        self.assertEqual(cs(text), [(1, "l", "DockStyle.Fill")])

    def test_autosize_initializer_passes(self):
        self.assertEqual(cs("var l = new Label { AutoSize = true, Dock = DockStyle.Top };\n"), [])

    def test_anchored_initializer_passes(self):
        self.assertEqual(cs("var l = new Label { Bounds = r, Anchor = AnchorStyles.Top | AnchorStyles.Bottom };\n"), [])

    def test_later_dock_fails(self):
        text = "Label _line;\nvoid Build()\n{\n    _line = new Label();\n    _line.Dock = DockStyle.Bottom;\n}\n"
        self.assertEqual(cs(text), [(4, "_line", "DockStyle.Bottom")])
        self.assertEqual(cs(text.replace("DockStyle.Bottom;", "DockStyle.Bottom; _line.AutoSize = true;")), [])

    def test_unnamed_label_fails(self):
        self.assertEqual(cs("Controls.Add(new Label { Dock = DockStyle.Fill });\n"), [(1, "new Label", "DockStyle.Fill")])

    def test_comments_and_other_types_do_not_count(self):
        text = ("// var l = new Label { Dock = DockStyle.Fill };\n/* new Label { Dock = DockStyle.Fill } */\n"
                "var t = new LabelLayout.Tally();\nvar p = new Panel { Dock = DockStyle.Fill };\n")
        self.assertEqual(cs(text), [])


class Tree(unittest.TestCase):
    def write(self, root, rel, text):
        path = Path(root) / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_allowed_and_stale_entries(self):
        with tempfile.TemporaryDirectory(dir=OUT) as root:
            self.write(root, "tools/a.ps1", RC4)
            self.write(root, "evidence/run/as-ran.ps1", RC4)      # copies of scripts as they ran are not read
            problems, report, _ = check(root, allowed={})
            self.assertEqual(len(problems), 1)
            self.assertIn("tools/a.ps1:4: $label is a Label with Dock 'Fill'", problems[0])
            problems, report, _ = check(root, allowed={("tools/a.ps1", "$label"): "a reason"})
            self.assertEqual(problems, [])
            self.assertEqual(len(report), 1)
            problems, _, _ = check(root, allowed={("tools/a.ps1", "$label"): "a reason", ("tools/b.ps1", "$x"): "gone"})
            self.assertEqual(problems, ["tools/b.ps1: the allowed finding $x is not there any more; remove it from ALLOWED"])

    def test_repository_passes(self):
        problems, report, winforms = check(REPO)
        self.assertEqual(problems, [])
        # The start window of 84f35234 is gone (2026-10-22): neither of its two scripts makes a WinForms
        # window any more, so the scan finds nothing to read there.
        for rel in ("tools/release/installer/start-window.ps1", "tools/release/installer/start-confirm.ps1"):
            self.assertNotIn(rel, winforms)
            self.assertFalse(any(line.startswith(rel + ":") for line in report))


if __name__ == "__main__":
    unittest.main()
