// Actual DDI wrappers + queue/fence callbacks; mocks only select admission outcomes.
#define BC250_WDDM_LOG_CALLS 4
#define BC250_WDDM_VMID 1
#define BC250_WDDM_MAGIC_CONTEXT 1
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)-3)
typedef void* HANDLE;
typedef struct {UINT NodeOrdinal; ULONGLONG RootPhysical;} BC250_WDDM_OBJECT;
typedef struct {HANDLE hContext;UINT NodeOrdinal,SubmissionFenceId;} DXGKARG_SUBMITCOMMAND;
typedef DXGKARG_SUBMITCOMMAND DXGKARG_SUBMITCOMMANDVIRTUAL;
static int admissionMode,computeRefuse,computeReadCount;
static ULONG computeSequence,computeArrived;
static const void* admissionData;static ULONG admissionBytes;static ULONGLONG admissionStart;
static BOOLEAN WddmSubmitHardware(BC250_DEVICE*,BC250_WDDM*,const BC250_WDDM_OBJECT*,ULONGLONG,ULONG,UINT,UINT);
static BC250_WDDM* WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static BC250_WDDM_OBJECT* WddmObject(HANDLE h,UINT magic){(void)magic;return h;}
static BOOLEAN GfxFenceArrived(BC250_DEVICE*d,ULONG seq){(void)d;computeReadCount++;return seq==computeArrived;}
static NTSTATUS GfxSubmitIb(BC250_DEVICE*d,UINT vm,ULONGLONG root,ULONGLONG va,ULONG bytes,ULONG*seq)
{(void)d;(void)vm;(void)root;(void)va;(void)bytes;if(computeRefuse)return -1;*seq=++computeSequence;return 0;}
static NTSTATUS Bc250WddmSubmitCommandImpl(HANDLE h,const DXGKARG_SUBMITCOMMAND* s)
{
 BC250_DEVICE*d=h;BC250_WDDM*w=d->Wddm;UINT n=s->NodeOrdinal;BC250_WDDM_FENCE_LEDGER snap;UINT f=0;BOOLEAN v=FALSE;
 (void)snap;(void)f;(void)v;
 if(admissionMode==1)return STATUS_INVALID_PARAMETER;
 if(admissionMode==2){w->RefusalPending[n]=TRUE;return STATUS_SUCCESS;}
 if(admissionMode==3){w->FenceLedger[n].Epoch++;return STATUS_SUCCESS;} // stale publisher negative control
 if(n==1){check(WddmSubmitPagingHardware(d,w,admissionData,admissionBytes,admissionStart,4,TRUE,s->SubmissionFenceId),"actual queued admission");return STATUS_SUCCESS;}
 if(!WddmSubmitHardware(d,w,s->hContext,0x4000,64,s->SubmissionFenceId,n))w->RefusalPending[n]=TRUE;
 return STATUS_SUCCESS;
}
static NTSTATUS Bc250WddmSubmitCommandVirtualImpl(HANDLE h,const DXGKARG_SUBMITCOMMANDVIRTUAL*s)
{return Bc250WddmSubmitCommandImpl(h,s);}
/* ACTUAL_FUNCTIONS */
static void ledger_tests(void)
{
 BC250_WDDM w={0};BC250_DEVICE d={&w};BC250_WDDM_OBJECT context={0,0x8000};
 DXGKARG_SUBMITCOMMAND call={NULL,1,101};BC250_WDDM_FENCE_LEDGER snap;UINT reported=0;BOOLEAN rv=FALSE;KIRQL irql;
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},c[64]={0};unsigned bytes=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,4);
 reset();eventCount=0;w.NodeCount=2;w.FenceLedger[0].Epoch=7;w.FenceLedger[1].Epoch=7;
 a[6]=11;b[6]=22;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);PagingPrivateQueuedHeader(b,sizeof(b),0,0x20000,4);PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 admissionMode=0;admissionData=a;admissionBytes=bytes;admissionStart=0x10000;
 check(Bc250WddmSubmitCommand(&d,&call)==0,"physical A accepted by OS");
 call.SubmissionFenceId=102;admissionData=b;admissionStart=0x20000;check(Bc250WddmSubmitCommandVirtual(&d,&call)==0,"virtual B accepted");
 call.SubmissionFenceId=103;admissionData=c;admissionStart=0x30000;check(Bc250WddmSubmitCommandVirtual(&d,&call)==0,"virtual C accepted");
 check(w.FenceLedger[1].SubmittedValid&&w.FenceLedger[1].Submitted==103&&!w.FenceLedger[1].HardwareValid,"accepted ABC is distinct from unexecuted hardware");
 check(submitted==1&&w.PagingHwFence==101&&w.PagingHwEpoch==7,"only A reaches physical transport");
 w.PreemptionPending[1]=1;arrived=activeSeq;WddmGpuFencePaging(&d);
 check(w.FenceLedger[1].HardwareValid&&w.FenceLedger[1].Hardware==101&&w.FenceLedger[1].Submitted==103,"A hardware completion never becomes queued C");
 check(!w.LastReportedValid[1],"hardware completion recorded before OS notification");
 KeAcquireSpinLock(&w.Lock,&irql);check(WddmSnapshotFenceLedgerLocked(&w,1,&snap,&reported,&rv)&&snap.Hardware==101&&snap.Submitted==103&&!rv,"joined recovery snapshot separates A/C/mailbox");
 w.ActiveSubmissions[1]=1;check(!WddmSnapshotFenceLedgerLocked(&w,1,&snap,&reported,&rv),"snapshot waits for CPU publisher");w.ActiveSubmissions[1]=0;
 WddmReleasePreemptedPagingLocked(&w);w.PreemptionPending[1]=0;KeReleaseSpinLock(&w.Lock,irql);
 check(!w.PagingHead&&w.FenceLedger[1].Hardware==101,"empty-queue completion race still names real A");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);check(w.LastReportedFence[1]==101&&w.FenceLedger[1].Hardware==101,"notification has its own unchanged history");
 call.SubmissionFenceId=102;admissionData=b;admissionStart=0x20000;Bc250WddmSubmitCommandVirtual(&d,&call);
 check(w.FenceLedger[1].Submitted==103,"same-ID scheduler replay cannot regress latest OS-owned C");
 arrived=activeSeq;WddmGpuFencePaging(&d);call.SubmissionFenceId=103;admissionData=c;admissionStart=0x30000;Bc250WddmSubmitCommandVirtual(&d,&call);arrived=activeSeq;WddmGpuFencePaging(&d);
 check(w.FenceLedger[1].Hardware==103&&submitted==3&&!w.PagingHead,"scheduler replay executes ABC once");
 // True virtual refusal is not OS accepted; physical success-with-refusal is.
 admissionMode=1;call.SubmissionFenceId=104;check(Bc250WddmSubmitCommandVirtual(&d,&call)==STATUS_INVALID_PARAMETER,"invalid virtual packet rejected");
 check(w.FenceLedger[1].Submitted==103&&w.RejectedPending[1],"rejected virtual fence does not enter submitted ledger");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);check(w.LastReportedFence[1]==104&&w.FenceLedger[1].Hardware==103,"rejection bookkeeping cannot masquerade as GPU execution");
 admissionMode=2;call.SubmissionFenceId=105;check(Bc250WddmSubmitCommand(&d,&call)==0,"physical refused hardware retains successful OS status");
 check(w.FenceLedger[1].Submitted==105&&w.FenceLedger[1].Hardware==103&&w.RefusalPending[1],"OS-owned faulted packet remains in reset submitted history");
 // Hardware/notification divergence on compute when later software work waits.
 admissionMode=0;call.hContext=&context;call.NodeOrdinal=0;call.SubmissionFenceId=121;computeSequence=10;computeArrived=0;
 check(Bc250WddmSubmitCommandVirtual(&d,&call)==0,"compute actual publication");
 w.DeferredValid=TRUE;w.DeferredFence=122;computeArrived=w.HwSeq;WddmGpuFence(&d);
 check(w.FenceLedger[0].HardwareValid&&w.FenceLedger[0].Hardware==121&&w.SubmittedFence[0]==122,"GPU121 is distinct from deferred notification122");
 computeRefuse=1;call.SubmissionFenceId=123;Bc250WddmSubmitCommandVirtual(&d,&call);computeRefuse=0;
 check(w.FenceLedger[0].Submitted==123&&w.FenceLedger[0].Hardware==121,"ring refusal never invents execution but preserves OS ownership");
 // Epoch-stale CPU publication and GPU observations do not affect new history.
 admissionMode=3;call.SubmissionFenceId=124;Bc250WddmSubmitCommandVirtual(&d,&call);
 check(w.FenceLedger[0].Submitted==123,"stale wrapper cannot publish into changed epoch");
 w.HwPending=TRUE;w.HwNode=0;w.HwEpoch=7;w.HwFence=125;w.HwSeq=77;computeArrived=77;computeReadCount=0;
 WddmGpuFence(&d);check(w.HwPending&&w.FenceLedger[0].Hardware==121&&!computeReadCount,"old hardware epoch cannot retire or probe new owner");w.HwPending=FALSE;
 admissionMode=0;w.RefusalPending[1]=FALSE;w.FenceLedger[1].Epoch=9;call.hContext=NULL;call.NodeOrdinal=1;call.SubmissionFenceId=126;
 PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);admissionData=a;admissionStart=0x10000;Bc250WddmSubmitCommandVirtual(&d,&call);
 w.FenceLedger[1].Epoch=10;arrived=activeSeq;WddmGpuFencePaging(&d);
 check(w.PagingHead&&w.PagingHwPending&&w.FenceLedger[1].Hardware==103,"old paging epoch retains ownership without false completion");
 w.PagingHwPending=FALSE;WddmGpuFencePaging(&d);check(w.PagingHead&&submitted==4,"old queued epoch cannot autonomously dispatch");TestStopDrain(&w);activeSeq=0;
 // Fence zero is valid; wrapping and original-ID replay are not numeric max.
 memset(&w.FenceLedger[0],0,sizeof(w.FenceLedger[0]));w.FenceLedger[0].Epoch=11;
 WddmRecordFenceLedgerLocked(&w,0,11,0xfffffffEu,FALSE);WddmRecordFenceLedgerLocked(&w,0,11,0u,FALSE);WddmRecordFenceLedgerLocked(&w,0,11,0xffffffffu,FALSE);
 check(w.FenceLedger[0].SubmittedValid&&w.FenceLedger[0].Submitted==0,"wrap accepted zero and older replay cannot regress");
 WddmRecordFenceLedgerLocked(&w,0,10,9u,FALSE);check(w.FenceLedger[0].Submitted==0,"old epoch history update refused");
 check(!WddmSnapshotFenceLedgerLocked(&w,2,&snap,&reported,&rv),"node bound enforced");
}
