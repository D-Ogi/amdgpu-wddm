// SPDX-License-Identifier: MIT
#include "hosted-dispatch.h"
#include <d3dkmthk.h>
#include <new>
#include <cstring>
#include <cstdio>
#include "ddi-trace.h"

namespace native12 {
namespace {
constexpr UINT64 kPage=4096,kGranule=65536;
bool extent(UINT64 base,UINT64 bytes) noexcept {
    return base && bytes && !(base&(kPage-1)) && !(bytes&(kPage-1)) && base<=UINT64_MAX-bytes;
}
bool overlap(UINT64 a,UINT64 a_bytes,UINT64 b,UINT64 b_bytes) noexcept {return a<b+b_bytes && b<a+a_bytes;}
// A backed page is readable, or readable and writable: nothing else may be asked for it.
bool backed_protection(const D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE& p) noexcept {
    return !p.Zero && !p.NoAccess && !p.SystemUseOnly && !p.Reserved;
}
bool unbacked_protection(const D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE& p) noexcept {
    D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE zero{},none{};zero.Zero=1;none.NoAccess=1;
    return p.Value==zero.Value || p.Value==none.Value;
}
// Exclusive hold of a component lock that the owner can drop around a callback and take again.
class Held final {
    SRWLOCK& lock_;
    bool held_{};
public:
    explicit Held(SRWLOCK& lock) noexcept:lock_(lock) {AcquireSRWLockExclusive(&lock_);held_=true;}
    ~Held() {if(held_)ReleaseSRWLockExclusive(&lock_);}
    Held(const Held&)=delete;
    Held& operator=(const Held&)=delete;
    void release() noexcept {if(held_){held_=false;ReleaseSRWLockExclusive(&lock_);}}
    void acquire() noexcept {if(!held_){AcquireSRWLockExclusive(&lock_);held_=true;}}
};
template<class Record> void unlink_from(Record*& head,Record* record) noexcept {
    auto link=&head;while(*link && *link!=record)link=&(*link)->next;
    if(*link)*link=record->next;
}
// Only one thread at a time may work against an HCONTEXT (WDK, display/changes-from-direct3d-10.md).
// Each internal context belongs to one RADV queue, whose submissions and bindings RADV makes under that
// queue's lock, so the borrows of one context never overlap across threads. Such an overlap is noted
// (trace mode 2, name hcontext-shared) and not refused: the lab checks the claim, the game does not
// lose its device over it. Under lock_; true when another thread's borrow is in flight.
template<class Context> bool lend(Context& ctx) noexcept {
    const DWORD self=GetCurrentThreadId();
    const bool shared=ctx.borrowed && ctx.user!=self;
    if(!ctx.borrowed)ctx.user=self;
    ++ctx.borrowed;return shared;
}
template<class Context> void give(Context& ctx) noexcept {if(!--ctx.borrowed)ctx.user=0;}
// Once per process: a false per-queue-lock claim would otherwise spend the failure-note budget within a
// few frames and hide the later device-remove note.
void note_shared(bool shared) noexcept {
    static std::atomic<bool> noted{};
    if(shared && !noted.exchange(true))ddi_failure_note("hcontext-shared",S_OK);
}
}
struct HostedDispatch::Reservation {
    Reservation* next{};
    UINT64 base{},bytes{};                      // base 0: the reserve callback has not answered
    unsigned borrowed{};                        // runtime callbacks in flight inside this range
    bool busy{};                                // its own reserve or free callback is in flight
    bool freeing{};                             // its free callback is in flight: no longer owned (owned())
};
struct HostedDispatch::Allocation {
    RuntimeAllocation owner;
    Allocation* next{};
    D3DKMT_HANDLE handle{};                     // owner's handle, published under the lock once created
    UINT64 va{},bytes{};
    unsigned borrowed{};                        // runtime callbacks in flight that name it
    bool busy{},locked{};
    bool freeing{};                             // the free callback of va is in flight: no longer owned (owned())
    Allocation(D3D12DDI_HRTDEVICE runtime,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb)
        :owner(runtime,cb) {}
};
struct HostedDispatch::Sync {
    Sync* next{};
    D3DKMT_HANDLE handle{};
    uint64_t id{};                              // identity inside the device: a KMT handle may be reissued
    D3DDDI_SYNCHRONIZATIONOBJECT_TYPE type{};
    const volatile UINT64* cpu{};
    UINT64 published{};                         // the largest progress value published on it (any context)
    unsigned borrowed{};                        // progress publications in flight that name it
    bool busy{};
};
HostedDispatch::HostedDispatch(bc250::umd::RuntimeDomain& domain,D3D12DDI_HRTDEVICE runtime,
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& user,const D3DDDI_DEVICECALLBACKS& kernel,
    HostedDispatchHooks hooks) noexcept:domain_(domain),runtime_(runtime),user_(user),kernel_(kernel),hooks_(hooks) {}
HostedDispatch::~HostedDispatch(){discard_metadata();}
unsigned HostedDispatch::discard_metadata() noexcept {
    active_.store(false);unsigned unresolved=0;
    Held held(lock_);
    while(allocations_){auto record=allocations_;allocations_=record->next;
        if(record->owner.handle())++unresolved;
        record->owner.invalidate_runtime();delete record;}
    while(reservations_){auto range=reservations_;reservations_=range->next;++unresolved;delete range;}
    for(auto& ctx:contexts_){if(ctx.handle)++unresolved;ctx={};}
    while(syncs_){auto sync=syncs_;syncs_=sync->next;if(sync->handle)++unresolved;delete sync;}
    if(paging_.queue)++unresolved;paging_={};paging_busy_=false;
    user_={};kernel_={};hooks_={};runtime_={};return unresolved;
}
void HostedDispatch::unlink(Allocation* record) noexcept {unlink_from(allocations_,record);}
void HostedDispatch::unlink(Reservation* range) noexcept {unlink_from(reservations_,range);}
void HostedDispatch::unlink(Sync* sync) noexcept {unlink_from(syncs_,sync);}
HostedDispatch::Allocation* HostedDispatch::find(D3DKMT_HANDLE handle) noexcept {
    if(!handle)return nullptr;
    for(auto record=allocations_;record;record=record->next)
        if(record->handle==handle)return record;
    return nullptr;
}
HostedDispatch::Reservation* HostedDispatch::containing(UINT64 base,UINT64 bytes) noexcept {
    for(auto range=reservations_;range;range=range->next)
        if(range->base && base>=range->base && bytes<=range->bytes && base-range->base<=range->bytes-bytes)return range;
    return nullptr;
}
// An extent whose free callback is in flight is not owned any more. The runtime releases it somewhere
// inside that callback and may hand it out again at once, to a map or reservation on another thread,
// before the freeing thread takes lock_ again (trial 172 removed the device at the map check below).
// The runtime cannot hand out an extent it has not released, so a free that fails loses nothing.
bool HostedDispatch::owned(UINT64 base,UINT64 bytes,const void* except) const noexcept {
    for(auto range=reservations_;range;range=range->next)
        if(range!=except && range->base && !range->freeing && overlap(base,bytes,range->base,range->bytes))return true;
    for(auto record=allocations_;record;record=record->next)
        if(record!=except && record->va && !record->freeing && overlap(base,bytes,record->va,record->bytes))return true;
    return false;
}
bool HostedDispatch::borrow(D3DKMT_HANDLE handle) noexcept {
    if(auto record=find(handle)){if(record->busy)return false;++record->borrowed;return true;}
    return handle && hooks_.borrow && hooks_.give_back && hooks_.borrow(hooks_.userdata,handle);
}
void HostedDispatch::give_back(D3DKMT_HANDLE handle) noexcept {
    // A borrowed record cannot be destroyed, so the owner found here is the one that lent it.
    if(auto record=find(handle)){if(record->borrowed)--record->borrowed;return;}
    if(hooks_.give_back)hooks_.give_back(hooks_.userdata,handle);
}
HRESULT HostedDispatch::update(void* argument) noexcept {
    const auto& a=*static_cast<const D3DKMT_UPDATEGPUVIRTUALADDRESS*>(argument);
    // The runtime queues wait(FenceValue), the updates and signal(FenceValue+1).
    if(!a.NumOperations || !a.Operations || a.Reserved0 || a.Reserved1 || !a.FenceValue ||
       a.FenceValue>=UINT64_MAX-1)return E_INVALIDARG;
    if(a.Flags.Value)return E_NOTIMPL;
    Held held(lock_);
    auto sync=find_sync(a.hFenceObject);
    if(!sync || sync->busy || sync->type!=D3DDDI_MONITORED_FENCE)return E_INVALIDARG;
    auto ctx=context(a.hContext);
    if(ctx && ctx->busy)return E_INVALIDARG;
    if(!ctx && !hooks_.queue)return E_NOTIMPL;
    Reservation* range=nullptr;
    auto backing=[&](const D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION& op) noexcept ->D3DKMT_HANDLE {
        return op.OperationType==D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP?op.Map.hAllocation:
            op.OperationType==D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT?op.MapProtect.hAllocation:0;
    };
    for(UINT i=0;i<a.NumOperations;++i){
        const auto& op=a.Operations[i];UINT64 base=0,bytes=0,offset=0,part=0;bool backed=true;
        switch(op.OperationType){
        case D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP:
            base=op.Map.BaseAddress;bytes=op.Map.SizeInBytes;
            offset=op.Map.AllocationOffsetInBytes;part=op.Map.AllocationSizeInBytes;break;
        case D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT:
            base=op.MapProtect.BaseAddress;bytes=op.MapProtect.SizeInBytes;
            offset=op.MapProtect.AllocationOffsetInBytes;part=op.MapProtect.AllocationSizeInBytes;
            if(!backed_protection(op.MapProtect.Protection))return E_INVALIDARG;
            break;
        case D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP:
            base=op.Unmap.BaseAddress;bytes=op.Unmap.SizeInBytes;backed=false;
            if(!unbacked_protection(op.Unmap.Protection))return E_INVALIDARG;
            break;
        default:return E_NOTIMPL;
        }
        if(!extent(base,bytes))return E_INVALIDARG;
        auto found=containing(base,bytes);
        if(!found || found->busy || (range && found!=range))return E_INVALIDARG;
        range=found;
        // A part of 0 stands for the whole extent; a smaller part repeats over it.
        if(backed && (!backing(op) || (offset&(kPage-1)) || (part&(kPage-1)) || part>bytes ||
           (part && bytes%part) || offset>UINT64_MAX-(part?part:bytes)))return E_INVALIDARG;
    }
    // Everything the callback names is held until it returns: a call that re-enters the bridge, or
    // one on another thread, cannot release the backing, the range, the fence or the context under it.
    for(UINT i=0;i<a.NumOperations;++i){
        const auto handle=backing(a.Operations[i]);
        if(handle && !borrow(handle)){
            for(UINT j=0;j<i;++j)if(const auto taken=backing(a.Operations[j]))give_back(taken);
            return E_INVALIDARG;
        }
    }
    ++range->borrowed;sync->busy=true;
    HANDLE context_handle=nullptr;
    if(ctx){ctx->busy=true;context_handle=ctx->handle;}
    held.release();
    HRESULT hr;
    if(ctx){
        // The callback's request is not the kernel's: full context handle, other field widths.
        D3DDDICB_UPDATEGPUVIRTUALADDRESS b{};b.hContext=context_handle;b.hFenceObject=a.hFenceObject;
        b.NumOperations=a.NumOperations;b.Operations=a.Operations;b.FenceValue=a.FenceValue;
        hr=kernel_.pfnUpdateGpuVirtualAddressCb?kernel_.pfnUpdateGpuVirtualAddressCb(runtime_.handle,&b):E_NOTIMPL;
    }else hr=hooks_.queue(hooks_.userdata,BC250_HOST_UpdateGpuVirtualAddress,argument);
    held.acquire();
    if(ctx)ctx->busy=false;
    sync->busy=false;--range->borrowed;
    for(UINT i=0;i<a.NumOperations;++i)if(const auto taken=backing(a.Operations[i]))give_back(taken);
    held.release();
    // S_OK is admission to the queue, not completion. Any other success is not a defined answer.
    if(SUCCEEDED(hr) && hr!=S_OK)return remove_device();
    return hr;
}
HostedDispatch::Sync* HostedDispatch::find_sync(D3DKMT_HANDLE handle) noexcept {
    if(!handle)return nullptr;
    for(auto sync=syncs_;sync;sync=sync->next)if(sync->handle==handle)return sync;
    return nullptr;
}
HostedDispatch::Sync* HostedDispatch::find_sync_id(uint64_t id) noexcept {
    if(!id)return nullptr;
    for(auto sync=syncs_;sync;sync=sync->next)if(sync->id==id)return sync;
    return nullptr;
}
// The marks of device-progress.h: one per monitored fence whose published value it has not reached. A fence
// whose destroy is in flight gets a mark with no value read (its CPU page may already be unmapped); the mark
// is satisfied when the fence is gone, which the ICD does only after waiting for it (radv_wddm2_cs.c:198).
void HostedDispatch::progress_snapshot(ProgressSnapshot* out) noexcept {
    if(!out)return;
    *out={};
    AcquireSRWLockShared(&lock_);
    for(auto sync=syncs_;sync;sync=sync->next){
        if(sync->type!=D3DDDI_MONITORED_FENCE || !sync->published || !sync->id)continue;
        if(!sync->busy && sync->cpu){
            const UINT64 value=*sync->cpu;MemoryBarrier();
            // A removed device never runs anything again; a fence that caught up needs no mark.
            if(value==UINT64_MAX || value>=sync->published)continue;
        }
        if(out->count==kMaxProgressMarks){out->complete=false;break;}
        out->marks[out->count++]={sync->id,sync->published};
    }
    ReleaseSRWLockShared(&lock_);
}
bool HostedDispatch::progress_retired(const ProgressSnapshot* snapshot) noexcept {
    if(!snapshot || !snapshot->complete)return false;
    bool retired=true;
    AcquireSRWLockShared(&lock_);
    for(unsigned i=0;i<snapshot->count && retired;++i){
        auto sync=find_sync_id(snapshot->marks[i].sync);
        if(!sync)continue;                       // destroyed after the snapshot: its work retired with it
        if(sync->busy || !sync->cpu){retired=false;continue;}
        const UINT64 value=*sync->cpu;MemoryBarrier();
        if(value!=UINT64_MAX && value<snapshot->marks[i].value)retired=false;
    }
    ReleaseSRWLockShared(&lock_);
    return retired;
}
ProgressSource HostedDispatch::progress_source() noexcept {
    return {this,
        [](void* owner,ProgressSnapshot* out) noexcept {static_cast<HostedDispatch*>(owner)->progress_snapshot(out);},
        [](void* owner,const ProgressSnapshot* snapshot) noexcept {
            return static_cast<HostedDispatch*>(owner)->progress_retired(snapshot);}};
}
// Never under lock_: the runtime's error callback is a callback like any other. A replay worker leaves the callback
// to report_deferred_removal.
HRESULT HostedDispatch::remove_device(int site) noexcept {
    char name[48];std::snprintf(name,sizeof(name),"hosted-remove-device:%d",site);
    ddi_failure_note(name,D3DDDIERR_DEVICEREMOVED);
    if(worker_==this){
        if(!lost_.exchange(true))removal_deferred_.store(true,std::memory_order_release);
        return D3DDDIERR_DEVICEREMOVED;
    }
    if(!lost_.exchange(true) && user_.pfnSetErrorCb)user_.pfnSetErrorCb(runtime_,D3DDDIERR_DEVICEREMOVED);
    return D3DDDIERR_DEVICEREMOVED;
}
void HostedDispatch::report_deferred_removal() noexcept {
    if(!removal_deferred_.load(std::memory_order_acquire) || worker_==this || !removal_deferred_.exchange(false))return;
    ddi_failure_note("hosted-remove-device:deferred",D3DDDIERR_DEVICEREMOVED);
    if(user_.pfnSetErrorCb)user_.pfnSetErrorCb(runtime_,D3DDDIERR_DEVICEREMOVED);
}
// What a recording call of the engine may need on a replay worker: device-level memory and CPU-side sync work. No
// context, submission, GPU wait or signal, progress publication or queue operation (engine-ddi replay.h, R4).
bool HostedDispatch::worker_admits(uint32_t op) noexcept {
    switch(op){
    case BC250_HOST_CREATE_PAGING:case BC250_HOST_DESTROY_PAGING:case BC250_HOST_CHECK_STATUS:
    case BC250_HOST_REPORT_LOST:
    case BC250_HOST_CreateAllocation2:case BC250_HOST_DestroyAllocation2:case BC250_HOST_ReserveGpuVirtualAddress:
    case BC250_HOST_MapGpuVirtualAddress:case BC250_HOST_FreeGpuVirtualAddress:case BC250_HOST_MakeResident:
    case BC250_HOST_Evict:case BC250_HOST_Lock2:case BC250_HOST_Unlock2:
    case BC250_HOST_CreateSynchronizationObject2:case BC250_HOST_DestroySynchronizationObject:
    case BC250_HOST_WaitForSynchronizationObjectFromCpu:case BC250_HOST_SignalSynchronizationObjectFromCpu:
    case BC250_HOST_GetDeviceState:return true;
    default:return false;
    }
}
bc250_host HostedDispatch::descriptor(uint64_t luid,void* identity) noexcept {
    bc250_host host{};host.sType=BC250_HOST_STYPE;host.version=BC250_HOST_VERSION;
    host.size=sizeof(host);host.adapter_luid=luid;host.identity=identity;
    host.userdata=this;host.dispatch=dispatch;return host;
}
HostedDispatch::Context* HostedDispatch::context(uint32_t token) noexcept {
    for(auto& ctx:contexts_)if(ctx.handle && ctx.token==token)return &ctx;
    return nullptr;
}
HRESULT HostedDispatch::internal_queue(uint32_t op,void* argument) noexcept {
    auto delegate=[&]() noexcept {return hooks_.queue?hooks_.queue(hooks_.userdata,op,argument):E_NOTIMPL;};
#define KT_QUEUE(name,args) (kernel_.pfn##name##Cb?kernel_.pfn##name##Cb(runtime_.handle,args):E_NOTIMPL)
    if(op==BC250_HOST_CREATE_QUEUE_CONTEXT || op==BC250_HOST_DESTROY_QUEUE_CONTEXT){
        auto& wrapper=*static_cast<bc250_host_queue_context*>(argument);
        if(wrapper.queue)return delegate();
        if(!wrapper.arguments)return E_INVALIDARG;
        if(op==BC250_HOST_CREATE_QUEUE_CONTEXT){
            auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(wrapper.arguments);
            Held held(lock_);
            if(a.hContext || next_context_==0x7fffffffu)return E_INVALIDARG;
            Context* ctx=nullptr;for(auto& entry:contexts_)if(!entry.handle && !entry.busy && !entry.borrowed){ctx=&entry;break;}
            if(!ctx)return E_OUTOFMEMORY;
            ctx->busy=true;held.release();
            D3DDDICB_CREATECONTEXTVIRTUAL b{};b.NodeOrdinal=a.NodeOrdinal;b.EngineAffinity=a.EngineAffinity;b.Flags=a.Flags;
            b.pPrivateDriverData=a.pPrivateDriverData;b.PrivateDriverDataSize=a.PrivateDriverDataSize;
            HRESULT hr=KT_QUEUE(CreateContextVirtual,&b);
            held.acquire();ctx->busy=false;
            if(SUCCEEDED(hr)){
                ctx->handle=b.hContext;ctx->token=0x80000000u|++next_context_;
                const bool created=ctx->handle!=nullptr;const uint32_t token=ctx->token;
                held.release();
                if(!created)return remove_device();a.hContext=token;
            }
            return hr;
        }
        auto& a=*static_cast<D3DKMT_DESTROYCONTEXT*>(wrapper.arguments);
        Held held(lock_);
        auto ctx=context(a.hContext);
        if(!ctx || ctx->busy || ctx->borrowed)return E_INVALIDARG;
        D3DDDICB_DESTROYCONTEXT b{};b.hContext=ctx->handle;
        ctx->busy=true;held.release();
        HRESULT hr=KT_QUEUE(DestroyContext,&b);
        held.acquire();ctx->busy=false;
        if(hr==S_OK)*ctx={};return hr;
    }
    if(op==BC250_HOST_PUBLISH_PROGRESS){
        auto& a=*static_cast<bc250_host_progress*>(argument);
        Held held(lock_);
        auto ctx=context(a.context);
        if(!ctx){held.release();return delegate();}
        if(!a.sync || !a.cpu_address || !a.value || a.value==UINT64_MAX ||
           (ctx->progress.sync && (ctx->progress.sync!=a.sync || ctx->progress.value>=a.value)))return E_INVALIDARG;
        ctx->progress=a;return S_OK;
    }
    if(op==BC250_HOST_WaitForSynchronizationObjectFromGpu){
        auto& a=*static_cast<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU*>(argument);
        Held held(lock_);
        auto ctx=context(a.hContext);
        if(!ctx){held.release();return delegate();}
        if(ctx->busy || !a.ObjectCount || !a.ObjectHandleArray || !a.MonitoredFenceValueArray)return E_INVALIDARG;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU b{};b.hContext=ctx->handle;
        b.ObjectCount=a.ObjectCount;b.ObjectHandleArray=a.ObjectHandleArray;b.MonitoredFenceValueArray=a.MonitoredFenceValueArray;
        const bool shared=lend(*ctx);held.release();note_shared(shared);
        HRESULT hr=KT_QUEUE(WaitForSynchronizationObjectFromGpu,&b);
        held.acquire();give(*ctx);return hr;
    }
    if(op==BC250_HOST_SignalSynchronizationObjectFromGpu2){
        auto& a=*static_cast<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2*>(argument);
        if(!a.BroadcastContextCount || a.BroadcastContextCount>D3DDDI_MAX_BROADCAST_CONTEXT || a.Flags.Value ||
           !a.ObjectCount || !a.ObjectHandleArray || !a.MonitoredFenceValueArray)return E_INVALIDARG;
        HANDLE handles[D3DDDI_MAX_BROADCAST_CONTEXT]{};Context* named[D3DDDI_MAX_BROADCAST_CONTEXT]{};
        Held held(lock_);
        for(UINT i=0;i<a.BroadcastContextCount;++i){
            auto ctx=context(a.BroadcastContextArray[i]);if(!ctx){held.release();return delegate();}
            if(ctx->busy)return E_INVALIDARG;
            named[i]=ctx;handles[i]=ctx->handle;
        }
        bool shared=false;for(UINT i=0;i<a.BroadcastContextCount;++i)shared|=lend(*named[i]);
        held.release();note_shared(shared);
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 b{};b.ObjectCount=a.ObjectCount;b.ObjectHandleArray=a.ObjectHandleArray;
        b.BroadcastContextCount=a.BroadcastContextCount;b.BroadcastContextArray=handles;b.MonitoredFenceValueArray=a.MonitoredFenceValueArray;
        HRESULT hr=KT_QUEUE(SignalSynchronizationObjectFromGpu2,&b);
        held.acquire();for(UINT i=0;i<a.BroadcastContextCount;++i)give(*named[i]);
        return hr;
    }
    if(op==BC250_HOST_SubmitCommand){
        auto& a=*static_cast<D3DKMT_SUBMITCOMMAND*>(argument);
        if(!a.BroadcastContextCount || a.BroadcastContextCount>D3DDDI_MAX_BROADCAST_CONTEXT || a.NumPrimaries>D3DDDI_MAX_WRITTEN_PRIMARIES ||
           a.Flags.NullRendering || a.Flags.PresentRedirected || a.Flags.NoKmdAccess || a.Flags.Reserved || a.PresentHistoryToken || a.NumHistoryBuffers)return E_NOTIMPL;
        D3DDDICB_SUBMITCOMMAND b{};b.Commands=a.Commands;b.CommandLength=a.CommandLength;
        b.pPrivateDriverData=a.pPrivateDriverData;b.PrivateDriverDataSize=a.PrivateDriverDataSize;b.BroadcastContextCount=a.BroadcastContextCount;
        Context* named[D3DDDI_MAX_BROADCAST_CONTEXT]{};
        Held held(lock_);
        for(UINT i=0;i<a.BroadcastContextCount;++i){
            auto ctx=context(a.BroadcastContext[i]);if(!ctx){held.release();return delegate();}
            if(ctx->busy)return E_INVALIDARG;
            named[i]=ctx;b.BroadcastContext[i]=ctx->handle;
        }
        bool shared=false;for(UINT i=0;i<a.BroadcastContextCount;++i)shared|=lend(*named[i]);
        held.release();note_shared(shared);
        b.NumPrimaries=a.NumPrimaries;for(UINT i=0;i<a.NumPrimaries;++i)b.WrittenPrimaries[i]=a.WrittenPrimaries[i];
        HRESULT hr=KT_QUEUE(SubmitCommand,&b);
        held.acquire();for(UINT i=0;i<a.BroadcastContextCount;++i)give(*named[i]);
        return hr;
    }
    return delegate();
#undef KT_QUEUE
}
HRESULT HostedDispatch::operation(uint32_t op,void* argument) noexcept {
    if(op==BC250_HOST_CHECK_STATUS){
        // The most frequent callback (every fence poll): a shared hold, no record changes.
        bool removed=false;
        AcquireSRWLockShared(&lock_);
        // Not while the paging queue's destroy is in flight: the kernel may have unmapped its fence page.
        if(!paging_busy_ && paging_.cpu_address && *static_cast<const volatile UINT64*>(paging_.cpu_address)==UINT64_MAX)removed=true;
        for(auto sync=syncs_;sync && !removed;sync=sync->next)if(!sync->busy && sync->cpu && *sync->cpu==UINT64_MAX)removed=true;
        ReleaseSRWLockShared(&lock_);
        if(removed)return remove_device();
        return lost()?D3DDDIERR_DEVICEREMOVED:S_OK;
    }
    if(op==BC250_HOST_REPORT_LOST)return remove_device();
    if(!argument)return E_INVALIDARG;
    if(lost())return D3DDDIERR_DEVICEREMOVED;
    // Validate both internal and application queue progress before delegation. The fence stays
    // named (borrowed) until the publication returns, so that its CPU mapping outlives the call.
    // Queue hooks must not retain the borrowed CPU mapping after this call.
    if(op==BC250_HOST_PUBLISH_PROGRESS){
        const auto& a=*static_cast<const bc250_host_progress*>(argument);
        Sync* sync=nullptr;
        {
            Held held(lock_);
            sync=find_sync(a.sync);
            if(!sync || sync->busy || sync->type!=D3DDDI_MONITORED_FENCE || !sync->cpu ||
               sync->cpu!=a.cpu_address || !a.value || a.value==UINT64_MAX)return E_INVALIDARG;
            ++sync->borrowed;
        }
        const HRESULT hr=internal_queue(op,argument);
        Held held(lock_);--sync->borrowed;
        // Device-wide progress (device-progress.h): the publication was accepted, so everything this
        // context submitted up to this value is on the GPU. Values of one fence only grow (the publication
        // is refused otherwise), and the maximum is taken in case several contexts ever share a fence.
        if(hr==S_OK && a.value>sync->published)sync->published=a.value;
        return hr;
    }
#define KT_CALL(name,args) (kernel_.pfn##name##Cb?kernel_.pfn##name##Cb(runtime_.handle,args):E_NOTIMPL)
    switch(op){
    case BC250_HOST_CREATE_PAGING:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        {Held held(lock_);if(paging_.queue || paging_busy_)return E_UNEXPECTED;paging_busy_=true;}
        D3DDDICB_CREATEPAGINGQUEUE b{};b.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        HRESULT hr=KT_CALL(CreatePagingQueue,&b);
        bc250_host_paging created{};
        {Held held(lock_);paging_busy_=false;
         if(hr==S_OK){paging_={b.hPagingQueue,b.hSyncObject,b.FenceValueCPUVirtualAddress};created=paging_;}}
        if(hr==S_OK){*static_cast<bc250_host_paging*>(argument)=created;
            if(!created.queue || !created.sync || !created.cpu_address)return remove_device();}
        return hr;
    }
    case BC250_HOST_DESTROY_PAGING:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        const auto queue=static_cast<bc250_host_paging*>(argument)->queue;
        {Held held(lock_);
         if(!paging_.queue || paging_busy_ || queue!=paging_.queue || allocations_ || reservations_)return E_INVALIDARG;
         paging_busy_=true;}
        D3DDDI_DESTROYPAGINGQUEUE b{};b.hPagingQueue=queue;
        HRESULT hr=KT_CALL(DestroyPagingQueue,&b);
        Held held(lock_);paging_busy_=false;if(hr==S_OK)paging_={};return hr;
    }
    case BC250_HOST_MakeResident:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        auto& a=*static_cast<D3DDDI_MAKERESIDENT*>(argument);
        if(!a.NumAllocations || !a.AllocationList || a.Flags.Reserved)return E_INVALIDARG;
        const UINT count=a.NumAllocations;
        // The allocations named stay (borrowed) until the call returns.
        {Held held(lock_);
         if(!paging_.queue || paging_busy_ || a.hPagingQueue!=paging_.queue)return E_INVALIDARG;
         // A record whose own create or destroy is in flight is refused, as borrow() refuses it: a destroy
         // that already passed its borrowed check must not find a new borrower when it retakes the lock.
         for(UINT i=0;i<count;++i){const auto r=find(a.AllocationList[i]);if(!r || r->busy)return E_INVALIDARG;}
         for(UINT i=0;i<count;++i)++find(a.AllocationList[i])->borrowed;}
        HRESULT hr=KT_CALL(MakeResident,&a);
        {Held held(lock_);for(UINT i=0;i<count;++i)if(const auto r=find(a.AllocationList[i]))--r->borrowed;}
        if((hr==S_OK || hr==E_PENDING) && (a.NumAllocations!=count ||
           (hr==E_PENDING && (!a.PagingFenceValue || a.PagingFenceValue==UINT64_MAX))))return remove_device();
        // S_OK is immediately resident; only E_PENDING defines this output.
        // RADV combines it with the mapping fence for every success.
        if(hr==S_OK)a.PagingFenceValue=0;
        return hr;
    }
    case BC250_HOST_Evict:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        auto& a=*static_cast<D3DKMT_EVICT*>(argument);
        if(!a.NumAllocations || !a.AllocationList || a.Flags.Reserved)return E_INVALIDARG;
        const UINT count=a.NumAllocations;
        {Held held(lock_);
         for(UINT i=0;i<count;++i){const auto r=find(a.AllocationList[i]);if(!r || r->busy)return E_INVALIDARG;}
         for(UINT i=0;i<count;++i)++find(a.AllocationList[i])->borrowed;}
        D3DDDICB_EVICT b{};b.NumAllocations=count;b.AllocationList=a.AllocationList;b.Flags=a.Flags;
        HRESULT hr=KT_CALL(Evict,&b);
        {Held held(lock_);for(UINT i=0;i<count;++i)if(const auto r=find(a.AllocationList[i]))--r->borrowed;}
        a.NumBytesToTrim=b.NumBytesToTrim;return hr;
    }
    case BC250_HOST_PUBLISH_PROGRESS:case BC250_HOST_CREATE_QUEUE_CONTEXT:case BC250_HOST_DESTROY_QUEUE_CONTEXT:
    case BC250_HOST_CreateContextVirtual:case BC250_HOST_DestroyContext:
    case BC250_HOST_WaitForSynchronizationObjectFromGpu:case BC250_HOST_SignalSynchronizationObjectFromGpu:
    case BC250_HOST_SignalSynchronizationObjectFromGpu2:case BC250_HOST_SubmitCommand:
    case BC250_HOST_CreateHwQueue:case BC250_HOST_DestroyHwQueue:case BC250_HOST_SubmitCommandToHwQueue:
    case BC250_HOST_SubmitWaitForSyncObjectsToHwQueue:case BC250_HOST_SubmitSignalSyncObjectsToHwQueue:
        return internal_queue(op,argument);
    case BC250_HOST_UpdateGpuVirtualAddress:return update(argument);
    case BC250_HOST_ReserveGpuVirtualAddress:{
        auto& a=*static_cast<D3DDDI_RESERVEGPUVIRTUALADDRESS*>(argument);
        const bool fixed=a.BaseAddress!=0;
        // Reserved0, Reserved1 and Reserved2 are the obsolete reservation type, driver protection
        // and paging fence.
        if(!a.Size || (a.Size&(kGranule-1)) || (a.BaseAddress&(kGranule-1)) || a.Reserved0 || a.Reserved1 ||
           a.Reserved2)return E_INVALIDARG;
        if(fixed){
            if(a.BaseAddress>UINT64_MAX-a.Size)return E_INVALIDARG;
        }else if((a.MinimumAddress&(kGranule-1)) || (a.MaximumAddress&(kGranule-1)) ||
                 (a.MaximumAddress && (a.MaximumAddress<=a.MinimumAddress ||
                  a.MaximumAddress-a.MinimumAddress<a.Size)))return E_INVALIDARG;
        auto range=new(std::nothrow) Reservation;if(!range)return E_OUTOFMEMORY;
        {Held held(lock_);
         if(fixed && owned(a.BaseAddress,a.Size,nullptr)){held.release();delete range;return E_INVALIDARG;}
         // Tracking storage exists before the runtime may reserve anything.
         range->busy=true;range->next=reservations_;reservations_=range;}
        D3DDDI_RESERVEGPUVIRTUALADDRESS b{};b.BaseAddress=a.BaseAddress;b.MinimumAddress=a.MinimumAddress;
        b.MaximumAddress=a.MaximumAddress;b.Size=a.Size;
        HRESULT hr=KT_CALL(ReserveGpuVirtualAddress,&b);
        Held held(lock_);range->busy=false;
        if(FAILED(hr)){unlink(range);held.release();delete range;return hr;}
        // The runtime holds a reservation now, whatever it answered: the record stays.
        const UINT64 va=b.VirtualAddress;
        const bool placed=va && !(va&(kGranule-1)) && va<=UINT64_MAX-a.Size &&
            (fixed?va==a.BaseAddress:(va>=a.MinimumAddress && (!a.MaximumAddress || va+a.Size<=a.MaximumAddress)));
        const bool free_extent=placed && !owned(va,a.Size,range);
        range->base=va;range->bytes=a.Size;
        held.release();
        if(hr!=S_OK || !free_extent)return remove_device();
        a.VirtualAddress=va;a.PagingFenceValue=b.PagingFenceValue;return S_OK;
    }
    case BC250_HOST_CreateAllocation2:{
        auto& a=*static_cast<D3DKMT_CREATEALLOCATION*>(argument);
        auto flags=a.Flags;flags.CreateResource=0;flags.NonSecure=0;
        D3DKMT_CREATEALLOCATIONFLAGS zero_flags{};
        if(a.hResource || a.hGlobalShare || a.NumAllocations!=1 || !a.pAllocationInfo2 ||
           std::memcmp(&flags,&zero_flags,sizeof(flags)) || (a.PrivateDriverDataSize&&!a.pPrivateDriverData))return E_INVALIDARG;
        const auto& input=*a.pAllocationInfo2;
        if(input.hAllocation || input.Flags.Value || (input.PrivateDriverDataSize&&!input.pPrivateDriverData))return E_INVALIDARG;
        auto record=new(std::nothrow) Allocation(runtime_,user_);if(!record)return E_OUTOFMEMORY;
        // Tracking storage exists before the runtime may allocate anything.
        {Held held(lock_);record->busy=true;record->next=allocations_;allocations_=record;}
        D3D12DDI_ALLOCATION_INFO_0022 info{};
        info.pSystemMem=input.pSystemMem;info.pPrivateDriverData=input.pPrivateDriverData;
        info.PrivateDriverDataSize=input.PrivateDriverDataSize;info.VidPnSourceId=input.VidPnSourceId;info.Priority=input.Priority;
        D3D12DDICB_ALLOCATE_0022 request{};request.pPrivateDriverData=a.pPrivateDriverData;
        request.PrivateDriverDataSize=a.PrivateDriverDataSize;request.NumAllocations=1;request.pAllocationInfo=&info;
        HRESULT hr=record->owner.open(request);
        Held held(lock_);record->busy=false;
        if(FAILED(hr)){unlink(record);held.release();delete record;return hr;}
        record->handle=record->owner.handle();
        const D3DKMT_HANDLE handle=record->handle;const auto address=record->owner.address();
        held.release();
        a.pAllocationInfo2->hAllocation=handle;a.pAllocationInfo2->GpuVirtualAddress=address;
        // Native hKMResource is reserved. Internal allocations are released by handle list.
        a.hResource=0;return S_OK;
    }
    case BC250_HOST_DestroyAllocation2:{
        auto& a=*static_cast<D3DKMT_DESTROYALLOCATION2*>(argument);
        if(a.hResource || a.Flags.Value || a.AllocationCount!=1 || !a.phAllocationList)return E_INVALIDARG;
        Held held(lock_);
        auto record=find(a.phAllocationList[0]);
        if(!record || record->busy || record->borrowed || record->locked || record->va)return E_INVALIDARG;
        record->busy=true;held.release();
        HRESULT hr=record->owner.close();
        held.acquire();record->busy=false;
        if(SUCCEEDED(hr)){unlink(record);held.release();delete record;}
        return hr;
    }
    case BC250_HOST_MapGpuVirtualAddress:{
        auto& a=*static_cast<D3DDDI_MAPGPUVIRTUALADDRESS*>(argument);
        if(!a.SizeInPages || a.SizeInPages>UINT64_MAX/kPage)return E_INVALIDARG;
        const UINT64 bytes=a.SizeInPages*kPage;
        Held held(lock_);
        if(!hooks_.paging && (!paging_.queue || paging_busy_ || a.hPagingQueue!=paging_.queue))return E_INVALIDARG;
        Reservation* range=nullptr;
        if(a.BaseAddress){
            if(!extent(a.BaseAddress,bytes))return E_INVALIDARG;
            range=containing(a.BaseAddress,bytes);
            // An extent that leaves its reservation, or enters one from outside, has no owner.
            if(!range && owned(a.BaseAddress,bytes,nullptr))return E_INVALIDARG;
        }
        if(range){
            // A view inside a reservation: zero pages, or an allocation whose own ordinary
            // mapping, if it has one, is not touched. One allocation may have many such views.
            if(range->busy)return E_INVALIDARG;
            const bool zero=!a.hAllocation;
            if(zero){
                D3DDDIGPUVIRTUALADDRESS_PROTECTION_TYPE only{};only.Zero=1;
                if(a.Protection.Value!=only.Value || a.OffsetInPages || a.DriverProtection)return E_INVALIDARG;
            }else if(!backed_protection(a.Protection) || a.OffsetInPages>UINT64_MAX/kPage-a.SizeInPages ||
                     !borrow(a.hAllocation))return E_INVALIDARG;
            ++range->borrowed;held.release();
            HRESULT hr=KT_CALL(MapGpuVirtualAddress,&a);
            held.acquire();--range->borrowed;if(!zero)give_back(a.hAllocation);held.release();
            if((hr==S_OK || hr==E_PENDING) && (a.VirtualAddress!=a.BaseAddress ||
               (hr==E_PENDING && (!a.PagingFenceValue || a.PagingFenceValue==UINT64_MAX))))return remove_device();
            return hr;
        }
        auto record=find(a.hAllocation);
        if(!record || record->busy || record->va)return E_INVALIDARG;
        record->busy=true;held.release();
        HRESULT hr=KT_CALL(MapGpuVirtualAddress,&a);
        held.acquire();record->busy=false;
        bool malformed=false;
        if(hr==S_OK || hr==E_PENDING){record->va=a.VirtualAddress;record->bytes=bytes;
            // Without a fixed base the answer lies inside the limits that were asked for.
            const bool outside=!a.BaseAddress && extent(record->va,bytes) && (record->va<a.MinimumAddress ||
                (a.MaximumAddress && (a.MaximumAddress<bytes || record->va>a.MaximumAddress-bytes)));
            malformed=!extent(record->va,bytes) || (a.BaseAddress && record->va!=a.BaseAddress) || outside ||
               owned(record->va,bytes,record) ||
               (hr==E_PENDING && (!a.PagingFenceValue || a.PagingFenceValue==UINT64_MAX));}
        held.release();
        if(malformed)return remove_device();
        return hr;
    }
    case BC250_HOST_FreeGpuVirtualAddress:{
        auto& a=*static_cast<D3DKMT_FREEGPUVIRTUALADDRESS*>(argument);Allocation* record=nullptr;
        D3DDDICB_FREEGPUVIRTUALADDRESS b{};b.BaseAddress=a.BaseAddress;b.Size=a.Size;
        Held held(lock_);
        // An extent being freed is skipped: its address may already belong to a newer record (owned()).
        for(auto range=reservations_;range;range=range->next){
            if(!range->base || range->freeing || range->base!=a.BaseAddress || range->bytes!=a.Size)continue;
            if(range->busy || range->borrowed)return E_INVALIDARG;
            // Frees the range and every view inside it; updates still queued for it are ignored.
            range->busy=range->freeing=true;held.release();
            HRESULT hr=KT_CALL(FreeGpuVirtualAddress,&b);
            held.acquire();range->busy=range->freeing=false;
            if(hr==S_OK){unlink(range);held.release();delete range;}
            return hr;
        }
        for(auto r=allocations_;r;r=r->next)if(r->va && !r->freeing && r->va==a.BaseAddress && r->bytes==a.Size){record=r;break;}
        if(!record || record->busy || record->borrowed || record->locked)return E_INVALIDARG;
        record->busy=record->freeing=true;held.release();
        HRESULT hr=KT_CALL(FreeGpuVirtualAddress,&b);
        held.acquire();record->busy=record->freeing=false;
        if(hr==S_OK){record->va=record->bytes=0;}return hr;
    }
    case BC250_HOST_Lock2:{
        auto& a=*static_cast<D3DKMT_LOCK2*>(argument);
        Held held(lock_);
        auto record=find(a.hAllocation);
        if(!record || record->busy || record->locked)return E_INVALIDARG;
        D3DDDICB_LOCK2 b{};b.hAllocation=a.hAllocation;b.Flags.Value=a.Flags.Value;
        record->busy=true;held.release();
        HRESULT hr=KT_CALL(Lock2,&b);
        held.acquire();record->busy=false;
        if(hr==S_OK){record->locked=true;held.release();a.pData=b.pData;if(!b.pData)return remove_device();}
        return hr;
    }
    case BC250_HOST_Unlock2:{
        auto& a=*static_cast<D3DKMT_UNLOCK2*>(argument);
        Held held(lock_);
        auto record=find(a.hAllocation);
        if(!record || record->busy || !record->locked)return E_INVALIDARG;
        D3DDDICB_UNLOCK2 b{};b.hAllocation=a.hAllocation;
        record->busy=true;held.release();
        HRESULT hr=KT_CALL(Unlock2,&b);
        held.acquire();record->busy=false;
        if(hr==S_OK)record->locked=false;return hr;
    }
    case BC250_HOST_CreateSynchronizationObject2:{
        auto& a=*static_cast<D3DKMT_CREATESYNCHRONIZATIONOBJECT2*>(argument);
        if(a.Info.Flags.Shared || a.Info.Flags.NtSecuritySharing)return E_NOTIMPL;
        if(a.hSyncObject)return E_INVALIDARG;
        auto sync=new(std::nothrow) Sync;if(!sync)return E_OUTOFMEMORY;
        {Held held(lock_);sync->busy=true;sync->next=syncs_;syncs_=sync;}
        D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 b{};b.Info=a.Info;
        HRESULT hr=KT_CALL(CreateSynchronizationObject2,&b);
        Held held(lock_);sync->busy=false;
        if(hr!=S_OK){unlink(sync);held.release();delete sync;return hr;}
        const bool duplicate=find_sync(b.hSyncObject)!=nullptr;
        sync->handle=b.hSyncObject;sync->type=b.Info.Type;sync->id=++next_sync_;
        if(sync->type==D3DDDI_MONITORED_FENCE)sync->cpu=static_cast<const volatile UINT64*>(b.Info.MonitoredFence.FenceValueCPUVirtualAddress);
        const bool malformed=!sync->handle || duplicate || (sync->type==D3DDDI_MONITORED_FENCE && !sync->cpu);
        held.release();
        if(malformed)return remove_device();
        a.Info=b.Info;a.hSyncObject=b.hSyncObject;return S_OK;
    }
    case BC250_HOST_DestroySynchronizationObject:{
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT b{};
        b.hSyncObject=static_cast<D3DKMT_DESTROYSYNCHRONIZATIONOBJECT*>(argument)->hSyncObject;
        Held held(lock_);
        auto sync=find_sync(b.hSyncObject);if(!sync || sync->busy || sync->borrowed)return E_INVALIDARG;
        sync->busy=true;held.release();
        HRESULT hr=KT_CALL(DestroySynchronizationObject,&b);
        held.acquire();sync->busy=false;
        if(hr==S_OK){
            for(auto& ctx:contexts_)if(ctx.progress.sync==b.hSyncObject)ctx.progress={};
            unlink(sync);held.release();delete sync;
        }
        return hr;
    }
    case BC250_HOST_WaitForSynchronizationObjectFromCpu:{
        auto& a=*static_cast<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU*>(argument);
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU b{};
        b.ObjectCount=a.ObjectCount;b.ObjectHandleArray=a.ObjectHandleArray;b.FenceValueArray=a.FenceValueArray;
        b.hAsyncEvent=a.hAsyncEvent;b.Flags=a.Flags;return KT_CALL(WaitForSynchronizationObjectFromCpu,&b);
    }
    case BC250_HOST_SignalSynchronizationObjectFromCpu:{
        auto& a=*static_cast<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU*>(argument);
        if(a.Flags.Value)return E_NOTIMPL;
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU b{};b.ObjectCount=a.ObjectCount;
        b.ObjectHandleArray=a.ObjectHandleArray;b.FenceValueArray=a.FenceValueArray;return KT_CALL(SignalSynchronizationObjectFromCpu,&b);
    }
    default:return E_NOTIMPL;
    }
#undef KT_CALL
}
int32_t HostedDispatch::dispatch(void* userdata,uint32_t op,void* argument) noexcept {
    auto self=static_cast<HostedDispatch*>(userdata);
    if(!self || !self->active_.load(std::memory_order_acquire) || !self->runtime_.handle || !self->domain_.entered())
        return static_cast<int32_t>(0xc000000du);
    if(worker_==self && !worker_admits(op)){
        char name[40];std::snprintf(name,sizeof(name),"replay-worker-op:%u",op);
        ddi_failure_note(name,E_INVALIDARG);return static_cast<int32_t>(0xc000000du);
    }
    HRESULT hr=self->operation(op,argument);
    if(hr==D3DDDIERR_DEVICEREMOVED || hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET || hr==DXGI_ERROR_DEVICE_HUNG){
        self->remove_device();return static_cast<int32_t>(0xc00002b6u);}
    if(hr==E_PENDING && (op==BC250_HOST_MapGpuVirtualAddress || op==BC250_HOST_MakeResident))return 0x103;
    if(hr==S_OK)return 0;
    return static_cast<int32_t>(hr==E_NOTIMPL?0xc00000bbu:hr==E_INVALIDARG?0xc000000du:hr==E_OUTOFMEMORY?0xc0000017u:0xc0000001u);
}
}
