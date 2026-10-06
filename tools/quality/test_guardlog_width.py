"""Self-test of the guardlog-width gate (tools/quality/guardlog_width.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Synthetic sources and one real one: a new over-width format must fail, a baselined format at its
baselined width must pass, a baselined format that shrank must pass with a note that asks for its
line to be removed, an included .inc file must be read like a .c file, --prune must tighten the
widths it keeps and refuse to write while the gate fails, and the driver the repository actually
holds must pass against the baseline file next to it.
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
    def source(self, formats, name="fake.c"):
        """A directory holding one source file with a GuardLog call for each given format."""
        d = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(d.cleanup)
        body = "".join('    GuardLog("%s");\n' % t for t in formats)
        (Path(d.name) / name).write_text("void f(void)\n{\n%s}\n" % body, encoding="utf-8")
        return d.name

    def baseline_file(self, text):
        """A baseline file holding the given text, inside a directory of its own."""
        d = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(d.cleanup)
        path = Path(d.name) / "baseline.txt"
        path.write_text(text, encoding="utf-8", newline="\n")
        return path

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

    def test_an_included_file_is_read_like_a_translation_unit(self):
        # wddm.c includes wddm_allocation_identity.inc and that file holds a GuardLog call. A gate
        # that reads *.c alone cannot see it, and a format one word longer there loses its fields.
        for name in ("fake.inc", "fake.h"):
            failures, _, _ = g.check(self.source([LONG], name=name), {})
            self.assertEqual(len(failures), 1, name)
            self.assertIn(name, failures[0])

    def test_every_file_of_the_real_driver_that_calls_guardlog_is_read(self):
        read = {p.name for p in g.sources(REPO / "driver" / "kmd")}
        calling = {p.name for p in (REPO / "driver" / "kmd").iterdir()
                   if p.is_file() and p.suffix in (".c", ".inc", ".h")
                   and g.CALL.search(p.read_text(encoding="utf-8", errors="replace"))}
        self.assertTrue(calling)
        self.assertEqual(calling - read, set())

    def test_prune_tightens_the_width_it_keeps(self):
        shorter = "z" * 200
        root = self.source([shorter])
        path = self.baseline_file("# head\n" + g.line("fake.c", shorter, 285) + "\n"
                                 + g.line("fake.c", "gone", 300) + "\n")
        self.assertEqual(g.main(["--kmd", root, "--baseline", str(path)]), 0)
        self.assertEqual(g.load(path)[("fake.c", shorter)], 285)      # the stale allowance
        self.assertEqual(g.main(["--kmd", root, "--baseline", str(path), "--prune"]), 0)
        kept = g.load(path)
        self.assertEqual(kept, {("fake.c", shorter): 200})            # tightened, and one removed
        self.assertTrue(path.read_text(encoding="utf-8").startswith("# head\n"))
        self.assertNotIn("\r", path.read_text(encoding="utf-8"))

    def test_prune_refuses_to_write_while_the_gate_fails(self):
        root = self.source([LONG])
        text = "# head\n"
        path = self.baseline_file(text)
        self.assertEqual(g.main(["--kmd", root, "--baseline", str(path), "--prune"]), 1)
        self.assertEqual(path.read_text(encoding="utf-8"), text)

    def test_the_baseline_file_reads_back(self):
        baseline = g.load(Path(__file__).with_name("guardlog_width_baseline.txt"))
        self.assertTrue(baseline)
        for (file, fmt), w in baseline.items():
            self.assertIn(Path(file).suffix, g.SUFFIXES, file)
            self.assertGreater(w, g.LIMIT)
            self.assertEqual(g.width(fmt), w)

    def test_the_driver_in_this_repository_passes(self):
        baseline = g.load(Path(__file__).with_name("guardlog_width_baseline.txt"))
        failures, _, _ = g.check(REPO / "driver" / "kmd", baseline)
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
