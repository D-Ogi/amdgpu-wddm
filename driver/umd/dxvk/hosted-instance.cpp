// SPDX-License-Identifier: MIT
#include "hosted-instance.h"
#include <cstring>
#include <new>
#include <vector>
namespace bc250::umd {
HRESULT HostedInstance::open(PFN_vkGetInstanceProcAddr get, const bc250_host &host,
    UINT32 api_version, UINT32 extension_count, const char *const *extensions) {
    if (!domain_.entered() || info_.Instance) return E_UNEXPECTED;
    if (!get || host.sType!=BC250_HOST_STYPE || host.version!=BC250_HOST_VERSION ||
        host.size!=sizeof(host) || !host.identity || !host.dispatch ||
        api_version<VK_API_VERSION_1_1 || (extension_count && !extensions)) return E_INVALIDARG;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE,"vkCreateInstance"));
    if (!create) return E_NOINTERFACE;
    host_=host;
    info_={}; info_.Size=sizeof(info_); info_.GetInstanceProcAddr=get;
    info_.ApiVersion=api_version; info_.ExtensionCount=extension_count; info_.ExtensionNames=extensions;
    VkApplicationInfo app{}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="BC250 system D3D UMD"; app.apiVersion=api_version;
    VkInstanceCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pNext=&host_; ci.pApplicationInfo=&app;
    ci.enabledExtensionCount=extension_count; ci.ppEnabledExtensionNames=extensions;
    VkResult result=create(&ci,nullptr,&info_.Instance);
    if (result!=VK_SUCCESS) {
        info_={};
        return result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY ? E_OUTOFMEMORY : E_FAIL;
    }
    destroy_=reinterpret_cast<PFN_vkDestroyInstance>(get(info_.Instance,"vkDestroyInstance"));
    auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(info_.Instance,"vkEnumeratePhysicalDevices"));
    auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(info_.Instance,"vkGetPhysicalDeviceProperties2"));
    if (!destroy_ || !enumerate || !properties) { close(); return E_NOINTERFACE; }
    HRESULT failure=DXGI_ERROR_NOT_FOUND;
    try {
        for (unsigned attempt=0;attempt<3;++attempt) {
            uint32_t count=0;
            result=enumerate(info_.Instance,&count,nullptr);
            if (result!=VK_SUCCESS) { failure=E_FAIL; break; }
            if (!count) break;
            std::vector<VkPhysicalDevice> devices(count);
            result=enumerate(info_.Instance,&count,devices.data());
            if (result==VK_INCOMPLETE) { failure=E_FAIL; continue; }
            if (result!=VK_SUCCESS || count>devices.size()) { failure=E_FAIL; break; }
            for (uint32_t i=0;i<count;++i) {
                VkPhysicalDeviceIDProperties id{}; id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                VkPhysicalDeviceProperties2 props{}; props.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                props.pNext=&id; properties(devices[i],&props);
                if (id.deviceLUIDValid && !std::memcmp(id.deviceLUID,&host_.adapter_luid,VK_LUID_SIZE)) {
                    if (info_.PhysicalDevice) { close(); return E_FAIL; } // ambiguous identity
                    info_.PhysicalDevice=devices[i];
                }
            }
            if (info_.PhysicalDevice) return S_OK;
            break;
        }
    } catch (const std::bad_alloc &) { failure=E_OUTOFMEMORY; }
    close(); return failure;
}
HRESULT HostedInstance::close() {
    if (!domain_.entered()) return E_UNEXPECTED;
    if (info_.Instance) {
        // Broken ICD: retain the instance/module instead of losing a live handle.
        if (!destroy_) return E_NOINTERFACE;
        destroy_(info_.Instance,nullptr);
    }
    info_={}; host_={}; destroy_=nullptr; return S_OK;
}
}
