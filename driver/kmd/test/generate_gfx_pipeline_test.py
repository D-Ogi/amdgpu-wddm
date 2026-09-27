from pathlib import Path
import sys
repo=Path(__file__).resolve().parents[3]
source=Path(sys.argv[2]) if len(sys.argv)>2 else repo
s=(source/'driver/kmd/wddm.c').read_text()
assert 'if (WddmSubmitPresentHardware(device,wddm,context,' in s, 'BGP1 must use tested bounded admission'
a=s.index('static void WddmGfxHeadLocked(')
b=s.index('// ---- ADR 0008 stage D:',a)
Path(sys.argv[1]).write_text(s[a:b])

mesa=repo.parent/'scratch/m12/mesa-current-src/src/amd/vulkan/winsys/wddm2/radv_wddm2_cs.c'
s=mesa.read_text()
a=s.index('      /* Independent of application signals:')
b=s.index('   } else if (submit->cs_count > 0)',a)
Path(sys.argv[1]).with_name('gather_actual.inc').write_text(s[a:b])
