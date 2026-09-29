// SPDX-License-Identifier: MIT
// engine-ddi: descriptor heaps, the views and samplers written into them, descriptor copies, and the list slots
// that use them (ClearRenderTargetView, ClearDepthStencilView, SetDescriptorHeaps).
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
static_assert(D3D12DDI_BUFFER_SRV_FLAG_RAW == static_cast<int>(D3D12_BUFFER_SRV_FLAG_RAW), "buffer SRV flags");
static_assert(D3D12DDI_DEFAULT_SHADER_4_COMPONENT_MAPPING == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
              "component mapping");
static_assert(D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_DEPTH == static_cast<int>(D3D12_DSV_FLAG_READ_ONLY_DEPTH) &&
              D3D12DDI_CREATE_DSV_FLAG_READ_ONLY_STENCIL == static_cast<int>(D3D12_DSV_FLAG_READ_ONLY_STENCIL),
              "DSV flags");
static_assert(D3D12DDI_TEXTURE_ADDRESS_MODE_MIRRORONCE == static_cast<int>(D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE),
              "sampler address modes");

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
    if (!r) {
        c->report(E_INVALIDARG);
        return {};
    }
    // A heap that is not shader visible has no GPU handle: the answer is zero and no error. The runtime was
    // seen asking this of a render target view heap.
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

void APIENTRY create_srv(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002* args,
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
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = args->Format;
    d.Shader4ComponentMapping = args->Shader4ComponentMapping;
    const bool ms = r && r->desc.SampleDesc.Count > 1;
    switch (args->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Buffer = {args->Buffer.FirstElement, args->Buffer.NumElements, args->Buffer.StructureByteStride,
                    static_cast<D3D12_BUFFER_SRV_FLAGS>(args->Buffer.Flags)};
        break;
    case D3D12DDI_RD_TEXTURE1D:
        if (r && arrayed(r)) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {args->Tex1D.MostDetailedMip, args->Tex1D.MipLevels, args->Tex1D.FirstArraySlice,
                                args->Tex1D.ArraySize, args->Tex1D.ResourceMinLODClamp};
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            d.Texture1D = {args->Tex1D.MostDetailedMip, args->Tex1D.MipLevels, args->Tex1D.ResourceMinLODClamp};
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (ms && r && arrayed(r)) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        } else if (ms) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else if (r && arrayed(r)) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {args->Tex2D.MostDetailedMip, args->Tex2D.MipLevels, args->Tex2D.FirstArraySlice,
                                args->Tex2D.ArraySize, args->Tex2D.PlaneSlice, args->Tex2D.ResourceMinLODClamp};
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D = {args->Tex2D.MostDetailedMip, args->Tex2D.MipLevels, args->Tex2D.PlaneSlice,
                           args->Tex2D.ResourceMinLODClamp};
        }
        break;
    case D3D12DDI_RD_TEXTURE3D:
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D = {args->Tex3D.MostDetailedMip, args->Tex3D.MipLevels, args->Tex3D.ResourceMinLODClamp};
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        // INFERENCE, as for arrayed(): more than one cube in the resource selects the cube array view.
        if (r && r->desc.DepthOrArraySize > 6) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            d.TextureCubeArray = {args->TexCube.MostDetailedMip, args->TexCube.MipLevels, args->TexCube.First2DArrayFace,
                                  args->TexCube.NumCubes, args->TexCube.ResourceMinLODClamp};
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            d.TextureCube = {args->TexCube.MostDetailedMip, args->TexCube.MipLevels, args->TexCube.ResourceMinLODClamp};
        }
        break;
    case D3D12DDI_RD_RAYTRACING_ACCELERATION_STRUCTURE_0042:
        if (r) {
            c->report(E_INVALIDARG);                    // the structure is named by its address, not a resource
            return;
        }
        d.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        d.RaytracingAccelerationStructure.Location = args->RaytracingAccelerationStructure.Location;
        break;
    default:
        c->report(E_INVALIDARG);
        return;
    }
    c->device->CreateShaderResourceView(r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr, &d,
                                        D3D12_CPU_DESCRIPTOR_HANDLE{dest.ptr});
}

void APIENTRY create_dsv(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW* args,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!args || !dest.ptr || (args->Flags & ~D3D12DDI_CREATE_DSV_FLAG_MASK)) {
        c->report(E_INVALIDARG);
        return;
    }
    ResourceRecord* r = view_resource(c, args->hDrvResource);
    if (args->hDrvResource.pDrvPrivate && !r) {
        c->report(E_INVALIDARG);
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = args->Format;
    d.Flags = static_cast<D3D12_DSV_FLAGS>(args->Flags);
    const bool ms = r && r->desc.SampleDesc.Count > 1;
    switch (args->ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D:
        if (r && arrayed(r)) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray = {args->Tex1D.MipSlice, args->Tex1D.FirstArraySlice, args->Tex1D.ArraySize};
        } else {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
            d.Texture1D = {args->Tex1D.MipSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURE2D:
        if (ms && r && arrayed(r)) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            d.Texture2DMSArray = {args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        } else if (ms) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        } else if (r && arrayed(r)) {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {args->Tex2D.MipSlice, args->Tex2D.FirstArraySlice, args->Tex2D.ArraySize};
        } else {
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            d.Texture2D = {args->Tex2D.MipSlice};
        }
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {args->TexCube.MipSlice, args->TexCube.FirstArraySlice, args->TexCube.ArraySize};
        break;
    default:
        c->report(E_INVALIDARG);
        return;
    }
    c->device->CreateDepthStencilView(r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr, &d,
                                      D3D12_CPU_DESCRIPTOR_HANDLE{dest.ptr});
}

void APIENTRY create_cbv(D3D12DDI_HDEVICE device, const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC* args,
                         D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!args || !dest.ptr) {
        c->report(E_INVALIDARG);
        return;
    }
    const D3D12_CONSTANT_BUFFER_VIEW_DESC d{args->BufferLocation, args->SizeInBytes};
    c->device->CreateConstantBufferView(&d, D3D12_CPU_DESCRIPTOR_HANDLE{dest.ptr});
}

void APIENTRY create_sampler(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_SAMPLER* args,
                             D3D12DDI_CPU_DESCRIPTOR_HANDLE dest) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!args || !args->pSamplerDesc || !dest.ptr) {
        c->report(E_INVALIDARG);
        return;
    }
    const D3D12DDI_SAMPLER_DESC& s = *args->pSamplerDesc;
    D3D12_SAMPLER_DESC d{};
    d.Filter = static_cast<D3D12_FILTER>(s.Filter);
    d.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.AddressU);
    d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.AddressV);
    d.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.AddressW);
    d.MipLODBias = s.MipLODBias;
    d.MaxAnisotropy = s.MaxAnisotropy;
    d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(s.ComparisonFunc);
    std::memcpy(d.BorderColor, s.BorderColor, sizeof(d.BorderColor));
    d.MinLOD = s.MinLOD;
    d.MaxLOD = s.MaxLOD;
    c->device->CreateSampler(&d, D3D12_CPU_DESCRIPTOR_HANDLE{dest.ptr});
}

// Handles are the engine's own values and both handle types are one pointer-sized member (asserted above), so the
// range arrays pass through as they are.
void APIENTRY copy_descriptors(D3D12DDI_HDEVICE device, UINT dst_count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dst,
                               const UINT* dst_sizes, UINT src_count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* src,
                               const UINT* src_sizes, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if ((dst_count && !dst) || (src_count && !src) || type < 0 || type >= D3D12DDI_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) {
        c->report(E_INVALIDARG);
        return;
    }
    c->device->CopyDescriptors(dst_count, reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(dst), dst_sizes,
                               src_count, reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(src), src_sizes,
                               static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
}

void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE device, UINT count, D3D12DDI_CPU_DESCRIPTOR_HANDLE dst,
                                      D3D12DDI_CPU_DESCRIPTOR_HANDLE src, D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if ((count && (!dst.ptr || !src.ptr)) || type < 0 || type >= D3D12DDI_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) {
        c->report(E_INVALIDARG);
        return;
    }
    c->device->CopyDescriptorsSimple(count, D3D12_CPU_DESCRIPTOR_HANDLE{dst.ptr}, D3D12_CPU_DESCRIPTOR_HANDLE{src.ptr},
                                     static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
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

// The DDI passes the clear flags as a plain UINT. INFERENCE: the bits are the API's (depth 1, stencil 2); any
// other bit, or none, is refused rather than guessed.
void APIENTRY clear_dsv(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_CPU_DESCRIPTOR_HANDLE view, UINT flags, FLOAT depth,
                        UINT8 stencil, UINT rect_count, const D3D12DDI_RECT* rects) {
    CommandListRecord* l = list_of(hlist, "ClearDepthStencilView");
    if (!l || reject_in_compute_table(l)) return;
    constexpr UINT known = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
    if (!view.ptr || !flags || (flags & ~known) || (rect_count && !rects)) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE{view.ptr}, static_cast<D3D12_CLEAR_FLAGS>(flags), depth,
                                     stencil, rect_count, rects);
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
    t->pfnCreateShaderResourceView = create_srv;
    t->pfnCreateDepthStencilView = create_dsv;
    t->pfnCreateConstantBufferView = create_cbv;
    t->pfnCreateSampler = create_sampler;
    t->pfnCopyDescriptors = copy_descriptors;
    t->pfnCopyDescriptorsSimple = copy_descriptors_simple;
}

void fill_list_descriptors(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t table_index) noexcept {
    t->pfnSetDescriptorHeaps = set_descriptor_heaps;
    if (table_index == 1) {                                          // the compute table keeps its rejection
        t->pfnClearRenderTargetView = clear_rtv;
        t->pfnClearDepthStencilView = clear_dsv;
    }
}

} // namespace engine_ddi
