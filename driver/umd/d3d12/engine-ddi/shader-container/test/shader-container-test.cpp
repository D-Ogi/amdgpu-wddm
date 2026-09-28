// SPDX-License-Identifier: MIT
// Offline control for shader-container, on the development PC through the Vulkan loader (no lab, no window).
//   1. Original containers: fxc (shader model 5) and dxc (shader model 6, DXIL, stems "dxil.*") output of
//      test/hlsl, compiled by build-run.ps1 with the Windows SDK compilers.
//   2. Reconstruct: each container is stripped to what the D3D12 DDI gives, the program (SHEX/SHDR tokens or the
//      DXIL part) and D3D12DDIARG_SIGNATURE_ENTRY_0012 arrays without names, and rebuilt with BuildContainer.
//      Input layouts and stream-output declarations go the same way: to their DDI form (registers), then back to
//      semantics.
//   3. Compare: both containers go through the vkd3d-proton engine's ID3D12Device. Rendered pixels,
//      stream-output buffers and UAV contents must be byte-identical. Two controls map a register wrongly and
//      must be detected; the unsupported cases must be refused.
// The DDI form of a signature is this test's model of the runtime (see README.md); it is not measured.
//
// Usage: shader-container-test <amdgpu_wddm_vkd3d.dll> <cso directory> <output directory> [adapter substring]
//            [--so-engine <amdgpu_wddm_vkd3d.dll>]
// --so-engine runs the stream-output cases on a second engine that accepts holes in the declaration (NULL
// SemanticName); without it they run on the first engine with the holes left out of both variants.

#define D3D12_TOKENIZED_PROGRAM_FORMAT_HEADER

#include <windows.h>
#include <d3d12.h>
#include <d3d12umddi.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>

#include "bc250_vkd3d_engine.h"
#include "../dxil-metadata.h"
#include "../shader-container.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace sc = engine_ddi::shader_container;
namespace dxil = engine_ddi::shader_container::dxil;
using Entry = D3D12DDIARG_SIGNATURE_ENTRY_0012;
using Bytes = std::vector<uint8_t>;

namespace {

unsigned g_checks;
unsigned g_failures;
std::string g_out;

bool Check(bool ok, const std::string& what)
{
    g_checks++;
    if (!ok)
        g_failures++;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    return ok;
}

std::string Hex(HRESULT hr)
{
    char s[16];
    snprintf(s, sizeof(s), "0x%08lX", static_cast<unsigned long>(hr));
    return s;
}

std::string Sha256(const Bytes& b)
{
    uint8_t h[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(b.data()), ULONG(b.size()), h, 32);
    std::string s;
    char x[3];
    for (uint8_t v : h)
    {
        snprintf(x, sizeof(x), "%02X", v);
        s += x;
    }
    return s;
}

bool ReadFile(const std::string& path, Bytes* bytes)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    bytes->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void WriteFile(const std::string& name, const Bytes& bytes)
{
    std::ofstream f(g_out + "\\" + name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

uint32_t Rd32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// ---- Container reading (test side) --------------------------------------------------------------------------

struct Chunk
{
    std::string tag;
    const uint8_t* data;    // after the chunk header
    uint32_t size;
};

bool ParseContainer(const Bytes& b, std::vector<Chunk>* chunks)
{
    chunks->clear();
    if (b.size() < 32 || std::memcmp(b.data(), "DXBC", 4) || Rd32(&b[24]) != b.size())
        return false;
    uint32_t count = Rd32(&b[28]);
    if (32ull + 4ull * count > b.size())
        return false;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t off = Rd32(&b[32 + 4 * i]);
        if (off + 8ull > b.size())
            return false;
        uint32_t size = Rd32(&b[off + 4]);
        if (off + 8ull + size > b.size())
            return false;
        chunks->push_back({ std::string(reinterpret_cast<const char*>(&b[off]), 4), &b[off + 8], size });
    }
    return true;
}

const Chunk* FindChunk(const std::vector<Chunk>& chunks, std::initializer_list<const char*> tags)
{
    for (const char* tag : tags)
        for (const Chunk& c : chunks)
            if (c.tag == tag)
                return &c;
    return nullptr;
}

struct SigElement
{
    std::string name;
    uint32_t index, sysval, type, reg, mask, rw, stream, precision;
};

bool ParseSignature(const Chunk& c, std::vector<SigElement>* out)
{
    out->clear();
    bool v1 = c.tag == "ISG1" || c.tag == "OSG1" || c.tag == "PSG1";
    bool hasStream = v1 || c.tag == "OSG5";
    uint32_t stride = v1 ? 32 : hasStream ? 28 : 24;
    if (c.size < 8)
        return false;
    uint32_t count = Rd32(c.data);
    if (8ull + uint64_t(count) * stride > c.size)
        return false;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* p = c.data + 8 + i * stride;
        SigElement e = {};
        if (hasStream)
        {
            e.stream = Rd32(p);
            p += 4;
        }
        uint32_t nameOffset = Rd32(p);
        e.index = Rd32(p + 4);
        e.sysval = Rd32(p + 8);
        e.type = Rd32(p + 12);
        e.reg = Rd32(p + 16);
        uint32_t mask = Rd32(p + 20);
        e.mask = mask & 0xff;
        e.rw = (mask >> 8) & 0xff;
        e.precision = v1 ? Rd32(p + 24) : 0;
        if (nameOffset >= c.size)
            return false;
        size_t len = strnlen(reinterpret_cast<const char*>(c.data + nameOffset), c.size - nameOffset);
        if (nameOffset + len >= c.size)
            return false;
        e.name.assign(reinterpret_cast<const char*>(c.data + nameOffset), len);
        out->push_back(e);
    }
    return true;
}

// ---- The DDI form (this test's model of the runtime's conversion) --------------------------------------------

// D3D_NAME of a signature element to D3D10_SB_NAME: tessellation factors split by semantic index; names without
// a tokenized-format equivalent (SV_Target, SV_Depth, SV_Coverage, ...) become UNDEFINED.
D3D10_SB_NAME ToSbName(uint32_t d3dName, uint32_t index)
{
    switch (d3dName)
    {
    case D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR:   return D3D10_SB_NAME(D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR + index);
    case D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR: return D3D10_SB_NAME(D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR + index);
    case D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR:    return D3D10_SB_NAME(D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR + index);
    case D3D_NAME_FINAL_TRI_INSIDE_TESSFACTOR:  return D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR;
    case D3D_NAME_FINAL_LINE_DETAIL_TESSFACTOR: return D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR;
    case D3D_NAME_FINAL_LINE_DENSITY_TESSFACTOR: return D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR;
    default:
        if (d3dName <= uint32_t(D3D_NAME_SAMPLE_INDEX)
                || (d3dName >= uint32_t(D3D_NAME_BARYCENTRICS) && d3dName <= uint32_t(D3D_NAME_CULLPRIMITIVE)))
            return D3D10_SB_NAME(d3dName);
        return D3D10_SB_NAME_UNDEFINED;
    }
}

std::vector<Entry> ToDdi(const std::vector<SigElement>& sig)
{
    std::vector<Entry> entries;
    for (const SigElement& e : sig)
    {
        Entry d = {};
        d.SystemValue = ToSbName(e.sysval, e.index);
        d.Register = e.reg;
        d.Mask = BYTE(e.mask & 0xf);
        d.Stream = BYTE(e.stream);
        d.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_TYPE(e.type);
        d.MinPrecision = D3D11_SB_OPERAND_MIN_PRECISION(e.precision);
        entries.push_back(d);
    }
    return entries;
}

struct DdiShader
{
    std::vector<UINT> code;
    std::vector<Entry> input, output, patch;
    std::vector<SigElement> origInput, origOutput, origPatch;
};

bool Strip(const Bytes& container, DdiShader* s, std::string* why)
{
    std::vector<Chunk> chunks;
    if (!ParseContainer(container, &chunks))
        return *why = "container does not parse", false;

    // The DXIL part has the SizeInUint32 of its program header where the tokens have LenTok.
    const Chunk* code = FindChunk(chunks, { "SHEX", "SHDR", "DXIL" });
    if (!code || code->size < 8 || code->size % 4 || Rd32(code->data + 4) * 4ull != code->size)
        return *why = "no program chunk, or LenTok disagrees with the chunk size", false;
    s->code.resize(code->size / 4);
    std::memcpy(s->code.data(), code->data, code->size);

    struct { std::initializer_list<const char*> tags; std::vector<SigElement>* orig; std::vector<Entry>* ddi; } sigs[] = {
        { { "ISG1", "ISGN" }, &s->origInput, &s->input },
        { { "OSG1", "OSG5", "OSGN" }, &s->origOutput, &s->output },
        { { "PSG1", "PCSG" }, &s->origPatch, &s->patch },
    };
    for (auto& sig : sigs)
    {
        sig.orig->clear();
        if (const Chunk* c = FindChunk(chunks, sig.tags))
            if (!ParseSignature(*c, sig.orig))
                return *why = "signature chunk " + c->tag + " does not parse", false;
        *sig.ddi = ToDdi(*sig.orig);
    }
    return true;
}

sc::ProgramDesc Desc(const DdiShader& s)
{
    sc::ProgramDesc d;
    d.code = s.code.data();
    d.codeCapacity = s.code.size();
    d.input = { s.input.data(), UINT(s.input.size()) };
    d.output = { s.output.data(), UINT(s.output.size()) };
    d.patchConstant = { s.patch.data(), UINT(s.patch.size()) };
    return d;
}

bool SameEntry(const Entry& a, const Entry& b)
{
    return a.SystemValue == b.SystemValue && a.Register == b.Register && a.Mask == b.Mask && a.Stream == b.Stream
        && a.RegisterComponentType == b.RegisterComponentType && a.MinPrecision == b.MinPrecision;
}

std::vector<Entry> Registered(const std::vector<Entry>& v)
{
    std::vector<Entry> r;
    for (const Entry& e : v)
        if (e.Register != ~0u)
            r.push_back(e);
    return r;
}

bool SameEntries(const std::vector<Entry>& a, const std::vector<Entry>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!SameEntry(a[i], b[i]))
            return false;
    return true;
}

// ---- The engine -------------------------------------------------------------------------------------------------

struct Engine
{
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    LUID luid = {};
    BC250_VKD3D_ENGINE_FUNCS funcs = {};
    bool soGaps = false;    // takes holes (NULL SemanticName) in stream-output declarations
};

HRESULT CreateEngineDevice(const Engine& e, ComPtr<ID3D12Device>* device)
{
    BC250_VKD3D_DEVICE_CREATE_INFO info = {};
    info.Size = sizeof(info);
    info.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    info.GetInstanceProcAddr = e.gipa;
    info.AdapterLuid = e.luid;
    info.MinimumFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    info.QueueMode = BC250_VKD3D_QUEUE_MODE_THREADED;
    return e.funcs.CreateDevice(&info, __uuidof(ID3D12Device), reinterpret_cast<void**>(device->ReleaseAndGetAddressOf()));
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    bool soGaps = false;

    ~Gpu()
    {
        if (event)
            CloseHandle(event);
    }

    HRESULT Init(const Engine& e)
    {
        HRESULT hr = CreateEngineDevice(e, &device);
        if (FAILED(hr))
            return hr;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))
                || FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
                || FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                        IID_PPV_ARGS(&list)))
                || FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return hr;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event ? S_OK : E_FAIL;
    }

    HRESULT Execute()
    {
        HRESULT hr = list->Close();
        if (FAILED(hr))
            return hr;
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(hr = queue->Signal(fence.Get(), ++value)) || FAILED(hr = fence->SetEventOnCompletion(value, event)))
            return hr;
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if (FAILED(hr = device->GetDeviceRemovedReason()))
            return hr;
        if (FAILED(hr = allocator->Reset()))
            return hr;
        return list->Reset(allocator.Get(), nullptr);
    }
};

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* d, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* d, DXGI_FORMAT format, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

ComPtr<ID3D12Resource> Upload(ID3D12Device* d, const void* data, size_t size)
{
    ComPtr<ID3D12Resource> r = MakeBuffer(d, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr;
    if (!r || FAILED(r->Map(0, nullptr, &p)))
        return nullptr;
    std::memcpy(p, data, size);
    r->Unmap(0, nullptr);
    return r;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

// A readback of a texture (tight rows) or a buffer, recorded now and read after Execute.
struct Readback
{
    std::string name;
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    bool texture = false;
    UINT64 size = 0;
};

bool ReadTexture(Gpu& g, ID3D12Resource* tex, const char* name, std::vector<Readback>* out)
{
    D3D12_RESOURCE_DESC desc = tex->GetDesc();
    Readback rb;
    rb.name = name;
    rb.texture = true;
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &rb.footprint, &rb.rows, &rb.rowBytes, &total);
    rb.buffer = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb.buffer)
        return false;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = rb.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = rb.footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    out->push_back(std::move(rb));
    return true;
}

bool ReadBuffer(Gpu& g, ID3D12Resource* buf, UINT64 size, const char* name, std::vector<Readback>* out)
{
    Readback rb;
    rb.name = name;
    rb.size = size;
    rb.buffer = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb.buffer)
        return false;
    g.list->CopyBufferRegion(rb.buffer.Get(), 0, buf, 0, size);
    out->push_back(std::move(rb));
    return true;
}

using Outputs = std::vector<std::pair<std::string, Bytes>>;

bool Collect(const std::vector<Readback>& rbs, Outputs* out)
{
    for (const Readback& rb : rbs)
    {
        void* p = nullptr;
        D3D12_RANGE range = { 0, SIZE_T(rb.buffer->GetDesc().Width) };
        if (FAILED(rb.buffer->Map(0, &range, &p)))
            return false;
        Bytes bytes;
        const uint8_t* src = static_cast<const uint8_t*>(p);
        if (rb.texture)
        {
            for (UINT y = 0; y < rb.rows; y++)
            {
                const uint8_t* row = src + rb.footprint.Offset + UINT64(y) * rb.footprint.Footprint.RowPitch;
                bytes.insert(bytes.end(), row, row + rb.rowBytes);
            }
        }
        else
        {
            bytes.assign(src, src + rb.size);
        }
        D3D12_RANGE none = { 0, 0 };
        rb.buffer->Unmap(0, &none);
        out->emplace_back(rb.name, std::move(bytes));
    }
    return true;
}

HRESULT RootSignature(ID3D12Device* d, const D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rs)
{
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
        return hr;
    return d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
            reinterpret_cast<void**>(rs->ReleaseAndGetAddressOf()));
}

// ---- Variants: what one run hands the engine ------------------------------------------------------------------

struct LayoutElement
{
    std::string name;
    UINT index;
    DXGI_FORMAT format;
    UINT slot;
    UINT offset;
    D3D12_INPUT_CLASSIFICATION cls;
    UINT step;
};

struct SoElement
{
    UINT stream;
    bool gap;
    std::string name;
    UINT index;
    BYTE start;
    BYTE count;
    BYTE slot;
};

struct Variant
{
    std::string label;
    std::map<std::string, Bytes> shaders;       // stage ("vs", "hs", ...) -> container
    std::vector<LayoutElement> layout;
    std::vector<SoElement> so;
};

D3D12_SHADER_BYTECODE Code(const Variant& v, const char* stage)
{
    auto it = v.shaders.find(stage);
    if (it == v.shaders.end())
        return {};
    return { it->second.data(), it->second.size() };
}

struct LayoutArrays
{
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    D3D12_INPUT_LAYOUT_DESC desc = {};
};

void BuildLayout(const Variant& v, LayoutArrays* a)
{
    for (const LayoutElement& e : v.layout)
        a->elements.push_back({ e.name.c_str(), e.index, e.format, e.slot, e.offset, e.cls, e.step });
    a->desc = { a->elements.data(), UINT(a->elements.size()) };
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC BaseGraphics(ID3D12RootSignature* rs)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = rs;
    d.SampleMask = ~0u;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    for (auto& rt : d.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleDesc.Count = 1;
    return d;
}

void Viewport(ID3D12GraphicsCommandList* list, UINT w, UINT h)
{
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, LONG(w), LONG(h) };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
}

// Render targets for a graphics case: created, cleared, bound; read back after the draw.
struct Targets
{
    std::vector<ComPtr<ID3D12Resource>> rts;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;

    bool Create(Gpu& g, const std::vector<DXGI_FORMAT>& formats, UINT w, UINT h, bool withDepth)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = UINT(formats.size());
        if (!formats.empty() && FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap))))
            return false;
        UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (size_t i = 0; i < formats.size(); i++)
        {
            auto rt = MakeTexture(g.device.Get(), formats[i], w, h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (!rt)
                return false;
            D3D12_CPU_DESCRIPTOR_HANDLE hdl = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            hdl.ptr += i * inc;
            g.device->CreateRenderTargetView(rt.Get(), nullptr, hdl);
            rts.push_back(rt);
        }
        if (withDepth)
        {
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            hd.NumDescriptors = 1;
            D3D12_CLEAR_VALUE cv = {};
            cv.Format = DXGI_FORMAT_D32_FLOAT;
            cv.DepthStencil.Depth = 1.0f;
            depth = MakeTexture(g.device.Get(), DXGI_FORMAT_D32_FLOAT, w, h, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
            if (!depth || FAILED(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap))))
                return false;
            g.device->CreateDepthStencilView(depth.Get(), nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());
        }
        return true;
    }

    void ClearAndBind(Gpu& g, const std::vector<std::array<float, 4>>& clears)
    {
        UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (size_t i = 0; i < rts.size(); i++)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE hdl = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            hdl.ptr += i * inc;
            g.list->ClearRenderTargetView(hdl, clears[i].data(), 0, nullptr);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
        if (depth)
        {
            dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap ? rtvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        g.list->OMSetRenderTargets(UINT(rts.size()), rts.empty() ? nullptr : &rtv, TRUE, depth ? &dsv : nullptr);
    }

    // skip: render targets not read back (bit i), such as one the pipeline never writes.
    bool Read(Gpu& g, std::vector<Readback>* rbs, uint32_t skip = 0)
    {
        for (size_t i = 0; i < rts.size(); i++)
        {
            if (skip & (1u << i))
                continue;
            Barrier(g.list.Get(), rts[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            std::string name = "rt" + std::to_string(i);
            if (!ReadTexture(g, rts[i].Get(), name.c_str(), rbs))
                return false;
        }
        if (depth)
        {
            Barrier(g.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (!ReadTexture(g, depth.Get(), "depth", rbs))
                return false;
        }
        return true;
    }
};

// ---- Cases -------------------------------------------------------------------------------------------------------

constexpr UINT W = 64;
constexpr UINT H = 64;

HRESULT RunVsps(Gpu& g, const Variant& v, Outputs* out)
{
    struct Vertex { float pos[3]; float color[4]; float uv[2]; float extra[4]; };
    static const Vertex verts[6] = {
        { { -0.9f, -0.9f, 0.1f }, { 0.9f, 0.1f, 0.2f, 0.3f }, { 0.0f, 0.0f }, { 0.1f, 0.7f, 0.3f, 0.9f } },
        { { -0.2f,  0.9f, 0.2f }, { 0.2f, 0.8f, 0.3f, 0.6f }, { 0.5f, 1.0f }, { 0.6f, 0.2f, 0.8f, 0.1f } },
        { {  0.5f, -0.9f, 0.3f }, { 0.3f, 0.4f, 0.7f, 0.9f }, { 1.0f, 0.0f }, { 0.3f, 0.3f, 0.1f, 0.5f } },
        // Opposite winding: back-facing
        { { -0.5f,  0.9f, 0.4f }, { 0.5f, 0.6f, 0.1f, 0.2f }, { 0.2f, 0.9f }, { 0.9f, 0.4f, 0.6f, 0.3f } },
        { {  0.2f, -0.9f, 0.5f }, { 0.7f, 0.2f, 0.9f, 0.4f }, { 0.8f, 0.1f }, { 0.2f, 0.9f, 0.4f, 0.7f } },
        { {  0.9f,  0.9f, 0.6f }, { 0.1f, 0.9f, 0.5f, 0.8f }, { 0.4f, 0.6f }, { 0.7f, 0.1f, 0.2f, 0.6f } },
    };
    static const UINT tags[2] = { 5u, 9u };

    ComPtr<ID3D12RootSignature> rs;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    HRESULT hr = RootSignature(g.device.Get(), rsd, &rs);
    if (FAILED(hr))
        return hr;

    LayoutArrays layout;
    BuildLayout(v, &layout);
    std::vector<DXGI_FORMAT> formats = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R32G32_FLOAT };

    auto pd = BaseGraphics(rs.Get());
    pd.VS = Code(v, "vs");
    pd.PS = Code(v, "ps");
    pd.InputLayout = layout.desc;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = UINT(formats.size());
    pd.BlendState.IndependentBlendEnable = TRUE;
    pd.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;     // SV_Target2 is not written
    for (size_t i = 0; i < formats.size(); i++)
        pd.RTVFormats[i] = formats[i];
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        return hr;

    Targets t;
    auto vb = Upload(g.device.Get(), verts, sizeof(verts));
    auto ib = Upload(g.device.Get(), tags, sizeof(tags));
    if (!vb || !ib || !t.Create(g, formats, W, H, false))
        return E_OUTOFMEMORY;

    t.ClearAndBind(g, { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0, 0, 0, 0 }, { 0.5f, 0.5f, 0.5f, 0.5f }, { -1.0f, 2.0f, 0, 0 } });
    g.list->SetGraphicsRootSignature(rs.Get());
    g.list->SetPipelineState(pso.Get());
    Viewport(g.list.Get(), W, H);
    D3D12_VERTEX_BUFFER_VIEW vbv[2] = {
        { vb->GetGPUVirtualAddress(), UINT(sizeof(verts)), UINT(sizeof(Vertex)) },
        { ib->GetGPUVirtualAddress(), UINT(sizeof(tags)), UINT(sizeof(UINT)) },
    };
    g.list->IASetVertexBuffers(0, 2, vbv);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(6, 2, 0, 0);

    std::vector<Readback> rbs;
    if (!t.Read(g, &rbs, 1u << 2))
        return E_OUTOFMEMORY;
    if (FAILED(hr = g.Execute()))
        return hr;
    return Collect(rbs, out) ? S_OK : E_FAIL;
}

HRESULT RunDepth(Gpu& g, const Variant& v, Outputs* out)
{
    ComPtr<ID3D12RootSignature> rs;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    HRESULT hr = RootSignature(g.device.Get(), rsd, &rs);
    if (FAILED(hr))
        return hr;

    std::vector<DXGI_FORMAT> formats = { DXGI_FORMAT_R32G32B32A32_FLOAT };
    auto pd = BaseGraphics(rs.Get());
    pd.VS = Code(v, "vs");
    pd.PS = Code(v, "ps");
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = formats[0];
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        return hr;

    Targets t;
    if (!t.Create(g, formats, W, H, true))
        return E_OUTOFMEMORY;
    t.ClearAndBind(g, { { 0.0f, 0.0f, 0.0f, 0.0f } });
    g.list->SetGraphicsRootSignature(rs.Get());
    g.list->SetPipelineState(pso.Get());
    Viewport(g.list.Get(), W, H);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(6, 1, 0, 0);

    std::vector<Readback> rbs;
    if (!t.Read(g, &rbs))
        return E_OUTOFMEMORY;
    if (FAILED(hr = g.Execute()))
        return hr;
    return Collect(rbs, out) ? S_OK : E_FAIL;
}

HRESULT RunGs(Gpu& g, const Variant& v, Outputs* out)
{
    constexpr UINT64 SoSize = 1024;
    static const UINT strides[2] = { 28, 20 };

    ComPtr<ID3D12RootSignature> rs;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    HRESULT hr = RootSignature(g.device.Get(), rsd, &rs);
    if (FAILED(hr))
        return hr;

    std::vector<D3D12_SO_DECLARATION_ENTRY> so;
    for (const SoElement& e : v.so)
        if (!e.gap || g.soGaps)     // vkd3d-proton 7bfcd7f0 strdup()s every SemanticName: a hole (NULL) crashes it
            so.push_back({ e.stream, e.gap ? nullptr : e.name.c_str(), e.index, e.start, e.count, e.slot });

    auto pd = BaseGraphics(rs.Get());
    pd.VS = Code(v, "vs");
    pd.GS = Code(v, "gs");
    pd.StreamOutput.pSODeclaration = so.data();
    pd.StreamOutput.NumEntries = UINT(so.size());
    pd.StreamOutput.pBufferStrides = strides;
    pd.StreamOutput.NumStrides = 2;
    pd.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        return hr;

    // Two stream-output buffers and one counter buffer (filled sizes at 0 and 256), all zeroed first.
    auto so0 = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_DEFAULT, SoSize, D3D12_RESOURCE_STATE_COPY_DEST);
    auto so1 = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_DEFAULT, SoSize, D3D12_RESOURCE_STATE_COPY_DEST);
    auto counters = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_DEFAULT, 512, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> zeros(SoSize, 0);
    auto zero = Upload(g.device.Get(), zeros.data(), zeros.size());
    if (!so0 || !so1 || !counters || !zero)
        return E_OUTOFMEMORY;
    g.list->CopyBufferRegion(so0.Get(), 0, zero.Get(), 0, SoSize);
    g.list->CopyBufferRegion(so1.Get(), 0, zero.Get(), 0, SoSize);
    g.list->CopyBufferRegion(counters.Get(), 0, zero.Get(), 0, 512);
    for (auto* r : { so0.Get(), so1.Get(), counters.Get() })
        Barrier(g.list.Get(), r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);

    g.list->SetGraphicsRootSignature(rs.Get());
    g.list->SetPipelineState(pso.Get());
    Viewport(g.list.Get(), W, H);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2] = {
        { so0->GetGPUVirtualAddress(), SoSize, counters->GetGPUVirtualAddress() },
        { so1->GetGPUVirtualAddress(), SoSize, counters->GetGPUVirtualAddress() + 256 },
    };
    g.list->SOSetTargets(0, 2, views);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.list->DrawInstanced(4, 1, 0, 0);
    g.list->SOSetTargets(0, 0, nullptr);

    std::vector<Readback> rbs;
    for (auto* r : { so0.Get(), so1.Get(), counters.Get() })
        Barrier(g.list.Get(), r, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!ReadBuffer(g, so0.Get(), SoSize, "so0", &rbs) || !ReadBuffer(g, so1.Get(), SoSize, "so1", &rbs)
            || !ReadBuffer(g, counters.Get(), 512, "filled", &rbs))
        return E_OUTOFMEMORY;
    if (FAILED(hr = g.Execute()))
        return hr;
    return Collect(rbs, out) ? S_OK : E_FAIL;
}

HRESULT RunTess(Gpu& g, const Variant& v, Outputs* out, UINT controlPoints)
{
    struct Vertex { float pos[3]; float uv[2]; };
    std::vector<Vertex> verts;
    if (controlPoints == 3)
        verts = {
            { { -0.9f, -0.9f, 0.0f }, { 0.0f, 0.0f } }, { { -0.9f, 0.9f, 0.1f }, { 0.0f, 1.0f } }, { { 0.9f, -0.9f, 0.2f }, { 1.0f, 0.0f } },
            { { 0.9f, 0.9f, 0.3f }, { 1.0f, 1.0f } }, { { 0.9f, -0.7f, 0.4f }, { 0.5f, 0.2f } }, { { -0.7f, 0.9f, 0.5f }, { 0.2f, 0.5f } },
        };
    else if (controlPoints == 4)
        verts = {
            { { -0.9f, -0.9f, 0.0f }, { 0.0f, 0.0f } }, { { 0.1f, -0.9f, 0.1f }, { 1.0f, 0.0f } },
            { { 0.1f, 0.1f, 0.2f }, { 1.0f, 1.0f } }, { { -0.9f, 0.1f, 0.3f }, { 0.0f, 1.0f } },
            { { -0.1f, -0.2f, 0.4f }, { 0.3f, 0.1f } }, { { 0.9f, -0.1f, 0.5f }, { 0.9f, 0.2f } },
            { { 0.8f, 0.9f, 0.6f }, { 0.8f, 0.9f } }, { { -0.2f, 0.8f, 0.7f }, { 0.1f, 0.7f } },
        };
    else
        verts = {
            { { -0.9f, -0.8f, 0.0f }, { 0.0f, 0.0f } }, { { 0.9f, -0.6f, 0.1f }, { 1.0f, 0.5f } },
            { { -0.8f, 0.1f, 0.2f }, { 0.2f, 0.8f } }, { { 0.7f, 0.3f, 0.3f }, { 0.9f, 0.1f } },
        };

    ComPtr<ID3D12RootSignature> rs;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    HRESULT hr = RootSignature(g.device.Get(), rsd, &rs);
    if (FAILED(hr))
        return hr;

    LayoutArrays layout;
    BuildLayout(v, &layout);
    std::vector<DXGI_FORMAT> formats = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_UINT };
    auto pd = BaseGraphics(rs.Get());
    pd.VS = Code(v, "vs");
    pd.HS = Code(v, "hs");
    pd.DS = Code(v, "ds");
    pd.PS = Code(v, "ps");
    pd.InputLayout = layout.desc;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pd.NumRenderTargets = 2;
    pd.RTVFormats[0] = formats[0];
    pd.RTVFormats[1] = formats[1];
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        return hr;

    Targets t;
    auto vb = Upload(g.device.Get(), verts.data(), verts.size() * sizeof(Vertex));
    if (!vb || !t.Create(g, formats, W, H, false))
        return E_OUTOFMEMORY;
    t.ClearAndBind(g, { { 0.0f, 0.0f, 0.0f, 0.0f }, { 0, 0, 0, 0 } });
    g.list->SetGraphicsRootSignature(rs.Get());
    g.list->SetPipelineState(pso.Get());
    Viewport(g.list.Get(), W, H);
    D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), UINT(verts.size() * sizeof(Vertex)), UINT(sizeof(Vertex)) };
    g.list->IASetVertexBuffers(0, 1, &vbv);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + controlPoints - 1));
    g.list->DrawInstanced(UINT(verts.size()), 1, 0, 0);

    std::vector<Readback> rbs;
    if (!t.Read(g, &rbs))
        return E_OUTOFMEMORY;
    if (FAILED(hr = g.Execute()))
        return hr;
    return Collect(rbs, out) ? S_OK : E_FAIL;
}

HRESULT RunCompute(Gpu& g, const Variant& v, Outputs* out, bool sm51)
{
    const UINT64 outSize = sm51 ? 1024 : 8192;
    const UINT64 counterSize = 16;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = sm51 ? 1 : 0;
    params[0].Constants.RegisterSpace = sm51 ? 1 : 0;
    params[0].Constants.Num32BitValues = 2;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = sm51 ? 3 : 0;
    params[1].Descriptor.RegisterSpace = sm51 ? 2 : 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = sm51 ? 2 : 3;
    rsd.pParameters = params;
    ComPtr<ID3D12RootSignature> rs;
    HRESULT hr = RootSignature(g.device.Get(), rsd, &rs);
    if (FAILED(hr))
        return hr;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs.Get();
    pd.CS = Code(v, "cs");
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(hr = g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))))
        return hr;

    auto ob = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_DEFAULT, outSize, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto cb = MakeBuffer(g.device.Get(), D3D12_HEAP_TYPE_DEFAULT, counterSize, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    std::vector<uint8_t> zeros(size_t(outSize), 0);
    auto zero = Upload(g.device.Get(), zeros.data(), zeros.size());
    if (!ob || !cb || !zero)
        return E_OUTOFMEMORY;
    g.list->CopyBufferRegion(ob.Get(), 0, zero.Get(), 0, outSize);
    g.list->CopyBufferRegion(cb.Get(), 0, zero.Get(), 0, counterSize);
    Barrier(g.list.Get(), ob.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(g.list.Get(), cb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g.list->SetComputeRootSignature(rs.Get());
    g.list->SetPipelineState(pso.Get());
    UINT constants[2] = { sm51 ? 2654435761u : 3u, sm51 ? 0x5a5a5a5au : 7u };
    g.list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    g.list->SetComputeRootUnorderedAccessView(1, ob->GetGPUVirtualAddress());
    if (!sm51)
        g.list->SetComputeRootUnorderedAccessView(2, cb->GetGPUVirtualAddress());
    g.list->Dispatch(4, sm51 ? 1 : 4, 1);

    std::vector<Readback> rbs;
    Barrier(g.list.Get(), ob.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g.list.Get(), cb.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!ReadBuffer(g, ob.Get(), outSize, "uav", &rbs) || (!sm51 && !ReadBuffer(g, cb.Get(), counterSize, "counters", &rbs)))
        return E_OUTOFMEMORY;
    if (FAILED(hr = g.Execute()))
        return hr;
    return Collect(rbs, out) ? S_OK : E_FAIL;
}

// ---- Case table ---------------------------------------------------------------------------------------------------

using RunFn = std::function<HRESULT(Gpu&, const Variant&, Outputs*)>;

struct CaseDef
{
    std::string name;
    std::string what;
    std::vector<std::pair<std::string, std::string>> stages;    // stage, cso file stem
    std::vector<LayoutElement> layout;                          // API form, original names
    std::vector<SoElement> so;                                  // API form, original names
    std::string soStage;
    RunFn run;
};

std::vector<CaseDef> Cases()
{
    std::vector<CaseDef> c;

    c.push_back({ "vsps", "vs_5_0+ps_5_0: packed varyings, VertexID/InstanceID/Position/IsFrontFace, Target0/1/3, min16f",
        { { "vs", "vsps.vs" }, { "ps", "vsps.ps" } },
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 5, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        },
        {}, "", RunVsps });

    c.push_back({ "depth", "vs_5_0+ps_5_0: SV_Depth, SV_PrimitiveID (pixel only), SV_ClipDistance0",
        { { "vs", "depth.vs" }, { "ps", "depth.ps" } }, {}, {}, "", RunDepth });

    c.push_back({ "gs_streams", "vs_5_0+gs_5_0: streams 0 and 1 into two buffers, registers reused across streams, a partial entry",
        { { "vs", "gs.vs" }, { "gs", "gs.gs" } }, {},
        {
            { 0, false, "SV_Position", 0, 0, 4, 0 },
            { 0, false, "TEXCOORD", 0, 1, 2, 0 },
            { 0, false, "TEXCOORD", 4, 0, 1, 0 },
            { 1, false, "TEXCOORD", 1, 0, 2, 1 },
            { 1, true, "", 0, 0, 1, 1 },
            { 1, false, "TEXCOORD", 2, 0, 1, 1 },
            { 1, false, "TEXCOORD", 3, 0, 1, 1 },
        },
        "gs", RunGs });

    const std::vector<LayoutElement> tessLayout = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    const char* domains[3][3] = { { "tess_tri", "tri", "triangle" }, { "tess_quad", "quad", "quad" }, { "tess_isoline", "line", "isoline" } };
    for (UINT i = 0; i < 3; i++)
    {
        std::string stem = domains[i][1];
        UINT cps = i == 0 ? 3 : i == 1 ? 4 : 2;
        c.push_back({ domains[i][0], std::string("vs/hs/ds/ps_5_0, ") + domains[i][2]
                + " domain: SV_TessFactor, SV_InsideTessFactor and user data in the patch constants",
            { { "vs", stem + ".vs" }, { "hs", stem + ".hs" }, { "ds", stem + ".ds" }, { "ps", stem + ".ps" } },
            tessLayout, {}, "",
            [cps](Gpu& g, const Variant& v, Outputs* o) { return RunTess(g, v, o, cps); } });
    }

    c.push_back({ "cs50", "cs_5_0: groupshared, thread IDs, root UAVs, atomics",
        { { "cs", "cs50.cs" } }, {}, {}, "",
        [](Gpu& g, const Variant& v, Outputs* o) { return RunCompute(g, v, o, false); } });
    c.push_back({ "cs51", "cs_5_1: register spaces",
        { { "cs", "cs51.cs" } }, {}, {}, "",
        [](Gpu& g, const Variant& v, Outputs* o) { return RunCompute(g, v, o, true); } });

    // Each case again with the dxc output of the same source (shader model 6.0, DXIL).
    std::vector<CaseDef> all;
    for (const CaseDef& s : c)
    {
        all.push_back(s);
        CaseDef d = s;
        d.name += "_dxil";
        for (auto& stage : d.stages)
            stage.second = "dxil." + stage.second;
        for (size_t p; (p = d.what.find("_5_")) != std::string::npos;)
            d.what.replace(p, 4, "_6_0");
        all.push_back(std::move(d));
    }
    return all;
}

// ---- Building the variants ----------------------------------------------------------------------------------------

struct Loaded
{
    std::map<std::string, Bytes> original;      // stage -> fxc container
    std::map<std::string, DdiShader> ddi;       // stage -> DDI form
    std::map<std::string, sc::Container> rebuilt;
};

// The DDI input layout: InputRegister from the original vertex input signature.
std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC> DdiLayout(const CaseDef& c, const DdiShader& vs)
{
    std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC> ddi;
    for (const LayoutElement& e : c.layout)
    {
        D3D12DDIARG_INPUT_ELEMENT_DESC d = {};
        d.InputSlot = e.slot;
        d.AlignedByteOffset = e.offset;
        d.Format = e.format;
        d.InputSlotClass = e.cls == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            ? D3D12DDI_INPUT_CLASSIFICIATION_PER_INSTANCE_DATA : D3D12DDI_INPUT_CLASSIFICIATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = e.step;
        d.InputRegister = ~0u;
        for (const SigElement& s : vs.origInput)
            if (_stricmp(s.name.c_str(), e.name.c_str()) == 0 && s.index == e.index)
                d.InputRegister = s.reg;
        ddi.push_back(d);
    }
    return ddi;
}

// The DDI stream-output declaration: registers and masks from the original output signature.
std::vector<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY> DdiSo(const CaseDef& c, const DdiShader& last)
{
    std::vector<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY> ddi;
    for (const SoElement& e : c.so)
    {
        D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY d = {};
        d.Stream = e.stream;
        d.OutputSlot = e.slot;
        d.RegisterIndex = ~0u;
        d.RegisterMask = BYTE((1u << e.count) - 1u);
        if (!e.gap)
        {
            for (const SigElement& s : last.origOutput)
            {
                if (_stricmp(s.name.c_str(), e.name.c_str()) == 0 && s.index == e.index && s.stream == e.stream)
                {
                    uint32_t first = 0;
                    while (!(s.mask & (1u << first)))
                        first++;
                    d.RegisterIndex = s.reg;
                    d.RegisterMask = BYTE(((1u << e.count) - 1u) << (first + e.start));
                }
            }
        }
        ddi.push_back(d);
    }
    return ddi;
}

using LayoutMutation = std::function<void(std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC>&)>;
using SoMutation = std::function<void(std::vector<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY>&)>;

bool Reconstructed(const CaseDef& c, const Loaded& l, const std::string& label, const LayoutMutation& ml,
        const SoMutation& ms, Variant* v)
{
    v->label = label;
    for (const auto& [stage, container] : l.rebuilt)
        v->shaders[stage] = container.bytes;

    if (!c.layout.empty())
    {
        const DdiShader& vs = l.ddi.at("vs");
        auto ddi = DdiLayout(c, vs);
        if (ml)
            ml(ddi);
        for (size_t i = 0; i < ddi.size(); i++)
        {
            sc::Semantic s;
            sc::Result r = sc::InputLayoutSemantic({ vs.input.data(), UINT(vs.input.size()) }, ddi[i].InputRegister, &s);
            if (!Check(bool(r), c.name + "/" + label + ": input element " + std::to_string(i) + " register "
                    + std::to_string(ddi[i].InputRegister) + " -> " + (r ? s.name + std::to_string(s.index) : r.detail)))
                return false;
            v->layout.push_back({ s.name, s.index, ddi[i].Format, ddi[i].InputSlot, ddi[i].AlignedByteOffset,
                ddi[i].InputSlotClass == D3D12DDI_INPUT_CLASSIFICIATION_PER_INSTANCE_DATA
                    ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                ddi[i].InstanceDataStepRate });
        }
    }

    if (!c.so.empty())
    {
        auto ddi = DdiSo(c, l.ddi.at(c.soStage));
        if (ms)
            ms(ddi);
        for (size_t i = 0; i < ddi.size(); i++)
        {
            sc::StreamOutputElement e;
            sc::Result r = sc::StreamOutputSemantic(l.rebuilt.at(c.soStage), ddi[i], &e);
            std::string text = e.gap ? "hole of " + std::to_string(e.componentCount)
                : e.semantic.name + std::to_string(e.semantic.index) + " start " + std::to_string(e.startComponent)
                        + " count " + std::to_string(e.componentCount);
            if (!Check(bool(r), c.name + "/" + label + ": stream-output entry " + std::to_string(i) + " stream "
                    + std::to_string(ddi[i].Stream) + " register " + std::to_string(int(ddi[i].RegisterIndex)) + " mask "
                    + std::to_string(ddi[i].RegisterMask) + " -> " + (r ? text : r.detail)))
                return false;
            v->so.push_back({ ddi[i].Stream, e.gap, e.semantic.name, e.semantic.index, e.startComponent,
                e.componentCount, BYTE(ddi[i].OutputSlot) });
        }
    }
    return true;
}

bool NonTrivial(const Bytes& b)
{
    // Not every 4-byte word equal: something was drawn or written.
    for (size_t i = 4; i + 4 <= b.size(); i += 4)
        if (std::memcmp(&b[i], &b[0], 4))
            return true;
    return false;
}

HRESULT RunVariant(const Engine& e, const CaseDef& c, const Variant& v, Outputs* out)
{
    Gpu g;
    g.soGaps = e.soGaps;
    HRESULT hr = g.Init(e);
    if (FAILED(hr))
        return hr;
    return c.run(g, v, out);
}

std::string Summary(const Outputs& o)
{
    std::string s;
    for (const auto& [name, bytes] : o)
        s += (s.empty() ? "" : ", ") + name + " " + std::to_string(bytes.size()) + " B " + Sha256(bytes).substr(0, 8);
    return s;
}

struct Row
{
    std::string name, what, result;
};

std::vector<Row> g_rows;

bool Compare(const Outputs& a, const Outputs& b, std::string* diff)
{
    if (a.size() != b.size())
        return *diff = "output count differs", false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (a[i].second != b[i].second)
        {
            size_t n = 0;
            for (size_t k = 0; k < std::min(a[i].second.size(), b[i].second.size()); k++)
                n += a[i].second[k] != b[i].second[k];
            *diff = a[i].first + ": " + std::to_string(n) + " bytes differ";
            return false;
        }
    }
    return true;
}

bool IsDxil(const DdiShader& s)
{
    return !s.code.empty() && ((s.code[0] >> 4) & 0xfu) >= 6;
}

// dxil-metadata against dxc: every entry of dxc's ISG1/OSG1/PSG1 with a register finds the metadata element on its
// register, first component and stream, with the same name and semantic index.
bool MetadataMatches(const DdiShader& s, std::string* detail)
{
    const uint8_t* bitcode;
    size_t size;
    dxil::Signatures md;
    std::string error = "no DXIL program";
    if (!dxil::ProgramBitcode(reinterpret_cast<const uint8_t*>(s.code.data()), s.code.size() * 4, &bitcode, &size)
            || !dxil::ReadSignatures(bitcode, size, &md, &error))
        return *detail = "metadata does not parse: " + error, false;

    unsigned matched = 0;
    struct { const std::vector<SigElement>* chunk; const std::vector<dxil::Element>* md; const char* what; } lists[] = {
        { &s.origInput, &md.input, "input" }, { &s.origOutput, &md.output, "output" },
        { &s.origPatch, &md.patchConstant, "patch constant" },
    };
    for (const auto& list : lists)
    {
        for (const SigElement& x : *list.chunk)
        {
            if (x.reg == ~0u)
                continue;
            uint32_t first = 0;
            while (first < 4 && !(x.mask & (1u << first)))
                first++;
            const dxil::Element* hit = nullptr;
            for (const dxil::Element& m : *list.md)
                if (m.startRow >= 0 && x.reg >= uint32_t(m.startRow) && x.reg - uint32_t(m.startRow) < m.rows
                        && m.startCol == int32_t(first) && m.stream == x.stream)
                    hit = &m;
            if (!hit || _stricmp(hit->name.c_str(), x.name.c_str()) || hit->indices[x.reg - uint32_t(hit->startRow)] != x.index)
                return *detail = std::string(list.what) + " " + x.name + std::to_string(x.index) + " on register "
                        + std::to_string(x.reg) + (hit ? " is " + hit->name + " in the metadata" : " not in the metadata"), false;
            matched++;
        }
    }
    *detail = std::to_string(matched) + " entries";
    return true;
}

void RunCase(const Engine& mainEngine, const Engine* soEngine, const CaseDef& c, const std::string& csoDir)
{
    // Stream-output cases run on the engine that takes holes, when there is one.
    const Engine& e = !c.so.empty() && soEngine ? *soEngine : mainEngine;
    printf("\n== %s: %s%s\n", c.name.c_str(), c.what.c_str(), &e == soEngine ? " [stream-output engine]" : "");
    Loaded l;

    for (const auto& [stage, stem] : c.stages)
    {
        Bytes bytes;
        if (!Check(ReadFile(csoDir + "\\" + stem + ".cso", &bytes), c.name + ": read " + stem + ".cso"))
            return g_rows.push_back({ c.name, c.what, "no input" });

        // The checksum port reproduces the compiler's (fxc; dxc through dxil.dll signing).
        uint8_t digest[16];
        sc::DxbcChecksum(bytes.data(), bytes.size(), digest);
        Check(bytes.size() > 20 && !std::memcmp(digest, &bytes[4], 16), c.name + ": " + stem + " checksum equals the compiler's");

        DdiShader s;
        std::string why;
        if (!Check(Strip(bytes, &s, &why), c.name + ": strip " + stem + (why.empty() ? "" : ": " + why)))
            return g_rows.push_back({ c.name, c.what, "strip failed" });

        if (IsDxil(s))
        {
            std::string detail;
            bool agree = MetadataMatches(s, &detail);
            Check(agree, c.name + ": " + stem + " dx.entryPoints signatures agree with dxc's ISG1/OSG1/PSG1: " + detail);
        }

        sc::Container rebuilt;
        sc::Result r = sc::BuildContainer(Desc(s), &rebuilt);
        if (!Check(bool(r), c.name + ": rebuild " + stem + (r ? "" : ": " + r.detail)))
            return g_rows.push_back({ c.name, c.what, "rebuild refused" });
        WriteFile(c.name + "." + stage + ".rebuilt.dxbc", rebuilt.bytes);

        // Round trip: the rebuilt container carries the same program and the same DDI signature fields.
        DdiShader back;
        bool parsed = Strip(rebuilt.bytes, &back, &why);
        std::vector<Chunk> chunks;
        ParseContainer(rebuilt.bytes, &chunks);
        std::string tags;
        for (const Chunk& ch : chunks)
            tags += (tags.empty() ? "" : ",") + ch.tag;
        uint8_t check[16];
        sc::DxbcChecksum(rebuilt.bytes.data(), rebuilt.bytes.size(), check);
        Check(parsed && back.code == s.code && !std::memcmp(check, &rebuilt.bytes[4], 16),
                c.name + ": " + stem + " rebuilt [" + tags + "], program unchanged, checksum valid");
        std::vector<Entry> expectOut = Registered(rebuilt.output);
        Check(parsed && SameEntries(Registered(back.input), Registered(s.input))
                && SameEntries(Registered(back.output), expectOut) && SameEntries(Registered(back.patch), Registered(s.patch)),
                c.name + ": " + stem + " rebuilt signatures carry the DDI fields unchanged");

        // Read masks of inputs: derived from the program's declarations; compare with fxc's.
        auto rwMatch = [](const std::vector<SigElement>& o, const std::vector<SigElement>& n, unsigned* same, unsigned* total) {
            std::vector<SigElement> reg;
            for (const SigElement& x : o)
                if (x.reg != ~0u)
                    reg.push_back(x);
            for (size_t i = 0; i < reg.size() && i < n.size(); i++, (*total)++)
                *same += reg[i].rw == n[i].rw;
        };
        unsigned same = 0, total = 0;
        rwMatch(s.origInput, back.origInput, &same, &total);
        if (stage == "ds")
            rwMatch(s.origPatch, back.origPatch, &same, &total);
        printf("info  %s: %s input read masks equal to the compiler's: %u of %u\n", c.name.c_str(), stem.c_str(), same, total);

        l.original[stage] = bytes;
        l.ddi[stage] = std::move(s);
        l.rebuilt[stage] = std::move(rebuilt);
    }

    // A geometry output signature without streams (as the D3D11 DDI passes it) must give the same container.
    if (l.ddi.count("gs"))
    {
        DdiShader zeroed = l.ddi["gs"];
        for (Entry& en : zeroed.output)
            en.Stream = 0;
        sc::Container derived;
        sc::Result r = sc::BuildContainer(Desc(zeroed), &derived);
        Check(r && derived.bytes == l.rebuilt["gs"].bytes, c.name + ": gs output streams zeroed, derived from "
                + (IsDxil(zeroed) ? "the metadata" : "dcl_stream") + ": identical container" + (r ? "" : " (" + r.detail + ")"));
    }

    Variant original;
    original.label = "original";
    original.shaders = l.original;
    original.layout = c.layout;
    original.so = c.so;

    Variant rebuilt;
    if (!Reconstructed(c, l, "reconstructed", nullptr, nullptr, &rebuilt))
        return g_rows.push_back({ c.name, c.what, "mapping failed" });

    Outputs oa, ob;
    HRESULT ha = RunVariant(e, c, original, &oa);
    Check(SUCCEEDED(ha), c.name + ": original run " + Hex(ha) + (SUCCEEDED(ha) ? " (" + Summary(oa) + ")" : ""));
    HRESULT hb = RunVariant(e, c, rebuilt, &ob);
    Check(SUCCEEDED(hb), c.name + ": reconstructed run " + Hex(hb) + (SUCCEEDED(hb) ? " (" + Summary(ob) + ")" : ""));
    if (FAILED(ha) || FAILED(hb))
        return g_rows.push_back({ c.name, c.what, "run failed" });

    bool nonTrivial = true;
    for (const auto& [name, bytes] : oa)
    {
        WriteFile(c.name + ".original." + name + ".bin", bytes);
        nonTrivial = nonTrivial && NonTrivial(bytes);
    }
    for (const auto& [name, bytes] : ob)
        WriteFile(c.name + ".reconstructed." + name + ".bin", bytes);
    Check(nonTrivial, c.name + ": every original output holds more than one value");

    std::string diff;
    bool same = Compare(oa, ob, &diff);
    Check(same, c.name + ": original and reconstructed outputs byte-identical" + (same ? "" : ": " + diff));
    g_rows.push_back({ c.name, c.what, same ? "match" : "MISMATCH (" + diff + ")" });

    // Mismatch controls: a wrong register mapping must be visible.
    LayoutMutation ml;
    SoMutation ms;
    std::string control;
    if (c.name == "vsps" || c.name == "vsps_dxil")
    {
        control = "M1 input layout: COLOR3 and TEXCOORD2 data on each other's register";
        ml = [](std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC>& d) { std::swap(d[1].InputRegister, d[3].InputRegister); };
    }
    else if (c.name == "gs_streams" || c.name == "gs_streams_dxil")
    {
        control = "M2 stream output: TEXCOORD0.yz declared on register 0 (SV_Position) instead of 1";
        ms = [](std::vector<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY>& d) { d[1].RegisterIndex = 0; };
    }
    if (!control.empty())
    {
        Variant wrong;
        Outputs ow;
        HRESULT hw = E_FAIL;
        if (Reconstructed(c, l, "control", ml, ms, &wrong))
            hw = RunVariant(e, c, wrong, &ow);
        std::string d2;
        bool detected = FAILED(hw) || !Compare(oa, ow, &d2);
        Check(detected, c.name + ": " + control + ": detected (" + (FAILED(hw) ? "run " + Hex(hw) : d2) + ")");
        g_rows.push_back({ c.name + " " + control.substr(0, 2), control.substr(3),
            detected ? "mismatch detected (" + (FAILED(hw) ? "run " + Hex(hw) : d2) + ")" : "NOT DETECTED" });
    }
}

// ---- Refusals ---------------------------------------------------------------------------------------------------

// A buffer whose last readable DWORD is followed by an inaccessible page: a read past it faults.
struct GuardedCode
{
    uint8_t* base = nullptr;
    UINT* code = nullptr;

    explicit GuardedCode(const std::vector<UINT>& dwords)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        VirtualProtect(base + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old);
        code = reinterpret_cast<UINT*>(base + si.dwPageSize - dwords.size() * sizeof(UINT));
        std::memcpy(code, dwords.data(), dwords.size() * sizeof(UINT));
    }
    ~GuardedCode() { VirtualFree(base, 0, MEM_RELEASE); }
};

const char* StatusName(sc::Status s)
{
    return s == sc::Status::Ok ? "Ok" : s == sc::Status::Unsupported ? "Unsupported" : "InvalidArgument";
}

void RunRefusals(const std::string& csoDir)
{
    printf("\n== refusals\n");
    std::map<std::string, DdiShader> s;
    for (const char* stem : { "vsps.vs", "vsps.ps", "gs.gs", "cs50.cs", "iface.ps", "dxil.vsps.ps" })
    {
        Bytes bytes;
        std::string why;
        if (!Check(ReadFile(csoDir + "\\" + std::string(stem) + ".cso", &bytes) && Strip(bytes, &s[stem], &why),
                std::string("refusals: load ") + stem))
            return;
    }

    struct Refusal
    {
        std::string what;
        sc::Status expected;
        std::function<sc::Result()> build;
    };

    auto build = [](DdiShader d) {
        sc::Container c;
        return sc::BuildContainer(Desc(d), &c);
    };

    std::vector<Refusal> cases = {
        { "class linkage: fxc ps_5_0 with interfaces (dcl_function_body, dcl_interface, fcall)", sc::Status::Unsupported,
            [&] { return build(s["iface.ps"]); } },
        { "DXIL library (lib_6_3 program header, kind 6)", sc::Status::Unsupported,
            [&] { DdiShader d; d.code = { 0x00060063u, 6u, 0x4C495844u, 0x103u, 16u, 0u }; return build(d); } },
        { "DXIL BitcodeSize past SizeInUint32", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["dxil.vsps.ps"]; d.code[5] += 4; return build(d); } },
        { "system value SV_Barycentrics (DXIL only)", sc::Status::Unsupported,
            [&] { DdiShader d = s["vsps.ps"]; Entry e = {}; e.SystemValue = D3D12_SB_NAME_BARYCENTRICS; e.Register = 8;
                  e.Mask = 0x7; e.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32; d.input.push_back(e); return build(d); } },
        { "16-bit component type (DXIL only)", sc::Status::Unsupported,
            [&] { DdiShader d = s["vsps.ps"]; d.input[0].RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT16; return build(d); } },
        { "unknown system value 40", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["vsps.ps"]; d.input[0].SystemValue = D3D10_SB_NAME(40); return build(d); } },
        { "stream 1 on a vertex output", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["vsps.vs"]; d.output[0].Stream = 1; return build(d); } },
        { "stream 4 on a gs_5_0 output", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["gs.gs"]; d.output[0].Stream = 4; return build(d); } },
        { "two entries on the same components", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["vsps.vs"]; d.output[1].Mask = 0x6; return build(d); } },
        { "minimum precision FLOAT_16 on a UINT32 entry", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["vsps.vs"]; d.input[4].MinPrecision = D3D11_SB_OPERAND_MIN_PRECISION_FLOAT_16; return build(d); } },
        { "patch-constant signature on a pixel program", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["vsps.ps"]; d.patch.push_back(d.input[0]); return build(d); } },
        { "signature on a compute program", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["cs50.cs"]; d.input.push_back(s["vsps.vs"].input[0]); return build(d); } },
        { "instruction of length 0", sc::Status::InvalidArgument,
            [&] { DdiShader d = s["vsps.vs"]; d.code[2] &= 0x80ffffffu; return build(d); } },
        { "LenTok below 2", sc::Status::InvalidArgument,
            [&] { GuardedCode g({ 0x00010050u, 1u }); sc::ProgramDesc p; p.code = g.code; p.codeCapacity = 2;
                  sc::Container c; return sc::BuildContainer(p, &c); } },
        { "LenTok past the readable DWORDs, which end at a guard page", sc::Status::InvalidArgument,
            [&] { std::vector<UINT> code = s["vsps.vs"].code; code.pop_back(); GuardedCode g(code);
                  sc::ProgramDesc p; p.code = g.code; p.codeCapacity = code.size(); sc::Container c; return sc::BuildContainer(p, &c); } },
        { "one readable DWORD at a guard page", sc::Status::InvalidArgument,
            [&] { GuardedCode g({ 0x00010050u }); sc::ProgramDesc p; p.code = g.code; p.codeCapacity = 1;
                  sc::Container c; return sc::BuildContainer(p, &c); } },
        { "accepted: two-DWORD program (VerTok, LenTok 2) ending at a guard page", sc::Status::Ok,
            [&] { GuardedCode g({ 0x00010050u, 2u }); sc::ProgramDesc p; p.code = g.code; p.codeCapacity = 2;
                  sc::Container c; return sc::BuildContainer(p, &c); } },
    };

    for (const Refusal& r : cases)
    {
        sc::Result got = r.build();
        bool ok = got.status == r.expected;
        Check(ok, "refusal: " + r.what + " -> " + StatusName(got.status) + (got.detail.empty() ? "" : " (" + got.detail + ")"));
        g_rows.push_back({ "refuse", r.what, std::string(StatusName(got.status)) + (ok ? "" : " (expected " + std::string(StatusName(r.expected)) + ")") });
    }
}

bool FindAdapter(const char* filter, LUID* luid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++)
    {
        DXGI_ADAPTER_DESC1 desc;
        char name[256];
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        if (!filter || strstr(name, filter))
        {
            printf("adapter: %s\n", name);
            *luid = desc.AdapterLuid;
            return true;
        }
    }
    return false;
}

} // namespace

// Loads an engine DLL by its full path; two copies of the same file name from different directories load apart.
bool LoadEngine(const char* path, PFN_vkGetInstanceProcAddr gipa, Engine* e)
{
    char full[MAX_PATH];
    HMODULE dll = nullptr;
    if (GetFullPathNameA(path, MAX_PATH, full, nullptr))
        dll = LoadLibraryExA(full, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto getFuncs = dll ? reinterpret_cast<PFN_BC250_VKD3D_ENGINE_GET_FUNCS>(
            reinterpret_cast<void*>(GetProcAddress(dll, BC250_VKD3D_ENGINE_GET_FUNCS_NAME))) : nullptr;
    e->gipa = gipa;
    e->funcs.Size = sizeof(e->funcs);
    return Check(dll && getFuncs && gipa && SUCCEEDED(getFuncs(BC250_VKD3D_ENGINE_ABI_VERSION, &e->funcs))
            && e->funcs.CreateDevice, std::string("engine loaded, Vulkan loader entry point, GetFuncs: ") + full);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* adapter = nullptr;
    const char* soPath = nullptr;
    for (int i = 4; i < argc; i++)
    {
        if (!strcmp(argv[i], "--so-engine") && i + 1 < argc)
            soPath = argv[++i];
        else
            adapter = argv[i];
    }
    if (argc < 4)
    {
        printf("usage: shader-container-test <amdgpu_wddm_vkd3d.dll> <cso directory> <output directory> [adapter substring]"
               " [--so-engine <amdgpu_wddm_vkd3d.dll>]\n");
        return 2;
    }
    g_out = argv[3];
    CreateDirectoryA(g_out.c_str(), nullptr);

    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    auto gipa = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"))) : nullptr;
    Engine e, so;
    if (!LoadEngine(argv[1], gipa, &e) || (soPath && !LoadEngine(soPath, gipa, &so))
            || !Check(FindAdapter(adapter, &e.luid), "adapter"))
        return 1;
    so.luid = e.luid;
    so.soGaps = true;

    for (const CaseDef& c : Cases())
        RunCase(e, soPath ? &so : nullptr, c, argv[2]);
    RunRefusals(argv[2]);

    printf("\n%-28s | %-100s | %s\n", "case", "what", "result");
    for (const Row& r : g_rows)
        printf("%-28s | %-100s | %s\n", r.name.c_str(), r.what.c_str(), r.result.c_str());

    printf("\n%s (%u checks, %u failed)\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
