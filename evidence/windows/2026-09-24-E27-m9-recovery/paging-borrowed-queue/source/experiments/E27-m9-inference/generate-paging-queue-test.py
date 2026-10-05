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
a=s.index('typedef struct _BC250_PAGING_JOB');b=s.index('typedef struct _BC250_WDDM',a)
code=code.replace('/* ACTUAL_JOB_TYPE */',s[a:b])
code+=extract('void WddmGpuFencePaging(')+'\n'
# Exact stop-drain block; mocks do not claim hardware retirement.
a=s.index('    while (wddm->PagingHead) {',s.index('void WddmStop('))
b=s.index('    VidMmStop();',a)
code+='static void TestStopDrain(BC250_WDDM* wddm) {\n'+s[a:b]+'}\n'
a=s.index('// Accept well-formed work')
code+=extract('static void WddmPagingSubmitDpcRoutine(')+'\n'
part=s[a:]
a=part.index('static BOOLEAN WddmSubmitPagingHardware(');b=part.index('// The report side,')
code+=part[a:b]
code+=(e/'paging-queue-test-suffix.c').read_text()
if len(sys.argv)>3:
 assert sys.argv[3]=='--late-slot-release'
 old='            if (retired->Borrowed) {\n                RtlZeroMemory(retired,sizeof(*retired));\n                retired=NULL;\n            }\n            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);'
 new='            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);\n            if (retired->Borrowed) {\n                RtlZeroMemory(retired,sizeof(*retired));\n                retired=NULL;\n            }'
 assert code.count(old)==1
 code=code.replace(old,new)
Path(sys.argv[2]).write_text(code)
