#define InterlockedAdd64(p,v) (*(p)+=(v))
typedef long long LONG64;

#include "paging_window.h"
#include "paging_intervals.h"
#include "paging_permutation.h"
#define BC250_GFX_TAG 0
#include "paging_aperture_state.h"
#define DXGK_OPERATION_MAP_APERTURE_SEGMENT 5
#define DXGK_OPERATION_UNMAP_APERTURE_SEGMENT 6
#include "paging_private.h"
#include "paging_pt_shadow.h"
#include "paging_graph_batch.h"
#include "paging_capture.h"
#define BC250_VIDMM_SHADOW_TAG 0
#define BC250_VIDMM_PAGING_VA_BYTES (1ull<<30)
#include "paging_stream.h"
#include "paging_mc.h"
#include "bc250_gart.h"
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef int BOOLEAN;
typedef unsigned int UINT;
#define TRUE 1
#define FALSE 0
#define PAGE_SIZE 4096
#define BC250_PAGING_MARKER_SLOT 5
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef enum {BC250PagingSupported,BC250PagingNotReady,BC250PagingNoTranslation,BC250PagingSystemMemory} BC250_WDDM_PAGING_UNSUPPORTED;
typedef struct {struct {long long QuadPart;} VramPhysical;ULONGLONG VramMcBase,VramLength;void*Gfx;int GfxPagingLock;BOOLEAN FullWddm,VramEnabled,VramWriteEnabled;struct {ULONG Pitch,Height;} Post;int GartLock;PAGING_APERTURE WddmAperture;} BC250_DEVICE;
typedef struct {BOOLEAN PagingWindowReady;PAGING_WINDOW PagingWindow;struct amdgpu_device*PagingDevicePtr;BOOLEAN PagingReady;struct amdgpu_ring*PagingRing;BOOLEAN PagingCpuBootstrap;struct bc250_mem PagingCopyStaging;} BC250_GFX;
static ULONGLONG translated;static int isSystem,translationOk=1,fragmented;
static int use_retained_walk,translation_by_offset;
static BOOLEAN VidMmTranslateRetainedPaging(ULONGLONG,ULONGLONG,ULONGLONG*,BOOLEAN*);
static unsigned virtual_graph_local_mask;
static int virtual_graph_map;static ULONGLONG virtual_graph_pages[6];
static int VidMmTranslatePaging(ULONGLONG r,ULONGLONG v,ULONGLONG*p,BOOLEAN*s){if(virtual_graph_map){unsigned page=(unsigned)(v>>12);if(page<16 || page>=22)return 0;*p=virtual_graph_pages[page-16]+(v&4095);*s=(virtual_graph_local_mask&(1u<<(page-16)))==0;return r==4096;}if(use_retained_walk)return VidMmTranslateRetainedPaging(r,v,p,s);(void)r;(void)v;*p=fragmented ? 0x100000ull+((v/4096)%17)*8192+(v&4095) : translated+(translation_by_offset?(v&4095):0);*s=isSystem;return translationOk;}

typedef long NTSTATUS;
typedef void* PVOID;
#define STATUS_NOT_SUPPORTED (-6)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_DEVICE_NOT_READY (-2)
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (-3)
static int gart_capture_lock,gart_capture_calls,gart_capture_unlocked;
static NTSTATUS gart_capture_status=STATUS_SUCCESS;
static void ExAcquireFastMutex(int* lock){(void)lock;gart_capture_lock++;}
static void ExReleaseFastMutex(int* lock){(void)lock;gart_capture_lock--;}
static NTSTATUS GartDevice(BC250_DEVICE* d,struct amdgpu_device** a,BOOLEAN* enabled)
{
 (void)d;gart_capture_calls++;
 if(gart_capture_lock!=1)gart_capture_unlocked++;
 *a=gart_capture_status==STATUS_SUCCESS?&g_adev:NULL;*enabled=FALSE;
 return gart_capture_status;
}
#define BC250_GFX_PAGING_BUFFER_BYTES 65536u
static int flush_lock_depth,flush_region_depth,cpu_lock_depth;
static void ExAcquirePushLockExclusive(int*p){(void)p;cpu_lock_depth++;}
static void ExReleasePushLockExclusive(int*p){(void)p;cpu_lock_depth--;}
static void KeEnterCriticalRegion(void){flush_region_depth++;}
static void KeLeaveCriticalRegion(void){flush_region_depth--;}
static void ExAcquirePushLockShared(int*p){(void)p;flush_lock_depth++;}
static void ExReleasePushLockShared(int*p){(void)p;flush_lock_depth--;}

#include "bc250_pte.h"
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define BC250_VIDMM_LEVELS 4u
#define BC250_VIDMM_PTES 512u
#define DXGK_PAGETABLEUPDATE_GPU_PHYSICAL 1
#define DXGK_PAGETABLEUPDATE_CPU_VIRTUAL 2
/* Field-level host model, not a WDK layout test; the full KMD build checks ABI. */
typedef struct {ULONG SegmentId;ULONGLONG SegmentOffset;} D3DGPU_PHYSICAL_ADDRESS;
typedef struct {ULONGLONG Flags,PageAddress;} DXGK_PTE;
typedef struct {
 UINT UpdateMode,PageTableLevel,NumPageTableEntries,StartIndex;
 struct {int Use64KBPages,Repeat;} Flags;
 union {D3DGPU_PHYSICAL_ADDRESS GpuPhysical;void*CpuVirtual;} PageTableAddress;
 const DXGK_PTE*pPageTableEntries;
} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
typedef struct {PAGING_APERTURE_STATE Aperture;PAGING_PT_SHADOW Shadow;PAGING_PT_SHADOW_SLOT GraphScratch;int CpuUpdateLock;ULONGLONG CpuEntries[512];BOOLEAN Ready,Write;ULONGLONG SegmentPhysical,SegmentLength;unsigned char*SegmentMapping;struct bc250_pte_context Pte;long Calls[4],CpuCalls,GpuCalls,BadCalls,Refused;long long Entries[4],Valid[4],Written,EncodedCoherentSystem,EncodedUncachedSystem,EncodedCoherencyMismatch;} BC250_VIDMM;
static BC250_VIDMM g_VidMm;
static enum bc250_pte_kind VidMmKind(UINT level){return level ? BC250_PTE_DIRECTORY : BC250_PTE_LEAF;}
static void KeMemoryBarrier(void){}

#include <excpt.h>
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_INSUFFICIENT_RESOURCES (-4)
#define BC250_VIDMM_LOG_CALLS 4
#define POOL_FLAG_PAGED 1
#define PAGE_READWRITE 2
#define PAGE_NOCACHE 4
typedef size_t SIZE_T;
typedef size_t ULONG_PTR;
typedef long long LONGLONG;
typedef struct {long long QuadPart;} PHYSICAL_ADDRESS;
static long TestIncrement(long*p){return ++*p;}
static long long TestIncrement64(long long*p){return ++*p;}
#define InterlockedIncrement TestIncrement
#define InterlockedIncrement64 TestIncrement64
static int cpu_map_fail,cpu_map_live,cpu_map_calls;
static ULONGLONG last_map_physical;static SIZE_T last_map_bytes;
__declspec(align(4096)) static u64 cpu_storage[4096];
#define cpu_table (cpu_storage+512)
static void*MmMapIoSpaceEx(PHYSICAL_ADDRESS p,SIZE_T bytes,unsigned protect){last_map_physical=(ULONGLONG)p.QuadPart;last_map_bytes=bytes;(void)protect;cpu_map_calls++;if(cpu_map_fail)return NULL;cpu_map_live++;return cpu_storage;}
static void MmUnmapIoSpace(void*p,SIZE_T bytes){(void)p;(void)bytes;cpu_map_live--;}
static void GuardLog(const char*format,...){(void)format;}

typedef unsigned char* PUCHAR;
#define MAXULONGLONG (~0ull)
#define PAGE_SHIFT 12
#define PASSIVE_LEVEL 0
static int KeGetCurrentIrql(void){return PASSIVE_LEVEL;}
static void ExInitializePushLock(int*p){*p=0;}
static unsigned cpu_write_setting=1;
static unsigned GuardReadSetting(const wchar_t*name,unsigned def){(void)name;(void)def;return cpu_write_setting;}
static ULONGLONG VidMmSystemLimit(void){return 0;}

static int shadow_pool_fail,shadow_pool_live;
static SIZE_T shadow_pool_last_bytes;
static void*ExAllocatePool2(unsigned flags,SIZE_T bytes,unsigned tag){void*p;(void)flags;(void)tag;if(shadow_pool_fail)return NULL;p=calloc(1,bytes);if(p){shadow_pool_live++;shadow_pool_last_bytes=bytes;}return p;}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)tag;free(p);shadow_pool_live--;}
static ULONGLONG override_cpu_physical;
static PHYSICAL_ADDRESS MmGetPhysicalAddress(void*p){PHYSICAL_ADDRESS a;a.QuadPart=(long long)(override_cpu_physical ? override_cpu_physical : g_VidMm.SegmentPhysical+(ULONGLONG)((unsigned char*)p-(unsigned char*)cpu_storage));return a;}
typedef enum {MmNonCached=0,MmCached=1,MmWriteCombined=2,MmMaximumCacheType=6} MEMORY_CACHING_TYPE;
static unsigned cache_attribute_calls;static ULONGLONG cache_attribute_physical;
static NTSTATUS MmGetCacheAttribute(PHYSICAL_ADDRESS physical,MEMORY_CACHING_TYPE* type) {
 check(cpu_lock_depth>0,"physical cache diagnostic runs under CPU table lifetime lock");
 cache_attribute_calls++;cache_attribute_physical=(ULONGLONG)physical.QuadPart;
 *type=MmNonCached;return STATUS_SUCCESS;
}


typedef struct {
 UINT NumPageTableEntries;ULONGLONG SrcPageTableAddress,DstPageTableAddress;
 UINT SrcStartPteIndex,DstStartPteIndex;
} DXGK_BUILDPAGINGBUFFER_COPY_RANGE;

// Match the anonymous struct/union syntax of the real WDK Transfer fields.
#pragma warning(push)
#pragma warning(disable:4201)
typedef union {
 struct {UINT Swizzle:1;UINT Unswizzle:1;UINT AllocationIsIdle:1;UINT TransferStart:1;UINT TransferEnd:1;UINT Reserved:27;};
 UINT Value;
} DXGK_TRANSFERFLAGS;
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
typedef union {struct {UINT CacheCoherent:1;UINT Reserved:31;};UINT Value;} DXGK_MAPAPERTUREFLAGS;
typedef struct {
 UINT Operation;
 void*pDmaBuffer;void*pDmaBufferPrivateData;
 ULONG DmaSize,DmaBufferPrivateDataSize,DmaBufferWriteOffset,MultipassOffset;
 struct {UINT NumRanges;DXGK_BUILDPAGINGBUFFER_COPY_RANGE*pRanges;} CopyPageTableEntries;
 struct {
  SIZE_T TransferSize;UINT TransferOffset,MdlOffset;DXGK_TRANSFERFLAGS Flags;
  struct {UINT SegmentId;union {PHYSICAL_ADDRESS SegmentAddress;void*pMdl;};} Source,Destination;
 } Transfer;
 struct {SIZE_T FillSize;UINT FillPattern;struct {UINT SegmentId;PHYSICAL_ADDRESS SegmentAddress;} Destination;} Fill;
 struct {UINT SegmentId;SIZE_T OffsetInPages,NumberOfPages;void* pMdl;DXGK_MAPAPERTUREFLAGS Flags;ULONG MdlOffset;} MapApertureSegment;
 struct {UINT SegmentId;SIZE_T OffsetInPages,NumberOfPages;PHYSICAL_ADDRESS DummyPage;} UnmapApertureSegment;
 struct {ULONGLONG TransferSizeInBytes,SourceVirtualAddress,DestinationVirtualAddress,AllocationOffsetInBytes;void* hAllocation;} TransferVirtual;
 struct {ULONGLONG FillSizeInBytes,DestinationVirtualAddress;ULONG FillPattern;} FillVirtual;
 ULONGLONG DmaBufferGpuVirtualAddress;
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE UpdatePageTable;
} DXGKARG_BUILDPAGINGBUFFER;
#pragma warning(pop)


#define BC250_VRAM_TOP_RESERVED 0x2000000ull
static int layout_fb_known=1;static ULONGLONG layout_fb_offset;
static BOOLEAN VramFramebufferOffset(const BC250_DEVICE*d,ULONGLONG*o){(void)d;*o=layout_fb_offset;return layout_fb_known;}

typedef unsigned char UCHAR;
#define STATUS_BUFFER_TOO_SMALL (-5)
#define BC250_WDDM_SEGMENT_APERTURE 2u
#define BC250_WDDM_APERTURE_BYTES 0x10000000ull
#define BC250_WDDM_PAGING_BUFFER_BYTES 0x10000ul
#define BC250_WDDM_LEVEL_COUNT 4u
#define BC250_WDDM_LEVEL_BITS 9u
#define BC250_WDDM_PAGE_TABLE_BYTES 4096u
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_TABLES 3u
static BOOLEAN g_ApertureOffered;
static int WddmAnswersLogged(const BC250_DEVICE*d){(void)d;return 0;}
typedef struct {struct {UINT CpuVisible,LocalBudgetGroup,DirectFlip,Aperture,CacheCoherent,Value;} Flags;PHYSICAL_ADDRESS BaseAddress,CpuTranslatedAddress;SIZE_T Size,CommitLimit;} DXGK_SEGMENTDESCRIPTOR4;
typedef struct {UINT NbSegment,SegmentDescriptorStride;void*pSegmentDescriptor;UINT PagingBufferSegmentId,PagingBufferSize,PagingBufferPrivateDataSize;} DXGK_QUERYSEGMENTOUT4;
typedef struct {UINT InputDataSize,OutputDataSize;void*pInputData;void*pOutputData;} DXGKARG_QUERYADAPTERINFO;
typedef struct {UINT LevelIndex;} DXGK_QUERYPAGETABLELEVELDESCIN;
typedef struct {UINT PageTableIndexBitCount,PageTableSizeInBytes,PageTableAlignmentInBytes,PageTableSegmentId,PagingProcessPageTableSegmentId;} DXGK_PAGE_TABLE_LEVEL_DESC;

#define BC250_IB_PROBE_DWORDS 240u
#define MM_COPY_MEMORY_PHYSICAL 1u
typedef union {PHYSICAL_ADDRESS PhysicalAddress;void*VirtualAddress;} MM_COPY_ADDRESS;
static int probe_copy_fail,probe_copy_short,probe_copy_calls;static u64 probe_copy_address;static SIZE_T probe_copy_bytes;
static NTSTATUS MmCopyMemory(void*dst,MM_COPY_ADDRESS src,SIZE_T bytes,ULONG flags,SIZE_T*done){unsigned i;check(flags==MM_COPY_MEMORY_PHYSICAL && flush_lock_depth>0,"physical diagnostic copy under table lifetime lock");probe_copy_calls++;probe_copy_address=(u64)src.PhysicalAddress.QuadPart;probe_copy_bytes=bytes;*done=probe_copy_short?bytes-4:bytes;for(i=0;i<*done/4;i++)((ULONG*)dst)[i]=0xABC00000u+i;return probe_copy_fail?STATUS_INVALID_PARAMETER:STATUS_SUCCESS;}

// MDL/PFN layout model: the full WDK build checks real platform types/macros.
typedef ULONGLONG PFN_NUMBER;
typedef struct {ULONG ByteCount,ByteOffset;} MDL,*PMDL;
#define MmGetMdlByteOffset(m) ((m)->ByteOffset)
#define MmGetMdlByteCount(m) ((m)->ByteCount)
#define MmGetMdlPfnArray(m) ((PFN_NUMBER*)((m)+1))

// Mdl != NULL selects OS PFNs; otherwise Address is already an MC address.
// Length bounds bytes from this endpoint's start; FirstPage applies only to MDLs.
typedef struct _BC250_PAGING_ENDPOINT {
    PMDL Mdl;
    ULONGLONG Address,Length;
    ULONG FirstPage;
    BOOLEAN Aperture;
} BC250_PAGING_ENDPOINT;
BOOLEAN GfxPagingEndpointValid(const BC250_DEVICE*,const BC250_PAGING_ENDPOINT*,ULONGLONG);

typedef struct _BC250_PAGING_APERTURE_OP {
    PMDL Mdl;
    ULONGLONG FirstPage,PageCount,DummyPhysical;
    ULONG MdlOffset;
    BOOLEAN Unmap,CacheCoherent;
} BC250_PAGING_APERTURE_OP;
typedef struct _BC250_PAGING_COPY_SLICE {
    ULONGLONG SourcePhysical,DestinationPhysical;
    ULONG Bytes;
    BOOLEAN SourceSystem,DestinationSystem;
} BC250_PAGING_COPY_SLICE;

BOOLEAN VidMmResolveAperture(ULONGLONG Mc, ULONG Bytes, ULONGLONG* Physical);
BOOLEAN VidMmApertureRangeValid(ULONGLONG Mc, ULONGLONG Bytes);

// Count actual graph planner calls in extracted builders; macro recursion suppression
// calls the real separately compiled implementation. Host instrumentation only.
static unsigned captured_graph_plan_calls;
#define PagingPageGraphPlan(...) (++captured_graph_plan_calls, PagingPageGraphPlan(__VA_ARGS__))
