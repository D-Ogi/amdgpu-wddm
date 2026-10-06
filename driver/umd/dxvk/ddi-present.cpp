// SPDX-License-Identifier: MIT
#include "ddi-present.h"
#include <atomic>
#include <cstdio>
namespace bc250::umd {
namespace {
struct Submission { DeviceOwner *owner; ID3D11Resource *resource; UINT subresource; };
HRESULT submit(void *data) {
    auto &s=*static_cast<Submission *>(data);
    // E3: flush the frame and process deferred optimized pipeline work. Plain
    // context.Flush is insufficient for the engine's frame lifecycle.
    HRESULT hr=s.owner->take_deferred_error(EngineErrorPolicy::allow_out_of_memory);
    if (FAILED(hr)) return hr;
    hr=s.owner->engine()->SubmitForPresent(s.resource,s.subresource);
    const HRESULT deferred=s.owner->take_deferred_error(EngineErrorPolicy::allow_out_of_memory);
    return FAILED(hr) ? hr : deferred;
}
HRESULT APIENTRY rotate(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args) {
    if (!args) return E_INVALIDARG;
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(args->hDevice);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    try {
        if (!owner.engine()) return E_FAIL;
        return ddi_device_status(rotate_present_resources(args->pResources,args->Resources,[&](ID3D11Resource *const *resources,UINT count) {
            return owner.engine()->RotateResourceIdentities(resources,count);
        },&owner.runtime()));
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
// BD-065: the shadow for this present, written by the engine from the buffer. A slot is written again only
// after the Present that read it has retired; a slot of another size or format is replaced.
HRESULT write_present_shadow(DeviceOwner &owner,const DdiResource &source,UINT subresource,
    DXGI_FORMAT format,PresentShadowSlot *&out) {
    auto &shadows=owner.present_shadows();
    auto &slot=shadows.slots[shadows.next%2];
    const auto &desc=source.runtime_surface->desc;
    HRESULT hr=S_OK;
    if (slot.surface && (slot.surface->desc.Width!=desc.Width || slot.surface->desc.Height!=desc.Height ||
        slot.surface->desc.Format!=format)) {
        // Closing waits for every Present to retire (close_runtime_texture), as a resize may.
        hr=owner.close_surface(*slot.surface);
        if (hr!=S_OK) return FAILED(hr) ? hr : E_FAIL;
        slot={};
    }
    if (!slot.surface) {
        RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 texture{};
        hr=present_shadow_request(desc.Width,desc.Height,format,request,texture);
        if (FAILED(hr)) return hr;
        RuntimeSurface *surface=nullptr;
        hr=owner.begin_surface(request,texture,surface);
        if (SUCCEEDED(hr)) hr=owner.wait_surface(*surface);
        if (hr!=S_OK) {
            if (surface) owner.release_surface_handle(*surface);
            return FAILED(hr) ? hr : E_FAIL;
        }
        slot.surface=surface; slot.retire=0;
        // A witness that the blt-model path is in use: one line per new shadow, bounded.
        static std::atomic_uint created{0};
        if (created.fetch_add(1,std::memory_order_relaxed)<4) {
            char text[160];
            std::snprintf(text,sizeof(text),"BC250 BD-065: Present shadow %ux%u format %u for buffer format %u\n",
                desc.Width,desc.Height,unsigned(format),unsigned(desc.Format));
            OutputDebugStringA(text);
        }
    } else {
        hr=wait_present_value(owner.bridge(),slot.retire);
        if (FAILED(hr)) return hr;
    }
    BC250_DXVK_BLT blt{};
    blt.Destination=slot.surface->texture.texture; blt.DestinationSubresource=0;
    blt.DestinationRect={0,0,LONG(desc.Width),LONG(desc.Height)};
    blt.Source=source.object; blt.SourceSubresource=subresource;
    blt.Flags=BC250_DXVK_BLT_CONVERT; blt.Rotation=DXGI_DDI_MODE_ROTATION_IDENTITY;
    hr=owner.engine()->Blt(&blt);
    if (FAILED(hr)) return hr;
    out=&slot; return S_OK;
}
HRESULT APIENTRY present(DXGI_DDI_ARG_PRESENT *args) {
    if (!args) return E_INVALIDARG;
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(args->hDevice);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    try {
        if (!owner.engine()) return E_FAIL;
        auto *source=reinterpret_cast<DdiResource *>(args->hSurfaceToPresent);
        auto *destination=reinterpret_cast<DdiResource *>(args->hDstResource);
        // Only an importer may establish the allocation/subresource identity.
        // Ordinary engine-owned resources cannot be presented via this callback.
        if (!source || !source->object || !source->present_allocation ||
            source->present_subresource!=args->SrcSubResourceIndex) return E_INVALIDARG;
        if (destination && (!destination->object || !destination->present_allocation ||
            destination->present_subresource!=args->DstSubResourceIndex)) return E_INVALIDARG;
        Submission submission{&owner,source->object,args->SrcSubResourceIndex};
        D3DKMT_HANDLE presented=source->present_allocation;
        PresentShadowSlot *shadow=nullptr;
        const DXGI_FORMAT shadowFormat=source->runtime_surface ?
            present_shadow_format(args->Flags,destination!=nullptr,source->blt_model_buffer,
                source->runtime_surface->desc.Format) : DXGI_FORMAT_UNKNOWN;
        if (shadowFormat!=DXGI_FORMAT_UNKNOWN) {
            HRESULT hr=write_present_shadow(owner,*source,args->SrcSubResourceIndex,shadowFormat,shadow);
            if (ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED) return D3DDDIERR_DEVICEREMOVED;
            if (SUCCEEDED(hr)) {
                presented=shadow->surface->allocation.allocation;
                submission.resource=shadow->surface->texture.texture; submission.subresource=0;
            } else {
                // The buffer is presented as before (the kernel driver drops it); the game keeps running.
                static std::atomic_uint reports{0};
                if (reports.fetch_add(1,std::memory_order_relaxed)<4) {
                    char text[160];
                    std::snprintf(text,sizeof(text),"BC250 BD-065: Present shadow failed hr=%08X format=%u width=%u height=%u\n",
                        unsigned(hr),unsigned(shadowFormat),source->runtime_surface->desc.Width,source->runtime_surface->desc.Height);
                    OutputDebugStringA(text);
                }
                shadow=nullptr;
            }
        }
        const HRESULT hr=present_runtime(owner.bridge(),presented,
            destination ? destination->present_allocation : 0,args->pDXGIContext,submit,&submission);
        if (shadow && SUCCEEDED(hr)) {
            // This Present reads the slot; the next write of it waits for this value.
            shadow->retire=owner.bridge().present_value;
            owner.present_shadows().next^=1u;
        }
        return ddi_device_status(hr);
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
void install_present_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &t) { t.pfnPresent=present; t.pfnRotateResourceIdentities=rotate; }
}
