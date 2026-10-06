// SPDX-License-Identifier: MIT
#include "heap-import.h"
#include "device-state.h"
#include "allocation-request.h"
#include <bc250_host_bootstrap.h>
#include <d3dkmthk.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
using namespace native12;
template<class T> T handle(uintptr_t n){return reinterpret_cast<T>(n);}
static std::string events;
static UINT64 completed=10;
static UINT64 gpu_address=UINT64_C(0x100000000);
static D3DKMT_HANDLE next_allocation=50;
static bool pending=false,fail_free=false,fail_import=false,unlock_on_free=false,fail_deallocate=false;
static bool resident_pending=false,fail_wait=false,fail_evict=false;
static HRESULT resident_result=S_OK;
static unsigned surfaces=0;
static unsigned long surface_format=0;
static uint32_t surface_width=256;            // LB7A.Width the next primary must carry
// BD-075, what the next shared create must publish: the LB7A geometry and the E26R v3 texture beside it.
static unsigned shared_created=0;
static bool refuse_shareable_create=false;    // the runtime's E_INVALIDARG for a shared resource's first attempt
static uint32_t shared_width=256,shared_height=64,shared_pitch=1024,shared_dxgi=0,shared_bind=0;
static uint64_t shared_size=65536;
static unsigned creates=0,makes_resident=0,evictions=0,waits=0,probes=0;
static uint32_t expected_type=0;
static void* identity=handle<void*>(0x5432);
static RuntimeHeapImports* imports;
// The device-wide progress source of the release gate (device-progress.h), as the hosted dispatch
// implements it: one mark, retired or not as the test says.
static bool progress_retired_flag=true;
static unsigned snapshots=0;
static void progress_snapshot_cb(void*,ProgressSnapshot* out) noexcept {
 ++snapshots;*out={};out->count=1;out->marks[0]={1,7};
}
static bool progress_retired_cb(void*,const ProgressSnapshot* snapshot) noexcept {
 return snapshot && snapshot->count==1 && snapshot->marks[0].value==7 && progress_retired_flag;
}
static char mapped[65536];
static HRESULT APIENTRY allocate_cb(D3D12DDI_HRTDEVICE d,D3D12DDICB_ALLOCATE_0022* a){
 assert(d.handle==handle<void*>(1) && a->hResource==handle<void*>(2) && !a->hKMResource);
 assert(a->NumAllocations==1);
 if(a->pAllocationInfo->PrivateDriverDataSize==32 && a->PrivateDriverDataSize==64){
  // BD-075, the shared surface: the same 32-byte LB7A v1 description, but an ordinary allocation (no
  // PRIMARY intent, video present source 0) under the 64-byte E26R v3 record. Read as the words on the
  // wire: Shared 1, Access 0, then the serialized D3D11_TEXTURE2D_DESC1 an opener rebuilds from.
  assert(a->pPrivateDriverData);
  assert(a->pAllocationInfo->Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE);
  assert(!a->pAllocationInfo->VidPnSourceId);
  uint32_t w[8];std::memcpy(w,a->pAllocationInfo->pPrivateDriverData,sizeof(w));
  assert(w[0]==0x4137424Cu && w[1]==1 && w[2]==shared_width && w[3]==shared_height && w[4]==shared_pitch);
  assert(w[5]==surface_format && w[6]==uint32_t(shared_size) && !w[7]);
  uint32_t r[16];std::memcpy(r,a->pPrivateDriverData,sizeof(r));
  assert(r[0]==0x52363245u && r[1]==3 && r[2]==1 && !r[3]);
  assert(r[4]==shared_width && r[5]==shared_height && r[6]==1 && r[7]==1 && r[8]==shared_dxgi);
  assert(r[9]==1 && !r[10] && !r[11] && r[12]==shared_bind && !r[13] && !r[14] && !r[15]);
  ++shared_created;events+='A';a->pAllocationInfo->hAllocation=++next_allocation;return S_OK;
 }
 if(a->pAllocationInfo->PrivateDriverDataSize==32){
  // The primary: the 32-byte LB7A v1 description, PRIMARY, no video present source, under the
  // 12-byte E26R v1 record with shared 1. Read as the words on the wire, not through the
  // producer's own structures.
  assert(a->pPrivateDriverData && a->PrivateDriverDataSize==12);
  uint32_t e[3];std::memcpy(e,a->pPrivateDriverData,sizeof(e));
  assert(e[0]==0x52363245u && e[1]==1 && e[2]==1);
  uint32_t w[8];std::memcpy(w,a->pAllocationInfo->pPrivateDriverData,sizeof(w));
  assert(w[0]==0x4137424Cu && w[1]==1 && w[2]==surface_width && w[3]==64 && w[4]==1024);
  assert(w[5]==surface_format && w[6]==65536 && w[7]==0);
  assert(a->pAllocationInfo->Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY);
  assert(a->pAllocationInfo->VidPnSourceId==D3DDDI_ID_UNINITIALIZED);
  ++surfaces;events+='A';a->pAllocationInfo->hAllocation=++next_allocation;return S_OK;
 }
 assert(!a->pPrivateDriverData && !a->PrivateDriverDataSize);
 assert(a->pAllocationInfo->Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE && !a->pAllocationInfo->VidPnSourceId);
 assert(a->pAllocationInfo->PrivateDriverDataSize==sizeof(bc250_umd_alloc_private));
 auto blob=static_cast<const bc250_umd_alloc_private*>(a->pAllocationInfo->pPrivateDriverData);
 assert(blob->alloc_size==65536 && blob->phys_alignment==65536);
 assert(blob->preferred_heap==uint32_t(expected_type?AMDGPU_GEM_DOMAIN_GTT:AMDGPU_GEM_DOMAIN_VRAM));
 assert(blob->gem_flags==(expected_type==0?uint64_t(AMDGPU_GEM_CREATE_NO_CPU_ACCESS):
     expected_type==1?uint64_t(AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED|AMDGPU_GEM_CREATE_CPU_GTT_USWC):
     uint64_t(AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)));
 events+='A';
 // BD-075: the runtime's measured answer to the allocate callback of a shared resource whose allocation
 // carries no resource-level private data (lab-20261005T172552Z, ddi.log line 50860).
 if(refuse_shareable_create)return E_INVALIDARG;
 a->pAllocationInfo->hAllocation=++next_allocation;return S_OK;
}
// D: by handle list. R: by the runtime resource. Both with the resident object's two flags.
static HRESULT APIENTRY deallocate_cb(D3D12DDI_HRTDEVICE,const D3D12DDICB_DEALLOCATE_0022* a){
 if(a->hResource){
  assert(a->hResource==handle<void*>(2) && !a->NumAllocations && !a->HandleList && unsigned(a->Flags)==3u);
  events+='R';return fail_deallocate?E_FAIL:S_OK;
 }
 assert(a->NumAllocations==1 && a->HandleList && unsigned(a->Flags)==3u);events+='D';return fail_deallocate?E_FAIL:S_OK;
}
static HRESULT APIENTRY paging_create(HANDLE d,D3DDDICB_CREATEPAGINGQUEUE* a){assert(d==handle<void*>(1));++creates;a->hPagingQueue=9;a->hSyncObject=10;a->FenceValueCPUVirtualAddress=&completed;return S_OK;}
static HRESULT APIENTRY paging_destroy(HANDLE,const D3DDDI_DESTROYPAGINGQUEUE*){events+='P';return S_OK;}
static HRESULT APIENTRY map_cb(HANDLE d,D3DDDI_MAPGPUVIRTUALADDRESS* a){assert(d==handle<void*>(1) && a->SizeInPages==16);events+='M';a->VirtualAddress=gpu_address;a->PagingFenceValue=20;return pending?E_PENDING:S_OK;}
static UINT64 memory_va_bias=0;
static HRESULT APIENTRY unmap_cb(HANDLE,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){assert(a->BaseAddress==gpu_address+memory_va_bias && a->Size==65536);events+='U';return fail_free?E_FAIL:S_OK;}
// Z: the import's own residency reference, on its paging queue, for the allocation it just mapped.
// E: that reference released before the VA. W: the CPU wait for the largest pending value.
static HRESULT APIENTRY make_cb(HANDLE d,D3DDDI_MAKERESIDENT* a){
 assert(d==handle<void*>(1) && a->hPagingQueue==9 && a->NumAllocations==1 && a->AllocationList);
 // CantTrimFurther only: D3D12's default residency succeeds over the current budget.
 assert(a->AllocationList[0]==next_allocation && a->Flags.Value==1 && a->Flags.CantTrimFurther && !a->PriorityList);
 ++makes_resident;events+='Z';
 if(resident_result!=S_OK)return resident_result;
 if(resident_pending){a->PagingFenceValue=30;return E_PENDING;}
 return S_OK;
}
static HRESULT APIENTRY evict_cb(HANDLE d,D3DDDICB_EVICT* a){
 assert(d==handle<void*>(1) && a->NumAllocations==1 && a->AllocationList && a->AllocationList[0] && !a->Flags.Value);
 events+='E';if(fail_evict)return E_FAIL;++evictions;return S_OK;
}
static HRESULT APIENTRY wait_cpu_cb(HANDLE d,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* a){
 assert(d==handle<void*>(1) && a->ObjectCount==1 && a->ObjectHandleArray[0]==10 && !a->hAsyncEvent && !a->Flags.Value);
 events+='W';++waits;
 // The largest pending value: the residency's when it is pending, else the mapping's.
 assert(a->FenceValueArray[0]==(resident_pending?30u:20u));
 if(fail_wait)return E_FAIL;
 completed=a->FenceValueArray[0];return S_OK;
}
static HRESULT APIENTRY lock_actual(HANDLE,D3DDDICB_LOCK2* a){a->pData=mapped;events+='L';return S_OK;}
static HRESULT APIENTRY unlock_cb(HANDLE,const D3DDDICB_UNLOCK2*){events+='N';return S_OK;}
static void VKAPI_CALL memory_properties(VkPhysicalDevice d,VkPhysicalDeviceMemoryProperties* out){
 assert(d==handle<VkPhysicalDevice>(3));*out={};out->memoryTypeCount=4;
 out->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
 out->memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 out->memoryTypes[2].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
 out->memoryTypes[3].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}
static VkResult VKAPI_CALL buffer_create(VkDevice d,const VkBufferCreateInfo* info,const VkAllocationCallbacks*,VkBuffer* out){
 assert(d==handle<VkDevice>(4) && info->size==4096 && (info->usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT));++probes;*out=handle<VkBuffer>(7);return VK_SUCCESS;
}
static void VKAPI_CALL buffer_destroy(VkDevice,VkBuffer,const VkAllocationCallbacks*){}
static void VKAPI_CALL buffer_requirements(VkDevice,VkBuffer,VkMemoryRequirements* out){*out={65536,65536,15};}
static VkResult VKAPI_CALL memory_allocate(VkDevice d,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks*,VkDeviceMemory* out){
 assert(d==handle<VkDevice>(4) && info->allocationSize==65536 && info->memoryTypeIndex==expected_type);
 auto flags=static_cast<const VkMemoryAllocateFlagsInfo*>(info->pNext);
 assert(flags->sType==VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO && flags->flags==VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);
 auto host=static_cast<const bc250_host_import*>(flags->pNext);
 assert(host->sType==BC250_HOST_IMPORT_FLAGS_STYPE && !host->pNext && host->identity==identity && host->allocation==next_allocation);
 assert(host->va==gpu_address && host->size==65536);
 // Only a CPU-visible heap asks the ICD to map it; type 0 is the GPU-only policy.
 assert(host->flags==(info->memoryTypeIndex?BC250_HOST_IMPORT_CPU_MAP:0u));
 events+='I';if(fail_import)return VK_ERROR_OUT_OF_DEVICE_MEMORY;
 *out=handle<VkDeviceMemory>(0x1000+next_allocation);return VK_SUCCESS;
}
static void VKAPI_CALL memory_free(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){
 events+='V';if(unlock_on_free){D3DKMT_UNLOCK2 args{};args.hAllocation=next_allocation;assert(imports->dispatch(BC250_HOST_Unlock2,&args)==S_OK);}
}
static PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice,const char* name){
#define FN(n,f) if(!std::strcmp(name,n))return reinterpret_cast<PFN_vkVoidFunction>(f)
 FN("vkAllocateMemory",memory_allocate);FN("vkFreeMemory",memory_free);FN("vkCreateBuffer",buffer_create);
 FN("vkDestroyBuffer",buffer_destroy);FN("vkGetBufferMemoryRequirements",buffer_requirements);return nullptr;
#undef FN
}
static PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance i,const char* name){
 assert(i==handle<VkInstance>(5));
 if(!std::strcmp(name,"vkGetPhysicalDeviceMemoryProperties"))return reinterpret_cast<PFN_vkVoidFunction>(memory_properties);
 if(!std::strcmp(name,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(gdpa);return nullptr;
}
int main(){
 Device device{};device.runtime={handle<void*>(1)};device.callbacks.pfnAllocateCb=allocate_cb;device.callbacks.pfnDeallocateCb=deallocate_cb;
 auto& k=device.kernel_callbacks;k.pfnCreatePagingQueueCb=paging_create;k.pfnDestroyPagingQueueCb=paging_destroy;k.pfnMapGpuVirtualAddressCb=map_cb;
 k.pfnFreeGpuVirtualAddressCb=unmap_cb;k.pfnMakeResidentCb=make_cb;k.pfnLock2Cb=lock_actual;k.pfnUnlock2Cb=unlock_cb;
 k.pfnEvictCb=evict_cb;k.pfnWaitForSynchronizationObjectFromCpuCb=wait_cpu_cb;
 bc250::umd::RuntimeDomain domain;
 // The whole suite below runs with the release gate off (ImportReleasePolicy::off(), the two experiment
 // switches import-progress-gate-off and import-quarantine-off): every event string here is adapter106's,
 // so the suite is also the proof that the switches off reproduce it. The gate itself is tested at the end.
 RuntimeHeapImports owner(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,
                          ImportReleasePolicy::off());imports=&owner;
 assert(owner.initialize()==E_UNEXPECTED);bc250::umd::RuntimeDomain::Scope scope(domain);assert(owner.initialize()==S_OK);
 D3D12DDIARG_CREATEHEAP_0001 heap{};heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12DDIARG_CREATERESOURCE_0088 resource{};resource.ResourceType=D3D12DDI_RT_BUFFER;
 engine_ddi::MemoryRequest req{};req.size=sizeof(req);req.rt_owner={handle<void*>(2)};req.heap=&heap;req.resource=&resource;req.byte_size=4096;req.alignment=65536;req.flags=engine_ddi::kMemoryDedicated;
 engine_ddi::ImportedMemory memory{};
 for(expected_type=0;expected_type<3;++expected_type){
 heap.CPUPageProperty=expected_type==0?D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE:expected_type==1?D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE:D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
 events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMZI" && memory.allocation==next_allocation && memory.memory_type_index==expected_type);
 assert(owner.owns_allocation(memory.allocation) && owner.close_after_engine_retirement()==E_PENDING);
 if(expected_type){D3DKMT_LOCK2 lock{};lock.hAllocation=memory.allocation;assert(owner.dispatch(BC250_HOST_Lock2,&lock)==S_OK && lock.pData==mapped);unlock_on_free=true;}
 assert(owner.free(&memory)==S_OK);assert(events==(expected_type?"AMZILVNEUD":"AMZIVEUD"));unlock_on_free=false;
 assert(!owner.owns_allocation(memory.allocation) && owner.free(&memory)==E_INVALIDARG);
 }
 // Both CPU-visible L0 policies accept the explicit coherent flag, including
 // heaps allowing every resource category. Actual non-buffer resources still fail.
 heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_BUFFERS|D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES|
     D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE);
 heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 for(expected_type=1;expected_type<=2;++expected_type){
  heap.CPUPageProperty=expected_type==1?D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE:D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
  events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMZI" && memory.memory_type_index==expected_type);
  assert(owner.free(&memory)==S_OK && events=="AMZIVEUD");
 }
 // A cached, visible type alone cannot fulfill COHERENT_SYSTEMWIDE. Nor can it
 // fulfill the existing upload policy, which already requires HOST_COHERENT.
 req.memory_type_bits=1u<<3;events.clear();const auto before_allocation=next_allocation;
 assert(owner.allocate(&req,&memory)==E_INVALIDARG && !memory.memory && events.empty() && next_allocation==before_allocation);
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE;
 assert(owner.allocate(&req,&memory)==E_INVALIDARG && !memory.memory && events.empty() && next_allocation==before_allocation);
 heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;expected_type=3;
 assert(owner.allocate(&req,&memory)==S_OK && events=="AMZI" && memory.memory_type_index==3);
 assert(owner.free(&memory)==S_OK && events=="AMZIVEUD");req.memory_type_bits=0;
 const auto coherent_flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_BUFFERS|D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE);
 heap.Flags=coherent_flags;
 // Every refusal taken before a probe names the check that took it in the report (BD-075: that name and the
 // heap flags beside it are what a shared create has to be read off a lab log), and nothing is allocated.
 auto reject_before_probe=[&](const char* why,uint32_t unimplemented=0){
  const auto old_probes=probes;const auto old_allocation=next_allocation;events.clear();
  assert(owner.allocate(&req,&memory)==E_NOTIMPL && !memory.memory && events.empty());
  assert(probes==old_probes && next_allocation==old_allocation);
  const auto& report=owner.last_report();
  assert(report.refusal && !std::strcmp(report.refusal,why) && report.unimplemented_heap_flags==unimplemented);
 };
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;reject_before_probe("systemwide coherency");
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
 heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;reject_before_probe("systemwide coherency");
 heap.MemoryPool=static_cast<D3D12DDI_MEMORY_POOL>(2);reject_before_probe("systemwide coherency");
 heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 heap.CreationNodeMask=2;reject_before_probe("node mask");heap.CreationNodeMask=1;
 heap.VisibleNodeMask=2;reject_before_probe("node mask");heap.VisibleNodeMask=1;
 heap.Flags=D3D12DDI_HEAP_FLAGS(unsigned(coherent_flags)|0x80000000u);
 reject_before_probe("heap flags",0x80000000u);
 // The shape of a shared create: the heap flag the D3D12 runtime adds is undefined in the DDI, so each
 // candidate bit is declined by the same check, under its own name, with the bit in the report.
 for(const uint32_t bit:{0x1u,0x40u}){
  heap.Flags=D3D12DDI_HEAP_FLAGS(unsigned(coherent_flags)|bit);reject_before_probe("heap flags",bit);
 }
 resource.ResourceType=D3D12DDI_RT_TEXTURE2D;heap.Flags=coherent_flags;
 reject_before_probe("texture on this heap");resource.ResourceType=D3D12DDI_RT_BUFFER;
 heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
 assert(creates==1 && makes_resident && makes_resident==evictions);
 expected_type=0;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
 // A committed texture on a heap without CPU access is raw memory like a buffer.
 for(const auto type:{D3D12DDI_RT_TEXTURE1D,D3D12DDI_RT_TEXTURE2D,D3D12DDI_RT_TEXTURE3D})
 for(const auto flags:{D3D12DDI_HEAP_FLAG_BUFFERS,D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES,D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES}){
  resource.ResourceType=type;
  heap.Flags=flags;events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMZI" && memory.memory_type_index==0);
  assert(owner.free(&memory)==S_OK && events=="AMZIVEUD");
 }
 resource.ResourceType=D3D12DDI_RT_TEXTURE2D;
 // System-wide coherence alone refuses an otherwise admitted texture.
 heap.Flags=D3D12DDI_HEAP_FLAGS(unsigned(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)|unsigned(D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE));
 reject_before_probe("texture on this heap");
 // Refused before any probe or allocation: a texture that is not the heap's one resource, a texture on a
 // CPU-visible heap, an unknown resource type, and a heap for buffers that does not allow buffers.
 heap.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
 req.flags=0;reject_before_probe("texture on this heap");req.flags=engine_ddi::kMemoryDedicated;
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;reject_before_probe("texture on this heap");
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE;reject_before_probe("texture on this heap");
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
 resource.ResourceType=static_cast<D3D12DDI_RESOURCE_TYPE>(0x7f);reject_before_probe("resource type");
 resource.ResourceType=D3D12DDI_RT_BUFFER;reject_before_probe("buffer heap");
 req.resource=nullptr;reject_before_probe("buffer heap");req.resource=&resource;
 heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
 req.flags=engine_ddi::kMemoryPrimary;reject_before_probe("request flags");req.flags=engine_ddi::kMemoryDedicated;
 req.memory_type_bits=2;assert(owner.allocate(&req,&memory)==E_INVALIDARG);req.memory_type_bits=0;
 gpu_address+=4096;events.clear();assert(owner.allocate(&req,&memory)==E_INVALIDARG && events=="AMZEUD" && !memory.memory);gpu_address-=4096;
 events.clear();fail_import=true;assert(owner.allocate(&req,&memory)==E_OUTOFMEMORY && !memory.memory && events=="AMZIEUD");fail_import=false;
 events.clear();assert(owner.allocate(&req,&memory)==S_OK);fail_free=true;assert(owner.free(&memory)==E_FAIL && events=="AMZIVEU");
 fail_free=false;assert(owner.close_after_engine_retirement()==S_OK && events=="AMZIVEUUDP");
 // The runtime refuses the release by handle list: the record and its allocation stay, and the later
 // cleanup asks again with the same flags, without a second Vulkan free or unmap.
 events.clear();assert(owner.allocate(&req,&memory)==S_OK);fail_deallocate=true;
 assert(owner.free(&memory)==E_FAIL && events=="AMZIVEUD" && owner.owns_allocation(memory.allocation));
 assert(owner.last_free_report().stage==FreeStage::Deallocate && !owner.last_free_report().owner_expired);
 events.clear();assert(owner.close_after_engine_retirement()==E_FAIL && events=="D" && owner.owns_allocation(memory.allocation));
 // Owned for its cleanup, but retired: it is no backing for a new view.
 assert(!owner.borrow_backing(memory.allocation));
 fail_deallocate=false;events.clear();
 assert(owner.close_after_engine_retirement()==S_OK && events=="DP" && !owner.owns_allocation(memory.allocation));
 // Backing lent to a runtime callback: its release waits for every return and changes nothing before.
 events.clear();assert(owner.allocate(&req,&memory)==S_OK && !owner.borrow_backing(memory.allocation+1));
 assert(owner.borrow_backing(memory.allocation) && owner.borrow_backing(memory.allocation));
 assert(owner.free(&memory)==E_PENDING && events=="AMZI");owner.return_backing(memory.allocation);
 assert(owner.free(&memory)==E_PENDING && events=="AMZI");owner.return_backing(memory.allocation);
 owner.return_backing(memory.allocation);assert(owner.borrow_backing(memory.allocation));owner.return_backing(memory.allocation);
 assert(owner.free(&memory)==S_OK && events=="AMZIVEUD" && !owner.borrow_backing(memory.allocation));
 events.clear();assert(owner.close_after_engine_retirement()==S_OK && events=="P");
 // The linear primary: all three flags on a PRIMARY heap. The memory type is one of the image's, no
 // buffer is probed, the address needs the image's alignment only, and the kernel gets the LB7A
 // description with the image's pitch and the backing's size.
 {
  const auto old_probes=probes;const auto old_surfaces=surfaces;
  constexpr uint32_t primary=engine_ddi::kMemoryDedicated|engine_ddi::kMemoryPrimary|engine_ddi::kMemoryLinearSurface;
  D3D12DDIARG_CREATERESOURCE_0088 target{};target.ResourceType=D3D12DDI_RT_TEXTURE2D;target.Width=256;target.Height=64;
  target.DepthOrArraySize=1;target.MipLevels=1;target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;target.SampleDesc={1,0};
  heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY);
  heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;
  engine_ddi::MemoryRequest s=req;s.resource=&target;s.flags=primary;s.byte_size=65536;s.alignment=128;
  s.memory_type_bits=1;s.surface_row_pitch=1024;s.surface_layout_size=65536;expected_type=0;
  surface_format=D3DDDIFMT_A8R8G8B8;events.clear();
  // Created in one resource DDI, released in the one that destroys it: by the runtime resource.
  {OwnerScope create(&owner,0);
   assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI" && memory.memory_type_index==0 && memory.byte_size==65536);}
  {OwnerScope destroy(&owner,memory.allocation);
   assert(owner.free(&memory)==S_OK && events=="AMZIVEUR" && probes==old_probes && surfaces==old_surfaces+1);
   assert(!owner.last_free_report().owner_expired && owner.last_free_report().surface);}
  assert(!owner.owns_allocation(memory.allocation));
  // A page-aligned address is enough for it; raw memory above needed 64 KiB. Handed back inside
  // the DDI that created it, as after an import the engine refuses.
  gpu_address+=4096;target.Format=DXGI_FORMAT_R8G8B8A8_UNORM;surface_format=D3DDDIFMT_A8B8G8R8;events.clear();
  {OwnerScope create(&owner,0);
   assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI" && memory.gpu_va==gpu_address);
   gpu_address-=4096;memory_va_bias=4096;assert(owner.free(&memory)==S_OK && events=="AMZIVEUR");memory_va_bias=0;}
  // A 10-bit swap chain is four bytes a pixel as well and composed the same way, whatever the monitor's
  // depth: the kernel gets its D3DDDIFORMAT, A2B10G10R10 (the DXGI name counts from the low bits).
  target.Format=DXGI_FORMAT_R10G10B10A2_UNORM;surface_format=D3DDDIFMT_A2B10G10R10;events.clear();
  {OwnerScope create(&owner,0);
   assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI");
   assert(owner.free(&memory)==S_OK && events=="AMZIVEUR");}
  // Construction that fails after the allocation releases it in the same DDI, by the same form.
  {OwnerScope create(&owner,0);target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;surface_format=D3DDDIFMT_A8R8G8B8;
   events.clear();fail_import=true;
   assert(owner.allocate(&s,&memory)==E_OUTOFMEMORY && !memory.memory && events=="AMZIEUR");fail_import=false;}
  target.Format=DXGI_FORMAT_R8G8B8A8_UNORM;surface_format=D3DDDIFMT_A8B8G8R8;
  // The image's memory types are the only ones: type 0 is not among these.
  s.memory_type_bits=2;events.clear();assert(owner.allocate(&s,&memory)==E_INVALIDARG && events.empty());s.memory_type_bits=1;
  auto refused=[&](HRESULT expected){
   const auto before=next_allocation;events.clear();
   assert(owner.allocate(&s,&memory)==expected && !memory.memory && events.empty() && next_allocation==before);
  };
  // Refused before any callback: a description the surface does not exist for, and one that the
  // reader's rules do not admit.
  // An FP16 swap chain is eight bytes a pixel: 128 pixels fill the 1024-byte pitch that held 256
  // four-byte ones, so the kernel gets A16B16G16R16F with the same pitch and size. At 256 pixels the
  // pitch is short by half: refused before any callback, by the table's bytes, not by a width * 4.
  target.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;refused(E_INVALIDARG);
  target.Width=128;surface_width=128;surface_format=D3DDDIFMT_A16B16G16R16F;events.clear();
  {OwnerScope create(&owner,0);
   assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI" && memory.byte_size==65536);
   assert(owner.free(&memory)==S_OK && events=="AMZIVEUR");}
  target.Width=256;surface_width=256;surface_format=D3DDDIFMT_A8B8G8R8;
  target.Format=DXGI_FORMAT_R10G10B10A2_UINT;refused(E_NOTIMPL);target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  // A primary keeps the storage column: there is no D3DDDIFORMAT for an sRGB view, so a primary asked for in one
  // is refused here and not quietly given the UNORM row (BD-075 round 2, where the shared surface moved to
  // Bc250SharedSurfaceFormat and the primary did not).
  target.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;refused(E_NOTIMPL);
  target.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;refused(E_NOTIMPL);target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  target.MipLevels=2;refused(E_NOTIMPL);target.MipLevels=1;
  target.SampleDesc.Count=4;refused(E_NOTIMPL);target.SampleDesc.Count=1;
  target.DepthOrArraySize=2;refused(E_NOTIMPL);target.DepthOrArraySize=1;
  heap.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;refused(E_NOTIMPL);
  heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY);
  heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;refused(E_NOTIMPL);
  heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
  s.flags=engine_ddi::kMemoryDedicated|engine_ddi::kMemoryPrimary;refused(E_NOTIMPL);
  s.flags=engine_ddi::kMemoryPrimary|engine_ddi::kMemoryLinearSurface;refused(E_NOTIMPL);s.flags=primary;
  s.memory_type_bits=0;refused(E_INVALIDARG);s.memory_type_bits=1;
  s.surface_row_pitch=0;refused(E_INVALIDARG);s.surface_row_pitch=1000;refused(E_INVALIDARG);
  s.surface_row_pitch=1008;refused(E_INVALIDARG);s.surface_row_pitch=1024;
  s.byte_size=61440;s.surface_layout_size=61440;refused(E_INVALIDARG);
  s.byte_size=65537;refused(E_INVALIDARG);s.byte_size=65536;s.surface_layout_size=65536;
  s.surface_layout_size=65537;refused(E_INVALIDARG);s.surface_layout_size=65536;
  target.Width=8193;refused(E_INVALIDARG);target.Width=256;
  s.reserved2=1;refused(E_INVALIDARG);s.reserved2=0;
  // Raw memory carries no surface fields.
  heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;s=req;s.surface_row_pitch=1024;refused(E_INVALIDARG);
  heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 }
 // BD-075, the shell half of a shared resource. Three requests, in the order one create makes them:
 // the shareable envelope (an ordinary dedicated request that says the resource may have to be shared),
 // the linear surface the runtime's refusal of that request asks for, and - on the opening side - an
 // allocation the runtime made itself, adopted without any allocate callback.
 {
  constexpr uint32_t shareable_flags=engine_ddi::kMemoryDedicated|engine_ddi::kMemoryShareable;
  constexpr uint32_t shared_flags=shareable_flags|engine_ddi::kMemoryLinearSurface;
  D3D12DDIARG_CREATERESOURCE_0088 target{};target.ResourceType=D3D12DDI_RT_TEXTURE2D;target.Width=256;target.Height=64;
  target.DepthOrArraySize=1;target.MipLevels=1;target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;target.SampleDesc={1,0};
  target.Flags=static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(unsigned(D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)|
      unsigned(D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE));
  heap.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
  heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;expected_type=0;
  engine_ddi::MemoryRequest s=req;s.resource=&target;s.flags=shareable_flags;
  // 1. The envelope changes nothing about the allocation: a committed texture on a GPU-only heap is raw
  // memory, as it was before the flag existed, and the engine places its image in it.
  events.clear();assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI" && memory.byte_size==65536);
  assert(owner.free(&memory)==S_OK && events=="AMZIVEUD");
  // 2. The runtime refuses that allocation for a shared resource. The refusal is read as "this resource
  // needs a surface record" only here: a shareable request that has asked the engine for nothing yet.
  {
   const auto before=next_allocation;refuse_shareable_create=true;events.clear();
   assert(owner.allocate(&s,&memory)==engine_ddi::kShareRequired && !memory.memory && events=="A" &&
          next_allocation==before);
   const auto& report=owner.last_report();
   assert(report.stage==ImportStage::AllocateCallback && report.refusal &&
          !std::strcmp(report.refusal,"shared resource needs a surface record"));
   // The same answer to a request that never said the resource may be shared stays the runtime's status,
   // and so does any other failure of a shareable request.
   s.flags=engine_ddi::kMemoryDedicated;events.clear();
   assert(owner.allocate(&s,&memory)==E_INVALIDARG && !memory.memory && events=="A");
   s.flags=shareable_flags;refuse_shareable_create=false;
  }
  // 3. The surface itself: the pitch and the backing engine-ddi measured for the image, published as the
  // LB7A description with the E26R v3 texture beside it. No buffer is probed, the address needs the
  // image's alignment only, and the record is the one the opener decodes.
  s.flags=shared_flags;s.byte_size=65536;s.alignment=128;s.memory_type_bits=1;
  s.surface_row_pitch=1024;s.surface_layout_size=65536;
  surface_format=D3DDDIFMT_A8R8G8B8;shared_dxgi=DXGI_FORMAT_B8G8R8A8_UNORM;shared_bind=0x28;
  {
   const auto old_probes=probes,old_surfaces=surfaces,old_shared=shared_created;
   events.clear();
   {OwnerScope create(&owner,0);
    assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI" && memory.byte_size==65536 &&
           memory.memory_type_index==0);}
   assert(shared_created==old_shared+1 && probes==old_probes && surfaces==old_surfaces);
   // One image owns this memory, so nothing is placed beside it; the allocation is this driver's.
   assert(owner.owns_allocation(memory.allocation) && !owner.borrow_backing(memory.allocation));
   {OwnerScope destroy(&owner,memory.allocation);
    assert(owner.free(&memory)==S_OK && events=="AMZIVEUR");
    assert(owner.last_free_report().surface && !owner.last_free_report().adopted &&
           !owner.last_free_report().owner_expired);}
  }
  // The three view flags the DDI states positively are the three the record carries, and no others.
  for(const auto pair:{std::pair<unsigned,uint32_t>{unsigned(D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET),0x20u},
                       {unsigned(D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE),0x8u},
                       {unsigned(D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS),0x80u},
                       {unsigned(D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS),0u},
                       {0u,0u}}){
   target.Flags=static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(pair.first);shared_bind=pair.second;
   events.clear();{OwnerScope create(&owner,0);assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI");}
   {OwnerScope destroy(&owner,memory.allocation);assert(owner.free(&memory)==S_OK && events=="AMZIVEUR");}
  }
  target.Flags=static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(unsigned(D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)|
      unsigned(D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE));shared_bind=0x28;
  // Every refusal of the shared surface names its own check, before any callback, and allocates nothing.
  auto refused=[&](HRESULT expected,const char* why){
   const auto before=next_allocation;events.clear();
   assert(owner.allocate(&s,&memory)==expected && !memory.memory && events.empty() && next_allocation==before);
   const auto& report=owner.last_report();
   assert(report.refusal && !std::strcmp(report.refusal,why));
  };
  // A shared surface is nobody's primary, and it lives in the one video memory pool this part has. The
  // PRIMARY heap flag is admitted for a primary request alone, so a shared request on such a heap is
  // declined one check earlier, by the heap flags, with the bit in the report.
  heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY);
  refused(E_NOTIMPL,"heap flags");
  assert(owner.last_report().unimplemented_heap_flags==unsigned(D3D12DDI_HEAP_FLAG_PRIMARY));
  heap.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
  heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;refused(E_NOTIMPL,"shared surface shape");
  heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;
  // The two flag combinations that are two contracts at once, and the one that is a heap to place shared
  // resources in later: each declined by name, not reduced to something this shell knows.
  s.flags=shared_flags|engine_ddi::kMemoryPrimary;refused(E_NOTIMPL,"request flags");
  s.flags=engine_ddi::kMemoryShareable;refused(E_NOTIMPL,"shareable heap without a resource");
  s.flags=shared_flags;
  // The geometry and the format are the surface rules, under the shared names.
  target.Format=DXGI_FORMAT_R10G10B10A2_UINT;refused(E_NOTIMPL,"shared surface format");
  target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  // BD-075 round 2: a shared surface's format is Bc250SharedSurfaceFormat's, so the sRGB view of either 8-bit row
  // is admitted and publishes its storage row - the LB7A format and the record's own DXGI number, which is the
  // view the creator asked for. This gate was the storage column alone, and s12to11-srgb died here with
  // "shared surface format" after engine-ddi had already admitted the description and retried (lab 03:06Z).
  // The other COMPOSED rows have no sRGB sibling, so there is nothing more to admit.
  for(const auto row:{std::tuple<DXGI_FORMAT,DXGI_FORMAT,unsigned long>
                      {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,D3DDDIFMT_A8R8G8B8},
                      {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,D3DDDIFMT_A8B8G8R8}}){
   target.Format=std::get<0>(row);shared_dxgi=std::get<1>(row);surface_format=std::get<2>(row);
   const auto before=shared_created;events.clear();
   {OwnerScope create(&owner,0);assert(owner.allocate(&s,&memory)==S_OK && events=="AMZI");}
   assert(shared_created==before+1);
   {OwnerScope destroy(&owner,memory.allocation);assert(owner.free(&memory)==S_OK && events=="AMZIVEUR");}
  }
  target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;shared_dxgi=DXGI_FORMAT_B8G8R8A8_UNORM;
  surface_format=D3DDDIFMT_A8R8G8B8;
  s.surface_layout_size=65537;refused(E_INVALIDARG,"shared surface layout");s.surface_layout_size=65536;
  s.surface_row_pitch=0;refused(E_INVALIDARG,"shared surface layout");s.surface_row_pitch=1024;
  target.MipLevels=2;refused(E_NOTIMPL,"shared surface shape");target.MipLevels=1;
  target.SampleDesc.Count=4;refused(E_NOTIMPL,"shared surface shape");target.SampleDesc.Count=1;
  heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;refused(E_NOTIMPL,"shared surface shape");
  heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
  // A pitch the record's own rules refuse never reaches a callback either: prepare_shared_surface takes it.
  {
   const auto before=next_allocation;s.surface_row_pitch=1020;events.clear();
   assert(owner.allocate(&s,&memory)==E_INVALIDARG && events.empty() && next_allocation==before);
   s.surface_row_pitch=1024;
  }
  // 4. The open path. The allocation exists, the runtime opened it for this device and destroys it itself:
  // no allocate callback runs, and the record that holds the mapping and the import is borrowed.
  engine_ddi::AdoptRequest adopt{};adopt.size=sizeof(adopt);adopt.flags=shared_flags;
  adopt.rt_owner={handle<void*>(2)};adopt.byte_size=65536;adopt.alignment=128;adopt.memory_type_bits=1;
  {
   engine_ddi::ImportedMemory borrowed{};
   adopt.allocation=++next_allocation;                  // the handle pOpenAllocationInfo[0] carried
   events.clear();
   assert(owner.adopt(&adopt,&borrowed)==S_OK && events=="MZI" && borrowed.allocation==adopt.allocation &&
          borrowed.byte_size==65536 && borrowed.memory_type_index==0 && borrowed.gpu_va==gpu_address);
   // In the store like any other allocation, so the ICD's Lock2/Unlock2 reach this record and its own state
   // checks (owns_allocation). No view is ever placed beside the opened image.
   assert(owner.owns_allocation(borrowed.allocation) && !owner.borrow_backing(borrowed.allocation));
   // The same handle adopted twice: two records, two imports, two independent releases. An application may call
   // OpenSharedHandle twice on one device, and nothing promises that dxgkrnl answers with two handles. The store
   // tells the two imports apart by the cookie each one carries, not by the handle.
   {
    engine_ddi::ImportedMemory again{};events.clear();
    // The stub's vkAllocateMemory answers with a handle derived from the allocation, so the two imports share
    // their VkDeviceMemory value here; the cookie is the field that tells the two records apart, and it is the
    // field free() now looks at.
    assert(owner.adopt(&adopt,&again)==S_OK && events=="MZI" && again.allocation==adopt.allocation &&
           again.cookie && again.cookie!=borrowed.cookie);
    assert(owner.owns_allocation(again.allocation));
    // Each import is released on its own, and neither release touches the other's record.
    events.clear();assert(owner.free(&again)==S_OK && events=="VU");
    assert(owner.last_free_report().adopted && owner.last_free_report().stage==FreeStage::Done);
    assert(owner.free(&again)==E_INVALIDARG && owner.owns_allocation(borrowed.allocation));
   }
   // The release returns the mapping and the import and makes no deallocate callback in either form. No Evict
   // either ('E' is absent): the runtime destroys a borrowed allocation as soon as its destroy returns, so a
   // callback naming that handle could evict an allocation dxgkrnl has since recycled.
   const unsigned evicted_before=evictions;
   events.clear();assert(owner.free(&borrowed)==S_OK && events=="VU" && evictions==evicted_before);
   assert(owner.last_free_report().adopted && owner.last_free_report().surface &&
          owner.last_free_report().stage==FreeStage::Done && !owner.last_free_report().owner_expired);
   assert(!owner.owns_allocation(borrowed.allocation) && owner.free(&borrowed)==E_INVALIDARG);
  }
  // A handle this driver created itself is never adopted over: that is the store corruption the refusal exists
  // for, and the only shape adopt still declines on a known handle.
  {
   engine_ddi::MemoryRequest plain=req;plain.resource=&target;plain.flags=shareable_flags;
   engine_ddi::ImportedMemory created{},stolen{};
   events.clear();assert(owner.allocate(&plain,&created)==S_OK && events=="AMZI");
   engine_ddi::AdoptRequest over=adopt;over.allocation=created.allocation;events.clear();
   assert(owner.adopt(&over,&stolen)==E_INVALIDARG && !stolen.memory && events.empty());
   assert(owner.last_report().refusal &&
          !std::strcmp(owner.last_report().refusal,"the allocation is one this driver created"));
   events.clear();assert(owner.free(&created)==S_OK && events=="VEUD");
  }
  // Every malformed adopt request is refused before the paging queue is touched, by one name.
  {
   auto bad=[&](const engine_ddi::AdoptRequest& request){
    engine_ddi::ImportedMemory out{};events.clear();
    assert(owner.adopt(&request,&out)==E_INVALIDARG && !out.memory && events.empty());
    const auto& report=owner.last_report();
    assert(report.stage==ImportStage::Request && report.refusal &&
           !std::strcmp(report.refusal,"malformed adopt request"));
   };
   engine_ddi::ImportedMemory out{};
   assert(owner.adopt(nullptr,&out)==E_INVALIDARG && owner.adopt(&adopt,nullptr)==E_INVALIDARG);
   auto bad_request=[&](auto change){engine_ddi::AdoptRequest r=adopt;r.allocation=next_allocation+1;change(r);bad(r);};
   bad_request([](engine_ddi::AdoptRequest& r){r.size=sizeof(r)-4;});
   bad_request([](engine_ddi::AdoptRequest& r){r.flags=shareable_flags;});
   bad_request([](engine_ddi::AdoptRequest& r){r.flags=shared_flags|engine_ddi::kMemoryPrimary;});
   bad_request([](engine_ddi::AdoptRequest& r){r.allocation=0;});
   bad_request([](engine_ddi::AdoptRequest& r){r.memory_type_bits=0;});
   bad_request([](engine_ddi::AdoptRequest& r){r.byte_size=0;});
   bad_request([](engine_ddi::AdoptRequest& r){r.byte_size=65535;});     // not page rounded
   bad_request([](engine_ddi::AdoptRequest& r){r.alignment=0;});
   bad_request([](engine_ddi::AdoptRequest& r){r.alignment=96;});        // not a power of two
   // A type mask without video memory in it: the opened surface is GPU-only, as the created one is.
   engine_ddi::AdoptRequest host=adopt;host.allocation=next_allocation+1;host.memory_type_bits=1u<<1;
   events.clear();assert(owner.adopt(&host,&out)==E_INVALIDARG && events.empty());
   assert(owner.last_report().refusal && !std::strcmp(owner.last_report().refusal,"no device-local memory type"));
  }
  // A borrowed allocation is never quarantined, whatever the release policy holds: the runtime destroys it
  // with the opened resource, so holding its address back would hold an address that is no longer ours.
  {
   ImportReleasePolicy policy{};policy.quarantine_depth=3;policy.quarantine_count_cap=64;
   RuntimeHeapImports held(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,
                           policy);
   assert(held.initialize()==S_OK && policy.holds());
   engine_ddi::AdoptRequest open=adopt;open.allocation=++next_allocation;
   engine_ddi::ImportedMemory borrowed{};
   events.clear();assert(held.adopt(&open,&borrowed)==S_OK && events=="MZI");
   events.clear();assert(held.free(&borrowed)==S_OK && events=="VU" && !held.held_count() && !held.held_bytes());
   assert(held.last_free_report().adopted && held.last_free_report().stage==FreeStage::Done);
   assert(held.close_after_engine_retirement()==S_OK && held.discard_metadata()==0);
  }
  heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 }
 // Pending paging: the VA reaches the engine only after one CPU wait for the largest value, the
 // residency's (30) when it is pending, else the mapping's (20); never before (trial 153).
 pending=true;completed=10;events.clear();auto before_waits=waits;
 assert(owner.allocate(&req,&memory)==S_OK && memory.memory && events=="AMZWI" && waits==before_waits+1 && completed==20);
 assert(owner.free(&memory)==S_OK && events=="AMZWIVEUD");
 resident_pending=true;completed=10;events.clear();
 assert(owner.allocate(&req,&memory)==S_OK && memory.memory && events=="AMZWI" && completed==30);
 assert(owner.free(&memory)==S_OK && events=="AMZWIVEUD");
 // A wait that fails hands nothing to the engine. The record keeps its reference and mapping until
 // both have completed; the later cleanup releases them in order.
 completed=10;fail_wait=true;events.clear();
 assert(owner.allocate(&req,&memory)==E_FAIL && !memory.memory && events=="AMZW");fail_wait=false;
 completed=30;events.clear();assert(owner.close_after_engine_retirement()==S_OK && events=="EUDP");
 resident_pending=false;
 // A refused reference (over budget) takes nothing: the mapping completes and is freed, no eviction.
 completed=10;resident_result=E_OUTOFMEMORY;events.clear();
 assert(owner.allocate(&req,&memory)==E_OUTOFMEMORY && !memory.memory && events=="AMZWUD");
 resident_result=S_OK;pending=false;completed=20;
 // An eviction that fails does not hold the VA: the mapping and the allocation are released anyway
 // (deallocation drops residency), so an error that never clears cannot leak the record.
 events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMZI");fail_evict=true;
 assert(owner.free(&memory)==S_OK && events=="AMZIVEUD");fail_evict=false;
 events.clear();assert(owner.close_after_engine_retirement()==S_OK && events=="P");
 assert(owner.discard_metadata()==0 && owner.allocate(&req,&memory)==E_UNEXPECTED && !owner.owns_allocation(next_allocation));
 // Every reference taken was released by an Evict callback except five: the refused reference, the failed
 // eviction, and the three borrowed allocations of the BD-075 section (two opens of one handle and the
 // quarantine instance's one), whose residency is dropped without a callback because the runtime destroys the
 // allocation itself and the handle may already be gone.
 assert(makes_resident==evictions+5);assert(surfaces==5);
 // The owner's authority ends with its DDI. Records that outlive it keep the allocation and never
 // reach the runtime again, in either form. A second owner, so that the first one's closure above
 // stays what it was.
 {
  RuntimeHeapImports late(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,
                          ImportReleasePolicy::off());
  assert(late.initialize()==S_OK);
  constexpr uint32_t primary=engine_ddi::kMemoryDedicated|engine_ddi::kMemoryPrimary|engine_ddi::kMemoryLinearSurface;
  D3D12DDIARG_CREATERESOURCE_0088 target{};target.ResourceType=D3D12DDI_RT_TEXTURE2D;target.Width=256;target.Height=64;
  target.DepthOrArraySize=1;target.MipLevels=1;target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;target.SampleDesc={1,0};
  heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY);
  heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;
  engine_ddi::MemoryRequest s=req;s.resource=&target;s.flags=primary;s.byte_size=65536;s.alignment=128;
  s.memory_type_bits=1;s.surface_row_pitch=1024;s.surface_layout_size=65536;expected_type=0;
  surface_format=D3DDDIFMT_A8R8G8B8;
  engine_ddi::ImportedMemory first{},second{},third{};
  // 1. The release callback fails inside the resource's DDI; the later cleanup makes no callback.
  events.clear();{OwnerScope create(&late,0);assert(late.allocate(&s,&first)==S_OK);}
  {OwnerScope destroy(&late,first.allocation);fail_deallocate=true;
   assert(late.free(&first)==E_FAIL && events=="AMZIVEUR");fail_deallocate=false;}
  events.clear();assert(late.close_after_engine_retirement()==kOwnerExpired && events.empty());
  assert(late.last_free_report().owner_expired && late.owns_allocation(first.allocation));
  // 2. The work retires only after the resource's DDI has returned: the release comes from a later
  // DDI, here the destruction of another primary, and names neither owner.
  events.clear();{OwnerScope create(&late,0);assert(late.allocate(&s,&second)==S_OK);}
  {OwnerScope create(&late,0);assert(late.allocate(&s,&third)==S_OK);}
  {OwnerScope destroy(&late,second.allocation);}
  events.clear();
  {OwnerScope destroy(&late,third.allocation);
   assert(late.free(&second)==kOwnerExpired && events=="VEU" && late.last_free_report().owner_expired);
   assert(late.free(&third)==S_OK && events=="VEUVEUR");}
  assert(late.owns_allocation(second.allocation) && !late.owns_allocation(third.allocation));
  // Outside any resource DDI the same holds.
  events.clear();{OwnerScope create(&late,0);assert(late.allocate(&s,&third)==S_OK);}
  events.clear();assert(late.free(&third)==kOwnerExpired && events=="VEU");
  // 3. Construction fails and its release fails: the record stays, without an owner.
  events.clear();{OwnerScope create(&late,0);fail_import=true;fail_deallocate=true;
   assert(late.allocate(&s,&memory)==E_OUTOFMEMORY && events=="AMZIEUR");fail_import=false;fail_deallocate=false;}
  events.clear();assert(late.close_after_engine_retirement()==kOwnerExpired && events.empty());
  // 4. A resource DDI entered inside another one is refused before its engine call and leaves the
  // outer scope as it was: the outer destroy still releases its primary, the outer create still
  // gives authority to what it allocates.
  {
   engine_ddi::ImportedMemory outer{};unsigned inner_calls=0,outer_calls=0;
   events.clear();{OwnerScope create(&late,0);assert(!create.refused() && late.allocate(&s,&outer)==S_OK);}
   events.clear();
   assert(destroy_in_owner_scope(&late,outer.allocation,[&]() noexcept {
    ++outer_calls;
    assert(create_in_owner_scope(&late,[&]() noexcept {++inner_calls;return S_OK;})==E_UNEXPECTED);
    assert(destroy_in_owner_scope(&late,outer.allocation,[&]() noexcept {++inner_calls;})==E_UNEXPECTED);
    assert(late.free(&outer)==S_OK && events=="VEUR");
   })==S_OK && outer_calls==1 && !inner_calls && !late.owns_allocation(outer.allocation));
   events.clear();
   assert(create_in_owner_scope(&late,[&]() noexcept {
    ++outer_calls;
    assert(destroy_in_owner_scope(&late,0,[&]() noexcept {++inner_calls;})==E_UNEXPECTED);
    assert(late.allocate(&s,&outer)==S_OK && late.free(&outer)==S_OK && events=="AMZIVEUR");
    return S_OK;
   })==S_OK && outer_calls==2 && !inner_calls);
   // Without imports there is no scope to refuse: the engine call runs.
   assert(create_in_owner_scope(nullptr,[&]() noexcept {++inner_calls;return S_FALSE;})==S_FALSE && inner_calls==1);events.clear();
  }
  // Four allocations stay owned, and the paging queue with them.
  assert(late.discard_metadata()==5 && events.empty());
  heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 }
 assert(surfaces==12);
 {
  // present-cached: the 16-byte E26R v2 record, shared 1, CPU_READ without the PRIMARY intent bit; the
  // allocation itself is unchanged (LB7A v1, PRIMARY, no video present source). Default stays v1.
  AllocationRequest cached;
  assert(cached.prepare_surface(256,64,1024,D3DDDIFMT_A8R8G8B8,65536,handle<void*>(2),true)==S_OK);
  assert(cached.args.PrivateDriverDataSize==16);
  uint32_t e[4];std::memcpy(e,cached.args.pPrivateDriverData,sizeof(e));
  assert(e[0]==0x52363245u && e[1]==2 && e[2]==1 && e[3]==2);
  assert(cached.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY && cached.info.VidPnSourceId==D3DDDI_ID_UNINITIALIZED);
  assert(cached.info.PrivateDriverDataSize==32);
  AllocationRequest plain;
  assert(plain.prepare_surface(256,64,1024,D3DDDIFMT_A8R8G8B8,65536,handle<void*>(2))==S_OK);
  assert(plain.args.PrivateDriverDataSize==12);
  std::memcpy(e,plain.args.pPrivateDriverData,12);
  assert(e[0]==0x52363245u && e[1]==1 && e[2]==1);
  assert(plain.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY && plain.info.VidPnSourceId==D3DDDI_ID_UNINITIALIZED);
  // present-noprimary: the same LB7A v1 surface under the same record, as an ordinary allocation.
  AllocationRequest ordinary;
  assert(ordinary.prepare_surface(256,64,1024,D3DDDIFMT_A8R8G8B8,65536,handle<void*>(2),false,false)==S_OK);
  assert(ordinary.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE && ordinary.info.VidPnSourceId==0);
  assert(ordinary.info.PrivateDriverDataSize==32 && ordinary.args.PrivateDriverDataSize==12);
  std::memcpy(e,ordinary.args.pPrivateDriverData,12);
  assert(e[0]==0x52363245u && e[1]==1 && e[2]==1);
  // Both experiments: the v2 CPU_READ record on an ordinary allocation.
  AllocationRequest both;
  assert(both.prepare_surface(256,64,1024,D3DDDIFMT_A8R8G8B8,65536,handle<void*>(2),true,false)==S_OK);
  assert(both.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE && both.args.PrivateDriverDataSize==16);
  std::memcpy(e,both.args.pPrivateDriverData,sizeof(e));
  assert(e[0]==0x52363245u && e[1]==2 && e[2]==1 && e[3]==2);
  // The pitch rule takes the bytes a pixel from the format's row: 4 for the 8- and 10-bit rows, 8 for FP16.
  // The LB7A words carry the format as given; the reader opens it by the same table.
  AllocationRequest fp16;
  assert(fp16.prepare_surface(256,64,2048,D3DDDIFMT_A16B16G16R16F,131072,handle<void*>(2))==S_OK);
  uint32_t w[8];std::memcpy(w,fp16.info.pPrivateDriverData,sizeof(w));
  assert(w[0]==0x4137424Cu && w[1]==1 && w[2]==256 && w[3]==64 && w[4]==2048 && w[5]==113 && w[6]==131072 && !w[7]);
  assert(fp16.prepare_surface(256,64,1024,D3DDDIFMT_A16B16G16R16F,131072,handle<void*>(2))==E_INVALIDARG);
  assert(fp16.prepare_surface(256,64,2048,D3DDDIFMT_A16B16G16R16F,65536,handle<void*>(2))==E_INVALIDARG);
  assert(fp16.prepare_surface(256,64,1024,D3DDDIFMT_A2B10G10R10,65536,handle<void*>(2))==S_OK);
  // Rows the table does not have, or has without COMPOSED: A2R10G10B10 (35), X8R8G8B8 (scan-out only).
  assert(fp16.prepare_surface(256,64,1024,D3DDDIFMT_A2R10G10B10,65536,handle<void*>(2))==E_NOTIMPL);
  assert(fp16.prepare_surface(256,64,1024,D3DDDIFMT_X8R8G8B8,65536,handle<void*>(2))==E_NOTIMPL);
  // M15.14 scan-out: the v2 record with PRIMARY and SCANOUT, shared still 1, and video present source 0
  // instead of D3DDDI_ID_UNINITIALIZED. The LB7A description is the composed one, unchanged.
  AllocationRequest direct;
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_A8R8G8B8,7680*1200,handle<void*>(2),false,true,true)==S_OK);
  assert(direct.args.PrivateDriverDataSize==16);
  std::memcpy(e,direct.args.pPrivateDriverData,sizeof(e));
  assert(e[0]==0x52363245u && e[1]==2 && e[2]==1 && e[3]==5);
  assert(direct.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY && direct.info.VidPnSourceId==0);
  assert(direct.info.PrivateDriverDataSize==32);
  {uint32_t d[8];std::memcpy(d,direct.info.pPrivateDriverData,sizeof(d));
   assert(d[0]==0x4137424Cu && d[1]==1 && d[2]==1920 && d[3]==1200 && d[4]==7680 && d[5]==21 && d[6]==7680u*1200u && !d[7]);}
  // X8 is the other scan-out row and is admitted here although composition refuses it above.
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_X8R8G8B8,7680*1200,handle<void*>(2),false,true,true)==S_OK);
  // The composed-only rows cannot be scanned out, which is what keeps the 10-bit and FP16 primaries on
  // the composition path whatever the instance selects.
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_A2B10G10R10,7680*1200,handle<void*>(2),false,true,true)==E_NOTIMPL);
  assert(direct.prepare_surface(1920,1200,15360,D3DDDIFMT_A16B16G16R16F,15360*1200,handle<void*>(2),false,true,true)==E_NOTIMPL);
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_A8B8G8R8,7680*1200,handle<void*>(2),false,true,true)==E_NOTIMPL);
  // Scan-out contradicts both of the other two intents and is refused rather than silently reduced.
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_A8R8G8B8,7680*1200,handle<void*>(2),true,true,true)==E_INVALIDARG);
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_A8R8G8B8,7680*1200,handle<void*>(2),false,false,true)==E_INVALIDARG);
  // The geometry rules are the same ones: a pitch that does not hold the row, and a size that does not
  // hold the rows, are refused before any record is built.
  assert(direct.prepare_surface(1920,1200,7676,D3DDDIFMT_A8R8G8B8,7680*1200,handle<void*>(2),false,true,true)==E_INVALIDARG);
  assert(direct.prepare_surface(1920,1200,7680,D3DDDIFMT_A8R8G8B8,4096,handle<void*>(2),false,true,true)==E_INVALIDARG);
 }
 // The release gate (M15.8, fixes F2 and F3 of the trial 245 report). One owner per policy, because a
 // policy is fixed for the owner's life. The event letters are the callbacks: A allocate, M map, Z the
 // residency reference, I the Vulkan import, V the Vulkan free, E evict, U free the VA, D deallocate.
 // A quarantined release stops after V and the rest follows when the policy admits it.
 {
  heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
  heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;expected_type=0;
  engine_ddi::ImportedMemory a{},b{},c{};
  // 1. The depth alone: the release of an import leaves after two further releases have reached the shell.
  {
   ImportReleasePolicy policy{};policy.quarantine_depth=2;policy.quarantine_count_cap=4;
   RuntimeHeapImports gate(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,policy);
   assert(gate.initialize()==S_OK);
   events.clear();
   assert(gate.allocate(&req,&a)==S_OK && gate.allocate(&req,&b)==S_OK && gate.allocate(&req,&c)==S_OK &&
          events=="AMZIAMZIAMZI");
   events.clear();assert(gate.free(&a)==S_OK && events=="V" && gate.held_count()==1 && gate.held_bytes()==65536);
   assert(gate.last_free_report().stage==FreeStage::Quarantined && gate.last_free_report().gpu_va==a.gpu_va &&
          gate.last_free_report().byte_size==65536 && !gate.last_free_report().released);
   events.clear();assert(gate.free(&b)==S_OK && events=="V" && gate.held_count()==2);
   events.clear();assert(gate.free(&c)==S_OK && events=="VEUD" && gate.held_count()==2 &&
                         gate.last_free_report().released==1);
   // The device's teardown drains the rest, and the paging queue closes after it ('P').
   events.clear();assert(gate.close_after_engine_retirement()==S_OK && events=="EUDEUDP" && !gate.held_count() &&
                         !gate.forced_releases());
   assert(gate.discard_metadata()==0);
  }
  // 2. The caps bound what the delay holds: over the count cap the oldest leaves whatever the depth says.
  {
   ImportReleasePolicy policy{};policy.quarantine_depth=1000;policy.quarantine_count_cap=1;
   RuntimeHeapImports caps(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,policy);
   assert(caps.initialize()==S_OK);
   assert(caps.allocate(&req,&a)==S_OK && caps.allocate(&req,&b)==S_OK);
   events.clear();assert(caps.free(&a)==S_OK && events=="V" && caps.held_count()==1);
   events.clear();assert(caps.free(&b)==S_OK && events=="VEUD" && caps.held_count()==1);
   events.clear();assert(caps.close_after_engine_retirement()==S_OK && events=="EUDP");
   assert(caps.discard_metadata()==0);
  }
  // 3. The byte cap, smaller than one import: every release is over it and leaves at once.
  {
   ImportReleasePolicy policy{};policy.quarantine_depth=1000;policy.quarantine_byte_cap=65535;
   RuntimeHeapImports bytes(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,policy);
   assert(bytes.initialize()==S_OK);
   assert(bytes.allocate(&req,&a)==S_OK);
   events.clear();assert(bytes.free(&a)==S_OK && events=="VEUD" && !bytes.held_count());
   events.clear();assert(bytes.close_after_engine_retirement()==S_OK && events=="P");
   assert(bytes.discard_metadata()==0);
  }
  // 4. The age bound: an import the application never follows with another release still leaves.
  {
   ImportReleasePolicy policy{};policy.quarantine_depth=1000;policy.quarantine_age_ms=1;
   RuntimeHeapImports aged(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,policy);
   assert(aged.initialize()==S_OK);
   assert(aged.allocate(&req,&a)==S_OK && aged.allocate(&req,&b)==S_OK);
   events.clear();assert(aged.free(&a)==S_OK && events=="V" && aged.held_count()==1);
   Sleep(32);                                   // GetTickCount64's step is about 16 ms
   events.clear();assert(aged.free(&b)==S_OK && events=="VEUD" && aged.held_count()==1);
   assert(aged.close_after_engine_retirement()==S_OK && aged.discard_metadata()==0);
  }
  // 5. The progress gate: the device-wide progress of the release decides, and a linear primary is never
  // quarantined (only its own runtime resource may release it, inside the destroy that ends it).
  {
   ImportReleasePolicy policy{};policy.progress_gate=true;
   RuntimeHeapImports gated(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity,policy);
   gated.bind_progress({&gated,progress_snapshot_cb,progress_retired_cb});
   assert(gated.initialize()==S_OK);
   assert(gated.allocate(&req,&a)==S_OK);
   progress_retired_flag=false;snapshots=0;
   events.clear();assert(gated.free(&a)==S_OK && events=="V" && gated.held_count()==1 && snapshots==1);
   events.clear();gated.retire_held();assert(events.empty() && gated.held_count()==1);
   progress_retired_flag=true;
   gated.retire_held();assert(events=="EUD" && !gated.held_count() && !gated.forced_releases());
   // The primary: allocated and released inside its resource's DDI, with the progress unretired.
   progress_retired_flag=false;
   constexpr uint32_t primary=engine_ddi::kMemoryDedicated|engine_ddi::kMemoryPrimary|engine_ddi::kMemoryLinearSurface;
   D3D12DDIARG_CREATERESOURCE_0088 target{};target.ResourceType=D3D12DDI_RT_TEXTURE2D;target.Width=256;target.Height=64;
   target.DepthOrArraySize=1;target.MipLevels=1;target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;target.SampleDesc={1,0};
   heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY);
   engine_ddi::MemoryRequest s=req;s.resource=&target;s.flags=primary;s.byte_size=65536;s.alignment=128;
   s.memory_type_bits=1;s.surface_row_pitch=1024;s.surface_layout_size=65536;surface_format=D3DDDIFMT_A8R8G8B8;
   engine_ddi::ImportedMemory surface{};
   events.clear();{OwnerScope create(&gated,0);assert(gated.allocate(&s,&surface)==S_OK);}
   {OwnerScope destroy(&gated,surface.allocation);
    assert(gated.free(&surface)==S_OK && events=="AMZIVEUR" && !gated.held_count());}
   heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
   // An import whose progress never retires is still released at the device's teardown, and counted.
   assert(gated.allocate(&req,&b)==S_OK);
   events.clear();assert(gated.free(&b)==S_OK && events=="V" && gated.held_count()==1);
   events.clear();assert(gated.close_after_engine_retirement()==S_OK && events=="EUDP" &&
                         gated.forced_releases()==1 && !gated.held_count());
   assert(gated.discard_metadata()==0);
  }
  // 6. The driver's defaults, as the switches leave them: the gate is on and holds three releases.
  {
   const auto defaults=ImportReleasePolicy::from_switches();
   assert(defaults.progress_gate && defaults.quarantine_depth==3 && defaults.quarantine_count_cap==64 &&
          defaults.quarantine_age_ms==250 && defaults.quarantine_byte_cap==(64ull<<20) && defaults.holds());
   assert(!ImportReleasePolicy::off().holds());
  }
  assert(surfaces==13);
 }
 std::puts("PASS heap import: DEFAULT/UPLOAD/READBACK, coherent L0 policy and rejection, exact private import, borrowed map, ordered cleanup, own residency reference and one CPU wait before the VA leaves, evicted before unmap, linear primary as an LB7A surface under E26R (8-, 10-bit and FP16 storage, pitch by the table's bytes), "
  "released by its runtime resource inside that resource's DDI only, present-cached v2 CPU_READ record, present-noprimary ordinary allocation, "
  "release gate off reproducing adapter106 and on in all five shapes (depth, count cap, byte cap, age bound, device progress with a forced teardown release), "
  "BD-075 shared resources (the shareable envelope unchanged, kShareRequired only from the runtime's refusal of a shareable create, the shared surface's "
  "LB7A and E26R v3 records with the three view flags and every refusal by name, and an adopted allocation that is borrowed: no allocate or deallocate "
  "callback, no quarantine, no backing to lend, one record per allocation handle)");
}
