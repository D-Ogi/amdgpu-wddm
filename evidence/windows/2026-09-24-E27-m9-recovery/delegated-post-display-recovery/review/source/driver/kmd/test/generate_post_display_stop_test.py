"""Extract real WddmStop, ordinary StopDevice and post-display handover."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
parts=[]
for file,sigs in [('wddm.c',['void WddmStop(']),('pnp.c',['NTSTATUS Bc250StopDevice(', 'NTSTATUS Bc250StopDeviceAndReleasePostDisplayOwnership('])]:
    s=(a.source/file).read_text();parts.extend(function(s,sig) for sig in sigs)
f=Path(__file__).with_name('post_display_stop_test.c').read_text();a.out.write_text(f.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted actual WddmStop and both PnP stop callbacks')
