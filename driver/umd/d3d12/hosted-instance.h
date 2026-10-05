// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan_core.h>
#include "bc250_host_bootstrap.h"
#include <atomic>
#include <cstring>

namespace native12 {
// Device-owned bridge. The owner pins the ICD and this object's address until
// the engine has destroyed its instance and all children. No destructor makes
// Vulkan calls. Creation/destruction and queue binding require owner lifetime
// serialization; concurrent ordinary GIPA reads use the same published instance.
class HostedInstanceBootstrap final {
public:
    using Dispatch = decltype(bc250_host::dispatch);
private:
    inline static thread_local HostedInstanceBootstrap* current_{};
    PFN_vkGetInstanceProcAddr real_{};
    std::atomic<VkInstance> instance_{VK_NULL_HANDLE};
    std::atomic<unsigned> violations_{};
    bool changing_{};
    bc250_host host_{};
    bc250_host_queue_funcs queues_{};
    bc250_host_queue_binding binding_{};
    bc250_host_policy policy_{};

    bool valid() const noexcept {
        return real_ && host_.adapter_luid && host_.identity && host_.dispatch;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL create(const VkInstanceCreateInfo* info,
            const VkAllocationCallbacks* allocator, VkInstance* out) {
        if(out) *out=VK_NULL_HANDLE;
        auto* self=current_;
        if(!self || !info || !out) return VK_ERROR_INITIALIZATION_FAILED;
        if(self->changing_ || self->instance()) {
            ++self->violations_;return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto fn=reinterpret_cast<PFN_vkCreateInstance>(self->real_(VK_NULL_HANDLE,"vkCreateInstance"));
        if(!fn) return VK_ERROR_INITIALIZATION_FAILED;
        self->changing_=true;
        self->queues_={};self->queues_.size=sizeof(self->queues_);
        // The engine's original chain and enabled extension list remain intact.
        auto copy=*info;
        self->policy_.pNext=copy.pNext;copy.pNext=&self->host_;
        VkInstance created=VK_NULL_HANDLE;
        VkResult result=fn(&copy,allocator,&created);
        self->policy_.pNext=nullptr;
        if(result==VK_SUCCESS && created) {
            self->instance_.store(created);
            if(!self->queues_.bind || !self->queues_.unbind) {
                // No device can exist yet. Refuse a mismatched bootstrap ABI.
                auto drop=reinterpret_cast<PFN_vkDestroyInstance>(self->real_(created,"vkDestroyInstance"));
                if(drop) {drop(created,allocator);self->instance_.store(VK_NULL_HANDLE);}
                // Missing destroy is retained as an explicit outstanding instance.
                ++self->violations_;result=VK_ERROR_INITIALIZATION_FAILED;
            }
        } else if(result==VK_SUCCESS) {
            ++self->violations_;result=VK_ERROR_INITIALIZATION_FAILED;
        }
        self->changing_=false;
        if(result==VK_SUCCESS) *out=created;
        else if(!self->instance()) self->queues_={};
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL destroy(VkInstance instance,const VkAllocationCallbacks* allocator) {
        auto* self=current_;
        if(!self) return;
        if(!instance) return;
        if(self->changing_ || instance!=self->instance()) {++self->violations_;return;}
        auto fn=reinterpret_cast<PFN_vkDestroyInstance>(self->real_(instance,"vkDestroyInstance"));
        if(!fn) {++self->violations_;return;}
        self->changing_=true;
        fn(instance,allocator);
        self->instance_.store(VK_NULL_HANDLE);self->queues_={};self->changing_=false;
    }
    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get(VkInstance instance,const char* name) {
        auto* self=current_;
        if(!self || !name) return nullptr;
        if(instance && instance!=self->instance()) {++self->violations_;return nullptr;}
        if(!std::strcmp(name,"vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(get);
        if(!std::strcmp(name,"vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(create);
        if(!std::strcmp(name,"vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(destroy);
        // In particular vkCreateDevice is the actual ICD entry, not a refusal.
        return self->real_(instance,name);
    }
public:
    // policy_flags is the adapter's resolved instance policy, the one its capability query was given.
    HostedInstanceBootstrap(PFN_vkGetInstanceProcAddr real,uint64_t luid,void* identity,
            void* userdata,Dispatch dispatch,uint32_t policy_flags) noexcept : real_(real) {
        queues_.size=sizeof(queues_);
        policy_={BC250_HOST_POLICY_STYPE,nullptr,BC250_HOST_POLICY_VERSION,sizeof(policy_),policy_flags,0};
        binding_={BC250_HOST_QUEUE_BINDING_STYPE,&policy_,BC250_HOST_QUEUE_BINDING_VERSION,sizeof(binding_),&queues_};
        host_={BC250_HOST_STYPE,&binding_,BC250_HOST_VERSION,sizeof(host_),luid,identity,userdata,dispatch};
    }
    HostedInstanceBootstrap(const HostedInstanceBootstrap&)=delete;
    HostedInstanceBootstrap& operator=(const HostedInstanceBootstrap&)=delete;
    ~HostedInstanceBootstrap()=default; // The owner must inspect closed() before releasing the ICD/storage.

    class Scope final {
        HostedInstanceBootstrap* owner_{};
        HostedInstanceBootstrap* previous_{};
    public:
        explicit Scope(HostedInstanceBootstrap& owner) noexcept {
            if(!owner.valid() || (current_ && current_!=&owner)) return;
            owner_=&owner;previous_=current_;current_=&owner;
        }
        Scope(const Scope&)=delete;
        Scope& operator=(const Scope&)=delete;
        ~Scope(){if(owner_)current_=previous_;}
        bool entered() const noexcept {return owner_!=nullptr;}
        PFN_vkGetInstanceProcAddr entry() const noexcept {
            return owner_ && current_==owner_ ? get : nullptr;
        }
    };
    PFN_vkGetInstanceProcAddr entry() const noexcept {return current_==this?get:nullptr;}
    VkInstance instance() const noexcept {return instance_.load();}
    bool closed() const noexcept {return instance()==VK_NULL_HANDLE;}
    unsigned violations() const noexcept {return violations_.load();}
    bool queue_functions_ready() const noexcept {return instance() && queues_.bind && queues_.unbind;}
    VkResult bind(VkQueue queue,void* cookie) noexcept {
        if(current_!=this || !queue || !queue_functions_ready()) return VK_ERROR_INITIALIZATION_FAILED;
        return static_cast<VkResult>(queues_.bind(queue,cookie));
    }
    VkResult unbind(VkQueue queue) noexcept {
        if(current_!=this || !queue || !queue_functions_ready()) return VK_ERROR_INITIALIZATION_FAILED;
        return static_cast<VkResult>(queues_.unbind(queue));
    }
};
}
