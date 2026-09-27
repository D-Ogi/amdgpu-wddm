import sys
from pathlib import Path
sys.path.insert(0, 'P:/BC-250/bc250-win/tools/win')
from target import Target

t = Target()
r = Path('P:/BC-250/scratch/witcher3/dx12/cpucopy-002')
r.mkdir(parents=True, exist_ok=True)
names = ['before-health.txt', 'after-health.txt', 'before-clock.txt', 'after-clock.txt', 'before-log-summary.txt', 'after-log-summary.txt', 'error.txt']
for s in ['A-witness']:
    for sz in ['1920x1200']:
        tag = s + '-' + sz
        names += ['present-' + tag + '.csv', 'modules-' + tag + '.json', 'vkcube-' + tag + '.stdout.txt', 'vkcube-' + tag + '.stderr.txt', tag + '.png']
for n in names:
    try:
        t.pull('C:\\BC250\\m12\\cpucopy-002\\' + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        (r / n).unlink(missing_ok=True)
        if n != 'error.txt':
            print(n, 'FAILED', e)
for n in ['run-cpucopy-002.log', 'done-cpucopy-002.json']:
    t.pull('C:\\BC250\\m12\\witcher3-dx12\\' + n, str(r / n))
    print(n, (r / n).stat().st_size)
raw = (r / 'run-cpucopy-002.log').read_bytes()
txt = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', 'replace')
(r / 'run-cpucopy-002.utf8.log').write_text(txt, encoding='utf-8')
print('\n'.join(l for l in txt.splitlines() if not l.startswith('t=')))
