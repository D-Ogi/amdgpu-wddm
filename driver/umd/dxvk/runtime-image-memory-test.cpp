// SPDX-License-Identifier: MIT
#include "runtime-image-memory.h"
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
namespace {
int identity; unsigned allocations,bindings,frees;
VkDeviceMemory memory=reinterpret_cast<VkDeviceMemory>(uintptr_t(7));
VkResult allocationResult=VK_SUCCESS,bindResult=VK_SUCCESS;
void check(bool b) { if (!b) std::abort(); }
void VKAPI_CALL requirements(VkDevice,VkImage,VkMemoryRequirements *r) { *r={4096,256,2}; }
VkResult VKAPI_CALL allocate(VkDevice,const VkMemoryAllocateInfo *a,const VkAllocationCallbacks *,VkDeviceMemory *m) {
    ++allocations;
    auto *i=static_cast<const bc250_host_import *>(a->pNext);
    auto *d=static_cast<const VkMemoryDedicatedAllocateInfo *>(i->pNext);
    check(i->identity==&identity && i->allocation==31 && i->va==65536 && i->size==8192 && d->image);
    check(d->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && a->allocationSize==4096 && a->memoryTypeIndex==1);
    if (allocationResult==VK_SUCCESS) *m=memory; return allocationResult;
}
VkResult VKAPI_CALL bind(VkDevice,VkImage,VkDeviceMemory m,VkDeviceSize offset) { ++bindings; check(m==memory && !offset); return bindResult; }
void VKAPI_CALL release(VkDevice,VkDeviceMemory m,const VkAllocationCallbacks *) { ++frees; check(m==memory); }
}
int main() {
    RuntimeDevice runtime; runtime.hDevice=&identity;
    VkDevice device=reinterpret_cast<VkDevice>(uintptr_t(1)); VkImage image=reinterpret_cast<VkImage>(uintptr_t(2));
    ImageMemoryDispatch dispatch{requirements,allocate,bind,release};
    VkPhysicalDeviceMemoryProperties properties{}; properties.memoryTypeCount=2; properties.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    bc250_host_import imported{}; imported.sType=BC250_HOST_IMPORT_STYPE; imported.identity=&identity;
    imported.allocation=31; imported.va=65536; imported.size=8192;
    VkDeviceMemory out=VK_NULL_HANDLE;
    auto run=[&]() { return import_runtime_image_memory(runtime,device,image,dispatch,properties,imported,out); };
    check(run()==E_INVALIDARG && !allocations);
    RuntimeDomain::Scope scope(runtime.domain);
    imported.size=100; check(run()==E_INVALIDARG && !allocations); imported.size=8192;
    ++imported.va; check(run()==E_INVALIDARG && !allocations); --imported.va;
    imported.identity=nullptr; check(run()==E_INVALIDARG && !allocations); imported.identity=&identity;
    allocationResult=VK_ERROR_OUT_OF_DEVICE_MEMORY; check(run()==E_OUTOFMEMORY && !out && !bindings && !frees);
    allocationResult=VK_SUCCESS; bindResult=VK_ERROR_DEVICE_LOST;
    check(run()==D3DDDIERR_DEVICEREMOVED && !out && frees==1);
    bindResult=VK_SUCCESS; check(run()==S_OK && out==memory && allocations==3 && bindings==2 && frees==1);
    check(run()==E_UNEXPECTED && allocations==3);
    // This test uses synthetic handles only, never an actual allocation/image.
    std::cout << "PASS private image-memory import descriptor, validation and rollback (mock Vulkan)\n";
}
