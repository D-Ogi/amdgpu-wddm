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

    // BD-075: the shared surface's two records. The pair a create publishes must be the pair an opener decodes,
    // so the test decodes what it wrote and compares every field, and the kernel driver's own parser must admit it.
    {
        const uint32_t width=256,height=254,pitch=1024;
        const uint64_t size=uint64_t(pitch)*256;        // the rows a reader of four-row blocks takes
        const uint32_t bind=BC250_SHARED_BIND_SHADER_RESOURCE|BC250_SHARED_BIND_RENDER_TARGET;
        assert(r.prepare_shared_surface(width,height,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind,owner)==S_OK);
        assert(r.args.hResource==owner && r.args.NumAllocations==1 && !r.args.hKMResource);
        assert(r.args.pAllocationInfo==&r.info && r.info.pPrivateDriverData==&r.surface);
        assert(r.info.PrivateDriverDataSize==32 && r.args.PrivateDriverDataSize==64);
        assert(r.args.pPrivateDriverData==&r.texture);
        // Not a primary: no information flag and no video present source.
        assert(r.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_NONE && r.info.VidPnSourceId==0);
        assert(r.held==size);
        assert(r.surface.magic==native12::kLb7aMagic && r.surface.version==1 && r.surface.width==width &&
               r.surface.height==height && r.surface.pitch==pitch && r.surface.size==size &&
               r.surface.format==D3DDDIFMT_A8R8G8B8);
        assert(r.texture.Magic==BC250_SURFACE_RESOURCE_MAGIC && r.texture.Version==3 && r.texture.Shared==1 &&
               r.texture.Access==0 && r.texture.Width==width && r.texture.Height==height && r.texture.MipLevels==1 &&
               r.texture.ArraySize==1 && r.texture.Format==DXGI_FORMAT_B8G8R8A8_UNORM && r.texture.SampleCount==1 &&
               !r.texture.SampleQuality && r.texture.Usage==0 && r.texture.BindFlags==bind &&
               !r.texture.CpuAccessFlags && !r.texture.MiscFlags && r.texture.TextureLayout==0);
        int shared_cpu=-1,cached_cpu=-1;
        assert(Bc250SurfaceResourcePolicy(&r.texture,sizeof(r.texture),&shared_cpu,&cached_cpu)==1);
        assert(shared_cpu==1 && cached_cpu==0);         // shared, and no cached CPU mapping is asked for
        BC250_SHARED_SURFACE back{};
        assert(Bc250SharedSurfaceDecode(&r.texture,sizeof(r.texture),&r.surface,sizeof(r.surface),&back)==
               BC250_SHARED_SURFACE_OK);
        assert(back.Width==width && back.Height==height && back.Pitch==pitch && back.Size==size &&
               back.DxgiFormat==DXGI_FORMAT_B8G8R8A8_UNORM && back.D3dDdiFormat==D3DDDIFMT_A8R8G8B8 &&
               back.BindFlags==bind && back.BytesPerPixel==4 && back.Shared==1 && !back.Access && !back.MiscFlags);
        // The other composed rows, with their own pixel size and D3DDDIFORMAT.
        struct Row { DXGI_FORMAT dxgi; uint32_t d3dddi,bpp; };
        for(const Row row:{Row{DXGI_FORMAT_R8G8B8A8_UNORM,D3DDDIFMT_A8B8G8R8,4},
                           Row{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,D3DDDIFMT_A8R8G8B8,4},
                           Row{DXGI_FORMAT_R10G10B10A2_UNORM,D3DDDIFMT_A2B10G10R10,4},
                           Row{DXGI_FORMAT_R16G16B16A16_FLOAT,D3DDDIFMT_A16B16G16R16F,8},
                           Row{DXGI_FORMAT_A8_UNORM,D3DDDIFMT_A8,1}}) {
            const uint32_t row_pitch=((64*row.bpp+255)&~255u);
            assert(r.prepare_shared_surface(64,64,row_pitch,row.dxgi,uint64_t(row_pitch)*64,
                                            BC250_SHARED_BIND_RENDER_TARGET,owner)==S_OK);
            assert(r.surface.format==row.d3dddi && r.texture.Format==uint32_t(row.dxgi));
        }
        // Every refusal, each leaving nothing behind: a pitch that is not a multiple of 16 and one below the row,
        // a size that is not page rounded and one that does not cover four-row blocks, an empty and an oversized
        // edge, bind flags outside the mask, and a format no shared surface has.
        for(const uint32_t bad_pitch:{uint32_t(0),uint32_t(1020),uint32_t(512)})
            assert(r.prepare_shared_surface(256,254,bad_pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG &&
                   !r.texture.Magic && !r.surface.magic && !r.args.pAllocationInfo);
        assert(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size+16,bind)==E_INVALIDARG);
        assert(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,uint64_t(pitch)*252,bind)==E_INVALIDARG);
        assert(r.prepare_shared_surface(0,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        assert(r.prepare_shared_surface(256,0,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        assert(r.prepare_shared_surface(8193,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        assert(r.prepare_shared_surface(256,8193,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,bind)==E_INVALIDARG);
        assert(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_B8G8R8A8_UNORM,size,0x4,owner)==E_INVALIDARG);
        assert(r.prepare_shared_surface(256,254,pitch,DXGI_FORMAT_R32_UINT,size,bind)==E_NOTIMPL &&
               !r.texture.Magic && !r.surface.magic);
        // The primary's own record is unchanged by all this: v1, PRIMARY, and no texture record on the wire.
        assert(r.prepare_surface(256,254,pitch,D3DDDIFMT_A8R8G8B8,size,owner)==S_OK);
        assert(r.args.PrivateDriverDataSize==native12::kE26rV1Bytes && r.args.pPrivateDriverData==&r.resource &&
               r.resource.shared==1 && !r.resource.access && !r.texture.Magic &&
               r.info.Flags==D3D12DDI_ALLOCATION_INFO_FLAGS_0022_PRIMARY);
    }
    puts("native allocation request accepted by KMD parser; cache/rounding/refusal gates passed; "
         "shared surface records written, decoded and admitted by the kernel driver's parser");
}
