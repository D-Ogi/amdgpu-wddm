// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-image-memory.h"
#include "../../contract/bc250_shared_resource.h"
namespace bc250::umd {
// BD-373 step 3: a shared resource in the ENGINE LAYOUT form (driver/contract/bc250_shared_resource.h). The memory is
// a runtime allocation (BC2A blob, S26R record); the engine's own buffer or LINEAR image is created on it, by the
// creator and again by every opener, from the same engine create info.
struct EngineResourceDesc {
    bool buffer=false;
    D3D11_BUFFER_DESC buffer_desc{};      // without the sharing flags
    D3D11_TEXTURE2D_DESC1 texture{};      // without the sharing flags; TextureLayout UNDEFINED
};
// VkPhysicalDeviceIDProperties of the engine's device: what makes memory layouts of two devices compatible.
struct EngineLayoutIdentity {
    UINT8 driver[16]{},device[16]{};
    bool valid=false;
};
// The Vulkan half: the device functions of the shell's images and buffers, and the physical device's answer for
// LINEAR images.
struct EngineResourceVulkan {
    const RuntimeImageDispatch *image=nullptr;
    PFN_vkCreateBuffer create_buffer=nullptr;
    PFN_vkDestroyBuffer destroy_buffer=nullptr;
    PFN_vkGetBufferMemoryRequirements buffer_requirements=nullptr;
    PFN_vkBindBufferMemory bind_buffer=nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties image_format=nullptr;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    EngineLayoutIdentity identity{};
};
// The engine half (E5 of the engine ABI). describe_buffer and wrap_buffer are null on an engine below ABI 1.9: that
// engine has no IBc250DxvkDevice7, and a shared buffer is refused.
struct EngineResourceEngine {
    void *engine=nullptr;
    HRESULT (*describe_image)(void *,const D3D11_TEXTURE2D_DESC1 *,VkImageCreateInfo *)=nullptr;
    HRESULT (*wrap_image)(void *,const D3D11_TEXTURE2D_DESC1 *,const VkImageCreateInfo *,VkImage,ID3D11Texture2D **)=nullptr;
    HRESULT (*describe_buffer)(void *,const D3D11_BUFFER_DESC *,VkBufferCreateInfo *)=nullptr;
    HRESULT (*wrap_buffer)(void *,const D3D11_BUFFER_DESC *,const VkBufferCreateInfo *,VkBuffer,ID3D11Buffer **)=nullptr;
    HRESULT (*wait)(void *,ID3D11Resource *)=nullptr;
    ULONG (*release)(ID3D11Resource *)=nullptr;
};
struct EngineResourceDispatch {
    EngineResourceVulkan vk{};
    EngineResourceEngine engine{};
};
// The engine half for an engine device. It asks for IBc250DxvkDevice2 and IBc250DxvkDevice7 at each call, as
// texture_import_dispatch does; describe_buffer is null when the shell was built against an engine header below 1.9.
EngineResourceEngine engine_resource_engine(IBc250DxvkDevice &);
struct RuntimeEngineResource {
    EngineResourceDesc desc{};
    VkImage image=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    ID3D11Resource *object=nullptr;
    bool retained=false;                  // the last Release found an unexpected owner
    VkImageCreateInfo image_info{};       // pNext belongs to the engine device until its final Release (engine ABI)
    VkBufferCreateInfo buffer_info{};
    VkMemoryRequirements requirements{};
    BC250_SHARED_RESOURCE record{};       // the record of this resource as this device's engine makes it
};
// The description half of a record, and the D3D11 sharing rule for it (Bc250SharedResourceDescWhy).
const char *engine_resource_desc_why(const EngineResourceDesc &,BC250_SHARED_RESOURCE &);
// The description that a received record names. E_INVALIDARG when the record is not one this form has.
HRESULT engine_resource_record_desc(const BC250_SHARED_RESOURCE &,EngineResourceDesc &);
// Creator: the engine's create info for desc, LINEAR for a texture, the shell's image or buffer (no memory yet), and
// the whole record. *why names the refusal (a BC250_SR_WHY_* string or a short reason of this module). On failure
// nothing is left in out.
HRESULT prepare_engine_resource(RuntimeDevice &,VkDevice,const EngineResourceDispatch &,const EngineResourceDesc &,
    RuntimeEngineResource &out,const char **why);
// Opener: prepare_engine_resource for the record's description, then a comparison of the whole record with the
// received one. A difference is E_INVALIDARG with *why naming the first field group that differs; no other layout
// is tried.
HRESULT match_engine_resource(RuntimeDevice &,VkDevice,const EngineResourceDispatch &,const BC250_SHARED_RESOURCE &,
    RuntimeEngineResource &out,const char **why);
// The first field group in which two records differ, BC250_SR_WHY_OK when they are the same.
const char *engine_resource_mismatch(const BC250_SHARED_RESOURCE &mine,const BC250_SHARED_RESOURCE &received);
// Imports the mapped runtime allocation into the hosted device, binds the prepared image or buffer at offset 0 and
// wraps it in the engine. On failure the image or buffer is destroyed, unless the engine kept the object (retained).
HRESULT bind_engine_resource(RuntimeDevice &,VkDevice,const EngineResourceDispatch &,
    const VkPhysicalDeviceMemoryProperties &,const bc250_host_import &,RuntimeEngineResource &);
// Unbind views first. Waits for the engine's use, releases the object, destroys the image or buffer and frees the
// memory. Any non-S_OK keeps the resource.
HRESULT close_engine_resource(HostBridge &,VkDevice,const EngineResourceDispatch &,RuntimeEngineResource &);
// Destroys the image or buffer and frees the memory. No wait: for a resource that the engine never used.
void destroy_engine_resource(VkDevice,const EngineResourceDispatch &,RuntimeEngineResource &);
}
