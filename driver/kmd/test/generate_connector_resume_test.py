"""BD-188: extract ConnectorResumeDisplay from connector.c for a CPU-only test."""
from pathlib import Path
import argparse
import re

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'connector.c')
p.add_argument('--out', type=Path, required=True)
# The negative control: the pre-fix resume, in which a sink that could not be restored failed the D0.
p.add_argument('--refusal-fails-d0', action='store_true')
a = p.parse_args()
s = a.source.read_text(encoding='utf-8')
found = list(re.finditer(r'(?m)^NTSTATUS ConnectorResumeDisplay\(BC250_DEVICE\* Device\)\n\{', s))
if len(found) != 1:
    raise SystemExit(f'Expected one ConnectorResumeDisplay, found {len(found)}')
start = found[0].start()
body = s[start:s.index('\n}\n', start) + 3]
if a.refusal_fails_d0:
    needle = '    ConnectorLinkResumeChanged(Device);\n'
    if body.count(needle) != 1:
        raise SystemExit('Resume fallback mutation anchor changed')
    # Before the fix an unreadable or changed sink took the fallback; a same sink that failed, and a missing
    # snapshot, returned their failure.
    body = body.replace(needle, '    if (!NT_SUCCESS(status) && (same || !next)) goto done;\n' + needle)
template = Path(__file__).with_name('connector_resume_test.c').read_text(encoding='utf-8')
if template.count('/* ACTUAL_SOURCE */') != 1:
    raise SystemExit('Production source marker changed')
a.out.write_text(template.replace('/* ACTUAL_SOURCE */', body), encoding='utf-8')
