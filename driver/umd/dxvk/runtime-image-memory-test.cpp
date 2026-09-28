// SPDX-License-Identifier: MIT
#include "runtime-image-memory.h"
#include "runtime-texture.h"
#include "runtime-surface.h"
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
namespace {
int identity; unsigned allocations,bindings,frees;
VkDeviceMemory memory=reinterpret_cast<VkDeviceMemory>(uintptr_t(7));
VkResult allocationResult=VK_SUCCESS,bindResult=VK_SUCCESS;
void check(bool b) { if (!b) std::abort(); }
void VKAPI_CALL requirements(VkDevice,VkImage,VkMemoryRequirements *r) { *r={4096,256,2}; }
VkResult VKAPI_CALL allocate(VkDevice,const VkMemoryAllocateInfo *a,const VkAllocationCallbacks *,VkDeviceMemory *m) {
    ++allocations;
    auto *i=static_cast<const bc250_host_import *>(a->pNext);
    auto *d=static_cast<const VkMemoryDedicatedAllocateInfo *>(i->pNext);
    check(i->identity==&identity && i->allocation==31 && i->va==65536 && i->size==8192 && d->image);
    check(d->sType==VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO && a->allocationSize==4096 && a->memoryTypeIndex==1);
    if (allocationResult==VK_SUCCESS) *m=memory; return allocationResult;
}
VkResult VKAPI_CALL bind(VkDevice,VkImage,VkDeviceMemory m,VkDeviceSize offset) { ++bindings; check(m==memory && !offset); return bindResult; }
void VKAPI_CALL release(VkDevice,VkDeviceMemory m,const VkAllocationCallbacks *) { ++frees; check(m==memory); }
}
namespace {
unsigned imageCreates=0,imageDestroys=0;
bool badPitch=false;
VkImageCreateInfo createdInfo{};
const VkFormat viewFormats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};
const VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,nullptr,2,viewFormats};
VkResult VKAPI_CALL image_create(VkDevice,const VkImageCreateInfo *info,const VkAllocationCallbacks *,VkImage *out) {
    createdInfo=*info; ++imageCreates; check(info->usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    *out=reinterpret_cast<VkImage>(uintptr_t(2)); return VK_SUCCESS;
}
void VKAPI_CALL image_destroy(VkDevice,VkImage,const VkAllocationCallbacks *) { ++imageDestroys; }
void VKAPI_CALL image_layout(VkDevice,VkImage,const VkImageSubresource *sub,VkSubresourceLayout *out) {
    check(sub->aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && !sub->mipLevel && !sub->arrayLayer);
    *out={0,4096,badPitch ? 512u : 256u,0,0};
}
}
namespace {
HRESULT textureWait=S_OK,textureWrap=S_OK; ULONG remainingRefs=0;
unsigned waits=0,releases=0;
HRESULT describe_texture(void *,const D3D11_TEXTURE2D_DESC1 *,VkImageCreateInfo *i) {
    *i={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; i->imageType=VK_IMAGE_TYPE_2D; i->format=VK_FORMAT_R8G8B8A8_UNORM;
    i->pNext=&formatList; i->flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    i->extent={64,16,1}; i->mipLevels=i->arrayLayers=1; i->samples=VK_SAMPLE_COUNT_1_BIT;
    i->usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; i->tiling=VK_IMAGE_TILING_OPTIMAL; return S_OK;
}
HRESULT wrap_texture(void *,const D3D11_TEXTURE2D_DESC1 *,const VkImageCreateInfo *info,VkImage,ID3D11Texture2D **t) {
    check(info && info->tiling==VK_IMAGE_TILING_LINEAR && info->usage==VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    check(info->extent.width==64 && info->extent.height==16);
    check(info->pNext==&formatList && info->flags==VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    check(createdInfo.pNext==info->pNext && createdInfo.flags==info->flags && createdInfo.tiling==info->tiling);
    if (SUCCEEDED(textureWrap)) *t=reinterpret_cast<ID3D11Texture2D *>(uintptr_t(9)); return textureWrap;
}
HRESULT wait_texture(void *,ID3D11Texture2D *) { ++waits; return textureWait; }
ULONG release_texture(ID3D11Texture2D *) { ++releases; return remainingRefs; }
}
namespace {
UINT64 surfaceFence=0;
RuntimeSurface *closingSurface=nullptr;
unsigned surfaceAllocates=0,surfaceUnmaps=0,surfaceDeallocates=0;
bool failSurfaceUnmap=false,failSurfaceResident=false;
HRESULT APIENTRY surface_allocate(HANDLE h,D3DDDICB_ALLOCATE *a) {
    check(h==&identity); ++surfaceAllocates;
    a->pAllocationInfo2[0].hAllocation=31; return S_OK;
}
HRESULT APIENTRY surface_deallocate(HANDLE,const D3DDDICB_DEALLOCATE2 *a) {
    check(a->NumAllocations==1 && *a->HandleList==31);
    check(closingSurface && !closingSurface->mapping.address && !closingSurface->texture.image.image);
    ++surfaceDeallocates; return S_OK;
}
HRESULT APIENTRY surface_map(HANDLE,D3DDDI_MAPGPUVIRTUALADDRESS *m) {
    check(m->hAllocation==31 && m->SizeInPages==2);
    m->VirtualAddress=65536; m->PagingFenceValue=5; return E_PENDING;
}
HRESULT APIENTRY surface_resident(HANDLE,D3DDDI_MAKERESIDENT *r) {
    if (failSurfaceResident) return E_OUTOFMEMORY;
    r->PagingFenceValue=9; return E_PENDING;
}
HRESULT APIENTRY surface_unmap(HANDLE,const D3DDDICB_FREEGPUVIRTUALADDRESS *m) {
    check(m->BaseAddress==65536 && m->Size==8192);
    check(closingSurface && !closingSurface->texture.texture && !closingSurface->texture.image.image && !closingSurface->texture.image.memory);
    ++surfaceUnmaps;
    return failSurfaceUnmap ? E_FAIL : S_OK;
}
}
namespace {
#pragma warning(push)
#pragma warning(disable:4100)
struct ImportEngine final : IBc250DxvkDevice2 {
    bool supported=true; unsigned drops=0,wraps=0; HRESULT wrapResult=S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **p) override {
        check(id==__uuidof(IBc250DxvkDevice2)); *p=nullptr;
        if (!supported) return E_NOINTERFACE;
        *p=static_cast<IBc250DxvkDevice2 *>(this); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { ++drops; return 1; }
    HRESULT STDMETHODCALLTYPE GetD3D11Device(REFIID riid, void **device) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetImmediateContext(REFIID riid, void **context) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateShader(const BC250_DXVK_SHADER_DESC *desc, REFIID riid,
                                                   void **shader) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const BC250_DXVK_INPUT_LAYOUT *layout,
                                                        ID3D11InputLayout **inputLayout) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexFormat(DXGI_FORMAT format, VkFormat *vkFormat,
                                                      UINT *elementSize) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetImageCreateInfo(const D3D11_TEXTURE2D_DESC1 *desc,
                                                         VkImageCreateInfo *info) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage(const D3D11_TEXTURE2D_DESC1 *desc, VkImage image,
                                                               ID3D11Texture2D **texture) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE WaitForResourceIdle(ID3D11Resource *resource) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsResourceBusy(ID3D11Resource *resource, UINT subresource) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SubmitForPresent(ID3D11Resource *source, UINT subresource) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RotateResourceIdentities(ID3D11Resource *const *resources, UINT count) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Blt(const BC250_DXVK_BLT *blt) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Blt1(const BC250_DXVK_BLT1 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage2(const D3D11_TEXTURE2D_DESC1 *d,
        const VkImageCreateInfo *i,VkImage image,ID3D11Texture2D **t) override {
        check(d && i && i->tiling==VK_IMAGE_TILING_LINEAR && image); ++wraps;
        if (SUCCEEDED(wrapResult)) *t=reinterpret_cast<ID3D11Texture2D *>(uintptr_t(9));
        return wrapResult;
    }
} importEngine;
#pragma warning(pop)
}
int main() {
    RuntimeDevice runtime; runtime.hDevice=&identity;
    VkDevice device=reinterpret_cast<VkDevice>(uintptr_t(1)); VkImage image=reinterpret_cast<VkImage>(uintptr_t(2));
    ImageMemoryDispatch dispatch{requirements,allocate,bind,release};
    VkPhysicalDeviceMemoryProperties properties{}; properties.memoryTypeCount=2; properties.memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    bc250_host_import imported{}; imported.sType=BC250_HOST_IMPORT_STYPE; imported.identity=&identity;
    imported.allocation=31; imported.va=65536; imported.size=8192;
    VkDeviceMemory out=VK_NULL_HANDLE;
    auto run=[&]() { return import_runtime_image_memory(runtime,device,image,dispatch,properties,imported,out); };
    check(run()==E_INVALIDARG && !allocations);
    RuntimeDomain::Scope scope(runtime.domain);
    imported.size=100; check(run()==E_INVALIDARG && !allocations); imported.size=8192;
    ++imported.va; check(run()==E_INVALIDARG && !allocations); --imported.va;
    imported.identity=nullptr; check(run()==E_INVALIDARG && !allocations); imported.identity=&identity;
    allocationResult=VK_ERROR_OUT_OF_DEVICE_MEMORY; check(run()==E_OUTOFMEMORY && !out && !bindings && !frees);
    allocationResult=VK_SUCCESS; bindResult=VK_ERROR_DEVICE_LOST;
    check(run()==D3DDDIERR_DEVICEREMOVED && !out && frees==1);
    bindResult=VK_SUCCESS; check(run()==S_OK && out==memory && allocations==3 && bindings==2 && frees==1);
    check(run()==E_UNEXPECTED && allocations==3);
    RuntimeImageDispatch imageDispatch{image_create,image_destroy,image_layout,dispatch};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D; info.extent={64,16,1}; info.mipLevels=info.arrayLayers=1;
    info.format=VK_FORMAT_R8G8B8A8_UNORM; info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_LINEAR; info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    RuntimeImage importedImage{};
    auto createImage=[&]() { return create_linear_runtime_image(runtime,device,imageDispatch,properties,info,imported,256,256,importedImage); };
    badPitch=true; check(createImage()==E_INVALIDARG && imageDestroys==1 && allocations==3 && !importedImage.image);
    badPitch=false; bindResult=VK_ERROR_DEVICE_LOST;
    check(createImage()==D3DDDIERR_DEVICEREMOVED && imageDestroys==2 && frees==2 && !importedImage.image);
    bindResult=VK_SUCCESS; check(createImage()==S_OK && importedImage.image && importedImage.memory);
    destroy_runtime_image(device,imageDispatch,importedImage);
    check(!importedImage.image && !importedImage.memory && imageDestroys==3 && frees==3);
    destroy_runtime_image(device,imageDispatch,importedImage);
    info.extent.height=UINT32_MAX; check(createImage()==E_INVALIDARG && imageCreates==3);
    info.extent.height=16; info.tiling=VK_IMAGE_TILING_OPTIMAL; check(createImage()==E_NOTIMPL && imageCreates==3);
    TextureImportDispatch textureDispatch{&identity,describe_texture,wrap_texture,wait_texture,release_texture};
    D3D11_TEXTURE2D_DESC1 textureDesc{}; textureDesc.Width=64;
    HostBridge bridge{}; bridge.device=&runtime;
    RuntimeTexture texture{};
    auto makeTexture=[&]() { return create_runtime_texture(runtime,device,imageDispatch,textureDispatch,properties,textureDesc,imported,256,texture); };
    textureWrap=E_OUTOFMEMORY;
    check(makeTexture()==E_OUTOFMEMORY && !texture.texture && !texture.image.image);
    textureWrap=S_OK; check(makeTexture()==S_OK && texture.texture);
    const unsigned beforeDestroy=imageDestroys;
    textureWait=E_FAIL;
    check(close_runtime_texture(bridge,device,imageDispatch,textureDispatch,texture)==E_FAIL && !releases && imageDestroys==beforeDestroy);
    textureWait=S_OK;
    check(close_runtime_texture(bridge,device,imageDispatch,textureDispatch,texture)==S_OK && releases==1 && !texture.image.image);
    check(close_runtime_texture(bridge,device,imageDispatch,textureDispatch,texture)==S_OK && releases==1);
    check(makeTexture()==S_OK); remainingRefs=1;
    check(close_runtime_texture(bridge,device,imageDispatch,textureDispatch,texture)==E_UNEXPECTED && texture.retained && texture.image.image);
    const unsigned retainedDestroy=imageDestroys;
    check(close_runtime_texture(bridge,device,imageDispatch,textureDispatch,texture)==E_UNEXPECTED && imageDestroys==retainedDestroy && releases==2);
    runtime.KTCallbacks.pfnAllocateCb=surface_allocate;
    runtime.KTCallbacks.pfnDeallocate2Cb=surface_deallocate;
    runtime.KTCallbacks.pfnMapGpuVirtualAddressCb=surface_map;
    runtime.KTCallbacks.pfnMakeResidentCb=surface_resident;
    runtime.KTCallbacks.pfnFreeGpuVirtualAddressCb=surface_unmap;
    SurfacePagingQueue queue{7,8,&surfaceFence};
    RuntimeSurfaceRequest request{};
    request.surface={BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,1,64,16,256,D3DDDIFMT_A8B8G8R8,8192};
    textureDesc.Height=16; textureDesc.MipLevels=textureDesc.ArraySize=1;
    textureDesc.SampleDesc.Count=1; textureDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    RuntimeSurface surface; closingSurface=&surface;
    auto begin=[&]() { return begin_runtime_surface(runtime,queue,request,textureDesc,surface); };
    auto finish=[&]() { return finish_runtime_surface(device,imageDispatch,textureDispatch,properties,surface); };
    auto close=[&]() { return close_runtime_surface(bridge,device,imageDispatch,textureDispatch,surface); };
    textureDesc.Height=15; check(begin()==E_INVALIDARG && !surfaceAllocates); textureDesc.Height=16;
    check(begin()==S_FALSE && surfaceAllocates==1 && !surface.texture.texture);
    check(begin()==E_UNEXPECTED && surfaceAllocates==1);
    unsigned previousCreates=imageCreates;
    check(finish()==S_FALSE && imageCreates==previousCreates);
    surfaceFence=5; check(finish()==S_FALSE && imageCreates==previousCreates);
    surfaceFence=9; remainingRefs=0;
    check(finish()==S_OK && surface.phase==SurfacePhase::ready && surface.texture.texture);
    check(finish()==S_OK && imageCreates==previousCreates+1);
    textureWait=E_FAIL;
    check(close()==E_FAIL && !surfaceUnmaps && !surfaceDeallocates);
    check(finish()==E_UNEXPECTED); // A closing resource cannot be republished.
    textureWait=S_OK; failSurfaceUnmap=true;
    check(close()==E_FAIL && !surface.texture.image.image && surface.mapping.address && !surfaceDeallocates);
    const unsigned releasedOnce=releases;
    failSurfaceUnmap=false; check(close()==S_OK && surfaceDeallocates==1 && releases==releasedOnce);
    check(close()==S_OK && surfaceDeallocates==1 && surface.phase==SurfacePhase::empty);
    surfaceFence=0; failSurfaceResident=true;
    check(begin()==E_OUTOFMEMORY && surface.mapping.address && !surface.mapping.resident);
    check(finish()==E_UNEXPECTED && close()==E_PENDING && surfaceDeallocates==1);
    surfaceFence=5; check(close()==S_OK && surfaceDeallocates==2);
    failSurfaceResident=false; surfaceFence=9;
    check(begin()==S_FALSE); textureWrap=E_OUTOFMEMORY;
    check(finish()==E_OUTOFMEMORY && surface.phase==SurfacePhase::failed);
    check(close()==S_OK && surfaceDeallocates==3); textureWrap=S_OK;
    check(begin()==S_FALSE && finish()==S_OK); remainingRefs=1;
    const unsigned unmappedBeforeRetention=surfaceUnmaps;
    check(close()==E_UNEXPECTED && surface.texture.retained);
    check(close()==E_UNEXPECTED && surfaceUnmaps==unmappedBeforeRetention && surfaceDeallocates==3);
    auto realDispatch=texture_import_dispatch(importEngine);
    ID3D11Texture2D *wrapped=nullptr;
    info.tiling=VK_IMAGE_TILING_LINEAR;
    check(realDispatch.wrap(realDispatch.engine,&textureDesc,&info,image,&wrapped)==S_OK);
    check(wrapped && importEngine.wraps==1 && importEngine.drops==1);
    importEngine.supported=false; wrapped=nullptr;
    check(realDispatch.wrap(realDispatch.engine,&textureDesc,&info,image,&wrapped)==E_NOINTERFACE);
    check(!wrapped && importEngine.wraps==1 && importEngine.drops==1);
    importEngine.supported=true; importEngine.wrapResult=E_INVALIDARG;
    check(realDispatch.wrap(realDispatch.engine,&textureDesc,&info,image,&wrapped)==E_INVALIDARG);
    check(!wrapped && importEngine.wraps==2 && importEngine.drops==2);
    // This test uses synthetic handles only, never an actual allocation/image.
    std::cout << "PASS image import and surface lifecycle: pending paging, teardown order, failure retention (mock callbacks)\n";
}
