// SPDX-License-Identifier: MIT
#include "device-engine.h"
#include "device-state.h"
#include "ddi-trace.h"
#include "adapter-caps.h"
#include "hosted-dispatch.h"
#include "hosted-instance.h"
#include "hosted-queue.h"
#include "heap-import.h"
#include "replay-log.h"
#include "engine-ddi/engine-ddi.h"
#include <d3dkmthk.h>
#include <atomic>
#include <memory>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <new>
#include "stdio-log.h"

namespace native12 {
namespace {
// Bounds of the retire hand-off (L3). A submission still runs the release sequence once this many releases
// are pending, or once no resource DDI has run it for this long, so memory a game destroys and never follows
// with another resource call is still returned. Trial 217 terminated about 0.6 allocations per frame, and its
// resource destroys ran on worker threads that also create; at that rate neither bound should be reached in
// steady state (expected, not measured). A create runs the sequence before it allocates, so an application
// that destroys and then creates at its budget gets the memory back first, as without the hand-off.
constexpr uint32_t kRetireBacklogBound=256;
constexpr uint32_t kRetireAgeBoundMs=250;
// Deferred replay: recording threads with a ring of their own (a game records on a handful), and the bytes of each
// ring. 4 MiB holds a frame of tens of thousands of small calls; a fuller ring makes its thread wait (counted).
constexpr uint32_t kReplayRings=8;
constexpr uint32_t kReplayRingBytes=4u<<20;
void stage(const char* name,HRESULT result) noexcept {
    if(FAILED(result))ddi_failure_note(name,result);
    amdgpu_wddm_log::print("d3d12-engine %s result=%08lx\n",name,static_cast<unsigned long>(result));
    amdgpu_wddm_log::flush();
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
    // No entry lock: DDI entries of this device run at once on several threads (DeviceEngineScope).
    // active_ turns false once, at close or retain, after the runtime has ended every entry.
    std::atomic<bool> active_{true};
    bool replay_{};                             // the deferred-replay policy is on (open, close)
    ReplayLog replay_log_;                      // experiment replay-log: the replay lines' own file (replay-log.h)
    std::atomic<bool> binding_failed_{};
    std::atomic<UINT64> callback_sequence_{};
    // Lever L2 (experiment recording-bind): the binding Device::recording points to while it is published,
    // and whether the experiment is on for this device, read once at open.
    RecordingBinding recording_{};
    std::atomic<bool> recording_bind_{};
    static int32_t dispatch(void* owner,uint32_t operation,void* argument) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        if(!self.active_.load(std::memory_order_acquire))return static_cast<int32_t>(0xc000000du);
        // Start/end pairs reveal an unfinished callback without stopping the
        // kernel. IDs are local to this device; no handles/private payload.
        // Full trace mode only: a game makes thousands of these callbacks per frame, and the
        // unconditional formatting cost 15 % of this module's main-thread samples (native 164).
        // The sequence is counted in that mode only, so that callbacks on several threads do not
        // share a cache line for a number nobody reads.
        // With recording-bind the device's trace mode, read once, replaces the per-call read (L2).
        const bool traced=self.recording_bind_.load(std::memory_order_relaxed)?self.device_.trace_mode==1:
            ddi_trace_enabled();
        const UINT64 sequence=traced?self.callback_sequence_.fetch_add(1,std::memory_order_relaxed)+1:0;
        LARGE_INTEGER start{},end{};
        if(traced){
            QueryPerformanceCounter(&start);
            amdgpu_wddm_log::print("{\"event\":\"hosted-callback\",\"edge\":\"begin\",\"sequence\":%llu,\"op\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
                sequence,operation,start.QuadPart,GetCurrentThreadId());amdgpu_wddm_log::flush();
        }
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
        if(!traced)return result;
        QueryPerformanceCounter(&end);
        amdgpu_wddm_log::print("{\"event\":\"hosted-callback\",\"edge\":\"end\",\"sequence\":%llu,\"op\":%u,\"qpc\":%lld,\"status\":\"%08x\"}\n",
            sequence,operation,end.QuadPart,static_cast<unsigned>(result));
        amdgpu_wddm_log::flush();return result;
    }
    static HRESULT APIENTRY bind(void* owner,void* cookie,VkQueue queue) {
        auto& self=*static_cast<DeviceEngine*>(owner);
        HRESULT result=vk_result(self.bootstrap_.bind(queue,cookie));
        stage("BindQueue",result);return result;
    }
    static void APIENTRY unbind(void* owner,void*,VkQueue queue) {
        auto& self=*static_cast<DeviceEngine*>(owner);
        HRESULT result=vk_result(self.bootstrap_.unbind(queue));
        if(FAILED(result))self.binding_failed_.store(true);
        stage("UnbindQueue",result);
    }
    static bool borrow_backing(void* owner,D3DKMT_HANDLE handle) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        return self.active_.load(std::memory_order_acquire) && self.imports_ && self.imports_->borrow_backing(handle);
    }
    static void return_backing(void* owner,D3DKMT_HANDLE handle) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        if(self.imports_)self.imports_->return_backing(handle);
    }
    static HRESULT queue_dispatch(void* owner,uint32_t op,void* arg) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        return self.active_.load(std::memory_order_acquire) && self.queue_bridge_?
            HostedQueue::dispatch(self.queue_bridge_.get(),op,arg):E_NOTIMPL;
    }
    static HRESULT checked_close(engine_ddi::EngineQueue** queue) noexcept {
        if(!queue || !*queue)return E_INVALIDARG;
        auto owned=*queue;*queue=nullptr;
        return engine_ddi::destroy_engine_queue(owned)==engine_ddi::QueueClose::Retired?S_OK:DXGI_ERROR_DEVICE_REMOVED;
    }
    static HRESULT health(void* owner) noexcept {
        auto& self=*static_cast<DeviceEngine*>(owner);
        return self.active_.load(std::memory_order_acquire) && self.engine_?
            self.engine_->GetDeviceRemovedReason():DXGI_ERROR_DEVICE_REMOVED;
    }
    static void APIENTRY report_error(void* shell,HRESULT hr) {
        report_device_error(*static_cast<Device*>(shell),hr);
    }
    static void APIENTRY report_list_error(void* shell,D3D12DDI_HRTCOMMANDLIST list,HRESULT hr) {
        auto& device=*static_cast<Device*>(shell);
        ddi_failure_note("list-error",hr);
        if(device_engine_entered(device) && device.callbacks.pfnSetCommandListErrorCb)
            device.callbacks.pfnSetCommandListErrorCb(list,hr);
        else report_device_error(device,hr);
    }
    // Deferred replay (engine-ddi set_replay_policy). A worker runs in this device's runtime domain and in the
    // hosted dispatch's worker scope, which admits only the device-level operations a recording call can make.
    static void APIENTRY replay_worker(void* shell,engine_ddi::ReplayBody body,void* ring) {
        auto& self=*static_cast<DeviceEngine*>(shell);
        bc250::umd::RuntimeDomain::Scope runtime(self.domain_);
        HostedDispatch::WorkerScope worker(self.dispatch_);
        body(ring);
    }
    static void APIENTRY replay_drained(void* shell) {static_cast<DeviceEngine*>(shell)->dispatch_.report_deferred_removal();}
    static void APIENTRY replay_log(void* shell,const char* line) {static_cast<DeviceEngine*>(shell)->replay_log_.write(line);}
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
        if(trace && ddi_trace_enabled() && request && device.engine && device.engine->imports_){
            // What was asked and where the import ended; the stage is where it returned, not a cause.
            const auto& r=device.engine->imports_->last_report();
            amdgpu_wddm_log::print("{\"event\":\"shell-memory-request\",\"flags\":%u,\"bytes\":%llu,\"alignment\":%llu,"
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
            // Instrumentation item 5 of the trial 245 report: the address and size the release named, with
            // the time and thread, so that the line meets the kernel's paging journal (which records the
            // unmap and the destroy of the same VA) and the engine's release lines. In the full trace every
            // call prints; in the failures-only mode of the lab's game runs the line goes to the debugger
            // at powers of two of the free count, and whenever the quarantine let something go, so a game
            // adds a line per doubling and one per actual release, not one per frame.
            const auto& r=device.engine->imports_->last_free_report();
            static std::atomic<uint64_t> frees{};
            const uint64_t count=frees.fetch_add(1,std::memory_order_relaxed)+1;
            LARGE_INTEGER now{};QueryPerformanceCounter(&now);
            if(ddi_trace_enabled())
                amdgpu_wddm_log::print("{\"event\":\"shell-memory-free\",\"surface\":%u,\"stage\":%u,\"owner_expired\":%u,"
                    "\"gpu_va\":%llu,\"byte_size\":%llu,\"released\":%u,\"held_count\":%u,\"held_bytes\":%llu,"
                    "\"qpc\":%lld,\"thread\":%lu,\"status\":\"%08lx\"}\n",
                    unsigned(r.surface),static_cast<unsigned>(r.stage),unsigned(r.owner_expired),
                    static_cast<unsigned long long>(r.gpu_va),static_cast<unsigned long long>(r.byte_size),
                    r.released,r.held_count,static_cast<unsigned long long>(r.held_bytes),
                    now.QuadPart,GetCurrentThreadId(),static_cast<unsigned long>(hr));
            else if(ddi_trace_mode()==2 && (r.released || !(count&(count-1)))){
                char line[256];
                std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 shell-memory-free n=%llu va=%llu bytes=%llu "
                    "stage=%u released=%u held=%u/%llu qpc=%lld thread=%lu status=%08lx\n",
                    static_cast<unsigned long long>(count),static_cast<unsigned long long>(r.gpu_va),
                    static_cast<unsigned long long>(r.byte_size),static_cast<unsigned>(r.stage),r.released,
                    r.held_count,static_cast<unsigned long long>(r.held_bytes),now.QuadPart,GetCurrentThreadId(),
                    static_cast<unsigned long>(hr));
                ddi_mode2_note(line);
            }
        }
        ddi_trace_end("shellFreeMemory",trace,hr);
        return hr;
    }
public:
    DeviceEngine(Device& device,const AdapterEngineAccess& access) noexcept
      :device_(device),adapter_(*device.adapter),access_(access),
       dispatch_(domain_,device.runtime,device.callbacks,device.kernel_callbacks,
                 {this,nullptr,queue_dispatch,borrow_backing,return_backing}),
       bootstrap_(access.driver_entry,device.adapter->contract.luid,this,this,dispatch,
                  device.adapter->instance_policy) {
        services_={sizeof(services_),this,bind,unbind};
    }
    // L2: publishes this owner's recording binding in Device::recording when `on` and the device is not in
    // the full trace (mode 1, two lines per call), clears it otherwise. The failures-only mode 2 of the lab's
    // debugger runs keeps the binding: it writes nothing on success, and the fast path notes its own refusals
    // (EntryPolicy::fast_denied). Called once the engine is open, and with false before it closes.
    bool bind_recording(bool on) noexcept {
        recording_={this,&domain_,&bootstrap_,&active_};
        recording_bind_.store(on,std::memory_order_relaxed);
        device_.recording=on && device_.trace_mode!=1 && bootstrap_.valid()?&recording_:nullptr;
        // The entry path experiment's direct entry is admitted with the binding and cleared with it (engine-ddi.h).
        engine_ddi::set_direct_entry(context_,device_.recording!=nullptr);
        return device_.recording!=nullptr;
    }
    engine_ddi::DeviceContext* context() const noexcept {return context_;}
    QueueEngineRegistry* queues() const noexcept {return queues_.get();}
    RuntimeHeapImports* imports() const noexcept {return imports_.get();}
    bool entered() const noexcept {
        return active_.load(std::memory_order_acquire) && domain_.entered() && bootstrap_.entry()!=nullptr;
    }
    HRESULT open() noexcept {
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
        amdgpu_wddm_log::print("{\"event\":\"clock\",\"frequency\":%lld}\n",frequency.QuadPart);
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
        // The release gate's shell half (M15.8, F2 and F3): the device-wide progress of every context comes
        // from the hosted dispatch, which sees every progress publication the ICD makes. Bound before any
        // import exists, so no release can miss it.
        imports_->bind_progress(dispatch_.progress_source());
        {
            const auto& policy=imports_->policy();
            amdgpu_wddm_log::print("d3d12-engine import release policy progress_gate=%u depth=%u count_cap=%u "
                "age_ms=%u byte_cap=%llu\n",unsigned(policy.progress_gate),policy.quarantine_depth,
                policy.quarantine_count_cap,policy.quarantine_age_ms,
                static_cast<unsigned long long>(policy.quarantine_byte_cap));amdgpu_wddm_log::flush();
            char line[192];
            std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 import release policy progress_gate=%u depth=%u "
                "count_cap=%u age_ms=%u byte_cap=%llu\n",unsigned(policy.progress_gate),policy.quarantine_depth,
                policy.quarantine_count_cap,policy.quarantine_age_ms,
                static_cast<unsigned long long>(policy.quarantine_byte_cap));
            ddi_mode2_note(line);
        }
        engine_ddi::ContextCreateInfo context{};
        context.size=sizeof(context);context.boundary_revision=engine_ddi::kBoundaryRevision;
        context.memory_mode=engine_ddi::MemoryMode::RuntimeBacked;
        context.ddi_interface=D3D12DDI_INTERFACE_VERSION_R8;context.ddi_version=D3D12DDI_BUILD_VERSION_0092<<16;
        context.engine_device=engine_;context.engine_funcs=&access_.functions;
        context.hooks={sizeof(context.hooks),&device_,report_error,report_list_error,is_lost,bind_list,allocate,free};
        result=engine_ddi::create_device_context(&context,&context_);stage("DeviceContext",result);
        if(result!=S_OK)return result;
        // The engine-ddi half of the release gate (M15.8, F1): two-phase retirement, on unless the
        // experiment release-two-phase-off says otherwise. Set before any queue exists, like the policy
        // below; engine-ddi's own default is one phase, so the switch alone decides.
        {
            engine_ddi::ReleasePolicy policy{};
            policy.size=sizeof(policy);
            policy.two_phase=ddi_experiment("release-two-phase-off")?0u:1u;
            result=engine_ddi::set_release_policy(context_,&policy);stage("ReleasePolicy",result);
            if(result!=S_OK)return result;
        }
        // Lever L3, on unless the experiment retire-handoff-off says otherwise: submissions leave the release
        // sequence to the resource DDIs, within kRetireBacklogBound and kRetireAgeBoundMs (engine-ddi.h,
        // set_retire_policy). engine-ddi's own default keeps the sequence, so this call alone decides.
        // Set before any queue exists.
        if(!ddi_experiment_off("retire-handoff")){
            engine_ddi::RetirePolicy policy{};
            policy.size=sizeof(policy);policy.handoff=1;
            policy.backlog_bound=kRetireBacklogBound;policy.age_bound_ms=kRetireAgeBoundMs;
            result=engine_ddi::set_retire_policy(context_,&policy);stage("RetireHandoff",result);
            if(result!=S_OK)return result;
        }
        // Deferred replay, on unless the experiment deferred-replay-off says otherwise: recording calls go to
        // a ring of the recording thread, and a worker thread per ring makes the engine calls (engine-ddi.h,
        // set_replay_policy). Off, neither exists. Set before any queue or list exists. With the experiment
        // replay-log, the replay lines also go to a file of their own (replay-log.h), for a game whose stderr
        // nobody reads.
        if(!ddi_experiment_off("deferred-replay")){
            engine_ddi::ReplayPolicy policy{};
            policy.size=sizeof(policy);policy.enabled=1;policy.rings=kReplayRings;policy.ring_bytes=kReplayRingBytes;
            policy.shell=this;policy.worker=replay_worker;policy.drained=replay_drained;
            if(ddi_experiment("replay-log") && replay_log_.open_for_process())policy.log=replay_log;
            result=engine_ddi::set_replay_policy(context_,&policy);stage("DeferredReplay",result);
            if(result!=S_OK)return result;
            replay_=true;
            ddi_mode2_note("amdgpu_wddm_d3d12 deferred-replay on\n");
        }
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
        // The replay workers drain and stop first: their pending calls use the engine, the context and the dispatch.
        if(replay_ && context_){
            engine_ddi::ReplayPolicy off{};off.size=sizeof(off);
            HRESULT hr=engine_ddi::set_replay_policy(context_,&off);stage("DeferredReplay-off",hr);
            if(hr!=S_OK)return false;
            replay_=false;
        }
        if(queues_){unsigned unresolved=0;
            HRESULT hr=queues_->discard_retired_metadata(unresolved);stage("QueueMetadata",hr);
            if(hr!=S_OK || unresolved)return false;
        }
        if(context_){
            uint32_t live=0;HRESULT hr=engine_ddi::destroy_device_context(context_,&live);
            amdgpu_wddm_log::print("d3d12-engine DeviceContext-close result=%08lx live=%u\n",static_cast<unsigned long>(hr),live);
            amdgpu_wddm_log::flush();if(hr!=S_OK)return false;context_=nullptr;
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
        active_.store(false,std::memory_order_release);
        const bool binding_failed=binding_failed_.load();
        amdgpu_wddm_log::print("d3d12-engine teardown instance_closed=%u binding_failed=%u unresolved=%u\n",
            unsigned(closed),unsigned(binding_failed),unresolved);amdgpu_wddm_log::flush();
        // Retain owner/code on uncertain unbind even if the engine discarded its instance.
        return closed && !binding_failed && unresolved==0;
    }
    void retain() noexcept {
        // Callback authority expires with the runtime Device. Keep storage/code
        // alive, but make any late callback fail before invoking that runtime.
        active_.store(false,std::memory_order_release);dispatch_.discard_metadata();
        if(imports_)imports_->discard_metadata();
        if(queue_bridge_)queue_bridge_->discard_metadata();
        ++adapter_.retained_engines;
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&stage),&pinned);
        stage("Owner-retained",E_UNEXPECTED);
    }
#ifdef AMDGPU_WDDM_D3D12_HOST_TEST
    HRESULT host_test_open(VkPhysicalDevice physical,VkDevice device) noexcept {
        bc250::umd::RuntimeDomain::Scope runtime(domain_);
        HostedInstanceBootstrap::Scope hosted(bootstrap_);
        if(!hosted.entered())return E_UNEXPECTED;
        auto create=reinterpret_cast<PFN_vkCreateInstance>(hosted.entry()(VK_NULL_HANDLE,"vkCreateInstance"));
        VkInstanceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        VkInstance instance{};
        if(!create || create(&info,nullptr,&instance)!=VK_SUCCESS || !instance || instance!=bootstrap_.instance())
            return E_FAIL;
        imports_.reset(new(std::nothrow) RuntimeHeapImports(device_,domain_,physical,device,instance,hosted.entry(),this));
        if(!imports_)return E_OUTOFMEMORY;
        return imports_->initialize();
    }
    void host_test_destroy_instance() noexcept {
        bc250::umd::RuntimeDomain::Scope runtime(domain_);
        HostedInstanceBootstrap::Scope hosted(bootstrap_);
        const VkInstance instance=bootstrap_.instance();
        if(!hosted.entered() || !instance)return;
        auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(hosted.entry()(instance,"vkDestroyInstance"));
        if(destroy)destroy(instance,nullptr);
    }
#endif
};
HRESULT create_device_engine(Device& device) noexcept {
    if(device.engine)return E_UNEXPECTED;
    AdapterEngineAccess access{};
    HRESULT result=get_adapter_engine(*device.adapter,&access);
    stage("AdapterEngine",result);if(FAILED(result))return result;
    device.trace_mode=ddi_trace_mode();device.recording=nullptr;
    auto owner=new(std::nothrow) DeviceEngine(device,access);if(!owner)return E_OUTOFMEMORY;
    device.engine=owner;
    result=owner->open();
    if(result==S_OK){
        // The device handle reaches the runtime only after this DDI returns, so no entry sees it change.
        // Lever L2 is on unless the experiment recording-bind-off says otherwise. S_FALSE here is the full
        // trace (mode 1) refusing the binding, not a failure: bind_recording explains why.
        if(!ddi_experiment_off("recording-bind")){
            const bool bound=owner->bind_recording(true);
            stage("RecordingBind",bound?S_OK:S_FALSE);
            // The lab's game runs capture the debugger log of trace mode 2, not stderr (the retire hand-off's
            // policy line reaches it through engine-ddi's log_line).
            if(bound)ddi_mode2_note("amdgpu_wddm_d3d12 recording-bind bound\n");
        }
        return S_OK;
    }
    if(owner->close())delete owner;else owner->retain();
    device.engine=nullptr;return result;
}
void destroy_device_engine(Device& device) noexcept {
    auto owner=device.engine;if(!owner)return;
    // DestroyDevice runs after every other entry of the device has returned: no recording entry holds it.
    owner->bind_recording(false);
    if(owner->close())delete owner;else owner->retain();
    device.engine=nullptr;
}
engine_ddi::DeviceContext* engine_context(Device& device) noexcept {return device.engine?device.engine->context():nullptr;}
QueueEngineRegistry* engine_queues(Device& device) noexcept {return device.engine?device.engine->queues():nullptr;}
RuntimeHeapImports* engine_imports(Device& device) noexcept {return device.engine?device.engine->imports():nullptr;}
bool device_engine_entered(Device& device) noexcept {return device.engine && device.engine->entered();}
void report_device_error(Device& device,HRESULT hr) noexcept {
    stage("DDI-error",hr);
    if(hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_HUNG || hr==DXGI_ERROR_DEVICE_RESET || hr==D3DDDIERR_DEVICEREMOVED)
        device.lost.store(true);
    // Reported on every failing call, also after the loss (see Device::remove).
    if(device_engine_entered(device) && device.callbacks.pfnSetErrorCb)device.callbacks.pfnSetErrorCb(device.runtime,hr);
}
// Every field this reads is either immutable while the device lives (device.engine, the domain and the
// bootstrap) or per thread (the three scopes), so entries of one device on several threads do not wait
// for each other here.
DeviceEngineScope::DeviceEngineScope(Device& device) noexcept {
    auto owner=device.engine;
    if(!owner || !owner->active_.load(std::memory_order_acquire) || (current_ && current_!=owner))return;
    runtime_.emplace(owner->domain_);hosted_.emplace(owner->bootstrap_);
    if(!hosted_->entered()){hosted_.reset();runtime_.reset();return;}
    owner_=owner;previous_=current_;current_=owner;
}
DeviceEngineScope::~DeviceEngineScope() noexcept {
    if(!owner_)return;
    hosted_.reset();runtime_.reset();current_=previous_;
}
#ifdef AMDGPU_WDDM_D3D12_HOST_TEST
HRESULT host_test_open_device_engine(Device& device,PFN_vkGetInstanceProcAddr icd,VkPhysicalDevice physical,
                                     VkDevice vk_device) noexcept {
    if(device.engine || !device.adapter || !icd)return E_UNEXPECTED;
    AdapterEngineAccess access{};access.driver_entry=icd;
    device.trace_mode=ddi_trace_mode();device.recording=nullptr;
    auto owner=new(std::nothrow) DeviceEngine(device,access);if(!owner)return E_OUTOFMEMORY;
    device.engine=owner;
    const HRESULT result=owner->host_test_open(physical,vk_device);
    if(result!=S_OK)(void)host_test_close_device_engine(device);
    return result;
}
bool host_test_bind_recording(Device& device,bool on) noexcept {
    return device.engine && device.engine->bind_recording(on);
}
bool host_test_close_device_engine(Device& device) noexcept {
    auto owner=device.engine;if(!owner)return false;
    owner->bind_recording(false);
    owner->host_test_destroy_instance();
    const bool closed=owner->close();
    if(closed)delete owner;else owner->retain();
    device.engine=nullptr;return closed;
}
#endif
}
