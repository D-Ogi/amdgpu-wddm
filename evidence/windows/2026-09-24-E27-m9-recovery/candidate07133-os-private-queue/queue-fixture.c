
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_NODE_COPY 1
#define BC250_WDDM_NODE_COUNT_MAX 2
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 500
#define BC250_WDDM_TAG 1
#include "paging_private.h"
#define C_ASSERT(e) typedef char assert_job_fits[(e)?1:-1]
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef unsigned char UCHAR;
#define POOL_FLAG_NON_PAGED 1
#define NT_SUCCESS(s) ((s)>=0)
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define RtlCopyMemory memcpy
typedef unsigned long ULONG,UINT;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef unsigned char BOOLEAN;
typedef int KIRQL;
typedef long NTSTATUS,LONG;
typedef struct { long long QuadPart; } LARGE_INTEGER;
typedef void KDPC;
typedef void* PVOID;
typedef struct _BC250_PAGING_JOB {
    struct _BC250_PAGING_JOB* Next;
    ULONGLONG Start;
    ULONG ByteCount, PrivateBytes;
    UINT Fence;
    BOOLEAN VirtualAddress, Borrowed;
    const UCHAR* Data;
} BC250_PAGING_JOB;
C_ASSERT(sizeof(BC250_PAGING_JOB)<=PAGING_PRIVATE_JOB_BYTES);


typedef struct {
 BC250_PAGING_JOB *PagingHead,*PagingTail;
 ULONGLONG PagingDeadline;
 int Stopping,WatchdogFaulted[2],PagingHwPending,PagingDeferredValid;
 ULONG PagingHwSeq;
 UINT PagingHwFence;
 int Lock,PagingSubmitTimer,PagingSubmitDpc;
 LONG PagingQueueBorrowed,PagingHwSubmitted,PagingHwCompleted,PagingHwRefused,PagingHwTimeouts;
} BC250_WDDM;
typedef struct { void* Wddm; } BC250_DEVICE;
static int checks,failures,allocated,lockHeld,submitted,completionCount,reports,failureCount,allocationCalls,immediateFence;
static ULONG nextSeq,activeSeq,arrived;
static ULONGLONG now;
static unsigned char submittedBytes[256];
static UINT fences[256];
static void check(int yes,const char*n){checks++;if(!yes){failures++;printf("FAIL %s\n",n);}}
static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;*i=0;check(!lockHeld,"nonrecursive lock");lockHeld=1;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;lockHeld=0;}
static ULONGLONG KeQueryInterruptTime(void){return now;}
static int KeCancelTimer(int*t){(void)t;return 0;}
static int KeSetTimer(int*t,LARGE_INTEGER d,int*p){(void)t;(void)d;(void)p;return 0;}
static LONG InterlockedIncrement(LONG*p){return ++*p;}
static void GuardLog(const char*f,...){(void)f;}
static BC250_PAGING_JOB* completionSlots[256];
static int RecordWord(void* unused,const PAGING_PRIVATE_SPAN* span)
{
 (void)unused;submittedBytes[submitted++]=(unsigned char)(span->Kind==PAGING_PRIVATE_NATIVE ? 77u : span->Words[0]);return 1;
}
static BOOLEAN GfxPagingFenceArrived(BC250_DEVICE*d,ULONG s)
{(void)d;if(arrived!=s)return FALSE;activeSeq=0;return TRUE;}
static NTSTATUS GfxSubmitPaging(BC250_DEVICE*d,const void*p,ULONG b,ULONGLONG start,ULONG count,BOOLEAN va,ULONG*seq)
{
 (void)d;(void)b;(void)start;(void)count;(void)va;
 check(!activeSeq,"never dispatch onto occupied ring");
 if(activeSeq)return -1;
 check(PagingPrivateVisitMixed(p,b,start,count,va,RecordWord,NULL),"real parser dispatch");
 activeSeq=++nextSeq;*seq=activeSeq;if(immediateFence)arrived=activeSeq;return 0;
}
static void WddmRecordCompletionLocked(BC250_WDDM*w,UINT fence,UINT node)
{
 unsigned i;unsigned char* slot=(unsigned char*)completionSlots[fence];
 (void)w;(void)node;check(lockHeld,"completion published under lock");
 if(slot) {
  for(i=0;i<PAGING_PRIVATE_JOB_BYTES;i++)check(slot[i]==0,"slot released before completion");
  /* OS may reuse/rewrite its storage as soon as completion is published. */
  memset(slot,0xa5,PAGING_PRIVATE_JOB_BYTES);completionSlots[fence]=NULL;
 }
 fences[completionCount++]=fence;
}
static void WddmQueueReport(BC250_WDDM*w){(void)w;check(!lockHeld,"report outside lock");reports++;}
static void WddmFailSubmission(BC250_DEVICE*d,UINT fence,UINT node)
{(void)d;(void)fence;(void)node;failureCount++;}
static void GfxPagingSubmitFail(BC250_DEVICE*d){(void)d;failureCount++;}
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    if (!wddm) return;
    for (;;) {
        BC250_PAGING_JOB* retired=NULL;
        BOOLEAN completed=FALSE, failed=FALSE;
        UINT fence=0;
        ULONG seq=0;
        NTSTATUS status;
        KIRQL irql;
        LARGE_INTEGER due;
        KeAcquireSpinLock(&wddm->Lock,&irql);
        if (wddm->Stopping) {
            KeReleaseSpinLock(&wddm->Lock,irql);
            return;
        }
        if (wddm->PagingHwPending && GfxPagingFenceArrived(Device,wddm->PagingHwSeq)) {
            retired=wddm->PagingHead;
            wddm->PagingHwPending=FALSE;
            KeCancelTimer(&wddm->PagingSubmitTimer);
            completed=TRUE;
        } else if (!wddm->PagingHwPending && wddm->PagingHead &&
                   !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY]) {
            BC250_PAGING_JOB* job=wddm->PagingHead;
            if (!job->ByteCount) {
                retired=job;
                completed=TRUE;
            } else {
                // Lock covers GPU publication and its CPU pending state together.
                // An immediate IH DPC cannot observe a half-published submission.
                status=GfxSubmitPaging(Device,job->Data,job->PrivateBytes,job->Start,
                    job->ByteCount,job->VirtualAddress,&seq);
                if (NT_SUCCESS(status)) {
                    wddm->PagingHwPending=TRUE;
                    wddm->PagingHwSeq=seq;
                    wddm->PagingHwFence=job->Fence;
                    wddm->PagingDeadline=KeQueryInterruptTime()+10000ull*BC250_WDDM_SUBMIT_TIMEOUT_MS;
                    due.QuadPart=-10000ll*BC250_WDDM_SUBMIT_TIMEOUT_MS;
                    KeSetTimer(&wddm->PagingSubmitTimer,due,&wddm->PagingSubmitDpc);
                    InterlockedIncrement(&wddm->PagingHwSubmitted);
                } else {
                    failed=TRUE;
                    fence=job->Fence;
                    // Stop another caller from retrying this head before fail publication.
                    wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY]=TRUE;
                    InterlockedIncrement(&wddm->PagingHwRefused);
                    GuardLog("wddm: queued paging dispatch refused 0x%08X fence %u",status,fence);
                }
            }
        }
        if (completed) {
            fence=retired->Fence;
            wddm->PagingHead=retired->Next;
            if (!wddm->PagingHead) wddm->PagingTail=NULL;
            if (retired->ByteCount) InterlockedIncrement(&wddm->PagingHwCompleted);
            // Publishing completion can let another CPU reuse the OS buffer.
            // Clear borrowed ownership first and never touch that slot again.
            RtlZeroMemory(retired,sizeof(*retired));
            retired=NULL;
            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);
        }
        KeReleaseSpinLock(&wddm->Lock,irql);
        if (failed) WddmFailSubmission(Device,fence,BC250_WDDM_NODE_COPY);
        if (!completed) return; // no polling loop while the GPU is executing
        WddmQueueReport(wddm);
    }
}
static void TestStopDrain(BC250_WDDM* wddm) {
    while (wddm->PagingHead) {
        BC250_PAGING_JOB* job=wddm->PagingHead;
        wddm->PagingHead=job->Next;
        RtlZeroMemory(job,sizeof(*job));
    }
    wddm->PagingTail=NULL;
}
static void WddmPagingSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    BOOLEAN timedOut = FALSE;
    UINT fence = 0;
    ULONG seq = 0;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFencePaging(device);         // late is still arrived
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->PagingHwPending && !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] &&
        KeQueryInterruptTime()>=wddm->PagingDeadline)
    {
        timedOut = TRUE;
        fence = wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        seq = wddm->PagingHwSeq;
        wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] = TRUE;
        // Preserve PagingHwPending until a real fence or the OS recovery path.
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // As on node0, do not retire unexecuted paging commands. OS TDR sees the
    // still-pending fence; closing node1 does not invent a successful memory transfer.
    InterlockedIncrement(&wddm->PagingHwTimeouts);
    GfxPagingSubmitFail(device);
    GuardLog("wddm: PAGING HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u remains pending for OS TDR, node 1 ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    // No completion report for a fence that has not arrived.
}
static BOOLEAN WddmSubmitPagingHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId)
{
    BC250_PAGING_JOB* job;
    KIRQL irql;
    if (!ByteCount || PrivateBytes>PAGING_PRIVATE_BUFFER_BYTES) return FALSE;
    job=(BC250_PAGING_JOB*)PagingPrivateQueueSlot((void*)PrivateData,PrivateBytes,
        Start,ByteCount,VirtualAddress);
    if (!job) return FALSE; // malformed or stale legacy records are not built here
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    if (Wddm->Stopping || Wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] || job->Borrowed) {
        KeReleaseSpinLock(&Wddm->Lock,irql);
        return FALSE;
    }
    // All live slot ownership changes are serialized with queue retirement.
    // Replaying a still-owned start cannot overwrite its existing fence/link.
    job->Next=NULL;job->Borrowed=TRUE;
    job->Start=Start;job->ByteCount=ByteCount;job->PrivateBytes=PrivateBytes;
    job->Fence=FenceId;job->VirtualAddress=VirtualAddress;
    job->Data=(const UCHAR*)PrivateData;
    if (Wddm->PagingTail) Wddm->PagingTail->Next=job;
    else Wddm->PagingHead=job;
    Wddm->PagingTail=job;
    InterlockedIncrement(&Wddm->PagingQueueBorrowed);
    KeReleaseSpinLock(&Wddm->Lock,irql);
    WddmGpuFencePaging(Device);
    return TRUE;
}


static void legacy(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};unsigned a[7]={0};
 PagingPrivateHeader(a,sizeof(a),0,0,4);a[6]=11;
 check(!WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0,4,TRUE,11),"legacy record rejected without allocator fallback");
 check(!WddmSubmitPagingHardware(&d,&w,NULL,0,0,0,FALSE,12),"empty submission is not admitted by paging callers");
 check(!w.PagingHead&&!w.PagingQueueBorrowed&&!submitted&&!completionCount&&!allocated&&!allocationCalls,"unsupported admission has no ownership or completion effects");
}

static void reset(void) {
 check(!allocated,"all legacy allocations retired");
 lockHeld=submitted=completionCount=reports=failureCount=immediateFence=0;
 nextSeq=activeSeq=arrived=0;now=0;memset(completionSlots,0,sizeof(completionSlots));
}
static void borrowed(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},native[32]={0};
 BC250_PAGING_JOB *one,*two,*three,*four;int before=allocationCalls;
 unsigned size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,12);
 reset();a[6]=11;a[7]=22;a[8]=33;b[6]=44;b[7]=55;b[8]=66;
 check(PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,12),"queued A construction");
 check(PagingPrivateQueuedHeader(b,sizeof(b),0,0x10000,12),"queued B construction");
 check(PagingPrivateQueuedNativeHeader(native,sizeof(native),0,0x20000,192,0x4000,64,8,0),"queued native construction");
 one=PagingPrivateQueueSlot(a,size,0x10000,4,TRUE);
 two=PagingPrivateQueueSlot(a,size,0x10004,4,TRUE);
 three=PagingPrivateQueueSlot(b,size,0x10000,4,TRUE);
 four=PagingPrivateQueueSlot(native,sizeof(native),0x20000,192,TRUE);
 check(one&&two&&three&&four&&one!=two&&one!=three,"distinct actual queue nodes");
 completionSlots[21]=one;completionSlots[22]=two;completionSlots[23]=three;completionSlots[24]=four;
 check(WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,21),"borrow first");
 check(WddmSubmitPagingHardware(&d,&w,a,size,0x10004,4,TRUE,22),"borrow second split while busy");
 check(WddmSubmitPagingHardware(&d,&w,b,size,0x10000,4,TRUE,23),"same VA independent buffer queued");
 check(WddmSubmitPagingHardware(&d,&w,native,sizeof(native),0x20000,192,TRUE,24),"native whole record queued");
 check(!WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,25),"live duplicate start preserves ownership");
 check(one->Fence==21&&one->Next==two&&two->Next==three&&three->Next==four,"FIFO links intact");
 check(!allocated&&allocationCalls==before&&submitted==1,"borrowed queue never allocates");
 arrived=1;WddmGpuFencePaging(&d);
 check(completionCount==1&&submittedBytes[1]==22,"second split dispatch after real fence");
 arrived=2;WddmGpuFencePaging(&d);
 check(completionCount==2&&submittedBytes[2]==44,"independent same VA content retained");
 arrived=3;WddmGpuFencePaging(&d);
 check(completionCount==3&&submittedBytes[3]==77,"native record dispatch");
 arrived=4;WddmGpuFencePaging(&d);
 check(completionCount==4&&!w.PagingHead&&!w.PagingTail&&!activeSeq,"borrowed queue drains");
 check(allocationCalls==before&&!allocated&&!failureCount,"no allocation or failure across completion reuse");
 // OS rebuilds the completed buffer, immediately completing the next dispatch.
 check(PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,12),"OS buffer rebuilt for reuse");
 completionSlots[26]=one;immediateFence=1;
 check(WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,26),"reused slot accepted");
 WddmGpuFencePaging(&d);
 check(completionCount==5&&fences[4]==26&&!w.PagingHead,"immediate fence retires rebuilt slot");
 // Physical direct ranges use the same reservation machinery.
 PagingPrivateQueuedHeader(a,sizeof(a),16,0,12);immediateFence=0;
 completionSlots[27]=PagingPrivateQueueSlot(a,size,20,4,FALSE);
 check(WddmSubmitPagingHardware(&d,&w,a,size,20,4,FALSE,27),"physical partial range accepted");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==6&&submittedBytes[5]==22&&!allocated,"physical partial content and retirement");
}
static void retention(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[24]={0};BC250_PAGING_JOB* slot;
 int before=allocationCalls;reset();a[6]=9;
 PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 slot=PagingPrivateQueueSlot(a,sizeof(a),0x10000,4,TRUE);
 completionSlots[31]=slot;
 check(WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,31),"timeout fixture admitted");
 now=w.PagingDeadline;WddmPagingSubmitDpcRoutine(NULL,&d,NULL,NULL);
 check(w.PagingHead==slot&&slot->Borrowed&&w.PagingHwPending&&!completionCount,"timeout retains borrowed memory and pending fence");
 check(w.WatchdogFaulted[1]&&failureCount==1,"timeout closes engine admission");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==1&&!w.PagingHead,"late real fence permits retirement");
 check(allocationCalls==before&&!allocated,"timeout path never allocates/frees OS memory");
 reset();memset(&w,0,sizeof(w));PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 check(WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,32),"stop fixture admitted");
 w.Stopping=1;TestStopDrain(&w);
 check(!w.PagingHead&&!w.PagingTail&&!slot->Borrowed&&!allocated,"stop drain clears borrowed ownership without freeing OS storage");
 check(!WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,33)&&!slot->Borrowed,"stopped admission leaves OS slot untouched");
 // Host drain only: hardware quiescence/OS cancellation must be proved separately.
 activeSeq=0;w.PagingHwPending=0;
}
static void built_records(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 const char* names[2]={"direct.bin","native.bin"};unsigned char* data[2]={0};
 unsigned headers[2][4]={{0}};ULONGLONG starts[2]={0};unsigned i;
 char folder[512],path[600];size_t required=0;int before=allocationCalls;
 if(getenv_s(&required,folder,sizeof(folder),"BC250_QUEUE_FIXTURE_DIR") || !required)return;
 reset();
 for(i=0;i<2;i++) {
  FILE* file=NULL;
  check(sprintf_s(path,sizeof(path),"%s/%s",folder,names[i])>0,"import fixture path");
  check(!fopen_s(&file,path,"rb") && file,"actual builder fixture exists");
  if(!file)return;
  check(fread(headers[i],sizeof(headers[i]),1,file)==1 && fread(&starts[i],sizeof(starts[i]),1,file)==1,"builder fixture framing");
  check(headers[i][0]<=PAGING_PRIVATE_BUFFER_BYTES && headers[i][0]>0,"fixture private extent bounded");
  if(!headers[i][0] || headers[i][0]>PAGING_PRIVATE_BUFFER_BYTES){fclose(file);return;}
  data[i]=calloc(1,headers[i][0]);check(data[i]!=NULL,"host fixture storage");
  if(!data[i]){fclose(file);return;}
  check(fread(data[i],headers[i][0],1,file)==1 && fgetc(file)==EOF,"exact emitted record imported");
  fclose(file);
  completionSlots[41+i]=PagingPrivateQueueSlot(data[i],headers[i][0],starts[i],headers[i][1],headers[i][2]);
  check(completionSlots[41+i]!=NULL,"real builder output supplies queue node");
  check(WddmSubmitPagingHardware(&d,&w,data[i],headers[i][0],starts[i],headers[i][1],(BOOLEAN)headers[i][2],41+i),"actual builder-to-submit positive path");
 }
 check(w.PagingQueueBorrowed==2 && submitted==1&&!allocated&&allocationCalls==before,"both actual records borrowed while first is pending");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==2&&fences[0]==41&&fences[1]==42&&!w.PagingHead&&!failureCount,"actual builder records retire in order");
 check(submittedBytes[0]==headers[0][3]&&submittedBytes[1]==headers[1][3],"queue preserves actual direct/native command interpretation");
 free(data[0]);free(data[1]);
}
int main(void) {
 legacy();borrowed();retention();built_records();
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
