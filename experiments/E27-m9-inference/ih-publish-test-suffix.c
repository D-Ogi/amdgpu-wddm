
static BOOLEAN queue(PVOID h){(void)h;queued++;return TRUE;}
static NTSTATUS syncCall(PVOID h,BOOLEAN (*cb)(PVOID),PVOID c,ULONG message,BOOLEAN*r){
 (void)h;check(!atDirql&&message==0,"sync message");
 if(mode==1)return -7;
 if(mode==2){*r=TRUE;return 0;}
 atDirql=1;*r=cb(c);atDirql=0;
 // Earliest ISR delivery, immediately after the synchronized transition.
 if(modelIh.Active)check(IhInterrupt(&dev)&&queued==1,"immediate interrupt queues prepared consumer");
 if(mode==3)*r=FALSE;
 if(mode==4)return -7;
 return 0;
}
static void reset(void){
 memset(&modelIh,0,sizeof(modelIh));memset(&adev,0,sizeof(adev));memset(&dpc,0,sizeof(dpc));memset(&dev,0,sizeof(dev));
 memset(caller,0,sizeof(caller));caller[1].Value=99;
 modelIh.DpcAdev=&dpc;modelIh.Stats=17;modelIh.Rptr=64;modelIh.Sequence.Writes=caller;modelIh.Sequence.MaxWrites=4;modelIh.Sequence.WriteCount=1;
 modelIh.DpcSequence.Dpc=TRUE;adev.backend=&modelIh.Sequence;adev.ringPrepared=1;dev.Ih=&modelIh;
 dev.Dxgk.DxgkCbSynchronizeExecution=syncCall;dev.Dxgk.DxgkCbQueueDpc=queue;
 locked=atDirql=mode=queued=enableCalls=shimResult=0;ioFault=0;
}
int main(void){
 NTSTATUS s;long result;int i;
 reset();s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
 check(s==0&&result==0&&modelIh.Active&&queued==1,"successful activation");
 check(adev.backend==&modelIh.Sequence&&caller[1].Value==123&&modelIh.Sequence.WriteCount==2,"report merge after callback");
 for(i=1;i<=4;i++){
  reset();mode=i;s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
  check(s==((i==1||i==4)?-7:STATUS_IO_DEVICE_ERROR),"native/undelivered/refused status");
  check(adev.backend==&modelIh.Sequence&&!atDirql&&!locked,"restore backend on failure");
  if(i<=2)check(!enableCalls&&!modelIh.Active&&caller[1].Value==99,"no callback means no hardware");
 }
 reset();ioFault=-8;s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
 check(s==-8&&!modelIh.Active&&!queued&&modelIh.Sequence.Fault==-8&&modelIh.Sequence.FaultOffset==88,"MMIO failure deactivates and propagates");
 reset();shimResult=-9;s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
 check(s==STATUS_IO_DEVICE_ERROR&&result==-9&&!modelIh.Active&&!queued,"shim failure deactivates");
 reset();modelIh.Sequence.MaxWrites=1;s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
 check(s==0&&caller[1].Value==99&&modelIh.Sequence.WriteCount==2,"full caller log preserves capacity");
 reset();modelIh.Sequence.Writes=NULL;modelIh.Sequence.MaxWrites=0;s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
 check(s==0&&modelIh.Sequence.WriteCount==2,"optional caller log");
 for(i=0;i<3;i++){
  reset();if(i==0)dev.Dxgk.DxgkCbSynchronizeExecution=NULL;
  if(i==1)dev.Dxgk.DxgkCbQueueDpc=NULL;
  if(i==2)modelIh.DpcAdev=NULL;
  s=IhPublishAndEnable(&dev,&modelIh,&adev,&result);
  check(s==STATUS_DEVICE_NOT_READY&&!enableCalls&&!modelIh.Active,"missing prerequisite refuses activation");
 }
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
