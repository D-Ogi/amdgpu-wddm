from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
s=a.source.read_text()
def function(sig):
 start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
body='\n'.join(function(sig) for sig in ['static int RunEngineStage(', 'BOOLEAN GfxPowerIsSuspended(', 'static void GfxResetRetainedRing(', 'static NTSTATUS GfxPowerRetainedLocked(', 'NTSTATUS GfxSetPowerRetained('])
a.out.write_text(Path(__file__).with_name('gfx_retained_test.c').read_text().replace('/* ACTUAL_SOURCE */',body))
