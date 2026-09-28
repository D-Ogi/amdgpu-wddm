// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include "bc250_host_bootstrap.h"
#include <cstring>
namespace native12 {
// One synchronous QueryAdapterCaps call. The engine must destroy its temporary
// instance before returning. Nothing from this scope can be used by a device.
class AdapterQueryScope final {
    inline static thread_local AdapterQueryScope* current_{};
    PFN_vkGetInstanceProcAddr real_{};
    VkInstance instance_{};
    VkAllocationCallbacks allocator_{};
    bool custom_allocator_{};
    bc250_host host_{};
    bc250_host_queue_funcs queue_functions_{};
    bc250_host_queue_binding binding_{};
    bc250_host_adapter_query query_{};
    unsigned forbidden_{};
    bool entered_{};
    bool created_{};
    static int32_t dispatch(void* data,uint32_t operation,void*) noexcept {
        auto& scope=*static_cast<AdapterQueryScope*>(data);
        if(operation==BC250_HOST_CHECK_STATUS)return 0;
        ++scope.forbidden_;
        return static_cast<int32_t>(0xC0000002u); // STATUS_NOT_IMPLEMENTED; no runtime device exists.
    }
    static VKAPI_ATTR VkResult VKAPI_CALL create(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator,VkInstance* out) {
        if(!current_ || !info || !out)return VK_ERROR_INITIALIZATION_FAILED;
        *out=VK_NULL_HANDLE;
        auto& scope=*current_;
        if(scope.instance_){++scope.forbidden_;return VK_ERROR_INITIALIZATION_FAILED;}
        auto real=reinterpret_cast<PFN_vkCreateInstance>(scope.real_(VK_NULL_HANDLE,"vkCreateInstance"));
        if(!real)return VK_ERROR_INITIALIZATION_FAILED;
        // Preserve the engine's extension list and pNext chain.
        auto copy=*info;scope.query_.pNext=copy.pNext;copy.pNext=&scope.host_;
        VkResult result=real(&copy,allocator,out);
        if(result==VK_SUCCESS) {
            if(!*out){++scope.forbidden_;return VK_ERROR_INITIALIZATION_FAILED;}
            scope.created_=true;
            scope.instance_=*out;scope.custom_allocator_=allocator!=nullptr;
            if(allocator)scope.allocator_=*allocator;
        }
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy(VkInstance instance,const VkAllocationCallbacks* allocator) {
        if(!current_)return;
        auto& scope=*current_;
        if(instance!=scope.instance_){++scope.forbidden_;return;}
        auto real=reinterpret_cast<PFN_vkDestroyInstance>(scope.real_(instance,"vkDestroyInstance"));
        if(!real){++scope.forbidden_;return;}
        real(instance,allocator);scope.instance_=VK_NULL_HANDLE;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL refuse_device(VkPhysicalDevice,const VkDeviceCreateInfo*,
        const VkAllocationCallbacks*,VkDevice* out) {
        if(out)*out=VK_NULL_HANDLE;
        if(current_)++current_->forbidden_;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get(VkInstance instance,const char* name) {
        if(!current_ || !name)return nullptr;
        if(!std::strcmp(name,"vkGetInstanceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(get);
        if(!std::strcmp(name,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(create);
        if(!std::strcmp(name,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(destroy);
        if(!std::strcmp(name,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(refuse_device);
        return current_->real_(instance,name);
    }
public:
    AdapterQueryScope(PFN_vkGetInstanceProcAddr real,UINT64 luid) noexcept:real_(real) {
        if(current_ || !real || !luid)return;
        query_={BC250_HOST_ADAPTER_QUERY_STYPE,nullptr,BC250_HOST_ADAPTER_QUERY_VERSION,sizeof(query_)};
        queue_functions_.size=sizeof(queue_functions_);
        binding_={BC250_HOST_QUEUE_BINDING_STYPE,&query_,BC250_HOST_QUEUE_BINDING_VERSION,sizeof(binding_),&queue_functions_};
        host_={BC250_HOST_STYPE,&binding_,BC250_HOST_VERSION,sizeof(host_),luid,this,this,dispatch};
        current_=this;entered_=true;
    }
    AdapterQueryScope(const AdapterQueryScope&)=delete;
    AdapterQueryScope& operator=(const AdapterQueryScope&)=delete;
    ~AdapterQueryScope() {
        if(!entered_)return;
        // Query-only instances cannot have children. Defensive cleanup for a
        // failed engine query; completed() still refuses a leaked instance.
        if(instance_)destroy(instance_,custom_allocator_?&allocator_:nullptr);
        current_=nullptr;
    }
    bool entered() const noexcept{return entered_;}
    bool completed() const noexcept{return entered_ && created_ && !instance_ && !forbidden_;}
    PFN_vkGetInstanceProcAddr entry() const noexcept{return entered_?get:nullptr;}
};
}
