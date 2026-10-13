// Host controls for actual inherited timing decoder and FillSignalInfo.
// Totals 2080x1235 are M86; the clock/blank fixtures are synthetic reference inputs,
// not an assertion that DPREFCLK/DTO have already been measured on the lab.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef long NTSTATUS;
typedef void* HANDLE;
typedef unsigned char BOOLEAN;
typedef struct {long long QuadPart;} LARGE_INTEGER;
#define FALSE 0
#define TRUE 1
#define STATUS_INVALID_PARAMETER (-7L)
#define D3DDDI_ROTATION_IDENTITY 1u
#define BC250_WDDM_MAGIC_ALLOCATION 1u
#define BC250_CHILD_UID 0x250001ul
#define WddmDdiDescribeAllocation 1u
static unsigned guardLines;
static char lastGuard[512];
static void GuardLog(const char* fmt,...)
{va_list args;va_start(args,fmt);vsnprintf(lastGuard,sizeof(lastGuard),fmt,args);va_end(args);guardLines++;}
#define STATUS_SUCCESS 0L
#define STATUS_NOT_SUPPORTED (-1L)
#define STATUS_DEVICE_DATA_ERROR (-2L)
#define STATUS_INVALID_DEVICE_STATE (-3L)
#define STATUS_DEVICE_NOT_READY (-4L)
#define STATUS_DEVICE_BUSY (-5L)
#define STATUS_ACCESS_DENIED (-6L)
#define NT_SUCCESS(s) ((s)>=0)
#define MAXULONG 0xFFFFFFFFul
#define PASSIVE_LEVEL 0
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define D3DKMDT_FREQUENCY_NOTSPECIFIED 0xFFFFFFFEu
#define D3DKMDT_VSS_OTHER 255u
#define D3DDDI_VSSLO_PROGRESSIVE 1u
typedef struct {UINT cx,cy;} REGION;
typedef struct {UINT Numerator,Denominator;} RATIONAL;
typedef struct {UINT VideoStandard;REGION TotalSize,ActiveSize;RATIONAL VSyncFreq,HSyncFreq;SIZE_T PixelRate;UINT ScanLineOrdering;} D3DKMDT_VIDEO_SIGNAL_INFO;
struct TEST_VIDPN_INTERFACE;
#include "dcn_scale.h"
#include "edid.h"
#include "../shim/bc250_ddc.h"
typedef struct {BC250_INHERITED_PIPE InheritedPipe;NTSTATUS InheritedPipeStatus;
    ULONG InheritedSupport;BOOLEAN InheritedFixed;
    volatile long InheritedSupportCalls,InheritedCofuncCalls,InheritedCommitCalls;
    ULONG Requested;BOOLEAN EdidValid,DpcdValid;BC250_DUAL_MODE_ADAPTER Adapter;
    ULONG Level,Refusals,Commits,Changes,Busy,Scaling;
    NTSTATUS LastStatus;BOOLEAN Suspended,PipeChanged;int Lock;
    long SourceWidth,SourceHeight;BC250_MODE_LIST Modes;
    BC250_SCALER_PLAN Plan;BC250_SCALER_RESULT LastResult;} BC250_MODESET;
typedef struct {void* Mmio;int FullWddm,ComposedSourceModes,DcnWriteEnabled;struct {ULONG Width,Height,Pitch;} Post;
    D3DKMDT_VIDEO_SIGNAL_INFO InheritedSignal;BOOLEAN InheritedSignalValid,InheritedTmds;BC250_MODESET Modeset;
    BOOLEAN ModeActive,CommitSeen; ULONG CommittedSourceWidth,CommittedSourceHeight,CommittedSourceFormat;
    struct {ULONG StartAccepted;} Connector;
    struct {NTSTATUS (*DxgkCbQueryVidPnInterface)(void*,unsigned,const struct TEST_VIDPN_INTERFACE**);
        void *DxgkCbSynchronizeExecution,*DxgkCbQueueDpc,*DxgkCbIndicateChildStatus;} Dxgk;
} BC250_DEVICE;

// This gate retains the inherited route. Dynamic selection has its own actual
// helper gate; these transport seams must never be reached here.
static volatile int unexpectedConnector = 1;
static void UnexpectedConnector(void){if(unexpectedConnector) abort();}
static BOOLEAN testConnectorEnabled;
static D3DKMDT_VIDEO_SIGNAL_INFO testConnectorSignal;
static NTSTATUS testConnectorStatus;
static BOOLEAN ConnectorIsEnabled(const BC250_DEVICE* d){(void)d;return testConnectorEnabled;}
static NTSTATUS ConnectorLinkCurrentSignal(BC250_DEVICE* d,D3DKMDT_VIDEO_SIGNAL_INFO* s)
{(void)d;if(!testConnectorEnabled)UnexpectedConnector();*s=testConnectorSignal;return testConnectorStatus;}
typedef UINT D3DDDIFORMAT;
typedef struct {struct {UINT Width,Height,Format;} Allocation;} BC250_WDDM_OBJECT;
typedef struct {HANDLE hAllocation;UINT Width,Height;D3DDDIFORMAT Format;
    struct {UINT NumSamples,NumQualityLevels;} MultisampleMethod;RATIONAL RefreshRate;
    UINT PrivateDriverFormatAttribute,Rotation;} DXGKARG_DESCRIBEALLOCATION;
static BC250_WDDM_OBJECT* WddmObject(HANDLE h,UINT magic){(void)magic;return (BC250_WDDM_OBJECT*)h;}
static void* WddmOf(HANDLE h){return h;}
static int WddmFirstCalls(void* w,UINT call){(void)w;(void)call;return 0;}
static int irql;
static int KeGetCurrentIrql(void){return irql;}
static NTSTATUS MmioDcnRead(const BC250_DEVICE*,ULONG,ULONG*);
static NTSTATUS MmioRead(const BC250_DEVICE*,ULONG,ULONG*);
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER*);
static void KeStallExecutionProcessor(ULONG);
#include "../display_timing.h"
// BD-122 (#38): the real refusal logger under its own name; the decoder under test calls the counting stub, so the
// read counts of the decoder checks below stay the decoder's own.
static wchar_t detail[400];
static void GuardStartDetail(const wchar_t* format,...){va_list a;va_start(a,format);vswprintf(detail,400,format,a);va_end(a);}
#define DisplayLogRefusedTiming RealDisplayLogRefusedTiming
#include "display_timing_refused.h"
#undef DisplayLogRefusedTiming
static unsigned refusedLogs;static NTSTATUS lastRefusal;
static void DisplayLogRefusedTiming(const BC250_DEVICE* d,NTSTATUS s){(void)d;refusedLogs++;lastRefusal=s;}
NTSTATUS DisplayValidateInheritedSignal(BC250_DEVICE*);
#include "inherited_timing_fixture.h"
#include "inherited_commit_fixture.h"
#include "inherited_actual.inc"
#include "display_timing_actual.inc"
#include "describe_timing_actual.inc"
#include "inherited_vidpn_fixture.h"
#include "inherited_vidpn_actual.inc"
#include "inherited_negotiation_fixture.h"
#include "inherited_negotiation_actual.inc"
#include "display_inherited_vidpn.h"
#include "inherited_ddi_actual.inc"
#include "inherited_vidpn_commit_actual.inc"
static NTSTATUS TestFillSignalInfo(BC250_DEVICE* d,D3DKMDT_VIDEO_SIGNAL_INFO* s)
{
    NTSTATUS result=DisplayPrepareInheritedTiming(d);
    if(!NT_SUCCESS(result)){memset(s,0,sizeof(*s));s->PixelRate=D3DKMDT_FREQUENCY_NOTSPECIFIED;return result;}
    return FillSignalInfo(d,s);
}
static ULONG values[TimingCount];
static unsigned checks,failures,reads,tear,readFailure;static ULONG otg1Value;
static ULONG routes[TmdsRouteCount],frameBase;
static ULONGLONG qpc,period=200000;
static int stopped,unstable,routeTear,modeTear,skipEdge,frozenQpc;
static unsigned polls;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static NTSTATUS ReadValue(ULONG offset,ULONG* value)
{
    static const ULONG offsets[]={BC250_REG_DMU_OTG0_OTG_CONTROL,BC250_REG_DMU_OTG0_OTG_H_TOTAL,
        BC250_REG_DMU_OTG0_OTG_V_TOTAL,BC250_REG_DMU_OTG0_OTG_H_BLANK_START_END,
        BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END,BC250_REG_DMU_OTG0_PIXEL_RATE_CNTL,
        BC250_REG_DMU_DP_DTO0_PHASE,BC250_REG_DMU_DP_DTO0_MODULO,
        BC250_REG_DMU_OTG0_OTG_INTERLACE_CONTROL,BC250_REG_DMU_OTG0_OTG_V_TOTAL_CONTROL,
        BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT};
    unsigned i;reads++;
    if(readFailure && reads==readFailure)return STATUS_ACCESS_DENIED;
    if(offset==BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT) {
        ULONGLONG frame=qpc/period;
        if(unstable && frame>=5)frame=5+(qpc-5*period)/(period+period/5);
        if(skipEdge && frame>=4)frame++;
        *value=(frameBase+(stopped?0:(ULONG)frame))&OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
        return STATUS_SUCCESS;
    }
    {
        static const ULONG routeOffsets[]={BC250_REG_DMU_DIG0_DIG_FE_CNTL,BC250_REG_DMU_DIG1_DIG_FE_CNTL,
            BC250_REG_DMU_DIG0_DIG_BE_CNTL,BC250_REG_DMU_DIG1_DIG_BE_CNTL,
            BC250_REG_DMU_DIG0_DIG_BE_EN_CNTL,BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL};
        for(i=0;i<TmdsRouteCount;i++)if(offset==routeOffsets[i]) {
            *value=routes[i];if(routeTear && qpc>period*8 && i==TmdsFe0)*value=1;
            return STATUS_SUCCESS;
        }
    }
    if(offset==BC250_REG_DMU_OTG1_OTG_CONTROL){*value=otg1Value;return STATUS_SUCCESS;}
    for(i=0;i<RTL_NUMBER_OF(offsets);i++)if(offsets[i]==offset){*value=values[i];
        if(tear && reads>TimingCount && i==TimingPhase)(*value)++;
        if(modeTear && qpc>period*8 && i==TimingHTotal)(*value)++;
        return STATUS_SUCCESS;}
    CHECK(0);return STATUS_ACCESS_DENIED;
}
static NTSTATUS MmioDcnRead(const BC250_DEVICE* d,ULONG offset,ULONG* value)
{(void)d;CHECK(offset!=BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT);return ReadValue(offset,value);}
static NTSTATUS MmioRead(const BC250_DEVICE* d,ULONG offset,ULONG* value)
{(void)d;CHECK(offset==BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT);return ReadValue(offset,value);}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER* frequency)
{LARGE_INTEGER result;if(frequency)frequency->QuadPart=12000000;result.QuadPart=(long long)qpc;return result;}
static void KeStallExecutionProcessor(ULONG us)
{polls++;if(!frozenQpc)qpc+=(ULONGLONG)us*12;}
static void Mode(BC250_DEVICE* d,ULONG w,ULONG h,ULONG ht,ULONG vt,ULONG pixel)
{
    memset(values,0,sizeof(values));reads=tear=readFailure=0;
    qpc=0;polls=0;period=200000;frameBase=0;
    stopped=unstable=routeTear=modeTear=skipEdge=frozenQpc=0;
    memset(routes,0,sizeof(routes));d->InheritedTmds=FALSE;
    d->Mmio=d;d->FullWddm=1;d->Post.Width=w;d->Post.Height=h;
    d->Post.Pitch=w*4;PipeFixture(w,h,w,h);
    values[TimingControl]=OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    values[TimingPixelControl]=OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK;
    values[TimingHTotal]=ht-1;values[TimingVTotal]=vt-1;
    values[TimingHBlank]=((ht-w-1)<<OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT)|(ht-1);
    values[TimingVBlank]=((vt-h-1)<<OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT)|(vt-1);
    values[TimingPhase]=pixel;values[TimingModulo]=600000000u;values[TimingReference]=6000;
}
static void Adapter(BC250_DEVICE* d)
{
    Mode(d,1920,1080,2080,1119,0);d->Post.Pitch=7680;
    // Exact tuple saved in issue #38's unit-A adapter start record.
    values[TimingControl]=0x80011311;values[TimingHBlank]=0x007007F0;values[TimingVBlank]=0x0024045C;
    values[TimingPixelControl]=0;
    routes[TmdsFe1]=1; // inactive encoder keeps an OTG1 selection
    routes[TmdsBe0]=(1ul<<DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT)|(3ul<<DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT);
    routes[TmdsEnable0]=DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK;
}
#include "inherited_pipe_cases.h"
#include "inherited_vidpn_cases.h"
#include "inherited_field38_cases.h"
#include "inherited_start_cases.h"
#include "inherited_negotiation_cases.h"
int main(int argc,char** argv)
{
    BC250_DEVICE d={0};D3DKMDT_VIDEO_SIGNAL_INFO s;unsigned before;
    if(argc!=1) {
        NTSTATUS result;unsigned i;char* end;
        if(argc!=15 || strcmp(argv[1],"--snapshot")) {
            puts("usage: timing_test --snapshot width height control htotal vtotal hblank vblank pixelcontrol phase modulo interlace vtotalcontrol reference");
            return 2;
        }
        d.Mmio=&d;d.FullWddm=1;
        d.Post.Width=strtoul(argv[2],&end,0);if(*end)return 2;
        d.Post.Height=strtoul(argv[3],&end,0);if(*end)return 2;
        for(i=0;i<TimingCount;i++){values[i]=strtoul(argv[4+i],&end,0);if(*end)return 2;}
        result=TestFillSignalInfo(&d,&s);
        printf("decode status %ld, reads %u\n",result,reads);
        if(result==STATUS_SUCCESS)printf("active %ux%u total %ux%u pixel %llu Hz hsync %u/%u Hz vsync %u/%u Hz (%.9f)\n",
            s.ActiveSize.cx,s.ActiveSize.cy,s.TotalSize.cx,s.TotalSize.cy,(ULONGLONG)s.PixelRate,
            s.HSyncFreq.Numerator,s.HSyncFreq.Denominator,s.VSyncFreq.Numerator,s.VSyncFreq.Denominator,
            (double)s.VSyncFreq.Numerator/s.VSyncFreq.Denominator);
        return result==STATUS_SUCCESS?0:1;
    }
    InheritedPipeCases();
    InheritedVidpnCases();
    InheritedField38Cases();refusedLogs=0;
    InheritedStartCases();refusedLogs=0;
    NegotiationCases();refusedLogs=0;
    Mode(&d,1920,1200,2080,1235,154000000);
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && reads==2*TimingCount && refusedLogs==0);
    CHECK(s.TotalSize.cx==2080 && s.TotalSize.cy==1235 && s.ActiveSize.cx==1920 && s.ActiveSize.cy==1200);
    CHECK(s.PixelRate==154000000 && s.HSyncFreq.Numerator==154000000 && s.HSyncFreq.Denominator==2080);
    CHECK(s.VSyncFreq.Numerator==154000000 && s.VSyncFreq.Denominator==2568800);
    CHECK(s.ScanLineOrdering==D3DDDI_VSSLO_PROGRESSIVE);
    {
        BC250_WDDM_OBJECT allocation={{1920,1200,21}};
        DXGKARG_DESCRIBEALLOCATION description={0};
        description.hAllocation=&allocation;
        before=reads;
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_SUCCESS);
        CHECK(description.RefreshRate.Numerator==s.VSyncFreq.Numerator &&
              description.RefreshRate.Denominator==s.VSyncFreq.Denominator);
        CHECK((ULONGLONG)description.RefreshRate.Numerator*1000u !=
              (ULONGLONG)60000u*description.RefreshRate.Denominator); // old60Hz must differ
        CHECK(reads==before); // allocation description never queries MMIO
        values[TimingPhase]++; // subsequent reads must retain the published mode
        CHECK(FillSignalInfo(&d,&s)==STATUS_SUCCESS && reads==before && s.PixelRate==154000000);
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_SUCCESS && reads==before &&
              description.RefreshRate.Numerator==s.VSyncFreq.Numerator &&
              description.RefreshRate.Denominator==s.VSyncFreq.Denominator);
        d.InheritedSignalValid=FALSE; // StopDevice invalidation
        CHECK(FillSignalInfo(&d,&s)==STATUS_DEVICE_NOT_READY);
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_DEVICE_NOT_READY);
    }
    Mode(&d,1366,768,1500,800,72000000);
    values[TimingReference]=7000;values[TimingModulo]=350000000;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && s.PixelRate==144000000);
    CHECK(s.VSyncFreq.Numerator==144000000 && s.VSyncFreq.Denominator==1200000);
    Mode(&d,1920,1080,2200,1125,148500000);
    values[TimingModulo]=600600000;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && s.PixelRate==148351648);
    CHECK(s.VSyncFreq.Numerator==148351648 && s.VSyncFreq.Denominator==2475000);
    tear=1;reads=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_BUSY && s.PixelRate==D3DKMDT_FREQUENCY_NOTSPECIFIED);
    CHECK(refusedLogs==1 && lastRefusal==STATUS_DEVICE_BUSY); // every refusal names its registers once
    tear=0;values[TimingReference]=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR && s.PixelRate==D3DKMDT_FREQUENCY_NOTSPECIFIED);
    CHECK(refusedLogs==2 && lastRefusal==STATUS_DEVICE_DATA_ERROR);
    values[TimingReference]=6000;values[TimingInterlace]=OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE_MASK;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED);
    values[TimingInterlace]=0;values[TimingVTotalControl]=OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MIN_SEL_MASK;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED);
    values[TimingVTotalControl]=0;d.Post.Width++;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED); // geometry decoded; inconsistent viewport refused
    d.Post.Width--;reads=0;readFailure=3;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_ACCESS_DENIED && reads==3);
    readFailure=0;before=reads;irql=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_INVALID_DEVICE_STATE && reads==before);
    irql=0;d.Mmio=NULL;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_NOT_READY && reads==before);
    d.FullWddm=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && reads==before && s.PixelRate==D3DKMDT_FREQUENCY_NOTSPECIFIED);
    // BD-122 (#38): the real logger. #38's case: a POST of 800x600, OTG0 not enabled, the firmware drove OTG1.
    d.FullWddm=1;Mode(&d,800,600,1056,628,40000000);d.Post.Pitch=3328;
    values[TimingControl]=0;otg1Value=OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;before=reads;
    RealDisplayLogRefusedTiming(&d,STATUS_NOT_SUPPORTED);
    CHECK(reads==before+TimingCount+1);
    CHECK(wcsstr(detail,L"post 800x600 pitch 3328 otg0 ctl 00000000 ht 0000041F vt 00000273")!=NULL);
    CHECK(wcsstr(detail,L"otg1 ctl 00000001")!=NULL && wcsstr(detail,L"timing 0xFFFFFFFF ")==detail);
    d.Mmio=NULL;detail[0]=0;before=reads;
    RealDisplayLogRefusedTiming(&d,STATUS_DEVICE_DATA_ERROR);
    CHECK(reads==before && wcsstr(detail,L"post 800x600 registers not read 0xFFFFFFFC")!=NULL);
    d.Mmio=&d;irql=1;detail[0]=0;
    RealDisplayLogRefusedTiming(&d,STATUS_DEVICE_DATA_ERROR);
    CHECK(reads==before && wcsstr(detail,L"registers not read")!=NULL);irql=0;
    readFailure=reads+2;detail[0]=0;
    RealDisplayLogRefusedTiming(&d,STATUS_NOT_SUPPORTED);
    CHECK(wcsstr(detail,L"registers not read 0xFFFFFFFA")!=NULL);readFailure=0;
    Adapter(&d);
    // The field #38 class: GOP 1024x768 feeding a 1920x1080 TMDS target.
    // No clock is inferred from these dimensions; the same QPC/frame-counter
    // model as the adapter control supplies the measured target refresh.
    d.Post.Width=1024;d.Post.Height=768;d.Post.Pitch=4096;
    PipeFixture(1024,768,1920,1080);
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && d.Modeset.InheritedFixed);
    CHECK(s.ActiveSize.cx==1920 && s.ActiveSize.cy==1080 && s.PixelRate==139650000);
    CHECK(d.Modeset.InheritedSupport==BC250_SCALING_BIT(BC250_SCALING_STRETCHED));
    CHECK(pipeReads==2*BC250_INHERITED_PIPE_READS && pipeWrites==0);
    {
        unsigned n;
        for(n=1;n<=2*BC250_INHERITED_PIPE_READS;n++) {
            PipeFixture(1024,768,1920,1080);pipeReadFailure=n;
            CHECK(TestFillSignalInfo(&d,&s)==STATUS_ACCESS_DENIED && !d.InheritedSignalValid);
            CHECK(!d.Modeset.InheritedSupport && pipeWrites==0);
        }
        PipeFixture(1024,768,1920,1080);pipeTear=1;
        CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_BUSY && !d.InheritedSignalValid && pipeWrites==0);
        PipeFixture(1024,768,1920,1080);
        pipeRegisters[BC250_REG_DMU_MPCC0_MPCC_TOP_SEL/4]=1;
        CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && !d.InheritedSignalValid && pipeWrites==0);
    }
    Adapter(&d);
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && d.InheritedTmds && d.InheritedSignalValid);
    CHECK(s.TotalSize.cx==2080 && s.TotalSize.cy==1119 && s.PixelRate==139650000);
    CHECK(s.VSyncFreq.Numerator==139650000 && s.VSyncFreq.Denominator==2327520);
    CHECK(polls>0 && qpc<=6000000 && polls<50000);
    before=reads;CHECK(FillSignalInfo(&d,&s)==STATUS_SUCCESS && reads==before);
    {
        BC250_WDDM_OBJECT allocation={{1920,1080,21}};
        DXGKARG_DESCRIBEALLOCATION description={0};
        BC250_EDID_TIMING dtd={0};
        description.hAllocation=&allocation;
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_SUCCESS && reads==before);
        CHECK(description.RefreshRate.Numerator==s.VSyncFreq.Numerator && description.RefreshRate.Denominator==s.VSyncFreq.Denominator);
        dtd.HActive=1920;dtd.HBlank=160;dtd.VActive=1080;dtd.VBlank=20;dtd.PixelClock10Khz=22880;
        CHECK(DisplayTmdsPixel(228800001,1920,1080,2080,1119,&dtd)==228800000); // measured rounds independently
        CHECK(DisplayTmdsPixel(228123456,1920,1080,2080,1119,&dtd)==228120000); // LG DTD must NOT be selected
        CHECK(DisplayTmdsPixel(228123456,1920,1080,2080,1100,&dtd)==228800000); // matching geometry and clock
        CHECK(DisplayTmdsPixel(139651200,1920,1080,2080,1100,&dtd)==139650000); // wrong clock refused
        dtd.Flags=0x80;CHECK(DisplayTmdsPixel(228123456,1920,1080,2080,1100,&dtd)==228120000);
    }
    Adapter(&d);routes[TmdsBe0]=1ul<<DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT; // DP, DTO off is not TMDS
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && !d.InheritedTmds && !d.InheritedSignalValid && polls==0);
    Adapter(&d);routes[TmdsEnable0]=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && polls==0);
    Adapter(&d);routes[TmdsFe0]=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && polls==0);
    Adapter(&d);routes[TmdsBe0]|=DIG0_DIG_BE_CNTL__DIG_DUAL_LINK_ENABLE_MASK;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && polls==0);
    Adapter(&d);routes[TmdsBe0]|=2ul<<DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && polls==0);
    Adapter(&d);routes[TmdsEnable1]=1;routes[TmdsBe1]=routes[TmdsBe0];
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && polls==0);
    Adapter(&d);routes[TmdsBe0]=(2ul<<DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT)|(2ul<<DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT);
    routes[TmdsFe0]=1;routes[TmdsFe1]=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && d.InheritedTmds); // DVI, front end 1
    Adapter(&d);frameBase=0x00FFFFFC;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && d.InheritedTmds); // real 24-bit rollover
    Adapter(&d);stopped=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR && !d.InheritedSignalValid && polls==50000);
    Adapter(&d);frozenQpc=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR && polls==50000);
    Adapter(&d);unstable=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR && !d.InheritedTmds);
    Adapter(&d);skipEdge=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR && !d.InheritedTmds);
    Adapter(&d);routeTear=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && !d.InheritedTmds);
    Adapter(&d);modeTear=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_BUSY && !d.InheritedTmds);
    Adapter(&d);period=60000; // 200 Hz
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && s.PixelRate==465500000);
    Adapter(&d);period=40000; // 300 Hz
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && !d.InheritedSignalValid);
    Adapter(&d);period=600000; // 20 Hz
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && !d.InheritedSignalValid);
    Adapter(&d);values[TimingInterlace]=OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE_MASK;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED && polls==0);
    Mode(&d,1920,1200,2080,1235,154000000);
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && !d.InheritedTmds && s.PixelRate==154000000 && polls==0);
    {
        BC250_WDDM_OBJECT allocation={{1280,720,21}};
        DXGKARG_DESCRIBEALLOCATION description={0};
        description.hAllocation=&allocation;
        testConnectorEnabled=TRUE;testConnectorStatus=STATUS_SUCCESS;
        d.InheritedSignal.VSyncFreq.Numerator=60;d.InheritedSignal.VSyncFreq.Denominator=1;
        testConnectorSignal.VSyncFreq.Numerator=75;testConnectorSignal.VSyncFreq.Denominator=1;
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_SUCCESS);
        CHECK(description.RefreshRate.Numerator==75 && description.RefreshRate.Denominator==1);
        testConnectorSignal.VSyncFreq.Numerator=60000;testConnectorSignal.VSyncFreq.Denominator=1001;
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_SUCCESS);
        CHECK(description.RefreshRate.Numerator==60000 && description.RefreshRate.Denominator==1001);
        testConnectorStatus=STATUS_DEVICE_NOT_READY;
        CHECK(Bc250WddmDescribeAllocation(&d,&description)==STATUS_DEVICE_NOT_READY);
        testConnectorEnabled=FALSE;
    }
    printf("inherited timing: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
