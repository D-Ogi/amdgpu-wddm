// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
// Prepare all storage before engine mutation. On failure, shell identity stays
// unchanged; after success the commit consists only of non-throwing scalar moves.
template<typename Rotate> HRESULT rotate_present_resources(const DXGI_DDI_HRESOURCE *handles,UINT count,Rotate &&rotate,RuntimeDevice *expectedOwner=nullptr) {
    if (!count || !handles) return E_INVALIDARG;
    std::vector<DdiResource *> resources(count);
    std::vector<ID3D11Resource *> objects(count);
    struct Backing {
        D3DKMT_HANDLE allocation,kernel_resource;
        SurfaceGpuMapping mapping;
        RuntimeImage image;
        UINT pitch;
        UINT64 bytes;
    };
    std::vector<Backing> backing(count);
    for (UINT i=0;i<count;++i) {
        auto *r=reinterpret_cast<DdiResource *>(handles[i]);
        if (!r || !r->object) return E_INVALIDARG;
        for (UINT j=0;j<i;++j) if (objects[j]==r->object) return E_INVALIDARG;
        resources[i]=r; objects[i]=r->object;
        auto *surface=r->runtime_surface;
        if (bool(surface)!=bool(resources[0]->runtime_surface)) return E_INVALIDARG;
        if (surface) {
            auto *first=resources[0]->runtime_surface;
            if (surface->phase!=SurfacePhase::ready || surface->texture.retained ||
                !surface->owner || surface->owner!=first->owner ||
                (expectedOwner && surface->owner!=expectedOwner) ||
                surface->queue.queue!=first->queue.queue || surface->queue.sync!=first->queue.sync ||
                !surface->mapping.resident || !surface->mapping.address ||
                !surface->texture.image.image || !surface->texture.image.memory ||
                surface->texture.texture!=r->object || r->present_subresource ||
                !surface->allocation.allocation || surface->allocation.allocation!=r->present_allocation)
                return E_INVALIDARG;
            for (UINT j=0;j<i;++j) if (resources[j]->runtime_surface==surface) return E_INVALIDARG;
            backing[i]={surface->allocation.allocation,surface->allocation.kernel_resource,
                surface->mapping,surface->texture.image,surface->pitch,surface->bytes};
        }
    }
    HRESULT hr=rotate(objects.data(),count);
    if (hr!=S_OK) return hr;
    const D3DKMT_HANDLE firstAllocation=resources[0]->present_allocation;
    const UINT firstSubresource=resources[0]->present_subresource;
    for (UINT i=0;i+1<count;++i) {
        resources[i]->present_allocation=resources[i+1]->present_allocation;
        resources[i]->present_subresource=resources[i+1]->present_subresource;
    }
    resources[count-1]->present_allocation=firstAllocation;
    resources[count-1]->present_subresource=firstSubresource;
    for (UINT i=0;i<count;++i) {
        if (auto *surface=resources[i]->runtime_surface) {
            const auto &next=backing[(i+1)%count];
            // Runtime resource handle and COM identity belong to this logical
            // slot. Only backing storage follows the engine's image rotation.
            surface->allocation.allocation=next.allocation;
            surface->allocation.kernel_resource=next.kernel_resource;
            surface->mapping=next.mapping; surface->texture.image=next.image;
            surface->pitch=next.pitch; surface->bytes=next.bytes;
        }
    }
    return S_OK;
}
void install_present_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &);
}
