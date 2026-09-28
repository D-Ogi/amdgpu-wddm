"""cpucopy-002: witness candidate only (63AF86CB), both sizes, derived from run-cpucopy-001.ps1."""
from pathlib import Path

src = Path('P:/BC-250/scratch/witcher3/dx12')
cand = '63AF86CB862570709956E0376FDBDCB434DB3573E6A99412ED60311CFFAE48DA'
t = (src / 'run-cpucopy-001.ps1').read_text(encoding='utf-8')
lines = t.split('\n')
assert lines[0].startswith('# cpucopy-001:')
header = [
    '# cpucopy-002: copy witness on the Vulkan WSI GDI present path (E47 follow-up, message 056). Candidate 63AF86CB',
    '# (fork amdgpu-wddm/radv-wddm2-gdi-immediate 6358c3a9 = 50E99A84 plus the blit-buffer memory-type witness in the',
    '# present-log header and a copy_cycles column = QueryThreadCycleTime around the copy) swapped in place of the',
    '# registered ICD (E43 method, baseline copy and hash-checked restore in finally), vkcube 900 frames at 640x480 and',
    '# 1920x1200 on the CPU present path with the per-present timing log. No PresentMon, no ETW, no UMD/DWM/KMD change.',
]
i = next(k for k, l in enumerate(lines) if l.startswith('$ErrorActionPreference'))
t = '\n'.join(header + lines[i:])
t = t.replace("$out = 'C:\\BC250\\m12\\cpucopy-001'", "$out = 'C:\\BC250\\m12\\cpucopy-002'")
old_stages = t[t.index('$stages = @('):t.index(')\n$sizes')]
new_stages = ("$stages = @(\n"
              "  @{ name = 'A-witness'; path = 'C:\\BC250\\m12\\icd-candidates\\vulkan_radeon.63AF86CB.dll'; hash = '" + cand + "' }\n")
t = t.replace(old_stages, new_stages)
t = t.replace("Say \"cpucopy-001 $tag : vkcube $frames frames on the CPU present path ($(if ($s.name -eq 'A-cached') { 'host-cached candidate' } else { 'write-combined control' }))\"",
              "Say \"cpucopy-002 $tag : vkcube $frames frames on the CPU present path (copy witness candidate 63AF86CB)\"")
t = t.replace('Say "cpucopy-001 finished (exit $code)"', 'Say "cpucopy-002 finished (exit $code)"')
assert 'cpucopy-001' not in t and '6661C2D2' not in t and 'A-cached' not in t, [l for l in t.split('\n') if 'cpucopy-001' in l or '6661C2D2' in l or 'A-cached' in l]
(src / 'run-cpucopy-002.ps1').write_text(t, encoding='utf-8', newline='\n')
for name in ['launch-cpucopy-{}.ps1', 'worker-cpucopy-{}.ps1', 'status-cpucopy-{}.ps1', 'pull-cpucopy-{}.py']:
    s = (src / name.format('001')).read_text(encoding='utf-8')
    s = s.replace('cpucopy-001', 'cpucopy-002').replace('vulkan_radeon.50E99A84.dll', 'vulkan_radeon.63AF86CB.dll')
    s = s.replace('50E99A84C1FEA6295E6D54AB3E516498462E84FB3651A24A52FE5F2E942C151F', cand)
    s = s.replace("for s in ['A-cached', 'B-wc']:", "for s in ['A-witness']:")
    assert '50E99A84' not in s and 'cpucopy-001' not in s
    (src / name.format('002')).write_text(s, encoding='utf-8', newline='\n')
print('wrote cpucopy-002')
