"""Self-test of the guardlog-width gate (tools/quality/guardlog_width.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Three synthetic sources and one real one: a new over-width format must fail, a baselined format at
its baselined width must pass, a baselined format that shrank must pass with a note that asks for
its line to be removed, and the driver the repository actually holds must pass against the
baseline file next to it.
"""
import os
from pathlib import Path
import tempfile
import unittest

import guardlog_width as g

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]

# 160 characters of text, one over the limit, and nothing but text: no conversion to argue about.
LONG = "x" * 160
SHORT = "y" * 40


class GuardLogWidth(unittest.TestCase):
    def source(self, formats):
        """A directory holding one .c file with a GuardLog call for each given format."""
        d = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(d.cleanup)
        body = "".join('    GuardLog("%s");\n' % t for t in formats)
        (Path(d.name) / "fake.c").write_text("void f(void)\n{\n%s}\n" % body, encoding="utf-8")
        return d.name

    def test_width_counts_the_widest_printing(self):
        self.assertEqual(g.width("ab"), 2)
        self.assertEqual(g.width("%ld"), 11)                # -2147483648
        self.assertEqual(g.width("%lu"), 10)                # 4294967295
        self.assertEqual(g.width("%lld"), 20)
        self.assertEqual(g.width("%08lX"), 8)
        self.assertEqual(g.width("%p"), 16)
        self.assertEqual(g.width("%s"), 32)
        self.assertEqual(g.width("100%% of %u"), 18)

    def test_a_new_over_width_format_fails(self):
        failures, notes, over = g.check(self.source([LONG]), {})
        self.assertEqual((len(failures), len(notes), len(over)), (1, 0, 0))
        self.assertIn("not in the baseline", failures[0])

    def test_a_baselined_format_passes(self):
        baseline = {("fake.c", LONG): 160}
        failures, notes, over = g.check(self.source([LONG]), baseline)
        self.assertEqual((len(failures), len(notes), len(over)), (0, 0, 1))

    def test_a_baselined_format_that_got_wider_fails(self):
        baseline = {("fake.c", LONG + "z"): 160}
        failures, notes, over = g.check(self.source([LONG + "z"]), baseline)
        self.assertEqual((len(failures), len(notes), len(over)), (1, 0, 0))
        self.assertIn("wider than the baselined 160", failures[0])

    def test_a_baselined_format_that_shrank_notes(self):
        baseline = {("fake.c", SHORT): 160, ("fake.c", LONG): 160}
        failures, notes, over = g.check(self.source([SHORT]), baseline)
        self.assertEqual(len(failures), 0)
        self.assertEqual(len(notes), 2)
        self.assertIn("remove its baseline line", notes[0])     # the format shrank
        self.assertIn("remove its baseline line", notes[1])     # the format is gone

    def test_the_baseline_file_reads_back(self):
        baseline = g.load(Path(__file__).with_name("guardlog_width_baseline.txt"))
        self.assertTrue(baseline)
        for (file, fmt), w in baseline.items():
            self.assertTrue(file.endswith(".c"), file)
            self.assertGreater(w, g.LIMIT)
            self.assertEqual(g.width(fmt), w)

    def test_the_driver_in_this_repository_passes(self):
        baseline = g.load(Path(__file__).with_name("guardlog_width_baseline.txt"))
        failures, _, _ = g.check(REPO / "driver" / "kmd", baseline)
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
