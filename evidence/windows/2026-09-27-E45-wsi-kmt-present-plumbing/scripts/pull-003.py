import sys
from pathlib import Path
sys.path.insert(0, str(Path('P:/BC-250/bc250-win/tools/win').resolve()))
from target import Target

r = Path('P:/BC-250/scratch/wsi-kmt-2026-09-27/run003')
r.mkdir(parents=True, exist_ok=True)
t = Target()
names = ['present-A-kmt.csv', 'present-B-cpu.csv', 'presentmon.csv', 'presentmon.stdout.txt', 'presentmon.stderr.txt',
         'error.txt', 'modules-A-kmt.json', 'modules-B-cpu.json', 'vkcube-A-kmt.stdout.txt', 'vkcube-A-kmt.stderr.txt',
         'vkcube-B-cpu.stdout.txt', 'vkcube-B-cpu.stderr.txt', 'before-health.txt', 'after-health.txt',
         'before-clock.txt', 'after-clock.txt', 'before-log-summary.txt', 'after-A-kmt-log-summary.txt',
         'after-B-cpu-log-summary.txt', 'after-log-summary.txt', 'before-log.txt', 'after-log.txt', 'logman-stop.txt',
         'A-kmt-0.png', 'A-kmt-1.png', 'B-cpu-0.png', 'B-cpu-1.png', 'end.png']
for n in names:
    remote = 'C:\\BC250\\m12\\wsi-kmt-003\\' + n.replace('/', '\\')
    local = r / n
    local.parent.mkdir(parents=True, exist_ok=True)
    try:
        t.pull(remote, str(local))
        print(n, local.stat().st_size)
    except Exception as e:
        if n != 'error.txt':
            print(n, 'FAILED', e)
        else:
            local.unlink(missing_ok=True)
for n in ['run-kmt-003.log', 'done-kmt-003.json']:
    try:
        t.pull('C:\\BC250\\m12\\wsi-kmt\\' + n, str(r / n))
        print(n, (r / n).stat().st_size)
    except Exception as e:
        print(n, 'FAILED', e)
raw = (r / 'run-kmt-003.log').read_bytes()
txt = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', 'replace')
(r / 'run-kmt-003.utf8.log').write_text(txt, encoding='utf-8')
print('---- run log ----')
print(txt[-6000:])
