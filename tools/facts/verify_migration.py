#!/usr/bin/env python3
"""Proof that the migration lost nothing: the old table-based facts.md against docs/facts/data/.

    python tools/facts/verify_migration.py [--rev 43f186b3]     (old file read from git)
    python tools/facts/verify_migration.py --old <old facts.md>

Checks: the same ID set; for every row, claim, status, detail, evidence, source and why equal
after whitespace normalisation (runs of whitespace -> one space, ends stripped; the table escape
`\\|` counts as `|`); the date column equal to `date` where the row had one, and every other
row's date marked as derived. Independently of the field mapping, every non-empty cell of every
row must equal one text field of its fact. Exit 1 on any difference.
"""

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_facts  # noqa: E402
from migrate_facts_md import parse_old, split_cells, ROW_RE  # noqa: E402


def norm(text):
    return re.sub(r'\s+', ' ', (text or '').replace('\\|', '|')).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=str(gen_facts.REPO))
    ap.add_argument('--rev', default='43f186b3', help='git revision of the old docs/facts.md')
    ap.add_argument('--old', help='read the old facts.md from this file instead of git')
    args = ap.parse_args()
    root = Path(args.root)
    if args.old:
        old = Path(args.old).read_text(encoding='utf-8')
        source = args.old
    else:
        old = subprocess.run(['git', '-C', str(root), 'show', f'{args.rev}:docs/facts.md'],
                             capture_output=True, check=True).stdout.decode('utf-8')
        source = f'{args.rev}:docs/facts.md'
    rows, _ = parse_old(old)
    raw = {}
    for line in old.split('\n'):
        m = ROW_RE.match(line)
        if m:
            raw[m.group(1)] = [norm(c) for c in split_cells(line)[1:]]
    facts = {f['id']: f for a in gen_facts.load(root) for f in a['facts']}

    old_ids, new_ids = {r['id'] for r in rows}, set(facts)
    kinds = collections.Counter(i[0] for i in old_ids)
    print(f'old: {source}: {len(rows)} rows ({kinds["M"]} M, {kinds["S"]} S, {kinds["R"]} R)')
    kinds_new = collections.Counter(i[0] for i in new_ids)
    print(f'new: docs/facts/data: {len(facts)} facts ({kinds_new["M"]} M, {kinds_new["S"]} S, {kinds_new["R"]} R)')
    missing, extra = sorted(old_ids - new_ids, key=gen_facts.id_key), sorted(new_ids - old_ids, key=gen_facts.id_key)
    # Facts added after the migration carry IDs above the old range; an extra ID inside it would be invented.
    top = {k: max(gen_facts.id_key(i)[1] for i in old_ids if i[0] == k) for k in kinds}
    added = [i for i in extra if i[0] in top and gen_facts.id_key(i)[1] > top[i[0]]]
    extra = [i for i in extra if i not in added]
    changed = []
    fields_checked = 0
    for r in rows:
        f = facts.get(r['id'])
        if f is None:
            continue
        for key in ('claim', 'detail', 'evidence', 'source', 'why'):
            fields_checked += key in r
            if norm(r.get(key)) != norm(f.get(key)):
                changed.append(f'{r["id"]}.{key}')
        fields_checked += 1
        if norm(r['status_text']) != norm(f.get('status_text') or f['status']):
            changed.append(f'{r["id"]}.status')
        if 'date_text' in r:
            fields_checked += 1
            if r['date_text'] != f['date'] or 'date_from' in f:
                changed.append(f'{r["id"]}.date')
        elif 'date_from' not in f:
            changed.append(f'{r["id"]}.date (derived but not marked)')
        texts = {norm(f.get(k)) for k in gen_facts.TEXT_FIELDS} | {f['status'], f['date']}
        for i, c in enumerate(raw[r['id']]):
            if c and c not in texts:
                changed.append(f'{r["id"]}: cell {i + 2} not found verbatim')
    print(f'fields compared: {fields_checked}')
    print(f'missing IDs: {len(missing)} {missing[:20]}')
    print(f'extra IDs: {len(extra)} {extra[:20]}')
    print(f'added after the migration: {len(added)} {added[:20]}')
    print(f'changed: {len(changed)} {changed[:20]}')
    derived = collections.Counter(f.get('date_from') for f in facts.values() if 'date_from' in f)
    print('dates derived (no date column in the old row):', dict(derived))
    try:
        import yaml  # optional cross-check that the files are plain YAML; not needed by the tools
        same = all(yaml.safe_load(p.read_text(encoding='utf-8'))['facts'] ==
                   [{k: v for k, v in f.items()} for f in gen_facts.parse_area_file(p)['facts']]
                   for p in sorted((root / 'docs' / 'facts' / 'data').glob('*.yaml')))
        print(f'PyYAML {yaml.__version__} reads the data files identically: {same}')
        if not same:
            changed.append('PyYAML disagrees')
    except ImportError:
        print('PyYAML not installed: YAML cross-check skipped')
    ok = not missing and not extra and not changed
    print('PASS no loss' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
