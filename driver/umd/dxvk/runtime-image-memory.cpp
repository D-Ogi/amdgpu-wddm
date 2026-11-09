// SPDX-License-Identifier: MIT
#include "runtime-image-memory.h"
#include "diagnostics.h"
#include "../../contract/bc250_nv12_layout.h"
namespace bc250::umd {
namespace {
HRESULT status(VkResult result) {
    if (result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY) return E_OUTOFMEMORY;
    if (result==VK_ERROR_DEVICE_LOST) return D3DDDIERR_DEVICEREMOVED;
    return E_FAIL;
}
// Bytes of one texel of the engine's image for each composed row of the surface format table.
// DXVK stores A8_UNORM as A8_UNORM_KHR, or as R8_UNORM with a swizzle where the device lacks it.
VkDeviceSize texel_bytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_A8_UNORM_KHR: return 1;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
    }
}
}
HRESULT prepare_linear_runtime_image(RuntimeDevice &runtime,VkDevice device,const RuntimeImageDispatch &vk,
    const VkImageCreateInfo &info,RuntimeImage &out,RuntimeImageLayout &layout) {
    if (out.image || out.memory) return E_UNEXPECTED;
    layout={};
    if (!runtime.domain.entered() || !device || !vk.create || !vk.destroy || !vk.layout ||
        !vk.memory.requirements || info.sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
        !info.extent.width || !info.extent.height) return E_INVALIDARG;
    if (info.imageType!=VK_IMAGE_TYPE_2D || info.extent.depth!=1 || info.mipLevels!=1 || info.arrayLayers!=1 ||
        info.samples!=VK_SAMPLE_COUNT_1_BIT || info.tiling!=VK_IMAGE_TILING_LINEAR ||
        (info.flags&(VK_IMAGE_CREATE_DISJOINT_BIT|VK_IMAGE_CREATE_SPARSE_BINDING_BIT|
                     VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT|VK_IMAGE_CREATE_SPARSE_ALIASED_BIT))) return E_NOTIMPL;
    const bool nv12=info.format==VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    const VkDeviceSize texel=texel_bytes(info.format);
    if (!nv12 && !texel) return E_NOTIMPL;
    if (nv12 && ((info.extent.width|info.extent.height)&1u)) return E_INVALIDARG;
    RuntimeImage created{}; RuntimeImageLayout measured{};
    VkResult result=vk.create(device,&info,nullptr,&created.image);
    if (result!=VK_SUCCESS) return status(result);
    if (!created.image) return E_FAIL;
    measured.format=info.format; measured.width=info.extent.width; measured.height=info.extent.height;
    measured.planes=nv12 ? 2u : 1u;
    for (UINT i=0;i<measured.planes;++i) {
        VkImageSubresource sub{VkImageAspectFlags(nv12 ? (i ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT)
                                    : VK_IMAGE_ASPECT_COLOR_BIT),0,0};
        vk.layout(device,created.image,&sub,&measured.plane[i]);
    }
    vk.memory.requirements(device,created.image,&measured.memory);
    bool valid=measured.memory.size && measured.memory.alignment && measured.memory.memoryTypeBits &&
        !(measured.memory.alignment&(measured.memory.alignment-1));
    for (UINT i=0;i<measured.planes && valid;++i) {
        const auto &p=measured.plane[i];
        const VkDeviceSize row=VkDeviceSize(info.extent.width)*(nv12 ? 1u : texel);
        const VkDeviceSize rows=i ? info.extent.height/2u : info.extent.height;
        valid=p.size && p.rowPitch>=row && p.offset<=measured.memory.size &&
            p.size<=measured.memory.size-p.offset && row<=p.size && rows-1<=(p.size-row)/p.rowPitch;
    }
    if (nv12 && valid) {
        // Validate full padded rows, not just the last visible texel. A future
        // allocation record can retain these offsets without guessing UV.
        // Integrity only: the pitch and offsets are the device's own layout
        // answer, so no display engine's alignment or size limit applies here
        // (the scan-out limits are the KMD's, Bc250Nv12LayoutPrepare).
        const auto &y=measured.plane[0]; const auto &uv=measured.plane[1];
        BC250_NV12_STORAGE storage{info.extent.width,info.extent.height,
            UINT(y.rowPitch),UINT(uv.rowPitch),measured.memory.size,y.offset,uv.offset};
        valid=y.rowPitch<=UINT32_MAX && uv.rowPitch<=UINT32_MAX &&
            y.size>=y.rowPitch*info.extent.height && uv.size>=uv.rowPitch*(info.extent.height/2u) &&
            !(y.offset<uv.offset+uv.size && uv.offset<y.offset+y.size) &&
            Bc250Nv12StorageIntegrity(&storage);
    }
    if (!valid) { vk.destroy(device,created.image,nullptr); return E_INVALIDARG; }
    out=created; layout=measured; return S_OK;
}
HRESULT import_runtime_image_memory(RuntimeDevice &runtime,VkDevice device,VkImage image,
    const ImageMemoryDispatch &vk,const VkPhysicalDeviceMemoryProperties &properties,
    const bc250_host_import &source,VkDeviceMemory &out) {
    if (out) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !device || !image || !vk.requirements || !vk.allocate || !vk.bind || !vk.free ||
        source.sType!=BC250_HOST_IMPORT_STYPE || source.identity!=runtime.host_identity() || !source.identity ||
        !source.allocation || !source.va || !source.size || source.va>UINT64_MAX-source.size ||
        properties.memoryTypeCount>VK_MAX_MEMORY_TYPES) return E_INVALIDARG;
    VkMemoryRequirements requirements{}; vk.requirements(device,image,&requirements);
    if (!requirements.size || requirements.size>source.size || !requirements.alignment ||
        source.va%requirements.alignment) {
        surface_diagnostic("memory-requirements",E_INVALIDARG,0,0,0,requirements.size,source.size,requirements.alignment);
        return E_INVALIDARG;
    }
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
HRESULT create_linear_runtime_image(RuntimeDevice &runtime,VkDevice device,const RuntimeImageDispatch &vk,
    const VkPhysicalDeviceMemoryProperties &properties,const VkImageCreateInfo &info,
    const bc250_host_import &source,VkDeviceSize pitch,VkDeviceSize rowBytes,RuntimeImage &out) {
    if (out.image || out.memory) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !device || !vk.create || !vk.destroy || !vk.layout ||
        !vk.memory.free || !pitch || !rowBytes || rowBytes>pitch || !info.extent.width || !info.extent.height ||
        info.sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO) return E_INVALIDARG;
    if (info.imageType!=VK_IMAGE_TYPE_2D || info.extent.depth!=1 || info.mipLevels!=1 || info.arrayLayers!=1 ||
        info.samples!=VK_SAMPLE_COUNT_1_BIT || info.tiling!=VK_IMAGE_TILING_LINEAR) return E_NOTIMPL;
    // This path is for the color surfaces the compositor opens. Depth,
    // compressed and multiplanar formats need different aspect/row rules.
    // The caller's row comes from the LB7A format; the engine's texel must match it.
    const VkDeviceSize texel=texel_bytes(info.format);
    if (!texel) return E_NOTIMPL;
    if (rowBytes!=VkDeviceSize(info.extent.width)*texel) return E_INVALIDARG;
    // Division avoids overflow in (height-1)*pitch + rowBytes.
    if (rowBytes>source.size || VkDeviceSize(info.extent.height-1)>(source.size-rowBytes)/pitch) return E_INVALIDARG;
    RuntimeImage created{}; RuntimeImageLayout measured{};
    HRESULT prepared=prepare_linear_runtime_image(runtime,device,vk,info,created,measured);
    if (FAILED(prepared)) return prepared;
    const auto &layout=measured.plane[0];
    // Runtime allocation starts with pixel (0,0); there is no implicit offset
    // adjustment or row-by-row CPU fallback for a mismatching Vulkan layout.
    HRESULT hr=E_INVALIDARG;
    if (!layout.offset && layout.rowPitch==pitch && layout.size && layout.size<=source.size &&
        rowBytes<=layout.size && VkDeviceSize(info.extent.height-1)<=(layout.size-rowBytes)/pitch)
        hr=import_runtime_image_memory(runtime,device,created.image,vk.memory,properties,source,created.memory);
    if (FAILED(hr)) {
        // Measured layout against the runtime storage: offset, row pitch, size; then pitch, storage, row bytes.
        surface_diagnostic("image-layout",hr,unsigned(info.format),info.extent.width,info.extent.height,
            layout.offset,layout.rowPitch,layout.size);
        surface_diagnostic("image-storage",hr,unsigned(info.format),info.extent.width,info.extent.height,
            pitch,source.size,rowBytes);
        vk.destroy(device,created.image,nullptr); return hr;
    }
    out=created; return S_OK;
}
void destroy_runtime_image(VkDevice device,const RuntimeImageDispatch &vk,RuntimeImage &image) {
    if (image.image) vk.destroy(device,image.image,nullptr);
    if (image.memory) vk.memory.free(device,image.memory,nullptr);
    image={};
}}
