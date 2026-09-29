// SPDX-License-Identifier: MIT
#include "hosted-instance.h"
#include <cassert>
#include <cstdio>
#include <thread>

namespace {
int owner_a,owner_b,queue_tag,device_tag;
VkInstance instance_a=reinterpret_cast<VkInstance>(&owner_a);
VkInstance instance_b=reinterpret_cast<VkInstance>(&owner_b);
VkQueue queue=reinterpret_cast<VkQueue>(&queue_tag);
VkInstance expected_instance{};
void* expected_identity{};
const VkAllocationCallbacks* expected_allocator{};
VkBaseInStructure original_chain{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr};
unsigned creates{},destroys{},devices{},binds{},unbinds{};
bool fail_create{},omit_bind{},recursive_create{};
PFN_vkGetInstanceProcAddr scoped_get{};
int32_t dispatch(void* data,uint32_t operation,void*) {
    assert(data==expected_identity && operation==BC250_HOST_CHECK_STATUS);return 0;
}
int32_t bind(void* vk_queue,void* cookie) {
    assert(vk_queue==queue);assert(cookie==nullptr || cookie==expected_identity);++binds;return VK_SUCCESS;
}
int32_t unbind(void* vk_queue) {assert(vk_queue==queue);++unbinds;return VK_ERROR_UNKNOWN;}
VKAPI_ATTR VkResult VKAPI_CALL create(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* allocator,
        VkInstance* out) {
    ++creates;assert(allocator==expected_allocator);
    assert(info->enabledExtensionCount==1 && !std::strcmp(info->ppEnabledExtensionNames[0],"engine-extension"));
    auto host=static_cast<const bc250_host*>(info->pNext);
    assert(host->sType==BC250_HOST_STYPE && host->identity==expected_identity && host->userdata==expected_identity);
    assert(host->adapter_luid==42 && host->dispatch(host->userdata,BC250_HOST_CHECK_STATUS,nullptr)==0);
    auto binding=static_cast<const bc250_host_queue_binding*>(host->pNext);
    assert(binding->sType==BC250_HOST_QUEUE_BINDING_STYPE && binding->version==BC250_HOST_QUEUE_BINDING_VERSION);
    // No adapter-query structure is injected. Each owner's policy is its own.
    auto policy=static_cast<const bc250_host_policy*>(binding->pNext);
    assert(policy->sType==BC250_HOST_POLICY_STYPE && policy->version==BC250_HOST_POLICY_VERSION);
    assert(policy->size==sizeof(bc250_host_policy) && policy->size==32 && !policy->reserved);
    assert(policy->flags==(expected_identity==&owner_a?BC250_HOST_POLICY_SPARSE:0u));
    assert(policy->pNext==&original_chain);
    assert(binding->funcs->size==sizeof(bc250_host_queue_funcs));
    binding->funcs->bind=omit_bind?nullptr:bind;binding->funcs->unbind=unbind;
    if(recursive_create) {
        auto make=reinterpret_cast<PFN_vkCreateInstance>(scoped_get(VK_NULL_HANDLE,"vkCreateInstance"));
        VkInstance nested=instance_b;
        assert(make(info,allocator,&nested)==VK_ERROR_INITIALIZATION_FAILED && !nested);
    }
    if(fail_create) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out=expected_instance;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy(VkInstance instance,const VkAllocationCallbacks* allocator) {
    assert(instance==expected_instance && allocator==expected_allocator);++destroys;
}
VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice,const VkDeviceCreateInfo*,
        const VkAllocationCallbacks*,VkDevice* out) {
    ++devices;*out=reinterpret_cast<VkDevice>(&device_tag);return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL raw(VkInstance instance,const char* name) {
    assert(!instance || instance==expected_instance);
    if(!std::strcmp(name,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(create);
    if(!std::strcmp(name,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    if(!std::strcmp(name,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(create_device);
    return nullptr;
}
using Bootstrap=native12::HostedInstanceBootstrap;
}
int main() {
    Bootstrap a(raw,42,&owner_a,&owner_a,dispatch,BC250_HOST_POLICY_SPARSE),b(raw,42,&owner_b,&owner_b,dispatch,0);
    assert(a.closed() && !a.entry() && !a.queue_functions_ready());
    const char* extension="engine-extension";
    VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pNext=&original_chain;
    ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&extension;
    VkAllocationCallbacks allocator{};expected_allocator=&allocator;
    expected_identity=&owner_a;expected_instance=instance_a;
    PFN_vkDestroyInstance saved_destroy{};
    {
        Bootstrap::Scope scope(a);assert(scope.entered());scoped_get=scope.entry();
        {
            Bootstrap::Scope same(a);assert(same.entered() && same.entry()==scoped_get);
            Bootstrap::Scope other(b);assert(!other.entered() && !other.entry() && !b.entry());
        }
        assert(a.entry()==scoped_get);
        std::thread foreign([&] {assert(!a.entry() && !scoped_get(VK_NULL_HANDLE,"vkCreateInstance"));});
        foreign.join();
        auto make=reinterpret_cast<PFN_vkCreateInstance>(scoped_get(VK_NULL_HANDLE,"vkCreateInstance"));
        VkInstance instance{};recursive_create=true;
        assert(make(&ci,&allocator,&instance)==VK_SUCCESS && instance==instance_a);
        recursive_create=false;
        assert(ci.pNext==&original_chain && a.queue_functions_ready() && !a.closed());
        assert(a.violations()==1);
        assert(!scoped_get(instance_b,"vkCreateDevice") && a.violations()==2);
        auto make_device=reinterpret_cast<PFN_vkCreateDevice>(scoped_get(instance,"vkCreateDevice"));
        VkDevice device{};assert(make_device(nullptr,nullptr,nullptr,&device)==VK_SUCCESS && device);
        assert(devices==1);
        assert(a.bind(queue,nullptr)==VK_SUCCESS && a.bind(queue,&owner_a)==VK_SUCCESS);
        assert(a.unbind(queue)==VK_ERROR_UNKNOWN && binds==2 && unbinds==1);
        assert(b.bind(queue,nullptr)==VK_ERROR_INITIALIZATION_FAILED);
        saved_destroy=reinterpret_cast<PFN_vkDestroyInstance>(scoped_get(instance,"vkDestroyInstance"));
        VkInstance duplicate=instance_b;
        assert(make(&ci,&allocator,&duplicate)==VK_ERROR_INITIALIZATION_FAILED && !duplicate);
    }
    assert(!a.entry() && !scoped_get(instance_a,"vkCreateDevice"));
    saved_destroy(instance_a,&allocator);assert(!a.closed() && destroys==0); // no TLS authority
    assert(a.bind(queue,nullptr)==VK_ERROR_INITIALIZATION_FAILED);
    {
        Bootstrap::Scope scope(a);saved_destroy(instance_a,&allocator);
        assert(a.closed() && !a.queue_functions_ready() && destroys==1);
    }
    expected_identity=&owner_b;expected_instance=instance_b;
    {
        Bootstrap::Scope scope(b);auto get=scope.entry();
        auto make=reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE,"vkCreateInstance"));
        VkInstance instance=instance_a;fail_create=true;
        assert(make(&ci,&allocator,&instance)==VK_ERROR_OUT_OF_HOST_MEMORY && !instance && b.closed());
        fail_create=false;omit_bind=true;
        assert(make(&ci,&allocator,&instance)==VK_ERROR_INITIALIZATION_FAILED && !instance && b.closed());
        assert(destroys==2 && !b.queue_functions_ready());omit_bind=false;
        assert(make(&ci,&allocator,&instance)==VK_SUCCESS && instance==instance_b);
        auto drop=reinterpret_cast<PFN_vkDestroyInstance>(get(instance,"vkDestroyInstance"));drop(instance,&allocator);
        assert(b.closed() && destroys==3);
    }
    // Destroying the owner must never destroy an instance that may have children.
    expected_identity=&owner_a;expected_instance=instance_a;
    {
        Bootstrap retained(raw,42,&owner_a,&owner_a,dispatch,BC250_HOST_POLICY_SPARSE);Bootstrap::Scope scope(retained);
        auto make=reinterpret_cast<PFN_vkCreateInstance>(scope.entry()(VK_NULL_HANDLE,"vkCreateInstance"));
        VkInstance instance{};assert(make(&ci,&allocator,&instance)==VK_SUCCESS && !retained.closed());
    }
    assert(destroys==3);destroy(instance_a,&allocator); // fake ICD cleanup, outside the destroyed owner
    assert(destroys==4 && creates==5);
    Bootstrap invalid(raw,0,&owner_a,&owner_a,dispatch,0);Bootstrap::Scope denied(invalid);assert(!denied.entered());
    std::puts("hosted instance: chain, per-owner scopes, device forwarding, queue functions and explicit lifetime pass");
}
