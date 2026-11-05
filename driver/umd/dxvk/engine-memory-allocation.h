// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
// BD-309: the private query of the hosted RADV ICD that names the kernel allocation of a VkDeviceMemory. A copy of
// Mesa's src/amd/vulkan/radv_bc250_memory.h, revision 1 (mesa-amdgpu-wddm, branch t38/bd309-memory-allocation-hosted):
// the shell does not build against Mesa headers. The ICD answers the name through vk_icdGetInstanceProcAddr with a
// NULL instance; an ICD without the query answers NULL, and the shell then offers engine resources as hints only.
namespace bc250::umd {
inline constexpr const char *kMemoryAllocationQueryName="bc250GetMemoryAllocation";
inline constexpr uint32_t kMemoryAllocationRevision=1u;
inline constexpr uint32_t kMemoryAllocationCpuMapped=0x1u; // the ICD holds a CPU map (a Lock2) of the allocation
struct MemoryAllocationInfo {
    uint32_t revision;   // in: kMemoryAllocationRevision
    uint32_t size;       // in: sizeof(MemoryAllocationInfo)
    uint32_t allocation; // out: D3DKMT_HANDLE of the one kernel allocation of the memory
    uint32_t flags;      // out: kMemoryAllocationCpuMapped
    uint64_t bytes;      // out: size of the allocation
};
static_assert(sizeof(MemoryAllocationInfo)==24,"layout of radv_bc250_memory.h revision 1");
// VK_SUCCESS with the allocation; VK_ERROR_FEATURE_NOT_PRESENT for memory that is not one allocation of this ICD
// (imported, sparse); VK_ERROR_INITIALIZATION_FAILED for bad arguments.
typedef VkResult (VKAPI_PTR *PFN_bc250GetMemoryAllocation)(VkDevice device,VkDeviceMemory memory,
    MemoryAllocationInfo *info);
}
