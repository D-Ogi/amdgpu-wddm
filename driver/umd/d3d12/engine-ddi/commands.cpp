// SPDX-License-Identifier: MIT
// engine-ddi: command pools (D29-D32), command recorders (D92-D95), command lists (D36-D38) and the list slots
// that open and close recording (L0, L1).
//
// DDI 0040 splits the API's allocator and list: a pool is the API allocator, a recorder names the pool a list
// records into, and ResetCommandList names the recorder. A pool therefore does not know its list type at
// creation; it creates one engine allocator per list type on first use.
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
        CommandListRecord{{Tag::CommandList, 0, list, c}, rt, static_cast<D3D12_COMMAND_LIST_TYPE>(type), table};
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
    if (FAILED(hr)) l->h.device->report_list(l->rt, hr);
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
}

// ---- List state the runtime sets on every list ---------------------------------------------------------------------
// The runtime writes its default state into a list it has reset, through these slots among others, whatever
// the caps say about the feature. Each forwards to the engine list, which owns the feature check.
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

void fill_core_commands(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
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
}

void fill_list_commands(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t) noexcept {
    t->pfnCloseCommandList = close_list;
    t->pfnResetCommandList = reset_list;
    t->pfnSetPredication = set_predication;
    t->pfnOMSetDepthBounds = om_set_depth_bounds;
    t->pfnSetSamplePositions = set_sample_positions;
    t->pfnSetViewInstanceMask = set_view_instance_mask;
    t->pfnSetProtectedResourceSession = set_protected_session;
    t->pfnRSSetShadingRate = rs_set_shading_rate;
    t->pfnRSSetShadingRateImage = rs_set_shading_rate_image;
}

} // namespace engine_ddi
