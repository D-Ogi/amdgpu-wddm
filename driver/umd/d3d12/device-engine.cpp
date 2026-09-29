// SPDX-License-Identifier: MIT
#include "device-engine.h"
#include "device-state.h"
#include "ddi-trace.h"
#include "adapter-caps.h"
#include "hosted-dispatch.h"
#include "hosted-instance.h"
#include "hosted-queue.h"
#include "heap-import.h"
#include "engine-ddi/engine-ddi.h"
#include <d3dkmthk.h>
#include <memory>
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
    friend class DeviceEngineScope;
    Device& device_;
    Adapter& adapter_;
    AdapterEngineAccess access_;
    bc250::umd::RuntimeDomain domain_;
    HostedDispatch dispatch_;
    HostedInstanceBootstrap bootstrap_;
    ID3D12Device* engine_{};
    BC250_VKD3D_SHELL_SERVICES services_{};
    engine_ddi::DeviceContext* context_{};
    std::unique_ptr<RuntimeHeapImports> imports_;
    std::unique_ptr<QueueEngineRegistry> queues_;
    std::unique_ptr<HostedQueue> queue_bridge_;
    CRITICAL_SECTION entry_lock_{};
    bool lock_ready_{},active_{true};
    bool binding_failed_{};
    UINT64 callback_sequence_{};
    static int32_t dispatch(void* owner,uint32_t operation,void* argument) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        if(!self.active_)return static_cast<int32_t>(0xc000000du);
        const auto sequence=++self.callback_sequence_;
        LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
        // Start/end pairs reveal an unfinished callback without stopping the
        // kernel. IDs are local to this device; no handles/private payload.
        std::fprintf(stderr,"{\"event\":\"hosted-callback\",\"edge\":\"begin\",\"sequence\":%llu,\"op\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            sequence,operation,start.QuadPart,GetCurrentThreadId());std::fflush(stderr);
        int32_t result;
        bool heap=false;
        if(self.imports_ && argument && (operation==BC250_HOST_Lock2 || operation==BC250_HOST_Unlock2)){
            const auto allocation=operation==BC250_HOST_Lock2?static_cast<D3DKMT_LOCK2*>(argument)->hAllocation:
                static_cast<D3DKMT_UNLOCK2*>(argument)->hAllocation;
            heap=self.imports_->owns_allocation(allocation);
        }
        if(heap){
            const HRESULT hr=self.imports_->dispatch(operation,argument);
            result=hr==S_OK?0:static_cast<int32_t>(hr==E_OUTOFMEMORY?0xc0000017u:hr==E_INVALIDARG?0xc000000du:0xc0000001u);
        } else result=HostedDispatch::dispatch(&self.dispatch_,operation,argument);
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
    static HRESULT queue_dispatch(void* owner,uint32_t op,void* arg) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        return self.active_ && self.queue_bridge_?HostedQueue::dispatch(self.queue_bridge_.get(),op,arg):E_NOTIMPL;
    }
    static HRESULT checked_close(engine_ddi::EngineQueue** queue) noexcept {
        if(!queue || !*queue)return E_INVALIDARG;
        auto owned=*queue;*queue=nullptr;
        return engine_ddi::destroy_engine_queue(owned)==engine_ddi::QueueClose::Retired?S_OK:DXGI_ERROR_DEVICE_REMOVED;
    }
    static HRESULT health(void* owner) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        return self.active_ && self.engine_?self.engine_->GetDeviceRemovedReason():DXGI_ERROR_DEVICE_REMOVED;
    }
    static void APIENTRY report_error(void* shell,HRESULT hr) {
        report_device_error(*static_cast<Device*>(shell),hr);
    }
    static void APIENTRY report_list_error(void* shell,D3D12DDI_HRTCOMMANDLIST list,HRESULT hr) {
        auto& device=*static_cast<Device*>(shell);
        if(device_engine_entered(device) && device.callbacks.pfnSetCommandListErrorCb)
            device.callbacks.pfnSetCommandListErrorCb(list,hr);
        else report_device_error(device,hr);
    }
    static BOOL APIENTRY is_lost(void* shell) {
        auto& device=*static_cast<Device*>(shell);
        return !device.engine || device.lost.load() || device.engine->dispatch_.lost();
    }
    static HRESULT APIENTRY bind_list(void* shell,D3D12DDI_HRTCOMMANDLIST list,uint32_t index) {
        auto& device=*static_cast<Device*>(shell);
        if(index>1 || !list.handle || !device_engine_entered(device) || !device.callbacks.pfnSetCommandListDDITableCb)
            return E_INVALIDARG;
        AcquireSRWLockShared(&device.adapter->tables_lock);
        auto table=device.adapter->list_tables[index];
        ReleaseSRWLockShared(&device.adapter->tables_lock);
        if(!table.handle)return E_UNEXPECTED;
        device.callbacks.pfnSetCommandListDDITableCb(list,table);return S_OK;
    }
    static HRESULT APIENTRY allocate(void* shell,const engine_ddi::MemoryRequest* request,engine_ddi::ImportedMemory* memory) {
        const auto trace=ddi_trace_begin("shellAllocateMemory");
        auto& device=*static_cast<Device*>(shell);
        const HRESULT hr=device.engine && device.engine->imports_?device.engine->imports_->allocate(request,memory):E_UNEXPECTED;
        if(trace && request && device.engine && device.engine->imports_){
            // What was asked and where the import ended; the stage is where it returned, not a cause.
            const auto& r=device.engine->imports_->last_report();
            std::fprintf(stderr,"{\"event\":\"shell-memory-request\",\"flags\":%u,\"bytes\":%llu,\"alignment\":%llu,"
                "\"memory_type_bits\":%u,\"row_pitch\":%u,\"layout_size\":%llu,\"stage\":%u,\"memory_type\":%u,"
                "\"held\":%llu,\"address_alignment\":%llu,\"address\":%llu,\"status\":\"%08lx\"}\n",
                request->flags,static_cast<unsigned long long>(request->byte_size),
                static_cast<unsigned long long>(request->alignment),request->memory_type_bits,request->surface_row_pitch,
                static_cast<unsigned long long>(request->surface_layout_size),static_cast<unsigned>(r.stage),r.memory_type,
                static_cast<unsigned long long>(r.bytes),static_cast<unsigned long long>(r.alignment),
                static_cast<unsigned long long>(r.address),static_cast<unsigned long>(hr));
        }
        ddi_trace_end("shellAllocateMemory",trace,hr);
        return hr;
    }
    static HRESULT APIENTRY free(void* shell,const engine_ddi::ImportedMemory* memory) {
        const auto trace=ddi_trace_begin("shellFreeMemory");
        auto& device=*static_cast<Device*>(shell);
        const HRESULT hr=device.engine && device.engine->imports_?device.engine->imports_->free(memory):E_UNEXPECTED;
        if(trace && device.engine && device.engine->imports_){
            const auto& r=device.engine->imports_->last_free_report();
            std::fprintf(stderr,"{\"event\":\"shell-memory-free\",\"surface\":%u,\"stage\":%u,\"status\":\"%08lx\"}\n",
                unsigned(r.surface),static_cast<unsigned>(r.stage),static_cast<unsigned long>(hr));
        }
        ddi_trace_end("shellFreeMemory",trace,hr);
        return hr;
    }
public:
    DeviceEngine(Device& device,const AdapterEngineAccess& access) noexcept
      :device_(device),adapter_(*device.adapter),access_(access),
       dispatch_(domain_,device.runtime,device.callbacks,device.kernel_callbacks,{this,nullptr,queue_dispatch}),
       bootstrap_(access.driver_entry,device.adapter->contract.luid,this,this,dispatch) {
        services_={sizeof(services_),this,bind,unbind};
        lock_ready_=InitializeCriticalSectionEx(&entry_lock_,0,0)!=FALSE;
    }
    ~DeviceEngine(){if(lock_ready_)DeleteCriticalSection(&entry_lock_);}
    engine_ddi::DeviceContext* context() const noexcept {return context_;}
    QueueEngineRegistry* queues() const noexcept {return queues_.get();}
    bool entered() const noexcept {return active_ && domain_.entered() && bootstrap_.entry()!=nullptr;}
    HRESULT open() noexcept {
        if(!lock_ready_)return E_OUTOFMEMORY;
        bc250::umd::RuntimeDomain::Scope runtime(domain_);
        HostedInstanceBootstrap::Scope hosted(bootstrap_);
        if(!hosted.entered())return E_UNEXPECTED;
        BC250_VKD3D_DEVICE_CREATE_INFO info{};
        info.Size=sizeof(info);info.AbiVersion=BC250_VKD3D_ENGINE_ABI_VERSION;
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
        if(result!=S_OK)return FAILED(result)?result:E_UNEXPECTED;
        imports_.reset(new(std::nothrow) RuntimeHeapImports(device_,domain_,physical,device,instance,hosted.entry(),this));
        if(!imports_)return E_OUTOFMEMORY;
        result=imports_->initialize();stage("HeapImports",result);if(result!=S_OK)return result;
        engine_ddi::ContextCreateInfo context{};
        context.size=sizeof(context);context.boundary_revision=engine_ddi::kBoundaryRevision;
        context.memory_mode=engine_ddi::MemoryMode::RuntimeBacked;
        context.ddi_interface=D3D12DDI_INTERFACE_VERSION_R8;context.ddi_version=D3D12DDI_BUILD_VERSION_0092<<16;
        context.engine_device=engine_;context.engine_funcs=&access_.functions;
        context.hooks={sizeof(context.hooks),&device_,report_error,report_list_error,is_lost,bind_list,allocate,free};
        result=engine_ddi::create_device_context(&context,&context_);stage("DeviceContext",result);
        if(result!=S_OK)return result;
        auto ops=QueueEngineOps::native();ops.close=checked_close;ops.check_health=health;ops.health_cookie=this;
        queues_.reset(new(std::nothrow) QueueEngineRegistry(device_,context_,ops));
        if(!queues_)return E_OUTOFMEMORY;
        queue_bridge_.reset(new(std::nothrow) HostedQueue(domain_,*queues_,device_));
        return queue_bridge_?S_OK:E_OUTOFMEMORY;
    }
    bool close() noexcept {
        bc250::umd::RuntimeDomain::Scope runtime(domain_);
        HostedInstanceBootstrap::Scope hosted(bootstrap_);
        if(!hosted.entered())return false;
        if(queues_){unsigned unresolved=0;
            HRESULT hr=queues_->discard_retired_metadata(unresolved);stage("QueueMetadata",hr);
            if(hr!=S_OK || unresolved)return false;
        }
        if(context_){
            uint32_t live=0;HRESULT hr=engine_ddi::destroy_device_context(context_,&live);
            std::fprintf(stderr,"d3d12-engine DeviceContext-close result=%08lx live=%u\n",static_cast<unsigned long>(hr),live);
            std::fflush(stderr);if(hr!=S_OK)return false;context_=nullptr;
        }
        if(imports_){HRESULT hr=imports_->close_after_engine_retirement();stage("HeapImports-close",hr);if(hr!=S_OK)return false;}
        if(engine_){
            auto engine=engine_;engine_=nullptr;
            const ULONG references=engine->Release();
            stage("ReleaseDevice",references?E_UNEXPECTED:S_OK);
            if(references)return false;
        }
        const bool closed=bootstrap_.closed();
        unsigned unresolved=dispatch_.discard_metadata();
        if(queue_bridge_)unresolved+=queue_bridge_->discard_metadata();
        active_=false;
        std::fprintf(stderr,"d3d12-engine teardown instance_closed=%u binding_failed=%u unresolved=%u\n",
            unsigned(closed),unsigned(binding_failed_),unresolved);std::fflush(stderr);
        // Retain owner/code on uncertain unbind even if the engine discarded its instance.
        return closed && !binding_failed_ && unresolved==0;
    }
    void retain() noexcept {
        // Callback authority expires with the runtime Device. Keep storage/code
        // alive, but make any late callback fail before invoking that runtime.
        active_=false;dispatch_.discard_metadata();
        if(imports_)imports_->discard_metadata();
        if(queue_bridge_)queue_bridge_->discard_metadata();
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
    device.engine=owner;
    result=owner->open();
    if(result==S_OK)return S_OK;
    if(owner->close())delete owner;else owner->retain();
    device.engine=nullptr;return result;
}
void destroy_device_engine(Device& device) noexcept {
    auto owner=device.engine;if(!owner)return;
    if(owner->close())delete owner;else owner->retain();
    device.engine=nullptr;
}
engine_ddi::DeviceContext* engine_context(Device& device) noexcept {return device.engine?device.engine->context():nullptr;}
QueueEngineRegistry* engine_queues(Device& device) noexcept {return device.engine?device.engine->queues():nullptr;}
bool device_engine_entered(Device& device) noexcept {return device.engine && device.engine->entered();}
void report_device_error(Device& device,HRESULT hr) noexcept {
    stage("DDI-error",hr);
    if(hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_HUNG || hr==DXGI_ERROR_DEVICE_RESET || hr==D3DDDIERR_DEVICEREMOVED)
        device.lost.store(true);
    if(device_engine_entered(device) && device.callbacks.pfnSetErrorCb)device.callbacks.pfnSetErrorCb(device.runtime,hr);
}
DeviceEngineScope::DeviceEngineScope(Device& device) noexcept {
    auto owner=device.engine;
    if(!owner || !owner->active_ || !owner->lock_ready_ || (current_ && current_!=owner))return;
    EnterCriticalSection(&owner->entry_lock_);
    runtime_.emplace(owner->domain_);hosted_.emplace(owner->bootstrap_);
    if(!hosted_->entered()){hosted_.reset();runtime_.reset();LeaveCriticalSection(&owner->entry_lock_);return;}
    owner_=owner;previous_=current_;current_=owner;
}
DeviceEngineScope::~DeviceEngineScope() noexcept {
    if(!owner_)return;
    hosted_.reset();runtime_.reset();current_=previous_;LeaveCriticalSection(&owner_->entry_lock_);
}
}
