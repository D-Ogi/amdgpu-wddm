// SPDX-License-Identifier: MIT
#include "engine-session.h"
namespace bc250::umd {
static HRESULT vk_error(VkResult r) {
    if (r==VK_ERROR_OUT_OF_HOST_MEMORY || r==VK_ERROR_OUT_OF_DEVICE_MEMORY) return E_OUTOFMEMORY;
    if (r==VK_ERROR_DEVICE_LOST) return DXGI_ERROR_DEVICE_REMOVED;
    return E_FAIL;
}
HRESULT EngineSession::open(const BC250_DXVK_ENGINE_FUNCS &funcs,
    const BC250_DXVK_VULKAN_INSTANCE &instance, D3D_FEATURE_LEVEL level,
    const BC250_DXVK_SHELL_SERVICES &services) {
    if (!domain_.entered() || have_requirements_ || device_.Device || engine_ || leaked_) return E_UNEXPECTED;
    if (funcs.Size<sizeof(funcs) || funcs.AbiVersion!=BC250_DXVK_ENGINE_ABI_VERSION ||
        !funcs.QueryDeviceRequirements || !funcs.FreeDeviceRequirements || !funcs.GetAdapterInfo || !funcs.CreateDevice ||
        instance.Size<sizeof(instance) || !instance.Instance || !instance.PhysicalDevice || !instance.GetInstanceProcAddr ||
        services.Size<sizeof(services)) return E_INVALIDARG;
    funcs_=funcs; instance_=instance; services_=services;
    auto get=instance_.GetInstanceProcAddr;
    auto create=reinterpret_cast<PFN_vkCreateDevice>(get(instance_.Instance,"vkCreateDevice"));
    auto queue=reinterpret_cast<PFN_vkGetDeviceQueue>(get(instance_.Instance,"vkGetDeviceQueue"));
    destroy_=reinterpret_cast<PFN_vkDestroyDevice>(get(instance_.Instance,"vkDestroyDevice"));
    if (!create || !queue || !destroy_) return E_NOINTERFACE;
    BC250_DXVK_ADAPTER_INFO adapter{}; adapter.Size=sizeof(adapter);
    HRESULT hr=funcs_.GetAdapterInfo(&instance_,&adapter);
    if (FAILED(hr)) return hr;
    if (level>adapter.MaxFeatureLevel) return E_INVALIDARG;
    requirements_={}; requirements_.Size=sizeof(requirements_);
    hr=funcs_.QueryDeviceRequirements(&instance_,&requirements_);
    if (FAILED(hr)) return hr;
    have_requirements_=true;
    if (requirements_.Size<sizeof(requirements_) || !requirements_.Features ||
        requirements_.Features->sType!=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
        (requirements_.ExtensionCount && !requirements_.ExtensionNames)) { close(); return E_INVALIDARG; }
    const float priority=1.0f;
    VkDeviceQueueCreateInfo q{}; q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex=requirements_.QueueFamily; q.queueCount=1; q.pQueuePriorities=&priority;
    VkDeviceCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext=requirements_.Features; ci.queueCreateInfoCount=1; ci.pQueueCreateInfos=&q;
    ci.enabledExtensionCount=requirements_.ExtensionCount; ci.ppEnabledExtensionNames=requirements_.ExtensionNames;
    VkResult result=create(instance_.PhysicalDevice,&ci,nullptr,&device_.Device);
    if (result!=VK_SUCCESS) { device_.Device=VK_NULL_HANDLE; close(); return vk_error(result); }
    device_.Size=sizeof(device_); device_.QueueFamily=requirements_.QueueFamily;
    device_.ExtensionCount=requirements_.ExtensionCount; device_.ExtensionNames=requirements_.ExtensionNames;
    device_.Features=requirements_.Features;
    queue(device_.Device,device_.QueueFamily,0,&device_.Queue);
    if (!device_.Queue) { close(); return E_FAIL; }
    BC250_DXVK_DEVICE_CREATE_INFO info{}; info.Size=sizeof(info); info.AbiVersion=BC250_DXVK_ENGINE_ABI_VERSION;
    info.Instance=&instance_; info.Device=&device_; info.Services=&services_;
    info.FeatureLevel=level; info.Threading=BC250_DXVK_THREADING_INLINE;
    hr=funcs_.CreateDevice(&info,&engine_);
    if (FAILED(hr) || !engine_) { close(); return FAILED(hr) ? hr : E_FAIL; }
    return S_OK;
}
HRESULT EngineSession::close() {
    if (!domain_.entered()) return E_UNEXPECTED;
    if (leaked_) return E_UNEXPECTED;
    if (engine_) {
        const ULONG remaining=engine_->Release(); engine_=nullptr;
        if (remaining) { leaked_=true; return E_UNEXPECTED; }
    }
    if (device_.Device) { destroy_(device_.Device,nullptr); device_={}; }
    if (have_requirements_) { funcs_.FreeDeviceRequirements(&requirements_); have_requirements_=false; requirements_={}; }
    return S_OK;
}
}
