
int main(void){
 unsigned i,q;NTSTATUS s;
 reset();check(GpuStartupInitialize(&dev,NULL)==STATUS_INVALID_PARAMETER,"null report");
 check(GpuStartupInitialize(NULL,&report)==STATUS_INVALID_PARAMETER,"null device");
 for(i=0;i<29;i++){
  reset();
  switch(i){
  case 0:irql=1;break;case 1:dev.Started=1;break;case 2:dev.Wddm=&dev;break;
  case 3:dev.GpuStopUnconfirmed=1;break;case 4:dev.FullWddm=0;break;
  case 5:dev.Mmio=NULL;break;case 6:dev.MmioGartEnabled=0;break;
  case 7:dev.MmioPspEnabled=0;break;case 8:dev.MmioIhEnabled=0;break;
  case 9:dev.MmioGfxEnabled=0;break;case 10:dev.VramWriteEnabled=0;break;
  case 11:dev.Gart=NULL;break;case 12:dev.Psp=NULL;break;case 13:dev.Ih=NULL;break;
  case 14:dev.GpuMem=NULL;break;case 15:dev.InterruptIsMessage=0;break;
  case 16:dev.Dxgk.DxgkCbSynchronizeExecution=NULL;break;
  case 17:dev.Dxgk.DxgkCbNotifyInterrupt=NULL;break;case 18:dev.Dxgk.DxgkCbQueueDpc=NULL;break;
  case 19:dev.Dxgk.DxgkCbNotifyDpc=NULL;break;case 20:dev.WddmAperture.bytes=0;break;
  case 21:dev.Gfx=NULL;break;case 22:gfx.SubmitGate=0;break;case 23:gfx.PagingGate=0;break;
  case 24:planFail=1;break;case 25:alreadyEnabled=1;break;case 26:pspLoaded=1;break;
  case 27:ihActive=1;break;case 28:prepareFail=1;break;
  }
  s=GpuStartupInitialize(&dev,&report);
  check(!NT_SUCCESS(s)&&report.Status==s&&!report.Ready&&!report.Attempted&&!report.Unwound,"preflight refusal");
  check(!pos&&!live&&!released&&!shared&&!critical&&!mutex,"preflight no hardware or leaks");
 }
 for(i=1;i<=3;i++){
  reset();syncMode=(int)i;s=GpuStartupInitialize(&dev,&report);
  check(s==(i==1?STATUS_IO_DEVICE_ERROR:STATUS_DEVICE_NOT_READY),"synchronization failure preserved");
  check(syncCalls==1&&!pos&&!prepared&&!live&&!report.Ready&&!report.Unwound,"no hardware after disconnected interrupt");
 }
 for(i=0;i<4;i++)for(q=0;q<2;q++){
  static const char*expected[]={"Gixpg","GPixpg","GPIixpg","GPIXixpg"};
  reset();failPhase=1u<<i;quarantine=(int)q;
  s=GpuStartupInitialize(&dev,&report);
  check(s==STATUS_IO_DEVICE_ERROR&&report.Status==s,"preserve phase failure");
  check(report.Attempted==(2u*failPhase-1)&&report.Completed==failPhase-1,"partial progress");
  check(!report.Ready&&report.Unwound&&report.StopUnconfirmed==(int)q,"unwind verdict");
  check(!strcmp(trace,expected[i])&&!live&&released==1,"all stops ordered and firmware released");
  check(report.Gart.Result==101,"detailed report retained");
 }
 reset();readinessGap=1;
 check(GpuStartupInitialize(&dev,&report)==STATUS_DEVICE_NOT_READY,"no admission without system paging window");
 check(report.Attempted==15&&report.Completed==15&&!report.Ready&&report.Unwound,"stage success not readiness");
 check(!strcmp(trace,"GPIXixpg")&&!live&&released==1,"readiness failure unwind");
 reset();
 check(GpuStartupInitialize(&dev,&report)==0,"successful control");
 check(report.InterruptConnected&&report.Ready&&!report.Unwound&&report.Attempted==15&&report.Completed==15,"complete readiness");
 check(!strcmp(trace,"GPIX")&&!live&&released==1&&dev.Wddm==NULL,"success retains hardware; caller admits");
 check(GpuStartupInitialize(&dev,&report)==STATUS_DEVICE_NOT_READY&&pos==4,"duplicate call refuses before writes");
 for(i=1;i<=2;i++)for(q=0;q<2;q++){
  reset();if(q)geometryFail=(int)i;else geometryChange=(int)i;
  s=GpuStartupInitialize(&dev,&report);
  check(s==(q?STATUS_IO_DEVICE_ERROR:STATUS_INVALID_DEVICE_STATE)&&!report.Ready,"geometry refusal");
  check(!live&&(i==1?!pos:!strcmp(trace,"GPIXixpg")),"geometry failure before writes or complete unwind");
 }
 for(i=0;i<24;i++){
  reset();ready();
  switch(i){
  case 0:gfx.SubmitGate=0;break;case 1:gfx.PagingGate=0;break;
  case 2:gfx.Failed=1;break;case 3:gfx.SubmitFailed=1;break;case 4:gfx.PagingSubmitFailed=1;break;
  case 5:gfx.SubmitInFlight=1;break;case 6:gfx.PagingSubmitInFlight=1;break;
  case 7:gfx.SetUp=0;break;case 8:gfx.StagesDone=7;break;case 9:gfx.PagingReady=0;break;
  case 10:gfx.PagingWindowReady=0;break;case 11:gfx.PagingRing=NULL;break;
  case 12:ring.funcs=NULL;break;case 13:ring.max_dw=0;break;case 14:gfx.PagingDevicePtr=NULL;break;
  case 15:gfx.FencePage=0;break;case 16:gfx.SdmaFencePage=0;break;case 17:gfx.IbPage=0;break;
  case 18:gfx.PagingCopyStaging.size=4095;break;case 19:gfx.PagingCopyStaging.mc=0;break;
  case 20:modelAdev.sdma.fence_mem.cpu=NULL;break;case 21:missingFence=4;break;
  case 22:missingFence=5;break;case 23:irql=1;break;
  }
  check(!GfxStartupResources(&dev,TRUE),"missing resource refuses readiness");
  check(!shared&&!critical,"readiness releases lifetime lock");
 }
 reset();gfx.PagingCpuBootstrap=0;check(!GfxStartupResources(&dev,FALSE),"bootstrap closed refuses cold start");
 reset();gfx.StagesDone=1;check(!GfxStartupResources(&dev,FALSE),"partial prior stage refuses cold start");
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
