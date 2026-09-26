"""Extract actual DDI, completion snapshot and hardware-vsync publication."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'wddm.c')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args(); s=a.source.read_text()
def function(signature):
    start=s.index(signature)
    return s[start:s.index('\n}\n',start)+3]
body='\n'.join(function(x) for x in (
    'static BOOLEAN WddmReadCompletedPrimary(',
    'static NTSTATUS Bc250WddmSetVidPnSourceAddress(',
    'void WddmDcnVsync('))
fixture=Path(__file__).with_name('vidpn_flip_test.c').read_text()
assert fixture.count('/* ACTUAL_SOURCE */')==1
a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */',body))
print('Extracted publication and vsync functions from',a.source)
