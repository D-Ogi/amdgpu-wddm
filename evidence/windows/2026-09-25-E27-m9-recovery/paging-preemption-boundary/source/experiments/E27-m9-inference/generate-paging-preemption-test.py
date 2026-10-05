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
 LONG PagingQueueBorrowed,''')
code=code.replace(' (void)w;(void)node;check(lockHeld,', ' w->CompletionPending[node]=1;w->SubmittedFence[node]=(LONG)fence;check(lockHeld,')
extra=(here/'paging-preemption-test.c').read_text()
prefix,suffix=extra.split('/* ACTUAL_FUNCTIONS */')
code=code.replace('static void legacy(void)',prefix+'\n'+extract('static void WddmPreemptFence(')+'\n'+extract('static void WddmReportDpcRoutine(')+'\n'+suffix+'\nstatic void legacy(void)',1)
code=code.replace(' legacy();borrowed();retention();built_records();',' legacy();borrowed();retention();built_records();preemption();')
if len(sys.argv)>3:
    assert sys.argv[3]=='--old-drain'
    old='                   !wddm->PreemptionPending[BC250_WDDM_NODE_COPY] &&\n'
    assert code.count(old)==1
    code=code.replace(old,'')
out.write_text(code)
