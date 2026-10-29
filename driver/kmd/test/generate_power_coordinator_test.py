from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
# BD-188 negative controls: the pre-fix behaviour, one per requirement.
p.add_argument('--old-display-gate',action='store_true',help='a failed display step returns before the engines are saved')
p.add_argument('--old-post-gate',action='store_true',help='a refused POST ownership at D0 keeps the engines suspended')
a=p.parse_args()
s=a.source.read_text()
def function(sig):
 start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
def mutate(text,old,new):
 if text.count(old)!=1:raise SystemExit('Mutation anchor missing or ambiguous: '+old)
 return text.replace(old,new)
body='\n'.join(function(sig) for sig in ['static NTSTATUS PowerRestoreDisplay(', 'NTSTATUS GpuSetPowerRetained(', 'NTSTATUS PowerAdapterDown('])
if a.old_display_gate:
 body=mutate(body,'    return GpuSetPowerRetained(Device,State,Action);\n','    if (!NT_SUCCESS(display)) return display;\n    return GpuSetPowerRetained(Device,State,Action);\n')
if a.old_post_gate:
 body=mutate(body,'DxgkCbAcquirePostDisplayOwnership(Device->Dxgk.DeviceHandle,&post);\n','DxgkCbAcquirePostDisplayOwnership(Device->Dxgk.DeviceHandle,&post);\n    if (!NT_SUCCESS(status)) return status;\n')
a.out.write_text(Path(__file__).with_name('power_coordinator_test.c').read_text().replace('/* ACTUAL_SOURCE */',body))
