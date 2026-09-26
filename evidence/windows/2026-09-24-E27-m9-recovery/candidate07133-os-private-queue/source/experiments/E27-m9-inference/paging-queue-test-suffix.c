
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
