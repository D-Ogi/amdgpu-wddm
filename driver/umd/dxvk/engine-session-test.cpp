// SPDX-License-Identifier: MIT
#include "engine-session.h"
#include "engine-error.h"
#include "engine-abi.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
using namespace bc250::umd;
namespace {
void check(bool ok) { if (!ok) std::abort(); }
std::string trace;
int mode=0;
VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
const char *extensions[]={"test_extension"};
#pragma warning(push)
#pragma warning(disable:4100)
struct Engine final : IBc250DxvkDevice {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void **) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { trace+='X'; return mode==4 ? 1 : 0; }
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
} engine;
#pragma warning(pop)
HRESULT APIENTRY adapter(const BC250_DXVK_VULKAN_INSTANCE *,BC250_DXVK_ADAPTER_INFO *a) {
    trace+='A'; a->MaxFeatureLevel=D3D_FEATURE_LEVEL_11_1; return S_OK;
}
HRESULT APIENTRY requirements(const BC250_DXVK_VULKAN_INSTANCE *,BC250_DXVK_DEVICE_REQUIREMENTS *r) {
    trace+='R'; r->Features=mode==1 ? nullptr : &features;
    r->QueueFamily=3; r->ExtensionCount=1; r->ExtensionNames=extensions; return S_OK;
}
void APIENTRY free_requirements(BC250_DXVK_DEVICE_REQUIREMENTS *) { trace+='F'; }
VkResult VKAPI_PTR create(VkPhysicalDevice,const VkDeviceCreateInfo *c,const VkAllocationCallbacks *,VkDevice *d) {
    trace+='V'; check(c->pNext==&features && !c->pEnabledFeatures && c->queueCreateInfoCount==1);
    check(c->pQueueCreateInfos->queueFamilyIndex==3 && c->ppEnabledExtensionNames==extensions);
    if (mode==2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *d=reinterpret_cast<VkDevice>(3); return VK_SUCCESS;
}
void VKAPI_PTR destroy(VkDevice d,const VkAllocationCallbacks *) { check(d==reinterpret_cast<VkDevice>(3)); trace+='D'; }
void VKAPI_PTR queue(VkDevice, uint32_t family,uint32_t index,VkQueue *q) {
    check(family==3 && index==0); trace+='Q'; *q=reinterpret_cast<VkQueue>(4);
}
PFN_vkVoidFunction VKAPI_PTR get(VkInstance,const char *name) {
    if (!std::strcmp(name,"vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(create);
    if (!std::strcmp(name,"vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    if (!std::strcmp(name,"vkGetDeviceQueue")) return reinterpret_cast<PFN_vkVoidFunction>(queue);
    return nullptr;
}
HRESULT APIENTRY engine_create(const BC250_DXVK_DEVICE_CREATE_INFO *c,IBc250DxvkDevice **out) {
    trace+='E'; check(c->AbiVersion==kRequiredEngineAbi); check(c->Threading==BC250_DXVK_THREADING_INLINE && c->Device->Features==&features);
    check(c->Device->ExtensionNames==extensions && c->Device->QueueFamily==3);
    if (mode==3) return E_FAIL;
    *out=&engine; return S_OK;
}
}
int main() {
    {
        EngineErrorState state; unsigned reads=0; HRESULT next=S_OK;
        auto read=[&]() { ++reads; const HRESULT v=next; next=S_OK; return v; };
        check(state.poll(read,EngineErrorPolicy::allow_out_of_memory)==S_OK && reads==1);
        next=E_OUTOFMEMORY;
        check(state.poll(read,EngineErrorPolicy::allow_out_of_memory)==E_OUTOFMEMORY && reads==2);
        check(state.poll(read,EngineErrorPolicy::allow_out_of_memory)==S_OK && reads==3);
        next=E_FAIL;
        check(state.poll(read,EngineErrorPolicy::allow_out_of_memory)==DXGI_ERROR_DEVICE_REMOVED && reads==4);
        check(state.poll(read,EngineErrorPolicy::allow_out_of_memory)==DXGI_ERROR_DEVICE_REMOVED && reads==4);
        EngineErrorState strict;
        check(strict.poll([] { return E_OUTOFMEMORY; },EngineErrorPolicy::device_removed_only)==DXGI_ERROR_DEVICE_REMOVED);
        check(strict.poll([] { return S_OK; },EngineErrorPolicy::allow_out_of_memory)==DXGI_ERROR_DEVICE_REMOVED);
        EngineErrorState invalid;
        check(invalid.poll([] { return S_FALSE; },EngineErrorPolicy::allow_out_of_memory)==DXGI_ERROR_DEVICE_REMOVED);
        check(!compatible_engine_abi(0x10003) && compatible_engine_abi(0x10004) &&
            compatible_engine_abi(0x10005) && !compatible_engine_abi(0x20004));
    }
    BC250_DXVK_ENGINE_FUNCS funcs{sizeof(funcs),BC250_DXVK_ENGINE_ABI_VERSION,requirements,free_requirements,adapter,engine_create};
    BC250_DXVK_VULKAN_INSTANCE instance{}; instance.Size=sizeof(instance);
    instance.Instance=reinterpret_cast<VkInstance>(1); instance.PhysicalDevice=reinterpret_cast<VkPhysicalDevice>(2);
    instance.GetInstanceProcAddr=get;
    BC250_DXVK_SHELL_SERVICES services{}; services.Size=sizeof(services);
    const char *expected[]={"ARVQE","ARF","ARVF","ARVQEDF","ARVQE"};
    for (mode=0;mode<=4;++mode) {
        trace.clear(); RuntimeDomain domain; EngineSession session(domain);
        check(session.open(funcs,instance,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED && trace.empty());
        RuntimeDomain::Scope scope(domain);
        HRESULT hr=session.open(funcs,instance,D3D_FEATURE_LEVEL_11_0,services);
        check((mode==0 || mode==4) ? hr==S_OK : FAILED(hr));
        check(trace==expected[mode]);
        if (mode==4) {
            check(session.close()==E_UNEXPECTED && session.must_retain_owner());
            check(trace=="ARVQEX" && session.device()!=VK_NULL_HANDLE);
            check(session.close()==E_UNEXPECTED && trace=="ARVQEX");
        } else {
            check(session.close()==S_OK);
            if (!mode) check(trace=="ARVQEXDF");
            check(session.device()==VK_NULL_HANDLE);
            const auto saved=trace; check(session.close()==S_OK && saved==trace);
        }
    }
    std::cout << "PASS engine session: requirements, device creation, rollback, release-before-destroy, leaked-owner retention\n";
}
