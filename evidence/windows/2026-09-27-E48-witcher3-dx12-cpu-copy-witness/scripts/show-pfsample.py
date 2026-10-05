import sys
from pathlib import Path

raw = Path(sys.argv[1]).read_bytes()
txt = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', 'replace')
for l in txt.splitlines():
    l = l.replace('\\Process(witcher3)\\', 'w3.').replace('\\Process(dwm)\\', 'dwm.').replace('\\Memory\\', 'mem.')
    l = l.replace('Page Faults/sec', 'pf/s').replace('Available MBytes', 'availMB').replace('Pages Input/sec', 'pgin/s')
    l = l.replace('Working Set - Private', 'wsPriv').replace('Working Set', 'ws')
    sys.stdout.buffer.write((l[:230] + '\n').encode('ascii', 'replace'))
