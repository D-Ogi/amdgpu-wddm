#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef int NTSTATUS,BOOLEAN,DEVICE_POWER_STATE,POWER_ACTION;
typedef long LONG;
typedef unsigned long ULONG;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define PowerDeviceD0 1
#define PowerDeviceD3 4
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE -1
#define STATUS_INSUFFICIENT_RESOURCES -2
#define STATUS_DEVICE_NOT_READY -3
#define STATUS_GRAPHICS_INVALID_VIDPN -4
#define STATUS_INVALID_PARAMETER -5
#define STATUS_DEVICE_DATA_ERROR -6
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_POWER_TAG 1
#define POOL_FLAG_NON_PAGED 1
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef struct {int dummy;} BC250_ESCAPE_PSP;
struct bc250_clock_report {int ready;};
typedef struct {unsigned Width,Height,Pitch,ColorFormat;struct {long long QuadPart;}PhysicAddress;} DXGK_DISPLAY_INFORMATION;
typedef struct {int Started,FullWddm;volatile LONG RetainedPowerPhase,RuntimeDisplayChanged;struct {int Enabled;volatile LONG Pending;} Connector;DEVICE_POWER_STATE RetainedDownState;POWER_ACTION RetainedDownAction;int Smu;void*Mmio;int GfxTlbBootstrap,ModeActive,SourceVisible;DXGK_DISPLAY_INFORMATION Post;struct {void*DeviceHandle;NTSTATUS(*DxgkCbAcquirePostDisplayOwnership)(void*,DXGK_DISPLAY_INFORMATION*);}Dxgk;} BC250_DEVICE;
static unsigned checks,failures;static char trace[128];static unsigned pos;static char failAt;static int postMismatch,allocFail;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static NTSTATUS note(char c){trace[pos++]=c;trace[pos]=0;return c==failAt?-9:0;}
static int KeGetCurrentIrql(void){return 0;}
static LONG InterlockedCompareExchange(volatile LONG*p,LONG x,LONG c){LONG old=*p;if(old==c)*p=x;return old;}
static LONG InterlockedExchange(volatile LONG*p,LONG x){LONG old=*p;*p=x;return old;}
static void*ExAllocatePool2(int f,size_t n,int t){(void)f;(void)t;return allocFail?NULL:malloc(n);}
static void ExFreePoolWithTag(void*p,int t){(void)t;free(p);}
#define GuardLog(...) ((void)0)
static void StartHealthClose(BC250_DEVICE*d){(void)d;(void)note('H');}
static void SmuOwnerStop(int*s){(void)s;(void)note('S');}
static NTSTATUS WddmSuspendRetained(BC250_DEVICE*d){(void)d;return note('W');}
static NTSTATUS IhSetPowerRetained(BC250_DEVICE*d,int resume){(void)d;return note(resume?'i':'I');}
static NTSTATUS GfxSetPowerRetained(BC250_DEVICE*d,int resume){if(resume)d->GfxTlbBootstrap=0;return note(resume?'g':'G');}
static NTSTATUS PspSetPowerRetained(BC250_DEVICE*d,int resume,BC250_ESCAPE_PSP*r){(void)d;CHECK(r!=NULL);return note(resume?'p':'P');}
static NTSTATUS GartSetPowerRetained(BC250_DEVICE*d,int resume){if(resume)d->GfxTlbBootstrap=1;return note(resume?'t':'T');}
static NTSTATUS SmuOwnerStart(int*s,void*m){(void)s;(void)m;return note('s');}
static NTSTATUS SmuPrepareClock(int*s,struct bc250_clock_report*r){(void)s;r->ready=1;return note('c');}
static NTSTATUS DcnSetVisibility(BC250_DEVICE*d,int on){CHECK(!on);d->SourceVisible=0;return note('b');}
static NTSTATUS ModesetValidateInherited(BC250_DEVICE*d){(void)d;return note('v');}
static int GfxStartupResources(BC250_DEVICE*d,int on){CHECK(on&&!d->GfxTlbBootstrap);return 1;}
static NTSTATUS WddmResumeRetained(BC250_DEVICE*d){CHECK(!d->SourceVisible);return note('w');}
static void StartHealthResumeReady(BC250_DEVICE*d,int mode){CHECK(!d->SourceVisible&&(mode||d->Connector.Pending));(void)note('h');}
static NTSTATUS acquire(void*handle,DXGK_DISPLAY_INFORMATION*p){BC250_DEVICE*d=handle;*p=d->Post;p->Width+=(unsigned)postMismatch;return note('d');}
static BOOLEAN ConnectorIsEnabled(BC250_DEVICE*d){return d->Connector.Enabled;}
static NTSTATUS ConnectorResumeDisplay(BC250_DEVICE*d)
{CHECK(d->RetainedPowerPhase==3 && strchr(trace,'d')!=NULL);return note('r');}
static NTSTATUS ConnectorResumeNotifications(BC250_DEVICE*d)
{CHECK(d->RetainedPowerPhase==0);CHECK(strchr(trace,'w')!=NULL);return note('n');}
/* The display half of the way down (PowerAdapterDown): C connector, L HDMI link, M modes. */
static NTSTATUS ConnectorSuspend(BC250_DEVICE*d){CHECK(d->RetainedPowerPhase==0);return note('C');}
static BOOLEAN HdmiLinkStop(BC250_DEVICE*d){CHECK(d->RetainedPowerPhase==0);return note('L')==0;}
static void ModesetPowerDown(BC250_DEVICE*d){CHECK(d->RetainedPowerPhase==0);(void)note('M');}
/* ACTUAL_SOURCE */
static void resetTrace(void){pos=0;trace[0]=0;failAt=0;}
static void setup(BC250_DEVICE*d){memset(d,0,sizeof(*d));allocFail=0;d->Started=d->FullWddm=d->ModeActive=d->SourceVisible=1;d->Post.Width=1920;d->Post.Height=1200;d->Post.Pitch=7680;d->Post.PhysicAddress.QuadPart=1234;d->Dxgk.DeviceHandle=d;d->Dxgk.DxgkCbAcquirePostDisplayOwnership=acquire;postMismatch=0;resetTrace();}
int main(void){BC250_DEVICE d;
 setup(&d);CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0&&!pos);
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0&&d.RetainedPowerPhase==2&&d.RetainedDownAction==7);
 CHECK(!strcmp(trace,"HSWIGPT"));resetTrace();
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,8)==0&&!pos&&d.RetainedDownAction==7);
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0&&d.RetainedPowerPhase==0&&d.RetainedDownAction==7);
 CHECK(!strcmp(trace,"scdvbtpigwh"));CHECK(!d.SourceVisible);
 setup(&d);CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='g';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==-9&&d.RetainedPowerPhase==4);CHECK(!strchr(trace,'w')&&!strchr(trace,'h'));resetTrace();CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==STATUS_INVALID_DEVICE_STATE&&!pos);
 setup(&d);CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();postMismatch=1;CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==STATUS_GRAPHICS_INVALID_VIDPN&&!strchr(trace,'t'));
 setup(&d);CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='v';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==-9&&d.RetainedPowerPhase==4);
 CHECK(!strchr(trace,'b')&&!strchr(trace,'t')&&!strchr(trace,'w')&&!strchr(trace,'h'));
 // Runtime geometry bypasses all inherited POST acquisition/scaler paths.
 setup(&d);d.Connector.Enabled=1;d.RuntimeDisplayChanged=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();postMismatch=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0);
 CHECK(!strcmp(trace,"scdrbtpigwhn"));
 // Sink change before first runtime commit also forbids stale POST replay.
 setup(&d);d.Connector.Enabled=1;d.Connector.Pending=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0 && !strchr(trace,'v') && strchr(trace,'b'));
 setup(&d);d.Connector.Enabled=1;d.Connector.Pending=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='b';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==-9 && d.RetainedPowerPhase==4);
 CHECK(!strchr(trace,'t') && !strchr(trace,'w') && !strchr(trace,'n'));
 // BD-188: a refused POST ownership at D0 no longer keeps the engines down on the connector path. The sink is
 // restored or found again (r), the output is black (b), and every engine comes back (t p i g w h n).
 setup(&d);d.Connector.Enabled=1;d.RuntimeDisplayChanged=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='d';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0 && d.RetainedPowerPhase==0);
 CHECK(!strcmp(trace,"scdrbtpigwhn") && d.Connector.Pending==1 && !d.ModeActive);
 setup(&d);d.Connector.Enabled=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='d';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0 && d.RetainedPowerPhase==0);
 CHECK(!strchr(trace,'v') && strstr(trace,"drb")!=NULL && strchr(trace,'w') && d.Connector.Pending==1);
 // The static inherited path cannot find a sink again: there a refused POST answer still refuses the D0.
 setup(&d);CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='d';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==-9 && d.RetainedPowerPhase==4 && !strchr(trace,'t'));
 resetTrace();CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==STATUS_INVALID_DEVICE_STATE && !pos);
 setup(&d);d.Connector.Enabled=1;d.RuntimeDisplayChanged=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='r';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==-9 && d.RetainedPowerPhase==4);
 CHECK(!strchr(trace,'t') && !strchr(trace,'n'));
 setup(&d);d.Connector.Enabled=1;d.RuntimeDisplayChanged=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,7)==0);resetTrace();failAt='n';
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==-9 && d.RetainedPowerPhase==4);
 CHECK(strstr(trace,"nHS")!=NULL);
 // BD-188, lab 2026-10-19: a failed display step on the way down must not skip the save of the engines. Before the
 // fix Bc250SetPowerState returned after ConnectorSuspend failed, the phase stayed 0, and the D0 after the
 // hibernation restored nothing, so the first paging job went to an SDMA engine without firmware.
 setup(&d);d.Connector.Enabled=1;
 CHECK(PowerAdapterDown(&d,PowerDeviceD3,3)==0 && d.RetainedPowerPhase==2 && d.RetainedDownAction==3);
 CHECK(!strcmp(trace,"CLMHSWIGPT"));
 setup(&d);d.Connector.Enabled=1;failAt='C';
 CHECK(PowerAdapterDown(&d,PowerDeviceD3,3)==0 && d.RetainedPowerPhase==2);
 CHECK(!strcmp(trace,"CLMHSWIGPT"));resetTrace();
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==0 && d.RetainedPowerPhase==0);
 CHECK(!strcmp(trace,"scdrvbtpigwhn"));
 // The HDMI link restore of the static path: the same rule, and the modes still take their D3 state.
 setup(&d);failAt='L';
 CHECK(PowerAdapterDown(&d,PowerDeviceD3,3)==0 && d.RetainedPowerPhase==2 && !strcmp(trace,"LMHSWIGPT"));
 // A runtime mode has no inherited link or native shape to restore.
 setup(&d);d.Connector.Enabled=1;d.RuntimeDisplayChanged=1;
 CHECK(PowerAdapterDown(&d,PowerDeviceD3,3)==0 && !strcmp(trace,"CHSWIGPT"));
 // An engine failure on the way down is still the answer of the call, and the D0 after it refuses (phase 4).
 setup(&d);d.Connector.Enabled=1;failAt='G';
 CHECK(PowerAdapterDown(&d,PowerDeviceD3,3)==-9 && d.RetainedPowerPhase==4);resetTrace();
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==STATUS_INVALID_DEVICE_STATE && !pos);
 // No report memory on the way down: phase 4, not a phase 0 that a D0 would take for a device still up.
 setup(&d);allocFail=1;
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD3,3)==STATUS_INSUFFICIENT_RESOURCES && d.RetainedPowerPhase==4);
 CHECK(!strcmp(trace,"HS"));allocFail=0;resetTrace();
 CHECK(GpuSetPowerRetained(&d,PowerDeviceD0,99)==STATUS_INVALID_DEVICE_STATE && !pos);
 printf("Power coordinator: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
