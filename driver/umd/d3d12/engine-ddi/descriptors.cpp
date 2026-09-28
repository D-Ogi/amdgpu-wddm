// SPDX-License-Identifier: MIT
// engine-ddi: descriptor heaps (D42-D47), UAV and RTV descriptors (D51, D52), and the list slots that use them
// (L7 ClearRenderTargetView, L30 SetDescriptorHeaps).
//
// Descriptor handles are the engine's, one to one: the runtime computes start + index * increment from what
// GetCPU/GPUDescriptorHandleForHeapStart and GetDescriptorSizeInBytes return, and engine-ddi hands the result
// back to the engine unchanged.
#include "internal.h"

namespace engine_ddi {

static_assert(D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV == static_cast<int>(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) &&
              D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER == static_cast<int>(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) &&
              D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV == static_cast<int>(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) &&
              D3D12DDI_DESCRIPTOR_HEAP_TYPE_DSV == static_cast<int>(D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
              D3D12DDI_DESCRIPTOR_HEAP_TYPE_NUM_TYPES == static_cast<int>(D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES),
              "descriptor heap types");
static_assert(sizeof(D3D12DDI_CPU_DESCRIPTOR_HANDLE) == sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) &&
              sizeof(D3D12DDI_GPU_DESCRIPTOR_HANDLE) == sizeof(D3D12_GPU_DESCRIPTOR_HANDLE), "descriptor handles");
static_assert(D3D12DDI_BUFFER_UAV_FLAG_RAW == static_cast<int>(D3D12_BUFFER_UAV_FLAG_RAW), "buffer UAV flags");

namespace {
SIZE_T APIENTRY calc_heap(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001*) {
    return sizeof(DescriptorHeapRecord);
}

HRESULT APIENTRY create_heap(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001* args,
                             D3D12DDI_HDESCRIPTORHEAP h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate) return E_INVALIDARG;
    if (args->Type >= D3D12DDI_DESCRIPTOR_HEAP_TYPE_NUM_TYPES || !args->NumDescriptors || args->NodeMask > 1 ||
        (args->Flags & ~(D3D12DDI_DESCRIPTOR_HEAP_FLAG_CPU_VISIBLE | D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)))
        return E_INVALIDARG;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(args->Type);
    desc.NumDescriptors = args->NumDescriptors;
    desc.Flags = (args->Flags & D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                                              : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* heap = nullptr;
    HRESULT hr = c->device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
    if (FAILED(hr)) return hr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) gpu = heap->GetGPUDescriptorHandleForHeapStart();
    new (h.pDrvPrivate) DescriptorHeapRecord{{Tag::DescriptorHeap, 0, heap, c}, desc.Type, desc.NumDescriptors,
                                             heap->GetCPUDescriptorHandleForHeapStart(), gpu};
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_heap(D3D12DDI_HDEVICE device, D3D12DDI_HDESCRIPTORHEAP h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<DescriptorHeapRecord>(h.pDrvPrivate, Tag::DescriptorHeap, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}

UINT APIENTRY descriptor_size(D3D12DDI_HDEVICE device, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    DeviceContext* c = resolve(device);
    if (!c) return 0;
    if (type < 0 || type >= D3D12DDI_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) {
        c->report(E_INVALIDARG);
        return 0;
    }
    return c->increments[type];
}

D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY cpu_start(D3D12DDI_HDEVICE device, D3D12DDI_HDESCRIPTORHEAP h) {
    DeviceContext* c = resolve(device);
    if (!c) return {};
    auto* r = record_of<DescriptorHeapRecord>(h.pDrvPrivate, Tag::DescriptorHeap, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return {};
    }
    return {r->cpu.ptr};
}

D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY gpu_start(D3D12DDI_HDEVICE device, D3D12DDI_HDESCRIPTORHEAP h) {
    DeviceContext* c = resolve(device);
    if (!c) return {};
    auto* r = record_of<DescriptorHeapRecord>(h.pDrvPrivate, Tag::DescriptorHeap, c);
    if (!r || !r->gpu.ptr) {
        c->report(E_INVALIDARG);                        // unknown heap, or not shader visible
        return {};
    }
    return {r->gpu.ptr};
}

ResourceRecord* view_resource(DeviceContext* c, D3D12DDI_HRESOURCE h) noexcept {
    return h.pDrvPrivate ? record_of<ResourceRecord>(h.pDrvPrivate, Tag::Resource, c) : nullptr;
}

// The DDI folds TEXTUREnD and TEXTUREnDARRAY into one dimension; the resource's array size picks the view type
// (INFERENCE: the runtime's view of a one-slice array is indistinguishable from a plain view).
bool arrayed(const ResourceRecord* r) noexcept {
    return r->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D && r->desc.DepthOrArraySize > 1;
}

void APIENTRY create_uav(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002* args,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!args || !dest.ptr) {
        c->report(E_INVALIDARG);
        return;
    }
    ResourceRecord* r = view_resource(c, args->hDrvResource);
    if (args->hDrvResource.pDrvPrivate && !r) {
        c->report(E_INVALIDARG);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = args->Format;
    ID3D12Resource* counter = nullptr;
    switch (args->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.FirstElement = args->Buffer.FirstElement;
        d.Buffer.NumElements = args->Buffer.NumElements;
        d.Buffer.StructureByteStride = args->Buffer.StructureByteStride;
        d.Buffer.CounterOffsetInBytes = args->Buffer.CounterOffsetInBytes;
        d.Buffer.Flags = static_cast<D3D12_BUFFER_UAV_FLAGS>(args->Buffer.Flags);
        if (args->Buffer.hDrvCounterResource.pDrvPrivate) {
            ResourceRecord* cr = view_resource(c, args->Buffer.hDrvCounterResource);
            if (!cr) {
                c->report(E_INVALIDARG);
                return;
            }
            counter = static_cast<ID3D12Resource*>(cr->h.engine);
        }
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (r && arrayed(r)) {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {args->Tex1D.MipSlice, args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            d.Texture1D = {args->Tex1D.MipSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (r && arrayed(r)) {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {args->Tex2D.MipSlice, args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize,
                                args->Tex2D.PlaneSlice};
        } else {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D = {args->Tex2D.MipSlice, args->Tex2D.PlaneSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D = {args->Tex3D.MipSlice, args->Tex3D.FirstW, args->Tex3D.WSize};
        break;
    default:
        c->report(E_INVALIDARG);
        return;
    }
    c->device->CreateUnorderedAccessView(r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr, counter, &d,
                                         D3D12_CPU_DESCRIPTOR_HANDLE{dest.ptr});
}

void APIENTRY create_rtv(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002* args,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!args || !dest.ptr) {
        c->report(E_INVALIDARG);
        return;
    }
    ResourceRecord* r = view_resource(c, args->hDrvResource);
    if (args->hDrvResource.pDrvPrivate && !r) {
        c->report(E_INVALIDARG);
        return;
    }
    D3D12_RENDER_TARGET_VIEW_DESC d{};
    d.Format = args->Format;
    const bool ms = r && r->desc.SampleDesc.Count > 1;
    switch (args->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        d.ViewDimension = D3D12_RTV_DIMENSION_BUFFER;
        d.Buffer = {args->Buffer.FirstElement, args->Buffer.NumElements};
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (r && arrayed(r)) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {args->Tex1D.MipSlice, args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            d.Texture1D = {args->Tex1D.MipSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (ms && r && arrayed(r)) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        } else if (ms) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        } else if (r && arrayed(r)) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {args->Tex2D.MipSlice, args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize,
                                args->Tex2D.PlaneSlice};
        } else {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            d.Texture2D = {args->Tex2D.MipSlice, args->Tex2D.PlaneSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D = {args->Tex3D.MipSlice, args->Tex3D.FirstW, args->Tex3D.WSize};
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {args->TexCube.MipSlice, args->TexCube.FirstArraySlice, args->TexCube.ArraySize, 0};
        break;
    default:
        c->report(E_INVALIDARG);
        return;
    }
    c->device->CreateRenderTargetView(r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr, &d,
                                      D3D12_CPU_DESCRIPTOR_HANDLE{dest.ptr});
}

// ---- Command-list slots -----------------------------------------------------------------------------------------
void APIENTRY set_descriptor_heaps(D3D12DDI_HCOMMANDLIST hlist, UINT count, D3D12DDI_HDESCRIPTORHEAP* heaps) {
    CommandListRecord* l = list_of(hlist, "SetDescriptorHeaps");
    if (!l) return;
    ID3D12DescriptorHeap* engine[2] = {};
    if (count > 2 || (count && !heaps)) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        auto* r = record_of<DescriptorHeapRecord>(heaps[i].pDrvPrivate, Tag::DescriptorHeap, l->h.device);
        if (!r) {
            l->h.device->report_list(l->rt, E_INVALIDARG);
            return;
        }
        engine[i] = static_cast<ID3D12DescriptorHeap*>(r->h.engine);
    }
    l->list()->SetDescriptorHeaps(count, engine);
}

void APIENTRY clear_rtv(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4],
                        UINT rect_count, const D3D12DDI_RECT* rects) {
    CommandListRecord* l = list_of(hlist, "ClearRenderTargetView");
    if (!l || reject_in_compute_table(l)) return;
    if (!view.ptr || !color || (rect_count && !rects)) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE{view.ptr}, color, rect_count, rects);
}
} // namespace

void fill_core_descriptors(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateDescriptorHeapSize = calc_heap;
    t->pfnCreateDescriptorHeap = create_heap;
    t->pfnDestroyDescriptorHeap = destroy_heap;
    t->pfnGetDescriptorSizeInBytes = descriptor_size;
    t->pfnGetCPUDescriptorHandleForHeapStart = cpu_start;
    t->pfnGetGPUDescriptorHandleForHeapStart = gpu_start;
    t->pfnCreateUnorderedAccessView = create_uav;
    t->pfnCreateRenderTargetView = create_rtv;
}

void fill_list_descriptors(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t table_index) noexcept {
    t->pfnSetDescriptorHeaps = set_descriptor_heaps;
    if (table_index == 1) t->pfnClearRenderTargetView = clear_rtv;   // the compute table keeps its rejection
}

} // namespace engine_ddi
