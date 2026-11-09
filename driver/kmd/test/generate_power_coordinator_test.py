from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
# BD-188 negative controls: the pre-fix behaviour, one per requirement.
p.add_argument('--old-display-gate',action='store_true',help='a failed display step returns before the engines are saved')
p.add_argument('--old-post-gate',action='store_true',help='a refused POST ownership at D0 keeps the engines suspended')
# BD-188 D-2 negative controls (docs/design/power-loss.md section 7).
p.add_argument('--forced-d1',action='store_true',help='the forced way down also for D1 without a sleep action')
p.add_argument('--witness-ignored',action='store_true',help='the restore after an unsaved way down ignores the witness')
p.add_argument('--failed-restore-active',action='store_true',help='a failed restore ends in ACTIVE')
p.add_argument('--lost-before-witness',action='store_true',help='the blocks take the power-lost state before the witness proves it')
a=p.parse_args()
s=a.source.read_text()
def function(sig):
 start=s.index(sig);return s[start:s.index('\n}\n',start)+3]
def mutate(text,old,new):
 if text.count(old)!=1:raise SystemExit('Mutation anchor missing or ambiguous: '+old)
 return text.replace(old,new)
body='\n'.join(function(sig) for sig in ['static void PowerWitnessArm(', 'static ULONG PowerWitnessTake(', 'static BOOLEAN PowerDownStepFailed(', 'static NTSTATUS PowerRestoreDisplay(', 'NTSTATUS GpuSetPowerRetained(', 'NTSTATUS PowerAdapterDown('])
if a.forced_d1:
 body=mutate(body,'BOOLEAN lossExpected=Bc250PowerLossExpected((unsigned)State,(unsigned)Action) ? TRUE : FALSE;','BOOLEAN lossExpected=TRUE;')
if a.witness_ignored:
 body=mutate(body,'if (PowerWitnessTake(Device,unsaved)!=BC250_POWER_WITNESS_LOST && unsaved) {','if ((PowerWitnessTake(Device,unsaved),0) && unsaved) {')
if a.failed_restore_active:
 body=mutate(body,'InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_FAILED);\nDone:','InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_ACTIVE);\nDone:')
if a.lost_before_witness:
 anchor='    if (PowerWitnessTake(Device,unsaved)!=BC250_POWER_WITNESS_LOST && unsaved) {'
 body=mutate(body,anchor,'    if (unsaved) {IhPowerLost(Device);GfxPowerLost(Device);PspPowerLost(Device);GartPowerLost(Device);}\n'+anchor)
if a.old_display_gate:
 body=mutate(body,'    return GpuSetPowerRetained(Device,State,Action);\n','    if (!NT_SUCCESS(display)) return display;\n    return GpuSetPowerRetained(Device,State,Action);\n')
if a.old_post_gate:
 body=mutate(body,'DxgkCbAcquirePostDisplayOwnership(Device->Dxgk.DeviceHandle,&post);\n','DxgkCbAcquirePostDisplayOwnership(Device->Dxgk.DeviceHandle,&post);\n    if (!NT_SUCCESS(status)) return status;\n')
a.out.write_text(Path(__file__).with_name('power_coordinator_test.c').read_text().replace('/* ACTUAL_SOURCE */',body))
