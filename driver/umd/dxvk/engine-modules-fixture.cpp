// SPDX-License-Identifier: MIT
#include "engine-modules.h"
namespace {
UINT fixtureMode=0;
HRESULT APIENTRY requirements(const BC250_DXVK_VULKAN_INSTANCE *,BC250_DXVK_DEVICE_REQUIREMENTS *) { return E_NOTIMPL; }
void APIENTRY release(BC250_DXVK_DEVICE_REQUIREMENTS *) {}
HRESULT APIENTRY adapter(const BC250_DXVK_VULKAN_INSTANCE *,BC250_DXVK_ADAPTER_INFO *) { return E_NOTIMPL; }
HRESULT APIENTRY create(const BC250_DXVK_DEVICE_CREATE_INFO *,IBc250DxvkDevice **) { return E_NOTIMPL; }
}
extern "C" __declspec(dllexport) void APIENTRY SetFixtureMode(UINT mode) { fixtureMode=mode; }
extern "C" __declspec(dllexport) HRESULT APIENTRY Bc250DxvkEngineGetFuncs(UINT32 version,BC250_DXVK_ENGINE_FUNCS *f) {
    if (version!=BC250_DXVK_ENGINE_ABI_VERSION || !f || f->Size<sizeof(*f)) return E_NOINTERFACE;
    *f={sizeof(*f),BC250_DXVK_ENGINE_ABI_VERSION,requirements,release,adapter,create};
    if (fixtureMode==1) f->AbiVersion=0;
    if (fixtureMode==2) f->CreateDevice=nullptr;
    if (fixtureMode==3) return E_FAIL;
    return S_OK;
}
extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance,const char *) { return nullptr; }
