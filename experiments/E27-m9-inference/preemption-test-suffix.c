
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void) {
 BC250_WDDM w={0}; BC250_DEVICE d={&w};
 w.PreemptionPending[0]=1;w.PreemptionFence[0]=7;w.HwPending=1;
 WddmReportDpcRoutine(0,&d,0,0); CHECK(nr==0 && w.PreemptionPending[0]);
 w.HwPending=0;w.CompletionPending[0]=1;w.SubmittedFence[0]=101;
 WddmReportDpcRoutine(0,&d,0,0); CHECK(nr==2 && reports[0].InterruptType==1 && reports[1].DmaPreempted.LastCompletedFenceId==101 && !w.PreemptionPending[0]);
 memset(&w,0,sizeof(w));nr=0;w.PreemptionPending[0]=1;w.ActiveSubmissions[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.PreemptionPending[0]);
 w.ActiveSubmissions[0]=0;WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1);
 memset(&w,0,sizeof(w));nr=0;w.PreemptionPending[0]=1;w.CompletionPending[0]=1;w.SubmittedFence[0]=101;inject=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && w.PreemptionPending[0]);
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==3 && reports[2].DmaPreempted.LastCompletedFenceId==102);
 memset(&w,0,sizeof(w));nr=0;w.LastCompletedFence=999;w.LastReportedFence[1]=22;w.PreemptionPending[1]=1;w.PagingHwPending=1;w.PagingHead=&w;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0);
 w.PagingHwPending=0;w.PagingHead=NULL;WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && reports[0].DmaPreempted.LastCompletedFenceId==22);
 memset(&w,0,sizeof(w));nr=0;queued=0;w.ReportActive=1;w.PreemptionPending[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.ReportAgain && w.PreemptionPending[0]);
 w.ReportActive=0;WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && queued==1 && !w.ReportAgain);

 /* A rejected fence must not retire an earlier executing packet, on either node.
    A real late fence after a watchdog still makes that dependency complete. */
 for (unsigned node=0;node<2;node++) {
  memset(&w,0,sizeof(w));nr=0;
  w.RejectedPending[node]=1;w.RejectedFence[node]=102;
  w.PreemptionPending[node]=1;w.PreemptionFence[node]=8;
  if(node) {w.PagingHwPending=1;w.PagingHead=&w;}else w.HwPending=1;
  WddmReportDpcRoutine(0,&d,0,0);
  CHECK(nr==0 && w.RejectedPending[node] && !w.LastReportedValid[node]);
  w.WatchdogFaulted[node]=1;
  WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.RejectedPending[node]);
  w.HwPending=w.PagingHwPending=0;w.PagingHead=NULL;
  w.CompletionPending[node]=1;w.SubmittedFence[node]=101;
  WddmReportDpcRoutine(0,&d,0,0);
  CHECK(nr==2 && reports[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED);
  CHECK(reports[0].DmaCompleted.SubmissionFenceId==101);
  CHECK(reports[1].InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED);
  CHECK(reports[1].DmaPreempted.LastCompletedFenceId==102);
  CHECK(w.LastReportedValid[node] && !w.RejectedPending[node]);
 }
 /* Active submit and a completion arriving during publication both defer retirement. */
 memset(&w,0,sizeof(w));nr=0;w.RejectedPending[0]=1;w.RejectedFence[0]=103;
 w.ActiveSubmissions[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.RejectedPending[0]);
 w.ActiveSubmissions[0]=0;w.CompletionPending[0]=1;w.SubmittedFence[0]=101;inject=1;
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==1 && w.RejectedPending[0] && w.LastReportedFence[0]==101);
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==2 && reports[1].DmaCompleted.SubmissionFenceId==102);
 CHECK(w.LastReportedFence[0]==103 && !w.RejectedPending[0]);
 /* No fabricated interrupt for a rejected first packet, including high-bit IDs. */
 memset(&w,0,sizeof(w));nr=0;w.RejectedPending[0]=1;w.RejectedFence[0]=0x80000001U;
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==0 && (UINT)w.LastReportedFence[0]==0x80000001U && w.LastReportedValid[0]);
 /* A later completed packet subsumes rejection; an old rejected fence cannot regress it. */
 memset(&w,0,sizeof(w));nr=0;w.RejectedPending[0]=1;w.RejectedFence[0]=101;
 w.CompletionPending[0]=1;w.SubmittedFence[0]=102;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && w.LastReportedFence[0]==102);
 /* Normal 32-bit fence wrap, with an established predecessor. */
 memset(&w,0,sizeof(w));nr=0;w.LastReportedValid[0]=1;w.LastReportedFence[0]=(LONG)0xfffffffeU;
 w.RejectedPending[0]=1;w.RejectedFence[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.LastReportedFence[0]==1);

 memset(&w,0,sizeof(w));nr=0;
 w.RefusalPending[0]=1;w.WatchdogFaulted[0]=1;
 w.PreemptionPending[0]=1;w.RejectedPending[0]=1;w.RejectedFence[0]=304;
 w.CompletionPending[0]=1;w.SubmittedFence[0]=302;
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==1 && reports[0].DmaCompleted.SubmissionFenceId==302);
 CHECK(w.PreemptionPending[0] && w.RejectedPending[0] && w.LastReportedFence[0]==302);
 puts("PASS: six preemption controls; rejection ordering on both nodes, watchdog/late fence, active submit, publication race, high-bit initial fence, no regression, wrap");return 0;
}
