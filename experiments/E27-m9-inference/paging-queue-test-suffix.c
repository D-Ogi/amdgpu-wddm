
/* BD-114 7.1: the budget the driver latches once at WddmStart (submit_watchdog.h, Bc250SubmitBudgetMs) and
 * node 1 then shares with node 0. Every fixture below sets it, because the harness never runs WddmStart. The
 * value is the lab's own shipping default (TdrDelay 10 s plus the 2 s margin) and deliberately not the old flat
 * BC250_WDDM_SUBMIT_TIMEOUT_MS: a drain that prices its deadline or its timer from a constant again fails the
 * two checks in retention() instead of passing them by accident. */
#define TEST_BUDGET_MS 12000ul
static void budget_ms(BC250_WDDM* w){w->SubmitBudgetMs=(ULONG)TEST_BUDGET_MS;}

static void legacy(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};unsigned a[7]={0};
 budget_ms(&w);
 PagingPrivateHeader(a,sizeof(a),0,0,4);a[6]=11;
 check(!WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0,4,TRUE,11),"legacy record rejected without allocator fallback");
 check(!WddmSubmitPagingHardware(&d,&w,NULL,0,0,0,FALSE,12),"empty submission is not admitted by paging callers");
 check(!w.PagingHead&&!w.PagingQueueBorrowed&&!submitted&&!completionCount&&!allocated&&!allocationCalls,"unsupported admission has no ownership or completion effects");
}

static void reset(void) {
 check(!allocated,"all legacy allocations retired");
 check(!progressOpen,"every progress site entered was left");
 lockHeld=submitted=completionCount=reports=failureCount=immediateFence=0;
 helperDevice=NULL;helperBudget=inHelper=helperRuns=quotaExits=stopAtReport=0;drainExit=drainRetired=0;
 nextSeq=activeSeq=arrived=0;now=0;memset(completionSlots,0,sizeof(completionSlots));
}
static void borrowed(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},native[32]={0};
 BC250_PAGING_JOB *one,*two,*three,*four;int before=allocationCalls;
 unsigned size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,12);
 reset();budget_ms(&w);a[6]=11;a[7]=22;a[8]=33;b[6]=44;b[7]=55;b[8]=66;
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
 check(WddmSubmitPagingHardwareRoot(&d,&w,native,sizeof(native),0x20000,192,TRUE,24,0x9000),"native whole record queued");
 check(!WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,25),"live duplicate start preserves ownership");
 check(native[6]==0x9000 && native[7]==0,"submission replaced build-time root before queuing");
 check(!WddmSubmitPagingHardwareRoot(&d,&w,native,sizeof(native),0x20000,192,TRUE,25,0xA000) && native[6]==0x9000,"owned duplicate cannot replace pending root");
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
 int before=allocationCalls;reset();budget_ms(&w);a[6]=9;
 PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 slot=PagingPrivateQueueSlot(a,sizeof(a),0x10000,4,TRUE);
 completionSlots[31]=slot;
 check(WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,31),"timeout fixture admitted");
 // BD-114 7.1: the deadline and the one-shot timer are both priced from the budget the start latched, never
 // from a constant of this driver's own. reset() left the clock at 0, so the deadline is the whole budget.
 check(w.PagingDeadline==10000ull*TEST_BUDGET_MS,"node 1 deadline is the latched budget");
 check(w.PagingSubmitTimer==1&&w.PagingSubmitDpc==-(int)(10000ul*TEST_BUDGET_MS),"node 1 timer armed for the latched budget");
 now=w.PagingDeadline-1;WddmPagingSubmitDpcRoutine(NULL,&d,NULL,NULL);
 check(w.PagingHwPending&&!w.WatchdogFaulted[1]&&!failureCount,"node 1 does not fire one clock tick before the budget");
 now=w.PagingDeadline;WddmPagingSubmitDpcRoutine(NULL,&d,NULL,NULL);
 check(w.PagingHead==slot&&slot->Borrowed&&w.PagingHwPending&&!completionCount,"timeout retains borrowed memory and pending fence");
 check(w.WatchdogFaulted[1]&&failureCount==1,"timeout closes engine admission");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==1&&!w.PagingHead,"late real fence permits retirement");
 check(allocationCalls==before&&!allocated,"timeout path never allocates/frees OS memory");
 reset();memset(&w,0,sizeof(w));budget_ms(&w);PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
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
 reset();budget_ms(&w);
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
  check(WddmSubmitPagingHardwareRoot(&d,&w,data[i],headers[i][0],starts[i],headers[i][1],(BOOLEAN)headers[i][2],41+i,0x120000),"actual builder-to-submit positive path");
 }
 check(w.PagingQueueBorrowed==2 && submitted==1&&!allocated&&allocationCalls==before,"both actual records borrowed while first is pending");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==2&&fences[0]==41&&fences[1]==42&&!w.PagingHead&&!failureCount,"actual builder records retire in order");
 check(submittedBytes[0]==headers[0][3]&&submittedBytes[1]==headers[1][3],"queue preserves actual direct/native command interpretation");
 free(data[0]);free(data[1]);
}
/* KMD172 quota (docs/design/hang-detector.md). Jobs are independent OS buffers admitted through the actual submit path; job 1 is
 * published by its own admission. */
#define QUOTA_JOBS 20
static __declspec(align(8)) unsigned quotaBuffers[QUOTA_JOBS][64];
static BC250_PAGING_JOB* quotaSlots[QUOTA_JOBS];
static void quota_fixture(BC250_WDDM* w,BC250_DEVICE* d,unsigned n) {
 unsigned i,size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,12);
 reset();memset(w,0,sizeof(*w));budget_ms(w);memset(quotaBuffers,0,sizeof(quotaBuffers));
 for(i=0;i<n;i++) {
  quotaBuffers[i][6]=i+1;
  check(PagingPrivateQueuedHeader(quotaBuffers[i],sizeof(quotaBuffers[i]),0,0x10000,12),"quota job construction");
  quotaSlots[i]=PagingPrivateQueueSlot(quotaBuffers[i],size,0x10000,4,TRUE);
  completionSlots[100+i]=quotaSlots[i];
  check(WddmSubmitPagingHardware(d,w,quotaBuffers[i],size,0x10000,4,TRUE,100+i),"quota job admitted");
 }
 check(submitted==1&&w->PagingHwPending&&w->PagingQueueBorrowed==(LONG)n,"one packet in flight, the rest queued");
}
static int quota_fifo(unsigned n) {
 unsigned i;
 if(completionCount!=(int)n)return 0;
 for(i=0;i<n;i++)if(fences[i]!=100+i||submittedBytes[i]!=i+1)return 0;
 return 1;
}
static void quota(void) {
 BC250_WDDM w;BC250_DEVICE d={&w};unsigned rounds;
 // Success path: one caller retires the finished packet and publishes the next. No yield, no requeue.
 quota_fixture(&w,&d,3);
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(quota_fifo(1)&&w.PagingHwPending&&w.PagingHead==quotaSlots[1],"single caller retires one and publishes the next");
 check(drainExit==PagingDrainExitIdle&&!quotaExits&&!w.PagingDrainTimer,"single caller never reaches the quota");
 // The case only the requeue covers: a second caller publishes between the first caller's passes, then
 // stops. The quota exit leaves the ninth job queued with no packet in flight, so no interrupt will come for it.
 quota_fixture(&w,&d,12);helperDevice=&d;helperBudget=PAGING_DRAIN_QUOTA-1;
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(quota_fifo(PAGING_DRAIN_QUOTA)&&drainRetired==PAGING_DRAIN_QUOTA&&helperRuns==PAGING_DRAIN_QUOTA-1,"interleaved callers: the first yields at the quota");
 check(drainExit==PagingDrainExitQuota&&quotaExits==1&&reports==PAGING_DRAIN_QUOTA,"quota exit counted, every retirement reported");
 check(!w.PagingHwPending&&!activeSeq&&w.PagingHead==quotaSlots[PAGING_DRAIN_QUOTA],"head queued, nothing in flight");
 check(w.PagingDrainTimer==1&&w.PagingDrainDpc==-1,"requeue armed for the next clock tick");
 // The timer fires: the drain DPC publishes the head; each modeled interrupt then retires and publishes.
 helperDevice=NULL;WddmPagingDrainDpcRoutine(NULL,&d,NULL,NULL);
 check(w.PagingHwPending&&activeSeq&&completionCount==PAGING_DRAIN_QUOTA&&drainExit==PagingDrainExitIdle,"requeued drain publishes the waiting head");
 for(rounds=0;rounds<40&&w.PagingHead;rounds++){arrived=activeSeq;WddmGpuFencePaging(&d);}
 check(quota_fifo(12)&&!w.PagingHead&&!w.PagingTail&&!w.PagingHwPending&&w.PagingDrainTimer==1,"all jobs retire in FIFO order after the requeue");
 // Sustained contention: the second caller never stops. Each invocation is bounded by the quota; the requeue carries the rest.
 quota_fixture(&w,&d,QUOTA_JOBS);helperDevice=&d;helperBudget=1000;
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(drainRetired==PAGING_DRAIN_QUOTA&&drainExit==PagingDrainExitQuota&&w.PagingDrainTimer==1,"sustained interleaving: first invocation bounded");
 WddmPagingDrainDpcRoutine(NULL,&d,NULL,NULL);
 check(drainRetired==PAGING_DRAIN_QUOTA&&w.PagingDrainTimer==2&&completionCount==2*PAGING_DRAIN_QUOTA,"sustained interleaving: requeued invocation bounded");
 WddmPagingDrainDpcRoutine(NULL,&d,NULL,NULL);
 check(drainRetired==QUOTA_JOBS-2*PAGING_DRAIN_QUOTA&&drainExit==PagingDrainExitIdle&&w.PagingDrainTimer==2,"sustained interleaving: the remainder ends idle");
 check(quota_fifo(QUOTA_JOBS)&&!w.PagingHead&&!w.PagingHwPending&&quotaExits==2,"sustained interleaving keeps FIFO and ownership");
 // Boundary: a stop that lands during the quota's last report. The drain still yields, the requeue is not armed.
 quota_fixture(&w,&d,12);helperDevice=&d;helperBudget=PAGING_DRAIN_QUOTA-1;stopAtReport=PAGING_DRAIN_QUOTA;
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(drainExit==PagingDrainExitQuota&&!w.PagingDrainTimer,"no requeue once stopping");
 TestStopDrain(&w);
 check(!w.PagingHead&&!w.PagingTail&&!quotaSlots[PAGING_DRAIN_QUOTA]->Borrowed,"stop drain releases what the quota left queued");
 reset();
}
int main(void) {
 legacy();borrowed();retention();built_records();quota();
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
