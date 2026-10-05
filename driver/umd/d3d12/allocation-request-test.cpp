// SPDX-License-Identifier: MIT
#include "allocation-request.h"
extern "C" {
#include "../../kmd/umd_blob.h"
}
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
    using native12::AllocationAccess;
    native12::AllocationRequest r;
    HANDLE owner=reinterpret_cast<HANDLE>(UINT_PTR(0x123));
    for(auto access:{AllocationAccess::GpuOnly,AllocationAccess::CpuWriteCombined,AllocationAccess::CpuCached}) {
        assert(r.prepare(65537,65536,access,owner)==S_OK);
        assert(r.args.hResource==owner && r.args.NumAllocations==1 && !r.args.hKMResource);
        assert(r.args.pAllocationInfo==&r.info && r.info.pPrivateDriverData==&r.blob);
        umd_alloc_view view{};
        assert(UmdBlobParseAlloc(r.info.pPrivateDriverData,r.info.PrivateDriverDataSize,&view)==UMD_BLOB_OK);
        assert(view.bytes==131072 && view.alignment==65536 && !view.exact_va && !view.requested_va);
        assert(view.cache_policy_valid && r.blob.va_size==view.bytes);
        assert(UmdBlobAllocCpuCached(&view)==(access==AllocationAccess::CpuCached));
        assert(view.heap==(access==AllocationAccess::GpuOnly?UMD_BLOB_HEAP_VRAM:UMD_BLOB_HEAP_GTT));
    }
    for(auto alignment:{uint64_t(0),uint64_t(2048),uint64_t(6144)})
        assert(r.prepare(1,alignment,AllocationAccess::GpuOnly)==E_INVALIDARG && !r.args.pAllocationInfo);
    assert(r.prepare(0,4096,AllocationAccess::GpuOnly)==E_INVALIDARG);
    assert(r.prepare(UINT64_MAX,4096,AllocationAccess::GpuOnly)==E_INVALIDARG);
    assert(r.prepare(1,4096,static_cast<AllocationAccess>(99))==E_INVALIDARG && !r.blob.magic);
    assert(r.prepare(1,4096,AllocationAccess::CpuCached)==S_OK && r.blob.alloc_size==4096);
    r.blob.flags=BC250_UMD_A_SPARSE;umd_alloc_view view{};
    assert(UmdBlobParseAlloc(&r.blob,sizeof(r.blob),&view)==UMD_BLOB_BAD_FLAGS);
    puts("native allocation request accepted by KMD parser; cache/rounding/refusal gates passed");
}
