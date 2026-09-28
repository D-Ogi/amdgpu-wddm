// SPDX-License-Identifier: MIT
#include "device-engine.h"
#include "device-state.h"
#include "adapter-caps.h"
#include "hosted-dispatch.h"
#include "hosted-instance.h"
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <new>

namespace native12 {
namespace {
void stage(const char* name,HRESULT result) noexcept {
    std::fprintf(stderr,"d3d12-engine %s result=%08lx\n",name,static_cast<unsigned long>(result));
    std::fflush(stderr);
}
HRESULT vk_result(VkResult result) noexcept {
    switch(result){
    case VK_SUCCESS:return S_OK;
    case VK_ERROR_OUT_OF_HOST_MEMORY:case VK_ERROR_OUT_OF_DEVICE_MEMORY:return E_OUTOFMEMORY;
    case VK_ERROR_DEVICE_LOST:return DXGI_ERROR_DEVICE_REMOVED;
    default:return E_FAIL;
    }
}
}
class DeviceEngine final {
    Adapter& adapter_;
    AdapterEngineAccess access_;
    bc250::umd::RuntimeDomain domain_;
    HostedDispatch dispatch_;
    HostedInstanceBootstrap bootstrap_;
    ID3D12Device* engine_{};
    BC250_VKD3D_SHELL_SERVICES services_{};
    bool binding_failed_{};
    UINT64 callback_sequence_{};
    static int32_t dispatch(void* owner,uint32_t operation,void* argument) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        const auto sequence=++self.callback_sequence_;
        LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
        // Start/end pairs reveal an unfinished callback without stopping the
        // kernel. IDs are local to this device; no handles/private payload.
        std::fprintf(stderr,"{\"event\":\"hosted-callback\",\"edge\":\"begin\",\"sequence\":%llu,\"op\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            sequence,operation,start.QuadPart,GetCurrentThreadId());std::fflush(stderr);
        auto result=HostedDispatch::dispatch(&self.dispatch_,operation,argument);
        QueryPerformanceCounter(&end);
        std::fprintf(stderr,"{\"event\":\"hosted-callback\",\"edge\":\"end\",\"sequence\":%llu,\"op\":%u,\"qpc\":%lld,\"status\":\"%08x\"}\n",
            sequence,operation,end.QuadPart,static_cast<unsigned>(result));
        std::fflush(stderr);return result;
    }
    static HRESULT APIENTRY bind(void* owner,void* cookie,VkQueue queue) {
        auto& self=*static_cast<DeviceEngine*>(owner);
        HRESULT result=vk_result(self.bootstrap_.bind(queue,cookie));
        stage("BindQueue",result);return result;
    }
    static void APIENTRY unbind(void* owner,void*,VkQueue queue) {
        auto& self=*static_cast<DeviceEngine*>(owner);
        HRESULT result=vk_result(self.bootstrap_.unbind(queue));
        if(FAILED(result))self.binding_failed_=true;
        stage("UnbindQueue",result);
    }
public:
    DeviceEngine(Device& device,const AdapterEngineAccess& access) noexcept
      :adapter_(*device.adapter),access_(access),
       dispatch_(domain_,device.runtime,device.callbacks,device.kernel_callbacks),
       bootstrap_(access.driver_entry,device.adapter->contract.luid,this,this,dispatch) {
        services_={sizeof(services_),this,bind,unbind};
    }
    HRESULT open() noexcept {
        bc250::umd::RuntimeDomain::Scope runtime(domain_);
        HostedInstanceBootstrap::Scope hosted(bootstrap_);
        if(!hosted.entered())return E_UNEXPECTED;
        BC250_VKD3D_DEVICE_CREATE_INFO info{};
        info.Size=sizeof(info);info.AbiVersion=0x10002;
        info.GetInstanceProcAddr=hosted.entry();
        std::memcpy(&info.AdapterLuid,&adapter_.contract.luid,sizeof(info.AdapterLuid));
        info.MinimumFeatureLevel=D3D_FEATURE_LEVEL_11_0;
        info.QueueMode=BC250_VKD3D_QUEUE_MODE_INLINE;info.Services=&services_;
        info.InstanceMode=BC250_VKD3D_INSTANCE_MODE_PRIVATE;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        std::fprintf(stderr,"{\"event\":\"clock\",\"frequency\":%lld}\n",frequency.QuadPart);
        stage("CreateDevice-begin",S_OK);
        HRESULT result=access_.functions.CreateDevice(&info,__uuidof(ID3D12Device),reinterpret_cast<void**>(&engine_));
        stage("CreateDevice-end",result);
        if(result!=S_OK)return FAILED(result)?result:E_UNEXPECTED;
        if(!engine_ || !bootstrap_.instance() || dispatch_.lost())return E_UNEXPECTED;
        VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};UINT32 family{};
        result=access_.functions.GetVulkanHandles(engine_,&instance,&physical,&device,&family);
        if(result==S_OK && (instance!=bootstrap_.instance() || !physical || !device))result=E_UNEXPECTED;
        stage("GetVulkanHandles",result);
        return result==S_OK?S_OK:FAILED(result)?result:E_UNEXPECTED;
    }
    bool close() noexcept {
        bc250::umd::RuntimeDomain::Scope runtime(domain_);
        HostedInstanceBootstrap::Scope hosted(bootstrap_);
        if(!hosted.entered())return false;
        if(engine_){
            auto engine=engine_;engine_=nullptr;
            const ULONG references=engine->Release();
            stage("ReleaseDevice",references?E_UNEXPECTED:S_OK);
            if(references)return false;
        }
        const bool closed=bootstrap_.closed();
        const unsigned unresolved=dispatch_.discard_metadata();
        std::fprintf(stderr,"d3d12-engine teardown instance_closed=%u binding_failed=%u unresolved=%u\n",
            unsigned(closed),unsigned(binding_failed_),unresolved);std::fflush(stderr);
        // Retain owner/code on uncertain unbind even if the engine discarded its instance.
        return closed && !binding_failed_ && unresolved==0;
    }
    void retain() noexcept {
        // Callback authority expires with the runtime Device. Keep storage/code
        // alive, but make any late callback fail before invoking that runtime.
        dispatch_.discard_metadata();
        ++adapter_.retained_engines;
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&stage),&pinned);
        stage("Owner-retained",E_UNEXPECTED);
    }
};
HRESULT create_device_engine(Device& device) noexcept {
    if(device.engine)return E_UNEXPECTED;
    AdapterEngineAccess access{};
    HRESULT result=get_adapter_engine(*device.adapter,&access);
    stage("AdapterEngine",result);if(FAILED(result))return result;
    auto owner=new(std::nothrow) DeviceEngine(device,access);if(!owner)return E_OUTOFMEMORY;
    result=owner->open();
    if(result==S_OK){device.engine=owner;return S_OK;}
    if(owner->close())delete owner;else owner->retain();
    return result;
}
void destroy_device_engine(Device& device) noexcept {
    auto owner=device.engine;device.engine=nullptr;if(!owner)return;
    if(owner->close())delete owner;else owner->retain();
}
}
