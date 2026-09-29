// SPDX-License-Identifier: MIT
#pragma once
#include "allocation.h"
#include "../../contract/bc250_umd_submit.h"
#include "../../contract/amdgpu_wddm_surface_format.h"
#include <cstddef>
#include <cstdint>
#include <limits>
namespace native12 {
enum class AllocationAccess { GpuOnly, CpuWriteCombined, CpuCached };
// The LB7A v1 allocation description on the wire, as the kernel driver defines it
// (driver/kmd/gdi_private.h, BC250_WDDM_ALLOCATION_PRIVATE): 32 bytes, little endian.
struct Lb7aSurface {
    uint32_t magic,version,width,height,pitch,format; // format: D3DDDIFORMAT
    uint64_t size;
};
inline constexpr uint32_t kLb7aMagic=0x4137424Cu;
static_assert(sizeof(Lb7aSurface)==32 && offsetof(Lb7aSurface,pitch)==16 && offsetof(Lb7aSurface,size)==24);
// The E26R resource record, as the kernel driver defines it (driver/kmd/surface_resource_private.h):
// v1 is the first 12 bytes (magic, version, shared), v2 adds the CPU access intent word (16 bytes:
// PRIMARY=1, CPU_READ=2). The runtime's allocation call accepts the primary with v1 and shared 1.
struct E26rResource { uint32_t magic,version,shared,access; };
inline constexpr uint32_t kE26rMagic=0x52363245u;
inline constexpr uint32_t kE26rV1Bytes=12,kE26rCpuRead=2;
static_assert(sizeof(E26rResource)==16 && offsetof(E26rResource,access)==kE26rV1Bytes);
// The surface format table's numbers are the SDK's and the WDK's.
static_assert(AMDGPU_WDDM_DXGI_R16G16B16A16_FLOAT==DXGI_FORMAT_R16G16B16A16_FLOAT &&
              AMDGPU_WDDM_DXGI_R10G10B10A2_UNORM==DXGI_FORMAT_R10G10B10A2_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM==DXGI_FORMAT_R8G8B8A8_UNORM &&
              AMDGPU_WDDM_DXGI_R8G8B8A8_UNORM_SRGB==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM==DXGI_FORMAT_B8G8R8A8_UNORM &&
              AMDGPU_WDDM_DXGI_B8G8R8A8_UNORM_SRGB==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
static_assert(AMDGPU_WDDM_D3DDDI_A8R8G8B8==D3DDDIFMT_A8R8G8B8 && AMDGPU_WDDM_D3DDDI_X8R8G8B8==D3DDDIFMT_X8R8G8B8 &&
              AMDGPU_WDDM_D3DDDI_A2B10G10R10==D3DDDIFMT_A2B10G10R10 && AMDGPU_WDDM_D3DDDI_A8B8G8R8==D3DDDIFMT_A8B8G8R8 &&
              AMDGPU_WDDM_D3DDDI_A16B16G16R16F==D3DDDIFMT_A16B16G16R16F);
// Non-sparse, non-shared memory: raw (prepare), or one linear surface that a reader
// outside the engine opens by its LB7A description (prepare_surface). Storage is
// owned so callback pointers cannot dangle.
struct AllocationRequest final {
    bc250_umd_alloc_private blob{};
    Lb7aSurface surface{};
    E26rResource resource{};
    uint64_t held{};                            // what the allocation holds: mapped and imported
    D3D12DDI_ALLOCATION_INFO_0022 info{};
    D3D12DDICB_ALLOCATE_0022 args{};
    AllocationRequest()=default;
    AllocationRequest(const AllocationRequest&)=delete;
    AllocationRequest& operator=(const AllocationRequest&)=delete;
    HRESULT prepare(uint64_t bytes,uint64_t alignment,AllocationAccess access,
                    HANDLE runtimeOwner=nullptr) noexcept {
        blob={};surface={};resource={};info={};args={};held=0;
        constexpr uint64_t page=4096;
        // Do not silently shrink an engine requirement or overflow rounding.
        if(!bytes || alignment<page || (alignment&(alignment-1)) ||
           bytes>std::numeric_limits<uint64_t>::max()-(alignment-1)) return E_INVALIDARG;
        uint64_t rounded=(bytes+alignment-1)&~(alignment-1);
        if(rounded>0xfffffffffffff000ull) return E_INVALIDARG;
        uint32_t heap=0;uint64_t flags=0;
        switch(access){
        case AllocationAccess::GpuOnly:
            heap=AMDGPU_GEM_DOMAIN_VRAM;flags=AMDGPU_GEM_CREATE_NO_CPU_ACCESS;break;
        case AllocationAccess::CpuWriteCombined:
            heap=AMDGPU_GEM_DOMAIN_GTT;
            flags=AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED|AMDGPU_GEM_CREATE_CPU_GTT_USWC;break;
        case AllocationAccess::CpuCached:
            heap=AMDGPU_GEM_DOMAIN_GTT;flags=AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;break;
        default:return E_INVALIDARG;
        }
        blob.magic=BC250_UMD_ALLOC_MAGIC;
        blob.version=BC250_UMD_ALLOC_VERSION_CACHE_POLICY;blob.size=sizeof(blob);
        blob.alloc_size=rounded;blob.phys_alignment=alignment;
        blob.preferred_heap=heap;blob.gem_flags=flags;blob.va_size=rounded;
        // VA is mapped separately through the runtime; no exact-VA promise here.
        info.pPrivateDriverData=&blob;info.PrivateDriverDataSize=sizeof(blob);
        args.hResource=runtimeOwner;args.NumAllocations=1;args.pAllocationInfo=&info;
        held=rounded;
        return S_OK;
    }
    // The primary: the 32-byte LB7A v1 description, which the kernel driver and the compositor's
    // opener read, under the 12-byte E26R v1 resource record. pitch and size are the bound image's,
    // never chosen here. The allocation is a primary of no video present source: it is composed,
    // not scanned out, so its format is one the surface format table enables for composition.
    // cpuRead (lab experiment present-cached): the 16-byte v2 record with CPU_READ and without the
    // PRIMARY intent bit, so the kernel driver gives the shared aperture backing a cached CPU mapping
    // (M470). The CPU compositor samples this surface on every composition; through the default
    // write-combined mapping each read is uncached (104: dwm ~33% of the machine in llvmpipe shader
    // code for a 1280x720 window). The intent bit is left out on purpose: the documented rule against
    // Cached primaries is about scanout, and this primary has no video present source, while the
    // deployed compositor UMD offers no direct flip that could scan it out. GPU writes to the
    // aperture are snooped (CacheCoherent), so a CPU reader stays coherent.
    HRESULT prepare_surface(uint32_t width,uint32_t height,uint32_t pitch,D3DDDIFORMAT format,
                            uint64_t size,HANDLE runtimeOwner=nullptr,bool cpuRead=false) noexcept {
        blob={};surface={};resource={};info={};args={};held=0;
        constexpr uint32_t edge=8192;
        const auto* row=amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_d3dddi(uint32_t(format)),
                                                  AMDGPU_WDDM_SURFACE_COMPOSED);
        if(!row)return E_NOTIMPL;
        if(!width || width>edge || !height || height>edge || !pitch || (pitch&15))return E_INVALIDARG;
        const uint64_t width4=(uint64_t(width)+3)&~3ull,height4=(uint64_t(height)+3)&~3ull;
        if(pitch<width4*row->bytes_per_pixel || !size || (size&4095) || size>0xfffff000ull || size<uint64_t(pitch)*height4)
            return E_INVALIDARG;
        surface.magic=kLb7aMagic;surface.version=1;
        surface.width=width;surface.height=height;surface.pitch=pitch;
        surface.format=static_cast<uint32_t>(format);surface.size=size;
        info.pPrivateDriverData=&surface;info.PrivateDriverDataSize=sizeof(surface);
        info.Flags=D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
        info.VidPnSourceId=D3DDDI_ID_UNINITIALIZED;
        resource.magic=kE26rMagic;resource.version=cpuRead?2:1;resource.shared=1;
        resource.access=cpuRead?kE26rCpuRead:0;
        args.pPrivateDriverData=&resource;args.PrivateDriverDataSize=cpuRead?sizeof(resource):kE26rV1Bytes;
        args.hResource=runtimeOwner;args.NumAllocations=1;args.pAllocationInfo=&info;
        held=size;
        return S_OK;
    }
};
}
