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
using namespace native12;
template<class T> T handle(uintptr_t n){return reinterpret_cast<T>(n);}
static std::string events;
static UINT64 completed=10;
static UINT64 gpu_address=UINT64_C(0x100000000);
static D3DKMT_HANDLE next_allocation=50;
static bool pending=false,fail_free=false,fail_import=false,unlock_on_free=false,fail_deallocate=false;
static unsigned surfaces=0;
static unsigned long surface_format=0;
static unsigned creates=0,makes_resident=0,probes=0;
static uint32_t expected_type=0;
static void* identity=handle<void*>(0x5432);
static RuntimeHeapImports* imports;
static char mapped[65536];
static HRESULT APIENTRY allocate_cb(D3D12DDI_HRTDEVICE d,D3D12DDICB_ALLOCATE_0022* a){
 assert(d.handle==handle<void*>(1) && a->hResource==handle<void*>(2) && !a->hKMResource);
 assert(a->NumAllocations==1);
 if(a->pAllocationInfo->PrivateDriverDataSize==32){
  // The primary: the 32-byte LB7A v1 description, PRIMARY, no video present source, under the
  // 12-byte E26R v1 record with shared 1. Read as the words on the wire, not through the
  // producer's own structures.
  assert(a->pPrivateDriverData && a->PrivateDriverDataSize==12);
  uint32_t e[3];std::memcpy(e,a->pPrivateDriverData,sizeof(e));
  assert(e[0]==0x52363245u && e[1]==1 && e[2]==1);
  uint32_t w[8];std::memcpy(w,a->pAllocationInfo->pPrivateDriverData,sizeof(w));
  assert(w[0]==0x4137424Cu && w[1]==1 && w[2]==256 && w[3]==64 && w[4]==1024);
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
 events+='A';a->pAllocationInfo->hAllocation=++next_allocation;return S_OK;
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
static HRESULT APIENTRY make_cb(HANDLE,D3DDDI_MAKERESIDENT*){++makes_resident;return E_FAIL;}
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
 bc250::umd::RuntimeDomain domain;RuntimeHeapImports owner(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity);imports=&owner;
 assert(owner.initialize()==E_UNEXPECTED);bc250::umd::RuntimeDomain::Scope scope(domain);assert(owner.initialize()==S_OK);
 D3D12DDIARG_CREATEHEAP_0001 heap{};heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.CreationNodeMask=heap.VisibleNodeMask=1;
 D3D12DDIARG_CREATERESOURCE_0088 resource{};resource.ResourceType=D3D12DDI_RT_BUFFER;
 engine_ddi::MemoryRequest req{};req.size=sizeof(req);req.rt_owner={handle<void*>(2)};req.heap=&heap;req.resource=&resource;req.byte_size=4096;req.alignment=65536;req.flags=engine_ddi::kMemoryDedicated;
 engine_ddi::ImportedMemory memory{};
 for(expected_type=0;expected_type<3;++expected_type){
 heap.CPUPageProperty=expected_type==0?D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE:expected_type==1?D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE:D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
 events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMI" && memory.allocation==next_allocation && memory.memory_type_index==expected_type);
 assert(owner.owns_allocation(memory.allocation) && owner.close_after_engine_retirement()==E_PENDING);
 if(expected_type){D3DKMT_LOCK2 lock{};lock.hAllocation=memory.allocation;assert(owner.dispatch(BC250_HOST_Lock2,&lock)==S_OK && lock.pData==mapped);unlock_on_free=true;}
 assert(owner.free(&memory)==S_OK);assert(events==(expected_type?"AMILVNUD":"AMIVUD"));unlock_on_free=false;
 assert(!owner.owns_allocation(memory.allocation) && owner.free(&memory)==E_INVALIDARG);
 }
 // Both CPU-visible L0 policies accept the explicit coherent flag, including
 // heaps allowing every resource category. Actual non-buffer resources still fail.
 heap.Flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_BUFFERS|D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES|
     D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE);
 heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 for(expected_type=1;expected_type<=2;++expected_type){
  heap.CPUPageProperty=expected_type==1?D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE:D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
  events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMI" && memory.memory_type_index==expected_type);
  assert(owner.free(&memory)==S_OK && events=="AMIVUD");
 }
 // A cached, visible type alone cannot fulfill COHERENT_SYSTEMWIDE. Nor can it
 // fulfill the existing upload policy, which already requires HOST_COHERENT.
 req.memory_type_bits=1u<<3;events.clear();const auto before_allocation=next_allocation;
 assert(owner.allocate(&req,&memory)==E_INVALIDARG && !memory.memory && events.empty() && next_allocation==before_allocation);
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE;
 assert(owner.allocate(&req,&memory)==E_INVALIDARG && !memory.memory && events.empty() && next_allocation==before_allocation);
 heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;expected_type=3;
 assert(owner.allocate(&req,&memory)==S_OK && events=="AMI" && memory.memory_type_index==3);
 assert(owner.free(&memory)==S_OK && events=="AMIVUD");req.memory_type_bits=0;
 const auto coherent_flags=D3D12DDI_HEAP_FLAGS(D3D12DDI_HEAP_FLAG_BUFFERS|D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE);
 heap.Flags=coherent_flags;
 auto reject_before_probe=[&](){
  const auto old_probes=probes;const auto old_allocation=next_allocation;events.clear();
  assert(owner.allocate(&req,&memory)==E_NOTIMPL && !memory.memory && events.empty());
  assert(probes==old_probes && next_allocation==old_allocation);
 };
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;reject_before_probe();
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
 heap.MemoryPool=D3D12DDI_MEMORY_POOL_L1;reject_before_probe();
 heap.MemoryPool=static_cast<D3D12DDI_MEMORY_POOL>(2);reject_before_probe();heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 heap.CreationNodeMask=2;reject_before_probe();heap.CreationNodeMask=1;
 heap.VisibleNodeMask=2;reject_before_probe();heap.VisibleNodeMask=1;
 heap.Flags=D3D12DDI_HEAP_FLAGS(unsigned(coherent_flags)|0x80000000u);reject_before_probe();
 resource.ResourceType=D3D12DDI_RT_TEXTURE2D;heap.Flags=coherent_flags;reject_before_probe();resource.ResourceType=D3D12DDI_RT_BUFFER;
 heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
 assert(creates==1 && makes_resident==0);
 expected_type=0;heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
 // A committed texture on a heap without CPU access is raw memory like a buffer.
 for(const auto type:{D3D12DDI_RT_TEXTURE1D,D3D12DDI_RT_TEXTURE2D,D3D12DDI_RT_TEXTURE3D})
 for(const auto flags:{D3D12DDI_HEAP_FLAG_BUFFERS,D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES,D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES}){
  resource.ResourceType=type;
  heap.Flags=flags;events.clear();assert(owner.allocate(&req,&memory)==S_OK && events=="AMI" && memory.memory_type_index==0);
  assert(owner.free(&memory)==S_OK && events=="AMIVUD");
 }
 resource.ResourceType=D3D12DDI_RT_TEXTURE2D;
 // System-wide coherence alone refuses an otherwise admitted texture.
 heap.Flags=D3D12DDI_HEAP_FLAGS(unsigned(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)|unsigned(D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE));
 reject_before_probe();
 // Refused before any probe or allocation: a texture that is not the heap's one resource, a texture on a
 // CPU-visible heap, an unknown resource type, and a heap for buffers that does not allow buffers.
 heap.Flags=D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
 req.flags=0;reject_before_probe();req.flags=engine_ddi::kMemoryDedicated;
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;reject_before_probe();
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE;reject_before_probe();
 heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
 resource.ResourceType=static_cast<D3D12DDI_RESOURCE_TYPE>(0x7f);reject_before_probe();
 resource.ResourceType=D3D12DDI_RT_BUFFER;reject_before_probe();
 req.resource=nullptr;reject_before_probe();req.resource=&resource;
 heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;
 req.flags=engine_ddi::kMemoryPrimary;assert(owner.allocate(&req,&memory)==E_NOTIMPL);req.flags=engine_ddi::kMemoryDedicated;
 req.memory_type_bits=2;assert(owner.allocate(&req,&memory)==E_INVALIDARG);req.memory_type_bits=0;
 gpu_address+=4096;events.clear();assert(owner.allocate(&req,&memory)==E_INVALIDARG && events=="AMUD" && !memory.memory);gpu_address-=4096;
 events.clear();fail_import=true;assert(owner.allocate(&req,&memory)==E_OUTOFMEMORY && !memory.memory && events=="AMIUD");fail_import=false;
 events.clear();assert(owner.allocate(&req,&memory)==S_OK);fail_free=true;assert(owner.free(&memory)==E_FAIL && events=="AMIVU");
 fail_free=false;assert(owner.close_after_engine_retirement()==S_OK && events=="AMIVUUDP");
 // The runtime refuses the release by handle list: the record and its allocation stay, and the later
 // cleanup asks again with the same flags, without a second Vulkan free or unmap.
 events.clear();assert(owner.allocate(&req,&memory)==S_OK);fail_deallocate=true;
 assert(owner.free(&memory)==E_FAIL && events=="AMIVUD" && owner.owns_allocation(memory.allocation));
 assert(owner.last_free_report().stage==FreeStage::Deallocate && !owner.last_free_report().owner_expired);
 events.clear();assert(owner.close_after_engine_retirement()==E_FAIL && events=="D" && owner.owns_allocation(memory.allocation));
 // Owned for its cleanup, but retired: it is no backing for a new view.
 assert(!owner.borrow_backing(memory.allocation));
 fail_deallocate=false;events.clear();
 assert(owner.close_after_engine_retirement()==S_OK && events=="DP" && !owner.owns_allocation(memory.allocation));
 // Backing lent to a runtime callback: its release waits for every return and changes nothing before.
 events.clear();assert(owner.allocate(&req,&memory)==S_OK && !owner.borrow_backing(memory.allocation+1));
 assert(owner.borrow_backing(memory.allocation) && owner.borrow_backing(memory.allocation));
 assert(owner.free(&memory)==E_PENDING && events=="AMI");owner.return_backing(memory.allocation);
 assert(owner.free(&memory)==E_PENDING && events=="AMI");owner.return_backing(memory.allocation);
 owner.return_backing(memory.allocation);assert(owner.borrow_backing(memory.allocation));owner.return_backing(memory.allocation);
 assert(owner.free(&memory)==S_OK && events=="AMIVUD" && !owner.borrow_backing(memory.allocation));
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
   assert(owner.allocate(&s,&memory)==S_OK && events=="AMI" && memory.memory_type_index==0 && memory.byte_size==65536);}
  {OwnerScope destroy(&owner,memory.allocation);
   assert(owner.free(&memory)==S_OK && events=="AMIVUR" && probes==old_probes && surfaces==old_surfaces+1);
   assert(!owner.last_free_report().owner_expired && owner.last_free_report().surface);}
  assert(!owner.owns_allocation(memory.allocation));
  // A page-aligned address is enough for it; raw memory above needed 64 KiB. Handed back inside
  // the DDI that created it, as after an import the engine refuses.
  gpu_address+=4096;target.Format=DXGI_FORMAT_R8G8B8A8_UNORM;surface_format=D3DDDIFMT_A8B8G8R8;events.clear();
  {OwnerScope create(&owner,0);
   assert(owner.allocate(&s,&memory)==S_OK && events=="AMI" && memory.gpu_va==gpu_address);
   gpu_address-=4096;memory_va_bias=4096;assert(owner.free(&memory)==S_OK && events=="AMIVUR");memory_va_bias=0;}
  // Construction that fails after the allocation releases it in the same DDI, by the same form.
  {OwnerScope create(&owner,0);target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;surface_format=D3DDDIFMT_A8R8G8B8;
   events.clear();fail_import=true;
   assert(owner.allocate(&s,&memory)==E_OUTOFMEMORY && !memory.memory && events=="AMIUR");fail_import=false;}
  target.Format=DXGI_FORMAT_R8G8B8A8_UNORM;surface_format=D3DDDIFMT_A8B8G8R8;
  // The image's memory types are the only ones: type 0 is not among these.
  s.memory_type_bits=2;events.clear();assert(owner.allocate(&s,&memory)==E_INVALIDARG && events.empty());s.memory_type_bits=1;
  auto refused=[&](HRESULT expected){
   const auto before=next_allocation;events.clear();
   assert(owner.allocate(&s,&memory)==expected && !memory.memory && events.empty() && next_allocation==before);
  };
  // Refused before any callback: a description the surface does not exist for, and one that the
  // reader's rules do not admit.
  target.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;refused(E_NOTIMPL);target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
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
 // A genuinely pending mapping times out without import or premature release.
 pending=true;completed=10;events.clear();assert(owner.allocate(&req,&memory)==E_PENDING && !memory.memory && events=="AM");
 completed=20;pending=false;assert(owner.close_after_engine_retirement()==S_OK && events=="AMUDP");
 assert(owner.discard_metadata()==0 && owner.allocate(&req,&memory)==E_UNEXPECTED && !owner.owns_allocation(next_allocation));
 assert(makes_resident==0);assert(surfaces==3);
 // The owner's authority ends with its DDI. Records that outlive it keep the allocation and never
 // reach the runtime again, in either form. A second owner, so that the first one's closure above
 // stays what it was.
 {
  RuntimeHeapImports late(device,domain,handle<VkPhysicalDevice>(3),handle<VkDevice>(4),handle<VkInstance>(5),gipa,identity);
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
   assert(late.free(&first)==E_FAIL && events=="AMIVUR");fail_deallocate=false;}
  events.clear();assert(late.close_after_engine_retirement()==kOwnerExpired && events.empty());
  assert(late.last_free_report().owner_expired && late.owns_allocation(first.allocation));
  // 2. The work retires only after the resource's DDI has returned: the release comes from a later
  // DDI, here the destruction of another primary, and names neither owner.
  events.clear();{OwnerScope create(&late,0);assert(late.allocate(&s,&second)==S_OK);}
  {OwnerScope create(&late,0);assert(late.allocate(&s,&third)==S_OK);}
  {OwnerScope destroy(&late,second.allocation);}
  events.clear();
  {OwnerScope destroy(&late,third.allocation);
   assert(late.free(&second)==kOwnerExpired && events=="VU" && late.last_free_report().owner_expired);
   assert(late.free(&third)==S_OK && events=="VUVUR");}
  assert(late.owns_allocation(second.allocation) && !late.owns_allocation(third.allocation));
  // Outside any resource DDI the same holds.
  events.clear();{OwnerScope create(&late,0);assert(late.allocate(&s,&third)==S_OK);}
  events.clear();assert(late.free(&third)==kOwnerExpired && events=="VU");
  // 3. Construction fails and its release fails: the record stays, without an owner.
  events.clear();{OwnerScope create(&late,0);fail_import=true;fail_deallocate=true;
   assert(late.allocate(&s,&memory)==E_OUTOFMEMORY && events=="AMIUR");fail_import=false;fail_deallocate=false;}
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
    assert(late.free(&outer)==S_OK && events=="VUR");
   })==S_OK && outer_calls==1 && !inner_calls && !late.owns_allocation(outer.allocation));
   events.clear();
   assert(create_in_owner_scope(&late,[&]() noexcept {
    ++outer_calls;
    assert(destroy_in_owner_scope(&late,0,[&]() noexcept {++inner_calls;})==E_UNEXPECTED);
    assert(late.allocate(&s,&outer)==S_OK && late.free(&outer)==S_OK && events=="AMIVUR");
    return S_OK;
   })==S_OK && outer_calls==2 && !inner_calls);
   // Without imports there is no scope to refuse: the engine call runs.
   assert(create_in_owner_scope(nullptr,[&]() noexcept {++inner_calls;return S_FALSE;})==S_FALSE && inner_calls==1);events.clear();
  }
  // Four allocations stay owned, and the paging queue with them.
  assert(late.discard_metadata()==5 && events.empty());
  heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.MemoryPool=D3D12DDI_MEMORY_POOL_L0;
 }
 assert(surfaces==10);
 std::puts("PASS heap import: DEFAULT/UPLOAD/READBACK, coherent L0 policy and rejection, exact private import, borrowed map, ordered cleanup, pending retention, no residency, linear primary as an LB7A surface under E26R, "
  "released by its runtime resource inside that resource's DDI only");
}
