"""Self-test of the claim-shares gate (tools/quality/claim_shares.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Synthetic facts rows and the real data files. Every negative control is a wording this project
actually wrote, from the independent audit of 2026-10-10:

- the reverse denominator of the HIP perf rerun (0.836 us against a 4.641 us chain is 18.0 %,
  written 22.0 % of the 3.805 us cached chain) must fail `share-arithmetic`;
- "a quarter", for 0.112 of 0.297, which is 37.7 %, must fail it too;
- M842's "spends 76 % of its frame in RT", with a normalisation and no modelled label, must fail
  `clock-model-label`, and the corrected row must pass;
- M798's "112 of 113 ... fell inside those six spans (14.2 % of the window)" and M800's "6420 of
  416's counted gaps" must not fail: the first percentage belongs to another population, and the
  second number is a session, not a denominator;
- the real `docs/facts/data/*.yaml` with the baseline next to the gate must pass.
"""
import os
from pathlib import Path
import tempfile
import unittest

import claim_shares as c

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]

# The corrected M842 wording, shortened to the clause the rule reads.
M842_FIXED = ('the RT setting costs an estimated 76 % of the frame, a modelled incremental '
              'cost: at a 1.5 GHz equivalent clock the frame is 18.1 ms with RT off and '
              '76.4 ms with all four, under the fitted rate x (1.5 / GHz)^0.75')
M842_ORIGINAL = ('The Witcher 3 Remaster (D3D12) at HIGH with RT spends 76 % of its frame in '
                 'RT: at a 1.5 GHz equivalent clock the frame is 18.1 ms with RT off and '
                 '76.4 ms with all four, so every rate is normalised as '
                 'rate x (1.5 / GHz)^0.75 with an elasticity of about 0.75')


class ClaimShares(unittest.TestCase):
    def data_file(self, rows):
        """A facts data file holding the given (id, claim) rows, in a directory of its own."""
        d = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(d.cleanup)
        path = Path(d.name) / "fake.yaml"
        body = 'area: fake\norder: 99\ntitle: "Fake"\ndescription: "Fixture"\nfacts:\n'
        for fid, claim in rows:
            body += ('  - id: %s\n    status: MEASURED\n    date: "2026-10-10"\n'
                     '    claim: "%s"\n' % (fid, claim))
        path.write_text(body, encoding="utf-8", newline="\n")
        return path

    def rules(self, rows, baseline=()):
        """The rule names the gate reports for the given rows."""
        failures, _ = c.check([self.data_file(rows)], set(baseline))
        return sorted(t.split('\t')[0] for t in failures)

    def test_reverse_denominator_fails(self):
        """0.836 of a 4.641 chain is 18.0 %; 22.0 % is the share of the cached 3.805."""
        rows = [('M900', 'the state cache is worth 836 of 4641 chain tenths (22.0 %) a dispatch')]
        self.assertEqual(self.rules(rows), ['share-arithmetic'])

    def test_right_denominator_passes(self):
        rows = [('M900', 'the state cache is worth 836 of 4641 chain tenths (18.0 %) a dispatch')]
        self.assertEqual(self.rules(rows), [])

    def test_a_quarter_fails(self):
        """0.112 of 0.297 is 37.7 %, which the write-up called a quarter."""
        rows = [('M901', 'the two policies are 112 of 297 nanoseconds apart (25.0 %)')]
        self.assertEqual(self.rules(rows), ['share-arithmetic'])

    def test_truncated_third_digit_passes(self):
        """245 of 255 is 96.08 %, written 96.0 % in M796. That is not a swapped denominator."""
        rows = [('M902', 'every duty read-back is 245 of 255 (96.0 %)')]
        self.assertEqual(self.rules(rows), [])

    def test_restored_denominator_passes(self):
        """The corrected M797 step, with its own coverage next to it."""
        rows = [('M797', 'the completion sets the context to status 0x0000000a in 270 of the '
                         '418 stalls (64.6 %) and never at a fast boundary (0 of 16593)')]
        self.assertEqual(self.rules(rows), [])

    def test_percentage_of_another_population_passes(self):
        """M798: the 14.2 % is the share of the window, seven words from the pair."""
        rows = [('M798', '200 of 239 long stalls plus 112 of 113 VSync-ended ones fell inside '
                         'those six spans (14.2 % of the window)')]
        self.assertEqual(self.rules(rows), [])

    def test_possessive_session_number_passes(self):
        """M800: "6420 of 416's counted gaps" names session 416, not a denominator."""
        rows = [('M800', "it credits the whole gap in 6420 of 416's counted gaps")]
        self.assertEqual(self.rules(rows), [])

    def test_swapped_counts_fail(self):
        rows = [('M903', 'the sweep holds the context in 419 of 418 stalls')]
        self.assertEqual(self.rules(rows), ['count-order'])

    def test_unlabelled_clock_model_fails(self):
        self.assertEqual(self.rules([('M842', M842_ORIGINAL)]), ['clock-model-label'])

    def test_labelled_clock_model_passes(self):
        self.assertEqual(self.rules([('M842', M842_FIXED)]), [])

    def test_baseline_admits_a_named_row_only(self):
        rows = [('M842', M842_ORIGINAL)]
        self.assertEqual(self.rules(rows, [('clock-model-label', 'M842')]), [])
        self.assertEqual(self.rules(rows, [('clock-model-label', 'M799')]),
                         ['clock-model-label'])

    def test_baseline_refuses_a_share_rule(self):
        path = Path(tempfile.mkdtemp(dir=OUT)) / "baseline.txt"
        path.write_text("share-arithmetic\tM900\n", encoding="utf-8", newline="\n")
        with self.assertRaises(ValueError):
            c.load(path)

    def test_baseline_file_of_the_repository_loads(self):
        baseline = c.load(Path(__file__).with_name('claim_shares_baseline.txt'))
        self.assertIn(('clock-model-label', 'M799'), baseline)
        self.assertNotIn(('clock-model-label', 'M842'), baseline)

    def test_the_repositorys_facts_pass(self):
        paths = c.data_files(REPO)
        self.assertTrue(paths)
        failures, _ = c.check(paths, c.load(
            Path(__file__).with_name('claim_shares_baseline.txt')))
        self.assertEqual(failures, [])

    # --- difference-share, over plain document lines -------------------------------------

    def document(self, lines):
        """A plain document with no `- id:` row, in a directory of its own."""
        d = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(d.cleanup)
        path = Path(d.name) / "write-up.md"
        path.write_text('\n'.join(lines) + '\n', encoding="utf-8", newline="\n")
        return path

    def doc_rules(self, lines):
        failures, _ = c.check([self.document(lines)], set())
        return sorted(t.split('\t')[0] for t in failures)

    def test_reverse_denominator_in_a_table_row_fails(self):
        """The two rows of the HIP perf rerun, as the write-up published them."""
        lines = ['| launch, us a dispatch | 8.167 | 8.585 | **0.418 us (5.1 %)** |',
                 '| chain, us a dispatch | 3.805 | 4.641 | **0.836 us (22.0 %)** |']
        self.assertEqual(self.doc_rules(lines), ['difference-share', 'difference-share'])

    def test_reduction_from_the_original_cost_passes(self):
        lines = ['| launch, us a dispatch | 8.167 | 8.585 | **0.418 us (4.9 %)** |',
                 '| chain, us a dispatch | 3.805 | 4.641 | **0.836 us (18.0 %)** |']
        self.assertEqual(self.doc_rules(lines), [])

    def test_a_named_denominator_passes(self):
        lines = ['the cache is 3.805 against 4.641, an overhead of 0.836 us (22.0 %) '
                 'of the cached cost']
        self.assertEqual(self.doc_rules(lines), [])

    def test_a_difference_with_no_pair_on_the_line_passes(self):
        lines = ['| difference | **0.112 us (0.6 %)** | 0.005 us | 26 us (0.5 %) |']
        self.assertEqual(self.doc_rules(lines), [])

    def test_a_plain_document_is_read_line_by_line(self):
        path = self.document(['| a | 3.805 | 4.641 | 0.836 us (22.0 %) |'])
        failures, _ = c.check([path], set())
        self.assertEqual(len(failures), 1)
        self.assertIn('line 1', failures[0])

    def test_rows_reads_every_key_of_a_row(self):
        path = self.data_file([('M904', 'a claim')])
        text = path.read_text(encoding='utf-8')
        path.write_text(text + '    detail: "270 of 418 (22.0 %)"\n', encoding='utf-8',
                        newline='\n')
        failures, _ = c.check([path], set())
        self.assertEqual(len(failures), 1)
        self.assertIn('M904', failures[0])


if __name__ == '__main__':
    unittest.main()
