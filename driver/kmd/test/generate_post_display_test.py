"""Extract the actual bounded restore and bugcheck display callbacks."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
parts=[]
for file,sigs in [('dcn.c',['static NTSTATUS DcnFlipWriteSequence(', 'static NTSTATUS DcnReadScanoutPhysical(', 'static NTSTATUS DcnCheckPostSurface(', 'NTSTATUS DcnSetVisibility(', 'NTSTATUS DcnRestorePostDisplay(']),('display.c',['static BOOLEAN IsPostFormatSupported(', 'NTSTATUS Bc250SystemDisplayEnable(', 'void Bc250SystemDisplayWrite('])]:
    s=(a.source/file).read_text();parts.extend(function(s,sig) for sig in sigs)
f=Path(__file__).with_name('post_display_test.c').read_text();a.out.write_text(f.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted actual DCN restore and bugcheck display callbacks')
