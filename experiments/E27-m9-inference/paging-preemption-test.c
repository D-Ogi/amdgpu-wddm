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
// BD-085: the page-directory root a native span resolves against. A native paging record carries GPU virtual
// addresses, and the GPU's own walker resolves them: PagingWriteRing (gfx.c:4011-4019) emits an SDMA VM flush
// that writes the record's root into the page-directory base register pair of BC250_SDMA_PAGING_VMID, VMID 2
// (bc250_sdma.h:154-155, vmid_pool.h:37), invalidates that VMID and then emits the span's IB. No CPU
// translation happens on this route; the VidMmTranslate software walk of docs/design/paging-node.md:34-38
// belongs to the direct route, which that note's decision (:31-32) binds to physical MC addresses and to
// reprogramming no VMID. So the root has to be a page-directory base the hardware accepts:
// bc250_sdma_emit_vm_flush itself refuses a root that is not a multiple of AMDGPU_GPU_PAGE_SIZE, 4096
// (bc250_sdma.c:1248, amdgpu.h:450), and a root of zero is a VMID that was given no root in this engine
// instance (vmid_pool.h:43). paging_private.c's RootUsable (paging_private.c:201-205) is that contract at
// admission: it refuses a native span whose root is zero or not 4 KiB aligned. The one DDI that submits
// virtual paging work, Bc250WddmSubmitCommandVirtual, hands WddmSubmitPagingHardwareRoot the submitting
// context's RootPhysical (wddm.c:6228-6231). The fixtures below therefore submit through the Root entry point,
// as that DDI does, with one root: non-zero, 4 KiB aligned, never dereferenced, and the same value the record
// is built with, so that binding it at admission rewrites what the builder already wrote and the detachment
// check below can still compare a snapshot taken before the submit.
#define PREEMPT_ROOT 0x4000ull
// The same base plus 8 bytes: non-zero, so it passes the zero half of the gate, and misaligned, so it must
// fail the 4 KiB half. Without this value the alignment half of RootUsable is untested - dropping it from the
// parser left the whole suite green (BD-085 review finding 2).
#define PREEMPT_ROOT_MISALIGNED (PREEMPT_ROOT|8ull)
static BC250_PAGING_JOB *preemptedOne,*preemptedTwo;
static BOOLEAN WddmStopping(BC250_WDDM* w) {return (BOOLEAN)w->Stopping;}
static LONG InterlockedExchange(LONG* p,LONG value) {LONG old=*p;*p=value;return old;}
static int KeInsertQueueDpc(KDPC** d,void* a,void* b) {(void)d;(void)a;(void)b;return 1;}
// 0.7.210.1: the report says whether it reached dxgkrnl, and the pass pairs its own DxgkCbNotifyDpc when the
// NotifyDpcInReport gate is open. reportFails makes the refusal path testable: the pending flag must come back and
// no fence may advance. PairInThisPass is counted so a pass cannot quietly stop pairing.
static int reportFails,pairedReports,notifyCalls;
static BOOLEAN WddmReport(BC250_DEVICE* d,DXGKARGCB_NOTIFY_INTERRUPT_DATA* data,BOOLEAN PairInThisPass)
{
 check(!lockHeld,"scheduler notification outside queue lock");
 if(PairInThisPass) pairedReports++;
 if(reportFails) {reportFails--;return FALSE;}
 events[eventCount++]=*data;
 if(data->InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED) {
  check(!d->Wddm || !((BC250_WDDM*)d->Wddm)->PagingHead,"preempt notification releases private FIFO");
  check(!preemptedOne->Borrowed&&!preemptedTwo->Borrowed,"unexecuted slots released before scheduler callback");
  if(replayData) {
   check(WddmSubmitPagingHardwareRoot(d,d->Wddm,replayData,replayBytes,0x20000,192,TRUE,52,PREEMPT_ROOT),"scheduler may replay same fence during callback");
   replayData=NULL;
  }
 }
 return TRUE;
}
static void WddmPairingReport(BC250_WDDM* w,BOOLEAN vsync){(void)w;check(!vsync,"the report pass reports no vsync");}
static void WddmNotifyDpcNow(BC250_DEVICE* d,BOOLEAN samePass){(void)d;check(samePass,"the pass pairs its own reports");notifyCalls++;}
// F-q44-mem-4: the node-0 side of DxgkDdiSubmitCommandVirtual's STATUS_INVALID_PARAMETER. The generator extracts
// the actual wrapper (Bc250WddmSubmitCommandVirtual) with WddmBeginSubmissionLocked and WddmNoteSubmittedLocked;
// its body, the Impl, is this stand-in, which either puts the packet on the node-0 queue or answers the status the
// UMD blob parser gives a malformed submission. HwPending is what WddmGfxHeadLocked makes of that queue: TRUE while
// it has a head.
typedef void* HANDLE;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define BC250_WDDM_MAGIC_CONTEXT 0x4d37437au
typedef struct { ULONG Magic; UINT NodeOrdinal; } BC250_WDDM_OBJECT;
typedef struct { UINT SubmissionFenceId; HANDLE hContext; UINT NodeOrdinal; } DXGKARG_SUBMITCOMMANDVIRTUAL;
static BC250_WDDM_OBJECT gfxContext={BC250_WDDM_MAGIC_CONTEXT,0};
static UINT ringFences[8];
static unsigned ringCount;
static NTSTATUS implStatus;
static BC250_WDDM* WddmOf(const HANDLE h){return (BC250_WDDM*)((BC250_DEVICE*)h)->Wddm;}
static BC250_WDDM_OBJECT* WddmObject(const HANDLE h,ULONG magic)
{BC250_WDDM_OBJECT* o=(BC250_WDDM_OBJECT*)h;return o&&o->Magic==magic?o:NULL;}
static void ProgressEnterInput(BC250_PROGRESS_SITE_ID s,LONG v){(void)s;(void)v;progressOpen++;}
static NTSTATUS Bc250WddmSubmitCommandVirtualImpl(const HANDLE hAdapter,const DXGKARG_SUBMITCOMMANDVIRTUAL* s)
{
 BC250_WDDM* w=WddmOf(hAdapter);KIRQL irql;
 check(!lockHeld&&w->ActiveSubmissions[0]==1,"the wrapper holds the submission active, outside Lock, around its body");
 if(implStatus!=STATUS_SUCCESS)return implStatus;
 KeAcquireSpinLock(&w->Lock,&irql);
 ringFences[ringCount++]=s->SubmissionFenceId;
 w->HwPending=TRUE;
 KeReleaseSpinLock(&w->Lock,irql);
 return STATUS_SUCCESS;
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
 check(PagingPrivateQueuedNativeHeader(b,sizeof(b),0,0x20000,192,PREEMPT_ROOT,64,8,0),"preempt native B");
 check(PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4),"preempt direct C");
 one=PagingPrivateQueueSlot(a,direct,0x10000,4,TRUE);
 two=PagingPrivateQueueSlot(b,native,0x20000,192,TRUE);
 three=PagingPrivateQueueSlot(c,direct,0x30000,4,TRUE);
 check(one&&two&&three,"preempt slots valid");
 if(!one||!two||!three)return;
 preemptedOne=two;preemptedTwo=three;
 memcpy(beforeB,b,sizeof(beforeB));memcpy(beforeC,c,sizeof(beforeC));
 completionSlots[51]=one;completionSlots[52]=two;completionSlots[53]=three;
 check(WddmSubmitPagingHardwareRoot(&d,&w,a,direct,0x10000,4,TRUE,51,PREEMPT_ROOT),"preempt first executing");
 // The root gate, stated both ways before the queue is built on top of it. Offered with no root, this native
 // span cannot be resolved to a physical address and must be refused outright: nothing is borrowed, nothing is
 // queued, no submission is counted. This is the control that fails if RootUsable ever admits a rootless native
 // span again, which is what this fixture asked for until BD-085.
 check(!WddmSubmitPagingHardwareRoot(&d,&w,b,native,0x20000,192,TRUE,52,0),"native span without a root is refused");
 check(!two->Borrowed&&w.PagingHead==one&&w.PagingTail==one&&submitted==1,"a refused native span leaves the queue untouched");
 // The other half of the same gate. A misaligned root is not a page-directory base: bc250_sdma_emit_vm_flush
 // would refuse to write it (bc250_sdma.c:1248), and if admission bound it anyway the dispatch walk would
 // refuse the record it was bound into (NativeValid, paging_private.c:18-28, from gfx.c:4059), which leaves an
 // admitted paging job that can never reach the ring and a node closed with no completion.
 check(!WddmSubmitPagingHardwareRoot(&d,&w,b,native,0x20000,192,TRUE,52,PREEMPT_ROOT_MISALIGNED),
   "native span with a misaligned root is refused");
 check(!two->Borrowed&&w.PagingHead==one&&w.PagingTail==one&&submitted==1,"a misaligned native root leaves the queue untouched");
 check(!memcmp(beforeB,b,sizeof(beforeB)),"a refused native span keeps the root the builder wrote");
 check(WddmSubmitPagingHardwareRoot(&d,&w,b,native,0x20000,192,TRUE,52,PREEMPT_ROOT),"preempt second accepted with its context root");
 check(WddmSubmitPagingHardwareRoot(&d,&w,c,direct,0x30000,4,TRUE,53,PREEMPT_ROOT),"preempt third accepted");
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
 check(WddmSubmitPagingHardwareRoot(&d,&w,c,direct,0x30000,4,TRUE,53,PREEMPT_ROOT),"scheduler replays C unchanged fence");
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
 check(WddmSubmitPagingHardwareRoot(&d,&w,a,direct,0x10000,4,TRUE,61,PREEMPT_ROOT),"racing submit retains ownership while preempt pending");
 check(WddmSubmitPagingHardwareRoot(&d,&w,c,direct,0x30000,4,TRUE,62,PREEMPT_ROOT),"racing second submit retained");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!submitted&&!eventCount&&one->Borrowed&&three->Borrowed,"active admission blocks notification and hardware launch");
 w.ActiveSubmissions[1]=0;WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&!completionCount&&events[0].DmaPreempted.LastCompletedFenceId==0,"idle boundary preempts all queued work without completion");
 check(!w.PagingHead&&!one->Borrowed&&!three->Borrowed,"racing admissions returned to scheduler");
 completionSlots[61]=one;completionSlots[62]=three;
 check(WddmSubmitPagingHardwareRoot(&d,&w,a,direct,0x10000,4,TRUE,61,PREEMPT_ROOT),"idle-boundary same-fence replay");
 check(WddmSubmitPagingHardwareRoot(&d,&w,c,direct,0x30000,4,TRUE,62,PREEMPT_ROOT),"idle-boundary second replay");
 arrived=activeSeq;WddmGpuFencePaging(&d);arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==2&&!w.PagingHead&&!failureCount,"idle-boundary replay completes normally");

 // A watchdog's late completion is real, but cannot make the closed engine
 // available for replay. Retain the never-executed tail for recovery.
 reset();memset(&w,0,sizeof(w));eventCount=0;
 a[6]=11;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 completionSlots[71]=one;completionSlots[72]=three;
 check(WddmSubmitPagingHardwareRoot(&d,&w,a,direct,0x10000,4,TRUE,71,PREEMPT_ROOT),"fault control executing");
 check(WddmSubmitPagingHardwareRoot(&d,&w,c,direct,0x30000,4,TRUE,72,PREEMPT_ROOT),"fault control accepted tail");
 WddmPreemptFence(&d,93,1);w.WatchdogFaulted[1]=1;
 arrived=activeSeq;WddmGpuFencePaging(&d);WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&events[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&
  events[0].DmaCompleted.SubmissionFenceId==71,"late fence reports only real completion");
 check(w.PreemptionPending[1]&&w.PagingHead==three&&three->Borrowed&&submitted==1,
  "faulted replay path retains accepted ownership without false preempt acknowledgement");
 TestStopDrain(&w); // Host cleanup after simulated hardware idle, not recovery evidence.
}

// The GPU retires the oldest node-0 packet: the record WddmGpuFence makes, then the queue's new head.
static void retire0(BC250_WDDM* w)
{
 KIRQL irql;unsigned i;
 KeAcquireSpinLock(&w->Lock,&irql);
 WddmRecordCompletionLocked(w,ringFences[0],0);
 for(i=1;i<ringCount;i++)ringFences[i-1]=ringFences[i];
 ringCount--;
 w->HwPending=ringCount!=0;
 KeReleaseSpinLock(&w->Lock,irql);
}
static NTSTATUS submit0(BC250_DEVICE* d,UINT fence,NTSTATUS impl)
{
 DXGKARG_SUBMITCOMMANDVIRTUAL s;
 memset(&s,0,sizeof(s));s.SubmissionFenceId=fence;s.hContext=&gfxContext;
 implStatus=impl;
 return Bc250WddmSubmitCommandVirtual(d,&s);
}
// Node 0's last completed fence as the driver reports it: the read DMA_PREEMPTED.LastCompletedFenceId takes in
// the report pass, and the lower bound Bc250WddmResetEngine takes (Bc250HangNodeLastCompleted, then the boundary).
static int observed,observedKnown;
static UINT lastObserved;
static int lastCompleted0(BC250_WDDM* w,UINT* fence)
{
 int known=Bc250HangSchedulerBoundary(&w->Recovery[0],w->LastReportedValid[0],(UINT)w->LastReportedFence[0],fence);
 if(known) {
  check(!observedKnown||(LONG)(*fence-lastObserved)>=0,"the last completed fence never goes back");
  observedKnown=1;lastObserved=*fence;observed++;
 }
 return known;
}
static void rejected_setup(BC250_WDDM* w,BC250_DEVICE* d)
{
 static BC250_PAGING_JOB idle;
 reset();memset(w,0,sizeof(*w));d->Wddm=w;eventCount=0;replayData=NULL;
 preemptedOne=preemptedTwo=&idle;ringCount=0;observed=observedKnown=0;lastObserved=0;
}
// DxgkDdiSubmitCommandVirtual, STATUS_INVALID_PARAMETER (d3dkmddi.md, its return table): the SubmissionFenceId
// "will be considered completed after all previous packets on the hardware finished and at that point the driver
// notion of the last completed fence ID should be updated to this value". good1, the rejected one, good2: the
// rejected id is never the last completed fence before good1's DMA_COMPLETED reached dxgkrnl, it is after, and
// good2's completion moves past it. No DMA_COMPLETED ever names it, and no node is closed.
static void rejected_fence(void)
{
 BC250_WDDM w;BC250_DEVICE d={0};
 UINT f=0;

 rejected_setup(&w,&d);
 check(submit0(&d,10,STATUS_SUCCESS)==STATUS_SUCCESS,"good1 goes to the ring");
 check(submit0(&d,11,STATUS_INVALID_PARAMETER)==STATUS_INVALID_PARAMETER,"the malformed submit answers STATUS_INVALID_PARAMETER");
 check(w.RejectedPending[0]&&w.RejectedFence[0]==11&&w.ActiveSubmissions[0]==0,"the wrapper records the rejected id");
 check(ringCount==1&&w.LastSubmittedFence[0]==11,"the rejected packet is not on the ring, only in the submitted watermark");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!eventCount&&!lastCompleted0(&w,&f)&&w.RejectedPending[0],"nothing is completed while good1 runs, the rejected id least of all");
 WddmPreemptFence(&d,90,0);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!eventCount,"a preemption waits for good1");
 retire0(&w);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==2&&events[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&events[0].DmaCompleted.SubmissionFenceId==10&&
  events[0].DmaCompleted.NodeOrdinal==0,"good1's completion is reported, and only good1's");
 check(events[1].InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED&&events[1].DmaPreempted.LastCompletedFenceId==11,
  "after good1 the preemption names the rejected id as last completed");
 check(lastCompleted0(&w,&f)&&f==11&&!w.RejectedPending[0],"after good1 the last completed fence is the rejected id");
 check(submit0(&d,12,STATUS_SUCCESS)==STATUS_SUCCESS,"good2 goes to the ring: node 0 stays open");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==2&&lastCompleted0(&w,&f)&&f==11,"while good2 runs the last completed fence stays at the rejected id");
 retire0(&w);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==3&&events[2].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&events[2].DmaCompleted.SubmissionFenceId==12,
  "good2's completion is reported");
 check(lastCompleted0(&w,&f)&&f==12,"after good2 the last completed fence is good2");
 WddmPreemptFence(&d,91,0);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==4&&events[3].InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED&&events[3].DmaPreempted.LastCompletedFenceId==12,
  "after good2 the preemption names good2");
 check(observed==3&&!w.WatchdogFaulted[0]&&!w.RefusalPending[0]&&!failureCount,"no node closed, no submission failed");

 // good2 queued behind good1 before good1 retires (dxgkrnl pipelines node-0 packets). The rejected id waits for
 // the whole queue: while good2 runs the driver's value is good1's, which dxgkrnl cannot see, because a preemption
 // is reported only on an idle node and a ResetEngine with a job on the ring names that job's fence. It still
 // never runs ahead of good1 and never goes back.
 rejected_setup(&w,&d);
 check(submit0(&d,20,STATUS_SUCCESS)==STATUS_SUCCESS,"pipelined: good1");
 check(submit0(&d,21,STATUS_INVALID_PARAMETER)==STATUS_INVALID_PARAMETER,"pipelined: the rejected one");
 check(submit0(&d,22,STATUS_SUCCESS)==STATUS_SUCCESS&&ringCount==2,"pipelined: good2 queued behind good1");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!eventCount&&!lastCompleted0(&w,&f),"pipelined: nothing completed before good1");
 retire0(&w);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&events[0].DmaCompleted.SubmissionFenceId==20,"pipelined: good1's completion");
 check(lastCompleted0(&w,&f)&&f==20&&w.RejectedPending[0],"pipelined: the rejected id waits while good2 runs");
 retire0(&w);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==2&&events[1].DmaCompleted.SubmissionFenceId==22,"pipelined: good2's completion");
 check(lastCompleted0(&w,&f)&&f==22&&!w.RejectedPending[0],"pipelined: after good2 the last completed fence is good2");
 WddmPreemptFence(&d,92,0);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==3&&events[2].DmaPreempted.LastCompletedFenceId==22,"pipelined: the preemption names good2, never 21");
 check(!w.WatchdogFaulted[0]&&!w.RefusalPending[0]&&!failureCount,"pipelined: no node closed");

 // Two rejected packets behind good1: the newer id is the one the driver ends at. A rejected packet on an idle
 // node is completed at the next pass, with nothing before it to wait for.
 rejected_setup(&w,&d);
 check(submit0(&d,30,STATUS_SUCCESS)==STATUS_SUCCESS,"two rejected: good1");
 check(submit0(&d,31,STATUS_INVALID_PARAMETER)==STATUS_INVALID_PARAMETER&&
       submit0(&d,32,STATUS_INVALID_PARAMETER)==STATUS_INVALID_PARAMETER,"two rejected: both refused");
 check(w.RejectedFence[0]==32,"two rejected: the newer id is kept");
 retire0(&w);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&lastCompleted0(&w,&f)&&f==32,"two rejected: after good1 the last completed fence is the newer one");
 rejected_setup(&w,&d);
 check(submit0(&d,40,STATUS_INVALID_PARAMETER)==STATUS_INVALID_PARAMETER,"idle node: refused");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!eventCount&&lastCompleted0(&w,&f)&&f==40&&!w.RejectedPending[0],"idle node: the rejected id is completed at once, with no DMA_COMPLETED");
 check(!w.WatchdogFaulted[0]&&!failureCount,"idle node: no node closed");
 reset();
}
