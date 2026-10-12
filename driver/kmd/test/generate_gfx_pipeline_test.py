from pathlib import Path
import re
import sys
repo=Path(__file__).resolve().parents[3]
source=Path(sys.argv[2]) if len(sys.argv)>2 else repo
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
s=(source/'driver/kmd/wddm.c').read_text()
assert 'if (WddmSubmitPresentHardware(device,wddm,context,' in s, 'BGP1 must use tested bounded admission'
a=s.index('static void WddmGfxHeadLocked(')
b=s.index('// ---- ADR 0008 stage D:',a)
# KMD196: the held submission's bucket count sizes a BC250_WDDM field, so it goes ahead of the harness's struct;
# WddmStopping (above the region) and gfx.c's GfxRetireSignal are production code the region calls.
# BD-114: BC250_WDDM_HOLD_DEADLINE_MS joins it. The held submission's bound stayed at 500 ms when the submit
# watchdog's budget became TdrDelay-derived, because it bounds a CPU wait on a dxgkrnl worker thread and not
# the GPU's execution. The test asserts that bound, so it must read wddm.c's number and never a copy of it.
defines=[]
for name in ('BC250_WDDM_HOLD_BUCKETS','BC250_WDDM_HOLD_DEADLINE_MS'):
    found=re.findall(r'^#define '+name+r'\b.*$',s,re.M)
    assert len(found)==1, name+' must be defined once in wddm.c'
    defines.append(found[0])
Path(sys.argv[1]).with_name('gfx_pipeline_defines.inc').write_text('\n'.join(defines)+'\n')
gfx=(source/'driver/kmd/gfx.c').read_text()
Path(sys.argv[1]).write_text(function(s,'static BOOLEAN WddmStopping(')+function(gfx,'void GfxRetireSignal(')+s[a:b])

# The RADV tree holding the WDDM winsys: argv[3], the runner's -MesaSource; none with -KmdOnly. The old default, a
# path next to the repository, named nothing in a worktree.
if len(sys.argv)<4 or not sys.argv[3]: sys.exit(0)
mesa=Path(sys.argv[3])/'src/amd/vulkan/winsys/wddm2/radv_wddm2_cs.c'
s=mesa.read_text()
a=s.index('      /* Independent of application signals:')
b=s.index('   } else if (submit->cs_count > 0)',a)
Path(sys.argv[1]).with_name('gather_actual.inc').write_text(s[a:b])
