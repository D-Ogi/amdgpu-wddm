import sys, json, collections
from pathlib import Path
sys.path.insert(0, str(Path('P:/BC-250/bc250-win/tools/win').resolve()))
from target import Target

r = Path('P:/BC-250/scratch/witcher3/dx12/cts002')
r.mkdir(parents=True, exist_ok=True)
t = Target()
rem = 'C:\\BC250\\m12\\cts-extsem-002\\'
for n in ['cases.jsonl', 'baseline-summary.json', 'candidate-summary.json', 'error.txt', 'before-health.txt', 'after-health.txt']:
    try:
        t.pull(rem + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        if n != 'error.txt':
            print(n, 'FAILED', e)
        else:
            (r / n).unlink(missing_ok=True)
for n in ['run-cts-extsem-002.log', 'done-cts-extsem-002.json']:
    try:
        t.pull('C:\\BC250\\m12\\witcher3-dx12\\' + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        print(n, 'FAILED', e)
raw = (r / 'run-cts-extsem-002.log').read_bytes()
txt = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', 'replace')
(r / 'run-cts-extsem-002.utf8.log').write_text(txt, encoding='utf-8')
print('\n'.join(l for l in txt.splitlines() if not l.startswith('t=') and ' exit=' not in l))
rows = [json.loads(l) for l in (r / 'cases.jsonl').read_text(encoding='utf-8-sig').splitlines() if l.strip()]
by = collections.defaultdict(collections.Counter)
icd = collections.defaultdict(collections.Counter)
for row in rows:
    by[row['phase']][row['status']] += 1
    icd[row['phase']][(row['icd_module_seen'], row['icd_in_loader_log'])] += 1
for ph in by:
    print(ph, dict(by[ph]), 'icd(module_seen,loader_log)=', dict(icd[ph]), 'ms total', sum(x['ms'] for x in rows if x['phase'] == ph))
base = {x['case']: x['status'] for x in rows if x['phase'] == 'baseline'}
cand = {x['case']: x['status'] for x in rows if x['phase'] == 'candidate'}
diff = [(c, base.get(c), cand.get(c)) for c in sorted(set(base) | set(cand)) if base.get(c) != cand.get(c)]
print('differences', len(diff))
for d in diff:
    print('  ', d)
print('non-pass:')
for x in rows:
    if x['status'] != 'Pass':
        print('  ', x['phase'], x['status'], x['exit'], x['case'])
