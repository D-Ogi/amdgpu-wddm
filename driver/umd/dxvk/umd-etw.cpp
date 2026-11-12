// SPDX-License-Identifier: MIT
#include "umd-etw.h"
#include <evntrace.h> // EVENT_CONTROL_CODE_*, as umdprovider.h 19-20 includes it
#include <umdetw.h>
#include <algorithm>
#include <iterator>
#include <new>
#include <vector>
#pragma comment(lib,"advapi32.lib") // EventRegister, EventUnregister, EventWrite
namespace bc250::umd::etw {
namespace {
const EVENT_DESCRIPTOR &descriptor(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::map: return MapAllocationEvent;
    case EventKind::rundown: return RundownAllocationEvent;
    default: return UnmapAllocationEvent;
    }
}
}
HRESULT AllocationLog::register_provider() noexcept {
    AcquireSRWLockExclusive(&registration_);
    HRESULT hr=S_FALSE;
    if (!registered_) {
        // EventRegister can call enable_callback before it returns. handle_ is still zero then, so a capture-state
        // request inside the registration writes nothing; the table is empty at that time, because the process
        // registers at its first adapter open, before any device exists.
        REGHANDLE handle=0;
        const ULONG status=api_.register_provider(&UMDEtwProviderGuid,&AllocationLog::enable_callback,this,&handle);
        if (status==ERROR_SUCCESS) {
            AcquireSRWLockExclusive(&lock_); handle_=handle; ReleaseSRWLockExclusive(&lock_);
            registered_=true; hr=S_OK;
        } else hr=HRESULT_FROM_WIN32(status);
    }
    ReleaseSRWLockExclusive(&registration_);
    return hr;
}
void AllocationLog::unregister_provider() noexcept {
    AcquireSRWLockExclusive(&registration_);
    if (registered_) {
        // The handle leaves the table lock first: a writer that holds the lock finishes with a valid handle, and a
        // rundown that starts later sees zero. EventUnregister itself runs without the table lock, because it waits
        // for a callback in progress, and the rundown callback takes that lock.
        AcquireSRWLockExclusive(&lock_);
        const REGHANDLE handle=handle_; handle_=0;
        ReleaseSRWLockExclusive(&lock_);
        enabled_.store(false,std::memory_order_relaxed);
        api_.unregister_provider(handle);
        enabled_.store(false,std::memory_order_relaxed);
        registered_=false;
    }
    ReleaseSRWLockExclusive(&registration_);
}
void AllocationLog::write_locked(EventKind kind,const AllocationMapping &m) const noexcept {
    if (!handle_) return;
    // The payload of LogMapAllocation (umdprovider.h 151-167): four 8-byte fields, then Usage and Semantic, 4 bytes each.
    const UINT32 semantic=UINT32(m.semantic);
    EVENT_DATA_DESCRIPTOR data[6];
    EventDataDescCreate(&data[0],&m.d3d_allocation,8);
    EventDataDescCreate(&data[1],&m.dxg_allocation,8);
    EventDataDescCreate(&data[2],&m.offset,8);
    EventDataDescCreate(&data[3],&m.size,8);
    EventDataDescCreate(&data[4],&m.usage,4);
    EventDataDescCreate(&data[5],&semantic,4);
    api_.write(handle_,&descriptor(kind),6,data); // A lost event is the session's loss; the table stays right.
}
HRESULT AllocationLog::map(const AllocationMapping &m) noexcept {
    HRESULT hr=S_OK;
    AcquireSRWLockExclusive(&lock_);
    try {
        const auto [at,added]=live_.try_emplace(key(m),m);
        if (!added) hr=at->second==m ? S_FALSE : E_INVALIDARG;
        else if (enabled()) write_locked(EventKind::map,m);
    } catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
    ReleaseSRWLockExclusive(&lock_);
    return hr;
}
HRESULT AllocationLog::unmap(const AllocationMapping &m) noexcept {
    HRESULT hr=S_FALSE;
    AcquireSRWLockExclusive(&lock_);
    if (const auto at=live_.find(key(m)); at!=live_.end()) {
        if (enabled()) write_locked(EventKind::unmap,at->second);
        live_.erase(at); hr=S_OK;
    }
    ReleaseSRWLockExclusive(&lock_);
    return hr;
}
UINT AllocationLog::unmap_resource(UINT64 d3d) noexcept {
    UINT removed=0;
    AcquireSRWLockExclusive(&lock_);
    for (auto at=live_.lower_bound(Key{d3d,0,0});at!=live_.end() && std::get<0>(at->first)==d3d;++removed) {
        if (enabled()) write_locked(EventKind::unmap,at->second);
        at=live_.erase(at);
    }
    ReleaseSRWLockExclusive(&lock_);
    return removed;
}
HRESULT AllocationLog::replace(const AllocationMapping *mappings,UINT count) noexcept {
    if (!count) return S_OK;
    if (!mappings) return E_INVALIDARG;
    // A resource named twice in the batch is counted once (first_of).
    auto first_of=[&](UINT i) { for (UINT j=0;j<i;++j) if (mappings[j].d3d_allocation==mappings[i].d3d_allocation) return false; return true; };
    HRESULT hr=S_OK;
    AcquireSRWLockExclusive(&lock_);
    try {
        // Every allocation happens before the first change: the node store, and the nodes the batch has more of than
        // its resources had before.
        size_t existing=0;
        for (UINT i=0;i<count;++i) {
            if (!first_of(i)) continue;
            const UINT64 d3d=mappings[i].d3d_allocation;
            for (auto at=live_.lower_bound(Key{d3d,0,0});at!=live_.end() && std::get<0>(at->first)==d3d;++at) ++existing;
        }
        std::vector<decltype(live_)::node_type> nodes;
        nodes.reserve(existing>count ? existing : count);
        decltype(live_) extra;
        for (size_t i=existing;i<count;++i) extra.try_emplace(Key{0,0,i});
        // No allocation from here on.
        for (UINT i=0;i<count;++i) {
            if (!first_of(i)) continue;
            const UINT64 d3d=mappings[i].d3d_allocation;
            for (auto at=live_.lower_bound(Key{d3d,0,0});at!=live_.end() && std::get<0>(at->first)==d3d;) {
                const auto next=std::next(at);
                if (enabled()) write_locked(EventKind::unmap,at->second);
                nodes.push_back(live_.extract(at));
                at=next;
            }
        }
        while (!extra.empty()) nodes.push_back(extra.extract(extra.begin()));
        for (UINT i=0;i<count;++i) {
            auto node=std::move(nodes.back()); nodes.pop_back();
            node.key()=key(mappings[i]); node.mapped()=mappings[i];
            // A mapping that the batch names twice is mapped once.
            if (live_.insert(std::move(node)).inserted && enabled()) write_locked(EventKind::map,mappings[i]);
        }
    } catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
    ReleaseSRWLockExclusive(&lock_);
    return hr;
}
size_t AllocationLog::live() const noexcept {
    AcquireSRWLockShared(&lock_);
    const size_t count=live_.size();
    ReleaseSRWLockShared(&lock_);
    return count;
}
size_t AllocationLog::mappings_of(UINT64 d3d,AllocationMapping *out,size_t capacity) const noexcept {
    size_t count=0;
    AcquireSRWLockShared(&lock_);
    for (auto at=live_.lower_bound(Key{d3d,0,0});at!=live_.end() && std::get<0>(at->first)==d3d;++at,++count)
        if (out && count<capacity) out[count]=at->second;
    ReleaseSRWLockShared(&lock_);
    return count;
}
void AllocationLog::rundown() noexcept {
    // RundownAllocation for every live mapping (nf-umdprovider-umdetwregister.md 52-56), whether or not the provider
    // is enabled, as the header's callback does (umdprovider.h 109-121). The shared lock holds every map and unmap
    // until the rundown is complete, so each mapping is in the trace once: in the rundown, or in a later map event.
    AcquireSRWLockShared(&lock_);
    for (const auto &entry:live_) write_locked(EventKind::rundown,entry.second);
    ReleaseSRWLockShared(&lock_);
}
HRESULT AllocationLog::add_reporter(MemoryReporter *reporter) noexcept {
    if (!reporter) return E_INVALIDARG;
    HRESULT hr=S_OK;
    AcquireSRWLockExclusive(&reporters_lock_);
    try {
        if (std::find(reporters_.begin(),reporters_.end(),reporter)!=reporters_.end()) hr=S_FALSE;
        else {
            reporters_.push_back(reporter);
            // Under the lock: an enable change that comes now waits, and then reaches this reporter as well.
            reporter->set_reporting(enabled());
        }
    } catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
    ReleaseSRWLockExclusive(&reporters_lock_);
    return hr;
}
void AllocationLog::remove_reporter(MemoryReporter *reporter) noexcept {
    AcquireSRWLockExclusive(&reporters_lock_);
    const auto at=std::find(reporters_.begin(),reporters_.end(),reporter);
    if (at!=reporters_.end()) reporters_.erase(at);
    ReleaseSRWLockExclusive(&reporters_lock_);
}
void AllocationLog::write_external(EventKind kind,const AllocationMapping &m) const noexcept {
    AcquireSRWLockShared(&lock_);
    if (kind==EventKind::rundown || enabled()) write_locked(kind,m);
    ReleaseSRWLockShared(&lock_);
}
void AllocationLog::set_reporting(bool on) noexcept {
    AcquireSRWLockShared(&reporters_lock_);
    for (auto *reporter:reporters_) reporter->set_reporting(on);
    ReleaseSRWLockShared(&reporters_lock_);
}
void AllocationLog::report_external() noexcept {
    // After the table's rundown, which released lock_: a reporter writes its rundown through write_external.
    AcquireSRWLockShared(&reporters_lock_);
    for (auto *reporter:reporters_) reporter->report();
    ReleaseSRWLockShared(&reporters_lock_);
}
void NTAPI AllocationLog::enable_callback(LPCGUID,ULONG control,UCHAR,ULONGLONG,ULONGLONG,PEVENT_FILTER_DESCRIPTOR,
                                          PVOID context) {
    auto *log=static_cast<AllocationLog *>(context);
    if (!log) return;
    // The three control codes of the header's EnableCallback (umdprovider.h 101-122).
    switch (control) {
    case EVENT_CONTROL_CODE_DISABLE_PROVIDER:
        log->enabled_.store(false,std::memory_order_relaxed); log->set_reporting(false); break;
    case EVENT_CONTROL_CODE_ENABLE_PROVIDER:
        log->enabled_.store(true,std::memory_order_relaxed); log->set_reporting(true); break;
    case EVENT_CONTROL_CODE_CAPTURE_STATE: log->rundown(); log->report_external(); break;
    default: break;
    }
}
AllocationLog *process_allocation_log() noexcept {
    static const ProviderApi system{&::EventRegister,&::EventUnregister,&::EventWrite};
    try {
        static AllocationLog log(system); // An exception leaves it unbuilt, and the next call builds it again.
        return &log;
    } catch (...) { return nullptr; }
}
}
