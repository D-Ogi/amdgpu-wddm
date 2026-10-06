"""Extract the production observer and actual escape prefix through its branch."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
def function(s,sig):
    start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
s=(a.source/'dcn.c').read_text();parts=[function(s,'void DcnObserve(')]
s=(a.source/'display.c').read_text();parts.append(function(s,'static BOOLEAN WddmDiagnosticAllowed('));parts.append(function(s,'static BOOLEAN SoftwareReadEscape('))
start=s.index('NTSTATUS Bc250Escape(');end=s.index('    if (data->Command == BC250_ESCAPE_RUN_CLOCK)',start)
parts.append(s[start:end]+'    return STATUS_NOT_SUPPORTED;\n}\n')
f=Path(__file__).with_name('dcn_observe_test.c').read_text();f=f.replace('/* TIMING_SOURCE */',(a.source/'display_timing_snapshot.h').read_text());a.out.write_text(f.replace('/* ACTUAL_SOURCE */','\n'.join(parts)))
print('Extracted actual observer, timing reader, diagnostic whitelist and escape prefix')
