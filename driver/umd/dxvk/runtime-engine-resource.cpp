// SPDX-License-Identifier: MIT
#include "runtime-engine-resource.h"
#include "diagnostics.h"
#include <cstring>
#include <initializer_list>
namespace bc250::umd {
static_assert(BC250_SR_USAGE_DEFAULT==D3D11_USAGE_DEFAULT);
static_assert(BC250_SR_BIND_VERTEX_BUFFER==D3D11_BIND_VERTEX_BUFFER && BC250_SR_BIND_INDEX_BUFFER==D3D11_BIND_INDEX_BUFFER &&
    BC250_SR_BIND_CONSTANT_BUFFER==D3D11_BIND_CONSTANT_BUFFER && BC250_SR_BIND_SHADER_RESOURCE==D3D11_BIND_SHADER_RESOURCE &&
    BC250_SR_BIND_STREAM_OUTPUT==D3D11_BIND_STREAM_OUTPUT && BC250_SR_BIND_RENDER_TARGET==D3D11_BIND_RENDER_TARGET &&
    BC250_SR_BIND_DEPTH_STENCIL==D3D11_BIND_DEPTH_STENCIL && BC250_SR_BIND_UNORDERED_ACCESS==D3D11_BIND_UNORDERED_ACCESS &&
    BC250_SR_BIND_DECODER==D3D11_BIND_DECODER && BC250_SR_BIND_VIDEO_ENCODER==D3D11_BIND_VIDEO_ENCODER);
static_assert(BC250_SR_MISC_GENERATE_MIPS==D3D11_RESOURCE_MISC_GENERATE_MIPS &&
    BC250_SR_MISC_DRAWINDIRECT_ARGS==D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS &&
    BC250_SR_MISC_BUFFER_ALLOW_RAW==D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS &&
    BC250_SR_MISC_BUFFER_STRUCTURED==D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
static_assert(BC250_SR_VK_IMAGE_TYPE_2D==VK_IMAGE_TYPE_2D && BC250_SR_VK_TILING_LINEAR==VK_IMAGE_TILING_LINEAR &&
    BC250_SR_VK_SAMPLE_COUNT_1==VK_SAMPLE_COUNT_1_BIT && BC250_SR_VK_SHARING_EXCLUSIVE==VK_SHARING_MODE_EXCLUSIVE &&
    BC250_SR_VK_LAYOUT_UNDEFINED==VK_IMAGE_LAYOUT_UNDEFINED);
namespace {
// Module reasons, beside the record's own BC250_SR_WHY_* strings.
constexpr const char *kWhyEngine="engine";
constexpr const char *kWhyNoBufferAbi="engine without shared buffers";
constexpr const char *kWhyLinear="linear image not supported";
constexpr const char *kWhyPlanar="planar format";
constexpr const char *kWhyAlignment="memory alignment";
constexpr const char *kWhyVulkan="vulkan";
HRESULT status(VkResult result) {
    if (result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY) return E_OUTOFMEMORY;
    if (result==VK_ERROR_DEVICE_LOST) return D3DDDIERR_DEVICEREMOVED;
    return E_FAIL;
}
// Formats with more than one plane or a chroma subsampled layout: their LINEAR subresources need plane aspects and
// another hash. D3D11 shares NV12 in the LINEAR SURFACE form; the rest is refused here.
bool planar_format(VkFormat format) {
    const auto f=unsigned(format);
    return (f>=unsigned(VK_FORMAT_G8B8G8R8_422_UNORM) && f<=unsigned(VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM)) ||
        (f>=unsigned(VK_FORMAT_G8_B8R8_2PLANE_444_UNORM) && f<=unsigned(VK_FORMAT_G16_B16R16_2PLANE_444_UNORM));
}
bool vulkan_ready(const EngineResourceVulkan &vk,bool buffer) {
    if (buffer) return vk.create_buffer && vk.destroy_buffer && vk.buffer_requirements && vk.bind_buffer && vk.image &&
        vk.image->memory.allocate && vk.image->memory.free;
    return vk.image && vk.image->create && vk.image->destroy && vk.image->layout && vk.image->memory.requirements &&
        vk.image->memory.allocate && vk.image->memory.bind && vk.image->memory.free && vk.image_format && vk.physical;
}
// FNV-1a 64 over the layout of every subresource, layer-major then mip, as bc250_shared_resource.h says.
UINT64 layout_hash(VkDevice device,const RuntimeImageDispatch &vk,VkImage image,UINT mips,UINT layers) {
    UINT64 hash=BC250_SR_FNV_BASIS;
    for (UINT layer=0;layer<layers;++layer) {
        for (UINT mip=0;mip<mips;++mip) {
            const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,mip,layer};
            VkSubresourceLayout l{};
            vk.layout(device,image,&sub,&l);
            for (const UINT64 v:{UINT64(l.offset),UINT64(l.size),UINT64(l.rowPitch),UINT64(l.arrayPitch),UINT64(l.depthPitch)})
                hash=Bc250SharedResourceHash(hash,v);
        }
    }
    return hash;
}
void release_object(const EngineResourceDispatch &d,RuntimeEngineResource &r) {
    if (!r.object) return;
    const ULONG refs=d.engine.release(r.object); r.object=nullptr;
    if (refs) r.retained=true;
}
}
EngineResourceEngine engine_resource_engine(IBc250DxvkDevice &engine) {
    EngineResourceEngine e{};
    e.engine=&engine;
    e.describe_image=[](void *p,const D3D11_TEXTURE2D_DESC1 *d,VkImageCreateInfo *i) {
        return static_cast<IBc250DxvkDevice *>(p)->GetImageCreateInfo(d,i);
    };
    e.wrap_image=[](void *p,const D3D11_TEXTURE2D_DESC1 *d,const VkImageCreateInfo *info,VkImage i,ID3D11Texture2D **t) {
        IBc250DxvkDevice2 *extended=nullptr;
        HRESULT hr=static_cast<IBc250DxvkDevice *>(p)->QueryInterface(__uuidof(IBc250DxvkDevice2),reinterpret_cast<void **>(&extended));
        if (SUCCEEDED(hr)) hr=extended ? extended->CreateTexture2DFromImage2(d,info,i,t) : E_NOINTERFACE;
        if (extended) extended->Release();
        return hr;
    };
#if BC250_DXVK_ENGINE_ABI_MINOR>=9
    e.describe_buffer=[](void *p,const D3D11_BUFFER_DESC *d,VkBufferCreateInfo *i) {
        IBc250DxvkDevice7 *extended=nullptr;
        HRESULT hr=static_cast<IBc250DxvkDevice *>(p)->QueryInterface(__uuidof(IBc250DxvkDevice7),reinterpret_cast<void **>(&extended));
        if (SUCCEEDED(hr)) hr=extended ? extended->GetBufferCreateInfo(d,i) : E_NOINTERFACE;
        if (extended) extended->Release();
        return hr;
    };
    e.wrap_buffer=[](void *p,const D3D11_BUFFER_DESC *d,const VkBufferCreateInfo *info,VkBuffer b,ID3D11Buffer **out) {
        IBc250DxvkDevice7 *extended=nullptr;
        HRESULT hr=static_cast<IBc250DxvkDevice *>(p)->QueryInterface(__uuidof(IBc250DxvkDevice7),reinterpret_cast<void **>(&extended));
        if (SUCCEEDED(hr)) hr=extended ? extended->CreateBufferFromVkBuffer(d,info,b,out) : E_NOINTERFACE;
        if (extended) extended->Release();
        return hr;
    };
#endif
    e.wait=[](void *p,ID3D11Resource *r) { return static_cast<IBc250DxvkDevice *>(p)->WaitForResourceIdle(r); };
    e.release=[](ID3D11Resource *r) { return r->Release(); };
    return e;
}
const char *engine_resource_desc_why(const EngineResourceDesc &desc,BC250_SHARED_RESOURCE &r) {
    r={};
    r.Magic=BC250_SHARED_RESOURCE_MAGIC; r.Version=BC250_SHARED_RESOURCE_VERSION; r.Bytes=BC250_SHARED_RESOURCE_BYTES;
    if (desc.buffer) {
        const auto &b=desc.buffer_desc;
        r.Dimension=BC250_SHARED_RESOURCE_BUFFER;
        r.Width=b.ByteWidth; r.Height=1; r.MipLevels=1; r.ArraySize=1; r.SampleCount=1;
        r.Usage=UINT(b.Usage); r.BindFlags=b.BindFlags; r.CpuAccessFlags=b.CPUAccessFlags; r.MiscFlags=b.MiscFlags;
        r.StructureByteStride=b.StructureByteStride;
    } else {
        const auto &t=desc.texture;
        r.Dimension=BC250_SHARED_RESOURCE_TEXTURE2D;
        r.Width=t.Width; r.Height=t.Height; r.MipLevels=t.MipLevels; r.ArraySize=t.ArraySize; r.Format=UINT(t.Format);
        r.SampleCount=t.SampleDesc.Count; r.SampleQuality=t.SampleDesc.Quality; r.Usage=UINT(t.Usage);
        r.BindFlags=t.BindFlags; r.CpuAccessFlags=t.CPUAccessFlags; r.MiscFlags=t.MiscFlags;
        if (t.TextureLayout!=D3D11_TEXTURE_LAYOUT_UNDEFINED) return BC250_SR_WHY_MISC;
    }
    return Bc250SharedResourceDescWhy(&r);
}
HRESULT engine_resource_record_desc(const BC250_SHARED_RESOURCE &r,EngineResourceDesc &out) {
    if (!Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&r))) return E_INVALIDARG;
    EngineResourceDesc d{};
    if (r.Dimension==BC250_SHARED_RESOURCE_BUFFER) {
        d.buffer=true;
        d.buffer_desc={r.Width,D3D11_USAGE(r.Usage),r.BindFlags,r.CpuAccessFlags,r.MiscFlags,r.StructureByteStride};
    } else {
        d.texture={r.Width,r.Height,r.MipLevels,r.ArraySize,DXGI_FORMAT(r.Format),{r.SampleCount,r.SampleQuality},
            D3D11_USAGE(r.Usage),r.BindFlags,r.CpuAccessFlags,r.MiscFlags,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    }
    out=d; return S_OK;
}
HRESULT prepare_engine_resource(RuntimeDevice &runtime,VkDevice device,const EngineResourceDispatch &d,
    const EngineResourceDesc &desc,RuntimeEngineResource &out,const char **why) {
    const char *ignored=nullptr; if (!why) why=&ignored;
    *why=BC250_SR_WHY_ARGUMENTS;
    if (out.image || out.buffer || out.memory || out.object || out.retained) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !device || !d.engine.engine || !d.engine.wait || !d.engine.release ||
        !d.vk.identity.valid) return E_INVALIDARG;
    RuntimeEngineResource r{};
    r.desc=desc;
    *why=engine_resource_desc_why(desc,r.record);
    if (!Bc250SharedResourceWhyOk(*why)) return E_NOTIMPL;
    auto &rec=r.record;
    std::memcpy(rec.DriverUuid,d.vk.identity.driver,16); std::memcpy(rec.DeviceUuid,d.vk.identity.device,16);
    if (desc.buffer) {
        if (!d.engine.describe_buffer || !d.engine.wrap_buffer) { *why=kWhyNoBufferAbi; return E_NOTIMPL; }
        if (!vulkan_ready(d.vk,true)) { *why=kWhyVulkan; return E_NOTIMPL; }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        HRESULT hr=d.engine.describe_buffer(d.engine.engine,&desc.buffer_desc,&info);
        if (FAILED(hr)) { *why=kWhyEngine; return hr; }
        if (info.sType!=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || info.pNext || info.sharingMode!=VK_SHARING_MODE_EXCLUSIVE ||
            info.size<desc.buffer_desc.ByteWidth || info.flags || !info.usage) { *why=BC250_SR_WHY_CREATE_INFO; return E_NOTIMPL; }
        VkResult result=d.vk.create_buffer(device,&info,nullptr,&r.buffer);
        if (result!=VK_SUCCESS) { *why=kWhyVulkan; return status(result); }
        if (!r.buffer) { *why=kWhyVulkan; return E_FAIL; }
        d.vk.buffer_requirements(device,r.buffer,&r.requirements);
        r.buffer_info=info;
        rec.CreateFlags=info.flags; rec.CreateUsage=info.usage; rec.SharingMode=UINT(info.sharingMode);
        rec.BufferSize=info.size;
    } else {
        if (!vulkan_ready(d.vk,false) || !d.engine.describe_image || !d.engine.wrap_image) { *why=kWhyVulkan; return E_NOTIMPL; }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        HRESULT hr=d.engine.describe_image(d.engine.engine,&desc.texture,&info);
        if (FAILED(hr)) { *why=kWhyEngine; return hr; }
        // The one structure the engine chains is its view format list (engine ABI, GetImageCreateInfo).
        const VkImageFormatListCreateInfo *list=nullptr;
        for (auto *s=static_cast<const VkBaseInStructure *>(info.pNext);s;s=s->pNext) {
            if (s->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO || list) { *why=BC250_SR_WHY_CREATE_INFO; return E_NOTIMPL; }
            list=reinterpret_cast<const VkImageFormatListCreateInfo *>(s);
        }
        if (list && list->viewFormatCount>BC250_SHARED_RESOURCE_VIEW_FORMATS) { *why=BC250_SR_WHY_VIEW_FORMATS; return E_NOTIMPL; }
        if (planar_format(info.format)) { *why=kWhyPlanar; return E_NOTIMPL; }
        if (info.imageType!=VK_IMAGE_TYPE_2D || info.extent.width!=desc.texture.Width ||
            info.extent.height!=desc.texture.Height || info.extent.depth!=1 || info.mipLevels!=desc.texture.MipLevels ||
            info.arrayLayers!=desc.texture.ArraySize || info.samples!=VK_SAMPLE_COUNT_1_BIT ||
            info.sharingMode!=VK_SHARING_MODE_EXCLUSIVE || info.initialLayout!=VK_IMAGE_LAYOUT_UNDEFINED ||
            (info.flags&(VK_IMAGE_CREATE_DISJOINT_BIT|VK_IMAGE_CREATE_SPARSE_BINDING_BIT|VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT|
                         VK_IMAGE_CREATE_SPARSE_ALIASED_BIT|VK_IMAGE_CREATE_ALIAS_BIT))) { *why=BC250_SR_WHY_CREATE_INFO; return E_NOTIMPL; }
        // The engine ABI lets the shell select the tiling. LINEAR: no metadata, the same texels for every image of the
        // same create info on the same memory (bc250_shared_resource.h).
        info.tiling=VK_IMAGE_TILING_LINEAR;
        VkImageFormatProperties props{};
        VkResult result=d.vk.image_format(d.vk.physical,info.format,info.imageType,info.tiling,info.usage,info.flags,&props);
        if (result!=VK_SUCCESS || props.maxMipLevels<info.mipLevels || props.maxArrayLayers<info.arrayLayers ||
            props.maxExtent.width<info.extent.width || props.maxExtent.height<info.extent.height ||
            !(props.sampleCounts&VK_SAMPLE_COUNT_1_BIT)) {
            *why=kWhyLinear;
            surface_diagnostic("engine-layout-linear",E_NOTIMPL,unsigned(info.format),info.extent.width,info.extent.height,
                info.mipLevels,info.arrayLayers,info.usage);
            return result==VK_SUCCESS || result==VK_ERROR_FORMAT_NOT_SUPPORTED ? E_NOTIMPL : status(result);
        }
        result=d.vk.image->create(device,&info,nullptr,&r.image);
        if (result!=VK_SUCCESS) { *why=kWhyVulkan; return status(result); }
        if (!r.image) { *why=kWhyVulkan; return E_FAIL; }
        d.vk.image->memory.requirements(device,r.image,&r.requirements);
        r.image_info=info;
        rec.CreateFlags=info.flags; rec.CreateUsage=info.usage; rec.SharingMode=UINT(info.sharingMode);
        rec.ImageType=UINT(info.imageType); rec.VkFormat=UINT(info.format);
        rec.ExtentWidth=info.extent.width; rec.ExtentHeight=info.extent.height; rec.ExtentDepth=info.extent.depth;
        rec.VkMipLevels=info.mipLevels; rec.VkArrayLayers=info.arrayLayers; rec.VkSamples=UINT(info.samples);
        rec.Tiling=UINT(info.tiling); rec.InitialLayout=UINT(info.initialLayout);
        if (list) {
            rec.ViewFormatCount=list->viewFormatCount;
            for (UINT i=0;i<list->viewFormatCount;++i) rec.ViewFormats[i]=UINT(list->pViewFormats[i]);
        }
        rec.LayoutHash=layout_hash(device,*d.vk.image,r.image,info.mipLevels,info.arrayLayers);
    }
    rec.MemorySize=r.requirements.size; rec.MemoryAlignment=r.requirements.alignment;
    rec.MemoryTypeBits=r.requirements.memoryTypeBits;
    if (r.requirements.alignment>BC250_SHARED_RESOURCE_MAX_ALIGNMENT) {
        *why=kWhyAlignment;
        surface_diagnostic("engine-layout-alignment",E_NOTIMPL,rec.VkFormat,rec.Width,rec.Height,
            r.requirements.size,r.requirements.alignment,BC250_SHARED_RESOURCE_MAX_ALIGNMENT);
        destroy_engine_resource(device,d,r); return E_NOTIMPL;
    }
    *why=Bc250SharedResourceRecordWhy(&rec);
    if (!Bc250SharedResourceWhyOk(*why)) { destroy_engine_resource(device,d,r); return E_NOTIMPL; }
    out=r; return S_OK;
}
const char *engine_resource_mismatch(const BC250_SHARED_RESOURCE &a,const BC250_SHARED_RESOURCE &b) {
    if (a.Magic!=b.Magic || a.Version!=b.Version || a.Bytes!=b.Bytes || a.Dimension!=b.Dimension) return BC250_SR_WHY_DIMENSION;
    if (a.Width!=b.Width || a.Height!=b.Height || a.MipLevels!=b.MipLevels || a.ArraySize!=b.ArraySize ||
        a.Format!=b.Format || a.SampleCount!=b.SampleCount || a.SampleQuality!=b.SampleQuality || a.Usage!=b.Usage ||
        a.BindFlags!=b.BindFlags || a.CpuAccessFlags!=b.CpuAccessFlags || a.MiscFlags!=b.MiscFlags ||
        a.StructureByteStride!=b.StructureByteStride) return BC250_SR_WHY_DESCRIPTION;
    if (std::memcmp(a.DriverUuid,b.DriverUuid,16) || std::memcmp(a.DeviceUuid,b.DeviceUuid,16)) return BC250_SR_WHY_IDENTITY;
    if (a.CreateFlags!=b.CreateFlags || a.CreateUsage!=b.CreateUsage || a.SharingMode!=b.SharingMode ||
        a.ImageType!=b.ImageType || a.VkFormat!=b.VkFormat || a.ExtentWidth!=b.ExtentWidth ||
        a.ExtentHeight!=b.ExtentHeight || a.ExtentDepth!=b.ExtentDepth || a.VkMipLevels!=b.VkMipLevels ||
        a.VkArrayLayers!=b.VkArrayLayers || a.VkSamples!=b.VkSamples || a.Tiling!=b.Tiling ||
        a.InitialLayout!=b.InitialLayout || a.BufferSize!=b.BufferSize) return BC250_SR_WHY_CREATE_INFO;
    if (a.ViewFormatCount!=b.ViewFormatCount || std::memcmp(a.ViewFormats,b.ViewFormats,sizeof(a.ViewFormats)))
        return BC250_SR_WHY_VIEW_FORMATS;
    if (a.MemorySize!=b.MemorySize || a.MemoryAlignment!=b.MemoryAlignment || a.MemoryTypeBits!=b.MemoryTypeBits)
        return BC250_SR_WHY_MEMORY;
    if (a.LayoutHash!=b.LayoutHash) return BC250_SR_WHY_LAYOUT;
    if (a.Reserved0!=b.Reserved0) return BC250_SR_WHY_RESERVED;
    return BC250_SR_WHY_OK;
}
HRESULT match_engine_resource(RuntimeDevice &runtime,VkDevice device,const EngineResourceDispatch &d,
    const BC250_SHARED_RESOURCE &received,RuntimeEngineResource &out,const char **why) {
    const char *ignored=nullptr; if (!why) why=&ignored;
    *why=Bc250SharedResourceRecordWhy(&received);
    if (!Bc250SharedResourceWhyOk(*why)) return E_INVALIDARG;
    EngineResourceDesc desc{};
    HRESULT hr=engine_resource_record_desc(received,desc);
    if (FAILED(hr)) { *why=BC250_SR_WHY_ARGUMENTS; return hr; }
    RuntimeEngineResource r{};
    hr=prepare_engine_resource(runtime,device,d,desc,r,why);
    if (FAILED(hr)) return hr;
    // The whole recorded create info, memory and layout, never a fallback to another layout (BD-373).
    *why=engine_resource_mismatch(r.record,received);
    if (!Bc250SharedResourceWhyOk(*why)) { destroy_engine_resource(device,d,r); return E_INVALIDARG; }
    out=r; return S_OK;
}
HRESULT bind_engine_resource(RuntimeDevice &runtime,VkDevice device,const EngineResourceDispatch &d,
    const VkPhysicalDeviceMemoryProperties &properties,const bc250_host_import &source,RuntimeEngineResource &r) {
    if (r.memory || r.object || r.retained || (!r.image==!r.buffer)) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !device || !vulkan_ready(d.vk,r.buffer!=VK_NULL_HANDLE) ||
        source.sType!=BC250_HOST_IMPORT_STYPE || source.identity!=runtime.host_identity() || !source.identity ||
        !source.allocation || !source.va || !source.size || source.va>UINT64_MAX-source.size ||
        properties.memoryTypeCount>VK_MAX_MEMORY_TYPES) return E_INVALIDARG;
    const auto &req=r.requirements;
    if (!req.size || req.size>source.size || !req.alignment || source.va%req.alignment) {
        surface_diagnostic("engine-layout-memory",E_INVALIDARG,r.record.VkFormat,r.record.Width,r.record.Height,
            req.size,source.size,source.va%(req.alignment ? req.alignment : 1));
        destroy_engine_resource(device,d,r); return E_INVALIDARG;
    }
    UINT type=properties.memoryTypeCount;
    for (UINT i=0;i<properties.memoryTypeCount;++i) {
        if ((req.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type=i; break; }
    }
    if (type==properties.memoryTypeCount) { destroy_engine_resource(device,d,r); return E_NOTIMPL; }
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image=r.image; dedicated.buffer=r.buffer;
    bc250_host_import imported=source; imported.pNext=&dedicated;
    // A buffer of the engine has SHADER_DEVICE_ADDRESS usage (engine ABI, GetBufferCreateInfo).
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; flags.pNext=&imported;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext=r.buffer ? static_cast<const void *>(&flags) : &imported;
    allocation.allocationSize=req.size; allocation.memoryTypeIndex=type;
    VkResult result=d.vk.image->memory.allocate(device,&allocation,nullptr,&r.memory);
    if (result!=VK_SUCCESS || !r.memory) {
        r.memory=VK_NULL_HANDLE; destroy_engine_resource(device,d,r);
        return result!=VK_SUCCESS ? status(result) : E_FAIL;
    }
    result=r.buffer ? d.vk.bind_buffer(device,r.buffer,r.memory,0) : d.vk.image->memory.bind(device,r.image,r.memory,0);
    if (result!=VK_SUCCESS) { destroy_engine_resource(device,d,r); return status(result); }
    HRESULT hr=E_FAIL;
    if (r.buffer) {
        ID3D11Buffer *object=nullptr;
        hr=d.engine.wrap_buffer(d.engine.engine,&r.desc.buffer_desc,&r.buffer_info,r.buffer,&object);
        r.object=object;
    } else {
        ID3D11Texture2D *object=nullptr;
        hr=d.engine.wrap_image(d.engine.engine,&r.desc.texture,&r.image_info,r.image,&object);
        r.object=object;
    }
    if (FAILED(hr) || !r.object) {
        release_object(d,r);
        if (r.retained) return FAILED(hr) ? hr : E_UNEXPECTED;
        destroy_engine_resource(device,d,r);
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}
HRESULT close_engine_resource(HostBridge &bridge,VkDevice device,const EngineResourceDispatch &d,RuntimeEngineResource &r) {
    if (!bridge.device || !bridge.device->domain.entered() || !d.engine.wait || !d.engine.release) return E_INVALIDARG;
    if (r.retained) return E_UNEXPECTED;
    // No Present wait: a resource of this form is never a present source (convert routes primaries, present buffers
    // and displayable surfaces to the LINEAR SURFACE form).
    if (r.object) {
        // BD-253, as close_runtime_texture: the loss of a device whose work ended is not a failure of the close.
        const auto ended=[&](HRESULT s) { return device_lost_status(s) && settle_device_work(bridge,0); };
        HRESULT hr=d.engine.wait(d.engine.engine,r.object);
        if (FAILED(hr) && !ended(hr)) return hr;
        release_object(d,r);
        if (r.retained) return E_UNEXPECTED;
    }
    destroy_engine_resource(device,d,r);
    return S_OK;
}
void destroy_engine_resource(VkDevice device,const EngineResourceDispatch &d,RuntimeEngineResource &r) {
    if (r.image && d.vk.image && d.vk.image->destroy) d.vk.image->destroy(device,r.image,nullptr);
    if (r.buffer && d.vk.destroy_buffer) d.vk.destroy_buffer(device,r.buffer,nullptr);
    if (r.memory && d.vk.image && d.vk.image->memory.free) d.vk.image->memory.free(device,r.memory,nullptr);
    r.image=VK_NULL_HANDLE; r.buffer=VK_NULL_HANDLE; r.memory=VK_NULL_HANDLE;
}
}
