// SPDX-License-Identifier: MIT
#pragma once
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
// BD-253, lab flavour only: what a Present or a DXGI Blt wrote, read back from the GPU. The lines tell where the
// pixels of a window stop: in the buffer the application drew, in the shadow, or in the destination the copy
// wrote. A read costs a GPU wait, so only a few operations of a process are read (probe_due).
#include "device-owner.h"
#include "ddi-resource.h"
#include "runtime-surface-format.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
namespace bc250::umd {
// The first six operations of a kind in a process, then every 600th up to ten more.
inline bool probe_due(std::atomic_uint &count,unsigned &index) {
    index=count.fetch_add(1,std::memory_order_relaxed);
    return index<6 || (index%600==0 && index/600<=10);
}
// Four texels of one subresource of a 2D texture: the centre, near the top-left corner, near the bottom-right
// corner, and a quarter in. Each is copied into a 4x1 staging texture on the engine's immediate context, and Map
// waits for the copies. The words are the texels' 32 bits as a little-endian number: 0xAARRGGBB for B8G8R8A8,
// 0xAABBGGRR for R8G8B8A8. A texture of another size of texel, a multisampled one, or an error writes its reason.
inline void probe_texels(DeviceOwner &owner,ID3D11Resource *resource,UINT subresource,char *out,size_t size) {
    ID3D11Texture2D *texture=nullptr,*staging=nullptr;
    ID3D11Device5 *device=owner.device(); ID3D11DeviceContext4 *context=owner.context();
    HRESULT hr=resource && device && context ?
        resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void **>(&texture)) : E_POINTER;
    D3D11_TEXTURE2D_DESC d{};
    if (SUCCEEDED(hr)) {
        texture->GetDesc(&d);
        const auto *row=runtime_surface_format(d.Format);
        if (d.SampleDesc.Count!=1 || !row || row->bytes_per_pixel!=4 || !d.MipLevels ||
            subresource>=d.MipLevels*d.ArraySize) {
            std::snprintf(out,size,"not-read(format=%u samples=%u)",unsigned(d.Format),d.SampleDesc.Count);
            texture->Release(); return;
        }
        D3D11_TEXTURE2D_DESC s{4,1,1,1,d.Format,{1,0},D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0};
        hr=device->CreateTexture2D(&s,nullptr,&staging);
    }
    unsigned long words[4]{};
    if (SUCCEEDED(hr)) {
        const UINT mip=subresource%d.MipLevels;
        const UINT w=std::max(1u,d.Width>>mip),h=std::max(1u,d.Height>>mip);
        const UINT x[4]={w/2,std::min(1u,w-1),w>2 ? w-2 : w-1,w/4},y[4]={h/2,std::min(1u,h-1),h>2 ? h-2 : h-1,h/4};
        for (UINT i=0;i<4;++i) {
            const D3D11_BOX box{x[i],y[i],0,x[i]+1,y[i]+1,1};
            context->CopySubresourceRegion(staging,0,i,0,0,resource,subresource,&box);
        }
        D3D11_MAPPED_SUBRESOURCE map{};
        hr=context->Map(staging,0,D3D11_MAP_READ,0,&map);
        if (SUCCEEDED(hr)) {
            std::memcpy(words,map.pData,sizeof(words));
            context->Unmap(staging,0);
        }
    }
    if (SUCCEEDED(hr))
        std::snprintf(out,size,"%08lX %08lX %08lX %08lX",words[0],words[1],words[2],words[3]);
    else std::snprintf(out,size,"not-read(hr=%08X)",unsigned(hr));
    if (staging) staging->Release();
    if (texture) texture->Release();
}
// The format and size of a resource for the lines, or zeros.
inline void probe_shape(const DdiResource *resource,unsigned &format,unsigned &width,unsigned &height) {
    format=width=height=0;
    if (resource && resource->runtime_surface) {
        const auto &d=resource->runtime_surface->desc;
        format=unsigned(d.Format); width=d.Width; height=d.Height;
    }
}
}
#endif
