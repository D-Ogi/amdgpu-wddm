"""Generate the wsi-kmt-003 scripts from 002: new candidate (SubRectCnt fix), same procedure.

usage: python make003.py <NEWHASH64> <fork-commit>
"""
import pathlib
import sys

d = pathlib.Path(r'P:\bc-250\scratch\wsi-kmt-2026-09-27')
new_hash = sys.argv[1].upper()
commit = sys.argv[2]
assert len(new_hash) == 64
short = new_hash[:8]
old_hash = 'B9F4C95B22AB80F671FC94A678A70F3A960DB05F12D6615C0A8422FB8ED693F9'


def rep(s, pairs):
    for old, new in pairs:
        assert s.count(old) == 1, old[:60]
        s = s.replace(old, new)
    return s


run = (d / 'run-kmt-002.ps1').read_text(encoding='utf-8')
run = run.replace('wsi-kmt-002', 'wsi-kmt-003').replace('kmt-002', 'kmt-003')
run = run.replace(old_hash, new_hash).replace('vulkan_radeon.B9F4C95B.dll', f'vulkan_radeon.{short}.dll')
run = rep(run, [
    ("""# wsi-kmt-003: plumbing control (H1) of the Vulkan WSI KMT present path on candidate ICD B9F4C95B
# (fork amdgpu-wddm/radv-wddm2-wsi-kmt 1ae7b6af = present-log 0f9811a5 + KMT present). Run 001 showed that""",
     f"""# wsi-kmt-003: plumbing control (H1) of the Vulkan WSI KMT present path on candidate ICD {short}
# (fork amdgpu-wddm/radv-wddm2-wsi-kmt {commit}: B9F4C95B plus SubRectCnt >= 1, the documented Blt requirement that
# run 002 measured as STATUS_INVALID_PARAMETER for every present). Run 001 showed that"""),
])
run = run.replace('candidate ICD B9F4C95B', f'candidate ICD {short}')
assert run.count('B9F4C95B') == 1, run.count('B9F4C95B')   # the header's history line only
(d / 'run-kmt-003.ps1').write_text(run, encoding='utf-8', newline='\r\n')
for n in ('worker', 'launch', 'status', 'cleanup'):
    s = (d / f'{n}-kmt-002.ps1').read_text(encoding='utf-8').replace('wsi-kmt-002', 'wsi-kmt-003').replace('kmt-002', 'kmt-003')
    s = s.replace(old_hash, new_hash).replace('vulkan_radeon.B9F4C95B.dll', f'vulkan_radeon.{short}.dll').replace('candidate ICD B9F4C95B', f'candidate ICD {short}')
    (d / f'{n}-kmt-003.ps1').write_text(s, encoding='utf-8', newline='\r\n')
p = (d / 'pull-002.py').read_text(encoding='utf-8').replace('wsi-kmt-002', 'wsi-kmt-003').replace('kmt-002', 'kmt-003').replace('run002', 'run003')
(d / 'pull-003.py').write_text(p, encoding='utf-8', newline='\n')
print('written for', short)
