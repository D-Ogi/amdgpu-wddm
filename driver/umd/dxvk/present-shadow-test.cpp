// SPDX-License-Identifier: MIT
// BD-065: which presents go through a B8G8R8A8 shadow, and the shadow's allocation request, checked against the
// same allocate_runtime_surface admission a swap-chain buffer passes (stub runtime callbacks, no GPU).
#include "present-shadow.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
namespace {
void check(bool b) { if (!b) std::abort(); }
int deviceIdentity; unsigned allocates=0,deallocates=0;
BC250_WDDM_ALLOCATION_PRIVATE seenSurface{}; BC250_SURFACE_RESOURCE_PRIVATE seenTexture{}; HANDLE seenResource=&deviceIdentity;
HRESULT APIENTRY allocate(HANDLE h,D3DDDICB_ALLOCATE *a) {
    ++allocates; check(h==&deviceIdentity && a->NumAllocations==1 && a->PrivateDriverDataSize==sizeof(seenTexture));
    seenResource=a->hResource;
    std::memcpy(&seenTexture,a->pPrivateDriverData,sizeof(seenTexture));
    std::memcpy(&seenSurface,a->pAllocationInfo2[0].pPrivateDriverData,sizeof(seenSurface));
    check(!a->pAllocationInfo2[0].Flags.Primary);
    a->pAllocationInfo2[0].hAllocation=51; a->hKMResource=0; return S_OK;
}
HRESULT APIENTRY deallocate(HANDLE h,const D3DDDICB_DEALLOCATE2 *a) {
    ++deallocates;
    // A shell-owned device allocation is freed by its handle: there is no runtime resource to close.
    check(h==&deviceIdentity && !a->hResource && a->NumAllocations==1 && *a->HandleList==51); return S_OK;
}
DXGI_DDI_PRESENT_FLAGS flags(UINT value) { DXGI_DDI_PRESENT_FLAGS f{}; f.Value=value; return f; }
}
int main() {
    const auto blt=flags(1),flip=flags(2),both=flags(3),none=flags(0);
    // Session 353 (Stygian, Unity 2017): a windowed blt-model RGBA buffer, no destination.
    check(present_shadow_format(blt,false,true,DXGI_FORMAT_R8G8B8A8_UNORM)==DXGI_FORMAT_B8G8R8A8_UNORM);
    check(present_shadow_format(blt,false,true,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    // Left alone: a buffer the kernel driver copies as it is, a flip, a named destination, a flip-model buffer.
    for (DXGI_FORMAT f:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM,
                        DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_A8_UNORM,DXGI_FORMAT_UNKNOWN})
        check(present_shadow_format(blt,false,true,f)==DXGI_FORMAT_UNKNOWN);
    for (auto f:{flip,both,none}) check(present_shadow_format(f,false,true,DXGI_FORMAT_R8G8B8A8_UNORM)==DXGI_FORMAT_UNKNOWN);
    check(present_shadow_format(blt,true,true,DXGI_FORMAT_R8G8B8A8_UNORM)==DXGI_FORMAT_UNKNOWN);
    check(present_shadow_format(blt,false,false,DXGI_FORMAT_R8G8B8A8_UNORM)==DXGI_FORMAT_UNKNOWN);

    // The request: the B8G8R8A8 LB7A row (A8R8G8B8), the engine's 256-byte pitch, whole pages, no runtime resource.
    RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 desc{};
    check(present_shadow_request(1920,1200,DXGI_FORMAT_B8G8R8A8_UNORM,request,desc)==S_OK);
    check(request.surface.Format==D3DDDIFMT_A8R8G8B8 && request.surface.Pitch==7680 && request.surface.Size==9216000 &&
          request.surface.Width==1920 && request.surface.Height==1200);
    check(!request.runtime_resource && !request.primary && !request.shared && !request.displayable && !request.cpu_read);
    check(desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM && desc.BindFlags==(D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE) &&
          desc.Usage==D3D11_USAGE_DEFAULT && desc.MipLevels==1 && desc.ArraySize==1 && desc.SampleDesc.Count==1);
    check(request.texture.Format==UINT(DXGI_FORMAT_B8G8R8A8_UNORM) && request.texture.Shared==0 && request.texture.Access==0);
    // An odd size: pitch rounded up to 256 bytes, rows to 4, size to a page.
    check(present_shadow_request(33,7,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,request,desc)==S_OK);
    check(request.surface.Pitch==256 && request.surface.Size==4096 && request.surface.Format==D3DDDIFMT_A8R8G8B8);
    for (auto bad:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_A8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT})
        check(present_shadow_request(64,64,bad,request,desc)==E_INVALIDARG);
    check(present_shadow_request(0,64,DXGI_FORMAT_B8G8R8A8_UNORM,request,desc)==E_INVALIDARG);
    check(present_shadow_request(16385,64,DXGI_FORMAT_B8G8R8A8_UNORM,request,desc)==E_INVALIDARG);

    // The request passes the same admission as a swap-chain buffer and allocates without a runtime resource.
    RuntimeDevice device; device.hDevice=&deviceIdentity;
    device.KTCallbacks.pfnAllocateCb=allocate; device.KTCallbacks.pfnDeallocate2Cb=deallocate;
    check(present_shadow_request(1920,1200,DXGI_FORMAT_B8G8R8A8_UNORM,request,desc)==S_OK);
    RuntimeSurfaceAllocation allocation{};
    {
        RuntimeDomain::Scope scope(device.domain);
        check(allocate_runtime_surface(device,request,allocation)==S_OK && allocates==1);
        check(!seenResource && allocation.allocation==51 && !allocation.runtime_resource);
        check(seenSurface.Format==D3DDDIFMT_A8R8G8B8 && seenSurface.Pitch==7680 && seenTexture.Width==1920);
        check(deallocate_runtime_surface(device,allocation)==S_OK && deallocates==1 && !allocation.allocation);
    }
    std::cout << "PASS present shadow: blt-model RGBA buffers only, BGRA LB7A request admitted\n";
    return 0;
}
