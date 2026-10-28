// SPDX-License-Identifier: MIT
#include "hosted-instance.h"
#include <cstring>
#include <new>
#include <vector>
namespace bc250::umd {
namespace {
// The file name of the process executable in UTF-8, as DXVK's env::getExeName gives it.
std::string exe_name() {
    std::wstring path(MAX_PATH,L'\0');
    for (;;) {
        const DWORD n=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
        if (!n) return {};
        if (n<path.size()) { path.resize(n); break; }
        if (path.size()>=32768) return {};
        path.resize(path.size()*2);
    }
    const size_t slash=path.find_last_of(L'\\');
    if (slash!=std::wstring::npos) path.erase(0,slash+1);
    const int bytes=WideCharToMultiByte(CP_UTF8,0,path.c_str(),int(path.size()),nullptr,0,nullptr,nullptr);
    if (bytes<=0) return {};
    std::string name(size_t(bytes),'\0');
    WideCharToMultiByte(CP_UTF8,0,path.c_str(),int(path.size()),name.data(),bytes,nullptr,nullptr);
    return name;
}
}
HRESULT HostedInstance::open(PFN_vkGetInstanceProcAddr get, const bc250_host &host, UINT32 policy_flags,
    UINT32 api_version, UINT32 extension_count, const char *const *extensions) {
    if (!domain_.entered() || info_.Instance) return E_UNEXPECTED;
    if (!get || host.sType!=BC250_HOST_STYPE || host.version!=BC250_HOST_VERSION ||
        host.size!=sizeof(host) || !host.identity || !host.dispatch ||
        (policy_flags & ~BC250_HOST_POLICY_KNOWN_FLAGS) ||
        api_version<VK_API_VERSION_1_1 || (extension_count && !extensions)) return E_INVALIDARG;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE,"vkCreateInstance"));
    if (!create) return E_NOINTERFACE;
    // The D3D12 shell's chain: an ICD with the policy patch takes sparse binding from the host
    // alone, an older one skips the unknown structure and follows its environment.
    policy_={BC250_HOST_POLICY_STYPE,host.pNext,BC250_HOST_POLICY_VERSION,sizeof(policy_),policy_flags,0};
    host_=host; host_.pNext=&policy_;
    info_={}; info_.Size=sizeof(info_); info_.GetInstanceProcAddr=get;
    info_.ApiVersion=api_version; info_.ExtensionCount=extension_count; info_.ExtensionNames=extensions;
    // The same application info as DXVK's own instance (DxvkInstance::createInstance), because the ICD
    // selects its driconf engine and application sections by these names. The engine version is the one
    // that DXVK reports. The application version holds DXVK's instance flags, which are 0 for D3D10 and
    // D3D11 (RADV reads bit 0 as D3D9).
    try { app_name_=exe_name(); } catch (const std::bad_alloc &) { info_={}; return E_OUTOFMEMORY; }
    VkApplicationInfo app{}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName=app_name_.c_str(); app.applicationVersion=0;
    app.pEngineName="DXVK"; app.engineVersion=VK_MAKE_API_VERSION(0,3,1,1);
    app.apiVersion=api_version;
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
    info_={}; host_={}; policy_={}; destroy_=nullptr; app_name_.clear(); return S_OK;
}
}
