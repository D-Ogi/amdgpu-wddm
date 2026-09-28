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
namespace {
unsigned imageCreates=0,imageDestroys=0;
bool badPitch=false;
VkResult VKAPI_CALL image_create(VkDevice,const VkImageCreateInfo *info,const VkAllocationCallbacks *,VkImage *out) {
    ++imageCreates; check(info->usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    *out=reinterpret_cast<VkImage>(uintptr_t(2)); return VK_SUCCESS;
}
void VKAPI_CALL image_destroy(VkDevice,VkImage,const VkAllocationCallbacks *) { ++imageDestroys; }
void VKAPI_CALL image_layout(VkDevice,VkImage,const VkImageSubresource *sub,VkSubresourceLayout *out) {
    check(sub->aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && !sub->mipLevel && !sub->arrayLayer);
    *out={0,4096,badPitch ? 512u : 256u,0,0};
}
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
    RuntimeImageDispatch imageDispatch{image_create,image_destroy,image_layout,dispatch};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.extent={64,16,1}; info.mipLevels=info.arrayLayers=1;
    info.format=VK_FORMAT_R8G8B8A8_UNORM; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_LINEAR; info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    RuntimeImage importedImage{};
    auto createImage=[&]() { return create_linear_runtime_image(runtime,device,imageDispatch,properties,info,imported,256,256,importedImage); };
    badPitch=true; check(createImage()==E_INVALIDARG && imageDestroys==1 && allocations==3 && !importedImage.image);
    badPitch=false; bindResult=VK_ERROR_DEVICE_LOST;
    check(createImage()==D3DDDIERR_DEVICEREMOVED && imageDestroys==2 && frees==2 && !importedImage.image);
    bindResult=VK_SUCCESS; check(createImage()==S_OK && importedImage.image && importedImage.memory);
    destroy_runtime_image(device,imageDispatch,importedImage);
    check(!importedImage.image && !importedImage.memory && imageDestroys==3 && frees==3);
    destroy_runtime_image(device,imageDispatch,importedImage);
    info.extent.height=UINT32_MAX; check(createImage()==E_INVALIDARG && imageCreates==3);
    info.extent.height=16; info.tiling=VK_IMAGE_TILING_OPTIMAL; check(createImage()==E_NOTIMPL && imageCreates==3);
    // This test uses synthetic handles only, never an actual allocation/image.
    std::cout << "PASS private image-memory import descriptor, validation and rollback (mock Vulkan)\n";
}
