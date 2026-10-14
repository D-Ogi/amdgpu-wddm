"""Copy the production gate and extract the effective-token check for host tests."""
import argparse
from pathlib import Path
import hashlib
import json
import re

p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
here = Path(__file__).resolve().parent
kmd = here.parent
a.out.mkdir(parents=True, exist_ok=True)
hashes = {}
for name, dest in [('lab_gate.c', 'lab_gate_actual.c'), ('lab_gate.h', 'lab_gate.h')]:
    raw = (kmd / name).read_bytes()
    (a.out / dest).write_bytes(raw)
    hashes[name] = hashlib.sha256(raw).hexdigest()
(a.out / 'bc250kmd.h').write_text('/* Host types are supplied by lab_gate_test.c. */\n', encoding='utf-8')
display = (kmd / 'display.c').read_text(encoding='utf-8')
match = re.search(r'(?m)^static BOOLEAN WaveCallerIsAdmin\(void\)\s*\{[^{}]*\}', display)
if match is None:
    raise SystemExit('effective-token predicate shape changed')
template = (here / 'lab_gate_test.c').read_text(encoding='utf-8')
assert template.count('/* ACTUAL_ADMIN */') == 1
(a.out / 'lab_gate_test_actual.c').write_text(template.replace('/* ACTUAL_ADMIN */', match[0]), encoding='utf-8')
hashes['display.c'] = hashlib.sha256((kmd / 'display.c').read_bytes()).hexdigest()
(a.out / 'source-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n', encoding='utf-8')
