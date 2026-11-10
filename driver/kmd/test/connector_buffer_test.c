/* Actual production body with host-only allocation/mapping callbacks. */
#include <stdint.h>
#include "display_start.h"     /* BD-341: the display-start state */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef size_t SIZE_T;
typedef int NTSTATUS;
typedef struct {LONGLONG QuadPart;} PHYSICAL_ADDRESS;
typedef struct {
    ULONGLONG DisplayReservationEnd,VramLength;
    void* RuntimeFramebuffer;
    SIZE_T RuntimeFramebufferLength;
    PHYSICAL_ADDRESS VramPhysical,RuntimeFramebufferPhysical;
    unsigned DcnWriteEnabled,VramEnabled,VramWriteEnabled;
    struct {uint32_t Pitch,Height;} Post;
    unsigned long DisplayStart;     /* BD-341: display_start.h */
    unsigned long ColdScanoutBytes; /* BD-341: what the firmware can still scan out of at the bottom of VRAM */
} BC250_DEVICE;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define MAXULONGLONG UINT64_MAX
#define MAXLONGLONG INT64_MAX
#define BC250_VRAM_TOP_RESERVED (32ull<<20)
#define PAGE_READWRITE 4
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE (-1)
#define STATUS_NOT_SUPPORTED (-2)
#define STATUS_INVALID_PARAMETER (-3)
#define STATUS_NO_MEMORY (-4)
#define STATUS_INSUFFICIENT_RESOURCES (-5)
static unsigned checks,failures,maps,unmaps,zeros,barriers,full,offsetOk,mapFails;
static ULONGLONG postOffset;
static SIZE_T mappedBytes;
static PHYSICAL_ADDRESS mappedPhysical;
static unsigned char dummy;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("line%u: %s\n",__LINE__,#x);}}while(0)
static int WddmFullTableSelected(void){return (int)full;}
/* vram.c: offset 0 for an absent firmware display (vram_geometry_test.c runs the production body). */
static int VramFramebufferOffset(const BC250_DEVICE*d,ULONGLONG*out)
{*out=d->DisplayStart==BC250_DISPLAY_START_NO_POST?0:postOffset;return d->DisplayStart==BC250_DISPLAY_START_NO_POST?1:(int)offsetOk;}
static void* VramMapCpuRange(BC250_DEVICE*d,PHYSICAL_ADDRESS p,SIZE_T n,unsigned protection)
{(void)d;++maps;CHECK(protection==PAGE_READWRITE);mappedPhysical=p;mappedBytes=n;return mapFails?NULL:&dummy;}
static void MmUnmapIoSpace(void*p,SIZE_T n){++unmaps;CHECK(p==&dummy&&n==mappedBytes);}
static void RtlZeroMemory(void*p,SIZE_T n){++zeros;CHECK(p==&dummy&&n==mappedBytes);}
static void KeMemoryBarrier(void){++barriers;}
#define GuardLog(...) ((void)0)
/* dcn_cursor.c: the hardware pointer's surfaces lie after the buffer, so the release frees them first. */
static unsigned cursorReleases,cursorReleaseUnmaps;
static void DcnCursorRelease(BC250_DEVICE*d){(void)d;++cursorReleases;cursorReleaseUnmaps=unmaps;}
#include "connector_buffer_actual.inc"
static void init(BC250_DEVICE*d)
{
    memset(d,0,sizeof(*d));d->DcnWriteEnabled=d->VramEnabled=d->VramWriteEnabled=1;
    d->VramLength=8ull<<30;d->VramPhysical.QuadPart=0x80000000ll;
    d->Post.Pitch=1920*4;d->Post.Height=1080;
    full=offsetOk=1;mapFails=maps=unmaps=zeros=barriers=0;postOffset=123;
}
static void empty(const BC250_DEVICE*d)
{CHECK(!d->RuntimeFramebuffer&&!d->DisplayReservationEnd&&!d->RuntimeFramebufferLength);CHECK(!zeros&&!barriers);}
int main(void)
{
    BC250_DEVICE d;ULONGLONG start,end;unsigned i;
    init(&d);start=(postOffset+(ULONGLONG)d.Post.Pitch*d.Post.Height+65535)&~65535ull;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_SUCCESS);
    CHECK(maps==1&&zeros==1&&barriers==1);
    CHECK(mappedPhysical.QuadPart==d.VramPhysical.QuadPart+(LONGLONG)start);
    CHECK(mappedBytes==4096ull*2160*4&&d.DisplayReservationEnd==start+mappedBytes);
    CHECK(d.RuntimeFramebuffer==&dummy&&d.RuntimeFramebufferLength==mappedBytes);
    end=d.DisplayReservationEnd;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_DEVICE_STATE);CHECK(maps==1);
    ConnectorBufferRelease(&d);CHECK(!d.RuntimeFramebuffer&&unmaps==1&&d.DisplayReservationEnd==end);
    CHECK(cursorReleases==1&&cursorReleaseUnmaps==0);
    ConnectorBufferRelease(&d);CHECK(unmaps==1);
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_DEVICE_STATE);CHECK(maps==1);
    for(i=0;i<5;++i){
        init(&d);if(i==0)full=0;if(i==1)d.DcnWriteEnabled=0;if(i==2)d.VramEnabled=0;
        if(i==3)d.VramWriteEnabled=0;if(i==4)offsetOk=0;
        CHECK(ConnectorBufferPrepare(&d)==STATUS_NOT_SUPPORTED);CHECK(!maps);empty(&d);
    }
    init(&d);d.VramLength=BC250_VRAM_TOP_RESERVED;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_NOT_SUPPORTED);CHECK(!maps);empty(&d);
    init(&d);d.Post.Pitch=0;CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    init(&d);postOffset=d.VramLength;CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    init(&d);d.Post.Pitch=UINT32_MAX;d.Post.Height=UINT32_MAX;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    init(&d);d.VramLength=start+CONNECTOR_BUFFER_BYTES+BC250_VRAM_TOP_RESERVED;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_SUCCESS);ConnectorBufferRelease(&d);
    init(&d);d.VramLength=start+CONNECTOR_BUFFER_BYTES+BC250_VRAM_TOP_RESERVED-1;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_NO_MEMORY);CHECK(!maps);empty(&d);
    init(&d);mapFails=1;CHECK(ConnectorBufferPrepare(&d)==STATUS_INSUFFICIENT_RESOURCES);CHECK(maps==1);empty(&d);
    init(&d);d.VramPhysical.QuadPart=-1;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    init(&d);d.VramPhysical.QuadPart=MAXLONGLONG-(LONGLONG)start-(LONGLONG)CONNECTOR_BUFFER_BYTES;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_SUCCESS);
    CHECK(mappedPhysical.QuadPart==MAXLONGLONG-(LONGLONG)CONNECTOR_BUFFER_BYTES);
    ConnectorBufferRelease(&d);
    init(&d);d.VramPhysical.QuadPart=MAXLONGLONG-(LONGLONG)start-(LONGLONG)CONNECTOR_BUFFER_BYTES+1;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    init(&d);d.VramPhysical.QuadPart=MAXLONGLONG-(LONGLONG)start+1;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    /* BD-341: no firmware display. With no firmware scan-out to keep clear, the buffer starts at the bottom of VRAM and
     * the reservation ends right after it. */
    init(&d);d.DisplayStart=BC250_DISPLAY_START_NO_POST;d.Post.Pitch=0;d.Post.Height=0;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_SUCCESS);CHECK(maps==1&&zeros==1);
    CHECK(mappedPhysical.QuadPart==d.VramPhysical.QuadPart&&d.DisplayReservationEnd==CONNECTOR_BUFFER_BYTES);
    ConnectorBufferRelease(&d);
    /* The VGA allocation (VGA engine on, or a pipe that could not be read): the buffer and its zero fill start above it. */
    init(&d);d.DisplayStart=BC250_DISPLAY_START_NO_POST;d.Post.Pitch=0;d.Post.Height=0;
    d.ColdScanoutBytes=BC250_DISPLAY_START_VGA_ALLOCATION;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_SUCCESS);CHECK(maps==1&&zeros==1);
    CHECK(mappedPhysical.QuadPart==d.VramPhysical.QuadPart+(LONGLONG)BC250_DISPLAY_START_VGA_ALLOCATION);
    CHECK(d.DisplayReservationEnd==BC250_DISPLAY_START_VGA_ALLOCATION+CONNECTOR_BUFFER_BYTES);
    ConnectorBufferRelease(&d);
    /* An off pipe whose HUBP0 still holds 1080 lines of 1920: amdgpu keeps them, so does the layout (64 KiB rounded). */
    init(&d);d.DisplayStart=BC250_DISPLAY_START_NO_POST;d.Post.Pitch=0;d.Post.Height=0;d.ColdScanoutBytes=1080ul*1920ul*4ul;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_SUCCESS);
    CHECK(mappedPhysical.QuadPart==d.VramPhysical.QuadPart+(LONGLONG)((1080ull*1920*4+65535)&~65535ull));
    ConnectorBufferRelease(&d);
    /* amdgpu reserves nothing when less than 8 MiB would remain. */
    init(&d);d.DisplayStart=BC250_DISPLAY_START_NO_POST;d.Post.Pitch=0;d.Post.Height=0;
    d.VramLength=(ULONGLONG)BC250_DISPLAY_START_VGA_ALLOCATION+(8ull<<20)-1;d.ColdScanoutBytes=BC250_DISPLAY_START_VGA_ALLOCATION;
    ULONGLONG o,b;CHECK(VramFramebufferExtent(&d,&o,&b)&&o==0&&b==0);
    init(&d);d.DisplayStart=BC250_DISPLAY_START_NO_POST;d.Post.Pitch=0;d.Post.Height=0;
    d.VramLength=CONNECTOR_BUFFER_BYTES+BC250_VRAM_TOP_RESERVED-1;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_NO_MEMORY);CHECK(!maps);empty(&d);
    init(&d);d.VramLength=UINT64_MAX;postOffset=(ULONGLONG)MAXLONGLONG;
    CHECK(ConnectorBufferPrepare(&d)==STATUS_INVALID_PARAMETER);CHECK(!maps);empty(&d);
    printf("connector buffer: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
