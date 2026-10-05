"""Extract the actual DCN observation helpers and hardware GetScanLine DDI."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser()
p.add_argument('--dcn',type=Path,default=Path(__file__).resolve().parents[1]/'dcn.c')
p.add_argument('--wddm',type=Path,default=Path(__file__).resolve().parents[1]/'wddm.c')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
def function(s, signature):
    start=s.index(signature)
    return s[start:s.index('\n}\n',start)+3]
dcn=a.dcn.read_text();wddm=a.wddm.read_text()
body='\n'.join(function(dcn,x) for x in (
    'static NTSTATUS DcnReadScanoutPhysical(', 'NTSTATUS DcnReadScanoutAddress(', 'BOOLEAN DcnFlipPending(', 'NTSTATUS DcnReadScanLine('))
body+='\n'+function(wddm,'static NTSTATUS Bc250WddmGetScanLine(')
fixture=Path(__file__).with_name('dcn_observation_test.c').read_text()
assert fixture.count('/* ACTUAL_SOURCE */')==1
a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */',body))
print('Extracted DCN observation helpers and hardware GetScanLine DDI')
