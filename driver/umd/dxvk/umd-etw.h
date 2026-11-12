// SPDX-License-Identifier: MIT
#pragma once
// The UMD logging provider of WDDM 1.2 (Microsoft-Windows-D3DUmdLogging, A688EE40-D8D9-4736-B6F9-6B74935BA3B1): which
// kernel allocation holds each Direct3D resource of this process. The contract is the WDK 10.0.26100 header
// um\umdprovider.h (types 27-72, provider 74-139, payload 141-169) with the descriptors of um\umdetw.h (12-43), and the
// pages display\user-mode-driver-logging.md (mandatory for a full graphics driver, lines 18-27) and
// umdprovider\nf-umdprovider-umdetw*.md. This module does not include umdprovider.h: that header defines RegHandle,
// Enabled, InRundown, Rundown, EnableCallback and LogMapAllocation in the global namespace (80-169) and gives the
// rundown callback no context. The events here have the same descriptors and the same six-field payload; the host
// test compiles umdprovider.h beside this header and checks that the types agree.
//
// The log keeps one table of live mappings for the whole process, because a capture-state request must report every
// mapping that exists (user-mode-driver-logging.md 27; nf-umdprovider-umdetwregister.md 56). The table changes only
// when a resource gets or loses its memory (create, open, destroy, identity rotation), never on a draw, a map or a
// Present. An event is written only while a session has the provider enabled: one relaxed load per table change.
// The memory of the engine's own resources is not in the table: the engine keeps it and reports it through a
// MemoryReporter (engine ABI 1.8), because its renames (Map with DISCARD) are on the draw path.
#include <windows.h>
#include <evntprov.h>
#include <atomic>
#include <map>
#include <tuple>
#include <vector>
namespace bc250::umd::etw {
// UMDETW_ALLOCATION_USAGE (umdprovider.h 27-40): bit 0 Packed, bit 1 Renamed, bits 16-31 for the driver.
namespace usage {
inline constexpr UINT32 none=0,packed=1u,renamed=2u;
}
// UMDETW_ALLOCATION_SEMANTIC (umdprovider.h 46-55). None is a Direct3D resource
// (ne-umdprovider-_umdetw_allocation_semantic.md 55-57); the others name driver-internal memory.
enum class AllocationSemantic : UINT32 {
    none=0,dma_buffer=1,upload_staging=2,download_staging=3,context_save=4,driver_other_min=5,driver_other_max=0xFFFF
};
// One mapping: the six fields of the payload, in its order (umdprovider.h 154-159).
//   d3d_allocation  the hResource of CreateResource or OpenResource, which is the DdiResource address; zero for memory
//                   the driver uses internally (nf-umdprovider-umdetwlogmapallocation.md 52-58).
//   dxg_allocation  the dxgkrnl allocation that holds it (same page, 60-62): the D3DKMT_HANDLE that pfnAllocateCb
//                   returned or that OpenResource received.
//   offset, size    the byte range of the Direct3D allocation inside the dxgkrnl allocation (same page, 64-70). A
//                   resource that owns a whole allocation has offset 0 and the size of that allocation.
struct AllocationMapping {
    UINT64 d3d_allocation=0,dxg_allocation=0,offset=0,size=0;
    UINT32 usage=usage::none;
    AllocationSemantic semantic=AllocationSemantic::none;
    friend bool operator==(const AllocationMapping &,const AllocationMapping &)=default;
};
// Event ids and opcodes of um\umdetw.h 38-43: MapAllocation id 1 opcode 1, RundownAllocation id 2 opcode 3,
// UnmapAllocation id 3 opcode 2. All three: level 0, task 1, keyword MAPPING_KEYWORD with the channel bit.
enum class EventKind : USHORT { map=1,rundown=2,unmap=3 };
// A source of mappings that the log does not keep in its table: an engine device that keeps the memory of its own
// resources and writes their events through write_external (engine ABI 1.8, BD-373). The log calls it from the
// provider's enable callback, on the thread of that callback, while the reporter is added.
class MemoryReporter {
public:
    // A session enabled or disabled the provider; also once with the current state when the reporter is added.
    virtual void set_reporting(bool enabled) noexcept=0;
    // A capture-state request: write a rundown of every mapping of the reporter before returning.
    virtual void report() noexcept=0;
protected:
    ~MemoryReporter()=default;
};
// The ETW calls of the log. The process log uses EventRegister, EventUnregister and EventWrite; the host test gives a
// recording sink.
struct ProviderApi {
    decltype(&::EventRegister) register_provider;
    decltype(&::EventUnregister) unregister_provider;
    decltype(&::EventWrite) write;
};
class AllocationLog {
public:
    // Can throw std::bad_alloc: the table allocates its first node.
    explicit AllocationLog(const ProviderApi &api) : api_(api) {}
    AllocationLog(const AllocationLog &)=delete;
    AllocationLog &operator=(const AllocationLog &)=delete;
    // Unregisters (nf-umdprovider-umdetwunregister.md 48: before the driver unloads). The live mappings get no unmap
    // event: no resource was destroyed, and the process end is in the kernel's own events.
    ~AllocationLog() { unregister_provider(); }
    // S_OK registers; S_FALSE when this log is registered already; a failed EventRegister as its HRESULT, and the
    // table is kept all the same. Never from DllMain: EventRegister can call the enable callback before it returns.
    HRESULT register_provider() noexcept;
    void unregister_provider() noexcept;
    // S_OK adds the mapping and writes MapAllocation. S_FALSE when the same mapping is live already (no event).
    // E_OUTOFMEMORY writes nothing, so the table and the trace still agree.
    HRESULT map(const AllocationMapping &) noexcept;
    // Removes the mapping with these d3d_allocation, dxg_allocation and offset, and writes UnmapAllocation with the
    // values of the map (nf-umdprovider-umdetwlogunmapallocation.md 86). S_FALSE when no such mapping is live.
    HRESULT unmap(const AllocationMapping &) noexcept;
    // Every mapping of one Direct3D resource, at its destruction; the count of mappings removed. A destroy without a
    // map (a create that failed) removes nothing and writes nothing.
    UINT unmap_resource(UINT64 d3d_allocation) noexcept;
    // Moves Direct3D resources to new memory as one change: first an unmap of every mapping of each named resource,
    // then a map of each new mapping. Existing table nodes are reused, so a move of a resource that had a mapping
    // cannot fail; E_OUTOFMEMORY only when a resource gets more mappings than it had, and then nothing has changed.
    // The seam for engine memory that moves, packs or renames (usage::packed, usage::renamed).
    HRESULT replace(const AllocationMapping *mappings,UINT count) noexcept;
    // A reporter takes part in enable changes and capture-state requests from now on until remove_reporter returns.
    // S_OK; E_OUTOFMEMORY; S_FALSE when it is added already. remove_reporter waits for a callback that calls it.
    HRESULT add_reporter(MemoryReporter *) noexcept;
    void remove_reporter(MemoryReporter *) noexcept;
    // One event of a reporter, without a table change: map and unmap only while a session has the provider enabled,
    // rundown always (inside a capture-state request).
    void write_external(EventKind,const AllocationMapping &) const noexcept;
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    size_t live() const noexcept;
    // The live mappings of one Direct3D resource in table order: copies at most `capacity` of them to `out` and
    // returns how many there are.
    size_t mappings_of(UINT64 d3d_allocation,AllocationMapping *out,size_t capacity) const noexcept;
    // The PENABLECALLBACK of the provider; context is the log. Public for the host test.
    static void NTAPI enable_callback(LPCGUID source,ULONG control,UCHAR level,ULONGLONG any,ULONGLONG all,
                                      PEVENT_FILTER_DESCRIPTOR filter,PVOID context);
private:
    using Key=std::tuple<UINT64,UINT64,UINT64>;
    static Key key(const AllocationMapping &m) noexcept { return {m.d3d_allocation,m.dxg_allocation,m.offset}; }
    void write_locked(EventKind,const AllocationMapping &) const noexcept;
    void rundown() noexcept;
    const ProviderApi api_;
    void set_reporting(bool) noexcept;
    void report_external() noexcept;
    // Lock order: reporters_lock_, then a reporter's own locks, then lock_. No call to a reporter holds lock_.
    mutable SRWLOCK lock_=SRWLOCK_INIT;          // live_ and handle_
    SRWLOCK registration_=SRWLOCK_INIT;          // register_provider and unregister_provider only
    SRWLOCK reporters_lock_=SRWLOCK_INIT;        // reporters_
    std::vector<MemoryReporter *> reporters_;
    std::map<Key,AllocationMapping> live_;
    REGHANDLE handle_=0;
    bool registered_=false;                      // under registration_
    std::atomic<bool> enabled_{false};
};
// The log of this process, with the system ETW calls. Built at first use; its destructor runs at the module's
// DLL_PROCESS_DETACH and unregisters the provider. Null only when the first construction had no memory (the table's
// first node); a later call tries again. Callers then log nothing, and the DDI call still completes.
AllocationLog *process_allocation_log() noexcept;
}
