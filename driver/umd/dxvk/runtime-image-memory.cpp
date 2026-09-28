// SPDX-License-Identifier: MIT
#include "runtime-image-memory.h"
namespace bc250::umd {
namespace {
HRESULT status(VkResult result) {
    if (result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY) return E_OUTOFMEMORY;
    if (result==VK_ERROR_DEVICE_LOST) return D3DDDIERR_DEVICEREMOVED;
    return E_FAIL;
}
}
HRESULT import_runtime_image_memory(RuntimeDevice &runtime,VkDevice device,VkImage image,
    const ImageMemoryDispatch &vk,const VkPhysicalDeviceMemoryProperties &properties,
    const bc250_host_import &source,VkDeviceMemory &out) {
    if (out) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !device || !image || !vk.requirements || !vk.allocate || !vk.bind || !vk.free ||
        source.sType!=BC250_HOST_IMPORT_STYPE || source.identity!=runtime.hDevice || !source.identity ||
        !source.allocation || !source.va || !source.size || source.va>UINT64_MAX-source.size ||
        properties.memoryTypeCount>VK_MAX_MEMORY_TYPES) return E_INVALIDARG;
    VkMemoryRequirements requirements{}; vk.requirements(device,image,&requirements);
    if (!requirements.size || requirements.size>source.size || !requirements.alignment ||
        source.va%requirements.alignment) return E_INVALIDARG;
    UINT memoryType=properties.memoryTypeCount;
    for (UINT i=0;i<properties.memoryTypeCount;++i) {
        if ((requirements.memoryTypeBits & (1u<<i)) &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { memoryType=i; break; }
    }
    if (memoryType==properties.memoryTypeCount) return E_NOTIMPL;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image=image;
    bc250_host_import imported=source; imported.pNext=&dedicated;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext=&imported; allocation.allocationSize=requirements.size; allocation.memoryTypeIndex=memoryType;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkResult result=vk.allocate(device,&allocation,nullptr,&memory);
    if (result!=VK_SUCCESS) return status(result);
    if (!memory) return E_FAIL;
    result=vk.bind(device,image,memory,0);
    if (result!=VK_SUCCESS) { vk.free(device,memory,nullptr); return status(result); }
    out=memory; return S_OK;
}
}
