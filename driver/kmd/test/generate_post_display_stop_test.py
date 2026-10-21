"""Extract real WddmStop, board unbind, StopDevice and post-display handover."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True)
# BD-090 negative control: the release of 8869edf5, which never gave a link that was off a target again.
p.add_argument('--skip-handoff',action='store_true');a=p.parse_args()
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
parts=[]
for file,sigs in [('wddm.c',['void WddmStop(']),('board_provider.c',['void BoardProviderUnbind(']),('pnp.c',['NTSTATUS Bc250StopDevice(', 'NTSTATUS Bc250StopDeviceAndReleasePostDisplayOwnership('])]:
    s=(a.source/file).read_text();parts.extend(function(s,sig) for sig in sigs)
if a.skip_handoff:
    needle='(void)ConnectorLinkHandoff(device,&handoff);'
    if sum(x.count(needle) for x in parts)!=1:raise SystemExit('Handoff mutation anchor changed')
    parts=[x.replace(needle,'RtlZeroMemory(&handoff,sizeof(handoff));') for x in parts]
f=Path(__file__).with_name('post_display_stop_test.c').read_text();a.out.write_text(f.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted actual WddmStop, board unbind and both PnP stop callbacks'+('; handoff skipped' if a.skip_handoff else ''))
