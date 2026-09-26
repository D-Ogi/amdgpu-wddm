
static void legacy(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 unsigned a[7]={0},b[7]={0},c[7]={0};
 PagingPrivateHeader(a,sizeof(a),0,0,4);a[6]=11;
 PagingPrivateHeader(b,sizeof(b),0,0,4);b[6]=22;
 PagingPrivateHeader(c,sizeof(c),0,0,4);c[6]=33;
 check(WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0,4,TRUE,11),"first accepted");
 check(submitted==1&&completionCount==0&&allocated==1,"first executing only");
 check(WddmSubmitPagingHardware(&d,&w,b,sizeof(b),0,4,TRUE,12),"second accepted busy");
 check(WddmSubmitPagingHardware(&d,&w,NULL,0,0,0,FALSE,13),"software fence queued");
 check(WddmSubmitPagingHardware(&d,&w,c,sizeof(c),0,4,TRUE,14),"third hardware accepted");
 b[6]=99;c[6]=88;
 check(submitted==1&&completionCount==0&&allocated==4,"busy retains copies and no completions");
 now=100;arrived=1;WddmGpuFencePaging(&d);
 check(submitted==2&&submittedBytes[1]==22,"dispatch second from owned copy");
 check(completionCount==1&&fences[0]==11&&allocated==3,"only first retired");
 // An old timer DPC must not expire the newly dispatched packet.
 WddmPagingSubmitDpcRoutine(NULL,&d,NULL,NULL);
 check(!failureCount&&!w.WatchdogFaulted[1],"old timer does not fault new job");
 arrived=2;WddmGpuFencePaging(&d);
 check(completionCount==3&&fences[1]==12&&fences[2]==13,"software completion stays behind prior GPU");
 check(submitted==3&&submittedBytes[2]==33&&allocated==1,"third hardware copied and dispatched");
 arrived=3;WddmGpuFencePaging(&d);
 check(completionCount==4&&fences[3]==14&&!allocated&&!w.PagingHead&&!w.PagingTail,"all retired FIFO");
 check(!activeSeq&&!w.PagingHwPending&&!failureCount,"ring idle without fault");
 w.Stopping=TRUE;
 check(!WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0,4,TRUE,15)&&!allocated,"closed admission frees CPU copy");

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
int main(void) {
 legacy();borrowed();retention();
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
