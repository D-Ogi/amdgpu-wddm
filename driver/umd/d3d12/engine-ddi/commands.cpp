// SPDX-License-Identifier: MIT
// engine-ddi: command pools (D29-D32), command recorders (D92-D95), command lists (D36-D38) and the list slots
// that open and close recording (L0, L1); the acceleration structure slots (D108, L60-L62), the ray dispatch
// slots (L63, L64) and the mesh dispatch slot (L67).
//
// DDI 0040 splits the API's allocator and list: a pool is the API allocator, a recorder names the pool a list
// records into, and ResetCommandList names the recorder. A pool therefore does not know its list type at
// creation; it creates one engine allocator per list type on first use.
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <type_traits>
#include "internal.h"
#include "replay.h"

namespace engine_ddi {

static_assert(D3D12DDI_COMMAND_QUEUE_FLAG_3D == 0x1 && D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE == 0x2 &&
              D3D12DDI_COMMAND_QUEUE_FLAG_COPY == 0x4, "queue flags");

namespace {
// Engine list type of a DDI list: bundles by type, the rest by the queue flags. -1 when unsupported.
// The flags are a mask, read as the queue's create does (queue-engine.cpp): 3D before COMPUTE before COPY.
int engine_list_type(D3D12DDI_COMMAND_LIST_TYPE type, UINT queue_flags) noexcept {
    if (type == D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) return D3D12_COMMAND_LIST_TYPE_BUNDLE;
    if (type != D3D12DDI_COMMAND_LIST_TYPE_DIRECT) return -1;
    constexpr UINT known = D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE |
                           D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    if (!queue_flags || (queue_flags & ~known)) return -1;
    if (queue_flags & D3D12DDI_COMMAND_QUEUE_FLAG_3D) return D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (queue_flags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE) return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    return D3D12_COMMAND_LIST_TYPE_COPY;
}

// ---- Pools -----------------------------------------------------------------------------------------------------
SIZE_T APIENTRY calc_pool(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_POOL_0040*) {
    return sizeof(CommandPoolRecord);
}

HRESULT APIENTRY create_pool(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_COMMAND_POOL_0040* args,
                             D3D12DDI_HCOMMANDPOOL_0040 h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate) return E_INVALIDARG;
    if (args->PoolFlags != D3D12DDI_COMMAND_POOL_FLAG_NONE) return E_INVALIDARG;
    new (h.pDrvPrivate) CommandPoolRecord{{Tag::CommandPool, 0, nullptr, c}, {}};
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_pool(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDPOOL_0040 h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* p = record_of<CommandPoolRecord>(h.pDrvPrivate, Tag::CommandPool, c);
    if (!p) {
        c->report(E_INVALIDARG);
        return;
    }
    drain_all(c, Drain::Pool);                  // pending calls of lists recording into these allocators
    for (ID3D12CommandAllocator*& a : p->allocators) {
        if (a) a->Release();
        a = nullptr;
    }
    poison(p->h);
    c->live.fetch_sub(1);
}

void APIENTRY reset_pool(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDPOOL_0040 h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* p = record_of<CommandPoolRecord>(h.pDrvPrivate, Tag::CommandPool, c);
    if (!p) {
        c->report(E_INVALIDARG);
        return;
    }
    // By API no list recording into the pool's allocators is open here, so the pool's pending calls end with its
    // lists' Close entries: the reset waits for the last of them on each ring, and the replay of other pools' lists
    // goes on. A list still open into this pool (an application error) keeps the drain of every ring, as before.
    if (p->open_lists.load(std::memory_order_acquire)) drain_all(c, Drain::Pool);
    else drain_closes(c, p->closing);
    for (ID3D12CommandAllocator* a : p->allocators) {
        if (!a) continue;
        HRESULT hr = a->Reset();
        if (FAILED(hr)) c->report(hr);
    }
}

// ---- Recorders -------------------------------------------------------------------------------------------------
SIZE_T APIENTRY calc_recorder(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040*) {
    return sizeof(CommandRecorderRecord);
}

HRESULT APIENTRY create_recorder(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040* args,
                                 D3D12DDI_HCOMMANDRECORDER_0040 h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate) return E_INVALIDARG;
    if (args->RecorderFlags != D3D12DDI_COMMAND_RECORDER_FLAG_NONE) return E_INVALIDARG;
    if (engine_list_type(D3D12DDI_COMMAND_LIST_TYPE_DIRECT, args->QueueFlags) < 0) {
        log_line("command recorder refused: queue flags 0x%x", static_cast<unsigned>(args->QueueFlags));
        return E_NOTIMPL;
    }
    new (h.pDrvPrivate) CommandRecorderRecord{{Tag::CommandRecorder, 0, nullptr, c}, nullptr, static_cast<UINT>(args->QueueFlags)};
    return S_OK;
}

void APIENTRY destroy_recorder(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDRECORDER_0040 h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<CommandRecorderRecord>(h.pDrvPrivate, Tag::CommandRecorder, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    poison(r->h);
}

void APIENTRY set_pool_as_target(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDRECORDER_0040 hr,
                                 D3D12DDI_HCOMMANDPOOL_0040 hp) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<CommandRecorderRecord>(hr.pDrvPrivate, Tag::CommandRecorder, c);
    auto* p = record_of<CommandPoolRecord>(hp.pDrvPrivate, Tag::CommandPool, c);
    if (!r || (hp.pDrvPrivate && !p)) {
        c->report(E_INVALIDARG);
        return;
    }
    r->pool = p;
}

// ---- Lists -----------------------------------------------------------------------------------------------------
// A recording list is counted in the pool it was reset into (reset_pool reads the count); its Close, a new Reset and
// its destroy take it out again. A pool destroyed meanwhile is poisoned and not counted down.
void leave_pool(CommandListRecord* l) noexcept {
    if (auto* p = record_of<CommandPoolRecord>(l->pool, Tag::CommandPool, l->h.device))
        p->open_lists.fetch_sub(1, std::memory_order_release);
    l->pool = nullptr;
}

SIZE_T APIENTRY calc_list(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_LIST_0040*) {
    return sizeof(CommandListRecord);
}

HRESULT APIENTRY create_list(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_COMMAND_LIST_0040* args,
                             D3D12DDI_HCOMMANDLIST h, D3D12DDI_HRTCOMMANDLIST rt) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate) return E_INVALIDARG;
    const int type = engine_list_type(args->Type, args->QueueFlags);
    if (type < 0) {
        log_line("command list refused: type %u, queue flags 0x%x", static_cast<unsigned>(args->Type),
                 static_cast<unsigned>(args->QueueFlags));
        return E_NOTIMPL;
    }
    if (args->NodeMask > 1) return E_INVALIDARG;
    // The engine list starts closed; ResetCommandList opens it on the recorder's pool.
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT hr = c->device4->CreateCommandList1(0, static_cast<D3D12_COMMAND_LIST_TYPE>(type),
                                                D3D12_COMMAND_LIST_FLAG_NONE, __uuidof(ID3D12GraphicsCommandList),
                                                reinterpret_cast<void**>(&list));
    if (FAILED(hr)) return hr;
    const uint32_t table =
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_BUNDLE) ? 1u : 0u;
    auto* r = new (h.pDrvPrivate)
        CommandListRecord{{Tag::CommandList, 0, list, c}, rt, static_cast<D3D12_COMMAND_LIST_TYPE>(type), table,
                          false};
    // The hook runs with no engine-ddi lock held (engine-ddi.h, hooks).
    hr = c->hooks.bind_list_table(c->hooks.shell, rt, table);
    if (FAILED(hr)) {
        release_engine(r->h);
        poison(r->h);
        return hr;
    }
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_list(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDLIST h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* l = record_of<CommandListRecord>(h.pDrvPrivate, Tag::CommandList, c);
    if (!l) {
        c->report(E_INVALIDARG);
        return;
    }
    drain_list(l, Drain::Destroy);
    if (l->pool) leave_pool(l);
    // BD-133: a destroyed list carries no Present copy. One that still carries an unexecuted ticket removes the
    // device here and keeps its copy surface (present-copy-sync.h, rule D): its present waits for that copy.
    l->present_copy.release();
    release_engine(l->h);
    poison(l->h);
    c->live.fetch_sub(1);
}

// With replay on, Close is the list's last entry and the worker makes the engine Close: the calling thread no longer
// waits for the list's calls (306: 0.41 ms per frame of the main thread in that drain, 0.09 in the engine Close). The
// list counts as closed at once. An engine Close that fails there leaves its result in close_hr, and the list's next
// drain (ExecuteCommandLists, Reset, ExecuteBundle) reports it on that API thread: the runtime learns it there, not
// from the Close. The engine Close reaches the list's allocator, so the pool keeps where the entry ends (closing) for
// its reset and for a Reset into it. Made here (policy off, a thread without a ring), the Close reports its own
// failure, and a list whose Close failed stays what it was.
void APIENTRY close_list(D3D12DDI_HCOMMANDLIST h) {
    CommandListRecord* l = list_of(h, "CloseCommandList");
    if (!l) return;
    const HRESULT earlier = l->close_hr.load(std::memory_order_relaxed);
    if (FAILED(earlier)) return l->h.device->report_list(l->rt, earlier);
    record(l, [hr = &l->close_hr](ID3D12GraphicsCommandList* e) noexcept {
        const HRESULT result = e->Close();
        if (FAILED(result)) hr->store(result, std::memory_order_relaxed);
    });
    // With the policy off, a tail left from before it was turned off names nothing (the teardown replayed it).
    Replay* replay = l->h.device->replay;
    const uint64_t tail = replay ? l->replay_tail.load(std::memory_order_relaxed) : 0;
    const HRESULT hr = l->close_hr.load(std::memory_order_relaxed);
    if (FAILED(hr)) {
        l->h.device->report_list(l->rt, hr);
        if (!tail) return;
    }
    l->recording = false;
    if (tail) {
        // The list is complete: its worker need not wait for a later publish to replay it.
        wake_for(replay, tail);
        if (auto* p = l->pool ? record_of<CommandPoolRecord>(l->pool, Tag::CommandPool, l->h.device) : nullptr)
            p->closing[(tail >> 56) - 1].store(tail & kReplayPositionMask, std::memory_order_relaxed);
    }
    if (l->pool) leave_pool(l);
}

void APIENTRY reset_list(D3D12DDI_HCOMMANDLIST h, const D3D12DDIARG_RESETCOMMANDLIST_0040* args) {
    CommandListRecord* l = list_of(h, "ResetCommandList");
    if (!l) return;
    const HRESULT closed = drain_list(l, Drain::Reset);
    // D3D12 Close failures permanently invalidate that list (CPUEfficiency, Runtime Validation for Reset).
    // A successful Close still permits the normal Reset below; this is not a device-wide latch.
    if (FAILED(closed)) return l->h.device->report_list(l->rt, closed);
    DeviceContext* c = l->h.device;
    auto* r = args ? record_of<CommandRecorderRecord>(args->hDrvCommandRecorder.pDrvPrivate, Tag::CommandRecorder, c)
                   : nullptr;
    // The pool may have been destroyed since it became the target: its record is poisoned then.
    CommandPoolRecord* p = r ? record_of<CommandPoolRecord>(r->pool, Tag::CommandPool, c) : nullptr;
    if (!p) {
        c->report_list(l->rt, E_INVALIDARG);
        return;
    }
    drain_closes(c, p->closing);                // another list's pending Close still reaches the allocator
    ID3D12CommandAllocator*& a = p->allocators[l->type];
    if (!a) {
        HRESULT hr = c->device->CreateCommandAllocator(l->type, __uuidof(ID3D12CommandAllocator),
                                                       reinterpret_cast<void**>(&a));
        if (FAILED(hr)) {
            a = nullptr;
            c->report_list(l->rt, hr);
            return;
        }
    }
    HRESULT hr = l->list()->Reset(a, nullptr);
    if (FAILED(hr)) {
        c->report_list(l->rt, hr);
        return;
    }
    if (l->pool) leave_pool(l);                 // a Reset without a Close: the list leaves its earlier pool
    l->root_signatures[0] = l->root_signatures[1] = nullptr;   // the engine's Reset unbound them
    // BD-133: the engine's Reset dropped the list's commands, a Present copy among them, so the list carries no ticket
    // any more. A ticket that was never executed removes the device here (present-copy-sync.h, rule D). A Reset that
    // failed above keeps the ticket: the old commands are still there.
    l->present_copy.release();
    l->recording = true;
    l->pool = p;
    p->open_lists.fetch_add(1, std::memory_order_release);
}

// ---- List state the runtime sets on every list ---------------------------------------------------------------------
// The runtime writes default state into a list it has reset. SetPredication was seen there; the others are
// answered ahead of need. Each forwards to the engine list, which owns the feature check. Forwarding is
// not support: the caps of these features stay as they are. list_as (internal.h) is the interface query they and
// the slots further down share.

void APIENTRY set_predication(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HRESOURCE hres, UINT64 offset,
                              D3D12DDI_PREDICATION_OP op) {
    CommandListRecord* l = list_of(h, "SetPredication");
    if (!l) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, l->h.device);
    if (hres.pDrvPrivate && !r) return l->h.device->report_list(l->rt, E_INVALIDARG);
    ID3D12Resource* resource = r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr;
    const UINT64 at = r ? offset : 0;
    record(l, [=](ID3D12GraphicsCommandList* e) {
        e->SetPredication(resource, at, static_cast<D3D12_PREDICATION_OP>(op));
    });
}

// The interface is queried on the calling thread; a deferred call releases it on the worker (replay.h, R2).
void APIENTRY om_set_depth_bounds(D3D12DDI_HCOMMANDLIST h, FLOAT low, FLOAT high) {
    CommandListRecord* l = list_of(h, "OMSetDepthBounds");
    if (!l) return;
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l1->OMSetDepthBounds(low, high);
            l1->Release();
        });
}

// NumSamplesPerPixel x NumPixels positions (the API's array size).
void APIENTRY set_sample_positions(D3D12DDI_HCOMMANDLIST h, UINT per_pixel, UINT pixels,
                                   D3D12DDI_SAMPLE_POSITION* positions) {
    static_assert(sizeof(D3D12DDI_SAMPLE_POSITION) == sizeof(D3D12_SAMPLE_POSITION), "sample position");
    CommandListRecord* l = list_of(h, "SetSamplePositions");
    if (!l) return;
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const D3D12_SAMPLE_POSITION* p) {
                   l1->SetSamplePositions(per_pixel, pixels, const_cast<D3D12_SAMPLE_POSITION*>(p));
                   l1->Release();
               },
               in(reinterpret_cast<const D3D12_SAMPLE_POSITION*>(positions),
                  positions ? size_t{per_pixel} * pixels : 0));
}

void APIENTRY set_view_instance_mask(D3D12DDI_HCOMMANDLIST h, UINT mask) {
    CommandListRecord* l = list_of(h, "SetViewInstanceMask");
    if (!l) return;
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l1->SetViewInstanceMask(mask);
            l1->Release();
        });
}

// ---- L56 pfnWriteBufferImmediate --------------------------------------------------------------------------------
// Count writes of one UINT32 each to a GPU virtual address, on the list's own timeline, with an optional mode per
// write (H:6177-6182). The DDI parameter and the API parameter have one layout, and the three modes one set of
// values, so both arrays pass as they are; the engine reads both at the call (vkd3d-proton command.c,
// d3d12_command_list_WriteBufferImmediate), so a deferred entry carries copies of both, as the sample positions
// array does. The slot is in both command list tables: the engine admits the write on every queue type it has
// (device.c, d3d12_device_caps_init_feature_options3), and 1006 WriteBufferImmediateQueueFlags reports those
// queues (caps.cpp).
static_assert(sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032) ==
                      sizeof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER) &&
                  offsetof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032, Dst) ==
                      offsetof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, Dest) &&
                  offsetof(D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032, Value) ==
                      offsetof(D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, Value),
              "write buffer immediate parameter");
static_assert(sizeof(D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032) == sizeof(D3D12_WRITEBUFFERIMMEDIATE_MODE) &&
                  D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_DEFAULT ==
                      static_cast<int>(D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT) &&
                  D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN ==
                      static_cast<int>(D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN) &&
                  D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT ==
                      static_cast<int>(D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT),
              "write buffer immediate modes");

void APIENTRY write_buffer_immediate(D3D12DDI_HCOMMANDLIST h, UINT count,
                                     const D3D12DDI_WRITEBUFFERIMMEDIATE_PARAMETER_0032* params,
                                     const D3D12DDI_WRITEBUFFERIMMEDIATE_MODE_0032* modes) {
    CommandListRecord* l = list_of(h, "WriteBufferImmediate");
    if (!l) return;
    if (count && !params) return l->h.device->report_list(l->rt, E_INVALIDARG);
    if (auto* l2 = list_as<ID3D12GraphicsCommandList2>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p,
                   const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) {
                   l2->WriteBufferImmediate(count, p, m);
                   l2->Release();
               },
               in(reinterpret_cast<const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER*>(params), params ? count : 0),
               in(reinterpret_cast<const D3D12_WRITEBUFFERIMMEDIATE_MODE*>(modes), modes ? count : 0));
}

// ---- L15 pfnExecuteBundle ---------------------------------------------------------------------------------------
// The engine replays the bundle's commands into the parent at this call, so the bundle must be closed and the
// parent recording. Both DIRECT lists and bundles are bound to the graphics table: the types are checked here,
// a bundle executed from a bundle would otherwise be dropped by the engine without a word.
void APIENTRY execute_bundle(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HCOMMANDLIST hbundle) {
    CommandListRecord* l = list_of(h, "ExecuteBundle");
    if (!l) return;
    auto* b = record_of<CommandListRecord>(hbundle.pDrvPrivate, Tag::CommandList, l->h.device);
    if (l->type != D3D12_COMMAND_LIST_TYPE_DIRECT || !b || b->type != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
        log_line("ExecuteBundle: refused (parent type %d recording %d, bundle %s)", static_cast<int>(l->type),
                 l->recording ? 1 : 0, !b ? "unknown" : b->recording ? "recording" : "not a bundle");
        return l->h.device->report_list(l->rt, E_INVALIDARG);
    }
    // With replay on, both lists' pending calls run first, and the engine call is made here.
    const HRESULT bundle_closed = drain_list(b, Drain::Bundle);
    const HRESULT parent_closed = drain_list(l, Drain::Bundle);
    if (FAILED(parent_closed)) return l->h.device->report_list(l->rt, parent_closed);
    if (FAILED(bundle_closed)) return l->h.device->report_list(l->rt, bundle_closed);
    // An inline failed Close leaves recording=true. Report that first failure above, before state validation
    // could replace it with E_INVALIDARG. A healthy bundle must still be closed and its parent recording.
    if (!l->recording || b->recording) return l->h.device->report_list(l->rt, E_INVALIDARG);
    l->list()->ExecuteBundle(b->list());
}

// ---- L49 pfnSetMarker -------------------------------------------------------------------------------------------
// Accepted and dropped: a compatibility answer, not marker support. What the UINT64 means to a consumer of
// markers is not established here, and nothing reaches the engine.
void APIENTRY set_marker(D3D12DDI_HCOMMANDLIST h, UINT64) { (void)list_of(h, "SetMarker"); }

// BD-046. The runtime has no ClearState DDI: for CreateCommandList, Reset and the API's ClearState it calls the
// individual state DDIs, and this one "to clear root arguments to 0" (DirectX-Specs d3d/CPUEfficiency.md). On a
// just-reset list nothing is bound and nothing is done. Mid-list (ClearState) the signatures stay bound and every
// argument of each bound signature is set to zero through the engine's own calls, in parameter order: constants to
// 0, root descriptors to VA 0 (a null descriptor in the engine), tables to handle 0. Otherwise the arguments set
// before the ClearState stay, and a root descriptor keeps naming a buffer the application may destroy after it.
void APIENTRY clear_root_arguments(D3D12DDI_HCOMMANDLIST h) {
    using List = ID3D12GraphicsCommandList;
    CommandListRecord* l = list_of(h, "ClearRootArguments");
    if (!l) return;
    static constexpr UINT kZeros[D3D12_MAX_ROOT_COST] = {};
    for (int graphics = 0; graphics < 2; ++graphics) {
        const auto* r = record_of<RootSignatureRecord>(l->root_signatures[graphics], Tag::RootSignature, l->h.device);
        if (!r) continue;
        for (UINT i = 0; i < r->shapes; ++i) {
            const RootParameterShape p = r->shape()[i];
            switch (p.type) {
            case D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
                const UINT n = p.constants < D3D12_MAX_ROOT_COST ? p.constants : D3D12_MAX_ROOT_COST;
                if (graphics)
                    record(l, [=](List* e, const UINT* d) { e->SetGraphicsRoot32BitConstants(i, n, d, 0); }, in(kZeros, n));
                else
                    record(l, [=](List* e, const UINT* d) { e->SetComputeRoot32BitConstants(i, n, d, 0); }, in(kZeros, n));
                break;
            }
            case D3D12DDI_ROOT_PARAMETER_TYPE_CBV:
                if (graphics) record(l, [=](List* e) { e->SetGraphicsRootConstantBufferView(i, 0); });
                else record(l, [=](List* e) { e->SetComputeRootConstantBufferView(i, 0); });
                break;
            case D3D12DDI_ROOT_PARAMETER_TYPE_SRV:
                if (graphics) record(l, [=](List* e) { e->SetGraphicsRootShaderResourceView(i, 0); });
                else record(l, [=](List* e) { e->SetComputeRootShaderResourceView(i, 0); });
                break;
            case D3D12DDI_ROOT_PARAMETER_TYPE_UAV:
                if (graphics) record(l, [=](List* e) { e->SetGraphicsRootUnorderedAccessView(i, 0); });
                else record(l, [=](List* e) { e->SetComputeRootUnorderedAccessView(i, 0); });
                break;
            case D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                if (graphics) record(l, [=](List* e) { e->SetGraphicsRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{0}); });
                else record(l, [=](List* e) { e->SetComputeRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{0}); });
                break;
            default:
                break;
            }
        }
    }
}

// DDI 0092 names this slot, but the alpha factor travels as the fourth component of OMSetBlendFactor
// (DirectX-Specs, VulkanOn12: the separate entry is unused). Nothing is forwarded from here.
void APIENTRY om_set_alpha_blend_factor(D3D12DDI_HCOMMANDLIST h, FLOAT) { (void)list_of(h, "OmSetAlphaBlendFactor"); }

// No protected sessions exist here (none can be created), so only "none" is a valid session.
void APIENTRY set_protected_session(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
    CommandListRecord* l = list_of(h, "SetProtectedResourceSession");
    if (l && session.pDrvPrivate) l->h.device->report_list(l->rt, E_NOTIMPL);
}

void APIENTRY rs_set_shading_rate(D3D12DDI_HCOMMANDLIST h, D3D12DDI_SHADING_RATE_0062 rate,
                                  const D3D12DDI_SHADING_RATE_COMBINER_0062* combiners) {
    static_assert(sizeof(D3D12DDI_SHADING_RATE_COMBINER_0062) == sizeof(D3D12_SHADING_RATE_COMBINER), "combiner");
    CommandListRecord* l = list_of(h, "RSSetShadingRate");
    if (!l) return;
    if (auto* l5 = list_as<ID3D12GraphicsCommandList5>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const D3D12_SHADING_RATE_COMBINER* k) {
                   l5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(rate), k);
                   l5->Release();
               },
               in(reinterpret_cast<const D3D12_SHADING_RATE_COMBINER*>(combiners),
                  combiners ? D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT : 0));
}

void APIENTRY rs_set_shading_rate_image(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HRESOURCE hres) {
    CommandListRecord* l = list_of(h, "RSSetShadingRateImage");
    if (!l) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, l->h.device);
    if (hres.pDrvPrivate && !r) return l->h.device->report_list(l->rt, E_INVALIDARG);
    ID3D12Resource* image = r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr;
    if (auto* l5 = list_as<ID3D12GraphicsCommandList5>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l5->RSSetShadingRateImage(image);
            l5->Release();
        });
}
} // namespace

// ---- Command signatures and indirect execution ---------------------------------------------------------------------
static_assert(D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW == static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED == static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH == static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW ==
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW ==
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT == static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW ==
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW ==
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW ==
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS == static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH == static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) &&
              D3D12DDI_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT ==
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT),
              "indirect argument types");
// The argument buffer of a DISPATCH_RAYS signature holds the application's D3D12_DISPATCH_RAYS_DESC records (the
// API layout, which D3D12DDIARG_DISPATCH_RAYS_0054 matches, asserted at DispatchRays below); the engine reads them
// as they are.

SIZE_T APIENTRY calc_command_signature(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*) {
    return sizeof(CommandSignatureRecord);
}

// The engine refuses a root signature that no argument needs as well as a missing one, and the DDI does not say
// whether the runtime sends the handle for a signature of draws alone: it is passed on only when an argument
// changes root arguments.
// The runtime removes the device when this DDI fails with E_NOTIMPL (measured on the lab: a DISPATCH_RAYS argument
// answered E_NOTIMPL, CreateCommandSignature returned DXGI_ERROR_DEVICE_REMOVED and the removed reason was
// DXGI_ERROR_DRIVER_INTERNAL_ERROR). Every argument type of the DDI is therefore translated, mesh dispatch and the
// incrementing constant (ExecuteIndirectTier 1_1, which caps.cpp does not answer) included, and the engine decides.
// A type this shell does not know and a signature the engine refuses are answered with E_OUTOFMEMORY, the one
// failure a D3D10/11 create function may report without it being critical (windows-driver-docs display
// handling-errors.md); that the D3D12 runtime passes it on without removing the device is an INFERENCE, not
// measured. Both cases leave a log line.
HRESULT APIENTRY create_command_signature(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001* args,
                                          D3D12DDI_HCOMMANDSIGNATURE h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || !args->NumArgumentDescs || !args->pArgumentDescs || !args->ByteStride ||
        args->NodeMask > 1)
        return E_INVALIDARG;
    std::unique_ptr<D3D12_INDIRECT_ARGUMENT_DESC[]> out(new (std::nothrow)
                                                            D3D12_INDIRECT_ARGUMENT_DESC[args->NumArgumentDescs]{});
    if (!out) return E_OUTOFMEMORY;
    bool rooted = false;
    for (UINT i = 0; i < args->NumArgumentDescs; ++i) {
        const D3D12DDI_INDIRECT_ARGUMENT_DESC& in = args->pArgumentDescs[i];
        out[i].Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(in.Type);
        switch (in.Type) {
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW:
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH:
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
            break;
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
            out[i].VertexBuffer.Slot = in.VertexBuffer.Slot;
            break;
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            out[i].Constant = {in.Constant.RootParameterIndex, in.Constant.DestOffsetIn32BitValues,
                               in.Constant.Num32BitValuesToSet};
            rooted = true;
            break;
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            out[i].ConstantBufferView.RootParameterIndex = in.ConstantBufferView.RootParameterIndex;
            rooted = true;
            break;
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            out[i].ShaderResourceView.RootParameterIndex = in.ShaderResourceView.RootParameterIndex;
            rooted = true;
            break;
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
            out[i].UnorderedAccessView.RootParameterIndex = in.UnorderedAccessView.RootParameterIndex;
            rooted = true;
            break;
        case D3D12DDI_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT:
            out[i].IncrementingConstant = {in.IncrementingConstant.RootParameterIndex,
                                           in.IncrementingConstant.DestOffsetIn32BitValues};
            rooted = true;
            break;
        default:
            log_line("command signature: argument %u has the unknown type %d: E_OUTOFMEMORY", i,
                     static_cast<int>(in.Type));
            return E_OUTOFMEMORY;
        }
    }
    ID3D12RootSignature* root = nullptr;
    if (rooted) {
        auto* r = record_of<RootSignatureRecord>(args->hRootSignature.pDrvPrivate, Tag::RootSignature, c);
        if (!r) return E_INVALIDARG;
        root = static_cast<ID3D12RootSignature*>(r->h.engine);
    }
    const D3D12_COMMAND_SIGNATURE_DESC desc{args->ByteStride, args->NumArgumentDescs, out.get(), 0};
    ID3D12CommandSignature* signature = nullptr;
    const HRESULT hr = c->device->CreateCommandSignature(&desc, root, __uuidof(ID3D12CommandSignature),
                                                         reinterpret_cast<void**>(&signature));
    if (FAILED(hr) || !signature) {
        if (hr != E_OUTOFMEMORY)
            log_line("command signature: the engine refused %u arguments, stride %u (hr %08lx): E_OUTOFMEMORY",
                     args->NumArgumentDescs, args->ByteStride, static_cast<unsigned long>(hr));
        return E_OUTOFMEMORY;
    }
    new (h.pDrvPrivate) CommandSignatureRecord{{Tag::CommandSignature, 0, signature, c}, args->ByteStride};
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_command_signature(D3D12DDI_HDEVICE device, D3D12DDI_HCOMMANDSIGNATURE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<CommandSignatureRecord>(h.pDrvPrivate, Tag::CommandSignature, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    drain_all(c, Drain::Destroy);
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}

// A count buffer is optional: no handle means the maximum count is the count.
void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HCOMMANDSIGNATURE h, UINT max_count,
                               D3D12DDIARG_BUFFER_PLACEMENT arguments, D3D12DDIARG_BUFFER_PLACEMENT count) {
    CommandListRecord* l = list_of(hlist, "ExecuteIndirect");
    if (!l) return;
    DeviceContext* c = l->h.device;
    auto* s = record_of<CommandSignatureRecord>(h.pDrvPrivate, Tag::CommandSignature, c);
    auto* a = record_of<ResourceRecord>(arguments.BaseAddress.UMD.hResource.pDrvPrivate, Tag::Resource, c);
    void* count_handle =count.BaseAddress.UMD.hResource.pDrvPrivate;
    auto* n = count_handle ? record_of<ResourceRecord>(count_handle, Tag::Resource, c) : nullptr;
    if (!s || !a || (count_handle && !n)) return c->report_list(l->rt, E_INVALIDARG);
    auto* signature = static_cast<ID3D12CommandSignature*>(s->h.engine);
    auto* argument_buffer = static_cast<ID3D12Resource*>(a->h.engine);
    const UINT64 argument_offset = arguments.BaseAddress.UMD.Offset;
    ID3D12Resource* count_buffer = n ? static_cast<ID3D12Resource*>(n->h.engine) : nullptr;
    const UINT64 count_offset = n ? count.BaseAddress.UMD.Offset : 0;
    record(l, [=](ID3D12GraphicsCommandList* e) {
        e->ExecuteIndirect(signature, max_count, argument_buffer, argument_offset, count_buffer, count_offset);
    });
}

// ---- Resolves (graphics table) -------------------------------------------------------------------------------------
void APIENTRY resolve_subresource(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HRESOURCE dst, UINT dst_subresource,
                                  D3D12DDI_HRESOURCE src, UINT src_subresource, DXGI_FORMAT format) {
    CommandListRecord* l = list_of(hlist, "ResourceResolveSubresource");
    if (!l || reject_in_compute_table(l)) return;
    DeviceContext* c = l->h.device;
    auto* d = record_of<ResourceRecord>(dst.pDrvPrivate, Tag::Resource, c);
    auto* s = record_of<ResourceRecord>(src.pDrvPrivate, Tag::Resource, c);
    if (!d || !s) return c->report_list(l->rt, E_INVALIDARG);
    auto* dst_resource = static_cast<ID3D12Resource*>(d->h.engine);
    auto* src_resource = static_cast<ID3D12Resource*>(s->h.engine);
    record(l, [=](ID3D12GraphicsCommandList* e) {
        e->ResolveSubresource(dst_resource, dst_subresource, src_resource, src_subresource, format);
    });
}

void APIENTRY resolve_subresource_region(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HRESOURCE dst, UINT dst_subresource,
                                         UINT x, UINT y, D3D12DDI_HRESOURCE src, UINT src_subresource,
                                         D3D12DDI_RECT* rect, DXGI_FORMAT format, D3D12DDI_RESOLVE_MODE mode) {
    static_assert(D3D12DDI_RESOLVE_MODE_DECOMPRESS == static_cast<int>(D3D12_RESOLVE_MODE_DECOMPRESS) &&
                  D3D12DDI_RESOLVE_MODE_MIN == static_cast<int>(D3D12_RESOLVE_MODE_MIN) &&
                  D3D12DDI_RESOLVE_MODE_MAX == static_cast<int>(D3D12_RESOLVE_MODE_MAX) &&
                  D3D12DDI_RESOLVE_MODE_AVERAGE == static_cast<int>(D3D12_RESOLVE_MODE_AVERAGE),
                  "resolve modes");
    static_assert(sizeof(D3D12DDI_RECT) == sizeof(D3D12_RECT), "rectangle");
    CommandListRecord* l = list_of(hlist, "ResourceResolveSubresourceRegion");
    if (!l || reject_in_compute_table(l)) return;
    DeviceContext* c = l->h.device;
    if (mode > D3D12DDI_RESOLVE_MODE_AVERAGE) return c->report_list(l->rt, E_NOTIMPL);     // sampler feedback
    auto* d = record_of<ResourceRecord>(dst.pDrvPrivate, Tag::Resource, c);
    auto* s = record_of<ResourceRecord>(src.pDrvPrivate, Tag::Resource, c);
    if (!d || !s) return c->report_list(l->rt, E_INVALIDARG);
    auto* dst_resource = static_cast<ID3D12Resource*>(d->h.engine);
    auto* src_resource = static_cast<ID3D12Resource*>(s->h.engine);
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const D3D12DDI_RECT* r) {
                   l1->ResolveSubresourceRegion(dst_resource, dst_subresource, x, y, src_resource, src_subresource,
                                                const_cast<D3D12_RECT*>(r), format, static_cast<D3D12_RESOLVE_MODE>(mode));
                   l1->Release();
               },
               in(rect, rect ? 1 : 0));
}

// ---- Acceleration structures (D108, L60-L62) and ray dispatch (L63, L64) ------------------------------------------
// The engine's ID3D12Device5 and ID3D12GraphicsCommandList4 methods. The DDI 0054 structures below have the API's
// members at the API's offsets (H = d3d12umddi.h, A = d3d12.h, 10.0.26100), so the inputs, the geometry arrays they
// point to, the prebuild answer and the postbuild descriptions reach the engine in place. The build, emit and copy
// arguments pack what the API takes as parameters and are unpacked. Instance descriptions are GPU memory, which the
// engine reads in the API's layout; the DDI's is checked against it too.
#define ENGINE_DDI_SAME(D, dm, A, am) static_assert(offsetof(D, dm) == offsetof(A, am), #D "::" #dm)
// A pointer cast also needs the API's alignment, which equal sizes and offsets do not imply.
#define ENGINE_DDI_ALIGN(D, A) static_assert(alignof(D) == alignof(A), #D " alignment")
static_assert(std::is_same_v<D3D12DDI_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS>, "GPU VA");  // H:92, A:1397
// H:7958-7962, A:14479-14483
static_assert(sizeof(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE), "VA+stride");
ENGINE_DDI_ALIGN(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE);
ENGINE_DDI_SAME(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StartAddress, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StartAddress);
ENGINE_DDI_SAME(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StrideInBytes, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StrideInBytes);
// H:7977-7986, A:14498-14507
using DdiTriangles = D3D12DDI_RAYTRACING_GEOMETRY_TRIANGLES_DESC_0054;
using ApiTriangles = D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC;
static_assert(sizeof(DdiTriangles) == sizeof(ApiTriangles), "triangles");
ENGINE_DDI_ALIGN(DdiTriangles, ApiTriangles);
ENGINE_DDI_SAME(DdiTriangles, ColumnMajorTransform3x4, ApiTriangles, Transform3x4);
ENGINE_DDI_SAME(DdiTriangles, IndexFormat, ApiTriangles, IndexFormat);
ENGINE_DDI_SAME(DdiTriangles, VertexFormat, ApiTriangles, VertexFormat);
ENGINE_DDI_SAME(DdiTriangles, IndexCount, ApiTriangles, IndexCount);
ENGINE_DDI_SAME(DdiTriangles, VertexCount, ApiTriangles, VertexCount);
ENGINE_DDI_SAME(DdiTriangles, IndexBuffer, ApiTriangles, IndexBuffer);
ENGINE_DDI_SAME(DdiTriangles, VertexBuffer, ApiTriangles, VertexBuffer);
// H:7988-7996, A:14509-14517
static_assert(sizeof(D3D12DDI_RAYTRACING_AABB) == sizeof(D3D12_RAYTRACING_AABB), "AABB");
ENGINE_DDI_ALIGN(D3D12DDI_RAYTRACING_AABB, D3D12_RAYTRACING_AABB);
ENGINE_DDI_SAME(D3D12DDI_RAYTRACING_AABB, MinX, D3D12_RAYTRACING_AABB, MinX);
ENGINE_DDI_SAME(D3D12DDI_RAYTRACING_AABB, MinY, D3D12_RAYTRACING_AABB, MinY);
ENGINE_DDI_SAME(D3D12DDI_RAYTRACING_AABB, MinZ, D3D12_RAYTRACING_AABB, MinZ);
ENGINE_DDI_SAME(D3D12DDI_RAYTRACING_AABB, MaxX, D3D12_RAYTRACING_AABB, MaxX);
ENGINE_DDI_SAME(D3D12DDI_RAYTRACING_AABB, MaxY, D3D12_RAYTRACING_AABB, MaxY);
ENGINE_DDI_SAME(D3D12DDI_RAYTRACING_AABB, MaxZ, D3D12_RAYTRACING_AABB, MaxZ);
// H:7998-8002, A:14519-14523
using DdiAabbs = D3D12DDI_RAYTRACING_GEOMETRY_AABBS_DESC_0054;
using ApiAabbs = D3D12_RAYTRACING_GEOMETRY_AABBS_DESC;
static_assert(sizeof(DdiAabbs) == sizeof(ApiAabbs), "AABBs");
ENGINE_DDI_ALIGN(DdiAabbs, ApiAabbs);
ENGINE_DDI_SAME(DdiAabbs, AABBCount, ApiAabbs, AABBCount);
ENGINE_DDI_SAME(DdiAabbs, AABBs, ApiAabbs, AABBs);
// H:8004-8013, A:14650-14659
using DdiGeometry = D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054;
using ApiGeometry = D3D12_RAYTRACING_GEOMETRY_DESC;
static_assert(sizeof(DdiGeometry) == sizeof(ApiGeometry), "geometry");
ENGINE_DDI_ALIGN(DdiGeometry, ApiGeometry);
ENGINE_DDI_SAME(DdiGeometry, Type, ApiGeometry, Type);
ENGINE_DDI_SAME(DdiGeometry, Flags, ApiGeometry, Flags);
ENGINE_DDI_SAME(DdiGeometry, Triangles, ApiGeometry, Triangles);
ENGINE_DDI_SAME(DdiGeometry, AABBs, ApiGeometry, AABBs);
// H:8042-8050, A:14640-14648. The four bit-fields share the two UINTs between Transform and AccelerationStructure.
using DdiInstance = D3D12DDI_RAYTRACING_INSTANCE_DESC_0054;
using ApiInstance = D3D12_RAYTRACING_INSTANCE_DESC;
static_assert(sizeof(DdiInstance) == sizeof(ApiInstance) && sizeof(ApiInstance) == 64, "instance");
ENGINE_DDI_ALIGN(DdiInstance, ApiInstance);
ENGINE_DDI_SAME(DdiInstance, Transform, ApiInstance, Transform);
ENGINE_DDI_SAME(DdiInstance, AccelerationStructure, ApiInstance, AccelerationStructure);
static_assert(offsetof(ApiInstance, AccelerationStructure) == offsetof(ApiInstance, Transform) + sizeof(FLOAT[3][4]) + 8,
              "instance bit-fields");
// H:8058-8070, A:14661-14673
using DdiInputs = D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054;
using ApiInputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS;
static_assert(sizeof(DdiInputs) == sizeof(ApiInputs), "inputs");
ENGINE_DDI_ALIGN(DdiInputs, ApiInputs);
ENGINE_DDI_SAME(DdiInputs, Type, ApiInputs, Type);
ENGINE_DDI_SAME(DdiInputs, Flags, ApiInputs, Flags);
ENGINE_DDI_SAME(DdiInputs, NumDescs, ApiInputs, NumDescs);
ENGINE_DDI_SAME(DdiInputs, DescsLayout, ApiInputs, DescsLayout);
ENGINE_DDI_SAME(DdiInputs, InstanceDescs, ApiInputs, InstanceDescs);
ENGINE_DDI_SAME(DdiInputs, pGeometryDescs, ApiInputs, pGeometryDescs);
ENGINE_DDI_SAME(DdiInputs, ppGeometryDescs, ApiInputs, ppGeometryDescs);
// H:8143-8147, A:14571-14575
using DdiPostbuild = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC_0054;
using ApiPostbuild = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC;
static_assert(sizeof(DdiPostbuild) == sizeof(ApiPostbuild), "postbuild");
ENGINE_DDI_ALIGN(DdiPostbuild, ApiPostbuild);
ENGINE_DDI_SAME(DdiPostbuild, DestBuffer, ApiPostbuild, DestBuffer);
ENGINE_DDI_SAME(DdiPostbuild, InfoType, ApiPostbuild, InfoType);
// H:8172-8177, A:14683-14688
using DdiPrebuild = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054;
using ApiPrebuild = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO;
static_assert(sizeof(DdiPrebuild) == sizeof(ApiPrebuild), "prebuild");
ENGINE_DDI_SAME(DdiPrebuild, ResultDataMaxSizeInBytes, ApiPrebuild, ResultDataMaxSizeInBytes);
ENGINE_DDI_SAME(DdiPrebuild, ScratchDataSizeInBytes, ApiPrebuild, ScratchDataSizeInBytes);
ENGINE_DDI_SAME(DdiPrebuild, UpdateScratchDataSizeInBytes, ApiPrebuild, UpdateScratchDataSizeInBytes);
// DispatchRays (L64): H:7964-7975 and 8197-8206, A:14485-14496 and 20894-20903.
static_assert(sizeof(D3D12DDI_GPU_VIRTUAL_ADDRESS_RANGE) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE), "range");
ENGINE_DDI_ALIGN(D3D12DDI_GPU_VIRTUAL_ADDRESS_RANGE, D3D12_GPU_VIRTUAL_ADDRESS_RANGE);
ENGINE_DDI_SAME(D3D12DDI_GPU_VIRTUAL_ADDRESS_RANGE, StartAddress, D3D12_GPU_VIRTUAL_ADDRESS_RANGE, StartAddress);
ENGINE_DDI_SAME(D3D12DDI_GPU_VIRTUAL_ADDRESS_RANGE, SizeInBytes, D3D12_GPU_VIRTUAL_ADDRESS_RANGE, SizeInBytes);
using DdiRangeStride = D3D12DDI_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE;
using ApiRangeStride = D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE;
static_assert(sizeof(DdiRangeStride) == sizeof(ApiRangeStride), "range and stride");
ENGINE_DDI_ALIGN(DdiRangeStride, ApiRangeStride);
ENGINE_DDI_SAME(DdiRangeStride, StartAddress, ApiRangeStride, StartAddress);
ENGINE_DDI_SAME(DdiRangeStride, SizeInBytes, ApiRangeStride, SizeInBytes);
ENGINE_DDI_SAME(DdiRangeStride, StrideInBytes, ApiRangeStride, StrideInBytes);
using DdiDispatchRays = D3D12DDIARG_DISPATCH_RAYS_0054;
using ApiDispatchRays = D3D12_DISPATCH_RAYS_DESC;
static_assert(sizeof(DdiDispatchRays) == sizeof(ApiDispatchRays), "dispatch rays");
ENGINE_DDI_ALIGN(DdiDispatchRays, ApiDispatchRays);
ENGINE_DDI_SAME(DdiDispatchRays, RayGenerationShaderRecord, ApiDispatchRays, RayGenerationShaderRecord);
ENGINE_DDI_SAME(DdiDispatchRays, MissShaderTable, ApiDispatchRays, MissShaderTable);
ENGINE_DDI_SAME(DdiDispatchRays, HitGroupTable, ApiDispatchRays, HitGroupTable);
ENGINE_DDI_SAME(DdiDispatchRays, CallableShaderTable, ApiDispatchRays, CallableShaderTable);
ENGINE_DDI_SAME(DdiDispatchRays, Width, ApiDispatchRays, Width);
ENGINE_DDI_SAME(DdiDispatchRays, Height, ApiDispatchRays, Height);
ENGINE_DDI_SAME(DdiDispatchRays, Depth, ApiDispatchRays, Depth);
#undef ENGINE_DDI_ALIGN
#undef ENGINE_DDI_SAME
// Enumerations: geometry flags H:7934-7939 A:14453-14458, geometry type H:7942-7946 A:14462-14466, instance flags
// H:7948-7955 A:14469-14476, build flags H:8015-8024 A:14526-14535, copy mode H:8027-8034 A:14539-14546, structure type
// H:8036-8040 A:14549-14553, elements layout H:8052-8056 A:14556-14560, postbuild type H:8072-8078 A:14563-14569.
static_assert(D3D12DDI_RAYTRACING_GEOMETRY_FLAG_OPAQUE == static_cast<int>(D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE) &&
                  D3D12DDI_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION ==
                      static_cast<int>(D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION) &&
                  D3D12DDI_RAYTRACING_GEOMETRY_TYPE_TRIANGLES == static_cast<int>(D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) &&
                  D3D12DDI_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS ==
                      static_cast<int>(D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS),
              "geometry flags and types");
static_assert(D3D12DDI_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE ==
                      static_cast<int>(D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE) &&
                  D3D12DDI_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE ==
                      static_cast<int>(D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE) &&
                  D3D12DDI_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE == static_cast<int>(D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE) &&
                  D3D12DDI_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE ==
                      static_cast<int>(D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE),
              "instance flags");
static_assert(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE),
              "build flags");
static_assert(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE),
              "copy modes");
static_assert(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) &&
                  D3D12DDI_ELEMENTS_LAYOUT_ARRAY == static_cast<int>(D3D12_ELEMENTS_LAYOUT_ARRAY) &&
                  D3D12DDI_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS == static_cast<int>(D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS),
              "structure types and element layouts");
static_assert(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION) &&
                  D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE ==
                      static_cast<int>(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE),
              "postbuild info types");

// Opacity micromaps (tier 1.2): the DDI 0110 structures have the API's members at the API's offsets, so they too
// reach the engine in place. raytracing-tier12.h has both and checks them against the headers that define them.

namespace {
const ApiInputs* api(const DdiInputs* in) noexcept { return reinterpret_cast<const ApiInputs*>(in); }

// An opacity micromap array: its description, a histogram for its count, and in every entry a format and a
// subdivision level the API defines (OC1, levels 0 to 12). The engine reads the description and the histogram at
// the call (vkd3d-proton opacity_micromap.c) and takes neither NumDescs nor DescsLayout from the inputs.
bool valid_omm_array(const DdiInputs& in) noexcept {
    const rt12::ArrayDesc* d = rt12::array_desc(in);
    if (!d || (d->NumOmmHistogramEntries && !d->pOmmHistogram)) return false;
    for (UINT i = 0; i < d->NumOmmHistogramEntries; ++i) {
        const rt12::HistogramEntry& e = d->pOmmHistogram[i];
        if ((e.Format != rt12::kFormatOc1State2 && e.Format != rt12::kFormatOc1State4) ||
            e.SubdivisionLevel > rt12::kMaxSubdivisionLevel)
            return false;
    }
    return true;
}

// What the engine would read from the CPU: a known structure type and element layout, and for a bottom level every
// geometry description of a known type. GPU addresses are the application's, as for every other slot. With tier 1.2
// in the slots (DeviceContext::omm) also an opacity micromap array and OMM_TRIANGLES geometry with both of its
// pointers (the engine reads both without a check, opacity_micromap.c), and never ALLOW_DATA_ACCESS: the API has no
// such flag (it ends at ALLOW_DISABLE_OMMS) and the engine translates none. Without it, as at tier 1.1.
bool valid_inputs(const DeviceContext& c, const DdiInputs& in) noexcept {
    const bool array = in.DescsLayout == D3D12DDI_ELEMENTS_LAYOUT_ARRAY;
    if (!array && in.DescsLayout != D3D12DDI_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) return false;
    if (c.omm) {
        if (static_cast<UINT>(in.Flags) & rt12::kBuildAllowDataAccess) return false;
        if (static_cast<UINT>(in.Type) == rt12::kOmmArrayType) return valid_omm_array(in);
    }
    if (in.Type == D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) return true;
    if (in.Type != D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) return false;
    if (in.NumDescs && (array ? !in.pGeometryDescs : !in.ppGeometryDescs)) return false;
    for (UINT i = 0; i < in.NumDescs; ++i) {
        const DdiGeometry* g = array ? &in.pGeometryDescs[i] : in.ppGeometryDescs[i];
        if (!g) return false;
        if (g->Type == D3D12DDI_RAYTRACING_GEOMETRY_TYPE_TRIANGLES ||
            g->Type == D3D12DDI_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
            continue;
        if (c.omm && static_cast<UINT>(g->Type) == rt12::kOmmTrianglesType) {
            const rt12::OmmTriangles t = rt12::omm_triangles(*g);
            if (t.pTriangles && t.pOmmLinkage) continue;
        }
        return false;
    }
    return true;
}

// The engine writes the three sizes it knows (acceleration_structure.c, write_postbuild_info) and a zero for the
// tools visualization, which is refused here rather than answered with that zero.
HRESULT postbuild_type(D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE type) noexcept {
    switch (type) {
    case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE:
    case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION:
    case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE: return S_OK;
    case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION: return E_NOTIMPL;
    default: return E_INVALIDARG;
    }
}

// D3D12 records these commands in DIRECT and COMPUTE lists that are recording; the engine list of a closed list
// must not see them. A refusal is logged and reported once on the list, and nothing reaches the engine.
bool refused(const CommandListRecord* l, const char* slot, HRESULT hr) noexcept {
    const bool list_ok = l->recording && (l->type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                                          l->type == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    if (list_ok && SUCCEEDED(hr)) return false;
    if (list_ok) log_line("%s: refused (%08lx)", slot, static_cast<unsigned long>(hr));
    else log_line("%s: refused (list type %d recording %d)", slot, static_cast<int>(l->type), l->recording ? 1 : 0);
    l->h.device->report_list(l->rt, list_ok ? hr : E_INVALIDARG);
    return true;
}

// D108. Void, so a refusal zeroes the answer and reports E_INVALIDARG through report_device_error.
void APIENTRY get_prebuild_info(D3D12DDI_HDEVICE device, const DdiInputs* inputs, DdiPrebuild* info) {
    if (info) *info = DdiPrebuild{};
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!inputs || !info || !valid_inputs(*c, *inputs)) {
        log_line("GetRaytracingAccelerationStructurePrebuildInfo: refused (inputs %s, info %s)",
                 inputs ? "given" : "null", info ? "given" : "null");
        return c->report(E_INVALIDARG);
    }
    c->device5->GetRaytracingAccelerationStructurePrebuildInfo(api(inputs), reinterpret_cast<ApiPrebuild*>(info));
}

// L60. The DDI argument is the API description followed by the postbuild descriptions.
void APIENTRY build_acceleration_structure(D3D12DDI_HCOMMANDLIST h,
                                           const D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054* args) {
    constexpr const char* kSlot = "BuildRaytracingAccelerationStructure";
    CommandListRecord* l = list_of(h, kSlot);
    if (!l) return;
    HRESULT hr = E_INVALIDARG;
    if (args && valid_inputs(*l->h.device, args->Inputs) &&
        (!args->NumPostbuildInfoDescs || args->pPostbuildInfoDescs)) {
        hr = S_OK;
        for (UINT i = 0; SUCCEEDED(hr) && i < args->NumPostbuildInfoDescs; ++i)
            hr = postbuild_type(args->pPostbuildInfoDescs[i].InfoType);
    }
    if (refused(l, kSlot, hr) || !args) return;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.DestAccelerationStructureData = args->DestAccelerationStructureData;
    desc.Inputs = *api(&args->Inputs);
    desc.SourceAccelerationStructureData = args->SourceAccelerationStructureData;
    desc.ScratchAccelerationStructureData = args->ScratchAccelerationStructureData;
    const UINT postbuild = args->NumPostbuildInfoDescs;
    // Tier 1.2 in the slots: the inputs reach the call as one deep copy (replay.h, BuildInputs), because an
    // OMM_TRIANGLES geometry and an opacity micromap array point at further CPU structures, which the engine reads
    // at the call.
    if (l->h.device->omm) {
        if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
            record(l,
                   [=](ID3D12GraphicsCommandList*, const ApiInputs* inputs, const ApiPostbuild* info) {
                       D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d = desc;
                       d.Inputs = *inputs;
                       l4->BuildRaytracingAccelerationStructure(&d, postbuild, info);
                       l4->Release();
                   },
                   BuildInputs{api(&args->Inputs)},
                   in(postbuild ? reinterpret_cast<const ApiPostbuild*>(args->pPostbuildInfoDescs) : nullptr,
                      postbuild));
        return;
    }
    // A bottom level's geometry descriptions are CPU memory the engine reads at this call (an array, or an array of
    // pointers); the call gets them through the arguments below, a top level's instances stay a GPU address.
    const bool bottom = desc.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    const bool pointers = desc.Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    const UINT geometries = bottom ? desc.Inputs.NumDescs : 0;
    using Geometry = D3D12_RAYTRACING_GEOMETRY_DESC;
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const Geometry* array, const Geometry* const* gathered,
                   const ApiPostbuild* info) {
                   D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d = desc;
                   if (bottom && pointers) d.Inputs.ppGeometryDescs = gathered;
                   else if (bottom) d.Inputs.pGeometryDescs = array;
                   l4->BuildRaytracingAccelerationStructure(&d, postbuild, info);
                   l4->Release();
               },
               in(bottom && !pointers ? desc.Inputs.pGeometryDescs : nullptr, bottom && !pointers ? geometries : 0),
               Gather<Geometry>{bottom && pointers ? desc.Inputs.ppGeometryDescs : nullptr,
                                bottom && pointers ? geometries : 0},
               in(postbuild ? reinterpret_cast<const ApiPostbuild*>(args->pPostbuildInfoDescs) : nullptr, postbuild));
}

// L61. With no source structure there is nothing to write, and the engine is not called.
void APIENTRY emit_postbuild_info(D3D12DDI_HCOMMANDLIST h,
                                  const D3D12DDIARG_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_0054* args) {
    constexpr const char* kSlot = "EmitRaytracingAccelerationStructurePostbuildInfo";
    CommandListRecord* l = list_of(h, kSlot);
    if (!l) return;
    HRESULT hr = E_INVALIDARG;
    if (args && (!args->NumSourceAccelerationStructures || args->pSourceAccelerationStructureData))
        hr = postbuild_type(args->Desc.InfoType);
    if (refused(l, kSlot, hr) || !args || !args->NumSourceAccelerationStructures ||
        !args->pSourceAccelerationStructureData)
        return;
    const ApiPostbuild desc{args->Desc.DestBuffer,
                            static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE>(args->Desc.InfoType)};
    const UINT sources = args->NumSourceAccelerationStructures;
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const D3D12_GPU_VIRTUAL_ADDRESS* source) {
                   l4->EmitRaytracingAccelerationStructurePostbuildInfo(&desc, sources, source);
                   l4->Release();
               },
               in(args->pSourceAccelerationStructureData, sources));
}

// L62. Clone and compaction always. SERIALIZE and DESERIALIZE only with tier 1.2 in the slots: an engine at tier 1_2
// serializes and deserializes (acceleration_structure.c, vkd3d_acceleration_structure_copy; command.c for
// DESERIALIZE) and owns the identifier that D109 then forwards to it. Without it they are refused with E_NOTIMPL
// (engine-ddi serializes nothing: CheckDriverMatchingIdentifier answers UNRECOGNIZED). The tools visualization is
// refused with E_NOTIMPL either way.
void APIENTRY copy_acceleration_structure(D3D12DDI_HCOMMANDLIST h,
                                          const D3D12DDIARG_COPY_RAYTRACING_ACCELERATION_STRUCTURE_0054* args) {
    constexpr const char* kSlot = "CopyRaytracingAccelerationStructure";
    CommandListRecord* l = list_of(h, kSlot);
    if (!l) return;
    HRESULT hr = E_INVALIDARG;
    if (args) {
        switch (args->Mode) {
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE:
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT: hr = S_OK; break;
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE:
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE:
            hr = l->h.device->omm ? S_OK : E_NOTIMPL;
            break;
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS: hr = E_NOTIMPL; break;
        default: break;
        }
    }
    if (refused(l, kSlot, hr) || !args) return;
    const D3D12_GPU_VIRTUAL_ADDRESS dst = args->DestAccelerationStructureData;
    const D3D12_GPU_VIRTUAL_ADDRESS src = args->SourceAccelerationStructureData;
    const auto mode = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE>(args->Mode);
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l4->CopyRaytracingAccelerationStructure(dst, src, mode);
            l4->Release();
        });
}

// L63. A live ray tracing pipeline of the list's device (state-objects.cpp); a collection, an inert record or another
// device's state object is refused. The engine resolves its argument without a check, so null never reaches it.
void APIENTRY set_pipeline_state1(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HSTATEOBJECT_0054 so) {
    constexpr const char* kSlot = "SetPipelineState1";
    CommandListRecord* l = list_of(h, kSlot);
    if (!l) return;
    const auto* r = record_of<StateObjectRecord>(so.pDrvPrivate, Tag::StateObject, l->h.device);
    const bool usable = r && r->properties && r->executable;
    if (refused(l, kSlot, usable ? S_OK : E_INVALIDARG) || !usable) return;
    auto* state_object = static_cast<ID3D12StateObject*>(r->h.engine);
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l4->SetPipelineState1(state_object);
            l4->Release();
        });
}

// L64. The DDI argument is the API description (asserted above). Indirect dispatch goes through ExecuteIndirect
// with a DISPATCH_RAYS command signature, passed to the engine like the draw and dispatch signatures.
void APIENTRY dispatch_rays(D3D12DDI_HCOMMANDLIST h, const D3D12DDIARG_DISPATCH_RAYS_0054* args) {
    constexpr const char* kSlot = "DispatchRays";
    CommandListRecord* l = list_of(h, kSlot);
    if (!l) return;
    if (refused(l, kSlot, args ? S_OK : E_INVALIDARG) || !args) return;
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const ApiDispatchRays* d) {
                   l4->DispatchRays(d);
                   l4->Release();
               },
               in(reinterpret_cast<const ApiDispatchRays*>(args), 1));
}

// L67. The thread group counts of one mesh dispatch, which the engine's ID3D12GraphicsCommandList6 takes
// unchanged. The runtime sends the slot only when 1006 reports a mesh shader tier (caps.cpp, fill_options), and
// bounds the counts itself; the engine owns the per-dimension limits of its Vulkan device. Indirect mesh dispatch
// is ExecuteIndirect with a DISPATCH_MESH command signature, which create_command_signature already translates.
void APIENTRY dispatch_mesh(D3D12DDI_HCOMMANDLIST h, UINT x, UINT y, UINT z) {
    CommandListRecord* l = list_of(h, "DispatchMesh");
    if (!l) return;
    if (auto* l6 = list_as<ID3D12GraphicsCommandList6>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l6->DispatchMesh(x, y, z);
            l6->Release();
        });
}

// ---- The render pass table (D3D12DDI_TABLE_TYPE_0043_RENDER_PASS, H:2507) ----------------------------------------
// Its two slots at 0053, D3D12DDI_RENDER_PASS_FUNCS_0053 (H:18254-18259), onto ID3D12GraphicsCommandList4. The
// runtime asks for the table only while 1006 reports a render pass tier, which is the engine's own answer
// (caps.cpp, render_pass_tier); without it the runtime emulates render passes and never calls either slot.
//
// The DDI descriptions and the API's differ in three places and nowhere else: a render target and the depth
// stencil name their view by a CPU descriptor handle, a RESOLVE ending access names its two resources by DDI
// handles, and that access points at its own array of subresource parameters. The two access type
// enumerations, the clear value, the subresource parameters and the pass flags share their values and their
// layout, asserted below.
//
// The engine reads the view descriptors and every array at the call (vkd3d-proton command.c,
// d3d12_command_list_BeginRenderPass), so a deferred entry carries copies of all of them: the views through the
// ring's own heaps (replay.h, Snap - one RTV snapshot for the render targets, one DSV snapshot for the depth
// stencil), the two descriptions and the resolve subresource parameters through in() buffers. The parameters of
// every resolve go into one flat buffer, because an in() copy cannot hold a pointer into another copy; the lambda
// rebuilds the descriptions on its own stack and points each resolve back into that buffer by the offsets this
// slot recorded. Neither slot checks where a render pass is allowed: the engine owns that
// (d3d12_command_list_check_render_pass_validation).
static_assert(D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0053_DISCARD ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD) &&
                  D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0053_PRESERVE ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE) &&
                  D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0053_CLEAR ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) &&
                  D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0053_NO_ACCESS ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS) &&
                  D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0101_PRESERVE_LOCAL_RENDER ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_RENDER) &&
                  D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0101_PRESERVE_LOCAL_SRV ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_SRV) &&
                  D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0101_PRESERVE_LOCAL_UAV ==
                      static_cast<int>(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE_LOCAL_UAV),
              "render pass beginning access types");
static_assert(D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0053_DISCARD ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD) &&
                  D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0053_PRESERVE ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE) &&
                  D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0053_RESOLVE ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) &&
                  D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0053_NO_ACCESS ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS) &&
                  D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0101_PRESERVE_LOCAL_RENDER ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE_LOCAL_RENDER) &&
                  D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0101_PRESERVE_LOCAL_SRV ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE_LOCAL_SRV) &&
                  D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0101_PRESERVE_LOCAL_UAV ==
                      static_cast<int>(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE_LOCAL_UAV),
              "render pass ending access types");
static_assert(D3D12DDI_RENDER_PASS_FLAG_0053_NONE == static_cast<int>(D3D12_RENDER_PASS_FLAG_NONE) &&
                  D3D12DDI_RENDER_PASS_FLAG_0053_ALLOW_UAV_WRITES ==
                      static_cast<int>(D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES) &&
                  D3D12DDI_RENDER_PASS_FLAG_0053_SUSPENDING_PASS ==
                      static_cast<int>(D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS) &&
                  D3D12DDI_RENDER_PASS_FLAG_0053_RESUMING_PASS ==
                      static_cast<int>(D3D12_RENDER_PASS_FLAG_RESUMING_PASS) &&
                  D3D12DDI_RENDER_PASS_FLAG_0101_BIND_READ_ONLY_DEPTH ==
                      static_cast<int>(D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_DEPTH) &&
                  D3D12DDI_RENDER_PASS_FLAG_0101_BIND_READ_ONLY_STENCIL ==
                      static_cast<int>(D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_STENCIL),
              "render pass flags");
static_assert(sizeof(D3D12DDI_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS_0053) ==
                      sizeof(D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS) &&
                  offsetof(D3D12DDI_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS_0053, SrcRect) ==
                      offsetof(D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS, SrcRect),
              "render pass resolve subresource parameters");
// A CLEAR beginning access carries its clear value, which begin_access below copies out of the DDI union member
// by the length of the API's own. The equality is pinned in resources.cpp as well, for the create's clear value;
// it is repeated here because this translation unit is where the copy is, and a reader of this section has to be
// able to check the contract it states (H:474-482).
static_assert(sizeof(D3D12DDI_CLEAR_VALUES) == sizeof(D3D12_CLEAR_VALUE) &&
                  offsetof(D3D12DDI_CLEAR_VALUES, Color) == offsetof(D3D12_CLEAR_VALUE, Color) &&
                  sizeof(D3D12DDI_CLEAR_VALUES::Color) == sizeof(D3D12_CLEAR_VALUE::Color),
              "render pass clear value layout");

using ApiResolveSubresource = D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS;
constexpr UINT kRenderPassTargets = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
// One resolve subresource array may come with each render target's ending access and with the depth and the
// stencil ending access of the depth-stencil description.
constexpr UINT kRenderPassResolves = kRenderPassTargets + 2;
constexpr UINT kNoResolve = 0xFFFFFFFFu;            // an ending access with no subresource array of its own
// The ceiling of the one flat buffer those arrays are copied into. Every SubresourceCount is an unvalidated UINT
// of the runtime's payload, and up to kRenderPassResolves of them are summed, so the sum is kept in 64 bits and
// refused above this: the buffer is indexed with a UINT (kNoResolve stays above every index) and addressed with a
// size_t, which is 32 bits in the x86 (WoW64) build of this file (tools\build\build-umd-d3d12.ps1,
// UserModeDriverNameWow), where a truncated sum would reserve less room than the copies write.
constexpr uint64_t kMaxResolveParams = SIZE_MAX / sizeof(ApiResolveSubresource) < kNoResolve
                                           ? SIZE_MAX / sizeof(ApiResolveSubresource)
                                           : kNoResolve - 1;

D3D12_RENDER_PASS_BEGINNING_ACCESS begin_access(const D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_0053& in) noexcept {
    D3D12_RENDER_PASS_BEGINNING_ACCESS out{};
    out.Type = static_cast<D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE>(in.Type);
    switch (in.Type) {
    case D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0053_CLEAR:
        std::memcpy(&out.Clear.ClearValue, &in.Clear.ClearValue, sizeof(out.Clear.ClearValue));
        break;
    case D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0101_PRESERVE_LOCAL_RENDER:
    case D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0101_PRESERVE_LOCAL_SRV:
    case D3D12DDI_RENDER_PASS_BEGINNING_ACCESS_TYPE_0101_PRESERVE_LOCAL_UAV:
        out.PreserveLocal.AdditionalWidth = in.PreserveLocal.AdditionalWidth;
        out.PreserveLocal.AdditionalHeight = in.PreserveLocal.AdditionalHeight;
        break;
    default:
        break;                                      // DISCARD, PRESERVE and NO_ACCESS carry no parameters
    }
    return out;
}

// How many resolve subresource parameters one ending access carries.
UINT resolve_count(const D3D12DDI_RENDER_PASS_ENDING_ACCESS_0053& in) noexcept {
    return in.Type == D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0053_RESOLVE ? in.Resolve.SubresourceCount : 0;
}

// One ending access, translated into out. A resolve's subresource parameters are appended to flat at used, and
// *at becomes their index there; pSubresourceParameters stays null for the lambda to fill. room is how many
// elements flat holds, the sum the caller reserved, and it is checked here as well: the copy below is the one
// write into that buffer, so its bound belongs next to it. False for a resolve whose resources this device does
// not know, whose array is missing, or whose count no longer fits the room the caller reserved.
bool ending_access(CommandListRecord* l, const D3D12DDI_RENDER_PASS_ENDING_ACCESS_0053& in,
                   D3D12_RENDER_PASS_ENDING_ACCESS& out, ApiResolveSubresource* flat, UINT room, UINT& used,
                   UINT* at) noexcept {
    out = D3D12_RENDER_PASS_ENDING_ACCESS{};
    out.Type = static_cast<D3D12_RENDER_PASS_ENDING_ACCESS_TYPE>(in.Type);
    *at = kNoResolve;
    switch (in.Type) {
    case D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0053_RESOLVE: {
        const D3D12DDI_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS_0053& r = in.Resolve;
        auto* src = record_of<ResourceRecord>(r.hSrcResource.pDrvPrivate, Tag::Resource, l->h.device);
        auto* dst = record_of<ResourceRecord>(r.hDstResource.pDrvPrivate, Tag::Resource, l->h.device);
        if (!src || !dst || (r.SubresourceCount && !r.pSubresourceParameters)) return false;
        if (r.SubresourceCount > room - used) return false;     // used <= room, so this cannot wrap
        out.Resolve.pSrcResource = static_cast<ID3D12Resource*>(src->h.engine);
        out.Resolve.pDstResource = static_cast<ID3D12Resource*>(dst->h.engine);
        out.Resolve.SubresourceCount = r.SubresourceCount;
        out.Resolve.pSubresourceParameters = nullptr;
        out.Resolve.Format = r.Format;
        out.Resolve.ResolveMode = static_cast<D3D12_RESOLVE_MODE>(r.ResolveMode);
        out.Resolve.PreserveResolveSource = r.PreserveResolveSource;
        if (r.SubresourceCount) {
            *at = used;
            std::memcpy(flat + used, r.pSubresourceParameters, size_t{r.SubresourceCount} * sizeof(*flat));
            used += r.SubresourceCount;
        }
        break;
    }
    case D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0101_PRESERVE_LOCAL_RENDER:
    case D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0101_PRESERVE_LOCAL_SRV:
    case D3D12DDI_RENDER_PASS_ENDING_ACCESS_TYPE_0101_PRESERVE_LOCAL_UAV:
        out.PreserveLocal.AdditionalWidth = in.PreserveLocal.AdditionalWidth;
        out.PreserveLocal.AdditionalHeight = in.PreserveLocal.AdditionalHeight;
        break;
    default:
        break;                                      // DISCARD, PRESERVE and NO_ACCESS carry no parameters
    }
    return true;
}

void APIENTRY begin_render_pass(D3D12DDI_HCOMMANDLIST h, UINT count,
                                const D3D12DDIARG_RENDER_PASS_RENDER_TARGET_DESC_0053* targets,
                                const D3D12DDIARG_RENDER_PASS_DEPTH_STENCIL_DESC_0053* depth_stencil,
                                D3D12DDIARG_RENDER_PASS_FLAGS_0053 flags) {
    CommandListRecord* l = list_of(h, "BeginRenderPass");
    if (!l) return;
    if (count > kRenderPassTargets || (count && !targets)) {
        log_line("BeginRenderPass: refused (%u render targets, %s)", static_cast<unsigned>(count),
                 targets ? "given" : "no array");
        return l->h.device->report_list(l->rt, E_INVALIDARG);
    }
    const UINT n = count < kRenderPassTargets ? count : kRenderPassTargets;   // the bound above, for the copies
    const bool has_depth = depth_stencil != nullptr;
    uint64_t total = 0;
    for (UINT i = 0; i < n; ++i) total += resolve_count(targets[i].EndingAccess);
    if (has_depth)
        total += uint64_t{resolve_count(depth_stencil->DepthEndingAccess)} +
                 resolve_count(depth_stencil->StencilEndingAccess);
    if (total > kMaxResolveParams) {
        log_line("BeginRenderPass: refused (%llu resolve subresource parameters over %llu)",
                 static_cast<unsigned long long>(total), static_cast<unsigned long long>(kMaxResolveParams));
        return l->h.device->report_list(l->rt, E_INVALIDARG);
    }
    const UINT room = static_cast<UINT>(total);      // bounded above, so neither cast loses anything
    InlineArray<ApiResolveSubresource, 16> flat;
    if (!flat.reserve(static_cast<size_t>(total))) return l->h.device->report_list(l->rt, E_OUTOFMEMORY);

    D3D12_RENDER_PASS_RENDER_TARGET_DESC rts[kRenderPassTargets]{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kRenderPassTargets]{};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    std::array<UINT, kRenderPassResolves> at{};
    at.fill(kNoResolve);
    UINT used = 0;
    for (UINT i = 0; i < n; ++i) {
        rtvs[i].ptr = targets[i].cpuDescriptor.ptr;
        rts[i].BeginningAccess = begin_access(targets[i].BeginningAccess);
        if (!ending_access(l, targets[i].EndingAccess, rts[i].EndingAccess, flat.data(), room, used, &at[i]))
            return l->h.device->report_list(l->rt, E_INVALIDARG);
    }
    if (has_depth) {
        dsv.ptr = depth_stencil->cpuDescriptor.ptr;
        depth.DepthBeginningAccess = begin_access(depth_stencil->DepthBeginningAccess);
        depth.StencilBeginningAccess = begin_access(depth_stencil->StencilBeginningAccess);
        if (!ending_access(l, depth_stencil->DepthEndingAccess, depth.DepthEndingAccess, flat.data(), room, used,
                           &at[kRenderPassTargets]) ||
            !ending_access(l, depth_stencil->StencilEndingAccess, depth.StencilEndingAccess, flat.data(), room,
                           used, &at[kRenderPassTargets + 1]))
            return l->h.device->report_list(l->rt, E_INVALIDARG);
    }
    const auto api_flags = static_cast<D3D12_RENDER_PASS_FLAGS>(flags);
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l,
               [=](ID3D12GraphicsCommandList*, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* t,
                   const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* d, const D3D12_CPU_DESCRIPTOR_HANDLE* rv,
                   const D3D12_CPU_DESCRIPTOR_HANDLE* dv, const ApiResolveSubresource* f) {
                   D3D12_RENDER_PASS_RENDER_TARGET_DESC pass[kRenderPassTargets]{};
                   for (UINT i = 0; i < n; ++i) {
                       pass[i] = t[i];
                       pass[i].cpuDescriptor = rv[i];
                       if (at[i] != kNoResolve) pass[i].EndingAccess.Resolve.pSubresourceParameters = f + at[i];
                   }
                   D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds{};
                   if (d && dv) {
                       ds = *d;
                       ds.cpuDescriptor = *dv;
                       if (at[kRenderPassTargets] != kNoResolve)
                           ds.DepthEndingAccess.Resolve.pSubresourceParameters = f + at[kRenderPassTargets];
                       if (at[kRenderPassTargets + 1] != kNoResolve)
                           ds.StencilEndingAccess.Resolve.pSubresourceParameters = f + at[kRenderPassTargets + 1];
                   }
                   l4->BeginRenderPass(n, n ? pass : nullptr, d && dv ? &ds : nullptr, api_flags);
                   l4->Release();
               },
               in(n ? rts : nullptr, n), in(has_depth ? &depth : nullptr, has_depth ? 1u : 0u),
               Snap{n ? rtvs : nullptr, n, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FALSE},
               Snap{has_depth ? &dsv : nullptr, has_depth ? 1u : 0u, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, FALSE},
               in(total ? flat.data() : nullptr, static_cast<size_t>(total)));
}

void APIENTRY end_render_pass(D3D12DDI_HCOMMANDLIST h) {
    CommandListRecord* l = list_of(h, "EndRenderPass");
    if (!l) return;
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l))
        record(l, [=](ID3D12GraphicsCommandList*) {
            l4->EndRenderPass();
            l4->Release();
        });
}
} // namespace

void fill_core_commands(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateCommandSignatureSize = calc_command_signature;
    t->pfnCreateCommandSignature = create_command_signature;
    t->pfnDestroyCommandSignature = destroy_command_signature;
    t->pfnCalcPrivateCommandPoolSize = calc_pool;
    t->pfnCreateCommandPool = create_pool;
    t->pfnDestroyCommandPool = destroy_pool;
    t->pfnResetCommandPool = reset_pool;
    t->pfnCalcPrivateCommandRecorderSize = calc_recorder;
    t->pfnCreateCommandRecorder = create_recorder;
    t->pfnDestroyCommandRecorder = destroy_recorder;
    t->pfnCommandRecorderSetCommandPoolAsTarget = set_pool_as_target;
    t->pfnCalcPrivateCommandListSize = calc_list;
    t->pfnCreateCommandList = create_list;
    t->pfnDestroyCommandList = destroy_list;
    t->pfnGetRaytracingAccelerationStructurePrebuildInfo = get_prebuild_info;
}

void fill_list_commands(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t table_index) noexcept {
    t->pfnCloseCommandList = close_list;
    t->pfnResetCommandList = reset_list;
    t->pfnSetPredication = set_predication;
    t->pfnSetProtectedResourceSession = set_protected_session;
    t->pfnClearRootArguments = clear_root_arguments;
    t->pfnSetMarker = set_marker;
    if (table_index == 1) t->pfnExecuteBundle = execute_bundle;
    t->pfnExecuteIndirect = execute_indirect;
    t->pfnBuildRaytracingAccelerationStructure = build_acceleration_structure;
    t->pfnEmitRaytracingAccelerationStructurePostbuildInfo = emit_postbuild_info;
    t->pfnCopyRaytracingAccelerationStructure = copy_acceleration_structure;
    t->pfnSetPipelineState1 = set_pipeline_state1;
    t->pfnDispatchRays = dispatch_rays;
    // L56 is in both tables: the engine admits the immediate write on every queue type it creates, and 1006
    // WriteBufferImmediateQueueFlags reports exactly those queues.
    t->pfnWriteBufferImmediate = write_buffer_immediate;
    if (table_index != 1) return;                      // the compute table keeps its rejections
    t->pfnResourceResolveSubresource = resolve_subresource;
    t->pfnResourceResolveSubresourceRegion = resolve_subresource_region;
    t->pfnOMSetDepthBounds = om_set_depth_bounds;
    t->pfnSetSamplePositions = set_sample_positions;
    t->pfnSetViewInstanceMask = set_view_instance_mask;
    t->pfnRSSetShadingRate = rs_set_shading_rate;
    t->pfnRSSetShadingRateImage = rs_set_shading_rate_image;
    t->pfnDispatchMesh = dispatch_mesh;
    t->pfnOmSetAlphaBlendFactor = om_set_alpha_blend_factor;
}

void fill_render_pass_commands(D3D12DDI_RENDER_PASS_FUNCS_0053* t) noexcept {
    t->pfnBeginRenderPass = begin_render_pass;
    t->pfnEndRenderPass = end_render_pass;
}

} // namespace engine_ddi
