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
typedef struct {void* Mmio;int FullWddm;volatile long DcnSurfaceSequence;volatile long RetainedPowerPhase;} BC250_DEVICE;
/* WDK 10.0.26100 d3dukmdt.h: HardwareAccess 0x1, NoAdapterSynchronization 0x8. */
typedef union {ULONG Value;struct {ULONG HardwareAccess:1,Unused:2,NoAdapterSynchronization:1;};} D3DDDI_ESCAPEFLAGS;
typedef struct {void*pPrivateDriverData;ULONG PrivateDriverDataSize;D3DDDI_ESCAPEFLAGS Flags;} DXGKARG_ESCAPE;
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
/* The escapes the dispatcher answers ahead of the DCN observer: counted, never reached by an observe request. */
static unsigned logs,journals,others;
static NTSTATUS LogEscape(BC250_DEVICE*d,const DXGKARG_ESCAPE*e,BOOLEAN summary){(void)d;(void)e;CHECK(!summary);logs++;return STATUS_SUCCESS;}
static NTSTATUS PagingJournalEscape(const BC250_DEVICE*d,const DXGKARG_ESCAPE*e){(void)d;(void)e;journals++;return STATUS_SUCCESS;}
static void StartHealthRequest(BC250_DEVICE*d,BC250_ESCAPE_START_HEALTH*p,BOOLEAN a,ULONG f){(void)d;(void)p;(void)a;(void)f;others++;}
static void CuModeRequest(BC250_DEVICE*d,BC250_ESCAPE_CU_MODE*p,BOOLEAN a,ULONG f){(void)d;(void)p;(void)a;(void)f;others++;}
static void DpmRequest(BC250_DEVICE*d,BC250_ESCAPE_DPM*p,ULONG n,BOOLEAN a,ULONG f){(void)d;(void)p;(void)n;(void)a;(void)f;others++;}
static void DpmTuneRequest(BC250_DEVICE*d,BC250_ESCAPE_DPM_TUNE*p,ULONG n,BOOLEAN a,ULONG f){(void)d;(void)p;(void)n;(void)a;(void)f;others++;}
static void InteropRequest(BC250_DEVICE*d,BC250_ESCAPE_INTEROP*p,ULONG f){(void)d;(void)p;(void)f;others++;}
static void HwmonRequest(BC250_DEVICE*d,BC250_ESCAPE_HWMON*p,ULONG f){(void)d;(void)p;(void)f;others++;}
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
    /* Any flag but HardwareAccess alone is refused: NoAdapterSynchronization by the dispatcher, before the observer
       (the observer touches DCN registers, not adapter-owned state), every other combination by the observer. */
    for(flag=0;flag<32;flag++)if(flag!=1){
        NTSTATUS status;Prepare(&o);e.Flags.Value=flag;status=Bc250Escape(&d,&e);CHECK(o.Status==BC250_ESCAPE_STATUS_REFUSED && reads==0);
        if(flag&8)CHECK(status==STATUS_DEVICE_NOT_READY);
        else{CHECK(status==STATUS_SUCCESS);CHECK(o.NtStatus==(ULONG)STATUS_INVALID_PARAMETER && o.ValidMask==0);}
    }
    e.Flags.Value=1;
    /* A suspended or restoring adapter: refused before any register read. */
    Prepare(&o);d.RetainedPowerPhase=2;CHECK(Bc250Escape(&d,&e)==STATUS_DEVICE_NOT_READY);CHECK(o.Status==BC250_ESCAPE_STATUS_REFUSED && reads==0);d.RetainedPowerPhase=0;
    CHECK(logs==0 && journals==0 && others==0);
    /* The software reads: GET_LOG with NoAdapterSynchronization alone goes to the log, ahead of the power phase;
       NoAdapterSynchronization with HardwareAccess is not that read and is refused like any other. */
    Prepare(&o);o.Command=BC250_ESCAPE_GET_LOG;e.Flags.Value=8;d.RetainedPowerPhase=2;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(logs==1 && reads==0);
    Prepare(&o);o.Command=BC250_ESCAPE_GET_PAGING_JOURNAL;CHECK(Bc250Escape(&d,&e)==STATUS_SUCCESS);CHECK(journals==1 && reads==0);
    Prepare(&o);o.Command=BC250_ESCAPE_GET_LOG;e.Flags.Value=9;CHECK(Bc250Escape(&d,&e)==STATUS_DEVICE_NOT_READY);CHECK(logs==1 && o.Status==BC250_ESCAPE_STATUS_REFUSED);
    d.RetainedPowerPhase=0;e.Flags.Value=1;
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
    CHECK(others==0);
    /* The fan reading (0.7.208.1): a published snapshot, so the dispatcher answers it with
       NoAdapterSynchronization alone and ahead of the power-phase check, and only at its exact size. */
    {
        BC250_ESCAPE_HWMON h;DXGKARG_ESCAPE he={&h,sizeof(h),{8}};
        memset(&h,0xCC,sizeof(h));h.Magic=BC250_ESCAPE_MAGIC;h.Command=BC250_ESCAPE_RUN_HWMON;
        reads=0;d.RetainedPowerPhase=2;
        CHECK(Bc250Escape(&d,&he)==STATUS_SUCCESS);CHECK(others==1 && reads==0);
        he.PrivateDriverDataSize--;
        CHECK(Bc250Escape(&d,&he)==STATUS_INVALID_PARAMETER);CHECK(others==1);
        d.RetainedPowerPhase=0;
    }
    printf("DCN observer: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
