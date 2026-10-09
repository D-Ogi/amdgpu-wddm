"""The gate of llm_reference_numbers.py, with the draft it was written against.

WRONG_DRAFT is the shape of the first draft of the research note, the one the audit of
2026-10-10 rejected: the decode numbers of the graph-option-off column under a ratio
column computed from the option-on column, and the deepseek-r1-14B row dropped without a
word. Each test below names the rule it exercises.
"""
import pathlib
import unittest

import llm_reference_numbers as gate

REPO = pathlib.Path(__file__).resolve().parents[2]

GOOD_TABLE = """## 1. Table

| model | size | ROCm pp512 | Vulkan pp512 | pp ratio | ROCm tg64, graph opt on | ROCm tg64, graph opt off | Vulkan tg64 | tg ratio, opt on |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| qwen2.5-1.5B Q4_K_M | 1.04 GiB | 1798.6 | 1850.0 | 0.97 | 213.1 | 196.7 | 212.3 | 1.00 |
| qwen3-8B Q8_0 | 8.24 GiB | 409.4 | 394.7 | 1.04 | 39.5 | 38.5 | 39.0 | 1.01 |
| deepseek-r1-14B Q4_K_M | 8.37 GiB | 195.8 | 199.8 | 0.98 | 33.6 | 32.6 | 35.1 | 0.96 |
| qwen3-14B Q4_K_M | 8.63 GiB | 197.6 | 204.8 | 0.96 | 33.7 | 32.7 | 34.7 | 0.97 |
| qwen3.6-35B-A3B MoE IQ2_M | 10.72 GiB | 588.8 | 457.0 | 1.29 | 70.8 | 71.2 | 86.9 | 0.81 |
| qwen3.8-27B UD-IQ3_XXS | 11.09 GiB | 102.9 | 105.0 | 0.98 | 15.1 | 15.2 | 17.6 | 0.86 |
"""

# The rejected draft: option-off decode numbers, option-on ratios, no deepseek row.
WRONG_DRAFT = """## 1. Table

| model | size | ROCm pp512 | Vulkan pp512 | pp ratio | ROCm tg64, graph opt on | ROCm tg64, graph opt off | Vulkan tg64 | tg ratio, opt on |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| qwen2.5-1.5B Q4_K_M | 1.04 GiB | 1798.6 | 1850.0 | 0.97 | 196.7 | 213.1 | 212.3 | 0.81 |
| qwen3-8B Q8_0 | 8.24 GiB | 409.4 | 394.7 | 1.04 | 38.5 | 39.5 | 39.0 | 0.86 |
| qwen3-14B Q4_K_M | 8.63 GiB | 197.6 | 204.8 | 0.96 | 32.7 | 33.7 | 34.7 | 0.81 |
| qwen3.6-35B-A3B MoE IQ2_M | 10.72 GiB | 588.8 | 457.0 | 1.29 | 71.2 | 70.8 | 86.9 | 0.81 |
| qwen3.8-27B UD-IQ3_XXS | 11.09 GiB | 102.9 | 105.0 | 0.98 | 15.2 | 15.1 | 17.6 | 0.86 |
"""


class LlmReferenceNumbers(unittest.TestCase):

    def test_repository_document_passes(self):
        path = REPO / gate.DOC
        self.assertTrue(path.exists(), path)
        self.assertEqual(gate.check_text(path.read_text(encoding='utf-8')), [])

    def test_good_table_passes(self):
        self.assertEqual(gate.check_text(GOOD_TABLE), [])

    def test_rejected_draft_fails_on_the_decode_ratio(self):
        findings = gate.check_text(WRONG_DRAFT)
        self.assertTrue([f for f in findings if f.startswith('R3')], findings)

    def test_rejected_draft_fails_on_the_dropped_row(self):
        findings = gate.check_text(WRONG_DRAFT)
        self.assertIn('R1: model row deepseek-r1-14B is missing', findings)

    def test_swapped_decode_columns_fail(self):
        findings = gate.check_text(WRONG_DRAFT)
        self.assertTrue([f for f in findings if f.startswith('R4')], findings)

    def test_prefill_ratio_is_checked(self):
        bad = GOOD_TABLE.replace('| 1798.6 | 1850.0 | 0.97 |', '| 1798.6 | 1850.0 | 1.29 |')
        findings = gate.check_text(bad)
        self.assertTrue([f for f in findings if f.startswith('R2')], findings)

    def test_defect_without_its_fix_fails(self):
        text = GOOD_TABLE + '\nThe option computes wrong tokens as shipped.\n'
        findings = gate.check_text(text)
        self.assertEqual(len([f for f in findings if f.startswith('R5')]), 2, findings)

    def test_defect_with_its_fix_passes(self):
        text = (GOOD_TABLE + '\nThe option computes wrong tokens as shipped. Apply'
                ' alloc-deps.diff and set GGML_CUDA_GRAPH_OPT_ALLOC_DEPS=1.\n')
        self.assertEqual(gate.check_text(text), [])

    def test_figure_without_its_page_fails(self):
        text = GOOD_TABLE + '\n## 2. Cluster\n\nTwo boards run it at 7.57 tokens per second.\n'
        findings = gate.check_text(text)
        self.assertTrue([f for f in findings if f.startswith('R6')], findings)

    def test_figure_with_its_page_passes(self):
        text = (GOOD_TABLE + '\n## 2. Cluster\n\nThe file ai-locale.md states 7.57 tokens'
                ' per second on two boards.\n')
        self.assertEqual(gate.check_text(text), [])


if __name__ == '__main__':
    unittest.main()
