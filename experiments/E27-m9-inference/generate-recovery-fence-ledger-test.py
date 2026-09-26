from pathlib import Path
import subprocess,sys
repo=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve();here=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(here/'generate-paging-preemption-test.py'),str(repo),str(out)],check=True)
code=out.read_text();source=(repo/'driver/kmd/wddm.c').read_text()
def extract(marker):
 a=source.index(marker);b=source.index('{',a);depth=1;i=b+1
 while depth:
  if source[i]=='{':depth+=1
  elif source[i]=='}':depth-=1
  i+=1
 return source[a:i]
code=code.replace(' LONG LastCompletedFence;',' UINT NodeCount,HwNode,HwFence,DeferredFence; ULONG HwSeq; ULONGLONG HwEpoch;\n BOOLEAN DeferredValid; int SubmitTimer,SubmitDpc; LONG HwCompleted,HwSubmitted,HwRefused;\n LONG LastCompletedFence;')
fixture=(here/'recovery-fence-ledger-test.c').read_text();prefix,suffix=fixture.split('/* ACTUAL_FUNCTIONS */')
functions=['static BOOLEAN WddmSnapshotFenceLedgerLocked(', 'void WddmGpuFence(', 'static BOOLEAN WddmSubmitHardware(', 'static NTSTATUS Bc250WddmSubmitCommand(', 'static NTSTATUS Bc250WddmSubmitCommandVirtual(']
code=code.replace('int main(void) {',prefix+'\n'+'\n'.join(extract(f) for f in functions)+'\n'+suffix+'\nint main(void) {')
code=code.replace('built_records();preemption();','built_records();preemption();ledger_tests();')
if len(sys.argv)>3:
 mode=sys.argv[3]
 if mode=='--accept-rejected':code=code.replace('if (NT_SUCCESS(status))\n            WddmRecordFenceLedgerLocked','if (TRUE)\n            WddmRecordFenceLedgerLocked')
 elif mode=='--hardware-is-reported':code=code.replace('wddm->HwEpoch,wddm->HwFence,TRUE','wddm->HwEpoch,wddm->DeferredValid ? wddm->DeferredFence : wddm->HwFence,TRUE')
 elif mode=='--ignore-epoch':code=code.replace('if (Epoch!=ledger->Epoch) return;','(void)Epoch;')
 else:raise ValueError(mode)
out.write_text(code)
