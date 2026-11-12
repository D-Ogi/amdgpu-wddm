// SPDX-License-Identifier: MIT
// Host test of the UMD logging module (umd-etw.h) with a recording ETW sink: the payload and descriptor of each event,
// the order of map, unmap and rundown, one registration per log, unload with live mappings, and a destroy without a
// create. The WDK header umdprovider.h is compiled here only, to check that the module's types agree with it.
#include "umd-etw.h"
#include <umdprovider.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>
using namespace bc250::umd::etw;
#define CHECK(x) do{if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);std::abort();}}while(0)

// Heap allocations of this process, for the check that a rotation at a Present allocates nothing
static std::atomic<unsigned long long> heap_allocations{0};
void *operator new(size_t size) {
    heap_allocations.fetch_add(1,std::memory_order_relaxed);
    if (void *p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p,size_t) noexcept { std::free(p); }

static_assert(sizeof(UMDETW_ALLOCATION_USAGE)==sizeof(UINT32));
static_assert(sizeof(UMDETW_ALLOCATION_SEMANTIC)==sizeof(AllocationSemantic));
static_assert(UINT32(AllocationSemantic::none)==UMDETW_ALLOCATION_SEMANTIC_NONE);
static_assert(UINT32(AllocationSemantic::dma_buffer)==UMDETW_ALLOCATION_SEMANTIC_DMA_BUFFER);
static_assert(UINT32(AllocationSemantic::upload_staging)==UMDETW_ALLOCATION_SEMANTIC_UPLOAD_STAGING);
static_assert(UINT32(AllocationSemantic::download_staging)==UMDETW_ALLOCATION_SEMANTIC_DOWNLOAD_STAGING);
static_assert(UINT32(AllocationSemantic::context_save)==UMDETW_ALLOCATION_SEMANTIC_CONTEXT_SAVE);
static_assert(UINT32(AllocationSemantic::driver_other_min)==UMDETW_ALLOCATION_SEMANTIC_DRIVER_OTHER_MIN);
static_assert(UINT32(AllocationSemantic::driver_other_max)==UMDETW_ALLOCATION_SEMANTIC_DRIVER_OTHER_MAX);

namespace {
struct Event { USHORT id; UCHAR opcode,level; ULONGLONG keyword; AllocationMapping m; };
struct Sink {
    std::vector<Event> events;
    unsigned registers=0,unregisters=0,writes_after_unregister=0;
    REGHANDLE live=0;
    PENABLECALLBACK callback=nullptr; PVOID context=nullptr;
    bool fail_register=false;
    // A session that is on before the registration: EventRegister calls the callback before it returns.
    bool enable_inside_register=false;
};
Sink *sink=nullptr;
ULONG WINAPI fake_register(LPCGUID guid,PENABLECALLBACK callback,PVOID context,PREGHANDLE handle) {
    CHECK(guid && IsEqualGUID(*guid,UMDEtwProviderId)); // umdprovider.h 74-77
    ++sink->registers;
    if (sink->fail_register) return ERROR_ACCESS_DENIED;
    sink->callback=callback; sink->context=context;
    if (sink->enable_inside_register) {
        callback(guid,EVENT_CONTROL_CODE_ENABLE_PROVIDER,0,~0ull,0,nullptr,context);
        callback(guid,EVENT_CONTROL_CODE_CAPTURE_STATE,0,~0ull,0,nullptr,context);
    }
    *handle=sink->live=0x5150A;
    return ERROR_SUCCESS;
}
ULONG WINAPI fake_unregister(REGHANDLE handle) {
    CHECK(handle==sink->live && handle);
    ++sink->unregisters; sink->live=0;
    return ERROR_SUCCESS;
}
ULONG WINAPI fake_write(REGHANDLE handle,PCEVENT_DESCRIPTOR d,ULONG count,PEVENT_DATA_DESCRIPTOR data) {
    if (!sink->live || handle!=sink->live) { ++sink->writes_after_unregister; return ERROR_INVALID_HANDLE; }
    // The payload of umdprovider.h 151-159: six fields of 8, 8, 8, 8, 4 and 4 bytes.
    CHECK(count==6);
    const ULONG sizes[6]={8,8,8,8,4,4};
    for (ULONG i=0;i<6;++i) CHECK(data[i].Size==sizes[i]);
    Event e{d->Id,d->Opcode,d->Level,d->Keyword,{}};
    std::memcpy(&e.m.d3d_allocation,reinterpret_cast<const void *>(data[0].Ptr),8);
    std::memcpy(&e.m.dxg_allocation,reinterpret_cast<const void *>(data[1].Ptr),8);
    std::memcpy(&e.m.offset,reinterpret_cast<const void *>(data[2].Ptr),8);
    std::memcpy(&e.m.size,reinterpret_cast<const void *>(data[3].Ptr),8);
    std::memcpy(&e.m.usage,reinterpret_cast<const void *>(data[4].Ptr),4);
    std::memcpy(&e.m.semantic,reinterpret_cast<const void *>(data[5].Ptr),4);
    sink->events.push_back(e);
    return ERROR_SUCCESS;
}
const ProviderApi fake{&fake_register,&fake_unregister,&fake_write};
void control(ULONG code) { sink->callback(&UMDEtwProviderId,code,0,~0ull,0,nullptr,sink->context); }
void check_event(const Event &e,const EVENT_DESCRIPTOR &d,const AllocationMapping &m) {
    CHECK(e.id==d.Id && e.opcode==d.Opcode && e.level==d.Level && e.keyword==d.Keyword);
    CHECK(e.m==m);
}
AllocationMapping surface(UINT64 d3d,UINT64 dxg,UINT64 size) {
    return {d3d,dxg,0,size,usage::none,AllocationSemantic::none};
}
}
int main() {
    // The descriptors of um\umdetw.h 38-43 as this module writes them.
    CHECK(MapAllocationEvent.Id==1 && MapAllocationEvent.Opcode==1 && UnmapAllocationEvent.Id==3 &&
          UnmapAllocationEvent.Opcode==2 && RundownAllocationEvent.Id==2 && RundownAllocationEvent.Opcode==3);
    UMDETW_ALLOCATION_USAGE packed_bit{},renamed_bit{};
    packed_bit.Packed=1; renamed_bit.Renamed=1;
    CHECK(usage::packed==packed_bit.Value && usage::renamed==renamed_bit.Value);
    const auto a=surface(0x1000,0x40000041,0x100000),b=surface(0x2000,0x40000042,0x200000),
               c=surface(0x3000,0x40000043,0x300000);
    {   // Nothing is written before a session enables the provider, and the table is still kept for a rundown.
        Sink s; sink=&s;
        AllocationLog log(fake);
        CHECK(log.map(a)==S_OK && s.events.empty() && log.live()==1);
        CHECK(log.register_provider()==S_OK);
        CHECK(log.register_provider()==S_FALSE && s.registers==1); // double registration
        CHECK(log.map(b)==S_OK && s.events.empty() && !log.enabled());
        // A capture-state request reports every live mapping, in the order of the table, also with the provider off.
        control(EVENT_CONTROL_CODE_CAPTURE_STATE);
        CHECK(s.events.size()==2);
        check_event(s.events[0],RundownAllocationEvent,a); check_event(s.events[1],RundownAllocationEvent,b);
        s.events.clear();
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        CHECK(log.enabled());
        CHECK(log.map(c)==S_OK);
        CHECK(log.map(c)==S_FALSE);                       // the same mapping again: no second event
        auto other=c; other.size=1;
        CHECK(log.map(other)==E_INVALIDARG);              // another claim for a live mapping: refused, no event
        CHECK(s.events.size()==1); check_event(s.events[0],MapAllocationEvent,c);
        s.events.clear();
        // Destroy: the unmap repeats the values of the map (nf-umdprovider-umdetwlogunmapallocation.md 86).
        CHECK(log.unmap_resource(b.d3d_allocation)==1);
        CHECK(s.events.size()==1); check_event(s.events[0],UnmapAllocationEvent,b);
        s.events.clear();
        // A destroy without a create, and a second destroy of the same resource: nothing is written.
        CHECK(log.unmap_resource(0x9000)==0 && log.unmap_resource(b.d3d_allocation)==0 && s.events.empty());
        CHECK(log.unmap(surface(0x9000,1,1))==S_FALSE && s.events.empty());
        control(EVENT_CONTROL_CODE_DISABLE_PROVIDER);
        CHECK(!log.enabled());
        CHECK(log.unmap(a)==S_OK && s.events.empty() && log.live()==1);
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        control(EVENT_CONTROL_CODE_CAPTURE_STATE);
        CHECK(s.events.size()==1); check_event(s.events[0],RundownAllocationEvent,c);
        s.events.clear();
        // Unload with a live mapping: one unregistration, no unmap event, nothing written after it.
        log.unregister_provider();
        CHECK(s.unregisters==1 && !log.enabled() && s.events.empty());
        log.unregister_provider();
        CHECK(s.unregisters==1);
        CHECK(log.unmap_resource(c.d3d_allocation)==1 && s.writes_after_unregister==0 && s.events.empty());
    }
    {   // The destructor unregisters a log that has live mappings, and writes no event for them.
        Sink s; sink=&s;
        {
            AllocationLog log(fake);
            CHECK(log.register_provider()==S_OK);
            control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
            CHECK(log.map(a)==S_OK && log.map(b)==S_OK && s.events.size()==2);
        }
        CHECK(s.unregisters==1 && s.events.size()==2 && s.writes_after_unregister==0);
    }
    {   // A session that is on before the registration: the enable is kept, and the capture state inside
        // EventRegister writes nothing (no handle yet, and no mapping at the first adapter open).
        Sink s; sink=&s; s.enable_inside_register=true;
        AllocationLog log(fake);
        CHECK(log.register_provider()==S_OK && log.enabled() && s.events.empty() && s.writes_after_unregister==0);
        CHECK(log.map(a)==S_OK && s.events.size()==1);
        check_event(s.events[0],MapAllocationEvent,a);
    }
    {   // A failed registration keeps the table and is tried again by the next adapter open.
        Sink s; sink=&s; s.fail_register=true;
        AllocationLog log(fake);
        CHECK(log.register_provider()==HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
        CHECK(log.map(a)==S_OK && log.live()==1);
        s.fail_register=false;
        CHECK(log.register_provider()==S_OK && s.registers==2);
        log.unregister_provider();
        CHECK(s.unregisters==1);
    }
    {   // Identity rotation of three swap-chain buffers (replace): every unmap comes before every map, each unmap
        // repeats its map, and no allocation is mapped to two resources at one time.
        Sink s; sink=&s;
        AllocationLog log(fake);
        CHECK(log.register_provider()==S_OK);
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        CHECK(log.map(a)==S_OK && log.map(b)==S_OK && log.map(c)==S_OK);
        s.events.clear();
        const AllocationMapping moved[3]={surface(a.d3d_allocation,b.dxg_allocation,b.size),
            surface(b.d3d_allocation,c.dxg_allocation,c.size),surface(c.d3d_allocation,a.dxg_allocation,a.size)};
        CHECK(log.replace(moved,3)==S_OK && log.live()==3);
        CHECK(s.events.size()==6);
        check_event(s.events[0],UnmapAllocationEvent,a); check_event(s.events[1],UnmapAllocationEvent,b);
        check_event(s.events[2],UnmapAllocationEvent,c);
        for (int i=0;i<3;++i) check_event(s.events[3+i],MapAllocationEvent,moved[i]);
        s.events.clear();
        control(EVENT_CONTROL_CODE_CAPTURE_STATE);
        CHECK(s.events.size()==3);
        for (int i=0;i<3;++i) check_event(s.events[i],RundownAllocationEvent,moved[i]);
        s.events.clear();
        // With the provider off, the table still follows the rotation, and a rotation of a swap chain allocates
        // nothing: the off cost at each Present of a blt-model swap chain is the table lock and the node moves.
        control(EVENT_CONTROL_CODE_DISABLE_PROVIDER);
        const AllocationMapping back[3]={surface(a.d3d_allocation,c.dxg_allocation,c.size),
            surface(b.d3d_allocation,a.dxg_allocation,a.size),surface(c.d3d_allocation,b.dxg_allocation,b.size)};
        const unsigned long long before=heap_allocations.load();
        int *volatile probe=new int(1);                  // the counter counts
        delete probe;
        CHECK(heap_allocations.load()==before+1);
        CHECK(log.replace(back,3)==S_OK);
        CHECK(heap_allocations.load()==before+1 && s.events.empty() && log.live()==3);
        AllocationMapping now{};
        CHECK(log.mappings_of(a.d3d_allocation,&now,1)==1 && now==back[0]);
        CHECK(log.replace(moved,3)==S_OK);
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        s.events.clear();
        // The seam for engine memory: a resource that is packed into a parent allocation, then renamed to a second
        // instance, then moved to two allocations, then destroyed.
        const UINT64 r=0x7000;
        const AllocationMapping packed{r,0x40000050,0x1000,0x100,usage::packed,AllocationSemantic::none};
        const AllocationMapping renamed{r,0x40000051,0,0x100,usage::renamed,AllocationSemantic::none};
        CHECK(log.map(packed)==S_OK);
        CHECK(log.replace(&renamed,1)==S_OK);
        const AllocationMapping split[2]={{r,0x40000052,0,0x80,usage::none,AllocationSemantic::none},
                                          {r,0x40000053,0,0x80,usage::none,AllocationSemantic::none}};
        CHECK(log.replace(split,2)==S_OK);                 // one more mapping than before: a new node
        CHECK(log.unmap_resource(r)==2);
        CHECK(s.events.size()==8);
        check_event(s.events[0],MapAllocationEvent,packed);
        check_event(s.events[1],UnmapAllocationEvent,packed); check_event(s.events[2],MapAllocationEvent,renamed);
        check_event(s.events[3],UnmapAllocationEvent,renamed);
        check_event(s.events[4],MapAllocationEvent,split[0]); check_event(s.events[5],MapAllocationEvent,split[1]);
        check_event(s.events[6],UnmapAllocationEvent,split[0]); check_event(s.events[7],UnmapAllocationEvent,split[1]);
        s.events.clear();
        // Driver-internal memory has no Direct3D resource (hD3DAllocation zero) and names its semantic.
        const AllocationMapping staging{0,0x40000060,0,0x10000,usage::none,AllocationSemantic::upload_staging};
        CHECK(log.map(staging)==S_OK && log.unmap(staging)==S_OK && s.events.size()==2);
        check_event(s.events[0],MapAllocationEvent,staging); check_event(s.events[1],UnmapAllocationEvent,staging);
        CHECK(log.replace(nullptr,0)==S_OK && log.replace(nullptr,1)==E_INVALIDARG);
    }
    {   // Concurrent changes and rundowns: every live mapping is in the trace once, as a rundown or as a map, and
        // each unmap follows its map or rundown.
        Sink s; sink=&s;
        AllocationLog log(fake);
        CHECK(log.register_provider()==S_OK);
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        // The sink is not thread safe on its own; the log calls it only under its table lock.
        std::thread worker([&] {
            for (UINT64 i=1;i<=2000;++i) { CHECK(log.map(surface(i,0x40000000+i,4096))==S_OK); if (i%2) log.unmap_resource(i); }
        });
        for (int i=0;i<50;++i) control(EVENT_CONTROL_CODE_CAPTURE_STATE);
        worker.join();
        CHECK(log.live()==1000);
        std::vector<int> state(2001,0); // 0 never seen, 1 mapped, 2 unmapped
        bool rundown_seen_after_unmap=false;
        for (const auto &e:s.events) {
            const UINT64 i=e.m.d3d_allocation;
            CHECK(i>=1 && i<=2000 && e.m.dxg_allocation==0x40000000+i);
            if (e.id==MapAllocationEvent.Id) { CHECK(state[i]==0); state[i]=1; }
            else if (e.id==UnmapAllocationEvent.Id) { CHECK(state[i]==1); state[i]=2; }
            else if (state[i]==2) rundown_seen_after_unmap=true;
        }
        CHECK(!rundown_seen_after_unmap);
        for (UINT64 i=1;i<=2000;++i) CHECK(state[i]==(i%2 ? 2 : 1));
    }
    {   // Reporters (engine ABI 1.8): the state at add_reporter, every enable change, and a capture-state request
        // after the table's own rundown; external events need no table entry and change no table entry.
        struct Reporter final : MemoryReporter {
            AllocationLog *log=nullptr;
            std::vector<int> calls;                      // 0 reporting off, 1 reporting on, 2 report
            AllocationMapping m{};
            void set_reporting(bool on) noexcept override { calls.push_back(on ? 1 : 0); }
            void report() noexcept override { calls.push_back(2); log->write_external(EventKind::rundown,m); }
        };
        Sink s; sink=&s;
        AllocationLog log(fake);
        CHECK(log.register_provider()==S_OK);
        Reporter r; r.log=&log;
        r.m={0x8000,0x40000070,0x40,0x100,usage::packed|usage::renamed,AllocationSemantic::none};
        CHECK(log.add_reporter(&r)==S_OK && log.add_reporter(&r)==S_FALSE && log.add_reporter(nullptr)==E_INVALIDARG);
        CHECK((r.calls==std::vector<int>{0}));
        log.write_external(EventKind::map,r.m);          // provider off: nothing
        CHECK(s.events.empty() && log.live()==0);
        CHECK(log.map(a)==S_OK);
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        log.write_external(EventKind::map,r.m);
        log.write_external(EventKind::unmap,r.m);
        CHECK(s.events.size()==2 && log.live()==1);
        check_event(s.events[0],MapAllocationEvent,r.m); check_event(s.events[1],UnmapAllocationEvent,r.m);
        s.events.clear();
        control(EVENT_CONTROL_CODE_CAPTURE_STATE);
        CHECK(s.events.size()==2);
        check_event(s.events[0],RundownAllocationEvent,a); check_event(s.events[1],RundownAllocationEvent,r.m);
        control(EVENT_CONTROL_CODE_DISABLE_PROVIDER);
        CHECK((r.calls==std::vector<int>{0,1,2,0}));
        log.remove_reporter(&r);
        control(EVENT_CONTROL_CODE_ENABLE_PROVIDER);
        control(EVENT_CONTROL_CODE_CAPTURE_STATE);
        CHECK(r.calls.size()==4);
        // A reporter added while a session is on starts with reporting on.
        Reporter late; late.log=&log;
        CHECK(log.add_reporter(&late)==S_OK && (late.calls==std::vector<int>{1}));
        log.remove_reporter(&late);
        log.remove_reporter(&late);                      // a second removal does nothing
    }
    {   // The process log is one object.
        CHECK(process_allocation_log() && process_allocation_log()==process_allocation_log());
    }
    std::printf("umd-etw-test: all checks passed\n");
    return 0;
}
