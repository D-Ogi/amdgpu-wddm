// SPDX-License-Identifier: MIT
#include "runtime-texture.h"
#include "runtime-surface-format.h"
namespace bc250::umd {
TextureImportDispatch texture_import_dispatch(IBc250DxvkDevice &engine) {
    return {&engine,
        [](void *e,const D3D11_TEXTURE2D_DESC1 *d,VkImageCreateInfo *i) { return static_cast<IBc250DxvkDevice *>(e)->GetImageCreateInfo(d,i); },
        [](void *e,const D3D11_TEXTURE2D_DESC1 *d,const VkImageCreateInfo *info,VkImage i,ID3D11Texture2D **t) {
            IBc250DxvkDevice2 *extended=nullptr;
            HRESULT hr=static_cast<IBc250DxvkDevice *>(e)->QueryInterface(__uuidof(IBc250DxvkDevice2),reinterpret_cast<void **>(&extended));
            if (SUCCEEDED(hr)) hr=extended ? extended->CreateTexture2DFromImage2(d,info,i,t) : E_NOINTERFACE;
            if (extended) extended->Release();
            return hr;
        },
        [](void *e,ID3D11Texture2D *t) { return static_cast<IBc250DxvkDevice *>(e)->WaitForResourceIdle(t); },
        [](ID3D11Texture2D *t) { return t->Release(); }};
}
HRESULT create_runtime_texture(RuntimeDevice &runtime,VkDevice device,const RuntimeImageDispatch &vk,
    const TextureImportDispatch &engine,const VkPhysicalDeviceMemoryProperties &properties,
    const D3D11_TEXTURE2D_DESC1 &desc,const bc250_host_import &allocation,VkDeviceSize pitch,RuntimeTexture &out) {
    if (out.texture || out.image.image || out.image.memory || out.retained) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !engine.engine || !engine.describe || !engine.wrap || !engine.wait || !engine.release) return E_INVALIDARG;
    // A logical row is width times the table row's pixel size (1 byte for A8, 8 for RGBA16F), never 4.
    const auto *row=runtime_surface_format(desc.Format);
    if (!row) return E_NOTIMPL;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    HRESULT hr=engine.describe(engine.engine,&desc,&imageInfo);
    if (FAILED(hr)) return hr;
    // ABI permits selecting allocation tiling; keep every required usage/flag
    // and the engine-owned format-list pNext chain unchanged.
    imageInfo.tiling=VK_IMAGE_TILING_LINEAR;
    hr=create_linear_runtime_image(runtime,device,vk,properties,imageInfo,allocation,pitch,
        VkDeviceSize(desc.Width)*row->bytes_per_pixel,out.image);
    if (FAILED(hr)) return hr;
    hr=engine.wrap(engine.engine,&desc,&imageInfo,out.image.image,&out.texture);
    if (FAILED(hr) || !out.texture) {
        if (out.texture) {
            const ULONG refs=engine.release(out.texture); out.texture=nullptr;
            if (refs) { out.retained=true; return FAILED(hr) ? hr : E_UNEXPECTED; }
        }
        destroy_runtime_image(device,vk,out.image);
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}
HRESULT close_runtime_texture(HostBridge &bridge,VkDevice device,const RuntimeImageDispatch &vk,
    const TextureImportDispatch &engine,RuntimeTexture &texture) {
    if (!bridge.device || !bridge.device->domain.entered() || !engine.wait || !engine.release) return E_INVALIDARG;
    if (texture.retained) return E_UNEXPECTED;
    if (texture.texture) {
        // BD-253: a wait that reports the loss of a device whose work ended is not a failure of the close: no work
        // of this device runs on the texture any more. The kernel ended the work, or each fence of the device
        // reached its last value (settle_device_work). Before, every texture of a lost device stayed, so
        // DestroyDevice kept the whole device (close-surface, E_UNEXPECTED) for the life of the process.
        const auto ended=[&](HRESULT status) { return device_lost_status(status) && settle_device_work(bridge); };
        // Present runs on its own runtime context, outside engine completion.
        HRESULT hr=wait_present_idle(bridge);
        if (FAILED(hr) && !ended(hr)) return hr;
        hr=engine.wait(engine.engine,texture.texture);
        if (FAILED(hr) && !ended(hr)) return hr;
        const ULONG refs=engine.release(texture.texture); texture.texture=nullptr;
        if (refs) { texture.retained=true; return E_UNEXPECTED; }
    }
    destroy_runtime_image(device,vk,texture.image);
    return S_OK;
}
}
