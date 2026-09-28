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
// Caller owns image/layout compatibility and the runtime allocation. On success
// it owns returned VkDeviceMemory: destroy the image before freeing that memory,
// then release runtime allocation/VA only after all GPU users have retired.
HRESULT import_runtime_image_memory(RuntimeDevice &,VkDevice,VkImage,
    const ImageMemoryDispatch &,const VkPhysicalDeviceMemoryProperties &,
    const bc250_host_import &,VkDeviceMemory &);
}
