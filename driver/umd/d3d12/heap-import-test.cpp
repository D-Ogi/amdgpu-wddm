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
static bool pending=false,fail_free=false,fail_import=false,unlock_on_free=false;
static unsigned surfaces=0;
static unsigned long surface_format=0;
static unsigned creates=0,makes_resident=0,probes=0;
static uint32_t expected_type=0;
static void* identity=handle<void*>(0x5432);
static RuntimeHeapImports* imports;
static char mapped[65536];
static HRESULT APIENTRY allocate_cb(D3D12DDI_HRTDEVICE d,D3D12DDICB_ALLOCATE_0022* a){
 assert(d.handle==handle<void*>(1) && a->hResource==handle<void*>(2) && !a->hKMResource);
 assert(a->NumAllocations==1 && !a->pPrivateDriverData && !a->PrivateDriverDataSize);
 if(a->pAllocationInfo->PrivateDriverDataSize==sizeof(BC250_WDDM_ALLOCATION_PRIVATE)){
  // The primary: the 32-byte LB7A v1 description alone, PRIMARY, no video present source.
  auto s=static_cast<const BC250_WDDM_ALLOCATION_PRIVATE*>(a->pAllocationInfo->pPrivateDriverData);
  assert(s->Magic==0x4137424Cul && s->Version==1 && s->Width==256 && s->Height==64 && s->Pitch==1024);
  assert(s->Format==surface_format && s->Size==65536);
  assert(a->pAllocationInfo->Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY);
  assert(a->pAllocationInfo->VidPnSourceId==D3DDDI_ID_UNINITIALIZED);
  ++surfaces;events+='A';a->pAllocationInfo->hAllocation=++next_allocation;return S_OK;
 }
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
static HRESULT APIENTRY deallocate_cb(D3D12DDI_HRTDEVICE,const D3D12DDICB_DEALLOCATE_0022* a){assert(a->NumAllocations==1);events+='D';return S_OK;}
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
  assert(owner.allocate(&s,&memory)==S_OK && events=="AMI" && memory.memory_type_index==0 && memory.byte_size==65536);
  assert(owner.free(&memory)==S_OK && events=="AMIVUD" && probes==old_probes && surfaces==old_surfaces+1);
  // A page-aligned address is enough for it; raw memory above needed 64 KiB.
  gpu_address+=4096;target.Format=DXGI_FORMAT_R8G8B8A8_UNORM;surface_format=D3DDDIFMT_A8B8G8R8;events.clear();
  assert(owner.allocate(&s,&memory)==S_OK && events=="AMI" && memory.gpu_va==gpu_address);
  gpu_address-=4096;memory_va_bias=4096;assert(owner.free(&memory)==S_OK && events=="AMIVUD");memory_va_bias=0;
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
 assert(makes_resident==0);assert(surfaces==2);
 std::puts("PASS heap import: DEFAULT/UPLOAD/READBACK, coherent L0 policy and rejection, exact private import, borrowed map, ordered cleanup, pending retention, no residency, linear primary as an LB7A surface");
}
