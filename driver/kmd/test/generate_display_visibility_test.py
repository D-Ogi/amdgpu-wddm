"""Extract actual visibility, interrupt-enable/ISR and DDI integration."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
s=(a.source/'dcn.c').read_text();start=s.index('typedef struct _BC250_DCN_VSYNC_CHANGE');end=s.index('} BC250_DCN_VSYNC_CHANGE;',start)+len('} BC250_DCN_VSYNC_CHANGE;');parts=[s[start:end]]
for file,sigs in [('dcn.c',['NTSTATUS DcnSetVisibility(', 'static ULONG DcnVsyncAckValue(', 'static BOOLEAN DcnVsyncEnableSynchronized(', 'NTSTATUS DcnVsyncEnable(', 'BOOLEAN DcnVsyncInterrupt(']),('wddm.c',['static void WddmVSyncArm(', 'void WddmSourceVisibility(']),('display.c',['static void DisplayRetainVisibility(', 'static NTSTATUS SetVisibilityCore(', 'NTSTATUS Bc250SetVidPnSourceVisibility(', 'static void DisplayRetainCommitPower('])]:
    s=(a.source/file).read_text();parts.extend(function(s,sig) for sig in sigs)
h=(a.source/'bc250kmd.h').read_text();begin=h.index('#define BC250_VISIBILITY_HISTORY_COUNT');end=h.index('typedef struct _BC250_DEVICE',begin)
f=Path(__file__).with_name('display_visibility_test.c').read_text().replace('/* VISIBILITY_EVENT_TYPE */',h[begin:end]);a.out.write_text(f.replace('/* ACTUAL_SOURCE */','#define StartHealthEnter(d) ((void)(d))\n#define StartHealthLeave(d) ((void)(d))\n#define StartHealthVisibilityLocked(d,v) ((void)(d))\n'+'\n'.join(parts)))
print('Extracted actual visibility, synchronized enable/ISR and visibility DDI')
