// SPDX-License-Identifier: MIT
#include "engine-memory-log.h"
#if BC250_DXVK_ENGINE_ABI_MINOR>=8
namespace bc250::umd {
HRESULT EngineMemoryLog::start(IBc250DxvkDevice6 *engine,etw::AllocationLog *log) noexcept {
    if (!engine) return E_INVALIDARG;
    if (engine_) return E_UNEXPECTED;
    engine->AddRef();
    engine_=engine;
    if (!log) return S_FALSE;
    log_=log;
    const HRESULT hr=log->add_reporter(this);
    if (FAILED(hr)) log_=nullptr;
    return hr;
}
void EngineMemoryLog::stop() noexcept {
    // remove_reporter waits for a callback in progress; after it, no other thread calls the engine through here.
    if (log_) { log_->remove_reporter(this); log_=nullptr; }
    if (engine_) { engine_->SetMemoryReporting(FALSE); engine_->Release(); engine_=nullptr; }
}
HRESULT EngineMemoryLog::track(ID3D11Resource *resource,UINT64 cookie) noexcept {
    return engine_ ? engine_->TrackResourceMemory(resource,cookie) : S_FALSE;
}
UINT64 EngineMemoryLog::resolve(VkDeviceMemory memory,UINT64 *bytes) const noexcept {
    if (bytes) *bytes=0;
    if (!query_ || !device_ || !memory || !bytes) return 0;
    // The ICD reads the state of its BO: no lock and no kernel call (radv_wddm2_bo_get_kmt_allocation).
    MemoryAllocationInfo info{kMemoryAllocationRevision,uint32_t(sizeof(MemoryAllocationInfo)),0,0,0};
    if (query_(device_,memory,&info)!=VK_SUCCESS || !info.allocation) return 0;
    *bytes=info.bytes;
    return info.allocation;
}
bool EngineMemoryLog::convert(const BC250_DXVK_MEMORY_EVENT &e,etw::EventKind &kind,etw::AllocationMapping &m) noexcept {
    switch (e.Kind) {
    case BC250_DXVK_MEMORY_EVENT_MAP: kind=etw::EventKind::map; break;
    case BC250_DXVK_MEMORY_EVENT_UNMAP: kind=etw::EventKind::unmap; break;
    case BC250_DXVK_MEMORY_EVENT_RUNDOWN: kind=etw::EventKind::rundown; break;
    default: return false;
    }
    m.d3d_allocation=e.Resource; m.dxg_allocation=e.Allocation; m.offset=e.Offset; m.size=e.Size;
    // umdprovider.h 27-40: Packed is a range that shares its allocation; Renamed a range of a discard.
    m.usage=etw::usage::none;
    if (e.Offset || e.Size<e.AllocationSize) m.usage|=etw::usage::packed;
    if (e.Flags & BC250_DXVK_MEMORY_RENAMED) m.usage|=etw::usage::renamed;
    m.semantic=etw::AllocationSemantic::none;
    return true;
}
void EngineMemoryLog::events(const BC250_DXVK_MEMORY_EVENT *events,UINT32 count) const noexcept {
    if (!log_ || !events) return;
    for (UINT32 i=0;i<count;++i) {
        etw::EventKind kind; etw::AllocationMapping m;
        if (convert(events[i],kind,m)) log_->write_external(kind,m);
    }
}
void EngineMemoryLog::set_reporting(bool enabled) noexcept {
    if (engine_) engine_->SetMemoryReporting(enabled ? TRUE : FALSE);
}
void EngineMemoryLog::report() noexcept {
    if (engine_) engine_->ReportMemory();
}
}
#endif
