import sys
from pathlib import Path
sys.path.insert(0, str(Path('P:/BC-250/bc250-win/tools/win').resolve()))
from target import Target

r = Path('P:/BC-250/scratch/witcher3/dx12/run003')
r.mkdir(parents=True, exist_ok=True)
t = Target()
names = ['presentmon.csv', 'presentmon.stdout.txt', 'presentmon.stderr.txt', 'modules.json', 'result.json', 'launch.json',
         'vkd3d.log', 'witcher3_dxgi.log', 'before-health.txt', 'after-health.txt', 'before-clock.txt', 'after-clock.txt',
         'dxvk.conf', 'error.txt'] + ['auto-%03d.png' % i for i in range(0, 20)]
for n in names:
    try:
        t.pull('C:\\BC250\\m12\\witcher3-dx12-003\\' + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        if not n.startswith('auto-') and n != 'error.txt':
            print(n, 'FAILED', e)
        else:
            (r / n).unlink(missing_ok=True)
for n in ['run-w3dx12-003.log', 'done-w3dx12-003.json', 'input-003.log']:
    try:
        t.pull('C:\\BC250\\m12\\witcher3-dx12\\' + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        print(n, 'FAILED', e)
raw = (r / 'run-w3dx12-003.log').read_bytes()
txt = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', 'replace')
(r / 'run-w3dx12-003.utf8.log').write_text(txt, encoding='utf-8')
ts = [l for l in txt.splitlines() if l.startswith('t=')]
print('samples', len(ts), 'first', ts[0] if ts else '', 'peak tctl', max(float(l.split('tctl=')[1].split()[0]) for l in ts) if ts else '')
print('\n'.join(l for l in txt.splitlines() if not l.startswith('t=')))
