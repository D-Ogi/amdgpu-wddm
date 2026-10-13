// SPDX-License-Identifier: MIT
// BD-133: the order of a copied Present against a dependency model of the GPU scheduler (present-copy-sync.h).
//
// The model is the WDDM software scheduler as ref\windows-driver-docs\...\display\context-monitoring.md:45-51 states
// it: each context runs its operations in order, a GPU wait holds everything behind it on its context until the fence
// reaches the value, and contexts run independently of each other. Nothing runs while the host calls the shell: the
// GPU advances only in Gpu::run(), so a host call that returns did not need the GPU, and a context that run() leaves
// with operations is blocked, which is how a cycle shows. A callback that only answers S_OK proves no progress; here
// every wait and signal the shell asks for becomes an operation of the model, and the tests read what finished.
// The kernel callbacks of the device add the present context's waits and signals and the copy signals, the engine
// substitute adds the copies and other work of executed lists, and the test adds the runtime's kernel presents and
// the application's own queue waits and signals. The code under test is the production code: the present context
// and present_copy_answer (present-context.h), the ticket records and the reuse rule (present-copy-sync.h) and the
// queue registry's execute (queue-engine.cpp). Only the engine's choice of surface is substituted, by the same pure
// rule the engine uses (present_copy_choose); engine-ddi\tests\test-linear-primary.cpp tests the engine's own.
#include "queue-engine.h"
#include "present-context.h"
#include "present-copy-sync.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace {
using native12::PresentCopyChoice;
using native12::PresentCopyListMark;
using native12::PresentCopySlot;
using native12::PresentCopyTicket;

const HANDLE kPresentContext=reinterpret_cast<HANDLE>(UINT_PTR{0x30000033});
const HANDLE kQueue1=reinterpret_cast<HANDLE>(UINT_PTR{0x10000011});
const HANDLE kQueue2=reinterpret_cast<HANDLE>(UINT_PTR{0x20000022});
constexpr D3DKMT_HANDLE kFirstFence=0x900;
constexpr unsigned kContexts=4,kOps=128,kFences=48,kLog=512,kSurfaces=24;

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr,"FAILED: %s\n",what);
    std::abort();
}

enum class Kind : unsigned char {Wait,Signal,Copy,Work,Present};
struct Op {Kind kind;D3DKMT_HANDLE fence;UINT64 value;unsigned id;};

struct Gpu {
    HANDLE contexts[kContexts]{};
    Op ops[kContexts][kOps]{};
    unsigned queued[kContexts]{},done[kContexts]{};
    UINT64 fences[kFences]{};
    unsigned fences_made{};
    Op log[kLog]{};                             // every operation that finished, in the order it finished
    unsigned logged{};

    unsigned context(HANDLE h) {
        for(unsigned i=0;i<kContexts;++i)if(contexts[i]==h)return i;
        for(unsigned i=0;i<kContexts;++i)if(!contexts[i]){contexts[i]=h;return i;}
        fail("too many contexts");
    }
    D3DKMT_HANDLE make_fence() {
        if(fences_made>=kFences)fail("too many fences");
        return kFirstFence+fences_made++;
    }
    UINT64& fence(D3DKMT_HANDLE h) {
        if(h<kFirstFence || h>=kFirstFence+fences_made)fail("no such fence");
        return fences[h-kFirstFence];
    }
    void push(HANDLE h,const Op& op) {
        const unsigned c=context(h);
        if(queued[c]>=kOps)fail("too many operations");
        ops[c][queued[c]++]=op;
    }
    // One context, in order, as far as its next wait lets it.
    bool advance(unsigned c) {
        bool moved=false;
        while(done[c]<queued[c]){
            const Op op=ops[c][done[c]];
            if(op.kind==Kind::Wait && fence(op.fence)<op.value)break;
            if(op.kind==Kind::Signal && fence(op.fence)<op.value)fence(op.fence)=op.value;
            if(logged>=kLog)fail("log full");
            log[logged++]=op;
            ++done[c];
            moved=true;
        }
        return moved;
    }
    // Every context until none can move.
    void run() {
        for(bool moved=true;moved;){
            moved=false;
            for(unsigned c=0;c<kContexts;++c)if(advance(c))moved=true;
        }
    }
    bool idle() const {
        for(unsigned c=0;c<kContexts;++c)if(done[c]<queued[c])return false;
        return true;
    }
    // Where a copy, a work or a kernel present finished in the log; -1 when it did not.
    int at(Kind kind,unsigned id) const {
        for(unsigned i=0;i<logged;++i)if(log[i].kind==kind && log[i].id==id)return static_cast<int>(i);
        return -1;
    }
    bool finished(Kind kind,unsigned id) const {return at(kind,id)>=0;}
    // The operations queued on a context so far.
    unsigned count(HANDLE h) const {
        for(unsigned i=0;i<kContexts;++i)if(contexts[i]==h)return queued[i];
        return 0;
    }
    const Op& op(HANDLE h,unsigned i) {
        const unsigned c=context(h);
        if(i>=queued[c])fail("no such operation");
        return ops[c][i];
    }
};

struct Fixture;
struct RuntimeQueue {Fixture* fixture;HANDLE context;bool live;};
struct FakeQueue {Fixture* fixture;RuntimeQueue* runtime;};
// A command list as the engine substitute sees it: its ticket record, and what it records: a copy (copy, the
// present's number) or other work (work).
struct FakeList {PresentCopyListMark mark;unsigned copy{};unsigned work{};};
struct Surface {PresentCopySlot slot;D3DKMT_HANDLE allocation{};};
struct Source {Surface* surfaces[native12::kPresentCopySurfacesPerSource]{};unsigned count{};};

HRESULT make_engine(engine_ddi::DeviceContext*,const BC250_VKD3D_COMMAND_QUEUE_DESC*,void*,
    engine_ddi::EngineQueue**) noexcept;
HRESULT execute_engine(engine_ddi::EngineQueue*,UINT,const D3D12DDI_HCOMMANDLIST*) noexcept;
HRESULT close_engine(engine_ddi::EngineQueue**) noexcept;
HRESULT engine_health(void*) noexcept;
HRESULT list_mark(engine_ddi::DeviceContext*,D3D12DDI_HCOMMANDLIST,PresentCopyListMark**) noexcept;

native12::QueueEngineOps engine_ops(Fixture* f) noexcept {
    native12::QueueEngineOps ops{make_engine,execute_engine,close_engine,engine_health,f};
    ops.present_copy=list_mark;
    return ops;
}

D3D12DDIARG_CREATECOMMANDQUEUE_0050 descriptor() {
    D3D12DDIARG_CREATECOMMANDQUEUE_0050 d{};
    d.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D|D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE|D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    return d;
}

struct Fixture {
    native12::Device device;
    native12::QueueEngineRegistry registry;
    RuntimeQueue runtime[2];
    native12::QueueEngineSlot q1{},q2{};
    Gpu gpu;
    Surface surfaces[kSurfaces];
    unsigned surfaces_made{};
    unsigned calls[64]{};                       // how many lists each engine call carried, in order
    unsigned call_count{};
    unsigned errors{},gpu_waits{},cpu_waits{},destroys{},records{},grows{},reuses{},refusals{};
    D3DKMT_HANDLE present_fence{};              // the first fence the kernel made: the present context's
    bool fail_queue_signal{};                   // a copy signal on a queue context fails
    bool forbid_destroy{};                      // R3: a destroy callback entered from here on fails the suite

    Fixture() : registry(device,reinterpret_cast<engine_ddi::DeviceContext*>(this),engine_ops(this)) {
        runtime[0]={this,kQueue1,false};
        runtime[1]={this,kQueue2,false};
        device.runtime.handle=this;
        device.callbacks.pfnSetErrorCb=[](D3D10DDI_HRTDEVICE h,HRESULT hr) {
            auto f=static_cast<Fixture*>(h.handle);
            if(!f->device.lost.load() || hr!=D3DDDIERR_DEVICEREMOVED)fail("an error report that is no device removal");
            ++f->errors;
        };
        device.callbacks.pfnCreateContextVirtualCb=[](D3D12DDI_HRTCOMMANDQUEUE h,D3DDDICB_CREATECONTEXTVIRTUAL* a)->HRESULT {
            auto r=static_cast<RuntimeQueue*>(h.handle);
            assert(!r->live && !a->hContext && a->PrivateDriverDataSize==sizeof(bc250_umd_context_private));
            r->live=true;a->hContext=r->context;return S_OK;
        };
        device.callbacks.pfnDestroyContextCb=[](D3D12DDI_HRTCOMMANDQUEUE h,const D3DDDICB_DESTROYCONTEXT* a)->HRESULT {
            auto r=static_cast<RuntimeQueue*>(h.handle);
            assert(r->live && a->hContext==r->context);
            r->live=false;return S_OK;
        };
        // The kernel callbacks of the device, as the present context and the copy signal use them: each one is an
        // operation of the model.
        D3DDDI_DEVICECALLBACKS& k=device.kernel_callbacks;
        k.pfnCreateContextVirtualCb=[](HANDLE,D3DDDICB_CREATECONTEXTVIRTUAL* a)->HRESULT {
            assert(!a->pPrivateDriverData && !a->PrivateDriverDataSize && a->EngineAffinity==1);
            a->hContext=kPresentContext;return S_OK;
        };
        k.pfnDestroyContextCb=[](HANDLE h,const D3DDDICB_DESTROYCONTEXT* a)->HRESULT {
            auto& f=*static_cast<Fixture*>(h);
            if(f.forbid_destroy)fail("a context whose retirement was not proven was destroyed");
            assert(a->hContext==kPresentContext);
            ++f.destroys;return S_OK;
        };
        k.pfnCreateSynchronizationObject2Cb=[](HANDLE h,D3DDDICB_CREATESYNCHRONIZATIONOBJECT2* a)->HRESULT {
            auto& f=*static_cast<Fixture*>(h);
            assert(a->Info.Type==D3DDDI_MONITORED_FENCE && a->Info.MonitoredFence.EngineAffinity==1);
            a->hSyncObject=f.gpu.make_fence();
            if(!f.present_fence)f.present_fence=a->hSyncObject;
            a->Info.MonitoredFence.FenceValueCPUVirtualAddress=&f.gpu.fence(a->hSyncObject);
            return S_OK;
        };
        k.pfnDestroySynchronizationObjectCb=[](HANDLE h,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT*)->HRESULT {
            auto& f=*static_cast<Fixture*>(h);
            if(f.forbid_destroy)fail("a fence whose retirement was not proven was destroyed");
            ++f.destroys;return S_OK;
        };
        k.pfnSignalSynchronizationObjectFromGpu2Cb=[](HANDLE h,const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2* a)->HRESULT {
            auto& f=*static_cast<Fixture*>(h);
            assert(a->ObjectCount==1 && a->BroadcastContextCount==1);
            const HANDLE context=a->BroadcastContextArray[0];
            if(f.fail_queue_signal && context!=kPresentContext)return E_FAIL;
            f.gpu.push(context,{Kind::Signal,a->ObjectHandleArray[0],a->MonitoredFenceValueArray[0],0});
            return S_OK;
        };
        k.pfnWaitForSynchronizationObjectFromGpuCb=[](HANDLE h,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a)->HRESULT {
            auto& f=*static_cast<Fixture*>(h);
            assert(a->ObjectCount==1);
            f.gpu.push(a->hContext,{Kind::Wait,a->ObjectHandleArray[0],a->MonitoredFenceValueArray[0],0});
            ++f.gpu_waits;return S_OK;
        };
        // The CPU waits while the GPU runs: the event is set when the fence reached the value, never otherwise.
        k.pfnWaitForSynchronizationObjectFromCpuCb=[](HANDLE h,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* a)->HRESULT {
            auto& f=*static_cast<Fixture*>(h);
            assert(a->ObjectCount==1 && a->hAsyncEvent);
            ++f.cpu_waits;
            f.gpu.run();
            if(f.gpu.fence(a->ObjectHandleArray[0])>=a->FenceValueArray[0])SetEvent(a->hAsyncEvent);
            return S_OK;
        };
        const auto args=descriptor();
        if(registry.create(args,{&runtime[0]},q1)!=S_OK || registry.create(args,{&runtime[1]},q2)!=S_OK)
            fail("queue creation");
    }
    Fixture(const Fixture&)=delete;
    Fixture& operator=(const Fixture&)=delete;
    // The copy signal queued on a context, its fence and value, as an operation of the model.
    bool signal_at(HANDLE context,unsigned i,const Surface& s,UINT64 value) {
        const Op& o=gpu.op(context,i);
        return o.kind==Kind::Signal && o.fence==s.slot.fence && o.value==value;
    }
    bool queued_at(HANDLE context,unsigned i,Kind kind,unsigned id) {
        const Op& o=gpu.op(context,i);
        return o.kind==kind && o.id==id;
    }
};

HRESULT engine_health(void*) noexcept {return S_OK;}
HRESULT make_engine(engine_ddi::DeviceContext* context,const BC250_VKD3D_COMMAND_QUEUE_DESC*,void* cookie,
    engine_ddi::EngineQueue** output) noexcept {
    auto f=reinterpret_cast<Fixture*>(context);
    RuntimeQueue* runtime=nullptr;
    const HRESULT hr=f->registry.with_binding(cookie,f->device,[](void* user,const native12::QueueBindingView* view)->HRESULT {
        *static_cast<RuntimeQueue**>(user)=static_cast<RuntimeQueue*>(view->runtime_queue.handle);
        return S_OK;
    },&runtime);
    if(hr!=S_OK || !runtime)return E_FAIL;
    *output=reinterpret_cast<engine_ddi::EngineQueue*>(new FakeQueue{f,runtime});
    return S_OK;
}
// The engine submits every list to the queue's bound context before it returns (engine-ddi.h,
// execute_command_lists, INLINE mode): the copies and the other work of the lists become operations of that context.
HRESULT execute_engine(engine_ddi::EngineQueue* engine,UINT count,const D3D12DDI_HCOMMANDLIST* lists) noexcept {
    auto q=reinterpret_cast<FakeQueue*>(engine);
    Fixture& f=*q->fixture;
    if(f.call_count>=64)fail("too many engine calls");
    f.calls[f.call_count++]=count;
    for(UINT i=0;i<count;++i){
        const auto l=static_cast<const FakeList*>(lists[i].pDrvPrivate);
        f.gpu.push(q->runtime->context,l->copy ? Op{Kind::Copy,0,0,l->copy} : Op{Kind::Work,0,0,l->work});
    }
    return S_OK;
}
HRESULT close_engine(engine_ddi::EngineQueue** engine) noexcept {
    delete reinterpret_cast<FakeQueue*>(*engine);
    *engine=nullptr;
    return S_OK;
}
HRESULT list_mark(engine_ddi::DeviceContext*,D3D12DDI_HCOMMANDLIST list,PresentCopyListMark** mark) noexcept {
    if(!mark)return E_INVALIDARG;
    *mark=nullptr;
    if(!list.pDrvPrivate)return E_INVALIDARG;
    *mark=&static_cast<FakeList*>(list.pDrvPrivate)->mark;
    return S_OK;
}

// The engine half of pfnPresent: the list's ticket record, and the copy surface the source's pool gives by the
// engine's rule (present_copy_choose), prepared before the copy is recorded, as resources.cpp does.
struct PresentCall {Fixture* f;FakeList* list;Source* source;unsigned copy;};
HRESULT call_mark_of(void* state,PresentCopyListMark** mark) noexcept {
    *mark=&static_cast<PresentCall*>(state)->list->mark;
    return S_OK;
}
HRESULT call_record(void* state,const native12::PresentCopyPick& pick,PresentCopySlot** slot,
    D3DKMT_HANDLE* allocation) noexcept {
    auto& c=*static_cast<PresentCall*>(state);
    Fixture& f=*c.f;
    Source& s=*c.source;
    *slot=nullptr;
    *allocation=0;
    const PresentCopySlot* view[native12::kPresentCopySurfacesPerSource]{};
    for(unsigned i=0;i<s.count;++i)view[i]=&s.surfaces[i]->slot;
    unsigned index=0;
    Surface* surface=nullptr;
    switch(native12::present_copy_choose(view,s.count,pick.present_cpu,&index)){
    case PresentCopyChoice::Reuse:
        surface=s.surfaces[index];
        ++f.reuses;
        break;
    case PresentCopyChoice::Grow:
        if(f.surfaces_made>=kSurfaces)fail("too many surfaces");
        surface=&f.surfaces[f.surfaces_made++];
        surface->allocation=0x40000100+f.surfaces_made;
        s.surfaces[s.count++]=surface;
        ++f.grows;
        break;
    case PresentCopyChoice::Budget:
        ++f.refusals;
        return E_OUTOFMEMORY;
    case PresentCopyChoice::Removed:
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if(!surface)return E_UNEXPECTED;
    const HRESULT hr=pick.prepare(pick.state,surface->slot);
    if(hr!=S_OK)return hr;
    c.list->copy=c.copy;
    ++f.records;
    *slot=&surface->slot;
    *allocation=surface->allocation;
    return S_OK;
}
// What the reset or destroy of a list with an unexecuted ticket calls, as native-tables.cpp's present_copy_cancelled.
void cancelled(void* owner,const PresentCopyTicket& ticket) noexcept {
    auto& device=*static_cast<native12::Device*>(owner);
    device.present.cancel(ticket);
    device.remove();
}

// pfnPresent of a copied source, number copy, recorded on list: present_copy_answer as native-tables.cpp calls it,
// then (kernel) the runtime's kernel present on the context the present was answered with.
HRESULT present(Fixture& f,FakeList& list,Source& source,unsigned copy,bool kernel=true) {
    PresentCall call{&f,&list,&source,copy};
    const native12::PresentCopyEngine engine{&call,call_mark_of,call_record};
    D3DKMT_HANDLE allocation=0;
    HANDLE context=nullptr;
    unsigned stage=0;
    const unsigned waits=f.gpu_waits,records=f.records;
    const HRESULT hr=native12::present_copy_answer(f.device.present,&f,f.device.kernel_callbacks,engine,cancelled,
        &f.device,&allocation,&context,stage);
    if(hr!=S_OK){
        // A refused present names nothing, recorded nothing and queued no wait.
        if(allocation || context || f.gpu_waits!=waits || f.records!=records)fail("a refused present left work");
        return hr;
    }
    if(!allocation || context!=kPresentContext || f.gpu_waits!=waits+1 || f.records!=records+1 ||
       list.mark.state()!=PresentCopyListMark::Pending)
        fail("an answered present without its ticket");
    if(kernel)f.gpu.push(context,{Kind::Present,0,0,copy});
    return S_OK;
}
void kernel_present(Fixture& f,unsigned copy) {f.gpu.push(kPresentContext,{Kind::Present,0,0,copy});}
HRESULT execute(Fixture& f,const native12::QueueEngineSlot& queue,std::initializer_list<FakeList*> lists) {
    D3D12DDI_HCOMMANDLIST handles[16]{};
    UINT n=0;
    for(FakeList* l:lists){
        if(n>=16)fail("too many lists");
        handles[n++]={l};
    }
    return f.registry.execute(queue,n,handles);
}
// The runtime's Reset of a list.
void reset(FakeList& list) {list.mark.release();list.copy=0;}
// Every present of 1..n finished, each after its own copy.
bool presents_follow_copies(const Gpu& gpu,unsigned first,unsigned last) {
    for(unsigned n=first;n<=last;++n){
        const int copy=gpu.at(Kind::Copy,n),shown=gpu.at(Kind::Present,n);
        if(copy<0 || shown<0 || shown<copy)return false;
    }
    return true;
}
} // namespace

int main() {
    // 1. Present before execute, and execute before the kernel present: the kernel present waits on the GPU for its
    // own copy, whichever the host does first; the copy's signal follows the copy on the queue that executed it.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1;
        assert(present(*f,l1,s,1)==S_OK && s.count==1 && f->grows==1);
        const PresentCopyTicket t=l1.mark.ticket();
        assert(t.slot==&s.surfaces[0]->slot && t.value==1);
        f->gpu.run();
        assert(!f->gpu.finished(Kind::Present,1) && !f->gpu.idle());        // held by its wait: no copy yet
        assert(execute(*f,f->q1,{&l1})==S_OK && l1.mark.state()==PresentCopyListMark::Executed);
        assert(f->gpu.count(kQueue1)==2 && f->queued_at(kQueue1,0,Kind::Copy,1) && f->signal_at(kQueue1,1,*s.surfaces[0],1));
        assert(!f->cpu_waits && !f->gpu.finished(Kind::Copy,1));            // the execute did not wait for the GPU
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,1) && !f->errors);
        // The source's destroy proves both witnesses on the CPU and releases the fence; DestroyDevice then closes.
        PresentCopySlot* pool[]={&s.surfaces[0]->slot};
        assert(f->device.present.retire(pool,1,1000)==S_OK && !s.surfaces[0]->slot.fence && f->destroys==1);
        assert(f->device.present.close(1000)==S_OK && f->destroys==3 && !f->device.lost);
    }
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1;
        assert(present(*f,l1,s,1,false)==S_OK);
        assert(execute(*f,f->q2,{&l1})==S_OK && f->signal_at(kQueue2,1,*s.surfaces[0],1) && !f->gpu.count(kQueue1));
        f->gpu.run();
        assert(f->gpu.finished(Kind::Copy,1) && f->gpu.idle());
        kernel_present(*f,1);
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,1));
    }
    std::puts("PASSED: a copied present waits on the GPU for its own copy, present before execute and execute before "
              "present; the copy is signaled on the queue that executed it, and Execute never waits on the CPU");

    // 2. Two marked lists in one Execute, among unmarked ones: split after each marked list, each signal right after
    // its copy, the unmarked lists in their order. An Execute without a marked list is the engine call alone.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList u1,u2,u3,m1,m2;
        u1.work=11;u2.work=12;u3.work=13;
        assert(present(*f,m1,s,1)==S_OK && present(*f,m2,s,2)==S_OK && s.count==2);
        assert(execute(*f,f->q1,{&u1,&m1,&u2,&m2,&u3})==S_OK);
        assert(f->call_count==3 && f->calls[0]==2 && f->calls[1]==2 && f->calls[2]==1);
        assert(f->gpu.count(kQueue1)==7);
        assert(f->queued_at(kQueue1,0,Kind::Work,11) && f->queued_at(kQueue1,1,Kind::Copy,1));
        assert(f->signal_at(kQueue1,2,*s.surfaces[0],1) && f->queued_at(kQueue1,3,Kind::Work,12));
        assert(f->queued_at(kQueue1,4,Kind::Copy,2) && f->signal_at(kQueue1,5,*s.surfaces[1],1));
        assert(f->queued_at(kQueue1,6,Kind::Work,13));
        assert(m1.mark.state()==PresentCopyListMark::Executed && m2.mark.state()==PresentCopyListMark::Executed);
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,2));
        reset(u1);reset(u2);
        assert(execute(*f,f->q1,{&u1,&u2})==S_OK && f->call_count==4 && f->calls[3]==2 && f->gpu.count(kQueue1)==9);
        assert(f->queued_at(kQueue1,7,Kind::Work,11) && f->queued_at(kQueue1,8,Kind::Work,12) && !f->errors);
    }
    std::puts("PASSED: two marked lists in one Execute are split after each, each copy signaled right after it; an "
              "Execute without one is the engine call alone");

    // 3. The application fence F of the BD-133 review of 2026-10-12 (the C3 cycle through an application's wait):
    // Q1 waits for F = 1, ticket 1 is executed on Q1, ticket 2 on Q2, then Q2 signals F = 1. Both host Execute calls
    // return before either copy finishes, and with a completion fence per surface everything finishes.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,l2;
        assert(present(*f,l1,s,1)==S_OK && present(*f,l2,s,2)==S_OK && s.count==2);
        const D3DKMT_HANDLE F=f->gpu.make_fence();
        f->gpu.push(kQueue1,{Kind::Wait,F,1,0});
        assert(execute(*f,f->q1,{&l1})==S_OK);
        assert(execute(*f,f->q2,{&l2})==S_OK);
        f->gpu.push(kQueue2,{Kind::Signal,F,1,0});
        assert(!f->gpu.finished(Kind::Copy,1) && !f->gpu.finished(Kind::Copy,2) && !f->cpu_waits);
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,2));
        assert(f->gpu.at(Kind::Copy,2)<f->gpu.at(Kind::Copy,1));
        assert(f->gpu.at(Kind::Present,1)<f->gpu.at(Kind::Present,2) && !f->errors);
    }
    {   // The negative control: the C3 form of the earlier design, a wait on Q2 for copy 1 before Q2 signals F,
        // closes the cycle, and the model shows it: copy 1 and both presents never finish.
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,l2;
        assert(present(*f,l1,s,1)==S_OK && present(*f,l2,s,2)==S_OK);
        const D3DKMT_HANDLE F=f->gpu.make_fence();
        f->gpu.push(kQueue1,{Kind::Wait,F,1,0});
        assert(execute(*f,f->q1,{&l1})==S_OK && execute(*f,f->q2,{&l2})==S_OK);
        f->gpu.push(kQueue2,{Kind::Wait,s.surfaces[0]->slot.fence,1,0});
        f->gpu.push(kQueue2,{Kind::Signal,F,1,0});
        f->gpu.run();
        assert(!f->gpu.idle() && f->gpu.finished(Kind::Copy,2) && !f->gpu.finished(Kind::Copy,1));
        assert(!f->gpu.finished(Kind::Present,1) && !f->gpu.finished(Kind::Present,2));
    }
    std::puts("PASSED: the application-fence graph of the review finishes both copies and both presents, both Execute "
              "calls returning first; the C3 form of it blocks in the same model");

    // 4. Out of order on two queues: copy 2 finishes first, and present 1 still waits for copy 1 (no shared fence
    // that copy 2 could satisfy for it), present 2 behind it in its context's order.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,l2;
        assert(present(*f,l1,s,1)==S_OK && present(*f,l2,s,2)==S_OK);
        const D3DKMT_HANDLE F=f->gpu.make_fence();
        f->gpu.push(kQueue1,{Kind::Wait,F,1,0});
        assert(execute(*f,f->q1,{&l1})==S_OK && execute(*f,f->q2,{&l2})==S_OK);
        f->gpu.run();
        assert(f->gpu.finished(Kind::Copy,2) && !f->gpu.finished(Kind::Copy,1));
        assert(!f->gpu.finished(Kind::Present,1) && !f->gpu.finished(Kind::Present,2));
        f->gpu.fence(F)=1;                      // the application signals F from the CPU
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,2));
    }
    std::puts("PASSED: out of order on two queues, present N still waits for copy N");

    // 5. All eight surfaces of a source with readers not retired: copies 2..8 finished, copy 1 not executed, so the
    // present context holds at its first wait and no reader retires. The ninth present is refused: no new GPU wait,
    // nothing recorded, no surface, no output, its list without a ticket. Then the readers retire and a present
    // reuses a surface without growth.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l[10];
        for(unsigned i=0;i<8;++i)assert(present(*f,l[i],s,i+1)==S_OK);
        assert(s.count==8 && f->grows==8);
        for(unsigned i=1;i<8;++i)assert(execute(*f,i&1 ? f->q2 : f->q1,{&l[i]})==S_OK);
        f->gpu.run();
        for(unsigned i=2;i<=8;++i)assert(f->gpu.finished(Kind::Copy,i) && !f->gpu.finished(Kind::Present,i));
        assert(!f->gpu.finished(Kind::Present,1));
        const unsigned waits=f->gpu_waits,made=f->surfaces_made;
        assert(present(*f,l[8],s,9)==E_OUTOFMEMORY);
        assert(f->refusals==1 && s.count==8 && f->surfaces_made==made && f->gpu_waits==waits);
        assert(l[8].mark.state()==PresentCopyListMark::Empty && !l[8].copy && !f->device.lost);
        // The refusal left nothing that holds the GPU: once copy 1 runs, the eight presents finish in order.
        assert(execute(*f,f->q1,{&l[0]})==S_OK);
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,8));
        for(unsigned i=1;i<8;++i)assert(f->gpu.at(Kind::Present,i)<f->gpu.at(Kind::Present,i+1));
        // Both witnesses passed for surface 1 (its copy and its reader, read on the CPU): it is reused, no growth.
        assert(present(*f,l[9],s,10)==S_OK && f->reuses==1 && f->grows==8 && s.count==8);
        assert(l[9].mark.ticket().slot==&s.surfaces[0]->slot && l[9].mark.ticket().value==2);
        assert(execute(*f,f->q1,{&l[9]})==S_OK);
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,10,10));
    }
    std::puts("PASSED: with all eight surfaces unretired a ninth present is refused with no new wait and no output; "
              "after retirement a surface is reused without growth");

    // 6. The ordinary frame loop: one list, reset every frame. Two surfaces carry it, reused alternately, never more.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l;
        for(unsigned n=1;n<=24;++n){
            reset(l);
            assert(present(*f,l,s,n)==S_OK);
            assert(execute(*f,n&1 ? f->q1 : f->q2,{&l})==S_OK);
            f->gpu.run();
        }
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,24));
        assert(s.count==2 && f->grows==2 && f->reuses==22 && !f->errors && !f->cpu_waits);
        PresentCopySlot* pool[]={&s.surfaces[0]->slot,&s.surfaces[1]->slot};
        assert(f->device.present.retire(pool,2,1000)==S_OK && !pool[0]->fence && !pool[1]->fence);
        assert(f->device.present.close(1000)==S_OK);
    }
    std::puts("PASSED: a frame loop of 24 presents runs on two surfaces, each reused only after its copy and reader");

    // 7. UINT64_MAX is never a witness: a copy fence that reads it makes the present fail as removed, without a
    // wait or a new surface (it would otherwise pass "copied >= issued"); a present fence that reads it as well.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,l2;
        assert(present(*f,l1,s,1)==S_OK && execute(*f,f->q1,{&l1})==S_OK);
        f->gpu.run();
        f->gpu.fence(s.surfaces[0]->slot.fence)=UINT64_MAX;
        assert(present(*f,l2,s,2)==DXGI_ERROR_DEVICE_REMOVED && s.count==1 && f->grows==1 && !f->reuses);
        assert(l2.mark.state()==PresentCopyListMark::Empty);
    }
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,l2;
        assert(present(*f,l1,s,1)==S_OK && execute(*f,f->q1,{&l1})==S_OK);
        f->gpu.run();
        f->gpu.fence(f->present_fence)=UINT64_MAX;
        assert(present(*f,l2,s,2)==DXGI_ERROR_DEVICE_REMOVED && s.count==1 && !f->reuses);
    }
    std::puts("PASSED: a fence that reads UINT64_MAX is no reuse witness: the present fails as removed, with no wait");

    // 8. Rule D. Ticket 2's list is reset while ticket 1 is not signaled: the device is removed; nothing is released
    // (no signal from the CPU or the GPU, present 1 still held), nothing retires, and nothing is destroyed.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,l2;
        assert(present(*f,l1,s,1)==S_OK && present(*f,l2,s,2)==S_OK && s.count==2);
        const unsigned on_present=f->gpu.count(kPresentContext);
        reset(l2);
        assert(f->device.lost && f->errors==1 && f->device.present.kept() && s.surfaces[1]->slot.retained.load());
        assert(f->device.present.counts().cancelled==1 && f->gpu.count(kPresentContext)==on_present);
        assert(!f->gpu.count(kQueue1) && !f->gpu.count(kQueue2));
        f->gpu.run();
        assert(!f->gpu.finished(Kind::Present,1) && !f->gpu.finished(Kind::Present,2));
        // The removed device takes no execute: ticket 1 is never signaled.
        assert(execute(*f,f->q1,{&l1})==D3DDDIERR_DEVICEREMOVED && !f->gpu.count(kQueue1));
        f->forbid_destroy=true;
        PresentCopySlot* pool[]={&s.surfaces[0]->slot,&s.surfaces[1]->slot};
        assert(f->device.present.retire(pool,2,20)==DXGI_ERROR_DEVICE_REMOVED);
        assert(pool[0]->retained.load() && pool[1]->retained.load() && pool[0]->fence && pool[1]->fence);
        assert(f->device.present.close(20)==DXGI_ERROR_DEVICE_REMOVED && !f->destroys && !f->cpu_waits);
        // A later present on the device is refused before anything is recorded.
        FakeList l3;
        assert(present(*f,l3,s,3)==DXGI_ERROR_DEVICE_REMOVED);
    }
    {   // The first copy cancelled, then DestroyDevice: the context is kept, never destroyed, without a wait.
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1;
        assert(present(*f,l1,s,1)==S_OK);
        reset(l1);
        assert(f->device.lost && f->errors==1 && f->device.present.kept());
        f->forbid_destroy=true;
        assert(f->device.present.close(20)==DXGI_ERROR_DEVICE_REMOVED && !f->destroys && !f->cpu_waits);
        assert(f->device.present.close(20)==DXGI_ERROR_DEVICE_REMOVED);
    }
    std::puts("PASSED: rule D, a list reset or destroyed with an unexecuted ticket removes the device and releases, "
              "retires and destroys nothing, also at the first copy and at DestroyDevice");

    // 9. A ticket is signaled once. A list executed again before its Reset, or twice in one call, is refused before
    // anything is submitted, and the device is removed. A Reset after the execute frees the list, which then carries
    // no ticket: its execute signals nothing, and its next present takes a new ticket.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1;
        assert(present(*f,l1,s,1)==S_OK && execute(*f,f->q1,{&l1})==S_OK && f->gpu.count(kQueue1)==2);
        assert(execute(*f,f->q1,{&l1})==D3DDDIERR_DEVICEREMOVED);
        assert(f->device.lost && f->errors==1 && f->call_count==1 && f->gpu.count(kQueue1)==2);
    }
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1,u;
        assert(present(*f,l1,s,1)==S_OK);
        assert(execute(*f,f->q1,{&u,&l1,&l1})==D3DDDIERR_DEVICEREMOVED);
        assert(f->device.lost && !f->call_count && !f->gpu.count(kQueue1));
    }
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1;
        l1.work=21;
        assert(present(*f,l1,s,1)==S_OK && execute(*f,f->q1,{&l1})==S_OK);
        f->gpu.run();
        reset(l1);
        assert(!f->errors && l1.mark.state()==PresentCopyListMark::Empty);
        assert(execute(*f,f->q1,{&l1})==S_OK && f->gpu.count(kQueue1)==3 && f->queued_at(kQueue1,2,Kind::Work,21));
        reset(l1);
        assert(present(*f,l1,s,2)==S_OK);
        const PresentCopyTicket t=l1.mark.ticket();
        assert(!(t.slot==&s.surfaces[0]->slot && t.value==1));      // never the satisfied ticket again
        assert(execute(*f,f->q1,{&l1})==S_OK);
        f->gpu.run();
        assert(f->gpu.idle() && presents_follow_copies(f->gpu,1,2) && !f->errors);
    }
    std::puts("PASSED: a list executed again before its Reset, or twice in one call, is refused before submission; "
              "a Reset never lets a satisfied ticket be signaled or used again");

    // 10. R2: a copy signal that cannot be queued removes the device; its present stays held, and DestroyDevice keeps
    // the context without a wait or a destroy, because no queue signaled its ticket.
    {
        auto f=std::make_unique<Fixture>();
        Source s;FakeList l1;
        assert(present(*f,l1,s,1)==S_OK);
        f->fail_queue_signal=true;
        assert(execute(*f,f->q1,{&l1})==E_FAIL && f->device.lost && f->errors==1);
        f->gpu.run();
        assert(f->gpu.finished(Kind::Copy,1) && !f->gpu.finished(Kind::Present,1));
        f->forbid_destroy=true;
        assert(f->device.present.close(20)==DXGI_ERROR_DEVICE_REMOVED && !f->destroys && !f->cpu_waits);
    }
    std::puts("PASSED: R2, a copy signal that fails removes the device, never a silent S_OK");

    // 11. R1: twelve outstanding tickets on two devices, each in its own list's record. Nothing is pushed out; the
    // waits hold until the copies run; executed out of order on both queues of each device, every present follows
    // its own copy.
    {
        auto a=std::make_unique<Fixture>(),b=std::make_unique<Fixture>();
        Source sa,sb;FakeList la[8],lb[4];
        for(unsigned i=0;i<8;++i)assert(present(*a,la[i],sa,i+1)==S_OK);
        for(unsigned i=0;i<4;++i)assert(present(*b,lb[i],sb,101+i)==S_OK);
        for(unsigned i=0;i<8;++i){
            const PresentCopyTicket t=la[i].mark.ticket();
            assert(la[i].mark.state()==PresentCopyListMark::Pending && t.slot==&sa.surfaces[i]->slot && t.value==1);
        }
        for(unsigned i=0;i<4;++i)
            assert(lb[i].mark.state()==PresentCopyListMark::Pending && lb[i].mark.ticket().slot==&sb.surfaces[i]->slot);
        a->gpu.run();b->gpu.run();
        for(unsigned i=1;i<=8;++i)assert(!a->gpu.finished(Kind::Present,i));
        for(unsigned i=101;i<=104;++i)assert(!b->gpu.finished(Kind::Present,i));
        for(const unsigned i:{7u,5u,3u,1u})assert(execute(*a,a->q2,{&la[i]})==S_OK);
        for(const unsigned i:{6u,4u,2u,0u})assert(execute(*a,a->q1,{&la[i]})==S_OK);
        for(const unsigned i:{3u,2u,1u,0u})assert(execute(*b,i&1 ? b->q1 : b->q2,{&lb[i]})==S_OK);
        a->gpu.run();b->gpu.run();
        assert(a->gpu.idle() && b->gpu.idle());
        assert(presents_follow_copies(a->gpu,1,8) && presents_follow_copies(b->gpu,101,104));
        assert(!a->errors && !b->errors && !a->device.lost && !b->device.lost);
    }
    std::puts("PASSED: R1, twelve outstanding tickets on two devices: none pushed out, each present after its copy");
    return 0;
}
