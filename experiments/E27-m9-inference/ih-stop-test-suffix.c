
static void reset(void){
 memset(&device,0,sizeof(device));memset(&modelIh,0,sizeof(modelIh));memset(&modelAdev,0,sizeof(modelAdev));
 device.Ih=&modelIh;device.IhQuiet=TRUE;device.Dxgk.DxgkCbSynchronizeExecution=synchronize;
 modelIh.Active=1;modelIh.SetUp=modelIh.Enabled=1;modelAdev.backend=&device;
 mode=0;halt=lookup=lateIsr=1;queued=flushed=teardown=released=hw=mutex=syncs=0;
}
int main(void){
 int m,h,l;
 for(m=0;m<5;m++)for(h=0;h<2;h++)for(l=0;l<2;l++){
  reset();mode=m;halt=h;lookup=l;IhStop(&device);
  check(modelIh.Active==0&&flushed&&!mutex,"stop closes/drains/unlocks");
  check(syncs==1,"stop synchronizes exactly once");
  if(!m&&h&&l){
   check(device.IhQuiet&&!device.GpuStopUnconfirmed&&teardown==1&&released==1,"successful retirement");
   check(!modelIh.SetUp&&!modelIh.Enabled,"clear owner after success");
  }else{
   check(!device.IhQuiet&&device.GpuStopUnconfirmed&&!teardown&&!released,"failure retains backing and quarantines");
   check(modelIh.SetUp&&modelIh.Enabled,"failure retains complete owner description");
  }
  check(modelAdev.backend==&device,"restore backend");
  if(m||!l)check(!hw,"no hardware teardown without verified ISR exclusion and device");
 }
 reset();device.Dxgk.DxgkCbSynchronizeExecution=NULL;IhStop(&device);
 check(!modelIh.Active&&!teardown&&!released&&device.GpuStopUnconfirmed,"missing synchronization retains");
 reset();modelIh.Active=modelIh.SetUp=modelIh.Enabled=lateIsr=0;device.Dxgk.DxgkCbSynchronizeExecution=NULL;
 IhStop(&device);check(!syncs&&!hw&&!teardown&&flushed&&device.IhQuiet,"never initialized stop");
 reset();device.Ih=NULL;IhStop(&device);check(!syncs&&!hw&&!flushed&&!mutex,"null object stop");
 reset();halt=0;modelAdev.backend=&modelIh.Sequence;
 check(!Fini(&device,&modelIh,&modelAdev)&&modelIh.SetUp&&device.GpuStopUnconfirmed,"manual FINI failure retained");
 halt=1;check(Fini(&device,&modelIh,&modelAdev)&&released==1&&device.GpuStopUnconfirmed,"later verified halt retains sticky quarantine");
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
