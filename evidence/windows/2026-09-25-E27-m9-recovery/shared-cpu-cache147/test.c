#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WDDM2_0
#include <ntddk.h>
#include <dispmprt.h>
#include "umd_blob.h"
#define BC250_WDDM_LOG_CALLS 8
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_APERTURE 2u
#define BC250_WDDM_SEGMENT_SET(id) (1u<<((id)-1))
#define BC250_WDDM_MAGIC_RESOURCE 1u
#define BC250_WDDM_MAGIC_ALLOCATION 2u
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul
#define WddmDdiCreateAllocation 1
static unsigned checks,failures,created;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u %s\n",(unsigned)__LINE__,#x);}}while(0)
typedef struct {ULONG Magic,Version,Width,Height,Pitch,Format;ULONGLONG Size;} BC250_WDDM_ALLOCATION_PRIVATE;
typedef struct {LONG UmdAllocRefused,UmdAllocs;} BC250_WDDM;
typedef struct {BC250_WDDM Wddm;} BC250_DEVICE;
typedef struct {BC250_WDDM_ALLOCATION_PRIVATE Allocation;BOOLEAN UmdAlloc;ULONGLONG UmdBytes,UmdRequestedVa;ULONG UmdHeap;} BC250_WDDM_OBJECT;
static BOOLEAN g_ApertureOffered=TRUE;
static BC250_WDDM* WddmOf(HANDLE adapter){return &((BC250_DEVICE*)adapter)->Wddm;}
static int WddmFirstCalls(BC250_WDDM* w,int id){(void)w;(void)id;return 0;}
static BC250_WDDM_OBJECT* WddmNewObject(BC250_DEVICE* d,ULONG magic){(void)d;(void)magic;created++;return calloc(1,sizeof(BC250_WDDM_OBJECT));}
static void WddmFreeObject(BC250_WDDM_OBJECT* o){free(o);}
static void GuardLog(const char* f,...){(void)f;}
static void WddmCpuVisibleAllocationFlags(DXGK_ALLOCATIONINFOFLAGS_WDDM2_0* Flags)
{
    // Reserved fields are inputs from dxgkrnl, not ours to zero. In particular,
    // sharing metadata must survive DxgkDdiCreateAllocation.
    Flags->CpuVisible = 1;
    Flags->PermanentSysMem = 0;
    Flags->Cached = 0;
    Flags->Protected = 0;
    Flags->ExistingSysMem = 0;
    Flags->ExistingKernelSysMem = 0;
    Flags->FromEndOfSegment = 0;
    Flags->DisableLargePageMapping = 0;
    Flags->Overlay = 0;
    Flags->Capture = 0;
    Flags->HistoryBuffer = 0;
    Flags->AccessedPhysically = 0;
    Flags->ExplicitResidencyNotification = 0;
    Flags->HardwareProtected = 0;
}
static NTSTATUS WddmSurfaceResourcePolicy(const void* Data, UINT Bytes,
                                         BOOLEAN* SharedCpu, BOOLEAN* CachedCpu)
{
    const ULONG* words=(const ULONG*)Data;
    *SharedCpu=FALSE;
    *CachedCpu=FALSE;
    if (!Data || Bytes<sizeof(ULONG) || words[0]!=0x52363245ul)
        return STATUS_SUCCESS; // unrelated private resource ABI / standard allocation
    if (Bytes<3*sizeof(ULONG) || words[2]>1)
        return STATUS_INVALID_PARAMETER;
    if (words[1]==1 && Bytes==3*sizeof(ULONG)) {
        *SharedCpu=(BOOLEAN)words[2];
        return STATUS_SUCCESS;
    }
    if (words[1]!=2 || Bytes!=4*sizeof(ULONG) || (words[3]&~3ul)!=0)
        return STATUS_INVALID_PARAMETER;
    *SharedCpu=(BOOLEAN)words[2];
    *CachedCpu=(BOOLEAN)(*SharedCpu && (words[3]&2ul)!=0 && (words[3]&1ul)==0);
    return STATUS_SUCCESS;
}
static NTSTATUS Bc250WddmCreateAllocation(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION* pCreateAllocation)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT i;
    BOOLEAN sharedCpu = FALSE, cachedCpu = FALSE;
    NTSTATUS resourceStatus=WddmSurfaceResourcePolicy(pCreateAllocation->pPrivateDriverData,
        pCreateAllocation->PrivateDriverDataSize,&sharedCpu,&cachedCpu);
    if (!NT_SUCCESS(resourceStatus)) return resourceStatus;

    for (i = 0; i < pCreateAllocation->NumAllocations; i++)
    {
        DXGK_ALLOCATIONINFO* info = &pCreateAllocation->pAllocationInfo[i];
        const BC250_WDDM_ALLOCATION_PRIVATE* private = (const BC250_WDDM_ALLOCATION_PRIVATE*)info->pPrivateDriverData;
        BC250_WDDM_OBJECT* object;

        if (info->PrivateDriverDataSize == sizeof(BC250_WDDM_ALLOCATION_PRIVATE) && private != NULL &&
            private->Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && private->Width == 64 && private->Height == 32)
            GuardLog("wddm: E26 shared control allocation reached KMD flags %x allocation flags %x bytes %llu format %u",
                     pCreateAllocation->Flags.Value, info->FlagsWddm2.Value, private->Size, private->Format);

        // M8: "BC2A" is a UMD allocation, beside the GDI "LB7A" below. The two magics differ on purpose.
        // requested_va is stored on the object and not programmed here: the winsys maps it afterwards.
        if (UmdBlobIsAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize))
        {
            struct umd_alloc_view view;
            int st = UmdBlobParseAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize, &view);
            UINT segment;
            UINT align;

            if (st != UMD_BLOB_OK || !g_ApertureOffered)
            {
                if (wddm != NULL && InterlockedIncrement(&wddm->UmdAllocRefused) <= BC250_WDDM_LOG_CALLS)
                    GuardLog("wddm: umd alloc refused, %s, private %u, segment %s",
                             st != UMD_BLOB_OK ? UmdBlobStatusText(st) : "no segment",
                             info->PrivateDriverDataSize, g_ApertureOffered ? "yes" : "no");
                while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
                return STATUS_INVALID_PARAMETER;
            }
            object = WddmNewObject(device, BC250_WDDM_MAGIC_ALLOCATION);
            if (object == NULL)
            {
                while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            object->UmdAlloc = TRUE;
            object->UmdBytes = view.bytes;
            object->UmdHeap = view.heap;
            object->UmdRequestedVa = view.requested_va;
            segment = (view.heap == UMD_BLOB_HEAP_GTT) ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;
            align = 4096;
            if (view.alignment >= 64 && view.alignment <= 0x100000ull && (view.alignment & (view.alignment - 1ull)) == 0)
                align = (UINT)view.alignment;
            info->hAllocation = object;
            info->Size = (SIZE_T)ROUND_TO_PAGES((SIZE_T)view.bytes);
            info->Alignment = align;
            info->HintedBank.Value = 0;
            info->MaximumRenamingListLength = 0;
            info->pAllocationUsageHint = NULL;
            info->PitchAlignedSize = 0;
            info->PreferredSegment.Value = 0;
            info->PreferredSegment.SegmentId0 = segment;
            info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(segment);
            info->SupportedWriteSegmentSet = BC250_WDDM_SEGMENT_SET(segment);
            info->EvictionSegmentSet = 0;
            info->PhysicalAdapterIndex = 0;
            WddmCpuVisibleAllocationFlags(&info->FlagsWddm2);
            // RADV's CPU_GTT_USWC requests write-combined storage; without it,
            // CPU-accessible GTT requires cached backing store. VidMm supplies
            // CacheCoherent PTEs, which the encoder maps to AMDGPU_PTE_SNOOPED.
            info->FlagsWddm2.Cached = (UINT)UmdBlobAllocCpuCached(&view);
            info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
            if (wddm != NULL && InterlockedIncrement(&wddm->UmdAllocs) <= BC250_WDDM_LOG_CALLS)
                GuardLog("wddm: umd alloc %llu bytes heap 0x%lX align %u va 0x%llX gem 0x%llX cached %u", view.bytes, view.heap, align,
                         view.requested_va,view.gem_flags,info->FlagsWddm2.Cached);
            continue;
        }

        // Stage A can only size an allocation it described itself. An unknown blob is an honest failure: nothing
        // in the never-fail list reaches this DDI, and guessing a size would put VidMm and us out of step.
        if (private == NULL || info->PrivateDriverDataSize < sizeof(*private) ||
            private->Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || private->Size == 0)
        {
            if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
                GuardLog("wddm: CreateAllocation %u of %u refused, private data %u bytes", i,
                         pCreateAllocation->NumAllocations, info->PrivateDriverDataSize);
            while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
            return STATUS_INVALID_PARAMETER;
        }

        object = WddmNewObject(device, BC250_WDDM_MAGIC_ALLOCATION);
        if (object == NULL)
        {
            while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        object->Allocation = *private;

        info->hAllocation = object;
        info->Size = (SIZE_T)ROUND_TO_PAGES(private->Size);
        // DXGK_ALLOCATIONINFO is an OUT array that nobody promised to zero: every member is written, as both
        // reference drivers do (Alignment 64 is theirs too).
        info->Alignment = 64;
        info->HintedBank.Value = 0;
        info->MaximumRenamingListLength = 0;
        info->pAllocationUsageHint = NULL;
        info->PitchAlignedSize = 0;                     // the aperture segment is not a pitch-aligned one
        info->PreferredSegment.Value = 0;
        info->PreferredSegment.SegmentId0 = sharedCpu ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;
        info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(info->PreferredSegment.SegmentId0);
        info->SupportedWriteSegmentSet = info->SupportedReadSegmentSet;
        info->EvictionSegmentSet = 0;                   // surfaces live in the local segment only; no eviction target
        info->PhysicalAdapterIndex = 0;
        WddmCpuVisibleAllocationFlags(&info->FlagsWddm2);
        // Linear VRAM blits use physical mappings. System-memory pages are not assumed contiguous.
        info->FlagsWddm2.AccessedPhysically = !sharedCpu;
        // Explicit v2 non-primary CPU-read intent only. V1 and standard LB7A
        // retain Cached=0. System aperture coherency is supplied through VidMm
        // CacheCoherent PTEs, not by changing local scanout mapping attributes.
        info->FlagsWddm2.Cached = cachedCpu &&
            info->PreferredSegment.SegmentId0 == BC250_WDDM_SEGMENT_APERTURE;
        info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    if (pCreateAllocation->Flags.Resource && pCreateAllocation->hResource == NULL)
    {
        pCreateAllocation->hResource = WddmNewObject(device, BC250_WDDM_MAGIC_RESOURCE);
        if (pCreateAllocation->hResource == NULL)
        {
            for (i = 0; i < pCreateAllocation->NumAllocations; i++)
                WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
        GuardLog("wddm: CreateAllocation %u allocations, flags %x resource %s", pCreateAllocation->NumAllocations,
                 pCreateAllocation->Flags.Value, pCreateAllocation->hResource != NULL ? "yes" : "no");
    return STATUS_SUCCESS;
}
static void surface_case(ULONG magic,ULONG version,ULONG shared,ULONG access,UINT bytes,UINT reserved)
{
 BC250_DEVICE device={0};ULONG resource[5]={magic,version,shared,access,0};
 BC250_WDDM_ALLOCATION_PRIVATE data={0x4137424Cul,1,64,32,256,D3DDDIFMT_A8R8G8B8,8192};
 DXGK_ALLOCATIONINFO info={0},before;DXGKARG_CREATEALLOCATION create={0};NTSTATUS status;
 BOOLEAN recognized=(BOOLEAN)(bytes>=4 && magic==0x52363245ul);
 BOOLEAN valid=(BOOLEAN)(!recognized || (shared<=1 && ((version==1 && bytes==12) || (version==2 && bytes==16 && access<4))));
 BOOLEAN aperture=(BOOLEAN)(recognized && valid && shared==1);
 BOOLEAN cached=(BOOLEAN)(aperture && version==2 && (access&2) && !(access&1));
 info.FlagsWddm2.Value=reserved; info.pPrivateDriverData=&data;info.PrivateDriverDataSize=sizeof(data);before=info;
 create.pPrivateDriverData=resource;create.PrivateDriverDataSize=bytes;create.NumAllocations=1;create.pAllocationInfo=&info;
 created=0;status=Bc250WddmCreateAllocation(&device,&create);
 CHECK(status==(valid?STATUS_SUCCESS:STATUS_INVALID_PARAMETER));
 if(valid){
  CHECK(created==1);CHECK(info.FlagsWddm2.Cached==cached);CHECK(info.FlagsWddm2.CpuVisible==1);
  CHECK((info.FlagsWddm2.Value&reserved)==reserved);CHECK(info.FlagsWddm2.AccessedPhysically==(UINT)!aperture);
  CHECK(info.PreferredSegment.SegmentId0==(aperture?2u:1u));CHECK(info.SupportedReadSegmentSet==(aperture?2u:1u));
  CHECK(info.SupportedWriteSegmentSet==info.SupportedReadSegmentSet);CHECK(info.Size==8192);
  WddmFreeObject(info.hAllocation);
 }else{CHECK(created==0);CHECK(memcmp(&before,&info,sizeof(info))==0);CHECK(create.hResource==NULL);}
}
static void bc2a(void)
{
 ULONG version,heap,flags;
 for(version=1;version<=2;version++)for(heap=2;heap<=4;heap+=2)for(flags=0;flags<8;flags++){
  BC250_DEVICE device={0};ULONGLONG raw[24]={0};ULONG *d=(ULONG*)raw;
  DXGK_ALLOCATIONINFO info={0};DXGKARG_CREATEALLOCATION create={0};ULONG otherResource[4]={0xdeadbeef,2,1,2};
  d[0]=UMD_BLOB_ALLOC_MAGIC;d[1]=version;d[2]=sizeof(raw);raw[2]=8192;raw[3]=4096;d[8]=heap;raw[5]=flags;
  info.pPrivateDriverData=raw;info.PrivateDriverDataSize=sizeof(raw);info.FlagsWddm2.Reserved0=1;
  create.NumAllocations=1;create.pAllocationInfo=&info;create.pPrivateDriverData=otherResource;create.PrivateDriverDataSize=sizeof(otherResource);
  CHECK(Bc250WddmCreateAllocation(&device,&create)==STATUS_SUCCESS);
  CHECK(info.FlagsWddm2.Cached==(UINT)(version==2 && heap==2 && !(flags&6)));
  CHECK(info.FlagsWddm2.Reserved0==1);CHECK(info.PreferredSegment.SegmentId0==(heap==2?2u:1u));
  WddmFreeObject(info.hAllocation);
 }
}
int main(void)
{
 UINT version,shared,access,bytes,r;DXGK_ALLOCATIONINFOFLAGS_WDDM2_0 flag={0};UINT reserved[3];
 reserved[0]=0;flag.Reserved0=1;reserved[1]=flag.Value;flag.DXGK_ALLOC_RESERVED16=1;flag.DXGK_ALLOC_RESERVED17=1;reserved[2]=flag.Value;
 C_ASSERT(sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32);
 for(r=0;r<3;r++)for(version=0;version<4;version++)for(shared=0;shared<3;shared++)for(access=0;access<8;access++)for(bytes=0;bytes<=20;bytes++)
  surface_case(0x52363245,version,shared,access,bytes,reserved[r]);
 surface_case(0xdeadbeef,2,1,2,16,reserved[2]);surface_case(0,0,0,0,0,reserved[2]);
 bc2a();printf("Actual CreateAllocation: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
