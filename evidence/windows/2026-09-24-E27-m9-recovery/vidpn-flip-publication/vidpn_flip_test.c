// Deterministic callback interleavings of actual source, not a multicore test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int32_t NTSTATUS;
typedef long LONG;
typedef unsigned long ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int BOOLEAN;
typedef void *HANDLE;
typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define NT_SUCCESS(s) ((s)>=0)
#define DXGK_INTERRUPT_CRTC_VSYNC 1
#define WddmDdiSetVidPnSourceAddress 0
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
typedef struct {
    unsigned VidPnSourceId, PrimarySegment;
    PHYSICAL_ADDRESS PrimaryAddress;
    struct { unsigned Value; } Flags;
} DXGKARG_SETVIDPNSOURCEADDRESS;
typedef struct {
    unsigned InterruptType;
    struct { unsigned VidPnTargetId, PhysicalAdapterMask; PHYSICAL_ADDRESS PhysicalAddress; } CrtcVsync;
} DXGKARGCB_NOTIFY_INTERRUPT_DATA;
typedef struct {
    PHYSICAL_ADDRESS PrimaryAddress;
    volatile LONG PrimarySequence;
    unsigned PrimarySegment;
    volatile LONG Flips, FlipsAboveDispatch, VSyncReports;
    int VSyncEnabled;
    unsigned VSyncTargetId;
} BC250_WDDM;
typedef struct {
    BC250_WDDM *Wddm;
    int VidPnFlipEnabled;
    volatile LONG DcnVsyncAcked, DcnVsyncDeferred;
} BC250_DEVICE;
static unsigned checks,failures,hardware,arms,reports;
static int irq,pending,inject_update,nested_writer;
static NTSTATUS hardware_result;
static LONGLONG programmed,reported;
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);} } while(0)
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG compare){LONG old=*p;if(old==compare)*p=value;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG value){LONG old=*p;*p=value;return old;}
static LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p,LONGLONG value,LONGLONG compare){LONGLONG old=*p;if(old==compare)*p=value;return old;}
static LONGLONG InterlockedExchange64(volatile LONGLONG *p,LONGLONG value){LONGLONG old=*p;*p=value;return old;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static int KeGetCurrentIrql(void){return irq;}
static BC250_WDDM *WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static int WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return 0;}
static void WddmVSyncArm(BC250_DEVICE *d,int on){(void)d;CHECK(on && irq<=DISPATCH_LEVEL);++arms;}
static int WddmStopping(BC250_WDDM *w){(void)w;return 0;}
static void WddmReport(BC250_DEVICE *d,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data){(void)d;reports++;reported=data->CrtcVsync.PhysicalAddress.QuadPart;}
static BOOLEAN WddmReadCompletedPrimary(BC250_DEVICE*,BC250_WDDM*,PHYSICAL_ADDRESS*);
static NTSTATUS Bc250WddmSetVidPnSourceAddress(const HANDLE,const DXGKARG_SETVIDPNSOURCEADDRESS*);
void WddmDcnVsync(BC250_DEVICE*);
static NTSTATUS DcnFlipSourceAddress(BC250_DEVICE *d,ULONGLONG address,ULONGLONG *out)
{
    PHYSICAL_ADDRESS sample={-1};
    unsigned before=reports;
    DXGKARG_SETVIDPNSOURCEADDRESS other={0};
    (void)out;
    hardware++;
    CHECK(!WddmReadCompletedPrimary(d,d->Wddm,&sample));
    CHECK(sample.QuadPart==-1);
    d->DcnVsyncAcked=1;
    WddmDcnVsync(d);
    CHECK(reports==before);
    if(nested_writer){
        nested_writer=0;
        other.PrimaryAddress.QuadPart=99;
        CHECK(Bc250WddmSetVidPnSourceAddress(d,&other)==STATUS_DEVICE_BUSY);
    }
    if(NT_SUCCESS(hardware_result))programmed=(LONGLONG)address;
    return hardware_result;
}
static BOOLEAN DcnFlipPending(BC250_DEVICE *d)
{
    if(inject_update){
        DXGKARG_SETVIDPNSOURCEADDRESS next={0};
        inject_update=0;
        next.PrimaryAddress.QuadPart=30;next.PrimarySegment=3;
        CHECK(Bc250WddmSetVidPnSourceAddress(d,&next)==STATUS_SUCCESS);
    }
    return pending;
}
/* ACTUAL_SOURCE */
int main(void)
{
    int level;
    for(level=0;level<2;level++){
        BC250_WDDM w={0};BC250_DEVICE d={0};
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        PHYSICAL_ADDRESS sample={-1};
        d.Wddm=&w;d.VidPnFlipEnabled=1;w.VSyncEnabled=1;
        w.PrimaryAddress.QuadPart=10;w.PrimarySegment=1;
        request.PrimaryAddress.QuadPart=20;request.PrimarySegment=2;
        irq=level?3:0;hardware=arms=reports=0;pending=inject_update=nested_writer=0;
        hardware_result=STATUS_IO_TIMEOUT;programmed=10;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
        CHECK(w.PrimaryAddress.QuadPart==10 && w.PrimarySegment==1 && w.Flips==0);
        CHECK(programmed==10 && hardware==1 && arms==0 && !(w.PrimarySequence&1));
        d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==1 && reported==10);
        // Same address must reach hardware on retry, then be deduplicated only on success.
        hardware_result=STATUS_SUCCESS;nested_writer=1;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
        CHECK(programmed==20 && w.PrimaryAddress.QuadPart==20 && w.PrimarySegment==2);
        CHECK(w.Flips==1 && hardware==2 && !(w.PrimarySequence&1));
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
        CHECK(hardware==2 && w.Flips==1);
        pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);CHECK(reports==1);
        pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);CHECK(reports==2 && reported==20);
        // An entire second transaction during the pending read must invalidate this snapshot.
        inject_update=1;
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample));
        CHECK(sample.QuadPart==-1);
        CHECK(WddmReadCompletedPrimary(&d,&w,&sample) && sample.QuadPart==30);
        CHECK(w.PrimarySegment==3 && hardware==3);
        CHECK(level ? arms==0 : arms==3);
        request.VidPnSourceId=1;CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
        CHECK(hardware==3);
    }
    printf("vidpn publication: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
