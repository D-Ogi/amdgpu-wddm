// SPDX-License-Identifier: MIT
// engine-ddi: command pools (D29-D32), command recorders (D92-D95), command lists (D36-D38) and the list slots
// that open and close recording (L0, L1); the acceleration structure slots (D108, L60-L62).
//
// DDI 0040 splits the API's allocator and list: a pool is the API allocator, a recorder names the pool a list
// records into, and ResetCommandList names the recorder. A pool therefore does not know its list type at
// creation; it creates one engine allocator per list type on first use.
#include <atomic>
#include <memory>
#include <type_traits>
#include "internal.h"

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
    release_engine(l->h);
    poison(l->h);
    c->live.fetch_sub(1);
}

void APIENTRY close_list(D3D12DDI_HCOMMANDLIST h) {
    CommandListRecord* l = list_of(h, "CloseCommandList");
    if (!l) return;
    HRESULT hr = l->list()->Close();
    // A list whose Close failed is not a closed list: it stays what it was.
    if (FAILED(hr)) l->h.device->report_list(l->rt, hr);
    else l->recording = false;
}

void APIENTRY reset_list(D3D12DDI_HCOMMANDLIST h, const D3D12DDIARG_RESETCOMMANDLIST_0040* args) {
    CommandListRecord* l = list_of(h, "ResetCommandList");
    if (!l) return;
    DeviceContext* c = l->h.device;
    auto* r = args ? record_of<CommandRecorderRecord>(args->hDrvCommandRecorder.pDrvPrivate, Tag::CommandRecorder, c)
                   : nullptr;
    // The pool may have been destroyed since it became the target: its record is poisoned then.
    CommandPoolRecord* p = r ? record_of<CommandPoolRecord>(r->pool, Tag::CommandPool, c) : nullptr;
    if (!p) {
        c->report_list(l->rt, E_INVALIDARG);
        return;
    }
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
    if (FAILED(hr)) c->report_list(l->rt, hr);
    else l->recording = true;
}

// ---- List state the runtime sets on every list ---------------------------------------------------------------------
// The runtime writes default state into a list it has reset. SetPredication was seen there; the others are
// answered ahead of need. Each forwards to the engine list, which owns the feature check. Forwarding is
// not support: the caps of these features stay as they are.
template <class I> I* list_as(CommandListRecord* l) noexcept {
    I* out = nullptr;
    if (FAILED(l->list()->QueryInterface(__uuidof(I), reinterpret_cast<void**>(&out)))) {
        l->h.device->report_list(l->rt, E_NOINTERFACE);
        return nullptr;
    }
    return out;
}

void APIENTRY set_predication(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HRESOURCE hres, UINT64 offset,
                              D3D12DDI_PREDICATION_OP op) {
    CommandListRecord* l = list_of(h, "SetPredication");
    if (!l) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, l->h.device);
    if (hres.pDrvPrivate && !r) return l->h.device->report_list(l->rt, E_INVALIDARG);
    l->list()->SetPredication(r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr, r ? offset : 0,
                              static_cast<D3D12_PREDICATION_OP>(op));
}

void APIENTRY om_set_depth_bounds(D3D12DDI_HCOMMANDLIST h, FLOAT low, FLOAT high) {
    CommandListRecord* l = list_of(h, "OMSetDepthBounds");
    if (!l) return;
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l)) {
        l1->OMSetDepthBounds(low, high);
        l1->Release();
    }
}

void APIENTRY set_sample_positions(D3D12DDI_HCOMMANDLIST h, UINT per_pixel, UINT pixels,
                                   D3D12DDI_SAMPLE_POSITION* positions) {
    static_assert(sizeof(D3D12DDI_SAMPLE_POSITION) == sizeof(D3D12_SAMPLE_POSITION), "sample position");
    CommandListRecord* l = list_of(h, "SetSamplePositions");
    if (!l) return;
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l)) {
        l1->SetSamplePositions(per_pixel, pixels, reinterpret_cast<D3D12_SAMPLE_POSITION*>(positions));
        l1->Release();
    }
}

void APIENTRY set_view_instance_mask(D3D12DDI_HCOMMANDLIST h, UINT mask) {
    CommandListRecord* l = list_of(h, "SetViewInstanceMask");
    if (!l) return;
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l)) {
        l1->SetViewInstanceMask(mask);
        l1->Release();
    }
}

// ---- L15 pfnExecuteBundle ---------------------------------------------------------------------------------------
// The engine replays the bundle's commands into the parent at this call, so the bundle must be closed and the
// parent recording. Both DIRECT lists and bundles are bound to the graphics table: the types are checked here,
// a bundle executed from a bundle would otherwise be dropped by the engine without a word.
void APIENTRY execute_bundle(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HCOMMANDLIST hbundle) {
    CommandListRecord* l = list_of(h, "ExecuteBundle");
    if (!l) return;
    auto* b = record_of<CommandListRecord>(hbundle.pDrvPrivate, Tag::CommandList, l->h.device);
    if (l->type != D3D12_COMMAND_LIST_TYPE_DIRECT || !l->recording || !b ||
        b->type != D3D12_COMMAND_LIST_TYPE_BUNDLE || b->recording) {
        log_line("ExecuteBundle: refused (parent type %d recording %d, bundle %s)", static_cast<int>(l->type),
                 l->recording ? 1 : 0, !b ? "unknown" : b->recording ? "recording" : "not a bundle");
        return l->h.device->report_list(l->rt, E_INVALIDARG);
    }
    l->list()->ExecuteBundle(b->list());
}

// ---- L49 pfnSetMarker -------------------------------------------------------------------------------------------
// Accepted and dropped: a compatibility answer, not marker support. What the UINT64 means to a consumer of
// markers is not established here, and nothing reaches the engine.
void APIENTRY set_marker(D3D12DDI_HCOMMANDLIST h, UINT64) { (void)list_of(h, "SetMarker"); }

// Seen once, in the state the runtime writes into a list it has just reset. There the engine's Reset has
// already cleared every root binding, so nothing is left to do. The engine has no call that clears the
// arguments of a list in use: a null signature unbinds the signature and keeps the arguments. Whether the
// runtime ever asks for that is not known; until it is, the slot changes nothing and says so once.
void APIENTRY clear_root_arguments(D3D12DDI_HCOMMANDLIST h) {
    if (!list_of(h, "ClearRootArguments")) return;
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) log_line("ClearRootArguments: no engine operation, root state left as it is");
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
    if (auto* l5 = list_as<ID3D12GraphicsCommandList5>(l)) {
        l5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(rate),
                             reinterpret_cast<const D3D12_SHADING_RATE_COMBINER*>(combiners));
        l5->Release();
    }
}

void APIENTRY rs_set_shading_rate_image(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HRESOURCE hres) {
    CommandListRecord* l = list_of(h, "RSSetShadingRateImage");
    if (!l) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, l->h.device);
    if (hres.pDrvPrivate && !r) return l->h.device->report_list(l->rt, E_INVALIDARG);
    if (auto* l5 = list_as<ID3D12GraphicsCommandList5>(l)) {
        l5->RSSetShadingRateImage(r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr);
        l5->Release();
    }
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
                  static_cast<int>(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW),
              "indirect argument types");

SIZE_T APIENTRY calc_command_signature(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*) {
    return sizeof(CommandSignatureRecord);
}

// The engine refuses a root signature that no argument needs as well as a missing one, and the DDI does not say
// whether the runtime sends the handle for a signature of draws alone: it is passed on only when an argument
// changes root arguments.
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
        default:
            return E_NOTIMPL;                           // ray and mesh dispatch, incrementing constant
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
    if (FAILED(hr)) return hr;
    if (!signature) return E_UNEXPECTED;
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
    l->list()->ExecuteIndirect(static_cast<ID3D12CommandSignature*>(s->h.engine), max_count,
                               static_cast<ID3D12Resource*>(a->h.engine), arguments.BaseAddress.UMD.Offset,
                               n ? static_cast<ID3D12Resource*>(n->h.engine) : nullptr,
                               n ? count.BaseAddress.UMD.Offset : 0);
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
    l->list()->ResolveSubresource(static_cast<ID3D12Resource*>(d->h.engine), dst_subresource,
                                  static_cast<ID3D12Resource*>(s->h.engine), src_subresource, format);
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
    if (auto* l1 = list_as<ID3D12GraphicsCommandList1>(l)) {
        l1->ResolveSubresourceRegion(static_cast<ID3D12Resource*>(d->h.engine), dst_subresource, x, y,
                                     static_cast<ID3D12Resource*>(s->h.engine), src_subresource, rect, format,
                                     static_cast<D3D12_RESOLVE_MODE>(mode));
        l1->Release();
    }
}

// ---- Acceleration structures (D108, L60-L62) -----------------------------------------------------------------------
// The engine's ID3D12Device5 and ID3D12GraphicsCommandList4 methods. The DDI 0054 structures below have the API's
// members at the API's offsets (H = d3d12umddi.h, A = d3d12.h, 10.0.26100), so the inputs, the geometry arrays they
// point to, the prebuild answer and the postbuild descriptions reach the engine in place. The build, emit and copy
// arguments pack what the API takes as parameters and are unpacked. Instance descriptions are GPU memory, which the
// engine reads in the API's layout; the DDI's is checked against it too.
#define ENGINE_DDI_SAME(D, dm, A, am) static_assert(offsetof(D, dm) == offsetof(A, am), #D "::" #dm)
static_assert(std::is_same_v<D3D12DDI_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS>, "GPU VA");  // H:92, A:1397
// H:7958-7962, A:14479-14483
static_assert(sizeof(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE), "VA+stride");
ENGINE_DDI_SAME(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StartAddress, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StartAddress);
ENGINE_DDI_SAME(D3D12DDI_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StrideInBytes, D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StrideInBytes);
// H:7977-7986, A:14498-14507
using DdiTriangles = D3D12DDI_RAYTRACING_GEOMETRY_TRIANGLES_DESC_0054;
using ApiTriangles = D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC;
static_assert(sizeof(DdiTriangles) == sizeof(ApiTriangles), "triangles");
ENGINE_DDI_SAME(DdiTriangles, ColumnMajorTransform3x4, ApiTriangles, Transform3x4);
ENGINE_DDI_SAME(DdiTriangles, IndexFormat, ApiTriangles, IndexFormat);
ENGINE_DDI_SAME(DdiTriangles, VertexFormat, ApiTriangles, VertexFormat);
ENGINE_DDI_SAME(DdiTriangles, IndexCount, ApiTriangles, IndexCount);
ENGINE_DDI_SAME(DdiTriangles, VertexCount, ApiTriangles, VertexCount);
ENGINE_DDI_SAME(DdiTriangles, IndexBuffer, ApiTriangles, IndexBuffer);
ENGINE_DDI_SAME(DdiTriangles, VertexBuffer, ApiTriangles, VertexBuffer);
// H:7988-7996, A:14509-14517
static_assert(sizeof(D3D12DDI_RAYTRACING_AABB) == sizeof(D3D12_RAYTRACING_AABB), "AABB");
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
ENGINE_DDI_SAME(DdiAabbs, AABBCount, ApiAabbs, AABBCount);
ENGINE_DDI_SAME(DdiAabbs, AABBs, ApiAabbs, AABBs);
// H:8004-8013, A:14650-14659
using DdiGeometry = D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054;
using ApiGeometry = D3D12_RAYTRACING_GEOMETRY_DESC;
static_assert(sizeof(DdiGeometry) == sizeof(ApiGeometry), "geometry");
ENGINE_DDI_SAME(DdiGeometry, Type, ApiGeometry, Type);
ENGINE_DDI_SAME(DdiGeometry, Flags, ApiGeometry, Flags);
ENGINE_DDI_SAME(DdiGeometry, Triangles, ApiGeometry, Triangles);
ENGINE_DDI_SAME(DdiGeometry, AABBs, ApiGeometry, AABBs);
// H:8042-8050, A:14640-14648. The four bit-fields share the two UINTs between Transform and AccelerationStructure.
using DdiInstance = D3D12DDI_RAYTRACING_INSTANCE_DESC_0054;
using ApiInstance = D3D12_RAYTRACING_INSTANCE_DESC;
static_assert(sizeof(DdiInstance) == sizeof(ApiInstance) && sizeof(ApiInstance) == 64, "instance");
ENGINE_DDI_SAME(DdiInstance, Transform, ApiInstance, Transform);
ENGINE_DDI_SAME(DdiInstance, AccelerationStructure, ApiInstance, AccelerationStructure);
static_assert(offsetof(ApiInstance, AccelerationStructure) == offsetof(ApiInstance, Transform) + sizeof(FLOAT[3][4]) + 8,
              "instance bit-fields");
// H:8058-8070, A:14661-14673
using DdiInputs = D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054;
using ApiInputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS;
static_assert(sizeof(DdiInputs) == sizeof(ApiInputs), "inputs");
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
ENGINE_DDI_SAME(DdiPostbuild, DestBuffer, ApiPostbuild, DestBuffer);
ENGINE_DDI_SAME(DdiPostbuild, InfoType, ApiPostbuild, InfoType);
// H:8172-8177, A:14683-14688
using DdiPrebuild = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054;
using ApiPrebuild = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO;
static_assert(sizeof(DdiPrebuild) == sizeof(ApiPrebuild), "prebuild");
ENGINE_DDI_SAME(DdiPrebuild, ResultDataMaxSizeInBytes, ApiPrebuild, ResultDataMaxSizeInBytes);
ENGINE_DDI_SAME(DdiPrebuild, ScratchDataSizeInBytes, ApiPrebuild, ScratchDataSizeInBytes);
ENGINE_DDI_SAME(DdiPrebuild, UpdateScratchDataSizeInBytes, ApiPrebuild, UpdateScratchDataSizeInBytes);
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

namespace {
const ApiInputs* api(const DdiInputs* in) noexcept { return reinterpret_cast<const ApiInputs*>(in); }

// What the engine would read from the CPU: a known structure type and element layout, and for a bottom level every
// geometry description of a known type. GPU addresses are the application's, as for every other slot.
bool valid_inputs(const DdiInputs& in) noexcept {
    const bool array = in.DescsLayout == D3D12DDI_ELEMENTS_LAYOUT_ARRAY;
    if (!array && in.DescsLayout != D3D12DDI_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) return false;
    if (in.Type == D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) return true;
    if (in.Type != D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) return false;
    if (in.NumDescs && (array ? !in.pGeometryDescs : !in.ppGeometryDescs)) return false;
    for (UINT i = 0; i < in.NumDescs; ++i) {
        const DdiGeometry* g = array ? &in.pGeometryDescs[i] : in.ppGeometryDescs[i];
        if (!g || (g->Type != D3D12DDI_RAYTRACING_GEOMETRY_TYPE_TRIANGLES &&
                   g->Type != D3D12DDI_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS))
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
    if (!inputs || !info || !valid_inputs(*inputs)) {
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
    if (args && valid_inputs(args->Inputs) && (!args->NumPostbuildInfoDescs || args->pPostbuildInfoDescs)) {
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
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l)) {
        l4->BuildRaytracingAccelerationStructure(&desc, args->NumPostbuildInfoDescs,
                                                 args->NumPostbuildInfoDescs
                                                     ? reinterpret_cast<const ApiPostbuild*>(args->pPostbuildInfoDescs)
                                                     : nullptr);
        l4->Release();
    }
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
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l)) {
        l4->EmitRaytracingAccelerationStructurePostbuildInfo(&desc, args->NumSourceAccelerationStructures,
                                                             args->pSourceAccelerationStructureData);
        l4->Release();
    }
}

// L62. The engine copies by clone and compaction only (acceleration_structure.c, convert_copy_mode) and drops the
// other modes with a log line; they are refused here with E_NOTIMPL (engine-ddi serializes nothing:
// CheckDriverMatchingIdentifier answers UNRECOGNIZED).
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
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS:
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE:
        case D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE: hr = E_NOTIMPL; break;
        default: break;
        }
    }
    if (refused(l, kSlot, hr) || !args) return;
    if (auto* l4 = list_as<ID3D12GraphicsCommandList4>(l)) {
        l4->CopyRaytracingAccelerationStructure(args->DestAccelerationStructureData, args->SourceAccelerationStructureData,
                                                static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE>(args->Mode));
        l4->Release();
    }
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
    if (table_index != 1) return;                       // the compute table keeps its rejections
    t->pfnResourceResolveSubresource = resolve_subresource;
    t->pfnResourceResolveSubresourceRegion = resolve_subresource_region;
    t->pfnOMSetDepthBounds = om_set_depth_bounds;
    t->pfnSetSamplePositions = set_sample_positions;
    t->pfnSetViewInstanceMask = set_view_instance_mask;
    t->pfnRSSetShadingRate = rs_set_shading_rate;
    t->pfnRSSetShadingRateImage = rs_set_shading_rate_image;
    t->pfnOmSetAlphaBlendFactor = om_set_alpha_blend_factor;
}

} // namespace engine_ddi
