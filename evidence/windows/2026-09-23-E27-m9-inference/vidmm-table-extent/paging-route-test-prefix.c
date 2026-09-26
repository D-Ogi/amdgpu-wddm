
#include "paging_window.h"
#include "paging_private.h"
#include "paging_pt_shadow.h"
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
typedef struct {struct {long long QuadPart;} VramPhysical;ULONGLONG VramMcBase,VramLength;void*Gfx;int GfxPagingLock;BOOLEAN FullWddm,VramEnabled,VramWriteEnabled;} BC250_DEVICE;
typedef struct {BOOLEAN PagingWindowReady;PAGING_WINDOW PagingWindow;struct amdgpu_device*PagingDevicePtr;BOOLEAN PagingReady;struct amdgpu_ring*PagingRing;BOOLEAN PagingCpuBootstrap;struct bc250_mem PagingCopyStaging;} BC250_GFX;
static ULONGLONG translated;static int isSystem,translationOk=1,fragmented;
static int use_retained_walk,translation_by_offset;
static BOOLEAN VidMmTranslateRetainedPaging(ULONGLONG,ULONGLONG,ULONGLONG*,BOOLEAN*);
static int VidMmTranslatePaging(ULONGLONG r,ULONGLONG v,ULONGLONG*p,BOOLEAN*s){if(use_retained_walk)return VidMmTranslateRetainedPaging(r,v,p,s);(void)r;(void)v;*p=fragmented ? 0x100000ull+((v/4096)%17)*8192+(v&4095) : translated+(translation_by_offset?(v&4095):0);*s=isSystem;return translationOk;}

typedef long NTSTATUS;
typedef void* PVOID;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_DEVICE_NOT_READY (-2)
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (-3)
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
typedef struct {PAGING_PT_SHADOW Shadow;int CpuUpdateLock;ULONGLONG CpuEntries[512];BOOLEAN Ready,Write;ULONGLONG SegmentPhysical,SegmentLength;unsigned char*SegmentMapping;struct bc250_pte_context Pte;long Calls[4],CpuCalls,GpuCalls,BadCalls,Refused;long long Entries[4],Valid[4],Written;} BC250_VIDMM;
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
static void*ExAllocatePool2(unsigned flags,SIZE_T bytes,unsigned tag){void*p;(void)flags;(void)tag;if(shadow_pool_fail)return NULL;p=calloc(1,bytes);if(p)shadow_pool_live++;return p;}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)tag;free(p);shadow_pool_live--;}
static PHYSICAL_ADDRESS MmGetPhysicalAddress(void*p){PHYSICAL_ADDRESS a;a.QuadPart=(long long)(g_VidMm.SegmentPhysical+(ULONGLONG)((unsigned char*)p-(unsigned char*)cpu_storage));return a;}

typedef struct {
 UINT NumPageTableEntries;ULONGLONG SrcPageTableAddress,DstPageTableAddress;
 UINT SrcStartPteIndex,DstStartPteIndex;
} DXGK_BUILDPAGINGBUFFER_COPY_RANGE;

#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
typedef struct {
 void*pDmaBuffer;void*pDmaBufferPrivateData;
 ULONG DmaSize,DmaBufferPrivateDataSize,DmaBufferWriteOffset,MultipassOffset;
 struct {UINT NumRanges;DXGK_BUILDPAGINGBUFFER_COPY_RANGE*pRanges;} CopyPageTableEntries;
 ULONGLONG DmaBufferGpuVirtualAddress;
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE UpdatePageTable;
} DXGKARG_BUILDPAGINGBUFFER;

