
int main(void){
 BC250_WDDM w;BC250_DEVICE d={&w};unsigned node;
 CHECK(watchdog_controls()==0);
 for(node=0;node<2;node++){
  memset(&w,0,sizeof(w));reports=completions=0;closed[0]=closed[1]=0;arrived[0]=arrived[1]=0;
  if(node){w.PagingHwPending=1;w.PagingHwFence=10;w.PagingHwSeq=3;w.PagingDeferredValid=1;w.PagingDeferredFence=11;}
  else{w.HwPending=1;w.HwFence=10;w.HwSeq=3;w.DeferredValid=1;w.DeferredFence=11;}
  WddmFailSubmission(&d,12,node);
  CHECK(!completions && !reports && w.RefusalPending[node] && w.WatchdogFaulted[node] && closed[node]==1);
  CHECK(!w.DeferredValid && !w.PagingDeferredValid);
  WddmCompleteSoftware(&d,13,node);CHECK(!completions && !reports);
  arrived[node]=1;
  if(node)WddmGpuFencePaging(&d);else WddmGpuFence(&d);
  CHECK(completions==1 && lastFence==10 && lastNode==node);
  WddmCompleteSoftware(&d,14,node);CHECK(completions==1 && w.RefusalPending[node]);
 }
 memset(&w,0,sizeof(w));reports=completions=0;
 w.HwPending=w.PagingHwPending=1;w.HwFence=101;w.PagingHwFence=202;
 CHECK(Bc250WddmResetFromTimeout(&d)==STATUS_UNSUCCESSFUL);
 CHECK(w.HwPending && w.PagingHwPending && w.HwFence==101 && w.PagingHwFence==202 && !reports && !completions);
 puts("PASS: refusals retain outstanding work, block software retirement, preserve actual predecessor fence, and unverified reset fails without changing pending fences");return 0;
}
