#!/usr/bin/env python3
"""One-off migration of the table-based docs/facts.md (main 43f186b3) into docs/facts/data/*.yaml.

Kept for provenance and for verify_migration.py, which reuses its row parser. Run once:

    python tools/facts/migrate_facts_md.py --old <old facts.md> [--report edges.txt]

What it does, and nothing more:
- every row becomes one fact; claim, status, detail, evidence, source and why are copied verbatim
  (cell text with the table escape `\\|` turned back into `|`);
- status is the leading status word of the status cell; the full cell is kept as status_text
  when it says more;
- date is the row's date column; rows without one take the first date in their evidence or text,
  and rows without any date the author date of the commit that added the row (date_from says which);
- area is assigned by keyword scoring (AREA_RULES); it is a stored field from now on, not recomputed;
- edges are derived from explicit wording only (EDGE_RULES) and marked auto; a plain mention of
  another ID is a `uses` edge. Mentions whose wording suggests a stronger relation that no rule
  covers are listed in the report for review instead of being guessed.
"""

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_facts  # noqa: E402

# ---------------------------------------------------------------- old table parser

TABLES = {
    ('Claim', 'Status', 'Detail', 'Evidence or date'): ('claim', 'status_text', 'detail', 'evidence_or_date'),
    ('Claim', 'Status', 'Source', 'Date'): ('claim', 'status_text', 'source', 'date_text'),
    ('Claim', 'Status', 'Why', 'Date'): ('claim', 'status_text', 'why', 'date_text'),
    ('Claim', 'Status', 'Evidence', 'Date'): ('claim', 'status_text', 'evidence', 'date_text'),
}
ROW_RE = re.compile(r'^\| ([MSR]\d+) \|')
ISO_RE = re.compile(r'^\d{4}-\d{2}-\d{2}$')


def split_cells(line):
    """Split a table row on pipes that are neither escaped nor inside a code span (escapes kept)."""
    cells, cur, i, n = [], [], 0, len(line)
    code = 0
    while i < n:
        ch = line[i]
        if ch == '\\' and i + 1 < n:
            cur.append(line[i:i + 2]); i += 2; continue
        if ch == '`':
            j = i
            while j < n and line[j] == '`':
                j += 1
            run = j - i
            if code == 0:
                if re.search(r'(?<!`)' + '`' * run + r'(?!`)', line[j:]):
                    code = run
            elif run == code:
                code = 0
            cur.append(line[i:j]); i = j; continue
        if ch == '|' and code == 0:
            cells.append(''.join(cur)); cur = []; i += 1; continue
        cur.append(ch); i += 1
    cells.append(''.join(cur))
    return cells[1:-1]


def github_cell_count(line):
    return len(re.split(r'(?<!\\)\|', line)) - 2


def unescape(cell):
    return cell.strip().replace('\\|', '|')


def parse_old(text):
    """Rows of the old facts.md in file order, and notes on rows GitHub rendered differently."""
    rows, notes, fields = [], [], None
    for lineno, line in enumerate(text.split('\n'), 1):
        if line.startswith('| # |'):
            fields = TABLES[tuple(c.strip() for c in split_cells(line)[1:])]
            continue
        m = ROW_RE.match(line)
        if not m:
            continue
        cells = split_cells(line)
        if not 4 <= len(cells) <= 5:
            raise SystemExit(f'line {lineno}: {len(cells)} cells')
        if github_cell_count(line) != len(cells):
            notes.append(f'{m.group(1)}: a pipe inside a code span split the row on GitHub '
                         f'({github_cell_count(line)} cells shown, {len(cells)} meant); now escaped')
        rec = {'id': m.group(1), 'line': lineno}
        for name, c in zip(fields, cells[1:]):
            rec[name] = unescape(c)
        eod = rec.pop('evidence_or_date', None)
        if eod is not None:
            rec['date_text' if ISO_RE.match(eod) else 'evidence'] = eod
        rows.append(rec)
    return rows, notes


# ---------------------------------------------------------------- areas

AREAS = [
    ('hardware', 'Hardware, registers and firmware',
     'Register-level behaviour of the GPU and the platform: MMIO, IP blocks, SMU, PSP, interrupts, '
     'memory controller, clocks, power and temperature, firmware and BIOS.'),
    ('kmd', 'Kernel driver and WDDM',
     'The kernel-mode driver under Windows: WDDM start, dxgkrnl and VidMm contracts, paging, GART and '
     'page tables, submission, fences, residency, TDR and bugchecks, packages and deployment.'),
    ('display', 'Display, DWM and Present',
     'Scanout and the display engine, VidPN and flips, the desktop compositor (DWM), Present paths and the '
     'Vulkan WSI.'),
    ('icd', 'ICD, Vulkan, OpenGL and compute',
     'The Vulkan ICD (RADV on WDDM), Vulkan CTS, OpenGL and OpenCL layers on it (Zink, piglit, clvk), '
     'and compute workloads such as llama.cpp.'),
    ('d3d', 'Direct3D through the system runtime',
     'Direct3D 9 to 12 and DXGI: the native user-mode driver behind the system runtime, the DXVK and '
     'vkd3d-proton engines, feature levels, DXR and conformance clients.'),
    ('games', 'Games and performance',
     'Games, benchmarks and measured performance: frame rates, frame times, presets, profiling, and '
     'comparisons with Linux.'),
    ('linux', 'Linux reference',
     'Measurements on unit A under Linux (amdgpu, Mesa) that serve as the reference for the Windows work.'),
    ('tooling', 'Tooling, build and lab',
     'Build, signing and quality gates, host tools and models, the lab itself (remote access, monitoring, '
     'recovery, power), and project process.'),
]

# (area, weight, regex). Scored over the claim (x3) and the other text (x1); the highest score wins,
# ties go to the earlier area in AREAS.
AREA_RULES = [
    ('hardware', 3, r'\bregisters?\b|\bMMIO\b|\bBAR[0-5]\b|\bSMU\b|\bPSP\b|\bRLC\b|\bGRBM\b|\bmm[A-Z][A-Z0-9_]{3,}|'
                    r'\bSCLK\b|\bMHz\b|\bmV\b|temperature|thermal|\bfirmware\b|\bBIOS\b|\bVBIOS\b|\bACPI\b|\bIOMMU\b|'
                    r'\bPCIe?\b|\bUTCL2\b|\bMMHUB\b|\bGFXHUB\b|\bIP discovery\b|\bmicrocode\b|\bMEC\b|\bKIQ\b|\bHQD\b|'
                    r'\bvoltage\b|\bclocks?\b|\bwatts?\b|\b\d+ W\b|\bDPM\b|\bSMN\b|\bNBIO\b|\bcarve-out\b|\bGRBM_\w+|\bCP_\w+'),
    ('kmd', 3, r'\bKMD\w*|\bminiport\b|\bbc250kmd\b|\bdxgkrnl\b|\bdxgmms2?\b|\bVidMm\b|\bpaging\b|\bGART\b|\bPTEs?\b|'
               r'\bpage tables?\b|\bDxgkDdi\w*|\bDxgkCb\w*|\bDXGK_\w+|\bresidency\b|\bsegments?\b|\bfence\b|\bTDR\b|'
               r'\bbugchecks?\b|\b0x116\b|\bISR\b|\bDPC\b|\bSDMA\d?\b|\bsubmission|\bIB\b|\bWDDM\b|\bescape\b|'
               r'\bdriver package\b|\bINF\b|\bFullWddm\b|\bfull[- ]WDDM\b|\bfull start\b|\bdisplay-only\b|\bLocal0\d+|'
               r'\bpage fault|\bVMID\b|\bIH\b|\bring\b|\bGPU VA\b|\ballocations?\b|\bpages?\b|\bFill\b|\bTransfer\b|'
               r'\bwalker\b|\baperture\b|\balias\w*|\bretirement\b|\bcapture[ -]plan\b|\bPFNs?\b|\bMDLs?\b'),
    ('display', 3, r'\bDWM\d*\b|\bPresents?\b|\bflips?\b|\bVidPN\b|\bscanout\b|\bDCN\b|\bHUBP\d?\b|\bOTG\d?\b|\bVUPDATE\w*|'
                   r'\bframebuffer\b|\bprimary\b|\bdesktop\b|\bmonitor\b|\bWSI\b|\bswap ?chain\b|\bcompos\w+|\bcursor\b|'
                   r'\bdisplay\b(?!-only)|\bvsync\b|\bDirectComposition\b|\bDComp\b|\bdirty\b|\bBlt\b|\bLB7A\b|\bGDI\b|\bCDD\b'),
    ('icd', 3, r'\bVulkan\w*|\bRADV\b|\bICD\w*|\bCTS\b|\bvulkaninfo\b|\bpiglit\b|\bWGL\b|\bOpenGL\b|\bGLX\b|\bclvk\b|\bMesa\b|'
               r'\bllama\w*|\bstories15M\b|\bTinyLlama\b|\bSPIR-V\b|\bACO\b|\bvkcube\b|\bsparse\b|\bwinsys\b|\bkopper\b|'
               r'\bcompute\b(?!-unit)|\bshaders?\b|\bdispatch\w*'),
    ('d3d', 4, r'\bD3D1[012]\b|\bD3D9\b|\bDirect3D\b|\bDXGI\b|\bvkd3d\w*|\bDXVK\b|\bUMD\w*|\bd3d10umd\b|\bd3d1[12]\w*|'
               r'\bruntime\d{3}\b|\bsystem runtime\b|\bfeature level\b|\bFL ?1[12]_[01]\b|\bDXR\b|\bDXIL\b|\bHLSL\b|'
               r'\bengine ABI\b|\bABI ?1\.\d\b|\bshell\b|\bconformance client\b|\bZink\b|\bD3DKMT\w*|\bRaytracing\w*|'
               r'\bROV\b|\bCreateDevice\b|\bDDI\b'),
    ('games', 5, r'Witcher|\bgames?\b|\bfps\b|\bframe ?rates?\b|\bframe times?\b|\bpresets?\b|3DMark|Tomb Raider|'
                 r'The Ascent|Factorio|PresentMon|\bbenchmark\w*|\bSteam\b|\bthroughput\b|\bms/frame\b|'
                 r'\bpp512\b|\btg128\b'),
    ('linux', 4, r'\bunder Linux\b|\bLinux ?6\.\d+|\bmodprobe\b|\bdebugfs\b|\bAlpine\b|\bdiagnostic USB\b|'
                 r'\bumr\b|\bdmesg\b|\bsysfs\b|\bkernel 6\.\d+|\bXorg\b|\bGLX\b'),
    ('tooling', 2, r'\bregcalc\b|\bbuilds?\b|\bquality gates?\b|\bcatalogs?\b|\baudit\w*|\bmanifest\b|\bharness\b|'
                   r'\boverlay\b|\bbc250mon\b|\bSSH\b|\bsmart plug\b|\bscript\w*|\bCLI\b|\bcompil\w+|'
                   r'\bsource identity\b|\bunit tests?\b|\bWi-Fi\b|\bKDNET\b|\bWinDbg\b|\bcdb\b|\bstaging\b|\bsoak\b'),
]
AREA_RE = [(a, w, re.compile(rx, re.I if a in ('games',) else 0)) for a, w, rx in AREA_RULES]
AREA_ORDER = [a for a, _, _ in AREAS]


def classify(row):
    claim = row['claim']
    rest = ' '.join(row.get(k) or '' for k in ('detail', 'evidence', 'source', 'why'))
    score = collections.Counter()
    for area, w, rx in AREA_RE:
        score[area] += w * (3 * len(rx.findall(claim)) + len(rx.findall(rest)))
    if re.search(r'evidence/linux/', rest) and not re.search(r'evidence/windows/', rest):
        score['linux'] += 15
    if row['id'].startswith(('S', 'R')) and score['hardware']:
        score['hardware'] += 5
    if not score or max(score.values()) == 0:
        return 'kmd'  # most rows without any keyword describe kernel-driver internals
    best = max(score.values())
    return next(a for a in AREA_ORDER if score[a] == best)


# ---------------------------------------------------------------- dates

DATE_RE = re.compile(r'20\d\d-\d\d-\d\d')
EVIDENCE_DATE_RE = re.compile(r'evidence/(?:linux|windows)/(20\d\d-\d\d-\d\d)-')


def derive_date(row, repo, old_path_in_git):
    if 'date_text' in row:
        return row['date_text'], None
    for key in ('evidence', 'detail', 'claim', 'status_text'):
        text = row.get(key) or ''
        m = EVIDENCE_DATE_RE.search(text)
        if m:
            return m.group(1), 'evidence-path'
        m = DATE_RE.search(text)
        if m:
            return m.group(0), 'text'
    out = subprocess.run(['git', '-C', str(repo), 'log', '-S', f'| {row["id"]} |', '--format=%as', '--reverse',
                          '--', old_path_in_git], capture_output=True, text=True, check=True).stdout.split()
    if not out:
        raise SystemExit(f'{row["id"]}: no date anywhere')
    return out[0], 'git'


# ---------------------------------------------------------------- edges

MENTION_RE = re.compile(r'(?<![\w.-])([MSR])(\d+)(?!\w)(?!\.\d)')
RANGE_RE = re.compile(r'(?<![\w.-])M(\d+)-M(\d+)(?!\w)')
# Up to here the old table used M1-M15 for the roadmap milestones (from M26 on every mention of
# M1-M15 is a milestone, e.g. "M8 is not reached", "full M9 acceptance") and S0-S5 for ACPI sleep
# states ("one real S4"). Checked mention by mention when the table was migrated.
MILESTONE_CUTOFF = 26
MILESTONE_MAX = 15
BROAD_CUE = re.compile(r'supersed|refut|contradict|correct|confirm|replac|revis|wrong|retract|invalid|narrow|'
                       r'overturn|withdraw|instead of|explains?|disprov|not as', re.I)


def ID(i):
    return f'(?:[MSR]{i})'


# Each rule: (name, test(before, after, field, row) -> match, subject, type, partial)
# before/after: text left and right of the mention. subject 'self' = the row's own fact, 'other' = the mentioned one.
def _bare(after):
    return re.match(r'^\s*(?:[.;)]|$)', after) is not None


EDGE_RULES = [
    ('REFUTED in part by Y', lambda b, a, f, r: re.search(r'REFUTED in part by $', b), 'other', 'refutes', True),
    ('SUPERSEDED IN ITS CAUSE by Y', lambda b, a, f, r: re.search(r'SUPERSEDED IN ITS CAUSE by $', b), 'other', 'supersedes', True),
    ('corrected by Y', lambda b, a, f, r: re.search(r'(?i)corrected by $', b), 'other', 'supersedes', True),
    ('correction from/in/: Y', lambda b, a, f, r: re.search(r'(?i)correction(?: from| in|:) $', b), 'other', 'supersedes', True),
    ('supersedes/replaces Y', lambda b, a, f, r: re.search(r'(?i)\b(?:supersedes|replaces) $', b), 'self', 'supersedes', None),
    ('corrects Y', lambda b, a, f, r: re.search(r'(?i)\bcorrects(?: [\w-]+){0,2} $', b), 'self', 'supersedes', None),
    ("Y's ... is corrected", lambda b, a, f, r: re.match(r"^(?:'s)?(?: [\w-]+){0,3} (?:is |was )?corrected\b", a), 'self', 'supersedes', True),
    ('Y ... corrects', lambda b, a, f, r: re.match(r'^(?: later)?(?: [\w-]+){0,4}? (?:and )?corrects\b', a), 'other', 'supersedes', True),
    ("Y's ... was wrong", lambda b, a, f, r: re.match(r"^(?:'s)?(?: [\w-]+){0,2} (?:was|is) wrong\b", a), 'self', 'refutes', True),
    ('Confirms Y', lambda b, a, f, r: re.search(r'(?i)\bconfirms $', b), 'self', 'supports', None),
    ('(see Y: MEASURED/CONFIRMED)', lambda b, a, f, r: re.search(r'\(see $', b) and re.match(r'^[^:;)]*: (?:MEASURED|CONFIRMED)\b', a), 'other', 'supports', None),
    ('Y: the ... does not hold', lambda b, a, f, r: re.match(r'^: the [^;)]* does not hold\b', a), 'other', 'refutes', True),
    ('Y hold on hardware', lambda b, a, f, r: re.match(r'^(?:, [MSR]\d+)* hold on hardware\b', a), 'self', 'supports', None),
    ('REFUTED (by measurement, Y)', lambda b, a, f, r: f == 'status_text' and re.search(r'^REFUTED \(by [^)]*measurement, (?:[MSR]\d+ \+ )?$', b), 'other', 'refutes', None),
    ('Contradicts ... (Y)', lambda b, a, f, r: re.search(r'Contradicts [^.]*\($', b), 'self', 'refutes', None),
]


def window(text, start, end, width=70):
    s, e = max(0, start - width), min(len(text), end + width)
    return ('...' if s else '') + text[s:e] + ('...' if e < len(text) else '')


def derive_edges(rows, ids):
    """Returns (edges by subject id, review list, excluded mention counts)."""
    edges = collections.defaultdict(dict)  # subject -> (type, to) -> edge
    review, excluded = [], collections.Counter()
    excluded_list = []
    for row in rows:
        x = row['id']
        num = int(x[1:]) if x[0] == 'M' else 0
        for field in gen_facts.TEXT_FIELDS:
            text = row.get(field) or ''
            mentions = [(m.start(), m.end(), m.group(1) + m.group(2)) for m in MENTION_RE.finditer(text)]
            for m in RANGE_RE.finditer(text):
                lo, hi = int(m.group(1)), int(m.group(2))
                if 0 < hi - lo <= 5:
                    mentions += [(m.start(), m.end(), f'M{k}') for k in range(lo + 1, hi + 1)]
                else:
                    review.append((x, 'range', f'M{lo}-M{hi}', 'not expanded', window(text, m.start(), m.end())))
            for start, end, y in mentions:
                if y == x:
                    continue
                if y not in ids:
                    excluded['not a fact ID'] += 1
                    excluded_list.append((x, y, 'not a fact ID', window(text, start, end, 40)))
                    continue
                if num >= MILESTONE_CUTOFF and ((y[0] == 'M' and int(y[1:]) <= MILESTONE_MAX) or y[0] == 'S'):
                    kind = 'milestone' if y[0] == 'M' else 'ACPI sleep state'
                    excluded[kind] += 1
                    excluded_list.append((x, y, kind, window(text, start, end, 40)))
                    continue
                before, after = text[max(0, start - 80):start], text[end:end + 100]
                cue = window(text, start, end)
                hit = None
                for name, test, subj, typ, partial in EDGE_RULES:
                    if test(before, after, field, row):
                        hit = (name, subj, typ, partial)
                        break
                if hit:
                    name, subj, typ, partial = hit
                    if partial is None:
                        partial = not _bare(after) if typ == 'supersedes' else False
                    s, t = (x, y) if subj == 'self' else (y, x)
                    edge = {'type': typ, 'to': t, 'auto': True}
                    if partial:
                        edge['scope'] = 'partial'
                    if s != x:
                        edge['cue_in'] = x
                    edge['cue'] = cue
                    edges[s].setdefault((typ, t), edge)
                else:
                    edges[x].setdefault(('uses', y), {'type': 'uses', 'to': y, 'auto': True, 'cue': cue})
                    if BROAD_CUE.search(text[max(0, start - 60):end + 60]):
                        review.append((x, 'uses', y, 'wording may mean more than a reference', cue))
    # a stronger relation between the same pair replaces the plain reference
    for s, es in edges.items():
        for (typ, t) in [k for k in es if k[0] != 'uses']:
            es.pop(('uses', t), None)
    review = [r for r in review if r[1] != 'uses' or ('uses', r[2]) in edges[r[0]]]
    return edges, review, excluded, excluded_list


# ---------------------------------------------------------------- main

def migrate(old_text, repo, old_path_in_git):
    rows, notes = parse_old(old_text)
    ids = {r['id'] for r in rows}
    if len(ids) != len(rows):
        raise SystemExit('duplicate IDs in the old table')
    edges, review, excluded, excluded_list = derive_edges(rows, ids)
    areas = {key: {'area': key, 'order': i + 1, 'title': title, 'description': desc, 'facts': []}
             for i, (key, title, desc) in enumerate(AREAS)}
    for row in rows:
        status = gen_facts.leading_status(row['status_text'])
        if status is None:
            raise SystemExit(f'{row["id"]}: no status word in {row["status_text"]!r}')
        date, date_from = derive_date(row, repo, old_path_in_git)
        fact = {'id': row['id'], 'status': status, 'date': date}
        if date_from:
            fact['date_from'] = date_from
        for key in gen_facts.TEXT_FIELDS:
            if key in row and not (key == 'status_text' and row[key] == status):
                fact[key] = row[key]
        if edges.get(row['id']):
            fact['edges'] = list(edges[row['id']].values())
        areas[classify(row)]['facts'].append(fact)
    return rows, notes, areas, review, excluded, excluded_list


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--old', required=True, help='the old table-based facts.md')
    ap.add_argument('--root', default=str(gen_facts.REPO))
    ap.add_argument('--report', help='write the edge review list here')
    args = ap.parse_args()
    root = Path(args.root)
    rows, notes, areas, review, excluded, excluded_list = migrate(
        Path(args.old).read_text(encoding='utf-8'), root, 'docs/facts.md')
    data = root / 'docs' / 'facts' / 'data'
    data.mkdir(parents=True, exist_ok=True)
    for key, a in areas.items():
        (data / f'{key}.yaml').write_text(gen_facts.dump_area(a), encoding='utf-8', newline='\n')
    print(f'{len(rows)} rows ->', ', '.join(f'{k} {len(a["facts"])}' for k, a in areas.items()))
    for n in notes:
        print('note:', n)
    counts = collections.Counter()
    for a in areas.values():
        for f in a['facts']:
            for e in f.get('edges', []):
                counts[e['type'] + (' (partial)' if e.get('scope') else '')] += 1
    print('edges:', dict(counts))
    print('mentions not taken as edges:', dict(excluded))
    print('review:', len(review))
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as fh:
            fh.write('# Edge review list\n')
            for r in review:
                fh.write(' | '.join(r) + '\n')
            fh.write('# Mentions not taken as edges\n')
            for r in excluded_list:
                fh.write(' | '.join(r) + '\n')


if __name__ == '__main__':
    main()
