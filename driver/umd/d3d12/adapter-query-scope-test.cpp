// SPDX-License-Identifier: MIT
#include "adapter-query-scope.h"
#include <cassert>
#include <cstdio>
namespace {
unsigned creates{},destroys{},forwarded{};
uint32_t expected_policy{};
int32_t healthy(void*,uint32_t operation,void*){return operation==BC250_HOST_CHECK_STATUS?0:static_cast<int32_t>(0xC0000002u);}
int identity;
VkInstance fake_instance=reinterpret_cast<VkInstance>(&identity);
const void* original_chain=reinterpret_cast<const void*>(0x2000);
VKAPI_ATTR VkResult VKAPI_CALL create(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks*,VkInstance* out) {
    assert(ci->enabledExtensionCount==1 && !std::strcmp(ci->ppEnabledExtensionNames[0],"engine-extension"));
    auto host=static_cast<const bc250_host*>(ci->pNext);
    assert(host->sType==BC250_HOST_STYPE && host->version==5 && host->adapter_luid==42 && host->identity);
    auto binding=static_cast<const bc250_host_queue_binding*>(host->pNext);
    assert(binding->sType==BC250_HOST_QUEUE_BINDING_STYPE && binding->funcs->size==sizeof(bc250_host_queue_funcs));
    auto query=static_cast<const bc250_host_adapter_query*>(binding->pNext);
    assert(query->sType==BC250_HOST_ADAPTER_QUERY_STYPE && query->version==1);
    auto policy=static_cast<const bc250_host_policy*>(query->pNext);
    assert(policy->sType==BC250_HOST_POLICY_STYPE && policy->version==BC250_HOST_POLICY_VERSION &&
           policy->size==sizeof(bc250_host_policy));
#if BC250_HOST_POLICY_VERSION >= 2
    assert(!policy->specified && !policy->coalesce && !policy->gather_slots && !policy->progress_gpu &&
           !policy->deferred_destroy && !policy->reserved2);
#endif
    assert(policy->flags==expected_policy && !policy->reserved && policy->pNext==original_chain);
    assert(host->dispatch(host->userdata,BC250_HOST_CHECK_STATUS,nullptr)==0);
    *out=fake_instance;++creates;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkInstance instance,const VkAllocationCallbacks*){assert(instance==fake_instance);++destroys;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL raw(VkInstance,const char* name){
    if(!std::strcmp(name,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(create);
    if(!std::strcmp(name,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    ++forwarded;return nullptr;
}
}
int main(int argc,char** argv){
  if(argc==2) {
    auto module=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);assert(module);
    auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vk_icdGetInstanceProcAddr"));assert(get);
    {
      native12::AdapterQueryScope scope(get,42,BC250_HOST_POLICY_SPARSE);assert(scope.entered());
      VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
      VkInstance instance{};
      auto make=reinterpret_cast<PFN_vkCreateInstance>(scope.entry()(VK_NULL_HANDLE,"vkCreateInstance"));
      assert(make(&ci,nullptr,&instance)==VK_SUCCESS && instance);
      auto drop=reinterpret_cast<PFN_vkDestroyInstance>(scope.entry()(instance,"vkDestroyInstance"));
      drop(instance,nullptr);assert(scope.completed());
      bc250_host_adapter_query query{BC250_HOST_ADAPTER_QUERY_STYPE,nullptr,1,sizeof(query)};
      ci.pNext=&query;
      auto direct=reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE,"vkCreateInstance"));
      assert(direct(&ci,nullptr,&instance)==VK_ERROR_INITIALIZATION_FAILED);
      // The policy structure as the ICD's parser reads it. host -> query -> policy, no device.
      int owner{};
      bc250_host_policy second{BC250_HOST_POLICY_STYPE,nullptr,1,sizeof(second),0,0};
      bc250_host_policy policy{BC250_HOST_POLICY_STYPE,nullptr,1,sizeof(policy),0,0};
      query.pNext=&policy;
      bc250_host host{BC250_HOST_STYPE,&query,BC250_HOST_VERSION,sizeof(host),42,&owner,&owner,healthy};
      ci.pNext=&host;
      const auto made=[&]{
        VkInstance created{};const VkResult result=direct(&ci,nullptr,&created);
        if(result!=VK_SUCCESS){assert(result==VK_ERROR_INITIALIZATION_FAILED && !created);return false;}
        assert(created);
        reinterpret_cast<PFN_vkDestroyInstance>(get(created,"vkDestroyInstance"))(created,nullptr);return true;
      };
      assert(made());                                              // off
      policy.flags=BC250_HOST_POLICY_SPARSE;assert(made());        // on
      policy.flags=2;assert(!made());                              // a bit nobody knows
      policy.flags=BC250_HOST_POLICY_SPARSE|0x80000000u;assert(!made());
      policy.flags=BC250_HOST_POLICY_SPARSE;
      policy.reserved=1;assert(!made());policy.reserved=0;
      policy.version=2;assert(!made());policy.version=0;assert(!made());policy.version=1;
      policy.size=24;assert(!made());policy.size=40;assert(!made());policy.size=sizeof(policy);
      policy.pNext=&second;assert(!made());policy.pNext=nullptr;   // two policies in one chain
      assert(made());
      ci.pNext=&policy;assert(!made());                            // a policy without a host
    }
    FreeLibrary(module);
    std::puts("actual ICD: adapter-query chain accepted; missing host rejected; policy on and off accepted; "
              "unknown flag, reserved, version, size, second policy and missing host rejected; no enumeration or device");
  }
    {
      // Each query of the process carries its own policy: this one off, the next one on.
      expected_policy=0;const char* name="engine-extension";
      native12::AdapterQueryScope off(raw,42,0);assert(off.entered());
      VkInstanceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;info.pNext=original_chain;
      info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&name;
      VkInstance made{};
      auto first=reinterpret_cast<PFN_vkCreateInstance>(off.entry()(VK_NULL_HANDLE,"vkCreateInstance"));
      assert(first(&info,nullptr,&made)==VK_SUCCESS && info.pNext==original_chain);
      reinterpret_cast<PFN_vkDestroyInstance>(off.entry()(made,"vkDestroyInstance"))(made,nullptr);
      assert(off.completed() && creates==1 && destroys==1);
    }
    expected_policy=BC250_HOST_POLICY_SPARSE;
    native12::AdapterQueryScope scope(raw,42,expected_policy);assert(scope.entered());
    // A backend that returned a cached instance without consulting this query's
    // GIPA has not used this adapter authority, even if it reports query success.
    assert(!scope.completed());
    native12::AdapterQueryScope nested(raw,42,0);assert(!nested.entered());
    auto gipa=scope.entry();const char* extension="engine-extension";
    VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pNext=original_chain;
    ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&extension;
    VkInstance instance{};
    auto make=reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE,"vkCreateInstance"));
    assert(make(&ci,nullptr,&instance)==VK_SUCCESS && !scope.completed());
    assert(ci.pNext==original_chain);assert(!gipa(instance,"unknown-entry"));assert(forwarded==1);
    auto drop=reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance,"vkDestroyInstance"));drop(instance,nullptr);
    assert(scope.completed() && creates==2 && destroys==2);
    std::puts("adapter query scope: engine chain preserved, bound/query modes and the policy injected, lifetime closed");
}
