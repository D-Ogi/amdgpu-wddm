// SPDX-License-Identifier: MIT
#pragma once
#include "allocation.h"
#include "../../contract/bc250_umd_submit.h"
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
// Non-sparse, non-shared memory: raw (prepare), or one linear surface that a reader
// outside the engine opens by its LB7A description (prepare_surface). Storage is
// owned so callback pointers cannot dangle.
struct AllocationRequest final {
    bc250_umd_alloc_private blob{};
    Lb7aSurface surface{};
    uint64_t held{};                            // what the allocation holds: mapped and imported
    D3D12DDI_ALLOCATION_INFO_0022 info{};
    D3D12DDICB_ALLOCATE_0022 args{};
    AllocationRequest()=default;
    AllocationRequest(const AllocationRequest&)=delete;
    AllocationRequest& operator=(const AllocationRequest&)=delete;
    HRESULT prepare(uint64_t bytes,uint64_t alignment,AllocationAccess access,
                    HANDLE runtimeOwner=nullptr) noexcept {
        blob={};surface={};info={};args={};held=0;
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
    // The primary: the 32-byte LB7A v1 description alone, which the kernel driver and the
    // compositor's opener read. pitch and size are the bound image's, never chosen here.
    // The allocation is a primary of no video present source: it is composed, not scanned out.
    // primary false is a lab measurement only: the same description without the PRIMARY flag.
    HRESULT prepare_surface(uint32_t width,uint32_t height,uint32_t pitch,D3DDDIFORMAT format,
                            uint64_t size,HANDLE runtimeOwner=nullptr,bool primary=true) noexcept {
        blob={};surface={};info={};args={};held=0;
        constexpr uint32_t edge=8192;
        if(format!=D3DDDIFMT_A8R8G8B8 && format!=D3DDDIFMT_A8B8G8R8)return E_NOTIMPL;
        if(!width || width>edge || !height || height>edge || !pitch || (pitch&15))return E_INVALIDARG;
        const uint64_t width4=(uint64_t(width)+3)&~3ull,height4=(uint64_t(height)+3)&~3ull;
        if(pitch<width4*4 || !size || (size&4095) || size>0xfffff000ull || size<uint64_t(pitch)*height4)
            return E_INVALIDARG;
        surface.magic=kLb7aMagic;surface.version=1;
        surface.width=width;surface.height=height;surface.pitch=pitch;
        surface.format=static_cast<uint32_t>(format);surface.size=size;
        info.pPrivateDriverData=&surface;info.PrivateDriverDataSize=sizeof(surface);
        if(primary){
            info.Flags=D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY;
            info.VidPnSourceId=D3DDDI_ID_UNINITIALIZED;
        }
        args.hResource=runtimeOwner;args.NumAllocations=1;args.pAllocationInfo=&info;
        held=size;
        return S_OK;
    }
};
}
