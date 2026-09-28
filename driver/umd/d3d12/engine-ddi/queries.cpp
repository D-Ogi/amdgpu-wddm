// SPDX-License-Identifier: MIT
// engine-ddi: query heaps (D68-D70) and the query slots of a command list (L20-L22).
#include "internal.h"

namespace engine_ddi {

static_assert(D3D12DDI_QUERY_HEAP_TYPE_OCCLUSION == static_cast<int>(D3D12_QUERY_HEAP_TYPE_OCCLUSION) &&
              D3D12DDI_QUERY_HEAP_TYPE_TIMESTAMP == static_cast<int>(D3D12_QUERY_HEAP_TYPE_TIMESTAMP) &&
              D3D12DDI_QUERY_HEAP_TYPE_PIPELINE_STATISTICS == static_cast<int>(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS) &&
              D3D12DDI_QUERY_HEAP_TYPE_SO_STATISTICS == static_cast<int>(D3D12_QUERY_HEAP_TYPE_SO_STATISTICS) &&
              D3D12DDI_QUERY_HEAP_TYPE_0032_COPY_QUEUE_TIMESTAMP ==
                  static_cast<int>(D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP) &&
              D3D12DDI_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1 ==
                  static_cast<int>(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1),
              "query heap types");
static_assert(D3D12DDI_QUERY_TYPE_OCCLUSION == static_cast<int>(D3D12_QUERY_TYPE_OCCLUSION) &&
              D3D12DDI_QUERY_TYPE_BINARY_OCCLUSION == static_cast<int>(D3D12_QUERY_TYPE_BINARY_OCCLUSION) &&
              D3D12DDI_QUERY_TYPE_TIMESTAMP == static_cast<int>(D3D12_QUERY_TYPE_TIMESTAMP) &&
              D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS == static_cast<int>(D3D12_QUERY_TYPE_PIPELINE_STATISTICS) &&
              D3D12DDI_QUERY_TYPE_SO_STATISTICS_STREAM0 == static_cast<int>(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0) &&
              D3D12DDI_QUERY_TYPE_SO_STATISTICS_STREAM3 == static_cast<int>(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) &&
              D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS1 == static_cast<int>(D3D12_QUERY_TYPE_PIPELINE_STATISTICS1),
              "query types");

namespace {
bool supported_heap_type(D3D12DDI_QUERY_HEAP_TYPE type) noexcept {
    switch (type) {
    case D3D12DDI_QUERY_HEAP_TYPE_OCCLUSION:
    case D3D12DDI_QUERY_HEAP_TYPE_TIMESTAMP:
    case D3D12DDI_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
    case D3D12DDI_QUERY_HEAP_TYPE_SO_STATISTICS:
    case D3D12DDI_QUERY_HEAP_TYPE_0032_COPY_QUEUE_TIMESTAMP:
    case D3D12DDI_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1:
        return true;
    default:
        return false;                                   // video decode statistics: no engine support
    }
}

SIZE_T APIENTRY calc_query_heap(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_QUERY_HEAP_0001*) {
    return sizeof(QueryHeapRecord);
}

HRESULT APIENTRY create_query_heap(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_QUERY_HEAP_0001* args,
                                   D3D12DDI_HQUERYHEAP h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || !args->Count || args->NodeMask > 1) return E_INVALIDARG;
    if (!supported_heap_type(args->Type)) return E_NOTIMPL;
    D3D12_QUERY_HEAP_DESC desc{static_cast<D3D12_QUERY_HEAP_TYPE>(args->Type), args->Count, 0};
    ID3D12QueryHeap* heap = nullptr;
    HRESULT hr = c->device->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&heap));
    if (FAILED(hr)) return hr;
    new (h.pDrvPrivate) QueryHeapRecord{{Tag::QueryHeap, 0, heap, c}, desc.Type, desc.Count};
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_query_heap(D3D12DDI_HDEVICE device, D3D12DDI_HQUERYHEAP h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<QueryHeapRecord>(h.pDrvPrivate, Tag::QueryHeap, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}

// The heap of a query slot, checked against the index range.
QueryHeapRecord* query_heap(CommandListRecord* l, D3D12DDI_HQUERYHEAP h, UINT first, UINT count) noexcept {
    auto* r = record_of<QueryHeapRecord>(h.pDrvPrivate, Tag::QueryHeap, l->h.device);
    if (!r || first >= r->count || count > r->count - first) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return nullptr;
    }
    return r;
}

void APIENTRY begin_query(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HQUERYHEAP h, D3D12DDI_QUERY_TYPE type, UINT index) {
    CommandListRecord* l = list_of(hlist, "BeginQuery");
    if (!l) return;
    if (QueryHeapRecord* r = query_heap(l, h, index, 1))
        l->list()->BeginQuery(static_cast<ID3D12QueryHeap*>(r->h.engine), static_cast<D3D12_QUERY_TYPE>(type), index);
}

void APIENTRY end_query(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HQUERYHEAP h, D3D12DDI_QUERY_TYPE type, UINT index) {
    CommandListRecord* l = list_of(hlist, "EndQuery");
    if (!l) return;
    if (QueryHeapRecord* r = query_heap(l, h, index, 1))
        l->list()->EndQuery(static_cast<ID3D12QueryHeap*>(r->h.engine), static_cast<D3D12_QUERY_TYPE>(type), index);
}

void APIENTRY resolve_query_data(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HQUERYHEAP h, D3D12DDI_QUERY_TYPE type,
                                 UINT first, UINT count, D3D12DDI_HRESOURCE dst, UINT64 offset) {
    CommandListRecord* l = list_of(hlist, "ResolveQueryData");
    if (!l) return;
    QueryHeapRecord* r = query_heap(l, h, first, count);
    if (!r) return;
    auto* d = record_of<ResourceRecord>(dst.pDrvPrivate, Tag::Resource, l->h.device);
    if (!d) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->ResolveQueryData(static_cast<ID3D12QueryHeap*>(r->h.engine), static_cast<D3D12_QUERY_TYPE>(type), first,
                                count, static_cast<ID3D12Resource*>(d->h.engine), offset);
}
} // namespace

void fill_core_queries(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateQueryHeapSize = calc_query_heap;
    t->pfnCreateQueryHeap = create_query_heap;
    t->pfnDestroyQueryHeap = destroy_query_heap;
}

void fill_list_queries(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t) noexcept {
    t->pfnBeginQuery = begin_query;
    t->pfnEndQuery = end_query;
    t->pfnResolveQueryData = resolve_query_data;
}

} // namespace engine_ddi
