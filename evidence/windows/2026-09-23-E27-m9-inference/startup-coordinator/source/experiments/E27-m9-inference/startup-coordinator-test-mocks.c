
struct _BC250_PSP_FIRMWARE {int owned;};
static struct _BC250_PSP_FIRMWARE fw;
static BC250_DEVICE dev;
static BC250_GFX gfx;
static struct amdgpu_device modelAdev;
static RING ring;
static FUNCS funcs;
static BC250_START_REPORT report;
static unsigned failPhase,readinessGap;
static int geometryFail,geometryChange,captures;
static int planFail,alreadyEnabled,pspLoaded,ihActive,prepareFail,prepared,released,live,quarantine;
static char trace[32];
static unsigned pos;
static void event(char c){trace[pos++]=c;trace[pos]=0;}
static void ready(void){
 gfx.SetUp=1;gfx.StagesDone=8;gfx.PagingReady=1;gfx.PagingWindowReady=1;
 gfx.FencePage=1;gfx.SdmaFencePage=1;gfx.IbPage=1;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=8192;
 gfx.PagingRing=&ring;gfx.PagingDevicePtr=&modelAdev;
 ring.funcs=&funcs;ring.max_dw=1024;modelAdev.sdma.fence_mem.cpu=&modelAdev;
}
static NTSTATUS GartDevice(BC250_DEVICE*d,struct amdgpu_device**a,BOOLEAN*b)
{(void)d;check(mutex==1,"planning lock");*a=&modelAdev;*b=alreadyEnabled;return planFail?STATUS_IO_DEVICE_ERROR:0;}
static NTSTATUS GartCaptureAperture(BC250_DEVICE*d,PAGING_APERTURE*a)
{
 captures++;*a=d->WddmAperture;
 if(geometryChange==captures)a->mc++;
 return geometryFail==captures?STATUS_IO_DEVICE_ERROR:0;
}
static BOOLEAN PspIsLoaded(const BC250_DEVICE*d){(void)d;return pspLoaded;}
static BOOLEAN IhIsActive(const BC250_DEVICE*d){(void)d;return ihActive;}
static NTSTATUS PspPrepareFirmware(BC250_DEVICE*d,BC250_ESCAPE_PSP*p,struct _BC250_PSP_FIRMWARE**f)
{(void)d;(void)p;prepared++;check(pos==0,"firmware before hardware");if(prepareFail)return STATUS_IO_DEVICE_ERROR;*f=&fw;live=1;return 0;}
static void PspReleaseFirmware(struct _BC250_PSP_FIRMWARE*f)
{if(f){check(live==1,"firmware single release");live=0;released++;}}
static NTSTATUS phase(unsigned bit,char c) {
 check(report.Attempted==(report.Completed|bit),"attempt recorded before call");
 check(live==1 && dev.Wddm==NULL,"firmware alive; no admission");
 event(c);return failPhase==bit?STATUS_IO_DEVICE_ERROR:0;
}
static NTSTATUS GartInitializeHardware(BC250_DEVICE*d,BC250_ESCAPE_GART*p)
{(void)d;p->Result=101;return phase(1,'G');}
static NTSTATUS PspInitializePrepared(BC250_DEVICE*d,const struct _BC250_PSP_FIRMWARE*f,BC250_ESCAPE_PSP*p)
{(void)d;check(f==&fw,"prepared snapshot");p->Result=102;return phase(2,'P');}
static NTSTATUS IhInitializeHardware(BC250_DEVICE*d,BC250_ESCAPE_IH*p)
{(void)d;p->Result=103;return phase(4,'I');}
static NTSTATUS GfxInitializeHardware(BC250_DEVICE*d,BC250_ESCAPE_GFX*p)
{NTSTATUS s;(void)d;p->Result=104;s=phase(8,'X');if(NT_SUCCESS(s)){ready();if(readinessGap)gfx.PagingWindowReady=0;}return s;}
static void IhStop(BC250_DEVICE*d){(void)d;event('i');}
static void GfxStop(BC250_DEVICE*d){event('x');if(quarantine)d->GpuStopUnconfirmed=1;}
static void PspStop(BC250_DEVICE*d){(void)d;event('p');}
static void GartStop(BC250_DEVICE*d){(void)d;event('g');}
static void reset(void){
 memset(&dev,0,sizeof(dev));memset(&gfx,0,sizeof(gfx));memset(&report,0xcc,sizeof(report));
 dev.FullWddm=dev.MmioGartEnabled=dev.MmioPspEnabled=dev.MmioIhEnabled=dev.MmioGfxEnabled=1;
 dev.VramWriteEnabled=dev.InterruptIsMessage=1;
 dev.Mmio=dev.Gart=dev.Psp=dev.Ih=dev.GpuMem=&dev;dev.Gfx=&gfx;
 dev.Dxgk.DxgkCbSynchronizeExecution=dev.Dxgk.DxgkCbNotifyInterrupt=&dev;
 dev.Dxgk.DxgkCbQueueDpc=dev.Dxgk.DxgkCbNotifyDpc=&dev;
 dev.WddmAperture.bytes=4096;gfx.SubmitGate=gfx.PagingGate=gfx.PagingCpuBootstrap=1;
 irql=shared=critical=mutex=missingFence=planFail=alreadyEnabled=pspLoaded=ihActive=prepareFail=prepared=released=live=quarantine=0;
 geometryFail=geometryChange=captures=0;
 failPhase=readinessGap=pos=0;trace[0]=0;
}
