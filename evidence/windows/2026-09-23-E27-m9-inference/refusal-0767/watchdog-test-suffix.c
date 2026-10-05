
int main(void) {
 unsigned node;
 for(node=0;node<2;node++) {
  BC250_WDDM w={0};BC250_DEVICE d={&w};
  void (*watch)(KDPC*,PVOID,PVOID,PVOID)=node?WddmPagingSubmitDpcRoutine:WddmSubmitDpcRoutine;
  reports=completions=closed[0]=closed[1]=0;arrived[0]=arrived[1]=0;
  w.HwPending=!node;w.PagingHwPending=!!node;w.HwFence=w.PagingHwFence=7;
  w.HwNode=0;w.HwSeq=w.PagingHwSeq=1;
  w.DeferredValid=w.PagingDeferredValid=1;w.DeferredFence=w.PagingDeferredFence=8;
  watch(0,&d,0,0);
  CHECK(completions==0 && reports==0 && closed[node]==1);
  CHECK(node?w.PagingHwPending:w.HwPending);CHECK(w.WatchdogFaulted[node]);
  WddmCompleteSoftware(&d,9,node);CHECK(completions==0 && reports==0);
  CHECK(node?!w.PagingDeferredValid:!w.DeferredValid);
  watch(0,&d,0,0);CHECK(closed[node]==1); // idempotent timeout
  // Actual late arrival may retire ONLY the submitted hardware fence.
  arrived[node]=1;watch(0,&d,0,0);
  CHECK(completions==1 && lastFence==7 && lastNode==node && reports==1);
  CHECK(node?!w.PagingHwPending:!w.HwPending);
  WddmCompleteSoftware(&d,10,node);CHECK(completions==1);
  // Positive control: before deadline an arrived fence completes normally.
  memset(&w,0,sizeof(w));reports=completions=closed[0]=closed[1]=0;
  w.HwPending=!node;w.PagingHwPending=!!node;w.HwFence=w.PagingHwFence=11;
  watch(0,&d,0,0);CHECK(completions==1 && lastFence==11 && !closed[node] && !w.WatchdogFaulted[node]);
 }
 puts("PASS: both watchdogs keep pending work, emit no fake completion, block later software fences, accept only actual late arrival, and preserve normal completion");
 return 0;
}
