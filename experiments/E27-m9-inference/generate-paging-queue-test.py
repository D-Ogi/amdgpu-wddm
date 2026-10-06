from pathlib import Path
import sys
r=Path(sys.argv[1]);e=Path(__file__).resolve().parent
s=(r/'driver/kmd/wddm.c').read_text()
def extract(marker):
 a=s.index(marker);b=s.index('{',a);depth=1;i=b+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[a:i]
code=(e/'paging-queue-test-prefix.c').read_text()
a=s.index('typedef struct _BC250_PAGING_JOB');b=s.index('typedef struct _BC250_WDDM {',a)
code=code.replace('/* ACTUAL_JOB_TYPE */',s[a:b])
# Fence ledger (later revisions): extracted when the source has it.
if 'static void WddmRecordFenceLedgerLocked(' in s:
 code=code.replace('/* FENCE_LEDGER_FIELD */','BC250_WDDM_FENCE_LEDGER FenceLedger[2];')
 code+=extract('static void WddmRecordFenceLedgerLocked(')+'\n'
# C48 (later revisions): node 1's ring-gap edges run inside the drain. Extracted when the source has them, so
# the harness compiles the actual drain either way, and the instrument is exercised by every drain case here.
if 'static void WddmRingGapEdgeLocked(' in s:
 code=code.replace('/* RING_GAP_FIELD */',
                   'BC250_RING_GAP RingGap[BC250_WDDM_NODE_COUNT_MAX];\n'
                   ' volatile long long RingGapLastVsyncQpc;\n'
                   ' LARGE_INTEGER RingGapFrequency;')
 # One substitution, and it is the harness's own: the driver reads the counter frequency once at WddmStart and
 # passes NULL here, so in a harness that never runs WddmStart every gap would measure 0 us and the arithmetic
 # would go unexercised. The stand-in clock reports its frequency through that argument, so hand it the field.
 edge=extract('static void WddmRingGapEdgeLocked(')
 assert edge.count('KeQueryPerformanceCounter(NULL)')==1
 code+=edge.replace('KeQueryPerformanceCounter(NULL)','KeQueryPerformanceCounter(&Wddm->RingGapFrequency)')+'\n'
# KMD172: the quota requeue is called by the drain and armed on the actual timer.
code+=extract('static void WddmRequeuePagingDrain(')+'\n'
code+=extract('void WddmGpuFencePaging(')+'\n'
# Exact stop-drain block; mocks do not claim hardware retirement.
a=s.index('    while (wddm->PagingHead) {',s.index('void WddmStop('))
b=s.index('    VidMmStop();',a)
code+='static void TestStopDrain(BC250_WDDM* wddm) {\n'+s[a:b]+'}\n'
a=s.index('// Accept driver-built work')
# KMD172 split the watchdog DPC into its body and a wrapper that records the site.
if 'static void WddmPagingSubmitDpcCheck(' in s:
 code+=extract('static void WddmPagingSubmitDpcCheck(')+'\n'
code+=extract('static void WddmPagingSubmitDpcRoutine(')+'\n'
code+=extract('static void WddmPagingDrainDpcRoutine(')+'\n'
part=s[a:]
a=part.index('static BOOLEAN WddmSubmitPagingHardwareRoot(');b=part.index('// The report side,')
code+=part[a:b]
code+=(e/'paging-queue-test-suffix.c').read_text()
mutations={
 # Retirement order: completion published before the OS slot is released.
 '--late-slot-release':('            RtlZeroMemory(retired,sizeof(*retired));\n            retired=NULL;\n            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);',
                        '            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);\n            RtlZeroMemory(retired,sizeof(*retired));\n            retired=NULL;'),
 # KMD171 behaviour: no quota, a drain retires for as long as retirements keep coming.
 '--drop-quota':('next=PagingDrainNext(&retiredCount,completed,PAGING_DRAIN_QUOTA);',
                 'next=completed?PagingDrainContinue:PagingDrainReturn;'),
 # A quota without the requeue: the yield leaves the head with nothing to pick it up.
 '--drop-requeue':('            WddmRequeuePagingDrain(wddm);\n','            (void)WddmRequeuePagingDrain;\n'),
}
for m in sys.argv[3:]:
 old,new=mutations[m]
 assert code.count(old)==1,m
 code=code.replace(old,new)
Path(sys.argv[2]).write_text(code)
