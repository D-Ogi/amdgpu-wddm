// Host controls for actual inherited timing decoder and FillSignalInfo.
// Totals 2080x1235 are M86; the clock/blank fixtures are synthetic reference inputs,
// not an assertion that DPREFCLK/DTO have already been measured on the lab.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef long NTSTATUS;
typedef void* HANDLE;
typedef unsigned char BOOLEAN;
#define FALSE 0
#define TRUE 1
#define STATUS_INVALID_PARAMETER (-7L)
#define D3DDDI_ROTATION_IDENTITY 1u
#define BC250_WDDM_MAGIC_ALLOCATION 1u
#define WddmDdiDescribeAllocation 1u
#define GuardLog(...) ((void)0)
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
typedef struct {void* Mmio;int FullWddm;struct {ULONG Width,Height;} Post;
    D3DKMDT_VIDEO_SIGNAL_INFO InheritedSignal;BOOLEAN InheritedSignalValid;} BC250_DEVICE;
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
#include "../display_timing.h"
#include "display_timing_actual.inc"
#include "describe_timing_actual.inc"
static NTSTATUS TestFillSignalInfo(BC250_DEVICE* d,D3DKMDT_VIDEO_SIGNAL_INFO* s)
{
    NTSTATUS result=DisplayPrepareInheritedTiming(d);
    if(!NT_SUCCESS(result)){memset(s,0,sizeof(*s));s->PixelRate=D3DKMDT_FREQUENCY_NOTSPECIFIED;return result;}
    return FillSignalInfo(d,s);
}
static ULONG values[TimingCount];
static unsigned checks,failures,reads,tear,readFailure;
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
    for(i=0;i<RTL_NUMBER_OF(offsets);i++)if(offsets[i]==offset){*value=values[i];
        if(tear && reads>TimingCount && i==TimingPhase)(*value)++;
        return STATUS_SUCCESS;}
    CHECK(0);return STATUS_ACCESS_DENIED;
}
static NTSTATUS MmioDcnRead(const BC250_DEVICE* d,ULONG offset,ULONG* value)
{(void)d;CHECK(offset!=BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT);return ReadValue(offset,value);}
static NTSTATUS MmioRead(const BC250_DEVICE* d,ULONG offset,ULONG* value)
{(void)d;CHECK(offset==BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT);return ReadValue(offset,value);}
static void Mode(BC250_DEVICE* d,ULONG w,ULONG h,ULONG ht,ULONG vt,ULONG pixel)
{
    memset(values,0,sizeof(values));reads=tear=readFailure=0;
    d->Mmio=d;d->FullWddm=1;d->Post.Width=w;d->Post.Height=h;
    values[TimingControl]=OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    values[TimingPixelControl]=OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK;
    values[TimingHTotal]=ht-1;values[TimingVTotal]=vt-1;
    values[TimingHBlank]=((ht-w-1)<<OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT)|(ht-1);
    values[TimingVBlank]=((vt-h-1)<<OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT)|(vt-1);
    values[TimingPhase]=pixel;values[TimingModulo]=600000000u;values[TimingReference]=6000;
}
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
    Mode(&d,1920,1200,2080,1235,154000000);
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && reads==2*TimingCount);
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
    tear=0;values[TimingReference]=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR && s.PixelRate==D3DKMDT_FREQUENCY_NOTSPECIFIED);
    values[TimingReference]=6000;values[TimingInterlace]=OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE_MASK;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED);
    values[TimingInterlace]=0;values[TimingVTotalControl]=OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MIN_SEL_MASK;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_NOT_SUPPORTED);
    values[TimingVTotalControl]=0;d.Post.Width++;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_DATA_ERROR);
    d.Post.Width--;reads=0;readFailure=3;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_ACCESS_DENIED && reads==3);
    readFailure=0;before=reads;irql=1;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_INVALID_DEVICE_STATE && reads==before);
    irql=0;d.Mmio=NULL;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_DEVICE_NOT_READY && reads==before);
    d.FullWddm=0;
    CHECK(TestFillSignalInfo(&d,&s)==STATUS_SUCCESS && reads==before && s.PixelRate==D3DKMDT_FREQUENCY_NOTSPECIFIED);
    printf("inherited timing: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
