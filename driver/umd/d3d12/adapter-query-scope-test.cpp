// SPDX-License-Identifier: MIT
#include "adapter-query-scope.h"
#include <cassert>
#include <cstdio>
namespace {
unsigned creates{},destroys{},forwarded{};
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
    assert(query->sType==BC250_HOST_ADAPTER_QUERY_STYPE && query->version==1 && query->pNext==original_chain);
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
      native12::AdapterQueryScope scope(get,42);assert(scope.entered());
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
    }
    FreeLibrary(module);
    std::puts("actual ICD: adapter-query chain accepted; missing host rejected; no enumeration or device");
  }
    native12::AdapterQueryScope scope(raw,42);assert(scope.entered());
    native12::AdapterQueryScope nested(raw,42);assert(!nested.entered());
    auto gipa=scope.entry();const char* extension="engine-extension";
    VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pNext=original_chain;
    ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&extension;
    VkInstance instance{};
    auto make=reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE,"vkCreateInstance"));
    assert(make(&ci,nullptr,&instance)==VK_SUCCESS && !scope.completed());
    assert(ci.pNext==original_chain);assert(!gipa(instance,"unknown-entry"));assert(forwarded==1);
    auto drop=reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance,"vkDestroyInstance"));drop(instance,nullptr);
    assert(scope.completed() && creates==1 && destroys==1);
    std::puts("adapter query scope: engine chain preserved, bound/query modes injected, lifetime closed");
}
