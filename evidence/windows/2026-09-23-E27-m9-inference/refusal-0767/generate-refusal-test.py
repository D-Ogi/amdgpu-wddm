from pathlib import Path
import sys
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text()
names=['WddmCompleteSoftware','WddmFailSubmission','WddmGpuFence','WddmSubmitDpcRoutine','WddmGpuFencePaging','WddmPagingSubmitDpcRoutine','Bc250WddmResetFromTimeout']
chunks=[]
for name in names:
    ret='static NTSTATUS ' if name=='Bc250WddmResetFromTimeout' else ('void ' if name in ['WddmGpuFence','WddmGpuFencePaging'] else 'static void ')
    a=s.index(ret+name+'(');b=s.index('{',a)+1;depth=1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    chunks.append(s[a:b])
prefix=(here/'watchdog-test-prefix.c').read_text()+r'''
typedef int NTSTATUS;
typedef void* HANDLE;
#define STATUS_UNSUCCESSFUL (-1)
#define WddmDdiResetFromTimeout 0
static BC250_WDDM* WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static int WddmFirstCalls(BC250_WDDM*w,int id){(void)w;(void)id;return 0;}
'''
suffix=(here/'watchdog-test-suffix.c').read_text().replace('int main(void)','static int watchdog_controls(void)')
suffix+=(here/'refusal-test-suffix.c').read_text()
Path(sys.argv[2]).write_text(prefix+'\n'.join(chunks)+suffix)
