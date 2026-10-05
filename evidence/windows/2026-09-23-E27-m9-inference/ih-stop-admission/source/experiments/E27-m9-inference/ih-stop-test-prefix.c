#include <stdio.h>
#include <string.h>
typedef int BOOLEAN;
typedef long NTSTATUS;
typedef void* PVOID;
typedef long LONG;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_DEVICE_NOT_READY (-1L)
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef struct {int owner;} BC250_SEQUENCE;
struct amdgpu_device {void*backend;};
typedef struct {LONG Active;int SetUp,Enabled;BC250_SEQUENCE Sequence;} BC250_IH;
typedef struct {
 BC250_IH*Ih;int IhQuiet,GpuStopUnconfirmed,GartLock;
 struct {PVOID DeviceHandle;NTSTATUS (*DxgkCbSynchronizeExecution)(PVOID,BOOLEAN (*)(PVOID),PVOID,unsigned,BOOLEAN*);} Dxgk;
} BC250_DEVICE;
static BC250_DEVICE device;
static BC250_IH modelIh;
static struct amdgpu_device modelAdev;
static int checks,failures,mode,halt,lookup,lateIsr,queued,flushed,teardown,released,hw,mutex,syncs;
static void check(int ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
static LONG InterlockedExchange(LONG*p,LONG v){LONG old=*p;*p=v;return old;}
static void ExAcquireFastMutex(int*p){(void)p;mutex++;}
static void ExReleaseFastMutex(int*p){(void)p;mutex--;}
static void KeFlushQueuedDpcs(void){check(modelIh.Active==0,"closed before flush");queued=0;flushed++;}
static void bc250_ih_hw_fini(struct amdgpu_device*a){
 (void)a;hw++;
 check(!lateIsr&&!queued&&flushed,"ISR joined and DPC drained before hardware teardown");
}
static BOOLEAN RingReadsDisabled(const BC250_DEVICE*d){(void)d;return halt;}
static void bc250_ih_teardown(struct amdgpu_device*a){(void)a;teardown++;check(halt&&!lateIsr&&!queued,"storage retired after halt/drain");}
static void GpuMemRelease(BC250_DEVICE*d,const void*owner,BOOLEAN quiet){
 (void)d;check(owner==&modelIh.Sequence&&quiet&&teardown,"release correct retired owner");released++;
}
static NTSTATUS GartDevice(BC250_DEVICE*d,struct amdgpu_device**a,BOOLEAN*b)
{(void)d;*a=&modelAdev;*b=TRUE;return lookup?0:-3;}
static void SequenceBegin(BC250_SEQUENCE*s,BC250_DEVICE*d,BOOLEAN p,void*w,unsigned n)
{(void)s;(void)d;(void)p;(void)w;(void)n;}
static void GpuMemBeginSequence(BC250_DEVICE*d,void*w,unsigned n){(void)d;(void)w;(void)n;}
static NTSTATUS synchronize(PVOID h,BOOLEAN (*callback)(PVOID),PVOID c,unsigned message,BOOLEAN*r){
 (void)h;syncs++;check(message==0,"message zero");
 if(mode==1)return -7;
 if(mode==2){*r=TRUE;return 0;}
 // An ISR already observed Active before stop. Its queue operation must finish
 // before the synchronization callback, even though Active will be cleared.
 if(lateIsr){queued++;lateIsr=0;}
 *r=callback(c);
 if(mode==3)*r=FALSE;
 if(mode==4)return -7;
 return 0;
}
