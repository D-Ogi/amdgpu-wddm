// SPDX-License-Identifier: MIT
// Deferred command-list replay (replay.h, set_replay_policy), linked against the native engine-ddi.lib (no harness
// macro), the way the shell's DLL links it. No engine, no GPU: the engine list, device, descriptor heaps, allocators,
// queue and state object properties are fakes. The fake list logs every call with its arguments as they are when it
// runs: arrays by content, CPU descriptors by the descriptor they point to, engine objects by an id that is also
// checked to be alive. What it proves:
//
//   1. set_replay_policy's refusals; on, off and on again on one context.
//   2. Exactness: every converted recording slot (55 engine methods), called with the policy off and then on, reaches
//      the engine list with the same calls and arguments. With the policy on the worker is held until the whole
//      sequence is recorded, and every array and descriptor the slots were given is overwritten after its call: a
//      call that kept a pointer instead of a copy would see the overwrite.
//   3. Drains: Close, which waits for nothing and is the list's last entry, and whose engine failure reaches the
//      runtime from the list's next drain; Reset; ExecuteBundle; ExecuteCommandLists; the destroy of a list; a pool's
//      reset and destroy; a pool's reset and a Reset into it waiting for the pool's pending Close only;
//      SetPipelineStackSize.
//      The shell's drained hook runs once per drain a DDI call makes. Here and below, a drain that must wait is made
//      with the worker held at the list's gate until the drain waits (held, Opener): no check depends on timing.
//   4. Order across threads: a list recorded by a second thread (Switch) and by a thread without a ring (Direct).
//   5. Bounds: a 64 KiB ring that fills (Space); a call over the entry limit and calls without a snapshot heap, both
//      made directly and in order; snapshot slots reused (Slots).
//   6. Destroys: four recording threads hand objects to a destroyer thread, and no engine call names a released
//      object; a release that does not drain is seen by the same check.
//   7. Rings: the cap (a thread beyond it records directly), an exited thread's ring taken over, teardown with
//      entries pending, every worker joined and every snapshot heap released.
//   8. Cost on the calling thread, off and on (printed, not a gate), and no allocation while recording.
#include "internal.h"
#include "replay.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Every operator new of the process, for the check that recording allocates nothing.
static std::atomic<uint64_t> g_allocations{0};
void* operator new(size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

using namespace engine_ddi;

namespace {
int failures = 0;
void check(bool ok, const char* format, ...) {
    char text[640];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", text);
    failures += ok ? 0 : 1;
}

constexpr UINT kDescriptor = 8;                 // a fake CPU descriptor is one 64-bit word

uint64_t qpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}
uint64_t qpf() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    return frequency;
}
void spin(uint32_t ns) {
    if (!ns) return;
    const uint64_t until = qpc() + qpf() * ns / 1000000000;
    while (qpc() < until) YieldProcessor();
}
double ns_per(uint64_t ticks, uint64_t count) { return count ? 1e9 * static_cast<double>(ticks) / qpf() / count : 0; }

// ---- Fakes ----------------------------------------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable : 4100)                 // the fakes ignore most of their arguments

// An engine object the slots only pass through: an id for the logs, alive until its last Release.
struct Thing final : IUnknown {
    uint32_t id = 0;
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};

template <class I> class Unknown : public I {
public:
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
        if (!n) final_release();
        return n;
    }
    virtual void final_release() {}
};
template <class I> class Object : public Unknown<I> {
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
};
template <class I> class Child : public Object<I> {
public:
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
};

class FakeList;
// One engine call being logged. It enters the list (one call at a time, R2), waits at the list's gate on any thread
// but the caller's, spins the list's delay, counts, and formats the arguments unless the list is quiet.
class Call {
public:
    Call(FakeList* list, const char* name);
    ~Call();
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;
    Call& u(uint64_t v);
    Call& i(int64_t v);
    Call& f(double v);
    Call& x(uint64_t v);
    Call& s(const char* text);
    Call& obj(const void* p);                               // an engine object: its id, and a check that it is alive
    Call& raw(const void* p, size_t bytes);                 // "null", or the bytes in hex
    Call& cpu(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT n);  // n descriptors from handle, by content

private:
    void put(const char* format, ...);
    FakeList* l_;
    bool text_;
};

class FakeList final : public Child<ID3D12GraphicsCommandList5> {
public:
    static constexpr UINT kLatchStencil = 0xBAD;            // OMSetStencilRef(kLatchStencil) makes the next Close fail
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    uint32_t id = 0;
    std::string log;
    bool quiet = false;                                     // count and check only (the cost measurement)
    uint32_t delay_ns = 0;                                  // spun in every call, so that a worker falls behind
    HANDLE gate = nullptr;                                  // a manual-reset event the calls of other threads wait for
    std::atomic<DWORD> caller{0};                           // calls made on this thread count in caller_calls
    HRESULT close_result = S_OK;
    bool ordered = false;                                   // DrawInstanced's vertex counts must run 1, 2, 3, ...
    UINT next_vertex = 1;
    std::atomic<uint64_t> calls{0}, caller_calls{0}, crossed{0}, stale{0}, unexpected{0}, disorder{0};
    std::atomic<uint64_t> gate_timeouts{0};
    std::atomic<int> inside{0};
    std::atomic<uint64_t> calls_at_release{UINT64_MAX};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
            riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
            riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4) ||
            riid == __uuidof(ID3D12GraphicsCommandList5)) {
            AddRef();
            *out = static_cast<ID3D12GraphicsCommandList5*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    void final_release() override { calls_at_release = calls.load(); }
    void odd(const char* name) {
        Call(this, name);
        ++unexpected;
    }
    static void location(Call& c, const D3D12_TEXTURE_COPY_LOCATION* l) {
        if (!l) {
            c.s("null");
            return;
        }
        c.obj(l->pResource).u(l->Type);
        if (l->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
            c.u(l->SubresourceIndex);
            return;
        }
        const D3D12_SUBRESOURCE_FOOTPRINT& p = l->PlacedFootprint.Footprint;
        c.u(l->PlacedFootprint.Offset).u(p.Format).u(p.Width).u(p.Height).u(p.Depth).u(p.RowPitch);
    }
    static void geometry(Call& c, const D3D12_RAYTRACING_GEOMETRY_DESC* g) {
        if (!g) {
            c.s("null");
            return;
        }
        c.u(g->Type).u(g->Flags);
        if (g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
            const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& t = g->Triangles;
            c.x(t.Transform3x4).u(t.IndexFormat).u(t.VertexFormat).u(t.IndexCount).u(t.VertexCount).x(t.IndexBuffer)
                .x(t.VertexBuffer.StartAddress).u(t.VertexBuffer.StrideInBytes);
        } else {
            c.u(g->AABBs.AABBCount).x(g->AABBs.AABBs.StartAddress).u(g->AABBs.AABBs.StrideInBytes);
        }
    }

    // ID3D12CommandList, ID3D12GraphicsCommandList
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type; }
    HRESULT STDMETHODCALLTYPE Close() override {
        Call(this, "Close");
        return close_result;
    }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) override {
        Call(this, "Reset").u(allocator != nullptr).obj(state);
        close_result = S_OK;
        return S_OK;
    }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState*) override { odd("ClearState"); }
    void STDMETHODCALLTYPE DrawInstanced(UINT vertices, UINT instances, UINT first_vertex,
                                         UINT first_instance) override {
        Call(this, "DrawInstanced").u(vertices).u(instances).u(first_vertex).u(first_instance);
        if (ordered) {
            if (vertices != next_vertex) ++disorder;
            next_vertex = vertices + 1;
        }
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex,
                                                UINT first_instance) override {
        Call(this, "DrawIndexedInstanced").u(indices).u(instances).u(first_index).i(base_vertex).u(first_instance);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override { Call(this, "Dispatch").u(x).u(y).u(z); }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                                            UINT64 src_offset, UINT64 bytes) override {
        Call(this, "CopyBufferRegion").obj(dst).u(dst_offset).obj(src).u(src_offset).u(bytes);
    }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) override {
        Call c(this, "CopyTextureRegion");
        location(c, dst);
        c.u(x).u(y).u(z);
        location(c, src);
        c.raw(box, sizeof(*box));
    }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* dst, ID3D12Resource* src) override {
        Call(this, "CopyResource").obj(dst).obj(src);
    }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* tiled, const D3D12_TILED_RESOURCE_COORDINATE* start,
                                     const D3D12_TILE_REGION_SIZE* size, ID3D12Resource* buffer, UINT64 offset,
                                     D3D12_TILE_COPY_FLAGS flags) override {
        Call(this, "CopyTiles").obj(tiled).raw(start, sizeof(*start)).raw(size, sizeof(*size)).obj(buffer).u(offset)
            .u(flags);
    }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* dst, UINT dst_subresource, ID3D12Resource* src,
                                              UINT src_subresource, DXGI_FORMAT format) override {
        Call(this, "ResolveSubresource").obj(dst).u(dst_subresource).obj(src).u(src_subresource).u(format);
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) override {
        Call(this, "IASetPrimitiveTopology").u(topology);
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT* v) override {
        Call(this, "RSSetViewports").u(n).raw(v, n * sizeof(*v));
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT* r) override {
        Call(this, "RSSetScissorRects").u(n).raw(r, n * sizeof(*r));
    }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override {
        Call(this, "OMSetBlendFactor").raw(factor, 4 * sizeof(FLOAT));
    }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT ref) override {
        Call(this, "OMSetStencilRef").u(ref);
        if (ref == kLatchStencil) close_result = E_OUTOFMEMORY;
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* state) override {
        Call(this, "SetPipelineState").obj(state);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* barriers) override {
        Call c(this, "ResourceBarrier");
        c.u(n);
        for (UINT k = 0; k < n; ++k) {
            const D3D12_RESOURCE_BARRIER& b = barriers[k];
            c.u(b.Type).u(b.Flags);
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                c.obj(b.Transition.pResource).u(b.Transition.Subresource).u(b.Transition.StateBefore)
                    .u(b.Transition.StateAfter);
            else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
                c.obj(b.Aliasing.pResourceBefore).obj(b.Aliasing.pResourceAfter);
            else
                c.obj(b.UAV.pResource);
        }
    }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* bundle) override {
        Call(this, "ExecuteBundle").u(static_cast<FakeList*>(bundle)->id);
    }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* heaps) override {
        Call c(this, "SetDescriptorHeaps");
        c.u(n);
        for (UINT k = 0; k < n; ++k) c.obj(heaps[k]);
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* s) override {
        Call(this, "SetComputeRootSignature").obj(s);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* s) override {
        Call(this, "SetGraphicsRootSignature").obj(s);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) override {
        Call(this, "SetComputeRootDescriptorTable").u(index).x(base.ptr);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) override {
        Call(this, "SetGraphicsRootDescriptorTable").u(index).x(base.ptr);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT index, UINT data, UINT offset) override {
        Call(this, "SetComputeRoot32BitConstant").u(index).u(data).u(offset);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT index, UINT data, UINT offset) override {
        Call(this, "SetGraphicsRoot32BitConstant").u(index).u(data).u(offset);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT index, UINT n, const void* data, UINT offset) override {
        Call(this, "SetComputeRoot32BitConstants").u(index).u(n).raw(data, n * sizeof(UINT)).u(offset);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT index, UINT n, const void* data, UINT offset) override {
        Call(this, "SetGraphicsRoot32BitConstants").u(index).u(n).raw(data, n * sizeof(UINT)).u(offset);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) override {
        Call(this, "SetComputeRootConstantBufferView").u(index).x(va);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) override {
        Call(this, "SetGraphicsRootConstantBufferView").u(index).x(va);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) override {
        Call(this, "SetComputeRootShaderResourceView").u(index).x(va);
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) override {
        Call(this, "SetGraphicsRootShaderResourceView").u(index).x(va);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) override {
        Call(this, "SetComputeRootUnorderedAccessView").u(index).x(va);
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) override {
        Call(this, "SetGraphicsRootUnorderedAccessView").u(index).x(va);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) override {
        Call(this, "IASetIndexBuffer").raw(view, sizeof(*view));
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* views) override {
        Call(this, "IASetVertexBuffers").u(start).u(n).raw(views, n * sizeof(*views));
    }
    void STDMETHODCALLTYPE SOSetTargets(UINT start, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) override {
        Call(this, "SOSetTargets").u(start).u(n).raw(views, n * sizeof(*views));
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL range,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) override {
        Call c(this, "OMSetRenderTargets");
        c.u(n).u(range);
        if (!rtvs) c.s("null");
        else if (range && n) c.cpu(rtvs[0], n);
        else if (!range) for (UINT k = 0; k < n; ++k) c.cpu(rtvs[k], 1);
        if (dsv) c.cpu(*dsv, 1);
        else c.s("null");
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                 UINT8 stencil, UINT n, const D3D12_RECT* rects) override {
        Call(this, "ClearDepthStencilView").cpu(view, 1).u(flags).f(depth).u(stencil).u(n)
            .raw(rects, n * sizeof(*rects));
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT rgba[4], UINT n,
                                                 const D3D12_RECT* rects) override {
        Call(this, "ClearRenderTargetView").cpu(view, 1).raw(rgba, 4 * sizeof(FLOAT)).u(n)
            .raw(rects, n * sizeof(*rects));
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE view, ID3D12Resource* resource,
                                                        const UINT values[4], UINT n,
                                                        const D3D12_RECT* rects) override {
        Call(this, "ClearUnorderedAccessViewUint").x(gpu.ptr).cpu(view, 1).obj(resource).raw(values, 4 * sizeof(UINT))
            .u(n).raw(rects, n * sizeof(*rects));
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE view, ID3D12Resource* resource,
                                                         const FLOAT values[4], UINT n,
                                                         const D3D12_RECT* rects) override {
        Call(this, "ClearUnorderedAccessViewFloat").x(gpu.ptr).cpu(view, 1).obj(resource)
            .raw(values, 4 * sizeof(FLOAT))
            .u(n).raw(rects, n * sizeof(*rects));
    }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* resource, const D3D12_DISCARD_REGION* region) override {
        Call c(this, "DiscardResource");
        c.obj(resource);
        if (!region) c.s("whole");
        else c.u(region->NumRects).raw(region->pRects, region->NumRects * sizeof(D3D12_RECT))
                 .u(region->FirstSubresource).u(region->NumSubresources);
    }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE kind, UINT index) override {
        Call(this, "BeginQuery").obj(heap).u(kind).u(index);
    }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE kind, UINT index) override {
        Call(this, "EndQuery").obj(heap).u(kind).u(index);
    }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE kind, UINT first, UINT n,
                                            ID3D12Resource* buffer, UINT64 offset) override {
        Call(this, "ResolveQueryData").obj(heap).u(kind).u(first).u(n).obj(buffer).u(offset);
    }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP op) override {
        Call(this, "SetPredication").obj(buffer).u(offset).u(op);
    }
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override { odd("SetMarker"); }
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override { odd("BeginEvent"); }
    void STDMETHODCALLTYPE EndEvent() override { odd("EndEvent"); }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* signature, UINT max_count, ID3D12Resource* arguments,
                                           UINT64 argument_offset, ID3D12Resource* counts,
                                           UINT64 count_offset) override {
        Call(this, "ExecuteIndirect").obj(signature).u(max_count).obj(arguments).u(argument_offset).obj(counts)
            .u(count_offset);
    }
    // ID3D12GraphicsCommandList1
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT,
                                                ID3D12Resource* const*,
                                                const D3D12_SUBRESOURCE_RANGE_UINT64*) override {
        odd("AtomicCopyBufferUINT");
    }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT,
                                                  ID3D12Resource* const*,
                                                  const D3D12_SUBRESOURCE_RANGE_UINT64*) override {
        odd("AtomicCopyBufferUINT64");
    }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT low, FLOAT high) override {
        Call(this, "OMSetDepthBounds").f(low).f(high);
    }
    void STDMETHODCALLTYPE SetSamplePositions(UINT per_pixel, UINT pixels, D3D12_SAMPLE_POSITION* positions) override {
        Call(this, "SetSamplePositions").u(per_pixel).u(pixels)
            .raw(positions, size_t{per_pixel} * pixels * sizeof(*positions));
    }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource* dst, UINT dst_subresource, UINT x, UINT y,
                                                    ID3D12Resource* src, UINT src_subresource, D3D12_RECT* rect,
                                                    DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) override {
        Call(this, "ResolveSubresourceRegion").obj(dst).u(dst_subresource).u(x).u(y).obj(src).u(src_subresource)
            .raw(rect, sizeof(*rect)).u(format).u(mode);
    }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT mask) override { Call(this, "SetViewInstanceMask").u(mask); }
    // ID3D12GraphicsCommandList2, 3
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER*,
                                                const D3D12_WRITEBUFFERIMMEDIATE_MODE*) override {
        odd("WriteBufferImmediate");
    }
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession*) override {
        odd("SetProtectedResourceSession");
    }
    // ID3D12GraphicsCommandList4
    void STDMETHODCALLTYPE BeginRenderPass(UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                           const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
                                           D3D12_RENDER_PASS_FLAGS) override {
        odd("BeginRenderPass");
    }
    void STDMETHODCALLTYPE EndRenderPass() override { odd("EndRenderPass"); }
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand*, const void*, SIZE_T) override {
        odd("InitializeMetaCommand");
    }
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand*, const void*, SIZE_T) override {
        odd("ExecuteMetaCommand");
    }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc, UINT n,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* info) override {
        Call c(this, "BuildRaytracingAccelerationStructure");
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs = desc->Inputs;
        c.x(desc->DestAccelerationStructureData).u(inputs.Type).u(inputs.Flags).u(inputs.NumDescs)
            .u(inputs.DescsLayout);
        if (inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
            for (UINT k = 0; k < inputs.NumDescs; ++k)
                geometry(c, inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? &inputs.pGeometryDescs[k]
                                                                              : inputs.ppGeometryDescs[k]);
        } else {
            c.x(inputs.InstanceDescs);
        }
        c.x(desc->SourceAccelerationStructureData).x(desc->ScratchAccelerationStructureData).u(n);
        for (UINT k = 0; k < n; ++k) c.x(info[k].DestBuffer).u(info[k].InfoType);
    }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* desc, UINT n,
        const D3D12_GPU_VIRTUAL_ADDRESS* sources) override {
        Call(this, "EmitRaytracingAccelerationStructurePostbuildInfo").x(desc->DestBuffer).u(desc->InfoType).u(n)
            .raw(sources, n * sizeof(*sources));
    }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
        D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) override {
        Call(this, "CopyRaytracingAccelerationStructure").x(dst).x(src).u(mode);
    }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* state) override {
        Call(this, "SetPipelineState1").obj(state);
    }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) override {
        Call(this, "DispatchRays").x(d->RayGenerationShaderRecord.StartAddress)
            .u(d->RayGenerationShaderRecord.SizeInBytes)
            .x(d->MissShaderTable.StartAddress).u(d->MissShaderTable.SizeInBytes).u(d->MissShaderTable.StrideInBytes)
            .x(d->HitGroupTable.StartAddress).u(d->HitGroupTable.SizeInBytes).u(d->HitGroupTable.StrideInBytes)
            .x(d->CallableShaderTable.StartAddress).u(d->CallableShaderTable.SizeInBytes)
            .u(d->CallableShaderTable.StrideInBytes).u(d->Width).u(d->Height).u(d->Depth);
    }
    // ID3D12GraphicsCommandList5
    void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE rate,
                                            const D3D12_SHADING_RATE_COMBINER* combiners) override {
        Call(this, "RSSetShadingRate").u(rate)
            .raw(combiners, D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT * sizeof(*combiners));
    }
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource* image) override {
        Call(this, "RSSetShadingRateImage").obj(image);
    }
};

Call::Call(FakeList* list, const char* name) : l_(list), text_(!list->quiet) {
    if (l_->inside.fetch_add(1) != 0) ++l_->crossed;
    if (!l_->refs.load()) ++l_->stale;
    const bool own = GetCurrentThreadId() == l_->caller.load();
    if (l_->gate && !own && WaitForSingleObject(l_->gate, 5000) != WAIT_OBJECT_0) ++l_->gate_timeouts;
    spin(l_->delay_ns);
    ++l_->calls;
    if (own) ++l_->caller_calls;
    if (text_) {
        l_->log += name;
        l_->log += '(';
    }
}
Call::~Call() {
    if (text_) l_->log += ")\n";
    l_->inside.fetch_sub(1);
}
void Call::put(const char* format, ...) {
    char text[64];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n > 0) l_->log.append(text, static_cast<size_t>(n) < sizeof(text) ? static_cast<size_t>(n) : sizeof(text) - 1);
}
Call& Call::u(uint64_t v) {
    if (text_) put("%llu,", static_cast<unsigned long long>(v));
    return *this;
}
Call& Call::i(int64_t v) {
    if (text_) put("%lld,", static_cast<long long>(v));
    return *this;
}
Call& Call::f(double v) {
    if (text_) put("%.9g,", v);
    return *this;
}
Call& Call::x(uint64_t v) {
    if (text_) put("%llx,", static_cast<unsigned long long>(v));
    return *this;
}
Call& Call::s(const char* text) {
    if (text_) {
        l_->log += text;
        l_->log += ',';
    }
    return *this;
}
Call& Call::obj(const void* p) {
    const auto* t = static_cast<const Thing*>(p);
    if (t && !t->refs.load()) ++l_->stale;
    if (text_) {
        if (t) put("#%u,", t->id);
        else s("0");
    }
    return *this;
}
Call& Call::raw(const void* p, size_t bytes) {
    if (!text_) return *this;
    if (!p) return s("null");
    static const char hex[] = "0123456789abcdef";
    const auto* b = static_cast<const uint8_t*>(p);
    l_->log += '[';
    for (size_t k = 0; k < bytes; ++k) {
        l_->log += hex[b[k] >> 4];
        l_->log += hex[b[k] & 15];
    }
    l_->log += "],";
    return *this;
}
Call& Call::cpu(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT n) {
    if (!text_) return *this;
    if (!handle.ptr) return s("null");
    return raw(reinterpret_cast<const void*>(handle.ptr), size_t{n} * kDescriptor);
}

// Records the engine calls of the watched list at its Reset and at its final Release.
class FakeAllocator final : public Child<ID3D12CommandAllocator> {
public:
    FakeList* watched = nullptr;
    std::atomic<uint64_t> calls_at_reset{UINT64_MAX}, calls_at_release{UINT64_MAX};
    HRESULT STDMETHODCALLTYPE Reset() override {
        calls_at_reset = watched ? watched->calls.load() : 0;
        return S_OK;
    }
    void final_release() override { calls_at_release = watched ? watched->calls.load() : 0; }
};

class FakeDevice;
// A replay ring's snapshot heap: kDescriptor bytes per descriptor, CPU addresses as handles.
class FakeHeap final : public Child<ID3D12DescriptorHeap> {
public:
    FakeHeap(FakeDevice* owner, const D3D12_DESCRIPTOR_HEAP_DESC& d)
        : device_(owner), desc_(d), words_(d.NumDescriptors) {}
    void final_release() override;
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return {reinterpret_cast<SIZE_T>(words_.data())};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override { return {0}; }

private:
    FakeDevice* device_;
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    std::vector<uint64_t> words_;
};

// The engine device: descriptor heaps, descriptor copies and command allocators. Anything else counts as unexpected.
class FakeDevice final : public Object<ID3D12Device> {
public:
    bool fail_heaps = false;
    std::atomic<uint32_t> heaps_made{0}, heaps_live{0};
    std::atomic<uint64_t> copies{0}, unexpected{0};
    FakeAllocator allocators[64];
    std::atomic<uint32_t> allocators_made{0};

    template <class T> T odd(T value) {
        ++unexpected;
        return value;
    }
    HRESULT none(void** out) {
        if (out) *out = nullptr;
        return odd(E_NOTIMPL);
    }
    UINT STDMETHODCALLTYPE GetNodeCount() override { return odd(1u); }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID riid, void** out) override {
        const uint32_t n = allocators_made++;
        if (riid != __uuidof(ID3D12CommandAllocator) || n >= std::size(allocators)) {
            *out = nullptr;
            return E_OUTOFMEMORY;
        }
        *out = static_cast<ID3D12CommandAllocator*>(&allocators[n]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID,
                                                          void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID,
                                                         void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                ID3D12PipelineState*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE, void*, UINT) override { return odd(E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid,
                                                   void** out) override {
        *out = nullptr;
        if (fail_heaps || riid != __uuidof(ID3D12DescriptorHeap)) return E_OUTOFMEMORY;
        auto* heap = new FakeHeap(this, *d);
        ++heaps_made;
        ++heaps_live;
        *out = static_cast<ID3D12DescriptorHeap*>(heap);
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override { return kDescriptor; }
    Thing root_signature;                                   // what CreateRootSignature hands out (BD-046)
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void*, SIZE_T, REFIID, void** out) override {
        if (!out) return E_INVALIDARG;
        root_signature.id = 900;
        root_signature.AddRef();
        *out = &root_signature;
        return S_OK;
    }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource*, ID3D12Resource*,
                                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override { odd(0); }
    void STDMETHODCALLTYPE CopyDescriptors(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                           D3D12_DESCRIPTOR_HEAP_TYPE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE) override {
        std::memcpy(reinterpret_cast<void*>(dst.ptr), reinterpret_cast<const void*>(src.ptr), size_t{n} * kDescriptor);
        ++copies;
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT,
                                                                               const D3D12_RESOURCE_DESC*) override {
        return odd(D3D12_RESOURCE_ALLOCATION_INFO{});
    }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE) override {
        return odd(D3D12_HEAP_PROPERTIES{});
    }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                      const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC*, REFIID, void** out) override { return none(out); }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                   void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR,
                                                 HANDLE*) override {
        return odd(E_NOTIMPL);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void** out) override { return none(out); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) override { return odd(E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override { return odd(E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override { return odd(E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void** out) override { return none(out); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return odd(S_OK); }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC*, UINT, UINT, UINT64,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT*, UINT*, UINT64*,
                                                 UINT64*) override {
        odd(0);
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return odd(E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID,
                                                     void** out) override {
        return none(out);
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*,
                                             UINT, D3D12_SUBRESOURCE_TILING*) override {
        odd(0);
    }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return odd(LUID{}); }
};

void FakeHeap::final_release() {
    --device_->heaps_live;
    delete this;
}

class FakeFence final : public Child<ID3D12Fence> {
public:
    std::atomic<UINT64> value{0};
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return value.load(); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 v) override {
        value = v;
        return S_OK;
    }
};

// An engine queue whose work completes at once. It records the engine calls of the first list it is given.
class FakeQueue final : public Child<ID3D12CommandQueue> {
public:
    uint64_t calls_at_execute = UINT64_MAX;
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*,
                                              const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
                                              const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*,
                                              D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*,
                                            const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
                                            D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList* const* lists) override {
        if (n) calls_at_execute = static_cast<FakeList*>(lists[0])->calls.load();
    }
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override { return fence->Signal(value); }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*, UINT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override {
        *frequency = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override {
        *gpu = *cpu = 0;
        return S_OK;
    }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return {}; }
};

// Records the engine calls of the watched list when the stack size is set.
class FakeProperties final : public Unknown<ID3D12StateObjectProperties> {
public:
    FakeList* watched = nullptr;
    UINT64 size = 0;
    uint64_t calls_at_set = UINT64_MAX;
    void* STDMETHODCALLTYPE GetShaderIdentifier(LPCWSTR) override { return nullptr; }
    UINT64 STDMETHODCALLTYPE GetShaderStackSize(LPCWSTR) override { return 0; }
    UINT64 STDMETHODCALLTYPE GetPipelineStackSize() override { return size; }
    void STDMETHODCALLTYPE SetPipelineStackSize(UINT64 bytes) override {
        size = bytes;
        calls_at_set = watched ? watched->calls.load() : 0;
    }
};
#pragma warning(pop)

// ---- Shell, tables and records ------------------------------------------------------------------------------------
struct Shell {
    std::atomic<uint32_t> device_errors{0}, list_errors{0}, started{0}, ended{0}, drained{0};
    std::atomic<HRESULT> list_error{S_OK};
};
void APIENTRY device_error(void* shell, HRESULT) { ++static_cast<Shell*>(shell)->device_errors; }
void APIENTRY list_error(void* shell, D3D12DDI_HRTCOMMANDLIST, HRESULT hr) {
    auto* s = static_cast<Shell*>(shell);
    s->list_error = hr;
    ++s->list_errors;
}
BOOL APIENTRY device_lost(void*) { return FALSE; }
HRESULT APIENTRY bind_table(void*, D3D12DDI_HRTCOMMANDLIST, uint32_t) { return S_OK; }
void APIENTRY replay_worker(void* shell, ReplayBody body, void* ring) {
    auto* s = static_cast<Shell*>(shell);
    ++s->started;
    body(ring);
    ++s->ended;
}
void APIENTRY replay_drained(void* shell) { ++static_cast<Shell*>(shell)->drained; }

D3D12DDI_DEVICE_FUNCS_CORE_0088 g_core{};
D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 g_list{};             // the graphics table
DeviceContext* APIENTRY resolve_device(D3D12DDI_HDEVICE device) {
    return static_cast<DeviceContext*>(device.pDrvPrivate);
}

// A list record over a fake engine list, as create_list leaves it: closed until its first Reset.
struct ListBox {
    FakeList engine;
    CommandListRecord record;
    D3D12DDI_HCOMMANDLIST h;
    ListBox(DeviceContext* c, D3D12_COMMAND_LIST_TYPE kind, uint32_t number)
        : record{{Tag::CommandList, 0, &engine, c}, {&engine}, kind, 1, false}, h{&record} {
        engine.type = kind;
        engine.id = number;
    }
};
struct PoolBox {
    CommandPoolRecord pool{};
    CommandRecorderRecord recorder{};
};

// One device context over the fake device, with a pool and a recorder for DIRECT lists. The destructor turns the
// policy off (joins the workers) before the lists it owns go away.
struct Fixture {
    Shell shell;
    FakeDevice device;
    DeviceContext context;
    D3D12DDI_HDEVICE hdevice{};
    PoolBox pool;
    std::vector<std::unique_ptr<ListBox>> lists;
    Fixture() {
        context.hooks = {sizeof(ShellHooks), &shell, device_error, list_error, device_lost, bind_table, nullptr,
                         nullptr};
        context.device = &device;
        for (UINT& n : context.increments) n = kDescriptor;
        hdevice.pDrvPrivate = &context;
        make_pool(pool);
    }
    ~Fixture() { off(); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    bool on(uint32_t rings = 8, uint32_t bytes = 4u << 20) {
        const ReplayPolicy p{sizeof(ReplayPolicy), 1, rings, bytes, &shell, replay_worker, replay_drained};
        return set_replay_policy(&context, &p) == S_OK;
    }
    bool off() {
        ReplayPolicy p{};
        p.size = sizeof(p);
        return set_replay_policy(&context, &p) == S_OK;
    }
    Replay* replay() const { return context.replay; }
    uint64_t calls(Drain kind) const { return context.replay->drains[static_cast<size_t>(kind)].calls.load(); }
    uint64_t waits(Drain kind) const { return context.replay->drains[static_cast<size_t>(kind)].waits.load(); }
    void make_pool(PoolBox& p) {
        const D3D12DDIARG_CREATE_COMMAND_POOL_0040 pool_args{D3D12DDI_COMMAND_POOL_FLAG_NONE};
        const D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 recorder_args{D3D12DDI_COMMAND_QUEUE_FLAG_3D,
                                                                     D3D12DDI_COMMAND_RECORDER_FLAG_NONE};
        const bool ok = g_core.pfnCreateCommandPool(hdevice, &pool_args, {&p.pool}) == S_OK &&
                        g_core.pfnCreateCommandRecorder(hdevice, &recorder_args, {&p.recorder}) == S_OK;
        g_core.pfnCommandRecorderSetCommandPoolAsTarget(hdevice, {&p.recorder}, {&p.pool});
        if (!ok) check(false, "fixture: pool and recorder created");
    }
    ListBox& list(D3D12_COMMAND_LIST_TYPE kind = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        lists.push_back(std::make_unique<ListBox>(&context, kind, static_cast<uint32_t>(lists.size() + 1)));
        return *lists.back();
    }
    void reset(const ListBox& l, PoolBox* p = nullptr) {
        D3D12DDIARG_RESETCOMMANDLIST_0040 args{};
        args.hDrvCommandRecorder.pDrvPrivate = &(p ? p : &pool)->recorder;
        g_list.pfnResetCommandList(l.h, &args);
    }
    // Close, then the list's drain, so that the checks after it read a complete list: with the policy on, the engine
    // Close is the list's last entry, made by the worker (close_list). The drain is of a kind no DDI call makes, so
    // the drain counts the checks read stay those of the DDI calls; it reports a failed Close as the next drain would.
    void close(const ListBox& l) {
        g_list.pfnCloseCommandList(l.h);
        drain_list(const_cast<CommandListRecord*>(&l.record), Drain::Teardown);
    }
    void draws(const ListBox& l, UINT first, UINT count) {
        for (UINT k = 0; k < count; ++k) g_list.pfnDrawInstanced(l.h, first + k, 1, 0, 0);
    }
};

// Records of the objects the slots name, each over its own Thing, as the create slots leave them.
struct Objects {
    Thing things[24];
    uint32_t used = 0;
    ResourceRecord texture{}, texture2{}, buffer{}, buffer2{}, tiled{}, predicate{}, arguments{}, counts{};
    ResourceRecord rate_image{};
    QueryHeapRecord queries{};
    RootSignatureRecord signature{};
    PipelineRecord graphics{}, compute{};
    CommandSignatureRecord command_signature{};
    DescriptorHeapRecord view_heap{}, sampler_heap{};
    FakeProperties properties;
    StateObjectRecord state_object{};
    explicit Objects(DeviceContext* c) {
        resource(c, texture, D3D12_RESOURCE_DIMENSION_TEXTURE2D, ResourceKind::Placed);
        resource(c, texture2, D3D12_RESOURCE_DIMENSION_TEXTURE2D, ResourceKind::Placed);
        resource(c, buffer, D3D12_RESOURCE_DIMENSION_BUFFER, ResourceKind::Placed);
        resource(c, buffer2, D3D12_RESOURCE_DIMENSION_BUFFER, ResourceKind::Placed);
        resource(c, tiled, D3D12_RESOURCE_DIMENSION_TEXTURE2D, ResourceKind::Reserved);
        resource(c, predicate, D3D12_RESOURCE_DIMENSION_BUFFER, ResourceKind::Placed);
        resource(c, arguments, D3D12_RESOURCE_DIMENSION_BUFFER, ResourceKind::Placed);
        resource(c, counts, D3D12_RESOURCE_DIMENSION_BUFFER, ResourceKind::Placed);
        resource(c, rate_image, D3D12_RESOURCE_DIMENSION_TEXTURE2D, ResourceKind::Placed);
        queries.h = header(c, Tag::QueryHeap);
        queries.type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        queries.count = 16;
        signature.h = header(c, Tag::RootSignature);
        graphics.h = header(c, Tag::PipelineState);
        compute.h = header(c, Tag::PipelineState);
        compute.compute = true;
        command_signature.h = header(c, Tag::CommandSignature);
        command_signature.stride = 16;
        view_heap.h = header(c, Tag::DescriptorHeap);
        view_heap.type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sampler_heap.h = header(c, Tag::DescriptorHeap);
        sampler_heap.type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        state_object.h = header(c, Tag::StateObject);
        state_object.properties = &properties;
        state_object.executable = true;
    }
    RecordHeader header(DeviceContext* c, Tag tag) {
        Thing& t = things[used++];
        t.id = used;
        return RecordHeader{tag, 0, &t, c};
    }
    void resource(DeviceContext* c, ResourceRecord& r, D3D12_RESOURCE_DIMENSION dimension, ResourceKind kind) {
        r.h = header(c, Tag::Resource);
        r.desc.Dimension = dimension;
        r.kind = kind;
    }
};

// What the slots read through pointers, in one block.
struct Data {
    uint64_t descriptors[8];
    D3D12DDI_CPU_DESCRIPTOR_HANDLE handles[8];          // handles[k] points to descriptors[k]
    D3D12DDI_VIEWPORT viewports[4];
    D3D12DDI_RECT rects[4];
    FLOAT floats[4];
    UINT uints[16];
    D3D12DDI_INDEX_BUFFER_VIEW index_view;
    D3D12DDI_VERTEX_BUFFER_VIEW vertex_views[3];
    D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW so_views[3];
    D3D12DDI_SAMPLE_POSITION positions[8];
    D3D12DDI_SHADING_RATE_COMBINER_0062 combiners[2];
    D3D12DDI_TILED_RESOURCE_COORDINATE coordinate;
    D3D12DDI_TILE_REGION_SIZE region;
    D3D12DDI_BOX box;
    D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint;
    D3D12DDIARG_RESOURCE_BARRIER_0022 barriers[3];
    D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054 geometries[2];
    const D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054* geometry_pointers[2];
    D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC_0054 postbuild[2];
    D3D12DDI_GPU_VIRTUAL_ADDRESS addresses[3];
    D3D12_DISPATCH_RAYS_DESC rays;
};
// prepare(n) fills the block with bytes that depend on n, then makes valid what the slots validate; garble()
// overwrites all of it once the slot has returned.
struct Sources {
    Objects& o;
    Data d;
    explicit Sources(Objects& objects) : o(objects), d{} { prepare(0); }
    void prepare(uint32_t n) {
        auto* bytes = reinterpret_cast<uint8_t*>(&d);
        for (size_t k = 0; k < sizeof(d); ++k) bytes[k] = static_cast<uint8_t>(n * 151u + k * 13u + (k >> 5));
        for (size_t k = 0; k < std::size(d.handles); ++k) {
            d.descriptors[k] = 0xD000000000000000ull | (uint64_t{n} << 8) | k;    // unique to the call: the bytes
            d.handles[k].ptr = reinterpret_cast<SIZE_T>(&d.descriptors[k]);     // above repeat every 256 calls
        }
        d.box = {1, 2, 0, static_cast<LONG>(3 + n % 5), static_cast<LONG>(4 + n % 3), 1};
        d.footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.footprint.PhysicalDepth = 1;
        D3D12DDIARG_RESOURCE_BARRIER_0022& transition = d.barriers[0];
        transition.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_NONE;
        transition.Transition.hResource.pDrvPrivate = &o.texture;
        transition.Transition.Subresource = n;
        transition.Transition.StateBefore = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
        transition.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        d.barriers[1].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
        d.barriers[1].Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_NONE;
        d.barriers[1].UAV.hResource.pDrvPrivate = &o.buffer;
        d.barriers[2].Type = D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING;
        d.barriers[2].Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_NONE;
        d.barriers[2].Ranged.hResource.pDrvPrivate = &o.texture2;
        d.geometries[0].Type = D3D12DDI_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.geometries[1].Type = D3D12DDI_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        d.geometry_pointers[0] = &d.geometries[1];
        d.geometry_pointers[1] = &d.geometries[0];
        d.postbuild[0].InfoType = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        d.postbuild[1].InfoType = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
    }
    void garble() { std::memset(&d, 0xA5, sizeof(d)); }
};

// The engine methods the converted slots reach; the exactness sequence must reach each of them.
constexpr const char* kMethods[] = {
    "DrawInstanced", "DrawIndexedInstanced", "Dispatch", "ClearUnorderedAccessViewUint",
    "ClearUnorderedAccessViewFloat", "ClearRenderTargetView", "ClearDepthStencilView", "DiscardResource",
    "CopyTextureRegion", "CopyResource", "CopyTiles", "CopyBufferRegion", "ResolveSubresource", "ExecuteIndirect",
    "ResourceBarrier", "BeginQuery", "EndQuery", "ResolveQueryData", "SetPredication", "IASetPrimitiveTopology",
    "RSSetViewports", "RSSetScissorRects", "OMSetBlendFactor", "OMSetStencilRef", "SetPipelineState",
    "SetDescriptorHeaps", "SetComputeRootSignature", "SetGraphicsRootSignature", "SetComputeRootDescriptorTable",
    "SetGraphicsRootDescriptorTable", "SetComputeRoot32BitConstant", "SetGraphicsRoot32BitConstant",
    "SetComputeRoot32BitConstants", "SetGraphicsRoot32BitConstants", "SetComputeRootConstantBufferView",
    "SetGraphicsRootConstantBufferView", "SetComputeRootShaderResourceView", "SetGraphicsRootShaderResourceView",
    "SetComputeRootUnorderedAccessView", "SetGraphicsRootUnorderedAccessView", "IASetIndexBuffer",
    "IASetVertexBuffers", "SOSetTargets", "OMSetRenderTargets", "OMSetDepthBounds", "SetSamplePositions",
    "ResolveSubresourceRegion", "SetViewInstanceMask", "BuildRaytracingAccelerationStructure",
    "EmitRaytracingAccelerationStructurePostbuildInfo", "CopyRaytracingAccelerationStructure", "SetPipelineState1",
    "DispatchRays", "RSSetShadingRate", "RSSetShadingRateImage",
};
static_assert(std::size(kMethods) == 55, "converted engine methods");

// Every converted recording slot, some twice (with and without the optional pointers), on one list. Each call gets
// fresh sources, garbled as soon as the slot returns.
void sequence(const ListBox& l, Objects& o, Sources& s) {
    const D3D12DDI_HCOMMANDLIST h = l.h;
    Data& d = s.d;
    UINT n = 0;
    auto step = [&](auto call) {
        s.prepare(++n);
        call();
        s.garble();
    };
    auto res = [](ResourceRecord& r) { return D3D12DDI_HRESOURCE{&r}; };
    auto at = [](ResourceRecord& r, UINT64 offset) {
        D3D12DDIARG_BUFFER_PLACEMENT p{};
        p.BaseAddress.UMD.hResource.pDrvPrivate = &r;
        p.BaseAddress.UMD.Offset = offset;
        return p;
    };
    auto cpu = [&](size_t k) { return D3D12DDI_CPU_DESCRIPTOR_HANDLE{reinterpret_cast<SIZE_T>(&d.descriptors[k])}; };
    const D3D12DDI_HCOMMANDSIGNATURE signature{&o.command_signature};
    const D3D12DDI_HQUERYHEAP queries{&o.queries};

    step([&] { g_list.pfnDrawInstanced(h, n, n + 1, n + 2, n + 3); });
    step([&] { g_list.pfnDrawIndexedInstanced(h, n, 2, 3, -4, 5); });
    step([&] { g_list.pfnDispatch(h, n, 2, 3); });
    step([&] {
        g_list.pfnClearUnorderedAccessViewUint(h, D3D12DDI_GPU_DESCRIPTOR_HANDLE{0x100000u + n}, cpu(3), res(o.texture),
                                               d.uints, 2, d.rects);
    });
    step([&] {
        g_list.pfnClearUnorderedAccessViewFloat(h, D3D12DDI_GPU_DESCRIPTOR_HANDLE{0x200000u + n}, cpu(4), res(o.buffer),
                                                d.floats, 0, nullptr);
    });
    step([&] { g_list.pfnClearRenderTargetView(h, cpu(5), d.floats, 3, d.rects); });
    step([&] { g_list.pfnClearDepthStencilView(h, cpu(6), 3u, 0.25f, 7, 1, d.rects); });
    step([&] { g_list.pfnDiscardResource(h, res(o.texture), nullptr); });
    step([&] {
        const D3D12DDIARG_DISCARD_RESOURCE_0003 region{2, d.rects, 1, 3};
        g_list.pfnDiscardResource(h, res(o.texture2), &region);
    });
    step([&] {
        const D3D12DDIARG_BUFFER_PLACEMENT dst = at(o.texture, 2), src = at(o.buffer, 256);
        const D3D12DDIARG_PLACED_RESOURCE dst_layout{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
        const D3D12DDIARG_PLACED_RESOURCE src_layout{D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &d.footprint};
        g_list.pfnCopyTextureRegion(h, &dst, dst_layout, 1, 2, 3, &src, src_layout, &d.box);
    });
    step([&] {
        const D3D12DDIARG_BUFFER_PLACEMENT dst = at(o.texture2, 0), src = at(o.texture, 1);
        const D3D12DDIARG_PLACED_RESOURCE layout{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
        g_list.pfnCopyTextureRegion(h, &dst, layout, 0, 0, 0, &src, layout, nullptr);
    });
    step([&] { g_list.pfnResourceCopy(h, res(o.texture2), res(o.texture)); });
    step([&] {
        g_list.pfnCopyTiles(h, res(o.tiled), &d.coordinate, &d.region, res(o.buffer), 4096,
                            D3D12DDI_TILE_COPY_FLAG_NONE);
    });
    step([&] { g_list.pfnCopyBufferRegion(h, at(o.buffer2, 16), at(o.buffer, 32), 64 + n); });
    step([&] {
        g_list.pfnResourceResolveSubresource(h, res(o.texture2), 0, res(o.texture), 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    });
    step([&] { g_list.pfnExecuteIndirect(h, signature, 8, at(o.arguments, 64), at(o.counts, 4)); });
    step([&] { g_list.pfnExecuteIndirect(h, signature, 4, at(o.arguments, 0), D3D12DDIARG_BUFFER_PLACEMENT{}); });
    step([&] { g_list.pfnResourceBarrier(h, 3, d.barriers); });
    step([&] { g_list.pfnBeginQuery(h, queries, D3D12DDI_QUERY_TYPE_OCCLUSION, 3); });
    step([&] { g_list.pfnEndQuery(h, queries, D3D12DDI_QUERY_TYPE_OCCLUSION, 3); });
    step([&] { g_list.pfnResolveQueryData(h, queries, D3D12DDI_QUERY_TYPE_OCCLUSION, 2, 4, res(o.buffer), 128); });
    step([&] { g_list.pfnSetPredication(h, res(o.predicate), 8, D3D12DDI_PREDICATION_OP_EQUAL_ZERO); });
    step([&] { g_list.pfnSetPredication(h, D3D12DDI_HRESOURCE{}, 0, D3D12DDI_PREDICATION_OP_EQUAL_ZERO); });
    step([&] { g_list.pfnIaSetTopology(h, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST); });
    step([&] { g_list.pfnRsSetViewports(h, 3, d.viewports); });
    step([&] { g_list.pfnRsSetViewports(h, 0, nullptr); });
    step([&] { g_list.pfnRsSetScissorRects(h, 2, d.rects); });
    step([&] { g_list.pfnOmSetBlendFactor(h, d.floats); });
    step([&] { g_list.pfnOmSetBlendFactor(h, nullptr); });
    step([&] { g_list.pfnOmSetStencilRef(h, n); });
    step([&] { g_list.pfnSetPipelineState(h, D3D12DDI_HPIPELINESTATE{&o.graphics}); });
    step([&] { g_list.pfnSetPipelineState(h, D3D12DDI_HPIPELINESTATE{&o.compute}); });
    step([&] {
        D3D12DDI_HDESCRIPTORHEAP heaps[2] = {{&o.view_heap}, {&o.sampler_heap}};
        g_list.pfnSetDescriptorHeaps(h, 2, heaps);
    });
    step([&] { g_list.pfnSetComputeRootSignature(h, D3D12DDI_HROOTSIGNATURE{&o.signature}); });
    step([&] { g_list.pfnSetGraphicsRootSignature(h, D3D12DDI_HROOTSIGNATURE{&o.signature}); });
    step([&] { g_list.pfnSetGraphicsRootSignature(h, D3D12DDI_HROOTSIGNATURE{}); });
    step([&] { g_list.pfnSetComputeRootDescriptorTable(h, 1, D3D12DDI_GPU_DESCRIPTOR_HANDLE{0xABC000u + n}); });
    step([&] { g_list.pfnSetGraphicsRootDescriptorTable(h, 2, D3D12DDI_GPU_DESCRIPTOR_HANDLE{0xDEF000u + n}); });
    step([&] { g_list.pfnSetComputeRoot32BitConstant(h, 3, n, 4); });
    step([&] { g_list.pfnSetGraphicsRoot32BitConstant(h, 4, n, 5); });
    step([&] { g_list.pfnSetComputeRoot32BitConstants(h, 0, 13, d.uints, 1); });
    step([&] { g_list.pfnSetGraphicsRoot32BitConstants(h, 1, 16, d.uints, 0); });
    step([&] { g_list.pfnSetComputeRootConstantBufferView(h, 5, 0x10000u + n); });
    step([&] { g_list.pfnSetGraphicsRootConstantBufferView(h, 6, 0x20000u + n); });
    step([&] { g_list.pfnSetComputeRootShaderResourceView(h, 7, 0x30000u + n); });
    step([&] { g_list.pfnSetGraphicsRootShaderResourceView(h, 8, 0x40000u + n); });
    step([&] { g_list.pfnSetComputeRootUnorderedAccessView(h, 9, 0x50000u + n); });
    step([&] { g_list.pfnSetGraphicsRootUnorderedAccessView(h, 10, 0x60000u + n); });
    step([&] { g_list.pfnIASetIndexBuffer(h, &d.index_view); });
    step([&] { g_list.pfnIASetIndexBuffer(h, nullptr); });
    step([&] { g_list.pfnIASetVertexBuffers(h, 1, 3, d.vertex_views); });
    step([&] { g_list.pfnIASetVertexBuffers(h, 0, 2, nullptr); });
    step([&] { g_list.pfnSOSetTargets(h, 0, 3, d.so_views); });
    step([&] { g_list.pfnSOSetTargets(h, 1, 3, nullptr); });
    step([&] { g_list.pfnOMSetRenderTargets(h, 3, d.handles, FALSE, &d.handles[7]); });
    step([&] { g_list.pfnOMSetRenderTargets(h, 4, d.handles, TRUE, nullptr); });
    step([&] { g_list.pfnOMSetRenderTargets(h, 0, nullptr, FALSE, &d.handles[6]); });
    step([&] { g_list.pfnOMSetDepthBounds(h, 0.25f, 0.75f); });
    step([&] { g_list.pfnSetSamplePositions(h, 2, 4, d.positions); });
    step([&] {
        g_list.pfnResourceResolveSubresourceRegion(h, res(o.texture2), 0, 4, 5, res(o.texture), 1, &d.rects[1],
                                                   DXGI_FORMAT_R8G8B8A8_UNORM, D3D12DDI_RESOLVE_MODE_AVERAGE);
    });
    step([&] {
        g_list.pfnResourceResolveSubresourceRegion(h, res(o.texture2), 1, 0, 0, res(o.texture), 0, nullptr,
                                                   DXGI_FORMAT_R16_FLOAT, D3D12DDI_RESOLVE_MODE_MIN);
    });
    step([&] { g_list.pfnSetViewInstanceMask(h, 5 + n); });
    step([&] {     // a bottom level from an array, with two postbuild descriptions
        D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 a{};
        a.DestAccelerationStructureData = 0x700000u + n;
        a.Inputs.Type = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        a.Inputs.Flags = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        a.Inputs.NumDescs = 2;
        a.Inputs.DescsLayout = D3D12DDI_ELEMENTS_LAYOUT_ARRAY;
        a.Inputs.pGeometryDescs = d.geometries;
        a.ScratchAccelerationStructureData = 0x710000;
        a.NumPostbuildInfoDescs = 2;
        a.pPostbuildInfoDescs = d.postbuild;
        g_list.pfnBuildRaytracingAccelerationStructure(h, &a);
    });
    step([&] {     // a bottom level from an array of pointers, updating a source
        D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 a{};
        a.DestAccelerationStructureData = 0x720000u + n;
        a.Inputs.Type = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        a.Inputs.Flags = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
        a.Inputs.NumDescs = 2;
        a.Inputs.DescsLayout = D3D12DDI_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
        a.Inputs.ppGeometryDescs = d.geometry_pointers;
        a.SourceAccelerationStructureData = 0x730000;
        a.ScratchAccelerationStructureData = 0x740000;
        g_list.pfnBuildRaytracingAccelerationStructure(h, &a);
    });
    step([&] {     // a top level: its instances are a GPU address
        D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 a{};
        a.DestAccelerationStructureData = 0x750000u + n;
        a.Inputs.Type = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        a.Inputs.NumDescs = 7;
        a.Inputs.DescsLayout = D3D12DDI_ELEMENTS_LAYOUT_ARRAY;
        a.Inputs.InstanceDescs = 0x760000;
        a.ScratchAccelerationStructureData = 0x770000;
        a.NumPostbuildInfoDescs = 1;
        a.pPostbuildInfoDescs = &d.postbuild[1];
        g_list.pfnBuildRaytracingAccelerationStructure(h, &a);
    });
    step([&] {
        const D3D12DDIARG_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_0054 a{
            {0x780000u + n, D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE}, 3, d.addresses};
        g_list.pfnEmitRaytracingAccelerationStructurePostbuildInfo(h, &a);
    });
    step([&] {
        const D3D12DDIARG_COPY_RAYTRACING_ACCELERATION_STRUCTURE_0054 a{
            0x790000u + n, 0x7A0000, D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE};
        g_list.pfnCopyRaytracingAccelerationStructure(h, &a);
    });
    step([&] { g_list.pfnSetPipelineState1(h, D3D12DDI_HSTATEOBJECT_0054{&o.state_object}); });
    step([&] { g_list.pfnDispatchRays(h, reinterpret_cast<const D3D12DDIARG_DISPATCH_RAYS_0054*>(&d.rays)); });
    step([&] { g_list.pfnRSSetShadingRate(h, D3D12DDI_SHADING_RATE_0062_2X2, d.combiners); });
    step([&] { g_list.pfnRSSetShadingRate(h, D3D12DDI_SHADING_RATE_0062_1X1, nullptr); });
    step([&] { g_list.pfnRSSetShadingRateImage(h, res(o.rate_image)); });
    step([&] { g_list.pfnRSSetShadingRateImage(h, D3D12DDI_HRESOURCE{}); });
}

// Opens a gate once a drain waits on one of the replay's rings: a drain that stops spinning lowers its ring's
// next_wake under the ring's lock. With the worker held at the gate until then, the drain provably had calls to wait
// for. If no drain waits within 5 s the gate opens anyway, and parked() says so. Made while no drain waits: it first
// clears a next_wake that an earlier drain left lowered when the worker passed its target between the drain's spin and
// its lock (the worker clears it after its next entry, which the gate would hold).
class Opener {
public:
    Opener(Replay* rp, HANDLE gate) {
        for (uint32_t i = 0; i < rp->count.load(); ++i) rp->rings[i]->next_wake.store(UINT64_MAX);
        thread_ = std::thread([this, rp, gate] { run(rp, gate); });
    }
    ~Opener() {
        if (thread_.joinable()) thread_.join();
    }
    Opener(const Opener&) = delete;
    Opener& operator=(const Opener&) = delete;
    bool parked() {
        if (thread_.joinable()) thread_.join();
        return parked_;
    }

private:
    void run(const Replay* rp, HANDLE gate) {
        const uint64_t until = qpc() + 5 * qpf();
        for (; !parked_ && qpc() < until; Sleep(1))
            for (uint32_t i = 0; i < rp->count.load(); ++i)
                if (rp->rings[i]->next_wake.load() != UINT64_MAX) parked_ = true;
        SetEvent(gate);                                 // the last access: the drain cannot end before it
    }
    bool parked_ = false;
    std::thread thread_;
};
// Makes call, which drains, while the worker is held at gate until the drain waits. True if the drain waited.
template <class F> bool held(Replay* rp, HANDLE gate, F&& call) {
    Opener open(rp, gate);
    call();
    return open.parked();
}

std::vector<std::string> lines_of(const std::string& log) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string::npos) end = log.size();
        out.push_back(log.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
// Empty when the logs are equal, else the first line where they differ.
std::string difference(const std::string& a, const std::string& b) {
    if (a == b) return {};
    const std::vector<std::string> x = lines_of(a), y = lines_of(b);
    size_t k = 0;
    while (k < x.size() && k < y.size() && x[k] == y[k]) ++k;
    return "line " + std::to_string(k + 1) + ": " + (k < x.size() ? x[k].substr(0, 160) : "(end)") + " | " +
           (k < y.size() ? y[k].substr(0, 160) : "(end)");
}
bool starts(const std::string& line, const char* prefix) { return line.rfind(prefix, 0) == 0; }
unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));        // engine-ddi's log lines, in order with the results
    SetEnvironmentVariableA("AMDGPU_WDDM_LOG", "stderr"); // the sink is off by default (stdio-log.h)
    // A drain that never ends would hang the build: give up after two minutes.
    std::thread([] {
        Sleep(120000);
        std::printf("FAIL  watchdog: the test has run for 120 s\n");
        std::fflush(stdout);
        _exit(3);
    }).detach();
    const DWORD self = GetCurrentThreadId();

    const FillInfo fill{sizeof(FillInfo), resolve_device};
    check(fill_device_core(&g_core, sizeof(g_core), &fill) == S_OK &&
              fill_command_list(&g_list, sizeof(g_list), 1, &fill) == S_OK,
          "tables filled");

    // 1. The policy's refusals; on, off and on again.
    {
        Fixture f;
        const ReplayPolicy p{sizeof(ReplayPolicy), 1, 4, 1u << 20, &f.shell, replay_worker, replay_drained};
        auto refused = [&](ReplayPolicy q) {
            return set_replay_policy(&f.context, &q) == E_INVALIDARG && !f.context.replay;
        };
        ReplayPolicy q = p;
        check(set_replay_policy(nullptr, &p) == E_INVALIDARG && set_replay_policy(&f.context, nullptr) == E_INVALIDARG,
              "policy: null context and null policy refused");
        q.size += 4;
        const bool size = refused(q);
        q = p;
        q.enabled = 2;
        const bool enabled = refused(q);
        q = p;
        q.rings = 0;
        bool rings = refused(q);
        q.rings = kMaxReplayRings + 1;
        rings = rings && refused(q);
        q = p;
        q.ring_bytes = 32u << 10;
        bool bytes = refused(q);
        q.ring_bytes = 128u << 20;
        bytes = bytes && refused(q);
        q.ring_bytes = 96u << 10;
        bytes = bytes && refused(q);
        q = p;
        q.worker = nullptr;
        bool hooks = refused(q);
        q = p;
        q.drained = nullptr;
        hooks = hooks && refused(q);
        check(size && enabled && rings && bytes && hooks,
              "policy: wrong size, enabled 2, rings 0 or 17, ring bytes 32 KiB, 128 MiB or 96 KiB, and a null hook "
              "refused, nothing set");
        check(set_replay_policy(&f.context, &p) == S_OK && f.context.replay, "policy: on");
        Replay* first = f.context.replay;
        check(set_replay_policy(&f.context, &p) == E_INVALIDARG && f.context.replay == first,
              "policy: on while on refused, the policy held kept");
        ReplayPolicy junk{};
        junk.size = sizeof(junk);
        junk.rings = 99;
        check(set_replay_policy(&f.context, &junk) == S_OK && !f.context.replay && f.off(),
              "policy: off (enabled 0 ignores the other fields), and off again");
        check(f.on() && f.context.replay && f.off() && !f.context.replay && !f.shell.started,
              "policy: on again and off; no ring and no thread without a recording call");
    }

    // 2. Exactness: the same calls and arguments with the policy off and on.
    {
        Fixture f;
        Objects o(&f.context);
        Sources s(o);
        ListBox& a = f.list();
        ListBox& b = f.list();
        a.engine.caller = self;
        b.engine.caller = self;
        f.reset(a);
        sequence(a, o, s);
        f.close(a);
        check(f.on(), "exactness: policy on");
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        f.reset(b);
        b.engine.gate = gate;
        sequence(b, o, s);
        const uint64_t held = b.engine.calls.load();
        SetEvent(gate);
        f.close(b);
        const std::string where = difference(a.engine.log, b.engine.log);
        const size_t lines = lines_of(b.engine.log).size();
        check(where.empty() && lines > 70, "exactness: %zu engine calls, the same with the policy off and on%s%s",
              lines, where.empty() ? "" : "; first difference at ", where.c_str());
        size_t covered = 0;
        std::string missing;
        for (const char* name : kMethods) {
            if (b.engine.log.find(std::string("\n") + name + "(") != std::string::npos) ++covered;
            else missing += std::string(" ") + name;
        }
        check(covered == std::size(kMethods), "exactness: %zu of %zu converted engine methods reached%s", covered,
              std::size(kMethods), missing.c_str());
        check(held == 1 && b.engine.caller_calls == 1 && !b.engine.gate_timeouts,
              "exactness: every recorded call and the Close ran on the worker after the sequence (engine calls before "
              "the gate opened %llu: the Reset; on the recording thread %llu: the Reset)",
              ull(held), ull(b.engine.caller_calls));
        check(!a.engine.unexpected && !b.engine.unexpected && !b.engine.crossed && !b.engine.stale &&
                  !f.shell.list_errors && !f.shell.device_errors && !f.device.unexpected,
              "exactness: no slot refused, no unexpected engine method, one call at a time, no released object named");
        check(f.device.heaps_made == 3 && f.device.copies > 0,
              "exactness: snapshot heaps of three types (%u), %llu descriptor copies", f.device.heaps_made.load(),
              ull(f.device.copies));
        check(f.off() && !f.device.heaps_live && f.shell.started == 1 && f.shell.ended == 1,
              "exactness: policy off, one worker started and joined, snapshot heaps released");
        CloseHandle(gate);
    }

    // 3. Drains. Each list's worker calls are held at the list's gate until the drain waits (held, Opener), so every
    //    drain below had calls to wait for; each check reads when the engine call the drain guards ran.
    {
        Fixture f;
        Objects o(&f.context);
        check(f.on(), "drains: policy on");
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto hold = [&](ListBox& l) {                               // before the list's Reset, made on this thread
            drain_all(&f.context, Drain::Teardown);                 // nothing of an earlier list waits at the gate
            ResetEvent(gate);
            l.engine.caller = self;
            l.engine.gate = gate;
        };

        // Close: the list's last entry. It returns with the worker still held, the list closed; the engine Close runs
        // after the list's calls, and the E_OUTOFMEMORY a deferred call latched there reaches the runtime from the
        // list's next drain, here its Reset.
        ListBox& a = f.list();
        hold(a);
        f.reset(a);
        f.draws(a, 1, 20);
        g_list.pfnOmSetStencilRef(a.h, FakeList::kLatchStencil);
        f.draws(a, 21, 5);
        const uint32_t errors = f.shell.list_errors.load();
        g_list.pfnCloseCommandList(a.h);
        const bool at_once = a.engine.calls == 1 && !a.record.recording && f.shell.list_errors == errors;
        const bool reopened = held(f.replay(), gate, [&] { f.reset(a); });
        const std::vector<std::string> a_lines = lines_of(a.engine.log);
        check(at_once && reopened && a.engine.calls == 29 && a_lines.size() == 29 && starts(a_lines[27], "Close(") &&
                  starts(a_lines[28], "Reset(") && f.shell.list_errors == errors + 1 &&
                  f.shell.list_error == E_OUTOFMEMORY && a.record.recording,
              "drains: Close returns before the list's 26 calls run, the list closed; the engine Close runs on the "
              "worker after them, and its E_OUTOFMEMORY reaches the runtime from the Reset that drains the list");

        // Reset of a list that is still recording.
        ListBox& r = f.list();
        hold(r);
        f.reset(r);
        f.draws(r, 1, 20);
        const bool reset = held(f.replay(), gate, [&] { f.reset(r); });
        const std::vector<std::string> r_lines = lines_of(r.engine.log);
        check(reset && r_lines.size() == 22 && starts(r_lines.back(), "Reset("),
              "drains: Reset runs after the list's 20 calls");
        f.close(r);

        // ExecuteBundle: the bundle and the parent drained, the engine call made in between in order.
        ListBox& bundle = f.list(D3D12_COMMAND_LIST_TYPE_BUNDLE);
        f.reset(bundle);
        f.draws(bundle, 1, 5);
        f.close(bundle);
        ListBox& parent = f.list();
        hold(parent);
        f.reset(parent);
        parent.engine.ordered = true;
        f.draws(parent, 1, 20);
        const bool bundled = held(f.replay(), gate, [&] { g_list.pfnExecuteBundle(parent.h, bundle.h); });
        f.draws(parent, 21, 3);
        f.close(parent);
        const std::vector<std::string> p_lines = lines_of(parent.engine.log);
        check(bundled && p_lines.size() == 26 &&
                  p_lines[21] == "ExecuteBundle(" + std::to_string(bundle.engine.id) + ",)" &&
                  !parent.engine.disorder && f.calls(Drain::Bundle) == 2 && f.waits(Drain::Bundle) == 1,
              "drains: ExecuteBundle after the parent's 20 calls and before its next 3 (bundle drains %llu, waited "
              "%llu)",
              ull(f.calls(Drain::Bundle)), ull(f.waits(Drain::Bundle)));

        // ExecuteCommandLists of a list that was not closed: the queue gets it with all its calls made.
        FakeQueue queue;
        FakeFence fence;
        EngineQueue q{};
        q.context = &f.context;
        q.queue = &queue;
        q.fence = &fence;
        q.id = 1;
        q.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        InitializeSRWLock(&q.submit_lock);
        f.context.queues[0] = &q;
        f.context.queue_mask = 1;
        ListBox& e = f.list();
        hold(e);
        f.reset(e);
        f.draws(e, 1, 20);
        HRESULT executed = E_FAIL;
        const bool submitted = held(f.replay(), gate, [&] { executed = execute_command_lists(&q, 1, &e.h); });
        check(submitted && executed == S_OK && queue.calls_at_execute == 21 && f.waits(Drain::Ecl) == 1,
              "drains: ExecuteCommandLists submits the list after its 20 calls (the queue saw %llu engine calls)",
              ull(queue.calls_at_execute));
        f.close(e);
        f.context.queues[0] = nullptr;
        f.context.queue_mask = 0;

        // The destroy of a list: its calls run before the engine list's final Release.
        ListBox& gone = f.list();
        hold(gone);
        f.reset(gone);
        f.draws(gone, 1, 20);
        const bool destroyed = held(f.replay(), gate, [&] { g_core.pfnDestroyCommandList(f.hdevice, gone.h); });
        check(destroyed && gone.engine.calls_at_release == 21 && !gone.engine.stale &&
                  gone.record.h.tag == Tag::Poisoned,
              "drains: DestroyCommandList releases the engine list after its 21 calls (%llu)",
              ull(gone.engine.calls_at_release));

        // A pool's reset and destroy: every ring, so the lists recording into its allocators are done with them.
        PoolBox pool;
        f.make_pool(pool);
        ListBox& user = f.list();
        hold(user);
        f.reset(user, &pool);
        auto* allocator = static_cast<FakeAllocator*>(pool.pool.allocators[D3D12_COMMAND_LIST_TYPE_DIRECT]);
        allocator->watched = &user.engine;
        f.draws(user, 1, 20);
        const bool pool_reset = held(f.replay(), gate, [&] { g_core.pfnResetCommandPool(f.hdevice, {&pool.pool}); });
        ResetEvent(gate);
        f.draws(user, 21, 20);
        const bool pool_destroyed =
            held(f.replay(), gate, [&] { g_core.pfnDestroyCommandPool(f.hdevice, {&pool.pool}); });
        check(pool_reset && pool_destroyed && allocator->calls_at_reset == 21 && allocator->calls_at_release == 41 &&
                  f.waits(Drain::Pool) == 2,
              "drains: ResetCommandPool and DestroyCommandPool reach the allocator after the calls before them (%llu, "
              "%llu)",
              ull(allocator->calls_at_reset), ull(allocator->calls_at_release));
        f.close(user);

        // With no list of the pool open, its reset and a Reset into it wait for the pool's pending Close only: the
        // engine Close reaches the allocator.
        PoolBox pool2;
        f.make_pool(pool2);
        ListBox& closer = f.list();
        hold(closer);
        f.reset(closer, &pool2);
        auto* allocator2 = static_cast<FakeAllocator*>(pool2.pool.allocators[D3D12_COMMAND_LIST_TYPE_DIRECT]);
        allocator2->watched = &closer.engine;
        f.draws(closer, 1, 20);
        g_list.pfnCloseCommandList(closer.h);
        const uint64_t pool_drains = f.calls(Drain::Pool), close_waits = f.waits(Drain::Closes);
        const bool closes_reset =
            held(f.replay(), gate, [&] { g_core.pfnResetCommandPool(f.hdevice, {&pool2.pool}); });
        const bool reset_after_close = allocator2->calls_at_reset == 22 && f.calls(Drain::Pool) == pool_drains &&
                                       f.waits(Drain::Closes) == close_waits + 1;
        hold(closer);
        f.reset(closer, &pool2);
        f.draws(closer, 1, 20);
        g_list.pfnCloseCommandList(closer.h);
        ListBox& next = f.list();
        next.engine.caller = self;
        const bool closes_list = held(f.replay(), gate, [&] { f.reset(next, &pool2); });
        const std::vector<std::string> c_lines = lines_of(closer.engine.log);
        check(closes_reset && reset_after_close && closes_list && starts(c_lines.back(), "Close(") &&
                  next.engine.calls == 1 && f.waits(Drain::Closes) == close_waits + 2,
              "drains: ResetCommandPool with no list of the pool open, and a Reset into the pool, wait for the pool's "
              "pending Close only (allocator reset after %llu calls, Close waits %llu)",
              ull(allocator2->calls_at_reset), ull(f.waits(Drain::Closes) - close_waits));
        f.close(next);
        g_core.pfnDestroyCommandPool(f.hdevice, {&pool2.pool});

        // SetPipelineStackSize: every ring, so the size lands after the calls recorded before it.
        ListBox& rt = f.list();
        hold(rt);
        f.reset(rt);
        f.draws(rt, 1, 20);
        o.properties.watched = &rt.engine;
        const bool sized = held(f.replay(), gate, [&] {
            g_core.pfnSetPipelineStackSize(D3D12DDI_HSTATEOBJECT_0054{&o.state_object}, 4096);
        });
        check(sized && o.properties.calls_at_set == 21 && o.properties.size == 4096 && f.waits(Drain::Stack) == 1,
              "drains: SetPipelineStackSize reaches the engine after the 21 calls before it (%llu)",
              ull(o.properties.calls_at_set));
        f.close(rt);

        uint64_t hooked = 0;
        for (Drain k :
             {Drain::Reset, Drain::Bundle, Drain::Ecl, Drain::Destroy, Drain::Pool, Drain::Closes, Drain::Stack})
            hooked += f.calls(k);
        check(hooked > 0 && f.shell.drained == hooked && !f.calls(Drain::Switch) && !f.calls(Drain::Direct),
              "drains: the drained hook ran once per drain of a DDI call (%llu), never for an encode-side drain",
              ull(hooked));
        uint64_t timeouts = 0;
        for (const auto& l : f.lists) timeouts += l->engine.gate_timeouts;
        check(f.off() && !f.device.heaps_live && !timeouts, "drains: policy off, no call waited out its gate");
        CloseHandle(gate);
    }

    // 4. Order across threads: a second thread records a list whose calls wait on the first thread's ring, held at
    //    the list's gate until the second thread waits for them.
    {
        Fixture f;
        check(f.on(), "switch: policy on");
        ListBox& w = f.list();
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w.engine.caller = self;
        w.engine.gate = gate;
        f.reset(w);
        w.engine.ordered = true;
        const HANDLE recorded = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread first([&] {
            f.draws(w, 1, 30);
            SetEvent(recorded);
            WaitForSingleObject(release, INFINITE);
        });
        WaitForSingleObject(recorded, INFINITE);
        const bool parked = held(f.replay(), gate, [&] {           // first is alive: second gets a ring of its own
            std::thread([&] { f.draws(w, 31, 30); }).join();
        });
        // Read before this thread's Close, which is an entry too: it gets this thread a ring and a switch of its own.
        const bool switched = f.replay()->count == 2 && f.calls(Drain::Switch) == 1 && f.waits(Drain::Switch) == 1;
        SetEvent(release);
        first.join();
        f.close(w);
        check(parked && switched && !w.engine.disorder && w.engine.calls == 62 && !w.engine.gate_timeouts,
              "switch: a second thread waits for the list's calls on the first thread's ring, and the order holds "
              "(calls %llu, out of order %llu)",
              ull(w.engine.calls), ull(w.engine.disorder));
        check(f.off(), "switch: policy off");
        CloseHandle(gate);
        CloseHandle(recorded);
        CloseHandle(release);
    }
    {
        Fixture f;
        check(f.on(1), "direct: policy on with one ring");
        ListBox& w = f.list();
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w.engine.caller = self;
        w.engine.gate = gate;
        f.reset(w);
        w.engine.ordered = true;
        w.engine.caller_calls = 0;
        const HANDLE recorded = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread first([&] {
            f.draws(w, 1, 30);
            SetEvent(recorded);
            WaitForSingleObject(release, INFINITE);
        });
        WaitForSingleObject(recorded, INFINITE);
        const bool parked = held(f.replay(), gate, [&] {           // no ring left: second records directly
            std::thread([&] {
                w.engine.caller = GetCurrentThreadId();
                f.draws(w, 31, 30);
            }).join();
        });
        SetEvent(release);
        first.join();
        f.close(w);
        check(parked && !w.engine.disorder && w.engine.calls == 62 && w.engine.caller_calls == 30 &&
                  !w.engine.gate_timeouts && f.replay()->count == 1 && f.replay()->direct_lookups == 1 &&
                  f.calls(Drain::Switch) == 1 && f.waits(Drain::Switch) == 1,
              "direct: a thread beyond the ring cap waits for the list's calls on the ring, then records directly "
              "(direct calls %llu, out of order %llu)",
              ull(w.engine.caller_calls), ull(w.engine.disorder));
        check(f.off(), "direct: policy off");
        CloseHandle(gate);
        CloseHandle(recorded);
        CloseHandle(release);
    }

    // 5. Bounds.
    {
        Fixture f;
        check(f.on(2, 64u << 10), "bounds: policy on with 64 KiB rings");
        ListBox& w = f.list();
        f.reset(w);
        w.engine.delay_ns = 1000;
        w.engine.ordered = true;
        f.draws(w, 1, 4000);                                        // 48 bytes each: the ring fills three times
        f.close(w);
        check(!w.engine.disorder && w.engine.calls == 4002 && f.waits(Drain::Space) > 0,
              "space: 4000 calls through a 64 KiB ring, in order, the producer waiting for room %llu times",
              ull(f.waits(Drain::Space)));

        ListBox& big = f.list();
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        big.engine.caller = self;
        big.engine.gate = gate;
        f.reset(big);
        big.engine.ordered = true;
        std::vector<UINT> constants(4096);
        for (size_t k = 0; k < constants.size(); ++k) constants[k] = static_cast<UINT>(k * 2654435761u);
        f.draws(big, 1, 10);
        const bool parked = held(f.replay(), gate, [&] {           // 16 KiB: over the 8 KiB entry limit
            g_list.pfnSetGraphicsRoot32BitConstants(big.h, 0, 4096, constants.data(), 0);
        });
        f.draws(big, 11, 10);
        f.close(big);
        const std::vector<std::string> lines = lines_of(big.engine.log);
        check(parked && lines.size() == 23 && starts(lines[11], "SetGraphicsRoot32BitConstants(0,4096,[") &&
                  !big.engine.disorder && big.engine.caller_calls == 2 && f.replay()->oversize == 1 &&
                  f.calls(Drain::Direct) == 1 && f.waits(Drain::Direct) == 1,
              "oversize: a call over the entry limit is made directly, after the list's calls on the ring");
        check(f.off(), "bounds: policy off");
        CloseHandle(gate);
    }
    {
        Fixture f;
        Objects o(&f.context);
        Sources s(o);
        f.device.fail_heaps = true;
        check(f.on(), "no snapshot heap: policy on, the device refuses descriptor heaps");
        ListBox& w = f.list();
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w.engine.caller = self;
        w.engine.gate = gate;
        f.reset(w);
        w.engine.ordered = true;
        f.draws(w, 1, 5);
        bool parked = held(f.replay(), gate, [&] {
            g_list.pfnOMSetRenderTargets(w.h, 2, s.d.handles, FALSE, &s.d.handles[7]);
        });
        s.garble();
        ResetEvent(gate);
        f.draws(w, 6, 5);
        s.prepare(1);
        parked = held(f.replay(), gate, [&] {
            g_list.pfnClearRenderTargetView(w.h, s.d.handles[3], s.d.floats, 0, nullptr);
        }) && parked;
        f.draws(w, 11, 5);
        f.close(w);
        const std::vector<std::string> lines = lines_of(w.engine.log);
        check(parked && lines.size() == 19 && starts(lines[6], "OMSetRenderTargets(2,0,[") &&
                  starts(lines[12], "ClearRenderTargetView([") && !w.engine.disorder && w.engine.caller_calls == 3 &&
                  f.replay()->fallbacks == 2 && f.waits(Drain::Direct) == 2 && !f.device.heaps_made,
              "no snapshot heap: the calls that need one wait for the list's calls on the ring and are made directly "
              "(fallbacks %llu)",
              ull(f.replay()->fallbacks));
        check(f.off(), "no snapshot heap: policy off");
        CloseHandle(gate);
    }
    {
        Fixture f;
        Objects o(&f.context);
        Sources s(o);
        ListBox& a = f.list();
        ListBox& b = f.list();
        auto targets = [&](const ListBox& l) {
            for (uint32_t k = 0; k < 600; ++k) {
                s.prepare(k);
                g_list.pfnOMSetRenderTargets(l.h, 8, s.d.handles, FALSE, &s.d.handles[k % 8]);
                s.garble();
            }
        };
        f.reset(a);
        targets(a);
        f.close(a);
        check(f.on(), "slots: policy on");
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        b.engine.caller = self;
        b.engine.gate = gate;
        f.reset(b);
        const bool parked = held(f.replay(), gate, [&] { targets(b); });   // the worker held: no slot comes back
        f.close(b);
        const std::string where = difference(a.engine.log, b.engine.log);
        check(parked && where.empty() && f.waits(Drain::Slots) > 0,
              "slots: 600 calls with 8 render targets each (4800 RTV snapshots in 4096 slots), the same off and on, "
              "the producer waiting for slots %llu times%s%s",
              ull(f.waits(Drain::Slots)), where.empty() ? "" : "; first difference at ", where.c_str());
        check(f.off() && !f.device.heaps_live, "slots: policy off, snapshot heaps released");
        CloseHandle(gate);
    }

    // 6. Destroys while four threads record. Each recording thread hands an object to the destroyer once it has
    //    recorded its last call naming it; the destroys drain every ring, so no pending call names a released object.
    //    The workers are held at the lists' gate until the first drain waits, then spend 20 us in every call.
    {
        Fixture f;
        check(f.on(), "destroys: policy on");
        constexpr int kThreads = 4;
        constexpr int kObjects = 120;
        struct Recorder {
            PoolBox pool;
            ListBox* list = nullptr;
            Thing things[kObjects * 4];
            ResourceRecord resources[kObjects]{};
            RootSignatureRecord signatures[kObjects]{};
            PipelineRecord pipelines[kObjects]{};
            QueryHeapRecord queries[kObjects]{};
        };
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto recorders = std::make_unique<Recorder[]>(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            Recorder& r = recorders[t];
            f.make_pool(r.pool);
            r.list = &f.list();
            r.list->engine.delay_ns = 20000;
            r.list->engine.gate = gate;
            for (int k = 0; k < kObjects; ++k) {
                Thing* things = &r.things[k * 4];
                for (int j = 0; j < 4; ++j) things[j].id = static_cast<uint32_t>(1000 * (t + 1) + k * 4 + j);
                r.resources[k].h = RecordHeader{Tag::Resource, 0, &things[0], &f.context};
                r.resources[k].desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                r.resources[k].kind = ResourceKind::Placed;
                r.signatures[k].h = RecordHeader{Tag::RootSignature, 0, &things[1], &f.context};
                r.pipelines[k].h = RecordHeader{Tag::PipelineState, 0, &things[2], &f.context};
                r.queries[k].h = RecordHeader{Tag::QueryHeap, 0, &things[3], &f.context};
                r.queries[k].type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
                r.queries[k].count = 1;
            }
        }
        std::mutex lock;
        std::vector<std::pair<int, int>> handed;
        std::atomic<int> finished{0};
        uint64_t destroyed = 0;
        bool parked = false;
        std::thread destroyer([&] {
            std::vector<std::pair<int, int>> batch;
            for (;;) {
                const bool last = finished.load() == kThreads;      // read before the swap: nothing is handed after
                {
                    std::lock_guard<std::mutex> hold(lock);
                    batch.swap(handed);
                }
                if (batch.empty()) {
                    if (last) return;
                    SwitchToThread();
                    continue;
                }
                for (const auto& [t, k] : batch) {
                    Recorder& r = recorders[t];
                    auto destroy = [&] {
                        g_core.pfnDestroyRootSignature(f.hdevice, D3D12DDI_HROOTSIGNATURE{&r.signatures[k]});
                        g_core.pfnDestroyPipelineState(f.hdevice, D3D12DDI_HPIPELINESTATE{&r.pipelines[k]});
                        g_core.pfnDestroyQueryHeap(f.hdevice, D3D12DDI_HQUERYHEAP{&r.queries[k]});
                        g_core.pfnDestroyHeapAndResource(f.hdevice, D3D12DDI_HHEAP{},
                                                         D3D12DDI_HRESOURCE{&r.resources[k]});
                    };
                    if (destroyed) destroy();
                    else parked = held(f.replay(), gate, destroy);
                    ++destroyed;
                }
                batch.clear();
            }
        });
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] {
                Recorder& r = recorders[t];
                ListBox& l = *r.list;
                l.engine.caller = GetCurrentThreadId();             // its Reset and Close pass the gate
                f.reset(l, &r.pool);
                for (int k = 0; k < kObjects; ++k) {
                    D3D12DDIARG_RESOURCE_BARRIER_0022 barrier{};
                    barrier.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.hResource.pDrvPrivate = &r.resources[k];
                    barrier.Transition.StateBefore = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
                    barrier.Transition.StateAfter = D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    const D3D12DDI_HQUERYHEAP query{&r.queries[k]};
                    g_list.pfnSetGraphicsRootSignature(l.h, D3D12DDI_HROOTSIGNATURE{&r.signatures[k]});
                    g_list.pfnSetPipelineState(l.h, D3D12DDI_HPIPELINESTATE{&r.pipelines[k]});
                    g_list.pfnBeginQuery(l.h, query, D3D12DDI_QUERY_TYPE_OCCLUSION, 0);
                    g_list.pfnResourceBarrier(l.h, 1, &barrier);
                    g_list.pfnDrawInstanced(l.h, 3, 1, 0, 0);
                    g_list.pfnEndQuery(l.h, query, D3D12DDI_QUERY_TYPE_OCCLUSION, 0);
                    const D3D12DDI_HRESOURCE resource{&r.resources[k]};
                    g_list.pfnResourceCopy(l.h, resource, resource);
                    {
                        std::lock_guard<std::mutex> hold(lock);
                        handed.emplace_back(t, k);
                    }
                    if (k % 40 == 39) {
                        f.close(l);
                        f.reset(l, &r.pool);
                    }
                }
                f.close(l);
                ++finished;
            });
        for (std::thread& t : threads) t.join();
        destroyer.join();
        uint64_t stale = 0, crossed = 0, odd = 0, calls = 0, timeouts = 0;
        bool released = true;
        for (int t = 0; t < kThreads; ++t) {
            const FakeList& e = recorders[t].list->engine;
            stale += e.stale;
            crossed += e.crossed;
            odd += e.unexpected;
            calls += e.calls;
            timeouts += e.gate_timeouts;
            for (const Thing& x : recorders[t].things) released = released && !x.refs.load();
        }
        check(destroyed == kThreads * kObjects && released && !stale && !crossed && !odd && !timeouts &&
                  calls == kThreads * (7 * kObjects + 8) && !f.shell.list_errors && !f.shell.device_errors,
              "destroys: %llu objects destroyed while %d threads recorded %llu engine calls; none named a released "
              "object (stale %llu, crossed %llu)",
              ull(destroyed * 4), kThreads, ull(calls), ull(stale), ull(crossed));
        check(parked && f.waits(Drain::Destroy) > 0,
              "destroys: %llu of %llu destroy drains waited for pending calls (the window the drain closes)",
              ull(f.waits(Drain::Destroy)), ull(f.calls(Drain::Destroy)));

        // The same check sees a release that does not wait: the worker is held while the object goes away.
        ListBox& l = f.list();
        ResetEvent(gate);
        l.engine.caller = self;
        l.engine.gate = gate;
        f.reset(l);
        Thing x;
        x.id = 9999;
        RootSignatureRecord rs{};
        rs.h = RecordHeader{Tag::RootSignature, 0, &x, &f.context};
        for (int k = 0; k < 5; ++k) g_list.pfnSetGraphicsRootSignature(l.h, D3D12DDI_HROOTSIGNATURE{&rs});
        x.Release();                                                // a destroy without the drain
        SetEvent(gate);
        f.close(l);
        check(l.engine.stale == 5, "destroys: the check sees a release that does not drain (stale %llu)",
              ull(l.engine.stale));
        check(f.off(), "destroys: policy off");
        CloseHandle(gate);
    }

    // 7. Rings: the cap, an exited thread's ring, teardown with calls pending.
    {
        Fixture f;
        check(f.on(2), "rings: policy on with a cap of 2");
        ListBox& l1 = f.list();
        ListBox& l2 = f.list();
        ListBox& l3 = f.list();
        ListBox& l4 = f.list();
        for (ListBox* l : {&l1, &l2, &l3, &l4}) f.reset(*l);
        std::thread([&] {
            f.draws(l1, 1, 10);
            f.close(l1);
        }).join();                                                  // ring 0's owner has exited
        const HANDLE recorded = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const HANDLE hold = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto holder = [&](const ListBox& l) {
            f.draws(l, 1, 10);
            SetEvent(recorded);
            WaitForSingleObject(hold, INFINITE);
            f.close(l);
        };
        std::thread t2(holder, std::cref(l2));                      // takes ring 0 over
        WaitForSingleObject(recorded, INFINITE);
        std::thread t3(holder, std::cref(l3));                      // creates ring 1
        WaitForSingleObject(recorded, INFINITE);
        std::thread([&] {                                           // both rings owned by live threads: direct
            l4.engine.caller = GetCurrentThreadId();
            f.draws(l4, 1, 10);
            f.close(l4);
        }).join();
        SetEvent(hold);
        t2.join();
        t3.join();
        const Replay* rp = f.replay();
        check(rp->count == 2 && rp->reclaimed == 1 && rp->direct_lookups == 1 && !rp->ring_failures &&
                  l4.engine.caller_calls == 11 && l2.engine.calls == 12 && l3.engine.calls == 12,
              "rings: an exited thread's ring is taken over, a new thread gets the second ring, a third live thread "
              "records directly (rings %u, taken over %llu, direct lookups %llu)",
              rp->count.load(), ull(rp->reclaimed), ull(rp->direct_lookups));

        // Teardown: a thread exits with its calls pending, held at the list's gate until the teardown waits for them.
        ListBox& late = f.list();
        const HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        late.engine.caller = self;
        late.engine.gate = gate;
        f.reset(late);
        std::thread([&] { f.draws(late, 1, 50); }).join();
        const uint64_t before = late.engine.calls.load();
        bool off = false;
        const bool parked = held(f.replay(), gate, [&] { off = f.off(); });
        check(parked && off && !f.context.replay && before == 1 && late.engine.calls == 51 &&
                  !late.engine.gate_timeouts && f.shell.started == 2 && f.shell.ended == 2,
              "teardown: policy off with 50 calls pending runs them, joins both workers and frees the rings");
        f.close(late);                                              // direct now
        check(late.engine.calls == 52 && !late.engine.crossed, "teardown: the list records directly after it");
        CloseHandle(gate);
        CloseHandle(recorded);
        CloseHandle(hold);
    }

    // 8. Cost on the calling thread. The fake list's own cost is in the off figure; with the policy on it runs on
    //    the worker. A 64 MiB ring holds the whole measurement, so the producer never waits for room.
    {
        Fixture f;
        Objects o(&f.context);
        Sources s(o);
        auto pass = [&](const ListBox& l) {                         // 52 calls: a render pass of 16 draws
            g_list.pfnRsSetViewports(l.h, 1, s.d.viewports);
            g_list.pfnRsSetScissorRects(l.h, 1, s.d.rects);
            g_list.pfnOMSetRenderTargets(l.h, 1, s.d.handles, FALSE, &s.d.handles[7]);
            for (UINT k = 0; k < 16; ++k) {
                g_list.pfnSetGraphicsRoot32BitConstants(l.h, 0, 4, s.d.uints, 0);
                g_list.pfnIASetVertexBuffers(l.h, 0, 1, s.d.vertex_views);
                g_list.pfnDrawIndexedInstanced(l.h, 36, 1, 0, 0, 0);
            }
            g_list.pfnResourceBarrier(l.h, 1, s.d.barriers);
        };
        constexpr int kPasses = 4000;
        constexpr uint64_t kCalls = uint64_t{kPasses} * 52;
        constexpr UINT kDraws = 200000;
        double record[2]{}, with_close[2]{}, draws[2]{};
        uint64_t allocations[2]{}, space = 0, slots = 0;
        for (int on = 0; on < 2; ++on) {
            if (on) check(f.on(8, 64u << 20), "cost: policy on with 64 MiB rings");
            ListBox& l = f.list();
            l.engine.quiet = true;
            f.reset(l);
            pass(l);                                    // warm: the ring and the snapshot heaps exist after it
            const uint64_t a0 = g_allocations.load();
            const uint64_t t0 = qpc();
            for (int k = 0; k < kPasses; ++k) pass(l);
            const uint64_t t1 = qpc();
            allocations[on] = g_allocations.load() - a0;
            f.close(l);
            const uint64_t t2 = qpc();
            record[on] = ns_per(t1 - t0, kCalls);
            with_close[on] = ns_per(t2 - t0, kCalls);
            ListBox& d = f.list();
            d.engine.quiet = true;
            f.reset(d);
            const uint64_t t3 = qpc();
            f.draws(d, 0, kDraws);
            const uint64_t t4 = qpc();
            f.close(d);
            draws[on] = ns_per(t4 - t3, kDraws);
            if (on) {
                space = f.waits(Drain::Space);
                slots = f.waits(Drain::Slots);
            }
        }
        std::printf("measure  per call on the recording thread: off %.1f ns; on %.1f ns to record, %.1f ns with "
                    "the Close and the list's drain (%llu calls in render passes); DrawInstanced alone: off %.1f ns, "
                    "on %.1f ns; "
                    "waits for room %llu, for snapshot slots %llu\n",
                    record[0], record[1], with_close[1], ull(kCalls), draws[0], draws[1], ull(space), ull(slots));
        check(!allocations[0] && !allocations[1],
              "cost: no allocation while recording (off %llu, on %llu over %llu calls)", ull(allocations[0]),
              ull(allocations[1]), ull(kCalls));
        check(f.off(), "cost: policy off");
    }

    // BD-046. ClearRootArguments mid-list (the API's ClearState): each bound signature keeps its binding and gets
    // every argument set to zero, in parameter order, compute before graphics; a signature unbound by a null set is
    // left alone, and on a just-reset list nothing is bound and nothing happens. The same calls with the replay
    // policy off and on.
    {
        std::string root_lines[2];
        for (int on = 0; on < 2; ++on) {
            Fixture f;
            if (on) check(f.on(), "clear root arguments: policy on");
            D3D12DDI_DESCRIPTOR_RANGE_0013 range{};
            range.RangeType = D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 2;
            D3D12DDI_ROOT_PARAMETER_0013 params[5]{};
            params[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.Num32BitValues = 4;
            params[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_CBV;
            params[2].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_SRV;
            params[2].Descriptor.ShaderRegister = 1;
            params[3].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
            params[4].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[4].DescriptorTable.NumDescriptorRanges = 1;
            params[4].DescriptorTable.pDescriptorRanges = &range;
            D3D12DDI_ROOT_SIGNATURE_0013 desc{};
            desc.NumParameters = 5;
            desc.pRootParameters = params;
            D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 args{};
            args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
            args.pRootSignature_1_1 = &desc;
            const SIZE_T size = g_core.pfnCalcPrivateRootSignatureSize(f.hdevice, &args);
            std::vector<uint64_t> storage((size + 7) / 8);
            const D3D12DDI_HROOTSIGNATURE rs{storage.data()};
            check(size == sizeof(RootSignatureRecord) + 5 * sizeof(RootParameterShape) &&
                      g_core.pfnCreateRootSignature(f.hdevice, &args, rs) == S_OK,
                  "clear root arguments: a signature of five parameters, one of each type, its shapes sized");
            ListBox& l = f.list();
            f.reset(l);
            g_list.pfnClearRootArguments(l.h);                      // just reset: nothing bound
            const UINT values[4] = {1, 2, 3, 4};
            g_list.pfnSetComputeRootSignature(l.h, rs);
            g_list.pfnSetGraphicsRootSignature(l.h, rs);
            g_list.pfnSetGraphicsRoot32BitConstants(l.h, 0, 4, values, 0);
            g_list.pfnSetGraphicsRootConstantBufferView(l.h, 1, 0x10000);
            g_list.pfnSetComputeRootUnorderedAccessView(l.h, 3, 0x20000);
            g_list.pfnSetComputeRootDescriptorTable(l.h, 4, D3D12DDI_GPU_DESCRIPTOR_HANDLE{0x300});
            g_list.pfnClearRootArguments(l.h);                      // both bound: both cleared
            g_list.pfnSetGraphicsRootSignature(l.h, D3D12DDI_HROOTSIGNATURE{nullptr});
            g_list.pfnClearRootArguments(l.h);                      // graphics unbound: compute only
            f.close(l);
            f.reset(l);
            g_list.pfnClearRootArguments(l.h);                      // the Reset unbound both
            f.close(l);
            for (const std::string& line : lines_of(l.engine.log))
                if (line.find("Root") != std::string::npos) root_lines[on] += line + "\n";
            g_core.pfnDestroyRootSignature(f.hdevice, rs);
            if (on) check(f.off(), "clear root arguments: policy off");
        }
        auto cleared = [](const char* bind) {
            const std::string b = bind;
            return "Set" + b + "Root32BitConstants(0,4,[00000000000000000000000000000000],0,)\n" + "Set" + b +
                   "RootConstantBufferView(1,0,)\n" + "Set" + b + "RootShaderResourceView(2,0,)\n" + "Set" + b +
                   "RootUnorderedAccessView(3,0,)\n" + "Set" + b + "RootDescriptorTable(4,0,)\n";
        };
        const std::string expected = std::string("SetComputeRootSignature(#900,)\nSetGraphicsRootSignature(#900,)\n") +
                                     "SetGraphicsRoot32BitConstants(0,4,[01000000020000000300000004000000],0,)\n" +
                                     "SetGraphicsRootConstantBufferView(1,10000,)\n" +
                                     "SetComputeRootUnorderedAccessView(3,20000,)\n" +
                                     "SetComputeRootDescriptorTable(4,300,)\n" + cleared("Compute") + cleared("Graphics") +
                                     "SetGraphicsRootSignature(0,)\n" + cleared("Compute");
        check(root_lines[0] == expected, "clear root arguments: every argument zeroed in order, signatures kept%s%s",
              root_lines[0] == expected ? "" : "; got\n", root_lines[0] == expected ? "" : root_lines[0].c_str());
        check(root_lines[1] == root_lines[0], "clear root arguments: the same calls with the replay policy on");
    }

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
