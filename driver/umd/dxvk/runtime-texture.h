// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-image-memory.h"
namespace bc250::umd {
struct TextureImportDispatch {
    void *engine;
    HRESULT (*describe)(void *,const D3D11_TEXTURE2D_DESC1 *,VkImageCreateInfo *);
    HRESULT (*wrap)(void *,const D3D11_TEXTURE2D_DESC1 *,const VkImageCreateInfo *,VkImage,ID3D11Texture2D **);
    HRESULT (*wait)(void *,ID3D11Texture2D *);
    ULONG (*release)(ID3D11Texture2D *);
};
TextureImportDispatch texture_import_dispatch(IBc250DxvkDevice &);
struct RuntimeTexture {
    RuntimeImage image{};
    ID3D11Texture2D *texture=nullptr;
    bool retained=false; // Final Release found an unexpected surviving owner.
};
HRESULT create_runtime_texture(RuntimeDevice &,VkDevice,const RuntimeImageDispatch &,
    const TextureImportDispatch &,const VkPhysicalDeviceMemoryProperties &,
    const D3D11_TEXTURE2D_DESC1 &,const bc250_host_import &,VkDeviceSize pitch,RuntimeTexture &);
HRESULT close_runtime_texture(HostBridge &,VkDevice,const RuntimeImageDispatch &,
    const TextureImportDispatch &,RuntimeTexture &);
}
