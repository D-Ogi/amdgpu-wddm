// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "ddi/bc250_dxvk_engine.h"
namespace bc250::umd {
struct ImageMemoryDispatch {
    PFN_vkGetImageMemoryRequirements requirements;
    PFN_vkAllocateMemory allocate;
    PFN_vkBindImageMemory bind;
    PFN_vkFreeMemory free;
};
struct RuntimeImage { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; };
struct RuntimeImageDispatch {
    PFN_vkCreateImage create;
    PFN_vkDestroyImage destroy;
    PFN_vkGetImageSubresourceLayout layout;
    ImageMemoryDispatch memory;
};
// First import path: RGBA8/BGRA8, single-mip/layer linear 2D surface.
// imageInfo retains the engine-required format/usage/flags/pNext. The caller
// supplies verified runtime pitch and bytes occupied by one logical row.
HRESULT create_linear_runtime_image(RuntimeDevice &,VkDevice,const RuntimeImageDispatch &,
    const VkPhysicalDeviceMemoryProperties &,const VkImageCreateInfo &,
    const bc250_host_import &,VkDeviceSize pitch,VkDeviceSize rowBytes,RuntimeImage &);
// Caller must first retire GPU use and destroy all engine wrappers/views.
void destroy_runtime_image(VkDevice,const RuntimeImageDispatch &,RuntimeImage &);
// Caller owns image/layout compatibility and the runtime allocation. On success
// it owns returned VkDeviceMemory: destroy the image before freeing that memory,
// then release runtime allocation/VA only after all GPU users have retired.
HRESULT import_runtime_image_memory(RuntimeDevice &,VkDevice,VkImage,
    const ImageMemoryDispatch &,const VkPhysicalDeviceMemoryProperties &,
    const bc250_host_import &,VkDeviceMemory &);
}
