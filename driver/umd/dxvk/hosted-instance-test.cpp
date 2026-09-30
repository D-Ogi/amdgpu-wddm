// SPDX-License-Identifier: MIT
#include "hosted-instance.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
namespace {
void check(bool ok) { if (!ok) std::abort(); }
int identity, mode; unsigned destroyed, enumerations; uint32_t expected_policy;
constexpr uint64_t luid=0x123456789abcdef0;
int32_t dispatch(void *,uint32_t,void *) { return 0; }
VkResult VKAPI_PTR create(const VkInstanceCreateInfo *c,const VkAllocationCallbacks *,VkInstance *out) {
    const auto *host=static_cast<const bc250_host *>(c->pNext);
    check(host && host->adapter_luid==luid && host->identity==&identity && host->version==5);
    const auto *policy=static_cast<const bc250_host_policy *>(host->pNext);
    check(policy && policy->sType==BC250_HOST_POLICY_STYPE && policy->version==BC250_HOST_POLICY_VERSION &&
          policy->size==sizeof(*policy) && policy->flags==expected_policy && !policy->reserved && !policy->pNext);
    check(c->pApplicationInfo->apiVersion==VK_API_VERSION_1_3);
    if (mode==5) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out=reinterpret_cast<VkInstance>(1); return VK_SUCCESS;
}
void VKAPI_PTR destroy(VkInstance i,const VkAllocationCallbacks *) {
    check(i==reinterpret_cast<VkInstance>(1)); ++destroyed;
}
VkResult VKAPI_PTR enumerate(VkInstance,uint32_t *n,VkPhysicalDevice *devices) {
    ++enumerations;
    if (mode==6) { *n=0; return VK_SUCCESS; }
    if (!devices) { *n=2; return VK_SUCCESS; }
    if (mode==7 || (mode==3 && enumerations==2)) return VK_INCOMPLETE;
    check(*n==2); devices[0]=reinterpret_cast<VkPhysicalDevice>(2);
    devices[1]=reinterpret_cast<VkPhysicalDevice>(3); return VK_SUCCESS;
}
void VKAPI_PTR properties(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *out) {
    auto *id=static_cast<VkPhysicalDeviceIDProperties *>(out->pNext);
    check(id->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    uint64_t selected=(p==reinterpret_cast<VkPhysicalDevice>(3) || mode==4) ? luid : 17;
    if (mode==1) selected=18;
    std::memcpy(id->deviceLUID,&selected,sizeof(selected));
    id->deviceLUIDValid=mode==2 ? VK_FALSE : VK_TRUE;
}
PFN_vkVoidFunction VKAPI_PTR get(VkInstance,const char *name) {
    if (!std::strcmp(name,"vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(create);
    if (!std::strcmp(name,"vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    if (!std::strcmp(name,"vkEnumeratePhysicalDevices")) return reinterpret_cast<PFN_vkVoidFunction>(enumerate);
    if (!std::strcmp(name,"vkGetPhysicalDeviceProperties2")) return reinterpret_cast<PFN_vkVoidFunction>(properties);
    return nullptr;
}
}
int main() {
    bc250_host host{}; host.sType=BC250_HOST_STYPE; host.version=BC250_HOST_VERSION;
    host.size=sizeof(host); host.identity=&identity; host.adapter_luid=luid; host.dispatch=dispatch;
    for (mode=0;mode<8;++mode) {
        RuntimeDomain domain; HostedInstance instance(domain);
        destroyed=0; enumerations=0;
        check(instance.open(get,host)==E_UNEXPECTED);
        RuntimeDomain::Scope scope(domain);
        expected_policy=(mode&1) ? BC250_HOST_POLICY_SPARSE : 0;
        check(instance.open(get,host,2)==E_INVALIDARG);
        const HRESULT hr=instance.open(get,host,expected_policy);
        if (mode==0 || mode==3) {
            check(hr==S_OK && instance.info().PhysicalDevice==reinterpret_cast<VkPhysicalDevice>(3));
            check(!destroyed && instance.info().GetInstanceProcAddr==get);
            check(instance.close()==S_OK && destroyed==1);
        } else {
            check(FAILED(hr) && !instance.info().Instance);
            check(destroyed==(mode==5 ? 0u : 1u));
        }
        if (mode==7) check(enumerations==6);
        const auto count=destroyed;
        check(instance.close()==S_OK && destroyed==count);
    }
    std::cout << "PASS hosted instance: v5 chain with instance policy, exact LUID, invalid/ambiguous identity, enumeration retry and cleanup\n";
}
