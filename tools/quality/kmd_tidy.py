"""clang-tidy over the kernel-mode driver, as a ratchet gate.

    kmd_tidy.py --database <out>/kmd/compile_commands.json --out <dir>            the gate
    kmd_tidy.py ... --prune                                                       lower the baseline to today
    kmd_tidy.py ... --accept                                                      write every finding of today
                                                                                  into the baseline (a human
                                                                                  reviews the diff)

The check set is driver/kmd/.clang-tidy, which also says why each excluded check is excluded. The commands are
the cl.exe commands of driver/kmd/build.ps1, replayed with clang-cl (kmd_clang.py).

The baseline (kmd_tidy_baseline.txt) has one line a finding kind: <count> <check> <file> <key> <message>. The key
is a hash of the message and of the text of the source line, without the line number, so an edit above a finding
does not move it out of the baseline. The gate fails when a key has more findings than its baseline count, and when
clang cannot compile a translation unit (the gate cannot judge code that it cannot parse). A key below its count is
a note: --prune lowers the line. docs/testing/kmd-static-checks.md tells how to read a finding.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kmd_clang  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
CONFIG = REPO / 'driver' / 'kmd' / '.clang-tidy'
BASELINE = HERE / 'kmd_tidy_baseline.txt'
MATCH = r'/driver/(kmd|shim)/[^/]+\.c$'
# Warnings that the check set asks for and that are not on by default in clang-cl.
EXTRA_WARNINGS = ['-Wswitch-enum']

# file:line:col: warning: text [check]  (clang style) or file(line,col): warning: text [check]  (MSVC style)
_DIAG = re.compile(r'^(?P<file>.+?)(?::(?P<l1>\d+):(?P<c1>\d+):|\((?P<l2>\d+),(?P<c2>\d+)\)\s*:)\s*'
                   r'(?P<sev>warning|error):\s*(?P<msg>.*?)\s*\[(?P<check>[^\]]+)\]\s*$')
_ERROR = re.compile(r'(?:^|\s)(?:fatal )?error:', re.M)


def normalise(message):
    """The message without numbers that name a place (a line or a column), which move with an edit."""
    message = re.sub(r'\b(line|column) \d+', r'\1 N', message)
    return re.sub(r'\s+', ' ', message).strip()


def finding_key(message, source_line):
    text = normalise(message) + '\n' + re.sub(r'\s+', ' ', source_line).strip()
    return hashlib.sha256(text.encode('utf-8')).hexdigest()[:16]


class SourceLines:
    def __init__(self):
        self.files = {}

    def line(self, path, number):
        if path not in self.files:
            try:
                self.files[path] = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
            except OSError:
                self.files[path] = []
        lines = self.files[path]
        return lines[number - 1] if 0 < number <= len(lines) else ''


def parse_output(text, repo, lines):
    """The findings of one clang-tidy output: a dict a finding, with the place and the line-free key."""
    found = []
    for raw in text.splitlines():
        m = _DIAG.match(raw.strip())
        if not m:
            continue
        line = int(m.group('l1') or m.group('l2'))
        col = int(m.group('c1') or m.group('c2'))
        path = str(Path(m.group('file')).resolve())
        found.append({'file': kmd_clang.repo_relative(path, repo), 'line': line, 'col': col,
                      'severity': m.group('sev'), 'check': m.group('check'), 'message': m.group('msg'),
                      'key': finding_key(m.group('msg'), lines.line(path, line))})
    return found


def unique_findings(findings):
    """A header finding comes once from every translation unit that includes the header: count it once."""
    seen = {}
    for f in findings:
        seen.setdefault((f['file'], f['line'], f['col'], f['check'], f['message']), f)
    return sorted(seen.values(), key=lambda f: (f['file'], f['line'], f['col'], f['check'], f['message']))


def count_keys(findings):
    counts = {}
    for f in findings:
        k = (f['check'], f['file'], f['key'])
        counts[k] = counts.get(k, 0) + 1
    return counts


def read_baseline(path):
    rows = {}
    if not Path(path).is_file():
        return rows
    for raw in Path(path).read_text(encoding='utf-8').splitlines():
        if not raw.strip() or raw.startswith('#'):
            continue
        parts = raw.split('\t')
        if len(parts) < 4:
            raise ValueError('Malformed baseline line: ' + raw)
        rows[(parts[1], parts[2], parts[3])] = int(parts[0])
    return rows


BASELINE_HEAD = [
    '# kmd-tidy ratchet baseline: the clang-tidy findings of the kernel-mode driver on the day this gate started',
    '# (2026-11-04). The checks and the reason for each excluded check: driver/kmd/.clang-tidy. The gate:',
    '# tools/quality/kmd_tidy.py, run by tools/quality/quick.ps1 (kmd-tidy).',
    '#',
    '# One line a finding kind: <count><tab><check><tab><file><tab><key><tab><message>. The key is a hash of the',
    '# message and the text of the source line, so a line move keeps the finding in its line. The message column',
    '# is for the reader only. The gate fails when a key has more findings than its count. --prune lowers counts',
    '# after a fix; --accept adds the findings of today, and the reviewer of that change reads each new line.',
]


def write_baseline(path, counts, messages):
    out = list(BASELINE_HEAD)
    for k in sorted(counts):
        out.append('\t'.join([str(counts[k]), k[0], k[1], k[2], messages.get(k, '')]))
    Path(path).write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')


def compare(counts, allowance):
    new = {k: (n, allowance.get(k, 0)) for k, n in counts.items() if n > allowance.get(k, 0)}
    lower = {k: (counts.get(k, 0), n) for k, n in allowance.items() if counts.get(k, 0) < n}
    return new, lower


def tidy_one(entry, clang_tidy, config, database_dir, timeout):
    source = str(Path(entry['directory'], entry['file']))
    cmd = [str(clang_tidy), '--quiet', '--config-file=' + str(config), '-p', str(database_dir), source]
    start = time.monotonic()
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {'file': source, 'code': None, 'output': 'timeout after %d s' % timeout,
                'seconds': round(time.monotonic() - start, 2)}
    text = (r.stdout + r.stderr).decode('utf-8', errors='replace')
    return {'file': source, 'code': r.returncode, 'output': text, 'seconds': round(time.monotonic() - start, 2)}


def run(args):
    started = time.monotonic()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    llvm = Path(args.llvm) if args.llvm else kmd_clang.default_llvm_bin()
    clang_tidy, clang_cl = llvm / 'clang-tidy.exe', llvm / 'clang-cl.exe'
    for tool in (clang_tidy, clang_cl):
        if not tool.is_file():
            raise FileNotFoundError('kmd-tidy needs ' + str(tool) + ' (--llvm or BC250_LLVM_BIN)')
    entries = kmd_clang.load_entries(args.database, args.match)
    translated = [{'directory': e['directory'], 'file': str(Path(e['directory'], e['file'])),
                   'arguments': kmd_clang.clang_cl_argv(e, clang_cl)[:-1] + EXTRA_WARNINGS
                   + [str(Path(e['directory'], e['file']))]} for e in entries]
    (out / 'compile_commands.json').write_text(json.dumps(translated, indent=1), encoding='utf-8')
    config = Path(args.config).resolve()
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda e: tidy_one(e, clang_tidy, config, out, args.timeout), translated))
    lines = SourceLines()
    findings, broken = [], []
    logs = out / 'logs'
    logs.mkdir(exist_ok=True)
    for r in results:
        name = Path(r['file']).name
        (logs / (name + '.log')).write_text(r['output'], encoding='utf-8')
        # clang-tidy reports a compile error as clang-diagnostic-error and exits non-zero.
        if r['code'] is None or r['code'] != 0 or _ERROR.search(r['output']):
            first = [x for x in r['output'].splitlines() if 'error' in x][:3] or [r['output'][:300]]
            broken.append({'file': kmd_clang.repo_relative(r['file'], REPO), 'code': r['code'], 'errors': first})
        findings += parse_output(r['output'], REPO, lines)
    findings = unique_findings(findings)
    counts = count_keys(findings)
    messages = {}
    for f in findings:
        messages.setdefault((f['check'], f['file'], f['key']), f['message'])
    baseline = Path(args.baseline)
    if args.accept:
        write_baseline(baseline, counts, messages)
        print('%s: %d lines written from %d findings' % (baseline, len(counts), len(findings)))
    allowance = read_baseline(baseline)
    new, lower = compare(counts, allowance)
    if args.prune and not new:
        kept = {k: min(n, counts.get(k, 0)) for k, n in allowance.items() if counts.get(k, 0)}
        old = {k: messages.get(k, '') for k in kept}
        old.update({k: m for k, m in _baseline_messages(baseline).items() if k in kept and not old.get(k)})
        write_baseline(baseline, kept, old)
        print('%s: pruned to %d lines' % (baseline, len(kept)))
        allowance = read_baseline(baseline)
        new, lower = compare(counts, allowance)
    per_check = {}
    for f in findings:
        per_check[f['check']] = per_check.get(f['check'], 0) + 1
    status = 'PASS' if not new and not broken else 'FAIL'
    report = {'status': status, 'translation_units': len(entries), 'findings': len(findings),
              'per_check': dict(sorted(per_check.items())), 'broken': broken,
              'new': [{'check': k[0], 'file': k[1], 'key': k[2], 'count': n, 'allowed': a,
                       'places': [f'{f["file"]}:{f["line"]}:{f["col"]} {f["message"]}' for f in findings
                                  if (f['check'], f['file'], f['key']) == k]} for k, (n, a) in sorted(new.items())],
              'below_baseline': [{'check': k[0], 'file': k[1], 'key': k[2], 'count': n, 'allowed': a}
                                 for k, (n, a) in sorted(lower.items())],
              'seconds': round(time.monotonic() - started, 1),
              'slowest': sorted(({'file': Path(r['file']).name, 'seconds': r['seconds']} for r in results),
                                key=lambda x: -x['seconds'])[:5]}
    (out / 'report.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
    (out / 'findings.json').write_text(json.dumps(findings, indent=1), encoding='utf-8')
    for b in broken:
        print('FAIL clang cannot compile %s (exit %s): %s' % (b['file'], b['code'], ' | '.join(b['errors'])))
    for item in report['new']:
        print('FAIL new %s in %s (%d, baseline %d):' % (item['check'], item['file'], item['count'], item['allowed']))
        for p in item['places']:
            print('  ' + p)
    for item in report['below_baseline']:
        print('note: %s %s %s is down to %d from %d (--prune)' % (item['check'], item['file'], item['key'],
                                                                   item['count'], item['allowed']))
    print('kmd-tidy %s: %d translation units, %d findings, %d new, %d broken, %.1f s' % (
        status, len(entries), len(findings), len(new), len(broken), report['seconds']))
    for check, n in report['per_check'].items():
        print('  %5d %s' % (n, check))
    return 0 if status == 'PASS' else 1


def _baseline_messages(path):
    rows = {}
    if Path(path).is_file():
        for raw in Path(path).read_text(encoding='utf-8').splitlines():
            parts = raw.split('\t')
            if not raw.startswith('#') and len(parts) >= 5:
                rows[(parts[1], parts[2], parts[3])] = parts[4]
    return rows


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--database', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--match', default=MATCH)
    p.add_argument('--config', default=CONFIG, type=Path)
    p.add_argument('--baseline', default=BASELINE, type=Path)
    p.add_argument('--llvm', type=Path, help='the LLVM bin directory (default: BC250_LLVM_BIN or the workspace)')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--timeout', type=int, default=180, help='seconds a translation unit')
    g = p.add_mutually_exclusive_group()
    g.add_argument('--prune', action='store_true')
    g.add_argument('--accept', action='store_true')
    return run(p.parse_args(argv))


if __name__ == '__main__':
    sys.exit(main())
