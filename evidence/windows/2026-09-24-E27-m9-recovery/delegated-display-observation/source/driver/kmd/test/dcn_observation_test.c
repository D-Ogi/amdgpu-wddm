// Actual-source host controls. This MMIO model proves decoding/publication,
// not physical DCN latching or scan timing.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
typedef int32_t NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned UINT;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef unsigned char BOOLEAN;
typedef void* HANDLE;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef LARGE_INTEGER PHYSICAL_ADDRESS;
typedef struct { UINT VSyncTargetId; LARGE_INTEGER VSyncLast; } BC250_WDDM;
typedef struct {
    void *Mmio;
    BOOLEAN VramEnabled, VidPnFlipEnabled;
    PHYSICAL_ADDRESS VramPhysical;
    ULONGLONG VramMcBase, VramLength;
    struct { UINT Height; } Post;
    BC250_WDDM *Wddm;
} BC250_DEVICE;
typedef struct { UINT VidPnTargetId; BOOLEAN InVerticalBlank; UINT ScanLine; } DXGKARG_GETSCANLINE;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_WDDM_VSYNC_MS 16
#define WddmDdiGetScanLine 0
#define GuardLog(...) ((void)0)
static unsigned checks, failures, clock_queries;
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);} } while(0)
static struct {
    ULONGLONG inuse, next;
    ULONG pending, bounds, total, position;
    unsigned reads, change_on;
    NTSTATUS status;
} model;
static NTSTATUS MmioDcnRead(const BC250_DEVICE *d,ULONG reg,ULONG *value)
{
    (void)d;
    if (++model.reads==model.change_on) model.inuse=model.next;
    if (!NT_SUCCESS(model.status)) return model.status;
    switch(reg){
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH: *value=(ULONG)(model.inuse>>32);break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE: *value=(ULONG)model.inuse;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL: *value=model.pending;break;
    case BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END: *value=model.bounds;break;
    case BC250_REG_DMU_OTG0_OTG_V_TOTAL: *value=model.total;break;
    case BC250_REG_DMU_OTG0_OTG_STATUS_POSITION: *value=model.position;break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}
static BC250_WDDM* WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency)
{ LARGE_INTEGER now={1000000000};clock_queries++;frequency->QuadPart=1000000;return now; }
static int WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return 0;}
/* ACTUAL_SOURCE */
int main(void)
{
    BC250_WDDM w={0};BC250_DEVICE d={0};DXGKARG_GETSCANLINE line={0};
    ULONGLONG observed,old,next;unsigned phase,i;
    w.VSyncTargetId=0;d.Wddm=&w;d.Post.Height=1200;d.VidPnFlipEnabled=1;d.VramEnabled=1;d.Mmio=&model;
    // Exercise 8 and 12 GiB geometry and a high-dword address transition.
    d.VramPhysical.QuadPart=0x400000000ull;d.VramMcBase=0x800000000ull;
    for(i=0;i<2;i++){
        d.VramLength=(8ull+4ull*i)<<30;
        model.inuse=(ULONGLONG)d.VramPhysical.QuadPart+d.VramLength-0x100000;
        observed=0;CHECK(DcnReadScanoutAddress(&d,&observed)==STATUS_SUCCESS);
        CHECK(observed==d.VramMcBase+d.VramLength-0x100000);
        model.pending=0;CHECK(!DcnFlipPending(&d,observed));
        CHECK(DcnFlipPending(&d,observed-0x100000)); // cleared bit, old surface still in use
        model.pending=HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
        CHECK(DcnFlipPending(&d,observed));model.pending=0;
    }
    old=0x4fff00000ull;next=0x500100000ull;
    for(phase=1;phase<=3;phase++){
        model.inuse=old;model.next=next;model.reads=0;model.change_on=phase;
        CHECK(DcnReadScanoutAddress(&d,&observed)==STATUS_SUCCESS);
        CHECK(observed==d.VramMcBase+next-(ULONGLONG)d.VramPhysical.QuadPart);
        CHECK(model.reads<=6);
    }
    model.change_on=0;model.inuse=old;model.status=STATUS_DEVICE_NOT_READY;
    CHECK(DcnFlipPending(&d,d.VramMcBase+old-(ULONGLONG)d.VramPhysical.QuadPart));
    model.status=STATUS_SUCCESS;
    // Real geometry: 1235 total, 1200 active, blank wraps through counter zero.
    model.total=1234;model.bounds=(32u<<OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT)|1232u;
    for(i=0;i<1235;i++){
        model.position=i;
        CHECK(Bc250WddmGetScanLine(&d,&line)==STATUS_SUCCESS);
        CHECK(line.InVerticalBlank==(i<32 || i>=1232));
        CHECK(line.ScanLine==((i<32 || i>=1232)?0:i-32));
    }
    // A non-wrapping blank interval and active image crossing counter zero.
    model.total=999;model.bounds=(200u<<OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT)|100u;
    for(i=0;i<1000;i++){
        model.position=i;
        CHECK(Bc250WddmGetScanLine(&d,&line)==STATUS_SUCCESS);
        CHECK(line.InVerticalBlank==(i>=100 && i<200));
        CHECK(line.ScanLine==((i>=100 && i<200)?0:(i+800)%1000));
    }
    CHECK(clock_queries==0); // Hardware path never derives phase from stale VSyncLast.
    printf("DCN observation: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
