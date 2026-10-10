"""Consistency gate for docs/research/bc250-llm-reference-numbers.md.

    python tools/quality/llm_reference_numbers.py                 # the gate, on the repository document
    python tools/quality/llm_reference_numbers.py --doc <path>    # one named document

The document quotes another project's benchmark table. That table has two decode columns,
one with `GGML_CUDA_GRAPH_OPT=1` and one without it, and a ratio against Vulkan computed
from the first. A reader who mixes the two columns gets the right numbers with the wrong
per-model verdicts, which is the defect this gate catches. It also holds two rules about
sourcing, because three figures in an earlier draft had no page behind them.

The rules:

  R1  every model row of the quoted table is present, so no row can be dropped in silence
  R2  the prefill ratio column equals ROCm pp512 / Vulkan pp512, to two decimals
  R3  the decode ratio column equals the graph-option-on decode / Vulkan decode, to two
      decimals, which fails when the ratios and the numbers come from different columns
  R4  the graph-option-off decode is lower on the four models the option helps, and within
      2 percent on the two models where the option launches no streams
  R5  the paragraph set that names the wrong-token defect also names the published fix
  R6  a quoted figure sits in the same section as the source token that carries it
"""
import argparse
import pathlib
import re
import sys

DOC = 'docs/research/bc250-llm-reference-numbers.md'

HEADER = '| model | size | ROCm pp512 |'

MODELS = (
    'qwen2.5-1.5B',
    'qwen3-8B',
    'deepseek-r1-14B',
    'qwen3-14B',
    'qwen3.6-35B-A3B',
    'qwen3.8-27B',
)

# The two models where the multi-stream option launches no streams, so the two decode
# columns of the quoted table hold the same measurement twice.
OPTION_DOES_NOTHING = ('qwen3.6-35B-A3B', 'qwen3.8-27B')

DEFECT = re.compile(r'wrong token|word salad', re.I)
FIX_TOKENS = ('alloc-deps.diff', 'GGML_CUDA_GRAPH_OPT_ALLOC_DEPS')

# figure -> the token that names its page, which has to sit in the same section
SOURCED_FIGURES = (
    ('7.57', 'ai-locale'),
    ('54.99', 'vulkan-fused-gate-up'),
    ('75-100 tokens/sec', 'issue 14'),
)

TOLERANCE = 0.005
SAME = 0.02


def rows(text):
    """The data rows of the ROCm against Vulkan table, as lists of cells."""
    out = []
    inside = False
    for line in text.splitlines():
        if line.startswith(HEADER):
            inside = True
            continue
        if not inside:
            continue
        if not line.startswith('|'):
            break
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if all(re.fullmatch(r':?-{2,}:?', c) for c in cells):
            continue
        out.append(cells)
    return out


def sections(text):
    """The document split on its '## ' headings, heading line included."""
    out = []
    current = []
    for line in text.splitlines():
        if line.startswith('## ') and current:
            out.append('\n'.join(current))
            current = []
        current.append(line)
    if current:
        out.append('\n'.join(current))
    return out


def number(cell):
    m = re.search(r'\d+(?:\.\d+)?', cell)
    return float(m.group(0)) if m else None


def check_text(text):
    """Every rule over one document's text. Returns a list of findings."""
    findings = []
    table = rows(text)
    if not table:
        return ['R1: no ROCm against Vulkan table found (header "%s")' % HEADER]

    seen = {}
    for cells in table:
        if len(cells) != 9:
            findings.append('R1: row has %d cells, expected 9: %s' % (len(cells), cells[0]))
            continue
        name = cells[0]
        key = next((m for m in MODELS if m in name), None)
        if key is None:
            findings.append('R1: row "%s" is not one of the six quoted models' % name)
            continue
        if key in seen:
            findings.append('R1: model %s appears twice' % key)
        seen[key] = cells

    for key in MODELS:
        if key not in seen:
            findings.append('R1: model row %s is missing' % key)

    for key, cells in seen.items():
        pp_rocm, pp_vk, pp_ratio = (number(cells[2]), number(cells[3]), number(cells[4]))
        tg_on, tg_off, tg_vk, tg_ratio = (number(cells[5]), number(cells[6]),
                                          number(cells[7]), number(cells[8]))
        if None in (pp_rocm, pp_vk, pp_ratio, tg_on, tg_off, tg_vk, tg_ratio):
            findings.append('R2: model %s has a cell that holds no number' % key)
            continue
        want = round(pp_rocm / pp_vk, 2)
        if abs(want - pp_ratio) > TOLERANCE:
            findings.append('R2: model %s prefill ratio is %s, but %.1f / %.1f is %.2f'
                            % (key, pp_ratio, pp_rocm, pp_vk, want))
        want = round(tg_on / tg_vk, 2)
        if abs(want - tg_ratio) > TOLERANCE:
            findings.append('R3: model %s decode ratio is %s, but %.1f / %.1f is %.2f.'
                            ' The ratio and the decode column disagree'
                            % (key, tg_ratio, tg_on, tg_vk, want))
        if key in OPTION_DOES_NOTHING:
            if abs(tg_off - tg_on) / tg_on > SAME:
                findings.append('R4: model %s has the option worth %.1f against %.1f,'
                                ' and the quoted page states it launches no streams there'
                                % (key, tg_on, tg_off))
        elif tg_off >= tg_on:
            findings.append('R4: model %s has the option-off decode %.1f at or above the'
                            ' option-on decode %.1f. The two columns look swapped'
                            % (key, tg_off, tg_on))

    if DEFECT.search(text):
        for token in FIX_TOKENS:
            if token not in text:
                findings.append('R5: the document names the wrong-token defect but not "%s",'
                                ' the published fix' % token)

    for section in sections(text):
        low = section.lower()
        for figure, source in SOURCED_FIGURES:
            if figure.lower() in low and source.lower() not in low:
                head = section.splitlines()[0]
                findings.append('R6: figure %s appears under "%s" with no "%s" beside it'
                                % (figure, head.strip('# '), source))
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--doc', default=None)
    parser.add_argument('--root', default=None)
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root) if args.root else pathlib.Path(__file__).resolve().parents[2]
    path = pathlib.Path(args.doc) if args.doc else root / DOC
    findings = check_text(path.read_text(encoding='utf-8'))
    for f in findings:
        print('%s: %s' % (path.as_posix(), f))
    print('%s: %d findings' % (path.as_posix(), len(findings)))
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
