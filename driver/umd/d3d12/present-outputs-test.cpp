// SPDX-License-Identifier: MIT
#include "present-outputs.h"
#include "present-copy.h"
#include "present-copy-sync.h"
#include "present-context.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
const HANDLE kPresentContext=reinterpret_cast<HANDLE>(UINT_PTR{0x30000033});
const HANDLE kQueueContext=reinterpret_cast<HANDLE>(UINT_PTR{0x10000011});
const HANDLE kOtherQueueContext=reinterpret_cast<HANDLE>(UINT_PTR{0x20000022});
// Fence handles in the order the fake makes them: a context's present fence first, then the copy fences.
constexpr D3DKMT_HANDLE kFirstFence=0x900,kPresentFence=kFirstFence,kCopyFence=kFirstFence+1;
constexpr unsigned kFakeFences=8;
// The kernel callbacks of a device, as far as the present context uses them: each call is recorded in order, as an
// operation letter (C/c create/destroy context, F/f create/destroy fence, S GPU signal, W GPU wait, P CPU wait), the
// context it names, its object and its value. The device handle is this object. The fake has no CPU signal callback:
// rule D never needs one.
struct FakeKernel {
    struct Event {char op;HANDLE context;D3DKMT_HANDLE object;UINT64 value;};
    enum CpuWait {Complete,Wake,Never};         // the CPU wait reaches its value, wakes with wake_value, or never ends
    Event events[48]{};
    unsigned count{},contexts_live{},fences_live{},fences_made{};
    HRESULT create_hr{S_OK},fence_hr{S_OK},signal_hr{S_OK},wait_hr{S_OK};
    CpuWait cpu_wait{Complete};
    UINT64 wake_value{};
    bool forbid_destroy{};                      // R3: a destroy callback entered from here on fails the suite
    UINT64 values[kFakeFences]{};               // what each fence's CPU address reads
    bool live[kFakeFences]{};
    D3DDDI_DEVICECALLBACKS cb{};
    UINT64& value(D3DKMT_HANDLE fence) {
        assert(fence>=kFirstFence && fence<kFirstFence+kFakeFences);
        return values[fence-kFirstFence];
    }
    void push(char op,HANDLE context,D3DKMT_HANDLE object,UINT64 v) {
        assert(count<48);events[count++]={op,context,object,v};
    }
    bool is(unsigned i,char op,HANDLE context,D3DKMT_HANDLE object,UINT64 v) const {
        return i<count && events[i].op==op && events[i].context==context && events[i].object==object &&
            events[i].value==v;
    }
    bool ops(unsigned first,const char* expected) const {
        for(unsigned i=0;expected[i];++i)if(first+i>=count || events[first+i].op!=expected[i])return false;
        return true;
    }
    FakeKernel() {
        cb.pfnCreateContextVirtualCb=[](HANDLE h,D3DDDICB_CREATECONTEXTVIRTUAL* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(!a->hContext && a->NodeOrdinal==0 && a->EngineAffinity==1 && !a->Flags.Value);
            assert(!a->pPrivateDriverData && !a->PrivateDriverDataSize);    // what makes it no UMD context
            k.push('C',nullptr,0,0);
            if(FAILED(k.create_hr))return k.create_hr;
            ++k.contexts_live;a->hContext=kPresentContext;return S_OK;
        };
        cb.pfnDestroyContextCb=[](HANDLE h,const D3DDDICB_DESTROYCONTEXT* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(!k.forbid_destroy && a->hContext==kPresentContext && k.contexts_live);
            k.push('c',a->hContext,0,0);--k.contexts_live;return S_OK;
        };
        cb.pfnCreateSynchronizationObject2Cb=[](HANDLE h,D3DDDICB_CREATESYNCHRONIZATIONOBJECT2* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(a->Info.Type==D3DDDI_MONITORED_FENCE && a->Info.MonitoredFence.EngineAffinity==1 && !a->hSyncObject);
            k.push('F',nullptr,0,0);
            if(FAILED(k.fence_hr))return k.fence_hr;
            assert(k.fences_made<kFakeFences);
            const D3DKMT_HANDLE fence=kFirstFence+k.fences_made++;
            ++k.fences_live;k.live[fence-kFirstFence]=true;a->hSyncObject=fence;
            a->Info.MonitoredFence.FenceValueCPUVirtualAddress=&k.value(fence);return S_OK;
        };
        cb.pfnDestroySynchronizationObjectCb=[](HANDLE h,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(!k.forbid_destroy && k.fences_live);
            assert(a->hSyncObject>=kFirstFence && a->hSyncObject<kFirstFence+kFakeFences);
            assert(k.live[a->hSyncObject-kFirstFence]);
            k.live[a->hSyncObject-kFirstFence]=false;
            k.push('f',nullptr,a->hSyncObject,0);--k.fences_live;return S_OK;
        };
        cb.pfnSignalSynchronizationObjectFromGpu2Cb=[](HANDLE h,const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(a->ObjectCount==1 && a->BroadcastContextCount==1 && !a->Flags.Value);
            k.push('S',a->BroadcastContextArray[0],a->ObjectHandleArray[0],a->MonitoredFenceValueArray[0]);
            return k.signal_hr;
        };
        cb.pfnWaitForSynchronizationObjectFromGpuCb=[](HANDLE h,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(a->ObjectCount==1);
            k.push('W',a->hContext,a->ObjectHandleArray[0],a->MonitoredFenceValueArray[0]);
            return k.wait_hr;
        };
        cb.pfnWaitForSynchronizationObjectFromCpuCb=[](HANDLE h,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* a)->HRESULT {
            auto& k=*static_cast<FakeKernel*>(h);
            assert(a->ObjectCount==1 && a->hAsyncEvent);
            k.push('P',nullptr,a->ObjectHandleArray[0],a->FenceValueArray[0]);
            if(k.cpu_wait==Complete){k.value(a->ObjectHandleArray[0])=a->FenceValueArray[0];SetEvent(a->hAsyncEvent);}
            else if(k.cpu_wait==Wake){k.value(a->ObjectHandleArray[0])=k.wake_value;SetEvent(a->hAsyncEvent);}
            return S_OK;
        };
    }
};
} // namespace

namespace {
void reuse_rule() {
    using namespace native12;
    // BD-133: the reuse rule of a copy surface and the pool's budget (present-copy-sync.h).
    {
        UINT64 copied[kPresentCopySurfacesPerSource]{};
        PresentCopySlot slots[kPresentCopySurfacesPerSource];
        const PresentCopySlot* view[kPresentCopySurfacesPerSource]{};
        for(unsigned i=0;i<kPresentCopySurfacesPerSource;++i)view[i]=&slots[i];
        // A surface never copied into is reusable, whatever the present fence reads (but UINT64_MAX).
        assert(present_copy_reuse(slots[0],0)==PresentCopyReuse::Reusable);
        assert(present_copy_reuse(slots[0],UINT64_MAX)==PresentCopyReuse::Removed);
        // Ticket k = 3 read by the present behind present fence value 5: busy until both are witnessed on the CPU.
        slots[0].fence=0x901;slots[0].fence_cpu=&copied[0];slots[0].issued=3;slots[0].reader=5;
        copied[0]=2;
        assert(present_copy_reuse(slots[0],9)==PresentCopyReuse::Busy);         // its copy is outstanding
        copied[0]=3;
        assert(present_copy_reuse(slots[0],4)==PresentCopyReuse::Busy);         // its reader is outstanding
        assert(present_copy_reuse(slots[0],5)==PresentCopyReuse::Reusable);
        // UINT64_MAX, the removed device's value, is never a witness (R4): not of the copy, not of the reader.
        copied[0]=UINT64_MAX;
        assert(present_copy_reuse(slots[0],5)==PresentCopyReuse::Removed);
        copied[0]=3;
        assert(present_copy_reuse(slots[0],UINT64_MAX)==PresentCopyReuse::Removed);
        // A kept surface is never reused, even when both witnesses pass.
        slots[0].retained.store(true);
        assert(present_copy_reuse(slots[0],5)==PresentCopyReuse::Busy);
        slots[0].retained.store(false);
        // The choice: the first reusable surface; else another one while the source has fewer than the pool; else
        // the budget refusal; and the removed device whatever else the pool holds.
        unsigned index=99;
        assert(present_copy_choose(view,0,0,&index)==PresentCopyChoice::Grow && index==0);
        for(unsigned i=0;i<kPresentCopySurfacesPerSource;++i){
            slots[i].fence=0x901+i;slots[i].fence_cpu=&copied[i];slots[i].issued=1;slots[i].reader=10+i;copied[i]=1;
        }
        assert(present_copy_choose(view,kPresentCopySurfacesPerSource-1,9,&index)==PresentCopyChoice::Grow);
        assert(present_copy_choose(view,kPresentCopySurfacesPerSource,9,&index)==PresentCopyChoice::Budget);
        assert(present_copy_choose(view,kPresentCopySurfacesPerSource,12,&index)==PresentCopyChoice::Reuse &&
               index==0);
        copied[0]=0;
        assert(present_copy_choose(view,kPresentCopySurfacesPerSource,12,&index)==PresentCopyChoice::Reuse &&
               index==1);
        copied[5]=UINT64_MAX;
        assert(present_copy_choose(view,kPresentCopySurfacesPerSource,9,&index)==PresentCopyChoice::Removed);
        assert(present_copy_choose(view,kPresentCopySurfacesPerSource,UINT64_MAX,&index)==PresentCopyChoice::Removed);
        // The device's byte budget, at its edges.
        static_assert(kPresentCopySurfacesPerSource==8 && kPresentCopyDeviceBytes==(uint64_t{512}<<20));
        assert(present_copy_within_budget(0,kPresentCopyDeviceBytes));
        assert(!present_copy_within_budget(1,kPresentCopyDeviceBytes));
        assert(present_copy_within_budget(kPresentCopyDeviceBytes-4096,4096));
        assert(!present_copy_within_budget(kPresentCopyDeviceBytes-4095,4096));
        assert(!present_copy_within_budget(0,kPresentCopyDeviceBytes+1));
        assert(!present_copy_within_budget(UINT64_MAX,1));
        // The fence values that may be signaled or waited for (R5).
        assert(!present_fence_value_valid(0) && !present_fence_value_valid(UINT64_MAX));
        assert(present_fence_value_valid(1) && present_fence_value_valid(UINT64_MAX-1));
    }
    std::puts("PASSED: a copy surface is reused only when its last copy completed and its last reader retired, read "
              "on the CPU, never on UINT64_MAX; the pool grows to 8 a source and 512 MiB a device, then refuses");
}

void list_ticket_record() {
    using namespace native12;
    // BD-133, R1: the ticket a list carries, in the list's own record. One ticket a list, taken once by an execute,
    // released by a Reset or a destroy; a release of an unexecuted ticket is rule D's cancellation.
    {
        struct Cancelled {unsigned calls;UINT64 value;void* owner;};
        Cancelled seen{};
        const PresentCopyCancel cancel=[](void* owner,const PresentCopyTicket& ticket) noexcept {
            auto& s=*static_cast<Cancelled*>(owner);++s.calls;s.value=ticket.value;s.owner=owner;
        };
        PresentCopySlot slot;
        PresentCopyListMark mark;
        PresentCopyTicket taken{};
        assert(mark.state()==PresentCopyListMark::Empty && !mark.take(&taken) && !taken.slot);
        // Malformed tickets are not marked: no surface, value 0 or UINT64_MAX, no cancel hook.
        assert(!mark.mark({nullptr,1},cancel,&seen) && !mark.mark({&slot,0},cancel,&seen));
        assert(!mark.mark({&slot,UINT64_MAX},cancel,&seen) && !mark.mark({&slot,1},nullptr,&seen));
        assert(mark.state()==PresentCopyListMark::Empty);
        // Pending, and a second present on the list is refused while it is.
        assert(mark.mark({&slot,1},cancel,&seen) && mark.state()==PresentCopyListMark::Pending);
        assert(!mark.mark({&slot,2},cancel,&seen) && mark.ticket().value==1);
        // The execute takes it once; the list is Executed until its Reset, and takes no new ticket.
        assert(mark.take(&taken) && taken.slot==&slot && taken.value==1 && mark.state()==PresentCopyListMark::Executed);
        assert(!mark.take(&taken) && !taken.slot);
        assert(!mark.mark({&slot,2},cancel,&seen));
        // A Reset after the execute cancels nothing.
        mark.release();
        assert(mark.state()==PresentCopyListMark::Empty && !seen.calls);
        // A Reset or a destroy of a list whose ticket was never executed: the cancel hook, once, with that ticket.
        assert(mark.mark({&slot,7},cancel,&seen));
        mark.release();
        assert(seen.calls==1 && seen.value==7 && seen.owner==&seen && mark.state()==PresentCopyListMark::Empty);
        mark.release();
        assert(seen.calls==1);
        // Nine and more lists, each with its own record: nothing is pushed out (the 8-slot table did evict).
        PresentCopyListMark lists[12];
        PresentCopySlot surfaces[12];
        for(unsigned i=0;i<12;++i)assert(lists[i].mark({&surfaces[i],i+1},cancel,&seen));
        for(unsigned i=0;i<12;++i){
            assert(lists[i].state()==PresentCopyListMark::Pending && lists[i].ticket().slot==&surfaces[i]);
            assert(lists[i].take(&taken) && taken.value==i+1);
        }
        assert(seen.calls==1);
    }
    std::puts("PASSED: a list carries one ticket in its own record, taken once by its execute; a Reset or destroy of "
              "an unexecuted ticket is the cancellation; twelve outstanding tickets push nothing out");
}

void present_context_order() {
    using namespace native12;
    // BD-133: the present context (present-context.h), against fake kernel callbacks that record what they are asked.
    {
        FakeKernel k;
        PresentContext pc;
        UINT64 cpu=7;
        assert(pc.begin(&k,k.cb,nullptr)==E_INVALIDARG && !k.count);
        // Before the first copied present nothing exists: nothing to drain or to close.
        assert(pc.drain(0)==S_OK && pc.close(0)==S_OK && !k.count && !pc.context());
        // The first copied present creates the context (node 0, engine affinity 1, no private data: the fake asserts
        // it) and its present fence, and reads that fence: 0. Nothing is signaled yet.
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && cpu==0 && pc.context()==kPresentContext);
        assert(k.count==2 && k.ops(0,"CF") && k.contexts_live==1 && k.fences_live==1 && !pc.signaled());
        // A surface gets its completion fence once, before its first ticket; a ticket needs one.
        PresentCopySlot s1;
        PresentCopyTicket t{};
        assert(pc.ticket(s1,&t)==E_UNEXPECTED && !t.slot && k.count==2);
        assert(pc.prepare(s1)==S_OK && s1.fence==kCopyFence && s1.fence_cpu && k.count==3 && k.ops(2,"F"));
        assert(pc.prepare(s1)==S_OK && k.count==3);
        // The ticket: k = 1, its reader the present fence value 1 that the next signal carries, and the present
        // context's GPU wait for the copy fence to reach 1, queued now.
        assert(pc.ticket(s1,&t)==S_OK && t.slot==&s1 && t.value==1 && s1.issued==1 && s1.reader==1);
        assert(k.count==4 && k.is(3,'W',kPresentContext,kCopyFence,1));
        // The execute of its list: the copy fence is signaled on the executing queue's own context.
        assert(pc.copy_signal(kQueueContext,t)==S_OK && k.count==5 && k.is(4,'S',kQueueContext,kCopyFence,1));
        assert(s1.signaled==1);
        // The next copied present signals 1 behind the first kernel present, on the present context.
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && cpu==0 && k.count==6 && k.is(5,'S',kPresentContext,kPresentFence,1));
        assert(pc.signaled()==1);
        // Once both are witnessed, the surface takes ticket 2, read behind present fence value 2.
        k.value(kCopyFence)=1;k.value(kPresentFence)=1;
        assert(present_copy_reuse(s1,1)==PresentCopyReuse::Reusable);
        assert(pc.ticket(s1,&t)==S_OK && t.value==2 && s1.reader==2 && k.is(6,'W',kPresentContext,kCopyFence,2));
        assert(pc.copy_signal(kOtherQueueContext,t)==S_OK && k.is(7,'S',kOtherQueueContext,kCopyFence,2));
        // A copy signal of a value the surface never issued, or of 0 and UINT64_MAX (R5), is refused before any call.
        const unsigned before=k.count;
        assert(pc.copy_signal(kQueueContext,{&s1,3})==E_UNEXPECTED);
        assert(pc.copy_signal(kQueueContext,{&s1,0})==E_FAIL && pc.copy_signal(kQueueContext,{&s1,UINT64_MAX})==E_FAIL);
        assert(pc.copy_signal(nullptr,t)==E_INVALIDARG && k.count==before);
        // A failed copy signal is returned and changes nothing.
        k.signal_hr=E_FAIL;
        assert(pc.copy_signal(kQueueContext,t)==E_FAIL && s1.signaled==2 && k.count==before+1);
        k.signal_hr=S_OK;
        const PresentContext::Counts n=pc.counts();
        assert(n.presents==2 && n.signals==1 && n.copy_fences==1 && n.copy_signals==2 && !n.cancelled && !n.retained);
        // The destroy of the source: its reader (2) is the open present, so P is signaled 2 and the CPU waits for it;
        // the copy fence read 2; both proven, the copy fence is destroyed and the surface is released.
        k.value(kCopyFence)=2;
        PresentCopySlot* pool[]={&s1};
        assert(pc.retire(pool,1,1000)==S_OK && !s1.fence && !s1.issued && !s1.reader && !s1.retained.load());
        assert(k.ops(before+1,"SPf") && k.is(before+1,'S',kPresentContext,kPresentFence,2));
        assert(k.is(before+2,'P',nullptr,kPresentFence,2) && k.fences_live==1);
        // DestroyDevice: nothing open and the fence passed, so no wait; then the fence and the context.
        const unsigned closing=k.count;
        assert(pc.close(1000)==S_OK && k.ops(closing,"fc") && !k.contexts_live && !k.fences_live && !pc.context());
    }
    {   // A creation that fails leaves nothing behind, reports, and the next copied present tries again.
        FakeKernel k;
        PresentContext pc;
        UINT64 cpu=0;
        k.create_hr=E_OUTOFMEMORY;
        assert(pc.begin(&k,k.cb,&cpu)==E_OUTOFMEMORY && !pc.context());
        assert(k.count==1 && k.ops(0,"C") && !k.contexts_live);
        k.create_hr=S_OK;k.fence_hr=E_FAIL;k.count=0;
        assert(pc.begin(&k,k.cb,&cpu)==E_FAIL && !pc.context());
        assert(k.count==3 && k.ops(0,"CFc") && !k.contexts_live && !k.fences_live);
        k.fence_hr=S_OK;k.count=0;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && k.ops(0,"CF"));
        // A device whose callbacks lack one, or no device at all, is refused before any call.
        PresentContext bare;D3DDDI_DEVICECALLBACKS missing{};const unsigned before=k.count;
        assert(bare.begin(&k,missing,&cpu)==E_NOTIMPL && k.count==before);
        assert(bare.begin(nullptr,k.cb,&cpu)==E_INVALIDARG && k.count==before);
        // A copy fence that cannot be created fails the present before its copy; the surface stays fresh.
        PresentCopySlot s;
        k.fence_hr=E_FAIL;
        assert(pc.prepare(s)==E_FAIL && !s.fence && !s.fence_cpu);
        k.fence_hr=S_OK;
        // A signal of the present fence that fails fails the present and signals nothing; the next one signals the
        // same value.
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.signaled()==1);
        k.signal_hr=E_FAIL;
        assert(pc.begin(&k,k.cb,&cpu)==E_FAIL && pc.signaled()==1);
        k.signal_hr=S_OK;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.signaled()==2);
        // A GPU wait for the copy that fails is returned and takes no ticket.
        assert(pc.prepare(s)==S_OK);
        k.wait_hr=E_FAIL;PresentCopyTicket t{};
        assert(pc.ticket(s,&t)==E_FAIL && !t.slot && !s.issued && !s.reader && !pc.counts().presents);
        k.wait_hr=S_OK;
        PresentCopySlot* pool[]={&s};
        assert(pc.retire(pool,1,1000)==S_OK && !s.fence);
        assert(pc.close(1000)==S_OK && !k.contexts_live && !k.fences_live);
    }
    std::puts("PASSED: the copied present's own context: created once without private data, a completion fence per "
              "surface, the present context's wait queued with the ticket, the copy signal on the executing queue, "
              "retirement proven before a surface and its fence go");
}

void near_limit() {
    using namespace native12;
    // BD-133, R5: a value at the end of its range is refused before any callback, the present fence's and the copy
    // fence's alike, and the values before it still pass.
    {
        FakeKernel k;
        PresentContext pc(UINT64_MAX-2);
        UINT64 cpu=0;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.signaled()==UINT64_MAX-2);
        PresentCopySlot s;
        assert(pc.prepare(s)==S_OK);
        s.issued=UINT64_MAX-2;
        PresentCopyTicket t{};
        assert(pc.ticket(s,&t)==S_OK && t.value==UINT64_MAX-1 && s.reader==UINT64_MAX-1);
        assert(k.is(k.count-1,'W',kPresentContext,kCopyFence,UINT64_MAX-1));
        assert(pc.copy_signal(kQueueContext,t)==S_OK && k.is(k.count-1,'S',kQueueContext,kCopyFence,UINT64_MAX-1));
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.signaled()==UINT64_MAX-1);
        assert(k.is(k.count-1,'S',kPresentContext,kPresentFence,UINT64_MAX-1));
        const unsigned before=k.count;
        // The copy's next value would be UINT64_MAX: no wait is queued and no ticket taken.
        assert(pc.ticket(s,&t)==E_FAIL && !t.slot && s.issued==UINT64_MAX-1 && k.count==before);
        // The present fence's next value would be UINT64_MAX: the present fails before the signal callback.
        assert(pc.begin(&k,k.cb,&cpu)==E_FAIL && pc.signaled()==UINT64_MAX-1 && k.count==before);
        // A fresh surface still takes ticket 1, and its reader would be UINT64_MAX: refused as well.
        PresentCopySlot fresh;
        assert(pc.prepare(fresh)==S_OK && k.count==before+1);
        assert(pc.ticket(fresh,&t)==E_FAIL && !fresh.issued && k.count==before+1);
    }
    std::puts("PASSED: R5, fence values 0 and UINT64_MAX are refused before the callback, for the present fence and "
              "the copy fences, and UINT64_MAX-1 still passes");
}

void removed_sentinel() {
    using namespace native12;
    // BD-133, R4: UINT64_MAX on the present fence is the removed device, before the comparison and after the wake,
    // and a wake that leaves the fence short proves nothing.
    {
        FakeKernel k;
        PresentContext pc;
        UINT64 cpu=0;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK);
        // Before: the fence already reads UINT64_MAX, which is never "past the target": no CPU wait at all.
        k.value(kPresentFence)=UINT64_MAX;
        unsigned before=k.count;
        assert(pc.drain(1000)==DXGI_ERROR_DEVICE_REMOVED && k.ops(before,"S") && k.count==before+1);
        // begin reads it too, after its signal, and the present is not made.
        assert(pc.begin(&k,k.cb,&cpu)==DXGI_ERROR_DEVICE_REMOVED);
        k.value(kPresentFence)=0;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK);
        // After: the wake comes with UINT64_MAX.
        k.cpu_wait=FakeKernel::Wake;k.wake_value=UINT64_MAX;before=k.count;
        assert(pc.drain(1000)==DXGI_ERROR_DEVICE_REMOVED && k.ops(before,"SP"));
        // After: the wake comes and the fence is still short of the target.
        assert(pc.begin(&k,k.cb,&cpu)==DXGI_ERROR_DEVICE_REMOVED);       // the fence still reads UINT64_MAX
        k.value(kPresentFence)=0;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK);
        k.wake_value=1;before=k.count;
        assert(pc.drain(1000)==DXGI_ERROR_DEVICE_HUNG && k.ops(before,"SP"));
        // The negative control: a wake that reaches the target is S_OK.
        k.cpu_wait=FakeKernel::Complete;
        assert(pc.drain(1000)==S_OK);
        // A copy surface whose fence reads UINT64_MAX at its source's destroy is not proven idle: it is kept.
        PresentCopySlot s;
        PresentCopyTicket t{};
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.prepare(s)==S_OK && pc.ticket(s,&t)==S_OK);
        assert(pc.copy_signal(kQueueContext,t)==S_OK);
        k.value(kCopyFence)=UINT64_MAX;
        PresentCopySlot* pool[]={&s};
        const unsigned fences=k.fences_live;
        assert(pc.retire(pool,1,1000)==DXGI_ERROR_DEVICE_REMOVED && s.retained.load() && s.fence==kCopyFence);
        assert(k.fences_live==fences && pc.counts().retained==1);
    }
    std::puts("PASSED: R4, UINT64_MAX before the comparison or after the wake is the removed device, a short wake is "
              "a hung device, and neither is a witness of a retired surface");
}

void kept_unproven() {
    using namespace native12;
    // BD-133, R3: a close that cannot prove the context retired never destroys it, nor its fence, and stays refused.
    {
        FakeKernel k;
        PresentContext pc;
        UINT64 cpu=0;
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.begin(&k,k.cb,&cpu)==S_OK);
        // The wait never ends: the drain times out, and from here on no destroy callback may be entered.
        k.cpu_wait=FakeKernel::Never;k.forbid_destroy=true;
        assert(pc.close(10)==DXGI_ERROR_DEVICE_HUNG && pc.kept());
        assert(k.contexts_live==1 && k.fences_live==1);
        // Terminal: every later call is refused without a callback.
        const unsigned before=k.count;
        assert(pc.close(10)==DXGI_ERROR_DEVICE_REMOVED && pc.begin(&k,k.cb,&cpu)==DXGI_ERROR_DEVICE_REMOVED);
        assert(pc.drain(10)==DXGI_ERROR_DEVICE_REMOVED && k.count==before);
    }
    {   // A ticket no queue ever signaled: its wait can never be satisfied, so close keeps the context without waiting.
        FakeKernel k;
        PresentContext pc;
        UINT64 cpu=0;
        PresentCopySlot s;
        PresentCopyTicket t{};
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.prepare(s)==S_OK && pc.ticket(s,&t)==S_OK);
        k.forbid_destroy=true;
        const unsigned before=k.count;
        assert(pc.close(1000)==DXGI_ERROR_DEVICE_REMOVED && pc.kept() && k.count==before);
        // The negative control: once the ticket is signaled, a context like it closes and destroys both.
        FakeKernel k2;
        PresentContext pc2;
        PresentCopySlot s2;
        assert(pc2.begin(&k2,k2.cb,&cpu)==S_OK && pc2.prepare(s2)==S_OK && pc2.ticket(s2,&t)==S_OK);
        assert(pc2.copy_signal(kQueueContext,t)==S_OK);
        k2.value(kCopyFence)=1;
        PresentCopySlot* pool[]={&s2};
        assert(pc2.retire(pool,1,1000)==S_OK && pc2.close(1000)==S_OK && !k2.contexts_live && !k2.fences_live);
    }
    {   // Rule D: a cancelled ticket keeps the context, its surface and their fences, with no CPU signal (the fake has
        // no CPU signal callback at all), and the close after it destroys nothing.
        FakeKernel k;
        PresentContext pc;
        UINT64 cpu=0;
        PresentCopySlot s;
        PresentCopyTicket t{};
        assert(pc.begin(&k,k.cb,&cpu)==S_OK && pc.prepare(s)==S_OK && pc.ticket(s,&t)==S_OK);
        assert(!k.cb.pfnSignalSynchronizationObjectFromCpuCb);
        pc.cancel(t);
        assert(pc.kept() && s.retained.load() && pc.counts().cancelled==1 && pc.counts().retained==1);
        k.forbid_destroy=true;
        const unsigned before=k.count;
        PresentCopySlot* pool[]={&s};
        assert(pc.retire(pool,1,1000)==DXGI_ERROR_DEVICE_REMOVED && s.fence==kCopyFence && k.count==before);
        assert(pc.close(1000)==DXGI_ERROR_DEVICE_REMOVED && k.count==before && k.contexts_live==1);
        assert(present_copy_reuse(s,UINT64_MAX-1)==PresentCopyReuse::Busy);
    }
    std::puts("PASSED: R3, a context, a surface or a fence whose retirement is not proven is kept, never destroyed, "
              "and a cancelled ticket is never completed from the CPU");
}
} // namespace

int main() {
    using namespace native12;
    int surface_storage=0,destination_storage=0;
    D3D12DDI_ARG_PRESENTSURFACE surface{{&surface_storage},0};
    D3D12DDIARG_PRESENT_0001 args{};
    args.phSurfacesToPresent=&surface;args.SurfacesToPresent=1;
    args.VidPnSourceID=D3DDDI_ID_UNINITIALIZED;args.OptimizeForComposition=TRUE;
    D3D12DDI_PRESENT_0051 result;D3D12DDI_PRESENT_CONTEXTS_0051 contexts;D3D12DDI_PRESENT_HWQUEUES_0051 queues;

    assert(check_present(&args,&result,&contexts)==S_OK);
    assert(check_present(nullptr,&result,&contexts)==E_INVALIDARG);
    assert(check_present(&args,nullptr,&contexts)==E_INVALIDARG);
    assert(check_present(&args,&result,nullptr)==E_INVALIDARG);
    auto bad=args;bad.SurfacesToPresent=2;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.SurfacesToPresent=0;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.phSurfacesToPresent=nullptr;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    D3D12DDI_ARG_PRESENTSURFACE second{{&surface_storage},1};
    bad=args;bad.phSurfacesToPresent=&second;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    D3D12DDI_ARG_PRESENTSURFACE none{{nullptr},0};
    bad=args;bad.phSurfacesToPresent=&none;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.hDstResource.pDrvPrivate=&destination_storage;
    assert(check_present(&bad,&result,&contexts)==S_OK);
    bad.DstSubResourceIndex=1;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.DirtyRects=1;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.PrivateDriverDataSize=4;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);

    const HANDLE context=reinterpret_cast<HANDLE>(UINT_PTR{0x10000011});
    std::memset(&result,0xa5,sizeof(result));std::memset(&contexts,0xa5,sizeof(contexts));
    std::memset(&queues,0xa5,sizeof(queues));
    assert(fill_present({0x40000100,0,0,0,context},&result,&contexts,&queues)==S_OK);
    assert(result.BroadcastSrcAllocation[0]==0x40000100 && !result.BroadcastDstAllocation[0]);
    assert(!result.AddedGpuWork && result.BackBufferMultiplicity==1 && !result.SyncIntervalOverrideValid);
    for(unsigned i=1;i<=D3DDDI_MAX_BROADCAST_CONTEXT;++i)
        assert(!result.BroadcastSrcAllocation[i] && !result.BroadcastDstAllocation[i]);
    assert(contexts.hContext==context && !contexts.BroadcastContextCount && !contexts.BroadcastContext[0]);
    assert(!queues.BroadcastQueueCount && !queues.hHwQueues[0]);

    assert(fill_present({0x40000100,0,0x40000200,0,context},&result,&contexts,nullptr)==S_OK);
    assert(result.BroadcastDstAllocation[0]==0x40000200);

    // The per-application VSync setting: the override reaches the runtime as given, and only when asked for.
    const DXGI_DDI_FLIP_INTERVAL_TYPE intervals[]={DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,DXGI_DDI_FLIP_INTERVAL_ONE};
    for(const auto interval:intervals){
        std::memset(&result,0xa5,sizeof(result));
        assert(fill_present({0x40000100,0,0,0,context,true,interval},&result,&contexts,nullptr)==S_OK);
        assert(result.SyncIntervalOverrideValid==TRUE && result.SyncIntervalOverride==interval);
    }
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0x40000100,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_ONE},&result,&contexts,nullptr)==S_OK);
    assert(result.SyncIntervalOverrideValid==FALSE && result.SyncIntervalOverride==DXGI_DDI_FLIP_INTERVAL_IMMEDIATE);

    // A refusal leaves every output zero.
    std::memset(&result,0xa5,sizeof(result));std::memset(&contexts,0xa5,sizeof(contexts));
    std::memset(&queues,0xa5,sizeof(queues));
    assert(fill_present({0,0,0,0,context},&result,&contexts,&queues)==E_INVALIDARG);
    assert(!result.BroadcastSrcAllocation[0] && !result.BackBufferMultiplicity && !contexts.hContext);
    assert(!queues.BroadcastQueueCount);
    assert(fill_present({0x40000100,4096,0,0,context},&result,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0x40000200,4096,context},&result,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0,0,nullptr},&result,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0,0,context},nullptr,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0,0,context},&result,nullptr,&queues)==E_INVALIDARG);
    std::puts("PASSED: present outputs for one surface on a software-scheduled queue, refusals leave zeroes");

    // BD-133: AddedGpuWork is the one output the copy changes, and only when a copy was recorded.
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0x40000100,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,true},
                        &result,&contexts,&queues)==S_OK);
    assert(result.AddedGpuWork==TRUE && result.BroadcastSrcAllocation[0]==0x40000100);
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0x40000100,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,false},
                        &result,&contexts,&queues)==S_OK);
    assert(result.AddedGpuWork==FALSE);
    // A refused present reports no added work whatever the request said.
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,true},
                        &result,&contexts,&queues)==E_INVALIDARG);
    assert(result.AddedGpuWork==FALSE);
    std::puts("PASSED: AddedGpuWork is set only for a present that recorded a copy");

    // BD-133: the admission rule of a Present source that is not a linear primary (present-copy.h). The
    // description is a swap-chain back buffer's: one 2D texture, one mip, one slice, one sample.
    auto back_buffer=[](DXGI_FORMAT format,UINT64 width=1280,UINT height=720){
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc={1,0};
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        return d;
    };
    // primary: whether the destination has the primary form to fall back to (present_copy_primary_form).
    struct Admitted {DXGI_FORMAT format;unsigned bytes;unsigned d3dddi;bool blt;bool primary;};
    const Admitted admitted[]={
        {DXGI_FORMAT_B8G8R8A8_UNORM,4,D3DDDIFMT_A8R8G8B8,true,true},
        {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,4,D3DDDIFMT_A8R8G8B8,true,false},   // the sRGB view of the same storage
        {DXGI_FORMAT_R8G8B8A8_UNORM,4,D3DDDIFMT_A8B8G8R8,true,true},
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,4,D3DDDIFMT_A8B8G8R8,true,false},
        // 10-bit and FP16: admitted and copied, and the kernel driver's GPU present cannot blit either
        // (WddmLinearColorFormat). The architecture is not 8-bit only; that one component is.
        {DXGI_FORMAT_R10G10B10A2_UNORM,4,D3DDDIFMT_A2B10G10R10,false,true},
        {DXGI_FORMAT_R16G16B16A16_FLOAT,8,D3DDDIFMT_A16B16G16R16F,false,true},
        {DXGI_FORMAT_A8_UNORM,1,D3DDDIFMT_A8,false,true},
        // The D3D9 back buffer: D3D9On12 gives D3DFMT_X8R8G8B8 as B8G8R8X8_UNORM, which lab arms B and C
        // (2026-10-12 16:54Z) saw refused with "format has no linear surface", format 88. Its destination
        // carries LB7A A8R8G8B8, the A tag of its bytes (lab arm B of 18:35Z: the kernel present refused the
        // X8R8G8B8 tag against the A8R8G8B8 redirection surface), which the kernel driver's GPU present blits, and
        // both formats have the primary form, because neither form's record names a DXGI format (present-copy.h,
        // present_copy_primary_form).
        {DXGI_FORMAT_B8G8R8X8_UNORM,4,D3DDDIFMT_A8R8G8B8,true,true},
        {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,4,D3DDDIFMT_A8R8G8B8,true,true},
    };
    for(const auto& a:admitted){
        PresentCopyShape shape{};
        const D3D12_RESOURCE_DESC1 d=back_buffer(a.format);
        assert(present_copy_admit(d,&shape)==PresentCopy::Admitted);
        assert(shape.width==1280 && shape.height==720 && shape.format==a.format);
        assert(shape.bytes_per_pixel==a.bytes && shape.d3dddi==a.d3dddi);
        assert(shape.kernel_blt_format==a.blt);
        assert(present_copy_kernel_blt_format(a.d3dddi)==a.blt);
        // The row the destination's memory request and the engine's linear image are sized from is the row the
        // rule admitted, and the fallback form exists exactly where the table above says.
        const AMDGPU_WDDM_SURFACE_FORMAT* row=present_copy_format(static_cast<uint32_t>(a.format));
        assert(row && row->d3dddi==a.d3dddi && row->bytes_per_pixel==a.bytes);
        assert(present_copy_primary_form(static_cast<uint32_t>(a.format))==a.primary);
        // The destination's description is the source's geometry and format, on a GPU-only L1 heap that is
        // neither a primary nor system-wide coherent, with no size of its own.
        PresentCopyRequest request{};
        present_copy_description(shape,&request);
        assert(!request.heap.ByteSize && !request.heap.Alignment);
        assert(request.heap.CPUPageProperty==D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
        assert(request.heap.MemoryPool==D3D12DDI_MEMORY_POOL_L1);
        assert(request.heap.Flags==D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES);
        assert(request.heap.CreationNodeMask==1 && request.heap.VisibleNodeMask==1);
        assert(request.resource.ResourceType==D3D12DDI_RT_TEXTURE2D);
        assert(request.resource.Width==shape.width && request.resource.Height==shape.height);
        assert(request.resource.DepthOrArraySize==1 && request.resource.MipLevels==1);
        assert(request.resource.Format==a.format && request.resource.SampleDesc.Count==1);
        assert(request.resource.Layout==D3D12DDI_TL_UNDEFINED);
        assert(request.resource.Flags==D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET);
        assert(request.resource.InitialBarrierLayout==D3D12DDI_BARRIER_LAYOUT_COMMON);
        // The primary form differs in the heap's PRIMARY flag and in nothing else.
        PresentCopyRequest as_primary=request;
        present_copy_as_primary(&as_primary);
        assert(as_primary.heap.Flags==(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY));
        as_primary.heap.Flags=request.heap.Flags;
        assert(!std::memcmp(&as_primary,&request,sizeof(request)));
    }
    // The negative controls, one per reason, each with the back buffer's description changed in one way.
    {
        PresentCopyShape shape{};
        D3D12_RESOURCE_DESC1 d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);
        d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        assert(present_copy_admit(d,&shape)==PresentCopy::NotTexture2D);
        assert(!shape.width && !shape.bytes_per_pixel && !shape.kernel_blt_format);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        assert(present_copy_admit(d,&shape)==PresentCopy::NotTexture2D);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.MipLevels=2;
        assert(present_copy_admit(d,&shape)==PresentCopy::Subresources);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.DepthOrArraySize=2;
        assert(present_copy_admit(d,&shape)==PresentCopy::Subresources);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.SampleDesc={4,0};
        assert(present_copy_admit(d,&shape)==PresentCopy::Multisampled);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.SampleDesc={1,1};
        assert(present_copy_admit(d,&shape)==PresentCopy::Multisampled);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,0,720),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,1280,0),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,8193,720),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,1280,8193),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,8192,8192),&shape)==PresentCopy::Admitted);
        // A format the wire does not compose has no linear surface to copy into.
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_R32G32B32A32_FLOAT),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_R8G8B8A8_TYPELESS),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_UNKNOWN),&shape)==PresentCopy::Format);
        // The X8 bytes are admitted for the two DXGI formats that store them and for no other: not the typeless
        // family of B8G8R8X8, and not the other D3D9 back-buffer formats (D3DFMT_R5G6B5, X1R5G5B5, A1R5G5B5),
        // which have no row at all.
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8X8_TYPELESS),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B5G6R5_UNORM),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B5G5R5A1_UNORM),&shape)==PresentCopy::Format);
        assert(!shape.width && !shape.bytes_per_pixel && !shape.d3dddi);
        assert(!present_copy_format(DXGI_FORMAT_B8G8R8X8_TYPELESS) && !present_copy_format(DXGI_FORMAT_B5G6R5_UNORM));
        assert(!present_copy_primary_form(DXGI_FORMAT_B8G8R8X8_TYPELESS));
        assert(!present_copy_primary_form(DXGI_FORMAT_R32G32B32A32_FLOAT));
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        assert(present_copy_admit(d,&shape)==PresentCopy::DepthStencil);
        // Its own reason, and not the depth-stencil one: a refusal line that named the wrong flag would send the
        // next reader after the wrong resource (review finding 7).
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags|=D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        assert(present_copy_admit(d,&shape)==PresentCopy::DenyShaderResource);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);
        d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL|D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        assert(present_copy_admit(d,&shape)==PresentCopy::DepthStencil);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        assert(present_copy_admit(d,&shape)==PresentCopy::CrossAdapter);
        // A source that also carries the other view flags is still a source: they change nothing.
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);
        d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        assert(present_copy_admit(d,&shape)==PresentCopy::Admitted);
        // Every reason has a name of its own, and the rule writes nothing without an answer.
        const char* names[static_cast<unsigned>(PresentCopy::Count)]={};
        for(unsigned i=0;i<static_cast<unsigned>(PresentCopy::Count);++i){
            names[i]=present_copy_text(static_cast<PresentCopy>(i));
            assert(names[i] && names[i][0]);
            for(unsigned j=0;j<i;++j)assert(std::strcmp(names[i],names[j]));
        }
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM),nullptr)==PresentCopy::Admitted);
        present_copy_description(shape,nullptr);present_copy_as_primary(nullptr);
    }
    std::puts("PASSED: the Present copy admits every composed back-buffer format and refuses every other source");

    // BD-133, the D3D9 back buffer: the X8 bytes reach the copy and nothing else, under the A tag of those bytes.
    // The table is not changed, so every lookup by DXGI number that another component makes - the wire, the
    // scan-out rule, both shells' composed surfaces - still finds no row for 88 or 93; the copy's own lookup is the
    // one that finds one.
    {
        const AMDGPU_WDDM_SURFACE_FORMAT* x8=present_copy_x8_row();
        assert(x8 && x8->d3dddi==AMDGPU_WDDM_D3DDDI_X8R8G8B8 && x8->bytes_per_pixel==4 && !x8->dxgi &&
               !x8->dxgi_srgb && !(x8->policy&AMDGPU_WDDM_SURFACE_COMPOSED) &&
               (x8->policy&AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY));
        // Lab arm B of 2026-10-12 18:35Z: tagged X8R8G8B8 (22), every copied present was refused by the kernel's
        // format check against the A8R8G8B8 (21) redirection surface ("snapshot refused ... format", 240 of 240).
        // The tag is the A format of the same channel order, and every format without an X channel keeps its own.
        assert(present_copy_alpha_tag(AMDGPU_WDDM_D3DDDI_X8R8G8B8)==AMDGPU_WDDM_D3DDDI_A8R8G8B8);
        assert(present_copy_alpha_tag(static_cast<uint32_t>(D3DDDIFMT_X8B8G8R8))==AMDGPU_WDDM_D3DDDI_A8B8G8R8);
        for(const uint32_t same:{AMDGPU_WDDM_D3DDDI_A8R8G8B8,AMDGPU_WDDM_D3DDDI_A8B8G8R8,AMDGPU_WDDM_D3DDDI_A2B10G10R10,
                                 AMDGPU_WDDM_D3DDDI_A16B16G16R16F,AMDGPU_WDDM_D3DDDI_A8,0u})
            assert(present_copy_alpha_tag(same)==same);
        // The row of the copy of an X8 source is the composed BGRA8 row: LB7A A8R8G8B8, the X8 row's bytes a pixel,
        // a format the kernel blit admits - the same tag as the copy of an A8 (B8G8R8A8) source, so both meet an
        // A8R8G8B8 destination with one format.
        const AMDGPU_WDDM_SURFACE_FORMAT* bgra8=
            Bc250SharedSurfaceFormat(static_cast<unsigned long>(DXGI_FORMAT_B8G8R8A8_UNORM));
        assert(bgra8 && bgra8->d3dddi==AMDGPU_WDDM_D3DDDI_A8R8G8B8 && bgra8->bytes_per_pixel==x8->bytes_per_pixel &&
               (bgra8->policy&AMDGPU_WDDM_SURFACE_COMPOSED));
        for(const DXGI_FORMAT f:{DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB}){
            assert(!Bc250SharedSurfaceFormat(static_cast<unsigned long>(f)));
            assert(!amdgpu_wddm_surface_format_by_dxgi(static_cast<unsigned>(f)));
            assert(present_copy_own_format(static_cast<uint32_t>(f))==bgra8);
            assert(present_copy_format(static_cast<uint32_t>(f))==bgra8);
            PresentCopyShape tagged{};
            assert(present_copy_admit(back_buffer(f,256,256),&tagged)==PresentCopy::Admitted);
            assert(tagged.d3dddi==AMDGPU_WDDM_D3DDDI_A8R8G8B8 && tagged.kernel_blt_format && tagged.bytes_per_pixel==4);
        }
        {
            PresentCopyShape a8{};
            assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,256,256),&a8)==PresentCopy::Admitted);
            assert(a8.d3dddi==AMDGPU_WDDM_D3DDDI_A8R8G8B8 && a8.kernel_blt_format);
        }
        // A composed format is never the driver's own: its row stays the wire's, so its records do not change.
        for(const DXGI_FORMAT f:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_A8_UNORM}){
            assert(!present_copy_own_format(static_cast<uint32_t>(f)));
            assert(present_copy_format(static_cast<uint32_t>(f))==Bc250SharedSurfaceFormat(static_cast<unsigned long>(f)));
        }
        assert(!present_copy_own_format(DXGI_FORMAT_UNKNOWN) && !present_copy_own_format(DXGI_FORMAT_B8G8R8X8_TYPELESS));
        // The copy stays a byte copy: the destination's description has the source's own format, 88 for 88.
        PresentCopyShape shape{};
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8X8_UNORM,256,256),&shape)==PresentCopy::Admitted);
        PresentCopyRequest request{};
        present_copy_description(shape,&request);
        assert(request.resource.Format==DXGI_FORMAT_B8G8R8X8_UNORM && request.resource.Width==256 &&
               request.resource.Height==256);
    }
    std::puts("PASSED: the D3D9 X8R8G8B8 back buffer (B8G8R8X8 and its sRGB view) is copied byte for byte and tagged "
              "A8R8G8B8, as the copy of a B8G8R8A8 source is");

    // BD-133, review finding 8: the stage 3 -> stage 6 decision of pfnPresent, with both answers substituted, and R6:
    // the copy is asked for the windowed Blt form only.
    {
        struct Calls {
            HRESULT direct_hr,copy_hr;
            D3DKMT_HANDLE direct_allocation,copy_allocation;
            unsigned direct_calls,copy_calls;
        };
        Calls calls{};
        auto direct=[](void* state,D3DKMT_HANDLE* allocation) noexcept {
            auto& c=*static_cast<Calls*>(state);++c.direct_calls;
            if(c.direct_hr==S_OK)*allocation=c.direct_allocation;
            return c.direct_hr;
        };
        auto copy=[](void* state,D3DKMT_HANDLE* allocation) noexcept {
            auto& c=*static_cast<Calls*>(state);++c.copy_calls;
            if(c.copy_hr==S_OK)*allocation=c.copy_allocation;
            return c.copy_hr;
        };
        const PresentSourceHooks hooks{&calls,direct,copy};
        PresentSources from{};
        unsigned stage=0;
        // The runtime's DXGI_DDI_PRESENT_FLAGS (dxgiddi.h): Blt 0x1, Flip 0x2.
        DXGI_DDI_PRESENT_FLAGS blt{},flip{},both{},none_flags{},tearing{};
        blt.Blt=1;flip.Flip=1;both.Blt=1;both.Flip=1;tearing.Blt=1;tearing.AllowTearing=1;
        assert(blt.Value==0x1 && flip.Value==0x2 && both.Value==0x3 && tearing.Value==0x11);
        assert(present_copy_flags_admitted(blt) && present_copy_flags_admitted(tearing));
        assert(!present_copy_flags_admitted(flip) && !present_copy_flags_admitted(both));
        assert(!present_copy_flags_admitted(none_flags));
        // A linear primary: stage 3 answers, stage 6 is never asked, no added work - whatever the flags, as before.
        for(const DXGI_DDI_PRESENT_FLAGS& f:{blt,flip,none_flags}){
            calls={S_OK,E_FAIL,0x40000100,0x40000200,0,0};
            from=PresentSources{};
            assert(present_source(hooks,from,stage,f)==S_OK);
            assert(stage==3 && from.source==0x40000100 && !from.added_gpu_work);
            assert(calls.direct_calls==1 && calls.copy_calls==0);
        }
        // A source stage 3 refuses, in a windowed Blt: stage 6 answers, its allocation goes to the runtime, added work
        // is reported.
        calls={E_INVALIDARG,S_OK,0,0x40000200,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage,blt)==S_OK);
        assert(stage==6 && from.source==0x40000200 && from.added_gpu_work);
        assert(calls.direct_calls==1 && calls.copy_calls==1);
        // R6, the host controls: the same source in a Flip, in a Blt that also says Flip, or with neither, is refused
        // at stage 8 and the copy is never asked, so nothing is recorded and no wait is queued.
        for(const DXGI_DDI_PRESENT_FLAGS& f:{flip,both,none_flags}){
            calls={E_INVALIDARG,S_OK,0,0x40000200,0,0};
            from=PresentSources{};
            assert(present_source(hooks,from,stage,f)==E_INVALIDARG);
            assert(stage==8 && !from.source && !from.added_gpu_work);
            assert(calls.direct_calls==1 && calls.copy_calls==0);
            assert(fill_present(from,&result,&contexts,&queues)==E_INVALIDARG);
        }
        // Both refuse: the status is stage 6's, the source stays 0 and no added work is claimed. fill_present
        // then refuses as well, which is what leaves every output of the DDI zero.
        calls={E_INVALIDARG,E_OUTOFMEMORY,0,0,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage,blt)==E_OUTOFMEMORY);
        assert(stage==6 && !from.source && !from.added_gpu_work);
        assert(fill_present(from,&result,&contexts,&queues)==E_INVALIDARG);
        // A copy that answers S_OK and names no allocation is the one case neither side could be trusted on.
        calls={E_INVALIDARG,S_OK,0,0,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage,blt)==E_UNEXPECTED);
        assert(stage==6 && !from.source && !from.added_gpu_work);
        // A present is never attempted without both answers.
        from=PresentSources{};
        assert(present_source({&calls,nullptr,copy},from,stage,blt)==E_INVALIDARG);
        assert(present_source({&calls,direct,nullptr},from,stage,blt)==E_INVALIDARG);
    }
    std::puts("PASSED: Present asks the copy only when the zero-copy source is refused, and only for a windowed Blt "
              "(a Flip is refused before the copy is recorded)");

    reuse_rule();
    list_ticket_record();
    present_context_order();
    near_limit();
    removed_sentinel();
    kept_unproven();
    return 0;
}
