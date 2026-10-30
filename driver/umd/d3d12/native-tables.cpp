// SPDX-License-Identifier: MIT
#include "native-tables.h"
#include "adapter-caps.h"
#include "device-state.h"
#include "device-engine.h"
#include "device-table.h"
#include "ddi-entry.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "engine-ddi/ddi-modern-core.h"
#include "engine-ddi/ddi-modern-list.h"
#include "app-state-ddi.h"
#endif
#include "ddi-trace.h"
#include "entry-owner.h"
#include "fence-ddi.h"
#include "shell-core-ddi.h"
#include "native-queue-ddi.h"
#include "native-residency-ddi.h"
#include "present-outputs.h"
#include "present-context.h"
#include "heap-import.h"
#include "engine-ddi/entry.h"
#include <atomic>
#include <cstring>
#include "stdio-log.h"

namespace native12 {
namespace {
// Diagnostics must not turn a normally refused malformed input into a fault.
// Copy only fixed-size SDK inputs; never follow handles or unbounded arrays.
template<class T> bool trace_input(const T* source,T& destination) noexcept {
    if(!source)return false;
    __try {destination=*source;return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
             ? EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {destination={};return false;}
}
struct EntryPolicy:EntryOwner<Device> {
    using Scope=DeviceEngineScope;
    // The recording slots of the list tables (ddi-entry.h, ListBinding::fast) enter through the device's
    // recording binding when it is published (lever L2, device-engine.h); otherwise, and for every other
    // table, through Scope with the trace hooks below.
    using FastScope=RecordingScope;
    static const RecordingBinding* fast_binding(Device& device) noexcept {return RecordingScope::admit(device.recording);}
    // The leave hook's failure record for a call refused on the fast path (failures-only trace mode 2; the
    // binding is never published in mode 1).
    // Out of line: 134 recording thunks would otherwise each carry the note's formatting on their cold path.
    __declspec(noinline) static void fast_denied(Device*,const char* name,HRESULT outcome) noexcept {
        if(FAILED(outcome) && outcome!=E_PENDING)ddi_failure_note(name,outcome);
    }
    static uint64_t entry(Device*,const char* name) noexcept {return ddi_trace_begin(name);}
    static void leave(Device*,const char* name,uint64_t id,HRESULT outcome) noexcept {ddi_trace_end(name,id,outcome);}
    // Sizes only: other scalar returns can be addresses, which a trace must not carry.
    static void returned(Device*,const char* name,uint64_t id,uint64_t value) noexcept {
        if(!id || !ddi_trace_enabled() || (std::strncmp(name,"pfnCalcPrivate",14) && std::strcmp(name,"pfnGetDescriptorSizeInBytes")))return;
        amdgpu_wddm_log::print("{\"event\":\"ddi-return\",\"sequence\":%llu,\"name\":\"%s\",\"value\":%llu}\n",
            static_cast<unsigned long long>(id),name,static_cast<unsigned long long>(value));
        amdgpu_wddm_log::flush();
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,DXGI_FORMAT format,UINT* output) noexcept {
        if(ddi_trace_mode()==2){
            char line[128];std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 format name=%s format=%u support=%08x\n",name,unsigned(format),output?*output:0);
            ddi_mode2_note(line);return;
        }
        if(!ddi_trace_enabled())return;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        amdgpu_wddm_log::print("{\"event\":\"ddi-format\",\"name\":\"%s\",\"format\":%u,\"output_present\":%u,\"support\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(format),unsigned(output!=nullptr),output?*output:0,now.QuadPart,GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,DXGI_FORMAT format,UINT samples,
        D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags,UINT* output) noexcept {
        if(ddi_trace_mode()==2){
            char line[128];std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 msaa format=%u samples=%u flags=%u levels=%u\n",unsigned(format),samples,unsigned(flags),output?*output:0);
            ddi_mode2_note(line);return;
        }
        if(!ddi_trace_enabled())return;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        amdgpu_wddm_log::print("{\"event\":\"ddi-msaa\",\"name\":\"%s\",\"format\":%u,\"samples\":%u,\"flags\":%u,\"output_present\":%u,\"levels\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(format),samples,unsigned(flags),unsigned(output!=nullptr),output?*output:0,now.QuadPart,GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    static void observed(Device* device,const char* name,D3D12DDI_HDEVICE,UINT count,UINT* map) noexcept {
        if(!ddi_trace_enabled() || !map || !count)return;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        amdgpu_wddm_log::print("{\"event\":\"ddi-node-map\",\"name\":\"%s\",\"count\":%u,\"first\":%u,\"lost\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,count,map[0],unsigned(device && device->lost.load()),now.QuadPart,GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,
        const D3D12DDIARG_CREATEHEAP_0001* heap,D3D12DDI_HHEAP,D3D12DDI_HRTRESOURCE,
        const D3D12DDIARG_CREATERESOURCE_0088* resource,const D3D12DDI_CLEAR_VALUES*,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,D3D12DDI_HRESOURCE) noexcept {
        if(!ddi_trace_enabled())return;
        D3D12DDIARG_CREATEHEAP_0001 h{};D3D12DDIARG_CREATERESOURCE_0088 r{};
        D3D12DDIARG_ROW_MAJOR_RESOURCE_LAYOUT row{};
        const bool heap_readable=trace_input(heap,h),resource_readable=trace_input(resource,r);
        const bool row_present=resource_readable && r.pRowMajorLayout;
        // WDK26100 d3d12umddi.h:10884: pitches are meaningful only for ROW_MAJOR.
        const bool row_readable=row_present && r.Layout==D3D12DDI_TL_ROW_MAJOR && trace_input(r.pRowMajorLayout,row);
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        amdgpu_wddm_log::print("{\"event\":\"ddi-create-heap-resource\",\"name\":\"%s\","
            "\"heap_present\":%u,\"heap_readable\":%u,\"heap_flags\":%u,\"cpu_page\":%u,\"memory_pool\":%u,"
            "\"bytes\":%llu,\"alignment\":%llu,\"creation_node_mask\":%u,\"visible_node_mask\":%u,"
            "\"resource_present\":%u,\"resource_readable\":%u,\"resource_type\":%u,\"layout\":%u,"
            "\"width\":%llu,\"height\":%u,\"depth\":%u,\"mips\":%u,\"resource_flags\":%u,\"num_castable_formats\":%u,"
            "\"row_major_present\":%u,\"row_major_readable\":%u,\"row_pitch\":%u,\"slice_pitch\":%u,"
            "\"protected_session_present\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(heap!=nullptr),unsigned(heap_readable),unsigned(h.Flags),unsigned(h.CPUPageProperty),unsigned(h.MemoryPool),
            static_cast<unsigned long long>(h.ByteSize),static_cast<unsigned long long>(h.Alignment),h.CreationNodeMask,h.VisibleNodeMask,
            unsigned(resource!=nullptr),unsigned(resource_readable),unsigned(r.ResourceType),unsigned(r.Layout),
            static_cast<unsigned long long>(r.Width),r.Height,unsigned(r.DepthOrArraySize),unsigned(r.MipLevels),unsigned(r.Flags),r.NumCastableFormats,
            unsigned(row_present),unsigned(row_readable),row.RowPitch,row.SlicePitch,unsigned(session.pDrvPrivate!=nullptr),
            now.QuadPart,GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,const D3D12DDIARG_CREATERESOURCE_0088* resource,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS optimization,UINT32 alignment_restriction,UINT visible_nodes,
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* output) noexcept {
        if(!ddi_trace_enabled())return;
        D3D12DDIARG_CREATERESOURCE_0088 r{};D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
        const bool resource_readable=trace_input(resource,r),output_readable=trace_input(output,info);
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        amdgpu_wddm_log::print("{\"event\":\"ddi-allocation-info\",\"name\":\"%s\",\"optimization\":%u,\"alignment_restriction\":%u,"
            "\"visible_node_mask\":%u,\"resource_readable\":%u,\"resource_type\":%u,\"layout\":%u,\"format\":%u,"
            "\"width\":%llu,\"height\":%u,\"depth\":%u,\"mips\":%u,\"samples\":%u,\"resource_flags\":%u,"
            "\"output_readable\":%u,\"data_size\":%llu,\"data_alignment\":%llu,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(optimization),alignment_restriction,visible_nodes,unsigned(resource_readable),
            unsigned(r.ResourceType),unsigned(r.Layout),unsigned(r.Format),static_cast<unsigned long long>(r.Width),r.Height,
            unsigned(r.DepthOrArraySize),unsigned(r.MipLevels),r.SampleDesc.Count,unsigned(r.Flags),
            unsigned(output_readable),static_cast<unsigned long long>(info.ResourceDataSize),
            static_cast<unsigned long long>(info.ResourceDataAlignment),now.QuadPart,GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    // Fixed-size fields only: no surface array, rectangle, private data or handle is followed or printed.
    static void observed(Device*,const char* name,D3D12DDI_HCOMMANDLIST,D3D12DDI_HCOMMANDQUEUE queue,
        const D3D12DDIARG_PRESENT_0001* args,D3D12DDI_PRESENT_0051* result,
        D3D12DDI_PRESENT_CONTEXTS_0051* contexts,D3D12DDI_PRESENT_HWQUEUES_0051* queues) noexcept {
        if(!ddi_trace_enabled())return;
        D3D12DDIARG_PRESENT_0001 a{};
        const bool readable=trace_input(args,a);
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        amdgpu_wddm_log::print("{\"event\":\"ddi-present\",\"name\":\"%s\",\"args_readable\":%u,\"queue_present\":%u,"
            "\"surfaces\":%u,\"surfaces_present\":%u,\"destination_present\":%u,\"destination_subresource\":%u,"
            "\"flags\":%u,\"flip_interval\":%u,\"vidpn_source\":%u,\"dirty_rects\":%u,\"private_size\":%u,"
            "\"private_present\":%u,\"optimize_for_composition\":%u,\"result_present\":%u,\"contexts_present\":%u,"
            "\"hwqueues_present\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(readable),unsigned(queue.pDrvPrivate!=nullptr),a.SurfacesToPresent,
            unsigned(a.phSurfacesToPresent!=nullptr),unsigned(a.hDstResource.pDrvPrivate!=nullptr),a.DstSubResourceIndex,
            unsigned(a.Flags.Value),unsigned(a.FlipInterval),unsigned(a.VidPnSourceID),a.DirtyRects,a.PrivateDriverDataSize,
            unsigned(a.pPrivateDriverData!=nullptr),unsigned(a.OptimizeForComposition),unsigned(result!=nullptr),
            unsigned(contexts!=nullptr),unsigned(queues!=nullptr),now.QuadPart,GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    // Device, command list, state object and the null resolver: entry-owner.h, shared with device-table-test.cpp.
    using EntryOwner<Device>::resolve;
    static Device* resolve(D3D12DDI_HCOMMANDQUEUE handle) noexcept {return resolve_queue_device(handle);}
    static void failure(Device* device,HRESULT hr) noexcept {
        if(!device)return;
        if(device_engine_entered(*device))report_device_error(*device,hr);
        else device->lost.store(true);
    }
};
// The entry path experiment's timing (engine-ddi.h, "Entry path"; entry.h): with BC250_ENTRY_STATS=1 a timed slot's
// table entry counts the call, and samples its time, around the shell's own entry. Table tells the two list tables
// apart, whose entries for one member differ.
template<engine_ddi::EntryClass C,unsigned Table,class Fn> struct TimedEntry;
template<engine_ddi::EntryClass C,unsigned Table,class R,class... A> struct TimedEntry<C,Table,R(APIENTRY*)(A...)> {
    inline static R(APIENTRY* target)(A...)=nullptr;
    static R APIENTRY call(A... args) noexcept {
        engine_ddi::EntryTimer timer(C);
        return target(args...);
    }
};
// A refill of a table passes the same entries again: the target stays the shell's entry.
template<engine_ddi::EntryClass C,unsigned Table,class Fn> void time_entry(Fn& slot) noexcept {
    using T=TimedEntry<C,Table,Fn>;
    if(slot!=&T::call){T::target=slot;slot=&T::call;}
}
// Table is the table number, plus 2 for the 0094 layout: the two layouts wrap their slots apart (ddi-entry.h).
template<unsigned Table,class List> void time_list(List& t) noexcept {
#define N12_TIME_LIST(name) time_entry<engine_ddi::EntryClass::name,Table>(t.pfn##name);
    ENGINE_DDI_ENTRY_TIMED_LIST_CLASSES(N12_TIME_LIST)
#undef N12_TIME_LIST
}
template<unsigned Table,class Core> void time_core(Core& t) noexcept {
#define N12_TIME_CORE(name) time_entry<engine_ddi::EntryClass::name,Table>(t.pfn##name);
    ENGINE_DDI_ENTRY_TIMED_CORE_CLASSES(N12_TIME_CORE)
#undef N12_TIME_CORE
}
void time_queue(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001& t) noexcept {
#define N12_TIME_QUEUE(name) time_entry<engine_ddi::EntryClass::name,0>(t.pfn##name);
    ENGINE_DDI_ENTRY_TIMED_QUEUE_CLASSES(N12_TIME_QUEUE)
#undef N12_TIME_QUEUE
}
engine_ddi::DeviceContext* APIENTRY resolve_engine(D3D12DDI_HDEVICE handle) {
    auto device=static_cast<Device*>(handle.pDrvPrivate);
    return device && device_engine_entered(*device)?engine_context(*device):nullptr;
}
UINT APIENTRY present_private_size(D3D12DDI_HDEVICE,const D3D12DDIARG_PRESENT_0001*) {return 0;}
// The two resource DDIs are engine-ddi's, run inside the owner scope of the shell's imports: a linear
// primary is released by its runtime resource only while that resource's own DDI is running.
using CoreTable=D3D12DDI_DEVICE_FUNCS_CORE_0088;
// The AllowOutOfMemory clamp exists twice, once per module: engine_ddi::admitted_create_failure for what the
// slots themselves decide, native12::ddi_admitted_create_failure for what the DDI thunk refuses above them
// (BD-075). This translation unit is the one that sees both, so it is where they are held to the same answer -
// including the device codes, which must come out as D3DDDIERR_DEVICEREMOVED and not as the DXGI name.
static_assert(engine_ddi::admitted_create_failure(S_OK)==ddi_admitted_create_failure(S_OK));
static_assert(engine_ddi::admitted_create_failure(E_OUTOFMEMORY)==ddi_admitted_create_failure(E_OUTOFMEMORY));
static_assert(engine_ddi::admitted_create_failure(E_NOTIMPL)==ddi_admitted_create_failure(E_NOTIMPL));
static_assert(engine_ddi::admitted_create_failure(E_INVALIDARG)==ddi_admitted_create_failure(E_INVALIDARG));
static_assert(engine_ddi::admitted_create_failure(E_UNEXPECTED)==ddi_admitted_create_failure(E_UNEXPECTED));
static_assert(engine_ddi::admitted_create_failure(E_FAIL)==ddi_admitted_create_failure(E_FAIL));
static_assert(engine_ddi::admitted_create_failure(DXGI_ERROR_DEVICE_REMOVED)==engine_ddi::kDriverDeviceRemoved);
static_assert(engine_ddi::admitted_create_failure(DXGI_ERROR_DEVICE_RESET)==engine_ddi::kDriverDeviceRemoved);
static_assert(engine_ddi::admitted_create_failure(DXGI_ERROR_DEVICE_HUNG)==engine_ddi::kDriverDeviceRemoved);
static_assert(ddi_admitted_create_failure(DXGI_ERROR_DEVICE_REMOVED)==engine_ddi::kDriverDeviceRemoved);
static_assert(ddi_admitted_create_failure(DXGI_ERROR_DEVICE_RESET)==engine_ddi::kDriverDeviceRemoved);
static_assert(ddi_admitted_create_failure(DXGI_ERROR_DEVICE_HUNG)==engine_ddi::kDriverDeviceRemoved);
static_assert(engine_ddi::kDriverDeviceRemoved==kDdiDriverDeviceRemoved);
std::atomic<decltype(CoreTable{}.pfnCreateHeapAndResource)> engine_create_resource{};
std::atomic<decltype(CoreTable{}.pfnDestroyHeapAndResource)> engine_destroy_resource{};
std::atomic<decltype(CoreTable{}.pfnOpenHeapAndResource)> engine_open_resource{};
HRESULT APIENTRY create_heap_and_resource(D3D12DDI_HDEVICE handle,const D3D12DDIARG_CREATEHEAP_0001* heap,
    D3D12DDI_HHEAP driver_heap,D3D12DDI_HRTRESOURCE runtime,const D3D12DDIARG_CREATERESOURCE_0088* resource,
    const D3D12DDI_CLEAR_VALUES* clear,D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,
    D3D12DDI_HRESOURCE driver_resource) {
    const auto engine=engine_create_resource.load();
    const auto device=static_cast<Device*>(handle.pDrvPrivate);
    // Every failure of this slot leaves it as E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED (engine-ddi.h,
    // admitted_create_failure): a create DDI that reports anything else costs the application its device
    // (BD-075). The scope's own refusals come through here as well, which is why the clamp sits outside the
    // scope and not only in engine-ddi. The thunk above this wrapper clamps its own refusals itself
    // (ddi-entry.h, CoreBinding::allow_out_of_memory), which this wrapper never sees.
    if(!engine || !device)return engine_ddi::admitted_create_failure(E_UNEXPECTED);
    // A resource DDI entered from inside another one of this device is refused before the engine runs.
    return engine_ddi::admitted_create_failure(create_in_owner_scope(engine_imports(*device),[&]() noexcept {
        return engine(handle,heap,driver_heap,runtime,resource,clear,session,driver_resource);
    }));
}
// The open slot, in the same scope as the create (BD-075 review, 2026-10-06). An adopted record takes no
// authority of its own - begin_owner_scope skips it, because a borrowed allocation names no runtime resource - so
// the scope adds no permission here; what it adds is the refusal of a resource DDI entered from inside another one
// of this device, which the create has and the open had not, and the clamp outside the scope for the scope's own
// refusals. Without it the open was the one resource DDI of this table that could nest.
HRESULT APIENTRY open_heap_and_resource(D3D12DDI_HDEVICE handle,const D3D12DDIARG_OPENHEAP_0003* args,
    D3D12DDI_HHEAP driver_heap,D3D12DDI_HRTRESOURCE runtime,D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,
    D3D12DDI_HRESOURCE driver_resource) {
    const auto engine=engine_open_resource.load();
    const auto device=static_cast<Device*>(handle.pDrvPrivate);
    if(!engine || !device)return engine_ddi::admitted_create_failure(E_UNEXPECTED);
    return engine_ddi::admitted_create_failure(create_in_owner_scope(engine_imports(*device),[&]() noexcept {
        return engine(handle,args,driver_heap,runtime,session,driver_resource);
    }));
}
void APIENTRY destroy_heap_and_resource(D3D12DDI_HDEVICE handle,D3D12DDI_HHEAP heap,D3D12DDI_HRESOURCE resource) {
    const auto engine=engine_destroy_resource.load();
    const auto device=static_cast<Device*>(handle.pDrvPrivate);
    if(!engine || !device){if(device)report_device_error(*device,E_UNEXPECTED);return;}
    D3DKMT_HANDLE primary=0;
    // Not a linear primary, or no resource at all: the scope names no record.
    if(resource.pDrvPrivate && engine_ddi::present_allocation(engine_context(*device),resource,&primary)!=S_OK)primary=0;
    // BD-133: a source presented by copying it has a pool of copy surfaces, which kernel presents on the device's
    // present context read and which go with this resource. The engine holds a release for this device's queues only,
    // so the present context proves first, on the CPU and bounded, that each surface's last copy completed and its
    // last reader retired, and destroys its completion fence (present-context.h, retire). A surface it cannot prove
    // idle is kept with its fence, and the engine keeps its memory (engine-ddi release_present_copy, R3 of the review
    // of 2026-10-12); the failure is reported. The destroy still runs, because a destroy that refuses leaves the
    // objects to nobody.
    PresentCopySlot* copy_slots[kPresentCopySurfacesPerSource]{};
    unsigned copy_count=0;
    if(resource.pDrvPrivate &&
       engine_ddi::present_copy_slots(engine_context(*device),resource,copy_slots,kPresentCopySurfacesPerSource,
                                      &copy_count)==S_OK && copy_count){
        const HRESULT retired=device->present.retire(copy_slots,copy_count,10000);
        if(retired!=S_OK)report_device_error(*device,retired);
    }
    // Refused: nothing is destroyed, the objects stay with the device, and the device error says so.
    const HRESULT hr=destroy_in_owner_scope(engine_imports(*device),primary,[&]() noexcept {engine(handle,heap,resource);});
    if(hr!=S_OK)report_device_error(*device,hr);
}
// BD-133: what present_source (present-outputs.h) calls for stage 3 and stage 6, and what the copy's sequence
// (present-context.h, present_copy_answer) calls of the engine. The hooks exist so that the order between them is a
// pure function the host suites run, and so that this file holds nothing but the engine-ddi calls themselves.
struct PresentSourceCall {
    engine_ddi::DeviceContext* engine;
    D3D12DDI_HCOMMANDLIST list;
    D3D12DDI_HRESOURCE surface;
    Device* device;
    unsigned* stage;
    HANDLE present_context;                     // set by a copy: the context its kernel present is made on
};
HRESULT present_source_direct(void* state,D3DKMT_HANDLE* allocation) noexcept {
    const auto& call=*static_cast<const PresentSourceCall*>(state);
    return engine_ddi::present_allocation(call.engine,call.surface,allocation);
}
HRESULT present_copy_mark_of(void* state,PresentCopyListMark** mark) noexcept {
    const auto& call=*static_cast<const PresentSourceCall*>(state);
    return engine_ddi::present_copy_list_mark(call.engine,call.list,mark);
}
HRESULT present_copy_record(void* state,const PresentCopyPick& pick,PresentCopySlot** slot,
    D3DKMT_HANDLE* allocation) noexcept {
    const auto& call=*static_cast<const PresentSourceCall*>(state);
    return engine_ddi::present_copy_allocation(call.engine,call.list,call.surface,pick,slot,allocation);
}
// Rule D (present-copy-sync.h): the list that carried an unexecuted ticket was reset or destroyed. Its present waits
// for a copy that never comes and no CPU signal may stand in for it: the present context keeps the surface and
// itself, and the device is removed. Called from the list's Reset or destroy DDI, on its thread.
void present_copy_cancelled(void* owner,const PresentCopyTicket& ticket) noexcept {
    auto& device=*static_cast<Device*>(owner);
    device.present.cancel(ticket);
    device.remove();
}
HRESULT present_source_copy(void* state,D3DKMT_HANDLE* allocation) noexcept {
    auto& call=*static_cast<PresentSourceCall*>(state);
    const PresentCopyEngine engine{&call,present_copy_mark_of,present_copy_record};
    return present_copy_answer(call.device->present,call.device->runtime.handle,call.device->kernel_callbacks,engine,
        present_copy_cancelled,call.device,allocation,&call.present_context,*call.stage);
}
// R6 (present-outputs.h, present_copy_flags_admitted): a present of a source that needs the copy, with flags that are
// not the windowed Blt. One line per flags value that differs from the last one named.
void present_copy_flags_refused(UINT flags) noexcept {
    static std::atomic<UINT> named{0};
    if(named.exchange(flags,std::memory_order_relaxed)==flags)return;
    ddi_refusal("present copy: a present with flags 0x%x (Blt 0x1, Flip 0x2) names a source that is no linear primary: "
                "only a windowed Blt is copied, so the present is refused before any copy is recorded, and because "
                "pfnPresent names no status the runtime loses the device",
                flags);
}
// BD-133: the kernel present compares the LB7A format of the source with the destination's and refuses a pair that
// differs (driver\kmd\wddm_allocation_identity.inc, Bc250SnapshotFormat; counted as "snapshot refused ... format").
// A copied source has the copy surface's format, which need not be the destination's: one line per pair that
// differs, so that the lab tells a refusal of the format from a refusal of the context. Nothing is changed by it.
void present_copy_destination_check(engine_ddi::DeviceContext* engine,D3D12DDI_HRESOURCE source,
    D3D12DDI_HRESOURCE destination) noexcept {
    uint32_t copy=0,dxgi=0,lb7a=0;
    if(engine_ddi::present_copy_surface_format(engine,source,&copy)!=S_OK)return;
    if(engine_ddi::present_destination_format(engine,destination,&dxgi,&lb7a)!=S_OK || lb7a==copy)return;
    // The last pair named; a pair that alternates with another is named again, which is still once per change.
    static std::atomic<uint64_t> named{0};
    const uint64_t pair=(uint64_t{copy}<<32)|lb7a;
    if(named.exchange(pair,std::memory_order_relaxed)==pair)return;
    ddi_refusal("present copy: the destination's LB7A format %u (by its DXGI format %u; 0: no row) is not the copy "
                "surface's LB7A format %u, and the kernel present refuses a source and a destination of two formats "
                "(Bc250SnapshotFormat): a refusal of this present is the format's, not the present context's",
                lb7a,dxgi,copy);
}
// The surface's allocation and a context go back to the runtime, which makes the kernel call: the queue's context
// for a linear primary, the present context for a copied source (BD-133, present-context.h).
// stage: 1 arguments, 2 queue, 3 surface, 4 destination, 5 outputs, 6 the copy of a source that is no linear
// primary (BD-133), 7 the present context of that copy (BD-133), 8 a source that needs the copy in a present that is
// not a windowed Blt (BD-133, R6), 0 done. The numbers of the stages that existed before BD-133 are unchanged: a
// trace line of an earlier package means what it meant. The destination is resolved before the source since BD-133,
// so that a refused destination fails the present before a copy is recorded or its wait is queued.
HRESULT present_outputs(Device& device,D3D12DDI_HCOMMANDLIST list,D3D12DDI_HCOMMANDQUEUE queue,
    const D3D12DDIARG_PRESENT_0001* args,
    D3D12DDI_PRESENT_0051* result,D3D12DDI_PRESENT_CONTEXTS_0051* contexts,
    D3D12DDI_PRESENT_HWQUEUES_0051* queues,unsigned& stage,HANDLE& queue_context) noexcept {
    queue_context=nullptr;
    stage=1;
    HRESULT hr=check_present(args,result,contexts);if(hr!=S_OK)return hr;
    PresentSources from;
    stage=2;
    hr=queue_present_context(queue,device,&from.context);if(hr!=S_OK)return hr;
    queue_context=from.context;
    // Stage 3 is a linear primary of this device: the one kind of surface a reader outside the engine can open,
    // and the one present that costs nothing. Its path is unchanged, byte for byte: the allocation of the surface
    // itself goes to the runtime, nothing is recorded on the list and AddedGpuWork stays FALSE.
    //   Stage 6 (BD-133) is every other source of this device - an ordinary texture, which is what a D3D9On12
    // back buffer is: it is presented by copying it into a surface of that kind on the command list the runtime
    // handed to this DDI and closes and executes right afterwards (engine-ddi.h, present_copy_allocation). Before
    // BD-133 the refusal of stage 3 left the DDI with an error, which costs the application its device at the
    // first windowed present: the measured failure of BD-133.
    //   The order and the outputs of the two are present_source (present-outputs.h), which is host-tested; the copy's
    // own sequence is present_copy_answer (present-context.h): the present context and its fence (stage 7), the
    // surface of the source's pool and the recorded copy (stage 6), the copy's ticket and the present context's GPU
    // wait for it (stage 7), in that order. The kernel present of a copy is made on the present context, a context
    // without private driver data: the queue's context is a UMD context, and the kernel driver refuses a present on
    // one (present-context.h, lab arm B of 2026-10-12 17:26Z). A failure is the device error of this present, as
    // every other stage's; a refusal for want of a reusable surface or of budget comes before anything is recorded.
    PresentSourceCall call{engine_context(device),list,args->phSurfacesToPresent[0].hSurface,&device,&stage,nullptr};
    if(args->hDstResource.pDrvPrivate){
        stage=4;
        // The destination may also be a surface this device opened (BD-075): the runtime named it and owns it for
        // the call. The source form stays the strict one.
        hr=engine_ddi::present_destination_allocation(call.engine,args->hDstResource,&from.destination);
        if(hr!=S_OK)return hr;
    }
    hr=present_source({&call,present_source_direct,present_source_copy},from,stage,args->Flags);
    if(stage==8)present_copy_flags_refused(args->Flags.Value);
    if(hr!=S_OK)return hr;
    if(from.added_gpu_work){
        from.context=call.present_context;
        if(args->hDstResource.pDrvPrivate)present_copy_destination_check(call.engine,call.surface,args->hDstResource);
    }
    stage=5;
    // The per-application VSync setting as the sync interval override of this present.
    const auto vsync=amdgpu_wddm::app_settings::sync_override(device.settings());
    from.sync_override_valid=vsync.valid;
    from.sync_override=vsync.interval ? DXGI_DDI_FLIP_INTERVAL_ONE : DXGI_DDI_FLIP_INTERVAL_IMMEDIATE;
    hr=fill_present(from,result,contexts,queues);
    if(hr==S_OK)stage=0;
    return hr;
}
void APIENTRY present(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HCOMMANDQUEUE queue,
    const D3D12DDIARG_PRESENT_0001* args,D3D12DDI_PRESENT_0051* result,
    D3D12DDI_PRESENT_CONTEXTS_0051* contexts,D3D12DDI_PRESENT_HWQUEUES_0051* queues) {
    // The entry path experiment's frame clock and arms (engine-ddi.h, "Entry path"); nothing without its knobs.
    engine_ddi::entry_frame();
    const auto device=EntryPolicy::resolve(list);
    // The settings of this device (Device::settings): those of a used application-specific driver state blob, or
    // else the process settings.
    const auto& settings=device?device->settings():amdgpu_wddm::app_settings::process_settings();
    // The per-application FrameRateLimit (docs/design/per-app-graphics-settings.md): the wait comes before the
    // queue domain, so that no other queue operation of the device waits with this present.
    if(device)device->frame_limiter.frame(amdgpu_wddm::app_settings::frame_rate_limit(settings));
    unsigned stage=1;
    HANDLE queue_context=nullptr;
    HRESULT hr;
    {
        // A queue operation: its context must not be executing on another thread (QueueDomainScope).
        QueueDomainScope serial(device);
        // Every output given is zero before anything is validated, and again after a refusal.
        if(result)*result={};if(contexts)*contexts={};if(queues)*queues={};
        hr=device?present_outputs(*device,list,queue,args,result,contexts,queues,stage,queue_context):E_INVALIDARG;
        if(hr!=S_OK){if(result)*result={};if(contexts)*contexts={};if(queues)*queues={};}
    }
    // The per-application MaxFrameLatency (frame-latency.h): the application may have at most N presents ahead
    // of the GPU, so this present returns only once the frame N places back has finished. Outside the queue
    // domain, as the frame rate limit is, and never on a refused present. Without the setting nothing happens.
    if(device && hr==S_OK)
        engine_frame_gate(*device,amdgpu_wddm::app_settings::max_frame_latency(settings));
    // The frame-stall line (engine-ddi present_frame): after the latency gate, so that a frame's line counts the
    // time this present waited there as part of the frame.
    if(device && hr==S_OK)engine_ddi::present_frame(engine_context(*device));
    if(ddi_trace_enabled()){
        // present_context is the identity of the context the kernel present is made on, and queue_context that of
        // the queue the call named; they are equal for a linear primary and differ for a stage-6 copy, whose kernel
        // present is made on the device's present context (BD-133, present-context.h). flags is the runtime's
        // DXGI_DDI_PRESENT_FLAGS value (Blt 0x1, Flip 0x2): only a windowed Blt is copied (R6, stage 8 otherwise).
        D3D12DDIARG_PRESENT_0001 traced{};
        const UINT flags=trace_input(args,traced)?traced.Flags.Value:0u;
        amdgpu_wddm_log::print("{\"event\":\"present-outputs\",\"stage\":%u,\"status\":\"%08lx\",\"flags\":%u,"
            "\"source\":%u,\"destination\":%u,\"context\":%u,\"added_gpu_work\":%u,"
            "\"present_context\":%llu,\"queue_context\":%llu,\"list\":%llu,\"thread\":%lu}\n",stage,
            static_cast<unsigned long>(hr),flags,unsigned(hr==S_OK && result->BroadcastSrcAllocation[0]!=0),
            unsigned(hr==S_OK && result->BroadcastDstAllocation[0]!=0),
            unsigned(hr==S_OK && contexts->hContext!=nullptr),
            unsigned(hr==S_OK && result->AddedGpuWork!=FALSE),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hr==S_OK ? contexts->hContext : nullptr)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(queue_context)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(list.pDrvPrivate)),GetCurrentThreadId());
        amdgpu_wddm_log::flush();
    }
    if(hr!=S_OK && device)report_device_error(*device,hr);
}
using Queue=D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001;
using Extended=D3D12DDI_EXTENDED_FEATURES_FUNCS_0021;
const Queue& queue_original() noexcept {
    static const Queue table=[]() noexcept {Queue value{};fill_native_queue_table(&value);return value;}();return table;
}
const Extended& extended_original() noexcept {
    static const Extended table=[]() noexcept {Extended value{};fill_native_extended_table(&value);return value;}();return table;
}
struct QueueName_pfnExecuteCommandLists {static const char* name() noexcept {return "pfnExecuteCommandLists";}};
struct QueueName_pfnUpdateTileMappings {static const char* name() noexcept {return "pfnUpdateTileMappings";}};
struct QueueName_pfnCopyTileMappings {static const char* name() noexcept {return "pfnCopyTileMappings";}};
struct QueueName_pfnSignalFence {static const char* name() noexcept {return "pfnSignalFence";}};
struct QueueName_pfnWaitForFence {static const char* name() noexcept {return "pfnWaitForFence";}};
template<auto Member,class Name> struct QueueBinding {static const char* name() noexcept {return Name::name();} static auto original() noexcept {return queue_original().*Member;}};
struct ExtendedName_pfnGetSupportedExtendedFeatures {static const char* name() noexcept {return "pfnGetSupportedExtendedFeatures";}};
struct ExtendedName_pfnGetSupportedExtendedFeatureVersions {static const char* name() noexcept {return "pfnGetSupportedExtendedFeatureVersions";}};
struct ExtendedName_pfnEnableExtendedFeature {static const char* name() noexcept {return "pfnEnableExtendedFeature";}};
struct ExtendedName_pfnSetExtendedFeatureCallbacks {static const char* name() noexcept {return "pfnSetExtendedFeatureCallbacks";}};
template<auto Member,class Name> struct ExtendedBinding {static const char* name() noexcept {return Name::name();} static auto original() noexcept {return extended_original().*Member;}};
#if defined(D3D12DDI_BUILD_VERSION_0119)
using Extended96=D3D12DDI_EXTENDED_FEATURES_FUNCS_0096;
const Extended96& extended96_original() noexcept {
    static const Extended96 table=[]() noexcept {Extended96 value{};fill_native_extended_table(&value);return value;}();return table;
}
template<auto Member,class Name> struct Extended96Binding:Name {
    static auto original() noexcept {return extended96_original().*Member;}
};
#endif
}
#if defined(D3D12DDI_BUILD_VERSION_0119)
void APIENTRY modern_core_error(D3D12DDI_HDEVICE handle,HRESULT hr) {
    if(auto* device=EntryPolicy::resolve(handle))report_device_error(*device,hr);
}
void APIENTRY modern_list_error(D3D12DDI_HCOMMANDLIST handle,HRESULT hr) {
    engine_ddi::report_command_list_error(handle,hr);
}
#endif
HRESULT fill_native_tables(Adapter& adapter,D3D12DDI_TABLE_TYPE type,void* output,SIZE_T size,
    UINT number,D3D12DDI_HRTTABLE runtime) noexcept {
    if(!output)return E_INVALIDARG;
    const engine_ddi::FillInfo fill{sizeof(fill),resolve_engine};
    switch(type){
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE:{
        using Core=D3D12DDI_DEVICE_FUNCS_CORE_0088;
        if(number)return E_INVALIDARG;
        Core shell{},original{};
        install_native_queue_entries(shell);install_fence_entries(shell);
        install_shell_core_entries(shell);install_native_residency_entries(shell);
        shell.pfnGetPresentPrivateDriverDataSize=present_private_size;
        HRESULT hr=compose_core_0092(&original,sizeof(original),shell,fill);if(hr!=S_OK)return hr;
        if(!original.pfnCreateHeapAndResource || !original.pfnDestroyHeapAndResource ||
           !original.pfnOpenHeapAndResource)return E_UNEXPECTED;
        engine_create_resource.store(original.pfnCreateHeapAndResource);
        engine_destroy_resource.store(original.pfnDestroyHeapAndResource);
        engine_open_resource.store(original.pfnOpenHeapAndResource);
        original.pfnCreateHeapAndResource=create_heap_and_resource;
        original.pfnDestroyHeapAndResource=destroy_heap_and_resource;
        original.pfnOpenHeapAndResource=open_heap_and_resource;
#if defined(D3D12DDI_BUILD_VERSION_0119)
        // The application-specific driver state is the shell's (app-state-ddi.h): type 1 of CheckDriverMatchingIdentifier
        // is answered here, every other type by the engine.
        if(!original.pfnCheckDriverMatchingIdentifier)return E_UNEXPECTED;
        engine_check_identifier.store(original.pfnCheckDriverMatchingIdentifier,std::memory_order_release);
        original.pfnCheckDriverMatchingIdentifier=check_driver_matching_identifier;
#endif
        return with_ddi_layout(adapter.ddi_version.load(std::memory_order_acquire)>>16,[&](auto layout)->HRESULT {
            using Layout=decltype(layout); using C=typename Layout::Core;
            if(size!=sizeof(C))return E_INVALIDARG;
            C table{},wrapped_table{};
            if constexpr(std::is_same_v<C,Core>)table=original;
#if defined(D3D12DDI_BUILD_VERSION_0119)
            else {
                ModernCoreEntries entries=engine_ddi::modern_core_entries();
                entries.blob_size=app_state_blob_size;entries.blob=app_state_blob;
                entries.blob_status=app_state_blob_status;
                if(!upgrade_core(original,&table,modern_core_error,entries))return E_UNEXPECTED;
            }
#endif
            const HRESULT result=DdiEntryTables<EntryPolicy>::wrap_core(table,&wrapped_table);
            if(result!=S_OK)return result;
            if(engine_ddi::entry_stats_on())time_core<Layout::version>(wrapped_table);
            *static_cast<C*>(output)=wrapped_table;return S_OK;
        });
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D:{
        using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
        using List94=D3D12DDI_COMMAND_LIST_FUNCS_3D_0094;
        if(number>1 || !runtime.handle)return E_INVALIDARG;
        return with_ddi_layout(adapter.ddi_version.load(std::memory_order_acquire)>>16,[&](auto layout)->HRESULT {
            using Layout=decltype(layout); using L=typename Layout::List;
            if(size!=sizeof(L))return E_INVALIDARG;
            List original{};
            HRESULT hr=compose_list_0092(&original,sizeof(original),number,present,fill);if(hr!=S_OK)return hr;
            L table{},wrapped{};
            if constexpr(std::is_same_v<L,List>) {
                if(!engine_ddi::select_barrier(&original,Layout::version))return E_UNEXPECTED;
                table=original;
            }else {
                List94 base{};
                if(!engine_ddi::make_list_0094(original,&base))return E_UNEXPECTED;
                if constexpr(std::is_same_v<L,List94>)table=base;
#if defined(D3D12DDI_BUILD_VERSION_0119)
                else if(!upgrade_list(base,&table,number,modern_list_error,engine_ddi::modern_list_entries()))return E_UNEXPECTED;
#endif
            }
            hr=DdiEntryTables<EntryPolicy>::wrap_list(number,table,&wrapped);if(hr!=S_OK)return hr;
            if(engine_ddi::entry_stats_on()) {
                if(number)time_list<2*Layout::version+1>(wrapped);else time_list<2*Layout::version>(wrapped);
            }
            // Older direct entries carry driver handles. Newer layouts keep their typed wrappers until a
            // matching direct-entry implementation has been measured; never reinterpret an API handle.
            if constexpr(std::is_same_v<L,List> || std::is_same_v<L,List94>) {
                if(number==1 && !ddi_experiment_off("direct-entry") && !ddi_experiment_off("recording-bind") &&
                   !ddi_experiment_off("deferred-replay") && ddi_trace_mode()!=1)
                    (void)engine_ddi::install_direct_list(&wrapped,1);
            }
            HRESULT result=S_OK;
            AcquireSRWLockExclusive(&adapter.tables_lock);
            auto& prior=adapter.list_tables[number];
            if(prior.handle && prior.handle!=runtime.handle)result=E_UNEXPECTED;
            else{prior=runtime;*static_cast<L*>(output)=wrapped;}
            ReleaseSRWLockExclusive(&adapter.tables_lock);return result;
        });
    }

    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D:{
        if(number || size!=sizeof(Queue))return E_INVALIDARG;
        Queue wrapped{};
#define WRAP_QUEUE(member) wrapped.member=&EntryThunk<decltype(wrapped.member),QueueBinding<&Queue::member,QueueName_##member>,EntryPolicy>::call;
        WRAP_QUEUE(pfnExecuteCommandLists)
        WRAP_QUEUE(pfnUpdateTileMappings)
        WRAP_QUEUE(pfnCopyTileMappings)
        WRAP_QUEUE(pfnSignalFence)
        WRAP_QUEUE(pfnWaitForFence)
#undef WRAP_QUEUE
        if(engine_ddi::entry_stats_on())time_queue(wrapped);
        *static_cast<Queue*>(output)=wrapped;return S_OK;
    }
    // The render pass table (H:2507), two engine-ddi slots at 0053. It is filled only while GetCaps 1006 reports a
    // render pass tier, the engine's own answer: with no tier the runtime emulates render passes and never asks
    // for the table, so the cap and the table cannot disagree. The slots are engine-ddi's own, as the direct
    // entries of the graphics table are, so no entry thunk wraps them. Render pass is also an extended feature
    // (D3D12DDI_FEATURE_0043_RENDER_PASS), which extended_features (native-queue-ddi.cpp) does not advertise yet;
    // SLOTS.md carries that open step.
    case D3D12DDI_TABLE_TYPE_0043_RENDER_PASS:{
        using Pass=D3D12DDI_RENDER_PASS_FUNCS_0053;
        if(number || size!=sizeof(Pass))return E_INVALIDARG;
        bool supported=false;
        HRESULT hr=adapter_render_pass_supported(adapter,&supported);if(hr!=S_OK)return hr;
        if(!supported)return E_NOTIMPL;
        Pass table{};
        hr=engine_ddi::fill_render_pass(&table,sizeof(table),&fill);if(hr!=S_OK)return hr;
        *static_cast<Pass*>(output)=table;return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES:{
        if(number || size!=sizeof(Extended))return E_INVALIDARG;
        Extended wrapped{};
#define WRAP_EXTENDED(member) wrapped.member=&EntryThunk<decltype(wrapped.member),ExtendedBinding<&Extended::member,ExtendedName_##member>,EntryPolicy>::call;
        WRAP_EXTENDED(pfnGetSupportedExtendedFeatures)
        WRAP_EXTENDED(pfnGetSupportedExtendedFeatureVersions)
        WRAP_EXTENDED(pfnEnableExtendedFeature)
        WRAP_EXTENDED(pfnSetExtendedFeatureCallbacks)
#undef WRAP_EXTENDED
        *static_cast<Extended*>(output)=wrapped;return S_OK;
    }
#if defined(D3D12DDI_BUILD_VERSION_0119)
    case D3D12DDI_TABLE_TYPE_0096_EXTENDED_FEATURES:{
        if(number || size!=sizeof(Extended96))return E_INVALIDARG;
        if((adapter.ddi_version.load(std::memory_order_acquire)>>16)<D3D12DDI_BUILD_VERSION_0096)return E_NOTIMPL;
        Extended96 wrapped{};
        // Type 27 has its own typed source. The last three signatures and
        // originals match 0021, including unknown-feature and device checks.
#define WRAP_EXTENDED96(member) wrapped.member=&EntryThunk<decltype(wrapped.member),Extended96Binding<&Extended96::member,ExtendedName_##member>,EntryPolicy>::call;
        WRAP_EXTENDED96(pfnGetSupportedExtendedFeatures)
        WRAP_EXTENDED96(pfnGetSupportedExtendedFeatureVersions)
        WRAP_EXTENDED96(pfnEnableExtendedFeature)
        WRAP_EXTENDED96(pfnSetExtendedFeatureCallbacks)
#undef WRAP_EXTENDED96
        *static_cast<Extended96*>(output)=wrapped;return S_OK;
    }
#endif
    default:return E_NOTIMPL;
    }
}
}
