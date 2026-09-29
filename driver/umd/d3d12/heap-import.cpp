// SPDX-License-Identifier: MIT
#include "heap-import.h"
#include "device-state.h"
#include "allocation-request.h"
#include <bc250_host_bootstrap.h>
#include <d3dkmthk.h>
#include <algorithm>
#include <new>
namespace native12 {
namespace {
HRESULT from_vk(VkResult r) noexcept {
    if(r==VK_SUCCESS)return S_OK;
    if(r==VK_ERROR_OUT_OF_HOST_MEMORY || r==VK_ERROR_OUT_OF_DEVICE_MEMORY)return E_OUTOFMEMORY;
    if(r==VK_ERROR_DEVICE_LOST)return DXGI_ERROR_DEVICE_REMOVED;
    return E_FAIL;
}
}
struct RuntimeHeapImports::Record {
    RuntimeAllocation allocation;
    GpuMapping mapping;
    engine_ddi::ImportedMemory imported{};
    Record* next{};
    bool retired{true},locked{},busy{};
    Record(D3D12DDI_HRTDEVICE d,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb):allocation(d,cb) {}
};
RuntimeHeapImports::RuntimeHeapImports(Device& d,bc250::umd::RuntimeDomain& domain,
    VkPhysicalDevice physical,VkDevice device,VkInstance instance,PFN_vkGetInstanceProcAddr gipa,void* identity) noexcept
    :runtime_(d.runtime),callbacks_(d.callbacks),kernel_(d.kernel_callbacks),domain_(domain),
     paging_(d.runtime,d.kernel_callbacks),physical_(physical),device_(device),instance_(instance),gipa_(gipa),identity_(identity) {}
RuntimeHeapImports::~RuntimeHeapImports(){discard_metadata();}
HRESULT RuntimeHeapImports::initialize() noexcept {
    if(!active_ || !domain_.entered())return E_UNEXPECTED;
    if(initialized_)return S_OK;
    if(!runtime_.handle || !physical_ || !device_ || !instance_ || !gipa_ || !identity_)return E_INVALIDARG;
    auto props=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa_(instance_,"vkGetPhysicalDeviceMemoryProperties"));
    auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa_(instance_,"vkGetDeviceProcAddr"));
    if(!props || !gdpa)return E_NOINTERFACE;
    allocate_=reinterpret_cast<PFN_vkAllocateMemory>(gdpa(device_,"vkAllocateMemory"));
    free_=reinterpret_cast<PFN_vkFreeMemory>(gdpa(device_,"vkFreeMemory"));
    create_buffer_=reinterpret_cast<PFN_vkCreateBuffer>(gdpa(device_,"vkCreateBuffer"));
    destroy_buffer_=reinterpret_cast<PFN_vkDestroyBuffer>(gdpa(device_,"vkDestroyBuffer"));
    requirements_=reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(gdpa(device_,"vkGetBufferMemoryRequirements"));
    if(!allocate_ || !free_ || !create_buffer_ || !destroy_buffer_ || !requirements_)return E_NOINTERFACE;
    props(physical_,&properties_);
    if(!properties_.memoryTypeCount || properties_.memoryTypeCount>VK_MAX_MEMORY_TYPES)return E_UNEXPECTED;
    initialized_=true;return S_OK;
}
RuntimeHeapImports::Record* RuntimeHeapImports::find(D3DKMT_HANDLE handle) const noexcept {
    if(!handle)return nullptr;
    for(auto r=records_;r;r=r->next)if(r->allocation.handle()==handle)return r;
    return nullptr;
}
bool RuntimeHeapImports::owns_allocation(D3DKMT_HANDLE handle) const noexcept {
    return active_ && domain_.entered() && find(handle)!=nullptr;
}
void RuntimeHeapImports::erase(Record* record) noexcept {
    auto link=&records_;while(*link && *link!=record)link=&(*link)->next;
    if(*link){*link=record->next;delete record;}
}
HRESULT RuntimeHeapImports::release(Record& record) noexcept {
    if(!record.retired || record.busy)return E_UNEXPECTED;
    // Engine objects and uses have retired before free() reaches this point.
    // RADV may call Unlock2 from vkFreeMemory; keep the allocation record live.
    if(record.imported.memory){free_(device_,record.imported.memory,nullptr);record.imported.memory=VK_NULL_HANDLE;}
    if(record.locked)return E_UNEXPECTED;
    HRESULT hr=paging_.unmap_after_gpu_retirement(record.mapping);
    if(hr!=S_OK)return hr;
    return record.allocation.close();
}
HRESULT RuntimeHeapImports::allocate(const engine_ddi::MemoryRequest* request,engine_ddi::ImportedMemory* out) noexcept {
    if(out)*out={};
    if(!active_ || !domain_.entered() || !initialized_)return E_UNEXPECTED;
    if(!request || !out || request->size!=sizeof(*request) || !request->heap || request->reserved || !request->byte_size ||
       !request->alignment || (request->alignment&(request->alignment-1)))return E_INVALIDARG;
    const auto& heap=*request->heap;
    const unsigned allowed=D3D12DDI_HEAP_FLAG_BUFFERS|D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|
        D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE;
    if((request->flags&~engine_ddi::kMemoryDedicated) || (unsigned(heap.Flags)&~allowed) ||
       heap.CreationNodeMask>1 || heap.VisibleNodeMask>1)return E_NOTIMPL;
    // The allocation is raw memory; the engine places the buffer or image in it. A texture is admitted
    // only as the one resource of a heap without CPU access: CPU-visible texture layouts, primaries and
    // texture-only heaps for later placement each need their own contract.
    bool texture=false;
    if(request->resource)switch(request->resource->ResourceType){
    case D3D12DDI_RT_BUFFER:break;
    case D3D12DDI_RT_TEXTURE1D:case D3D12DDI_RT_TEXTURE2D:case D3D12DDI_RT_TEXTURE3D:texture=true;break;
    default:return E_NOTIMPL;
    }
    constexpr unsigned textures=D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    if(texture){
        if(!(request->flags&engine_ddi::kMemoryDedicated) || !(unsigned(heap.Flags)&(textures|D3D12DDI_HEAP_FLAG_BUFFERS)) ||
           heap.CPUPageProperty!=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE ||
           (heap.Flags&D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE))return E_NOTIMPL;
    } else if(!(heap.Flags&D3D12DDI_HEAP_FLAG_BUFFERS))return E_NOTIMPL;
    AllocationAccess access{};VkMemoryPropertyFlags want{};
    switch(heap.CPUPageProperty){
    case D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE:access=AllocationAccess::GpuOnly;want=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;break;
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE:access=AllocationAccess::CpuWriteCombined;want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;break;
    case D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK:access=AllocationAccess::CpuCached;want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;break;
    default:return E_NOTIMPL;
    }
    if(heap.Flags&D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE){
        // The admitted adapter supplies coherent GTT CPU mappings. Limit this
        // promise to CPU-visible system memory and require the same property
        // on the Vulkan type importing that runtime allocation.
        if(heap.MemoryPool!=D3D12DDI_MEMORY_POOL_L0 || access==AllocationAccess::GpuOnly)return E_NOTIMPL;
        want|=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    VkBufferCreateInfo probe_info{};probe_info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    probe_info.size=request->byte_size;probe_info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;probe_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer probe{};HRESULT hr=from_vk(create_buffer_(device_,&probe_info,nullptr,&probe));if(FAILED(hr))return hr;
    if(!probe)return E_UNEXPECTED;
    VkMemoryRequirements needs{};requirements_(device_,probe,&needs);destroy_buffer_(device_,probe,nullptr);
    uint32_t bits=needs.memoryTypeBits;if(request->memory_type_bits)bits&=request->memory_type_bits;
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<properties_.memoryTypeCount;++i)if((bits&(1u<<i)) &&
        (properties_.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
    if(type==UINT32_MAX)return E_INVALIDARG;
    const uint64_t alignment=std::max<uint64_t>(65536,std::max<uint64_t>(request->alignment,needs.alignment));
    AllocationRequest allocation;hr=allocation.prepare(std::max<uint64_t>(request->byte_size,needs.size),alignment,access,request->rt_owner.handle);
    if(FAILED(hr))return hr;
    if(!paging_open_){paging_open_=true;hr=paging_.open();if(hr!=S_OK)return hr;}
    auto record=new(std::nothrow) Record(runtime_,callbacks_);if(!record)return E_OUTOFMEMORY;
    record->next=records_;records_=record;record->busy=true;
    hr=record->allocation.open(allocation.args);record->busy=false;
    if(hr==S_OK)hr=paging_.map(record->allocation.handle(),allocation.blob.alloc_size,record->mapping);
    UINT64 address=0;
    if(hr==S_OK){
        const ULONGLONG start=GetTickCount64();
        do {hr=paging_.ready(record->mapping,&address);if(hr!=E_PENDING || GetTickCount64()-start>=2000)break;Sleep(1);}while(true);
    }
    if(hr==S_OK && (address&(alignment-1)))hr=E_INVALIDARG;
    if(hr==S_OK){
        bc250_host_import host{};host.sType=BC250_HOST_IMPORT_FLAGS_STYPE;host.identity=identity_;
        host.allocation=record->allocation.handle();host.va=address;host.size=allocation.blob.alloc_size;
        // The engine maps a CPU-visible heap. dispatch() answers the ICD's Lock2 and Unlock2 for it.
        if(access!=AllocationAccess::GpuOnly)host.flags=BC250_HOST_IMPORT_CPU_MAP;
        VkMemoryAllocateFlagsInfo flags{};flags.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;flags.pNext=&host;
        VkMemoryAllocateInfo info{};info.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;info.pNext=&flags;
        info.allocationSize=host.size;info.memoryTypeIndex=type;
        hr=from_vk(allocate_(device_,&info,nullptr,&record->imported.memory));
        if(hr==S_OK && !record->imported.memory)hr=E_UNEXPECTED;
        if(hr==S_OK){record->imported={sizeof(engine_ddi::ImportedMemory),type,record->imported.memory,
            host.size,host.allocation,0,address,record};record->retired=false;*out=record->imported;return S_OK;}
    }
    // Failed construction transfers nothing to engine-ddi. Pending mapping and
    // cleanup failures remain owned for an explicit later close, never forgotten.
    if(release(*record)==S_OK)erase(record);
    return hr;
}
HRESULT RuntimeHeapImports::free(const engine_ddi::ImportedMemory* memory) noexcept {
    if(!active_ || !domain_.entered())return E_UNEXPECTED;
    if(!memory || memory->size!=sizeof(*memory))return E_INVALIDARG;
    auto record=find(memory->allocation);
    if(!record || record!=memory->cookie || record->retired || record->imported.memory!=memory->memory ||
       record->imported.byte_size!=memory->byte_size || record->imported.gpu_va!=memory->gpu_va)return E_INVALIDARG;
    record->retired=true;HRESULT hr=release(*record);if(hr==S_OK)erase(record);return hr;
}
HRESULT RuntimeHeapImports::dispatch(uint32_t op,void* argument) noexcept {
    if(!active_ || !domain_.entered())return E_UNEXPECTED;
    if(!argument)return E_INVALIDARG;
    if(op==BC250_HOST_Lock2){
        auto& a=*static_cast<D3DKMT_LOCK2*>(argument);auto record=find(a.hAllocation);
        if(!record || record->busy || record->locked || !kernel_.pfnLock2Cb)return E_INVALIDARG;
        D3DDDICB_LOCK2 b{};b.hAllocation=a.hAllocation;b.Flags.Value=a.Flags.Value;
        record->busy=true;HRESULT hr=kernel_.pfnLock2Cb(runtime_.handle,&b);record->busy=false;
        if(hr==S_OK){record->locked=true;a.pData=b.pData;if(!b.pData)return E_UNEXPECTED;}return hr;
    }
    if(op==BC250_HOST_Unlock2){
        auto& a=*static_cast<D3DKMT_UNLOCK2*>(argument);auto record=find(a.hAllocation);
        if(!record || record->busy || !record->locked || !kernel_.pfnUnlock2Cb)return E_INVALIDARG;
        D3DDDICB_UNLOCK2 b{};b.hAllocation=a.hAllocation;
        record->busy=true;HRESULT hr=kernel_.pfnUnlock2Cb(runtime_.handle,&b);record->busy=false;
        if(hr==S_OK)record->locked=false;return hr;
    }
    return E_NOTIMPL;
}
HRESULT RuntimeHeapImports::close_after_engine_retirement() noexcept {
    if(!active_ || !domain_.entered())return E_UNEXPECTED;
    HRESULT result=S_OK;
    for(auto record=records_;record;){auto next=record->next;
        if(!record->retired)result=E_PENDING;
        else {HRESULT hr=release(*record);if(hr==S_OK)erase(record);else result=hr;}
        record=next;
    }
    if(records_)return result==S_OK?E_PENDING:result;
    HRESULT hr=paging_.close();if(hr==S_OK)paging_open_=false;return hr;
}
unsigned RuntimeHeapImports::discard_metadata() noexcept {
    active_=false;unsigned count=paging_open_?1u:0u;paging_open_=false;
    paging_.invalidate_runtime();runtime_={};callbacks_={};kernel_={};
    while(records_){auto record=records_;records_=record->next;
        if(record->allocation.handle() || record->imported.memory)++count;
        record->allocation.invalidate_runtime();delete record;}
    return count;
}
}
