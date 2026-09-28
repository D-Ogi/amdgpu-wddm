// SPDX-License-Identifier: MIT
#pragma once
#include "allocation.h"
#include "../../contract/bc250_umd_submit.h"
#include <cstdint>
#include <limits>
namespace native12 {
enum class AllocationAccess { GpuOnly, CpuWriteCombined, CpuCached };
// Non-sparse, non-shared raw memory. Texture metadata/Present surfaces require
// a separate contract. Storage is owned so callback pointers cannot dangle.
struct AllocationRequest final {
    bc250_umd_alloc_private blob{};
    D3D12DDI_ALLOCATION_INFO_0022 info{};
    D3D12DDICB_ALLOCATE_0022 args{};
    AllocationRequest()=default;
    AllocationRequest(const AllocationRequest&)=delete;
    AllocationRequest& operator=(const AllocationRequest&)=delete;
    HRESULT prepare(uint64_t bytes,uint64_t alignment,AllocationAccess access,
                    HANDLE runtimeOwner=nullptr) noexcept {
        blob={};info={};args={};
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
        return S_OK;
    }
};
}
