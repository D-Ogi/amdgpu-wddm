#include <stdio.h>
#include <string.h>
typedef int NTSTATUS,BOOLEAN;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define PAGE_SIZE 4096
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_INVALID_DEVICE_STATE -2
#define STATUS_DEVICE_DATA_ERROR -3
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_IO_TIMEOUT -5
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_GART_WINDOW 0x200000ull
#define BC250_GART_SCRATCH_OFFSET 0x100000ull
#define AMDGPU_MMHUB0(x) (1+(x))
typedef struct {int Fault;} SEQUENCE;
struct amdgpu_device {void*backend;unsigned long long ownerCookie;};
typedef struct {int PowerSuspended,Enabled,SetUp;SEQUENCE Sequence;struct amdgpu_device Adev;} BC250_GART;
typedef struct {BC250_GART*Gart;int GpuStopUnconfirmed,IhQuiet,GfxTlbBootstrap,MmioGartEnabled,GartLock;unsigned long long VramLength;} BC250_DEVICE;
static unsigned checks,failures,disables,rebuilds,configs,flushes,zeroes,order;
static int locked,gfxSuspended=1,pspSuspended=1,failPhase;
static BC250_GART*observed;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static int KeGetCurrentIrql(void){return 0;}
static void ExAcquireFastMutex(int*l){(void)l;CHECK(!locked);locked=1;}
static void ExReleaseFastMutex(int*l){(void)l;CHECK(locked);locked=0;}
static int GfxPowerIsSuspended(BC250_DEVICE*d){(void)d;return gfxSuspended;}
static int PspPowerIsSuspended(BC250_DEVICE*d){(void)d;return pspSuspended;}
static NTSTATUS CheckWindow(BC250_DEVICE*d){(void)d;return 0;}
static NTSTATUS ZeroVram(BC250_DEVICE*d,unsigned long long off,unsigned bytes){CHECK(off==d->VramLength-BC250_GART_WINDOW+BC250_GART_SCRATCH_OFFSET&&bytes==PAGE_SIZE);zeroes++;CHECK(order==0);order=1;return 0;}
static int GpuMemRebuildRetainedGtt(struct amdgpu_device*a){CHECK(a==&observed->Adev&&order==1);order=2;rebuilds++;return failPhase==1?-1:0;}
static void bc250_gmc_gart_disable(struct amdgpu_device*a){CHECK(a==&observed->Adev);disables++;}
static int bc250_gmc_gart_configure_observed(struct amdgpu_device*a,void*observer){CHECK(!observer&&a==&observed->Adev&&order==2);order=3;configs++;return failPhase==2?-1:0;}
static int bc250_gmc_flush_gpu_tlb(struct amdgpu_device*a,int vmid,int hub,int type){CHECK(a==&observed->Adev&&vmid==0&&hub==1&&type==0&&order==3);order=4;flushes++;return failPhase==3?-1:0;}
static void SequenceBegin(SEQUENCE*s,BC250_DEVICE*d,int plan,void*w,int n){(void)d;CHECK(locked&&!plan&&!w&&!n);s->Fault=0;}
/* ACTUAL_SOURCE */
static void setup(BC250_DEVICE*d,BC250_GART*g){memset(d,0,sizeof(*d));memset(g,0,sizeof(*g));d->Gart=g;d->IhQuiet=d->MmioGartEnabled=1;d->VramLength=8ull<<30;g->SetUp=g->Enabled=1;g->Adev.backend=d;g->Adev.ownerCookie=0x123456789abcdef0ull;observed=g;disables=rebuilds=configs=flushes=zeroes=order=0;failPhase=0;gfxSuspended=pspSuspended=1;}
int main(void){BC250_DEVICE d;BC250_GART g;unsigned n;
 setup(&d,&g);CHECK(GartSetPowerRetained(&d,FALSE)==0 && g.PowerSuspended&&!g.Enabled&&disables==1);CHECK(g.Adev.backend==&d&&g.SetUp);
 CHECK(GartSetPowerRetained(&d,FALSE)==0&&disables==1);
 CHECK(GartSetPowerRetained(&d,TRUE)==0&&g.Enabled&&!g.PowerSuspended&&d.GfxTlbBootstrap);
 CHECK(rebuilds==1&&configs==1&&flushes==1&&zeroes==1&&g.Adev.backend==&d&&g.Adev.ownerCookie==0x123456789abcdef0ull);
 setup(&d,&g);gfxSuspended=0;CHECK(GartSetPowerRetained(&d,FALSE)==STATUS_DEVICE_NOT_READY&&!disables);
 setup(&d,&g);pspSuspended=0;CHECK(GartSetPowerRetained(&d,FALSE)==STATUS_DEVICE_NOT_READY&&!disables);
 setup(&d,&g);d.IhQuiet=0;CHECK(GartSetPowerRetained(&d,FALSE)==STATUS_DEVICE_NOT_READY&&!disables);
 for(n=1;n<=3;n++){setup(&d,&g);CHECK(GartSetPowerRetained(&d,FALSE)==0);failPhase=(int)n;CHECK(GartSetPowerRetained(&d,TRUE)<0&&g.PowerSuspended&&g.SetUp&&g.Adev.backend==&d);CHECK(g.Adev.ownerCookie==0x123456789abcdef0ull);}
 printf("Retained GART: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
