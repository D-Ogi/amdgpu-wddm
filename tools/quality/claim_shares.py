"""Shares, denominators and modelled labels in the facts rows of docs/facts/data.

    python claim_shares.py [--root <repo>] [--baseline <file>] [--out <dir>]
    python claim_shares.py --files <path...>       # these files only; a file with no facts
                                                  # rows is read line by line, so a write-up
                                                  # can be checked before it is published
    python claim_shares.py --list-failing         # the failing rows as baseline lines

Four rules, all of them mechanical, all of them from the independent audit of 2026-10-10
(agent-discussion, findings A18 P3 and A19). They catch the ways a share went wrong in this
project's write-ups:

`share-arithmetic`
    A percentage that stands next to its own count pair must be that pair. `270 of the 418
    stalls (64.6 %)` passes, `0.836 of 4.641 (22.0 %)` does not, because 22.0 % is the share
    of the other number. The tolerance is 0.1 percentage points, which admits a truncated
    third digit (245 of 255 is 96.08 %, written 96.0 %) and refuses a reversed denominator
    (4.641 to 3.805 is 18.0 % of the first and 22.0 % of the second).

`difference-share`
    A difference with its own percentage, on a line that also holds the two measurements it is
    the difference of, is a share of the value it came down from. The HIP perf rerun wrote
    "| chain, us a dispatch | 3.805 | 4.641 | 0.836 us (22.0 %) |": 0.836 of 4.641 is 18.0 %,
    and 22.0 % is the overhead over the cached 3.805, a different question. The rule fires only
    where the smaller value is the denominator that matches and the line names no denominator
    ("relative to", "against the", "share of", "overhead"), so the fix is one phrase.

`count-order`
    `N of M` with N greater than M is a swapped pair.

`clock-model-label`
    A row that normalises a rate to an equivalent clock, or fits an exponent to one, states
    which of its numbers are measured and which come out of the model. The marker words are
    `estimated`, `modelled`/`modeled` or `model`. M842 said that The Witcher 3 "spends 76 % of
    its frame in RT"; the 76 % is `(76.4 - 18.1) / 76.4` of two frame times that a fitted
    `rate x (1.5 / GHz)^0.75` produced, not time read off a GPU timestamp inside an RT pass.
    No shader or driver change is needed to tell those apart, only the label.

The baseline is a ratchet, as in doclint and guardlog_width: it names the rows that already
carried an unlabelled normalisation when this gate landed, by rule and fact ID, and the gate
fails on any row outside it. Nothing adds a line by itself; `--list-failing` prints lines for
a human to read. A rule is not relaxed for a row by editing the row's numbers out of reach:
the share rules have no baseline at all, because an arithmetic error is never acceptable.
"""
import argparse
import pathlib
import re
import sys

# `<count> of [the] <count>`, then at most three short words, then its own percentage in
# brackets or after a comma. The short-word limit is what keeps the percentage of a different
# population out: in M798 "112 of 113 VSync-ended ones fell inside those six spans (14.2 % of
# the window)" the 14.2 % is the share of the window, seven words away, and does not match.
#
# A number followed by "'s" is a possessive, not a denominator: in M800 "6420 of 416's counted
# gaps" the 416 is the session, so the pattern refuses that form.
PAIR_PCT = re.compile(r"(\d+) of (?:the )?(\d+)(?!'s)\b(?:[ ,]+[A-Za-z][\w./-]*){0,3}[ ,]*"
                      r'\(?(\d+(?:\.\d+)?) ?%')
PAIR = re.compile(r"(\d+) of (?:the )?(\d+)(?!'s)\b")

# A difference with its own percentage: "0.836 us (22.0 %)" on a line that also holds the two
# measurements it is the difference of. A reduction is a share of the value it came down from.
DIFF_PCT = re.compile(r'(\d+(?:\.\d+)?)\s*(?:us|ms|ns|s|MHz|GHz|dwords|bytes)?\s*\**\s*'
                      r'\((\d+(?:\.\d+)?) ?%\)')
NUMBER = re.compile(r'\d+(?:\.\d+)?')
# Words that name which cost the percentage is a share of. With one of them the line says what
# it divided by, so the rule leaves it alone.
DENOMINATOR_NAMED = re.compile(r'relative to|against the|share of|overhead', re.I)

# A rate normalised to another clock, or an exponent fitted to one.
CLOCK_MODEL = re.compile(r'elasticity|equivalent clock|/ ?GHz\)|per GHz|normalised rate|'
                         r'frequency exponent|fitted|clock-normalis', re.I)
MODEL_LABEL = re.compile(r'estimat|model', re.I)

TOLERANCE = 0.1                 # percentage points: a truncated third digit, not a swap
ID = re.compile(r'\s*- id: (\S+)')


def rows(path):
    """[(fact id, text)] for a facts data file: every row's keys joined into one string.

    A file with no `- id:` row is read as a plain document, one row for each line, under the
    id `line <n>`. That is what `--files` is for: a write-up can be checked for a reversed
    denominator before it is published, while `evidence/` itself stays immutable.
    """
    text_all = pathlib.Path(path).read_text(encoding='utf-8')
    if not ID.search(text_all):
        return [('line %d' % n, line)
                for n, line in enumerate(text_all.splitlines(), 1) if line.strip()]
    out, fid, text = [], None, []
    for line in text_all.splitlines():
        m = ID.match(line)
        if m:
            if fid:
                out.append((fid, ' '.join(text)))
            fid, text = m.group(1), [line.strip()]
            continue
        if fid:
            text.append(line.strip())
    if fid:
        out.append((fid, ' '.join(text)))
    return out


def check_difference(fid, text):
    """[(rule, fact id, message)] for the `difference-share` rule over one row.

    It fires only where the reversed denominator is the one that matches: the percentage is
    the share of the smaller of the two measurements and not of the one the difference came
    down from. That is demonstrable, not a matter of taste, and the fix is one phrase.
    """
    if DENOMINATOR_NAMED.search(text):
        return []
    numbers = [float(t) for t in NUMBER.findall(text)]
    found = []
    for diff_text, pct_text in DIFF_PCT.findall(text):
        diff, pct = float(diff_text), float(pct_text)
        if diff <= 0:
            continue
        # The difference is written to a fixed number of digits, so the pair it came from may
        # differ by half of its last digit.
        decimals = len(diff_text.split('.')[1]) if '.' in diff_text else 0
        slack = 0.5 * 10 ** -decimals
        for i, a in enumerate(numbers):
            for b in numbers[i + 1:]:
                lo, hi = min(a, b), max(a, b)
                if lo <= 0 or abs((hi - lo) - diff) > slack:
                    continue
                over_hi, over_lo = 100.0 * diff / hi, 100.0 * diff / lo
                if abs(pct - over_hi) <= TOLERANCE or abs(pct - over_lo) > TOLERANCE:
                    continue
                found.append(('difference-share', fid,
                              '%s: "%s (%s %%)" is the difference of %g and %g, and %s %% is '
                              'its share of %g, the smaller one. The reduction from %g is '
                              '%.2f %%: name the cost the share is of, or use it'
                              % (fid, diff_text, pct_text, lo, hi, pct_text, lo, hi, over_hi)))
                return found                 # one message a row is enough
    return found


def check_row(fid, text):
    """[(rule, fact id, message)] for one row."""
    found = []
    for a, b, p in PAIR_PCT.findall(text):
        a, b, p = int(a), int(b), float(p)
        if b == 0:
            continue
        exact = 100.0 * a / b
        if abs(exact - p) > TOLERANCE:
            found.append(('share-arithmetic', fid,
                          '%s: "%d of %d" next to "%s %%" - %d/%d is %.2f %%, and %s %% is '
                          'the share of another number'
                          % (fid, a, b, ('%g' % p), a, b, exact, ('%g' % p))))
    for a, b in PAIR.findall(text):
        if int(a) > int(b):
            found.append(('count-order', fid,
                          '%s: "%s of %s" has the larger count first' % (fid, a, b)))
    found += check_difference(fid, text)
    m = CLOCK_MODEL.search(text)
    if m and not MODEL_LABEL.search(text):
        found.append(('clock-model-label', fid,
                      '%s: the row normalises to a clock ("%s") and no number is labelled '
                      'estimated or modelled' % (fid, m.group(0))))
    return found


def data_files(root):
    return sorted((pathlib.Path(root) / 'docs' / 'facts' / 'data').glob('*.yaml'))


def check(paths, baseline):
    """(failures, baselined) over the given facts data files."""
    failures, baselined = [], []
    for path in paths:
        for fid, text in rows(path):
            for rule, row_id, message in check_row(fid, text):
                where = '%s %s' % (path.name, message)
                if (rule, row_id) in baseline:
                    baselined.append('%s\t%s' % (rule, where))
                else:
                    failures.append('%s\t%s' % (rule, where))
    return failures, baselined


# Only clock-model-label may be baselined. An arithmetic error has no ratchet: a share that
# does not match its own counts is wrong in every row, old or new.
BASELINEABLE = ('clock-model-label',)


def load(path):
    """{(rule, fact id)} from a baseline file. A missing file is an empty baseline."""
    out = set()
    path = pathlib.Path(path)
    if not path.exists():
        return out
    for n, text in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if not text.strip() or text.lstrip().startswith('#'):
            continue
        parts = text.split('\t')
        if len(parts) != 2 or parts[0] not in BASELINEABLE:
            raise ValueError('%s line %d is not "<rule><tab><fact id>" with a rule of %s: %r'
                             % (path, n, '/'.join(BASELINEABLE), text))
        out.add((parts[0], parts[1].strip()))
    return out


def main(argv=None):
    here = pathlib.Path(__file__).resolve()
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--root', default=str(here.parents[2]))
    p.add_argument('--baseline', default=str(here.with_name('claim_shares_baseline.txt')))
    p.add_argument('--files', nargs='*', default=None)
    p.add_argument('--out', default='')
    p.add_argument('--list-failing', action='store_true',
                   help='print the failing rows as baseline lines and exit')
    a = p.parse_args(argv)

    paths = [pathlib.Path(f) for f in a.files] if a.files else data_files(a.root)
    baseline = load(a.baseline)
    failures, baselined = check(paths, baseline)

    if a.list_failing:
        for text in failures:
            rule, rest = text.split('\t', 1)
            if rule in BASELINEABLE:
                print('%s\t%s' % (rule, rest.split(':')[0].split(' ')[-1]))
        return 0

    report = ['claim-shares: %d input file(s), %d baselined row(s)'
              % (len(paths), len(baseline))]
    report += ['  baselined ' + t for t in baselined]
    report += ['FAIL: ' + t for t in failures]
    report.append('claim-shares: %s' % ('FAIL, %d row(s)' % len(failures) if failures
                                        else 'PASS'))
    print('\n'.join(report))
    if a.out:
        out = pathlib.Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        (out / 'claim-shares.txt').write_text('\n'.join(report) + '\n', encoding='utf-8')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
