/* Actual KMD constructors with field-level DDI inputs and lock-free host mocks. */
#define main packet_regression_main
#include "paging_packets.c"
#undef main
#include "paging_private.h"
#include "paging_stream.h"
#include "bc250_sdma_virtual_ptes.h"
typedef unsigned ULONG;
typedef unsigned long long ULONGLONG;
typedef long long LONG64;
typedef int BOOLEAN;
typedef int NTSTATUS;
typedef void* PVOID;
typedef unsigned char UCHAR;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER -2
#define NT_SUCCESS(s) ((s)>=0)
#define MAXULONGLONG (~0ull)
#define BC250_GFX_PAGING_BUFFER_BYTES 65536u
#define RtlZeroMemory(p,n) memset(p,0,n)
#define KeEnterCriticalRegion() ((void)0)
#define KeLeaveCriticalRegion() ((void)0)
#define ExAcquirePushLockShared(p) ((void)(p))
#define ExReleasePushLockShared(p) ((void)(p))
#define InterlockedIncrement64(p) (++*(p))
typedef struct { int PagingReady; struct amdgpu_ring* PagingRing; struct amdgpu_device* PagingDevicePtr; } BC250_GFX;
typedef struct { LONG64 PagingNativePtes; } BC250_WDDM;
typedef struct { void *Wddm,*Gfx; int GfxPagingLock; } BC250_DEVICE;
typedef struct {
    unsigned NumPageTableEntries;
    ULONGLONG SrcPageTableAddress,DstPageTableAddress;
    unsigned SrcStartPteIndex,DstStartPteIndex;
} DXGK_BUILDPAGINGBUFFER_COPY_RANGE;
typedef struct {
    void* pDmaBuffer; unsigned DmaSize;
    void* pDmaBufferPrivateData; unsigned DmaBufferPrivateDataSize;
    ULONGLONG DmaBufferGpuVirtualAddress;
    unsigned DmaBufferWriteOffset,MultipassOffset;
    struct {unsigned NumRanges; DXGK_BUILDPAGINGBUFFER_COPY_RANGE* pRanges;} CopyPageTableEntries;
} DXGKARG_BUILDPAGINGBUFFER;
static int modelTracked,translations,commits;
static BOOLEAN VidMmPagingRootTracked(ULONGLONG root){(void)root;return modelTracked;}
static BOOLEAN VidMmTranslatePaging(ULONGLONG root,ULONGLONG va,ULONGLONG* physical,BOOLEAN* system) {
    (void)root;++translations;*physical=va+0x100000;*system=FALSE;return TRUE;
}
static NTSTATUS VidMmCommitPagingCopy(ULONGLONG src,ULONGLONG dst,ULONG count) {
    check(src>=0x200100000ull && dst>=0x300100000ull && count>0,"logical metadata only uses translated identities");
    ++commits;return STATUS_SUCCESS;
}

static u64 address(const u32* p){return (u64)p[0]|((u64)p[1]<<32);}
