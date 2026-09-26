// Extends the existing queue harness with the actual report/preempt functions.
// These mocks represent scheduler callbacks; no GPU recovery is claimed.
#define DXGK_INTERRUPT_DMA_COMPLETED 1
#define DXGK_INTERRUPT_DMA_PREEMPTED 2
typedef struct { UINT SubmissionFenceId,NodeOrdinal,EngineOrdinal; } COMPLETE;
typedef struct { UINT PreemptionFenceId,LastCompletedFenceId,NodeOrdinal,EngineOrdinal; } PREEMPT;
typedef struct { int InterruptType; COMPLETE DmaCompleted; PREEMPT DmaPreempted; } DXGKARGCB_NOTIFY_INTERRUPT_DATA;
static DXGKARGCB_NOTIFY_INTERRUPT_DATA events[16];
static unsigned eventCount;
static const void* replayData;
static ULONG replayBytes;
static BC250_PAGING_JOB *preemptedOne,*preemptedTwo;
static BOOLEAN WddmStopping(BC250_WDDM* w) {return (BOOLEAN)w->Stopping;}
static LONG InterlockedExchange(LONG* p,LONG value) {LONG old=*p;*p=value;return old;}
static int KeInsertQueueDpc(KDPC** d,void* a,void* b) {(void)d;(void)a;(void)b;return 1;}
static void WddmReport(BC250_DEVICE* d,DXGKARGCB_NOTIFY_INTERRUPT_DATA* data)
{
 check(!lockHeld,"scheduler notification outside queue lock");
 events[eventCount++]=*data;
 if(data->InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED) {
  check(!d->Wddm || !((BC250_WDDM*)d->Wddm)->PagingHead,"preempt notification releases private FIFO");
  check(!preemptedOne->Borrowed&&!preemptedTwo->Borrowed,"unexecuted slots released before scheduler callback");
  if(replayData) {
   check(WddmSubmitPagingHardware(d,d->Wddm,replayData,replayBytes,0x20000,192,TRUE,52),"scheduler may replay same fence during callback");
   replayData=NULL;
  }
 }
}
/* ACTUAL_FUNCTIONS */
static void reset(void);
static void preemption(void)
{
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},c[64]={0};
 BC250_PAGING_JOB *one,*two,*three;
 unsigned direct=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,4);
 unsigned native=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_NATIVE,192);
 unsigned char beforeB[PAGING_PRIVATE_NATIVE_BYTES],beforeC[28];int beforeAlloc=allocationCalls;
 reset();eventCount=0;
 a[6]=11;c[6]=33;
 check(PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4),"preempt direct A");
 check(PagingPrivateQueuedNativeHeader(b,sizeof(b),0,0x20000,192,0x4000,64,8,0),"preempt native B");
 check(PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4),"preempt direct C");
 one=PagingPrivateQueueSlot(a,direct,0x10000,4,TRUE);
 two=PagingPrivateQueueSlot(b,native,0x20000,192,TRUE);
 three=PagingPrivateQueueSlot(c,direct,0x30000,4,TRUE);
 check(one&&two&&three,"preempt slots valid");
 if(!one||!two||!three)return;
 preemptedOne=two;preemptedTwo=three;
 memcpy(beforeB,b,sizeof(beforeB));memcpy(beforeC,c,sizeof(beforeC));
 completionSlots[51]=one;completionSlots[52]=two;completionSlots[53]=three;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,51),"preempt first executing");
 check(WddmSubmitPagingHardware(&d,&w,b,native,0x20000,192,TRUE,52),"preempt second accepted");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,53),"preempt third accepted");
 WddmPreemptFence(&d,91,1);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!eventCount&&two->Borrowed&&three->Borrowed,"active DMA prevents preempt report or ownership release");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(submitted==1&&!w.PagingHwPending&&completionCount==1,"preemption stops exactly after executing buffer");
 if(submitted!=1)return; // Negative mutation has dispatched B; avoid pretending it halted.
 check(w.PagingHead==two&&two->Next==three,"preempted FIFO retained until notification decision");
 replayData=b;replayBytes=native;
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==2&&events[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&
  events[0].DmaCompleted.SubmissionFenceId==51,"completion of A precedes preemption");
 check(events[1].InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED&&
  events[1].DmaPreempted.PreemptionFenceId==91&&events[1].DmaPreempted.LastCompletedFenceId==51,
  "preemption names only actual completed fence");
 check(submitted==2&&w.PagingHead==two&&two->Fence==52&&!three->Borrowed,"only scheduler replay launches B");
 check(!memcmp(beforeB,b,sizeof(beforeB))&&!memcmp(beforeC,c,sizeof(beforeC)),"command metadata and payload survive queue detachment");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,53),"scheduler replays C unchanged fence");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 arrived=activeSeq;WddmGpuFencePaging(&d);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(submitted==3&&completionCount==3&&fences[0]==51&&fences[1]==52&&fences[2]==53,"each accepted buffer executes exactly once after replay");
 check(submittedBytes[0]==11&&submittedBytes[1]==77&&submittedBytes[2]==33,"replay preserves direct/native command interpretation");
 check(!w.PagingHead&&!w.PagingTail&&!w.PreemptionPending[1]&&!failureCount&&allocationCalls==beforeAlloc,"replay drains without allocation or stranded work");

 // A submit already in flight at the preempt decision may join the software
 // FIFO. Its active wrapper prevents detachment until admission has finished.
 reset();memset(&w,0,sizeof(w));eventCount=0;replayData=NULL;
 a[6]=11;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 preemptedOne=one;preemptedTwo=three;
 WddmPreemptFence(&d,92,1);w.ActiveSubmissions[1]=1;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,61),"racing submit retains ownership while preempt pending");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,62),"racing second submit retained");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!submitted&&!eventCount&&one->Borrowed&&three->Borrowed,"active admission blocks notification and hardware launch");
 w.ActiveSubmissions[1]=0;WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&!completionCount&&events[0].DmaPreempted.LastCompletedFenceId==0,"idle boundary preempts all queued work without completion");
 check(!w.PagingHead&&!one->Borrowed&&!three->Borrowed,"racing admissions returned to scheduler");
 completionSlots[61]=one;completionSlots[62]=three;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,61),"idle-boundary same-fence replay");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,62),"idle-boundary second replay");
 arrived=activeSeq;WddmGpuFencePaging(&d);arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==2&&!w.PagingHead&&!failureCount,"idle-boundary replay completes normally");

 // A watchdog's late completion is real, but cannot make the closed engine
 // available for replay. Retain the never-executed tail for recovery.
 reset();memset(&w,0,sizeof(w));eventCount=0;
 a[6]=11;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 completionSlots[71]=one;completionSlots[72]=three;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,71),"fault control executing");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,72),"fault control accepted tail");
 WddmPreemptFence(&d,93,1);w.WatchdogFaulted[1]=1;
 arrived=activeSeq;WddmGpuFencePaging(&d);WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&events[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&
  events[0].DmaCompleted.SubmissionFenceId==71,"late fence reports only real completion");
 check(w.PreemptionPending[1]&&w.PagingHead==three&&three->Borrowed&&submitted==1,
  "faulted replay path retains accepted ownership without false preempt acknowledgement");
 TestStopDrain(&w); // Host cleanup after simulated hardware idle, not recovery evidence.
}
