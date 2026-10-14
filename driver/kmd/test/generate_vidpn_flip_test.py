"""Extract the actual flip path of wddm.c into the vidpn_flip_test.c fixture (BD-170 r6, docs/design/vidpn-flip.md
section 7.2): the report routine and WddmReport, the completion snapshot, the VSync switch's history, the hardware
vsync's report, the summary's record copy, the DMA-flip refusal, the whole flip state machine block (the flip DDI, its
transaction, the writers' sections, E5 and its DPC, the destroy restore), DestroyAllocation and ControlInterrupt.
Each anchor must occur exactly once, so that a renamed or duplicated function stops the extraction instead of testing
something else. The output has LF line ends, which run_vidpn_flip.ps1's mutations rely on."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'wddm.c')
p.add_argument('--out',type=Path,required=True)
a=p.parse_args(); s=a.source.read_text()
def once(text):
    count=s.count(text)
    if count!=1: raise SystemExit(f'anchor occurs {count} times in {a.source}, not once: {text!r}')
    return s.index(text)
def function(signature):
    start=once(signature)
    return s[start:s.index('\n}\n',start)+3]
def between(first,last):
    start=once(first)
    return s[start:once(last)]
body='\n'.join([
    between('typedef struct _BC250_WDDM_NOTIFY {','// The pairing counters'),
    *(function(x) for x in (
        'static BOOLEAN WddmReadCompletedPrimary(',
        'static void WddmVSyncControlNote(',
        'void WddmDcnVsync(',
        'static ULONG WddmImmediateRecordCopy(',
        'static NTSTATUS WddmPresentDmaFlip(')),
    between('// ---- BD-170 r6: the flip state machine','// ---- interrupts'),
    *(function(x) for x in (
        'static NTSTATUS Bc250WddmDestroyAllocation(',
        'static NTSTATUS Bc250WddmControlInterrupt('))])
fixture=Path(__file__).with_name('vidpn_flip_test.c').read_text()
if fixture.count('/* ACTUAL_SOURCE */')!=1: raise SystemExit('the fixture has no single /* ACTUAL_SOURCE */ marker')
a.out.write_text(fixture.replace('/* ACTUAL_SOURCE */',body),newline='\n')
print('Extracted the flip path from',a.source)
