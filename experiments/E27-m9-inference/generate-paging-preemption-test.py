"""Actual queue/report functions, with scheduler-controlled same-fence replay."""
from pathlib import Path
import subprocess
import sys
repo=Path(sys.argv[1]).resolve()
out=Path(sys.argv[2]).resolve()
here=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(here/'generate-paging-queue-test.py'),str(repo),str(out)],check=True)
code=out.read_text()
source=(repo/'driver/kmd/wddm.c').read_text()
def extract(marker):
    a=source.index(marker); b=source.index('{',a); depth=1; i=b+1
    while depth:
        if source[i]=='{': depth+=1
        elif source[i]=='}': depth-=1
        i+=1
    return source[a:i]
code=code.replace(' LONG PagingQueueBorrowed,', ''' LONG SubmittedNode[2],PreemptionNode[2],CompletionPending[2],SubmittedFence[2],ActiveSubmissions[2],LastReportedFence[2],PreemptionFence[2];
 LONG LastCompletedFence;
 BOOLEAN HwPending,ReportActive,ReportAgain,RejectedPending[2],RefusalPending[2],LastReportedValid[2];
 UINT RejectedFence[2]; KDPC* ReportDpc;
 BOOLEAN NotifyDpcInReport;
 LONG CompletionRetries[2],CompletionsDropped[2],PreemptionReportsLost;
 LONG PagingQueueBorrowed,''')
code=code.replace(' (void)w;(void)node;check(lockHeld,', ' w->CompletionPending[node]=1;w->SubmittedFence[node]=(LONG)fence;check(lockHeld,')
# The report pass reaches two more things than the queue harness declares: the retry bound of an undelivered
# completion report (0.7.210.1) and the device's dxgkrnl callbacks, which it uses on the stop path only.
code=code.replace('typedef struct { void* Wddm; } BC250_DEVICE;',
                  '''#define BC250_WDDM_REPORT_RETRY_MAX 4L
typedef struct { void (*DxgkCbQueueDpc)(void*); void* DeviceHandle; } BC250_DXGK_STUB;
typedef struct { void* Wddm; BC250_DXGK_STUB Dxgk; } BC250_DEVICE;''')
extra=(here/'paging-preemption-test.c').read_text()
prefix,suffix=extra.split('/* ACTUAL_FUNCTIONS */')
# KMD172 (2026-09-29) split the report pass into WddmReportDpcPublish and left WddmReportDpcRoutine as the progress
# wrapper around it. Both are extracted, or the generated file calls a function that is not in it. This harness is
# not in tools/quality/quick.ps1, so nothing else would say so.
code=code.replace('static void legacy(void)',prefix+'\n'+extract('static void WddmPreemptFence(')+'\n'+extract('static void WddmReportDpcPublish(')+'\n'+extract('static void WddmReportDpcRoutine(')+'\n'+suffix+'\nstatic void legacy(void)',1)
code=code.replace(' legacy();borrowed();retention();built_records();',' legacy();borrowed();retention();built_records();preemption();')
if len(sys.argv)>3:
    assert sys.argv[3]=='--old-drain'
    old='                   !wddm->PreemptionPending[BC250_WDDM_NODE_COPY] &&\n'
    assert code.count(old)==1
    code=code.replace(old,'')
out.write_text(code)
