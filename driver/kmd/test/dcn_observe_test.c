// Actual-source reader/dispatcher test. MMIO model has no write entry points.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "regs.generated.h"
#include "bc250kmd_escape.h"
#define C_ASSERT(x) _Static_assert(x,#x)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
typedef unsigned long ULONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef void* HANDLE;
typedef struct {void* Mmio;int FullWddm;volatile long DcnSurfaceSequence;} BC250_DEVICE;
typedef struct {void*pPrivateDriverData;ULONG PrivateDriverDataSize;struct {ULONG Value;} Flags;} DXGKARG_ESCAPE;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
static unsigned checks,failures,reads,admin=1,bump;
static ULONG fail_reg;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static BOOLEAN CallerIsAdmin(void){return (BOOLEAN)admin;}
static long InterlockedCompareExchange(volatile long*p,long a,long b){long v=*p;if(v==b)*p=a;return v;}
static NTSTATUS MmioDcnRead(const BC250_DEVICE*d,ULONG reg,ULONG*out)
{
    reads++;if(bump)((BC250_DEVICE*)d)->DcnSurfaceSequence+=2;
    if(reg==fail_reg)return STATUS_DEVICE_NOT_READY;
    *out=reg^0x5A000001u;return STATUS_SUCCESS;
}
static NTSTATUS MmioRead(const BC250_DEVICE*d,ULONG reg,ULONG*out){return MmioDcnRead(d,reg,out);}
/* TIMING_SOURCE */
/* ACTUAL_SOURCE */
static void Prepare(BC250_ESCAPE_DCN_OBSERVE*o)
{
    memset(o,0xCC,sizeof(*o));o->Magic=BC250_ESCAPE_MAGIC;o->Command=BC250_ESCAPE_OBSERVE_DCN;o->AbiVersion=BC250_DCN_OBSERVE_ABI;
    reads=0;fail_reg=0;bump=0;admin=1;
}
int main(void)
{
    BC250_DEVICE d={(void*)1,TRUE,12};BC250_ESCAPE_DCN_OBSERVE o;
    DXGKARG_ESCAPE e={&o,sizeof(o),{1}};unsigned flag;
    C_ASSERT(sizeof(o)==128);
    Prepare(&o);CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);
    CHECK(o.Status==BC250_ESCAPE_STATUS_DONE && o.NtStatus==0);
    CHECK(o.Version==BC250_KMD_VERSION && o.AbiVersion==1 && o.RegisterCount==22);
    CHECK(o.ValidMask==BC250_DCN_OBSERVE_VALID_ALL && reads==22);
    CHECK(o.SequenceBefore==12 && o.SequenceAfter==12 && d.DcnSurfaceSequence==12);
#define VALUE(field,reg) CHECK(o.field==((reg)^0x5A000001u))
    VALUE(PrimaryAddressLow,BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS);
    VALUE(PrimaryAddressHigh,BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH);
    VALUE(EarliestInUseLow,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE);
    VALUE(EarliestInUseHigh,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH);
    VALUE(FlipControl,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL);
    VALUE(SurfacePitch,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH);
    VALUE(OtgStatusPosition,BC250_REG_DMU_OTG0_OTG_STATUS_POSITION);
    VALUE(OtgGlobalControl0,BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0);
    VALUE(OtgBlankControl,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL);
    VALUE(OtgDoubleBufferControl,BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL);
    VALUE(OtgFrameCount,BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT);
    VALUE(TimingControl,BC250_REG_DMU_OTG0_OTG_CONTROL);
    VALUE(TimingHTotal,BC250_REG_DMU_OTG0_OTG_H_TOTAL);
    VALUE(TimingVTotal,BC250_REG_DMU_OTG0_OTG_V_TOTAL);
    VALUE(TimingHBlank,BC250_REG_DMU_OTG0_OTG_H_BLANK_START_END);
    VALUE(TimingVBlank,BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END);
    VALUE(TimingPixelControl,BC250_REG_DMU_OTG0_PIXEL_RATE_CNTL);
    VALUE(TimingPhase,BC250_REG_DMU_DP_DTO0_PHASE);
    VALUE(TimingModulo,BC250_REG_DMU_DP_DTO0_MODULO);
    VALUE(TimingInterlace,BC250_REG_DMU_OTG0_OTG_INTERLACE_CONTROL);
    VALUE(TimingVTotalControl,BC250_REG_DMU_OTG0_OTG_V_TOTAL_CONTROL);
    VALUE(TimingReference,BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT);
    Prepare(&o);bump=1;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.SequenceAfter-o.SequenceBefore==44);
    for(flag=0;flag<32;flag++)if(flag!=1){Prepare(&o);e.Flags.Value=flag;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.Status==BC250_ESCAPE_STATUS_REFUSED && o.NtStatus==(ULONG)STATUS_INVALID_PARAMETER && reads==0 && o.ValidMask==0);}
    e.Flags.Value=1;
    Prepare(&o);admin=0;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.Status==BC250_ESCAPE_STATUS_NOT_ADMIN && reads==0);
    Prepare(&o);o.AbiVersion=2;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.Status==BC250_ESCAPE_STATUS_REFUSED && reads==0);
    Prepare(&o);d.Mmio=NULL;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.NtStatus==(ULONG)STATUS_DEVICE_NOT_READY && reads==0);d.Mmio=(void*)1;
    Prepare(&o);e.PrivateDriverDataSize--;CHECK(Bc250Escape(&d,&e)==STATUS_INVALID_PARAMETER && reads==0);e.PrivateDriverDataSize++;
    Prepare(&o);fail_reg=BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE;
    CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.Status==BC250_ESCAPE_STATUS_REFUSED && o.ValidMask==(BC250_DCN_OBSERVE_VALID_ALL & ~(1u<<2)) && o.EarliestInUseLow==0);
    Prepare(&o);fail_reg=BC250_REG_DMU_DP_DTO0_PHASE;
    CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.ValidMask==((1u<<11)-1u) && o.TimingControl==0 && o.TimingReference==0);
    Prepare(&o);fail_reg=BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT;
    CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(o.ValidMask==((1u<<11)-1u) && o.TimingPhase==0);
    printf("DCN observer: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
