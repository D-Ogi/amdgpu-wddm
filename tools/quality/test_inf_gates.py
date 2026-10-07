"""Self-test of the inf-gates gate (tools/quality/inf_gates.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Every case starts from a synthetic pair that agrees - one INF, one defaults table, built from the
gate's own three tables so that none of their lines is stale - and then breaks it in one way: a gate
written without NOCLOBBER, a value that is not the released one, a gate the table does not name, a
release default the INF does not write, a setting written twice, and a line of the gate's own tables
that no longer applies. The real pair the repository holds is checked as well.
"""
import json
import os
from pathlib import Path
import tempfile
import unittest

import inf_gates as g

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]

HEADER = "; a test INF\r\n[Bc250_Parameters]\r\n"


def line(name, flags, value):
    return "HKR, Parameters, %s, 0x%08X, %s" % (name, flags, value)


class InfGates(unittest.TestCase):
    def base(self):
        """A pair that agrees: every line of the gate's three tables applies, plus one plain gate."""
        lines = [line(name, g.PLAIN_DWORD, 0) for name in sorted(g.UNCONDITIONAL)]
        lines += [line(name, g.NOCLOBBER, 0) for name in sorted(g.NOT_IN_TABLE)]
        lines += [line("EnableMmio", g.NOCLOBBER, 1)]
        parameters = {name: 1 for name in g.NOT_IN_INF}
        parameters["EnableMmio"] = 1
        return lines, parameters

    def failures(self, lines, parameters):
        directory = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        inf = root / "bc250kmd.inf"
        inf.write_text(HEADER + "".join(text + "\r\n" for text in lines), encoding="ascii", newline="")
        defaults = root / "registry-defaults.json"
        defaults.write_text(json.dumps({"defaults": {"parameters": parameters}}), encoding="utf-8")
        return g.check(inf, defaults)

    # ---- the agreed case ------------------------------------------------------------------------
    def test_agreed_pair_passes(self):
        lines, parameters = self.base()
        lines.append(line("EnableFullWddm", g.NOCLOBBER, 2))
        parameters["EnableFullWddm"] = 2
        self.assertEqual([], self.failures(lines, parameters))

    def test_a_commented_out_line_is_not_an_addreg_line(self):
        lines, parameters = self.base()
        lines.insert(0, "; " + line("EnableMmio", g.PLAIN_DWORD, 0))
        self.assertEqual([], self.failures(lines, parameters))

    # ---- the drift the gate exists for ---------------------------------------------------------
    def test_plain_dword_on_a_gate_fails(self):
        lines, parameters = self.base()
        lines[-1] = line("EnableMmio", g.PLAIN_DWORD, 1)
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("NOCLOBBER", failures[0])

    def test_value_other_than_the_release_default_fails(self):
        lines, parameters = self.base()
        lines[-1] = line("EnableMmio", g.NOCLOBBER, 0)
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("the INF writes 0, the release default is 1", failures[0])

    def test_a_gate_the_table_does_not_name_fails(self):
        lines, parameters = self.base()
        lines.append(line("EnableSomethingNew", g.NOCLOBBER, 1))
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("EnableSomethingNew", failures[0])
        self.assertIn("NOT_IN_TABLE", failures[0])

    def test_a_release_default_the_inf_does_not_write_fails(self):
        lines, parameters = self.base()
        parameters["EnableIh"] = 1
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("NOT_IN_INF", failures[0])

    def test_the_same_setting_written_twice_fails(self):
        lines, parameters = self.base()
        lines.append(line("EnableMmio", g.NOCLOBBER, 1))
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("written twice", failures[0])

    def test_an_inf_without_a_parameters_line_fails(self):
        failures = self.failures([], {"EnableMmio": 1})
        self.assertEqual(1, len(failures))
        self.assertIn("no HKR,Parameters line", failures[0])

    # ---- the gate's own three tables cannot rot ------------------------------------------------
    def test_an_unconditional_setting_must_keep_the_plain_write(self):
        name = sorted(g.UNCONDITIONAL)[0]
        lines, parameters = self.base()
        lines[0] = line(name, g.NOCLOBBER, 0)
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("UNCONDITIONAL", failures[0])

    def test_an_unconditional_setting_must_not_be_a_release_default(self):
        name = sorted(g.UNCONDITIONAL)[0]
        lines, parameters = self.base()
        parameters[name] = 0
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("in UNCONDITIONAL and a release default as well", failures[0])

    def test_a_not_in_table_setting_that_the_table_does_name_fails(self):
        name = sorted(g.NOT_IN_TABLE)[0]
        lines, parameters = self.base()
        parameters[name] = 0
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("Remove the NOT_IN_TABLE line", failures[0])

    def test_a_table_line_for_a_setting_the_inf_no_longer_writes_fails(self):
        lines, parameters = self.base()
        dropped = lines.pop(0)                       # the first UNCONDITIONAL setting
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("Remove the line", failures[0])
        self.assertIn(dropped.split(",")[2].strip(), failures[0])

    def test_a_not_in_inf_line_for_a_setting_the_inf_writes_fails(self):
        name = sorted(g.NOT_IN_INF)[0]
        lines, parameters = self.base()
        lines.append(line(name, g.NOCLOBBER, parameters[name]))
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("the INF does write it now", failures[0])

    def test_a_not_in_inf_line_for_a_setting_that_is_no_default_fails(self):
        name = sorted(g.NOT_IN_INF)[0]
        lines, parameters = self.base()
        del parameters[name]
        failures = self.failures(lines, parameters)
        self.assertEqual(1, len(failures))
        self.assertIn("not a release default any more", failures[0])

    # ---- the repository itself -----------------------------------------------------------------
    def test_the_repository_agrees(self):
        inf = REPO / "driver" / "kmd" / "bc250kmd.inf"
        defaults = REPO / "tools" / "release" / "installer" / "registry-defaults.json"
        self.assertEqual([], g.check(inf, defaults))

    def test_the_repository_inf_keeps_crlf(self):
        # The gate reads the INF as text; the file itself must stay CRLF, or the release's Reboot
        # directive and Inf2Cat work on a file Windows does not accept as an INF.
        data = (REPO / "driver" / "kmd" / "bc250kmd.inf").read_bytes()
        self.assertEqual(data.count(b"\n"), data.count(b"\r\n"))


if __name__ == "__main__":
    unittest.main()
