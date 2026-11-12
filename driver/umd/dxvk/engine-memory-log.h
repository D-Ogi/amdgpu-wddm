// SPDX-License-Identifier: MIT
#pragma once
// BD-373: the UMD logging of the engine's own resources (engine ABI 1.8, IBc250DxvkDevice6). The engine keeps the
// current memory of each resource that the shell tracks and writes events only while a session has the provider
// enabled, so a rename (Map with DISCARD) costs no shell call when no session is on. This object is the shell's half
// for one device: the two Services callbacks, and the device's place in the process log's list of reporters.
//
// Threads. resolve runs inside an engine call, on the DDI thread. events runs there too, or on the thread of the
// provider's enable callback inside report. set_reporting and report run on that callback's thread. start and stop
// run on the DDI thread; stop returns only after a callback that uses the engine has finished.
#include "ddi/bc250_dxvk_engine.h"
#include "engine-memory-allocation.h"
#include "umd-etw.h"
// An engine header before 1.8 has no IBc250DxvkDevice6: the shell then has no engine logging (device-owner.cpp).
#if BC250_DXVK_ENGINE_ABI_MINOR>=8
namespace bc250::umd {
class EngineMemoryLog final : public etw::MemoryReporter {
public:
    EngineMemoryLog()=default;
    EngineMemoryLog(const EngineMemoryLog &)=delete;
    EngineMemoryLog &operator=(const EngineMemoryLog &)=delete;
    ~EngineMemoryLog() { stop(); }
    // The ICD query that names the kernel allocation of a VkDeviceMemory (BD-309), and the device it answers for.
    // Without it resolve answers 0, and the engine logs nothing.
    void bind(PFN_bc250GetMemoryAllocation query,VkDevice device) noexcept { query_=query; device_=device; }
    // Takes a reference to engine and joins log's reporters. S_FALSE without a log (no events then); the reference
    // is released at stop.
    HRESULT start(IBc250DxvkDevice6 *engine,etw::AllocationLog *log) noexcept;
    // Leaves the reporters and releases the engine. Before the engine's final Release.
    void stop() noexcept;
    bool active() const noexcept { return engine_!=nullptr; }
    // TrackResourceMemory of the engine (cookie 0 ends tracking); S_FALSE when the engine has no ABI 1.8.
    HRESULT track(ID3D11Resource *resource,UINT64 cookie) noexcept;
    // The Services callbacks, with this object as their context.
    UINT64 resolve(VkDeviceMemory memory,UINT64 *bytes) const noexcept;
    void events(const BC250_DXVK_MEMORY_EVENT *events,UINT32 count) const noexcept;
    // One engine event in the terms of the provider (umd-etw.h). Packed when the range is not the whole kernel
    // allocation; Renamed from the engine's flag. False for an unknown kind.
    static bool convert(const BC250_DXVK_MEMORY_EVENT &,etw::EventKind &,etw::AllocationMapping &) noexcept;
    void set_reporting(bool enabled) noexcept override;
    void report() noexcept override;
private:
    PFN_bc250GetMemoryAllocation query_=nullptr;
    VkDevice device_=VK_NULL_HANDLE;
    IBc250DxvkDevice6 *engine_=nullptr;
    etw::AllocationLog *log_=nullptr;
};
}
#endif
