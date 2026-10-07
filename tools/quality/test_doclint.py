"""Self-test of the documentation style gate (tools/quality/doclint.py).

    python -m unittest discover -s tools/quality      # the 'quality-controls' gate of quick.ps1

Synthetic trees and one real one: a new document with a finding must fail, a clean new document
must pass, a baselined document at its line must pass, one that got worse must fail, one that
improved must pass with a note, --prune must lower and remove lines and must refuse to write
while the gate fails, the out-of-scope trees must stay out, a generated page must be skipped,
the glossary spellings and the dash rule must fire in prose and stay quiet inside code, and the
documents this repository actually holds must pass against the baseline file next to them.
"""
import contextlib
import io
import os
from pathlib import Path
import tempfile
import unittest

import doclint

OUT = os.environ.get("BC250_TEST_OUT") or None
REPO = Path(__file__).resolve().parents[2]

GLOSSARY = '''terms:
  - term: adapter
    means: "The WDDM display adapter."
    instead_of: ["graphics card"]
'''

# One semicolon is one hard finding of the vendored linter, and nothing else on the line.
ONE_FINDING = 'The panel is open; the fan is off.\n'
CLEAN = 'The panel is open. The fan is off.\n'


class Tree:
    """A throwaway repository: a glossary, a baseline and whatever documents a test writes."""

    def __init__(self, directory, baseline_lines=()):
        self.root = Path(directory)
        (self.root / 'docs').mkdir(parents=True, exist_ok=True)
        self.glossary = self.root / 'docs' / 'glossary.yaml'
        self.glossary.write_text(GLOSSARY, encoding='utf-8')
        self.baseline = self.root / 'baseline.txt'
        self.write_baseline(baseline_lines)

    def write_baseline(self, lines):
        text = '# baseline of the test tree\n' + ''.join(f'{n}\t{p}\n' for n, p in lines)
        self.baseline.write_text(text, encoding='utf-8')

    def doc(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
        return path

    def run(self, *extra):
        argv = ['--root', str(self.root), '--baseline', str(self.baseline),
                '--glossary', str(self.glossary), *extra]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = doclint.main(argv)
        return code, out.getvalue()

    def baseline_counts(self):
        return doclint.read_baseline(self.baseline)


@contextlib.contextmanager
def tree(baseline_lines=()):
    with tempfile.TemporaryDirectory(dir=OUT) as d:
        yield Tree(d, baseline_lines)


class DocLintGate(unittest.TestCase):
    def test_new_document_must_be_clean(self):
        with tree() as t:
            t.doc('docs/new.md', ONE_FINDING)
            code, text = t.run()
            self.assertEqual(code, 1)
            self.assertIn('outside the baseline', text)
            self.assertIn('semicolon', text)            # the findings are printed, not only counted

    def test_clean_new_document_passes(self):
        with tree() as t:
            t.doc('docs/new.md', CLEAN)
            code, text = t.run()
            self.assertEqual(code, 0, text)

    def test_baselined_document_at_its_line_passes(self):
        with tree([(1, 'docs/old.md')]) as t:
            t.doc('docs/old.md', ONE_FINDING)
            code, text = t.run()
            self.assertEqual(code, 0, text)

    def test_baselined_document_that_got_worse_fails(self):
        with tree([(1, 'docs/old.md')]) as t:
            t.doc('docs/old.md', ONE_FINDING + ONE_FINDING)
            code, text = t.run()
            self.assertEqual(code, 1)
            self.assertIn('2 findings, 1 allowed', text)

    def test_improvement_notes_and_prune_lowers(self):
        with tree([(3, 'docs/old.md')]) as t:
            t.doc('docs/old.md', ONE_FINDING)
            code, text = t.run()
            self.assertEqual(code, 0, text)
            self.assertIn('down to 1 from 3', text)
            code, text = t.run('--prune')
            self.assertEqual(code, 0, text)
            self.assertEqual(t.baseline_counts(), {'docs/old.md': 1})

    def test_prune_removes_a_clean_or_vanished_document(self):
        with tree([(1, 'docs/old.md'), (2, 'docs/gone.md')]) as t:
            t.doc('docs/old.md', CLEAN)
            code, text = t.run()
            self.assertEqual(code, 0, text)
            self.assertIn('docs/gone.md is no longer in the tree', text)
            t.run('--prune')
            self.assertEqual(t.baseline_counts(), {})

    def test_prune_refuses_while_the_gate_fails(self):
        with tree([(1, 'docs/old.md')]) as t:
            t.doc('docs/old.md', ONE_FINDING)
            t.doc('docs/new.md', ONE_FINDING)
            code, text = t.run('--prune')
            self.assertEqual(code, 1)
            self.assertIn('before you prune', text)
            self.assertEqual(t.baseline_counts(), {'docs/old.md': 1})

    def test_prune_keeps_the_comment_header(self):
        with tree([(3, 'docs/old.md')]) as t:
            t.doc('docs/old.md', ONE_FINDING)
            t.run('--prune')
            self.assertTrue(t.baseline.read_text(encoding='utf-8').startswith('# baseline'))

    def test_out_of_scope_trees_and_generated_pages(self):
        with tree() as t:
            t.doc('evidence/run/RESULT.md', ONE_FINDING)
            t.doc('experiments/E99-run/README.md', ONE_FINDING)
            t.doc('tools/quality/third_party/PROVENANCE.md', ONE_FINDING)
            t.doc('docs/generated.generated.md', ONE_FINDING)
            t.doc('docs/facts/kmd.md', '> **GENERATED by `x`**\n\n' + ONE_FINDING)
            code, text = t.run()
            self.assertEqual(code, 0, text)
            self.assertIn('0 documents', text)

    def test_scope_is_readme_docs_driver_and_tools(self):
        with tree() as t:
            t.doc('README.md', CLEAN)
            t.doc('docs/one.md', CLEAN)
            t.doc('driver/kmd/README.md', CLEAN)
            t.doc('tools/win/README.md', CLEAN)
            t.doc('NOTICE.md', ONE_FINDING)            # a top-level page that is not README.md
            code, text = t.run()
            self.assertEqual(code, 0, text)
            self.assertIn('4 documents', text)

    def test_glossary_spelling_in_prose_only(self):
        with tree() as t:
            t.doc('docs/a.md', 'The graphics card is warm.\n')
            code, text = t.run()
            self.assertEqual(code, 1)
            self.assertIn("'adapter'", text)
            t.doc('docs/a.md', 'The `graphics card` string is a label.\n')
            code, text = t.run()
            self.assertEqual(code, 0, text)
            t.doc('docs/a.md', '```\nthe graphics card\n```\n')
            code, text = t.run()
            self.assertEqual(code, 0, text)

    def test_dash_rule(self):
        with tree() as t:
            t.doc('docs/a.md', 'The fan — the loud one — is off.\n')
            code, text = t.run()
            self.assertEqual(code, 1)
            self.assertIn('em dash', text)
            t.doc('docs/a.md', 'The range is 1 – 2.\n')
            code, text = t.run()
            self.assertEqual(code, 1)
            self.assertIn('en dash', text)
            t.doc('docs/a.md', '```\n--option a—b\n```\n')
            code, text = t.run()
            self.assertEqual(code, 0, text)

    def test_files_mode_reports_without_the_ratchet(self):
        with tree() as t:
            path = t.doc('docs/a.md', ONE_FINDING)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = doclint.main(['--glossary', str(t.glossary), '--files', str(path)])
            self.assertEqual(code, 1)
            self.assertIn('1 findings', out.getvalue())

    def test_glossary_subset_is_strict(self):
        with tree() as t:
            t.glossary.write_text('terms:\n  - term: a\n    means: unquoted\n', encoding='utf-8')
            with self.assertRaises(doclint.GlossaryError):
                t.run()
            t.glossary.write_text('terms:\n  - term: a\n', encoding='utf-8')
            with self.assertRaises(doclint.GlossaryError):
                t.run()
            t.glossary.write_text('terms:\n  - term: a\n    means: "one"\n    colour: "red"\n',
                                  encoding='utf-8')
            with self.assertRaises(doclint.GlossaryError):
                t.run()

    def test_project_glossary_parses(self):
        terms = doclint.load_glossary(REPO / 'docs' / 'glossary.yaml')
        self.assertTrue(any(term['instead_of'] for term in terms))
        self.assertTrue(all(term['means'] for term in terms))

    def test_this_repository_passes_its_own_baseline(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = doclint.main(['--root', str(REPO)])
        self.assertEqual(code, 0, out.getvalue())

    def test_the_style_page_is_clean(self):
        rules = doclint.glossary_rules(doclint.load_glossary(REPO / 'docs' / 'glossary.yaml'))
        found = doclint.findings_for(REPO / 'docs' / 'style.md', rules)
        self.assertEqual(found, [], found)

    def test_vendored_linter_selftest(self):
        import ste_lint
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ste_lint.selftest()
        self.assertIn('selftest OK', out.getvalue())


if __name__ == '__main__':
    unittest.main()
