"""Generate the wsi-kmt-<NEW> scripts from wsi-kmt-<OLD>: new candidate hash, same procedure.

usage: python makenext.py <old3> <new3> <OLDHASH8> <NEWHASH64> <fork-commit> "<one-line what changed>"
"""
import pathlib
import sys

d = pathlib.Path(r'P:\bc-250\scratch\wsi-kmt-2026-09-27')
old, new, old8, new_hash, commit, what = sys.argv[1:7]
new_hash = new_hash.upper()
assert len(new_hash) == 64 and len(old8) == 8
new8 = new_hash[:8]

run = (d / f'run-kmt-{old}.ps1').read_text(encoding='utf-8')
old_full = [l for l in run.splitlines() if l.startswith('$candidateHash = ')][0].split("'")[1]
assert old_full.startswith(old8), old_full


def swap(s):
    return (s.replace(f'wsi-kmt-{old}', f'wsi-kmt-{new}').replace(f'kmt-{old}', f'kmt-{new}')
             .replace(old_full, new_hash).replace(f'vulkan_radeon.{old8}.dll', f'vulkan_radeon.{new8}.dll')
             .replace(f'candidate ICD {old8}', f'candidate ICD {new8}'))


run = swap(run)
lines = run.split('\n')
lines.insert(1, f'# {new}: {what} (fork {commit}).')
(d / f'run-kmt-{new}.ps1').write_text('\n'.join(lines), encoding='utf-8', newline='\r\n')
for n in ('worker', 'launch', 'status', 'cleanup'):
    (d / f'{n}-kmt-{new}.ps1').write_text(swap((d / f'{n}-kmt-{old}.ps1').read_text(encoding='utf-8')),
                                           encoding='utf-8', newline='\r\n')
p = swap((d / f'pull-{old}.py').read_text(encoding='utf-8')).replace(f'run{old}', f'run{new}')
(d / f'pull-{new}.py').write_text(p, encoding='utf-8', newline='\n')
print('written', new, new8)
