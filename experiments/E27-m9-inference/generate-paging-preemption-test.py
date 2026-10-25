"""Actual queue/report functions, with scheduler-controlled same-fence replay."""
from pathlib import Path
import subprocess
import sys
repo=Path(sys.argv[1]).resolve()
out=Path(sys.argv[2]).resolve()
here=Path(__file__).resolve().parent
# -E, so that PYTHONOPTIMIZE in the environment cannot strip the child generator's own guards either.
subprocess.run([sys.executable,'-E',str(here/'generate-paging-queue-test.py'),str(repo),str(out)],check=True)
code=out.read_text()
source=(repo/'driver/kmd/wddm.c').read_text()
# BD-085: every substitution below is a text anchor in generated code or in wddm.c, and a plain str.replace that
# matches nothing is silent. That silence is how this harness rotted twice: KMD172 (6420bb68) renamed the report
# pass and e2e35ed4 gave it the hang-recovery node state, and the generator went on emitting a file that did
# not compile
# instead of saying which anchor had moved. sub() states the expected hit count and fails on the anchor.
# These guards raise SystemExit and never assert: python -O and PYTHONOPTIMIZE strip an assert, and a guard
# that an environment variable can switch off is the same silence under another name (BD-085 review finding 4).
def fail(message):
    raise SystemExit('generate-paging-preemption-test: '+message)
def sub(text,old,new,count=1):
    found=text.count(old)
    if found!=count: fail('anchor moved (%d of %d): %.80r' % (found,count,old))
    return text.replace(old,new)
def extract(marker):
    if source.count(marker)!=1: fail('wddm.c no longer has exactly one %r' % marker)
    a=source.index(marker); b=source.index('{',a); depth=1; i=b+1
    while depth:
        if source[i]=='{': depth+=1
        elif source[i]=='}': depth-=1
        i+=1
    return source[a:i]
code=sub(code,' LONG PagingQueueBorrowed,', ''' LONG SubmittedNode[2],PreemptionNode[2],CompletionPending[2],SubmittedFence[2],ActiveSubmissions[2],LastReportedFence[2],PreemptionFence[2];
 LONG LastCompletedFence,LastSubmittedFence[2];
 BOOLEAN HwPending,ComputeHwPending,ReportActive,ReportAgain,RejectedPending[2],RefusalPending[2],LastReportedValid[2],LastSubmittedValid[2];
 UINT RejectedFence[2]; KDPC* ReportDpc;
 BOOLEAN NotifyDpcInReport;
 LONG CompletionRetries[2],CompletionsDropped[2],PreemptionReportsLost;
 LONG PagingQueueBorrowed,''')
code=sub(code,' struct {BOOLEAN ResetActive;} Recovery[2];',
         ' BC250_HANG_NODE_STATE Recovery[BC250_WDDM_NODE_COUNT_MAX];')
# e2e35ed4 (2026-10-10, BD-114) gave the report pass the M15.12 hang-recovery node state: the publication gate
# (Bc250HangReportBegin/End), the reset gate and the scheduler boundary. hang_recovery.h says of itself that it
# holds the pure decisions with no kernel types and no I/O, so the harness includes it exactly as it includes
# paging_private.h, paging_drain.h, progress.h and ring_gap.h, and runs the real gate rather than a stub of it.
# Its own includes (vmid_pool.h, bc250_fence_order.h) are pure too; bc250_fence_order.h is under
# driver/shim/include, which is why the runner adds that include path.
code=sub(code,'#include "ring_gap.h"','#include "ring_gap.h"\n#include "hang_recovery.h"')
# The queue harness's WddmRecordCompletionLocked stub stands in for wddm.c:1011, and keeps the queue harness's
# own check that the caller holds the lock, which the real body does not make. The report pass reads what that
# function records, so the stub records the same four things in the same order: the hang-recovery observation,
# the node, the fence and the pending flag (wddm.c:1013-1016).
# Nothing used to assert that the real body still records those four, so the stub could drift from it in
# silence (BD-085 review finding 5). This compares the two, statement by statement.
mirror=['Bc250HangObserveCompleted(&Wddm->Recovery[NodeOrdinal], FenceId);',
        'Wddm->SubmittedNode[NodeOrdinal] = (LONG)NodeOrdinal;',
        'Wddm->SubmittedFence[NodeOrdinal] = (LONG)FenceId;',
        'Wddm->CompletionPending[NodeOrdinal] = 1;']
record=extract('static void WddmRecordCompletionLocked(')
for statement in mirror:
    if record.count(statement)!=1: fail('wddm.c WddmRecordCompletionLocked no longer records %r' % statement)
if record.count(';')!=len(mirror):
    fail('wddm.c WddmRecordCompletionLocked has %d statements, not the %d the stub below mirrors'
         % (record.count(';'),len(mirror)))
code=sub(code,' (void)w;(void)node;check(lockHeld,', ' Bc250HangObserveCompleted(&w->Recovery[node],fence);'
         'w->SubmittedNode[node]=(LONG)node;w->SubmittedFence[node]=(LONG)fence;'
         'w->CompletionPending[node]=1;check(lockHeld,')
# The report pass reaches two more things than the queue harness declares: the retry bound of an undelivered
# completion report (0.7.210.1) and the device's dxgkrnl callbacks, which it uses on the stop path only.
code=sub(code,'typedef struct { void* Wddm; } BC250_DEVICE;',
                  '''#define BC250_WDDM_REPORT_RETRY_MAX 4L
typedef struct { void (*DxgkCbQueueDpc)(void*); void* DeviceHandle; } BC250_DXGK_STUB;
typedef struct { void* Wddm; BC250_DXGK_STUB Dxgk; } BC250_DEVICE;''')
extra=(here/'paging-preemption-test.c').read_text()
prefix,suffix=extra.split('/* ACTUAL_FUNCTIONS */')
# KMD172 (2026-09-29) split the report pass into WddmReportDpcPublish and left WddmReportDpcRoutine as the progress
# wrapper around it. Both are extracted, or the generated file calls a function that is not in it. Until BD-085
# this harness was in no gate, so nothing said so; driver/kmd/test/run_paging_preemption.ps1 now runs it from
# tools/quality/quick.ps1.
# F-q44-mem-4: the submit DDI's wrapper too, where a STATUS_INVALID_PARAMETER of its body is recorded as the rejected
# fence that the report pass then completes. The body itself is the harness's stand-in.
submit=(extract('static void WddmNoteSubmittedLocked(')+'\n'+extract('static BOOLEAN WddmBeginSubmissionLocked(')+'\n'+
        extract('static NTSTATUS Bc250WddmSubmitCommandVirtual(')+'\n')
# The compute node (docs/design/compute-node.md) gave nodes 0 and 2 one ring-pending answer, WddmRingPendingLocked,
# which the preemption and the report pass read for every node except node 1. The harness has two nodes, so node 2
# is out of its range, but the answer is the actual one.
ring='#define BC250_WDDM_NODE_COMPUTE 2u\n'+extract('static BOOLEAN WddmRingPendingLocked(')+'\n'
code=sub(code,'static void legacy(void)',prefix+'\n'+ring+extract('static void WddmPreemptFence(')+'\n'+extract('static void WddmReportDpcPublish(')+'\n'+extract('static void WddmReportDpcRoutine(')+'\n'+submit+suffix+'\nstatic void legacy(void)')
code=sub(code,' legacy();borrowed();retention();built_records();',' legacy();borrowed();retention();built_records();preemption();rejected_fence();')
if len(sys.argv)>3:
    if sys.argv[3]=='--old-drain':
        code=sub(code,'                   !wddm->PreemptionPending[BC250_WDDM_NODE_COPY] &&\n','')
    elif sys.argv[3]=='--early-reject':
        # The rejected id completed at once, before the packets ahead of it on the node have finished.
        code=sub(code,'        if (wddm->RejectedPending[node] && !wddm->RefusalPending[node] &&\n'
                      '            wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&\n'
                      '            !(node == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : WddmRingPendingLocked(wddm, node)))\n',
                 '        if (wddm->RejectedPending[node] && !wddm->RefusalPending[node])\n')
    else: fail('unknown mutation %r' % sys.argv[3])
out.write_text(code)
