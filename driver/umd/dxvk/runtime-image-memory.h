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
// The image is retained while its layout is used to allocate backing storage.
// No engine pointer/pNext chain is retained. The caller owns the exact image,
// and must destroy it on every refusal before releasing its runtime allocation.
struct RuntimeImageLayout {
    VkFormat format=VK_FORMAT_UNDEFINED;
    UINT width=0,height=0,planes=0;
    VkSubresourceLayout plane[2]{};
    VkMemoryRequirements memory{};
};
// No memory allocation or binding. The create flags/usage/view-list are the
// engine's exact description, with LINEAR selected by the caller. Supports the
// current packed formats and one non-disjoint NV12 image. NV12 success describes
// storage only: no COMPOSED or overlay capability follows from it.
HRESULT prepare_linear_runtime_image(RuntimeDevice &,VkDevice,const RuntimeImageDispatch &,
    const VkImageCreateInfo &,RuntimeImage &,RuntimeImageLayout &);
// Import path: single-mip/layer linear 2D color surface of a composed format
// (A8 at 1 byte a texel, RGBA8/BGRA8/RGB10A2 at 4, RGBA16F at 8).
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
