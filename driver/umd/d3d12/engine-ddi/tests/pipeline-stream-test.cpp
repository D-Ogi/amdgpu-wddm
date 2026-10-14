// SPDX-License-Identifier: MIT
// The pipeline subobject stream of pfnCreatePipelineState (D34, pipelines.cpp), linked against the native
// engine-ddi.lib (no harness macro), the way the shell's DLL links it. No engine and no GPU: the engine device is a
// fake ID3D12Device4 that keeps the bytes of every D3D12_PIPELINE_STATE_STREAM_DESC it is handed and counts the
// older pipeline entries, and the checks walk the stream the way the engine walks it (vkd3d-proton state.c,
// vkd3d_pipeline_state_desc_from_d3d12_stream_desc: a type word, its value, the pair's size rounded up to a
// pointer). What this proves:
//
//   1. A rasterizing pipeline is created through ID3D12Device2::CreatePipelineState and not through
//      ID3D12Device::CreateGraphicsPipelineState, and a compute pipeline still takes the compute entry.
//   2. The stream is one the engine can read: it begins pointer-aligned, every subobject is a type the engine
//      knows, no type appears twice, and the last subobject ends exactly at the end of the stream.
//   3. Every state of the DDI payload arrives: the root signature, the programs, the three states, the sample
//      mask, the formats, the sample description, the node mask, the strip cut value and the topology type.
//   4. L52 depth bounds: the state's DepthBoundsTestEnable reaches DEPTH_STENCIL1, the only subobject that carries
//      it, and a pipeline that asks for the bound test is no longer refused.
//   5. L57 view instancing: the locations and the flags reach VIEW_INSTANCING; a pipeline with no view instances
//      writes no such subobject; a count the contract does not allow is refused with E_INVALIDARG and reaches no
//      engine.
//   7. BD-158 asynchronous compilation (set_pso_policy, pso-async.cpp): the policy is refused unless its shape is
//      the one the boundary names; with it on, a create answers S_OK before the engine has seen the pipeline, the
//      engine's create then arrives on a pool thread and carries the same state as the synchronous one, including
//      the view-instance locations, which the DDI call's frame no longer holds by then; every consumer gets the
//      pipeline (a consumer that arrives before a worker makes the create itself); a destroy before any use makes
//      no engine create at all; a failure after the S_OK reaches the consumer as a failure and binds nothing; and
//      none of it depends on how the creating threads interleave.
//   6. D85-D90 pipeline libraries: the blob reaches the engine's CreatePipelineLibrary unchanged; a pipeline
//      added at an index is stored under that index's name; a serialize is handed exactly the capacity the size
//      query of its own thread answered, is refused when no query covers the library's current contents, and is
//      refused rather than handed another thread's size; and a create whose LibraryReference names a library is a
//      load out of that library, of the stream the create would have written, never a new pipeline on the engine
//      device. Every engine failure of a creation slot is clamped to a code the runtime admits.
//
// A mesh pipeline shares put_common_state and put_view_instancing with the graphics path, so these checks hold for
// its stream as well, except for the AS and MS subobjects: a mesh or amplification program passes the shader
// intake only with real DXIL metadata (shader-container.h, Slot: metadata naming), which no synthetic program here can
// carry. The mesh conformance client on the lab covers those two subobjects.
#include "shader-container/shader-container.h"      // first: it selects the D3D12 tokenized program format header
#include "internal.h"
#include "pso-async.h"
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <io.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace engine_ddi;

namespace {
int failures = 0;
void check(bool ok, const char* format, ...) {
    char what[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(what, sizeof(what), format, args);
    va_end(args);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// ---- The stream, read as the engine reads it -------------------------------------------------------------------
// What one subobject type looks like in the stream: where its value sits inside the pair, how long the value is,
// and the stride to the next type word. This is the engine's own layout (vkd3d-proton state.c,
// VKD3D_HANDLE_SUBOBJECT_EXPLICIT: `stream_ptr += align(sizeof(*subobject), sizeof(void*))` over a packed struct of
// the type and the value), written out once here so the test does not share the code it checks.
struct Shape {
    size_t value_offset = 0;
    size_t value_size = 0;
    size_t stride = 0;
    bool known = false;
};
template <class T> Shape shape_of() noexcept {
    struct Pair {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        T value;
    };
    constexpr size_t kAlign = sizeof(void*);
    return {offsetof(Pair, value), sizeof(T), (sizeof(Pair) + kAlign - 1) / kAlign * kAlign, true};
}
Shape shape(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) noexcept {
#define SHAPE(name, T)                                                                                             \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##name:                                                               \
        return shape_of<T>()
    switch (type) {
        SHAPE(ROOT_SIGNATURE, ID3D12RootSignature*);
        SHAPE(VS, D3D12_SHADER_BYTECODE);
        SHAPE(PS, D3D12_SHADER_BYTECODE);
        SHAPE(DS, D3D12_SHADER_BYTECODE);
        SHAPE(HS, D3D12_SHADER_BYTECODE);
        SHAPE(GS, D3D12_SHADER_BYTECODE);
        SHAPE(CS, D3D12_SHADER_BYTECODE);
        SHAPE(AS, D3D12_SHADER_BYTECODE);
        SHAPE(MS, D3D12_SHADER_BYTECODE);
        SHAPE(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC);
        SHAPE(BLEND, D3D12_BLEND_DESC);
        SHAPE(SAMPLE_MASK, UINT);
        SHAPE(RASTERIZER, D3D12_RASTERIZER_DESC);
        SHAPE(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC);
        SHAPE(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1);
        SHAPE(DEPTH_STENCIL2, D3D12_DEPTH_STENCIL_DESC2);
        SHAPE(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC);
        SHAPE(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        SHAPE(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE);
        SHAPE(RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY);
        SHAPE(DEPTH_STENCIL_FORMAT, DXGI_FORMAT);
        SHAPE(SAMPLE_DESC, DXGI_SAMPLE_DESC);
        SHAPE(NODE_MASK, UINT);
        SHAPE(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE);
        SHAPE(FLAGS, D3D12_PIPELINE_STATE_FLAGS);
        SHAPE(VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC);
    default:
        return {};
    }
#undef SHAPE
}

// One subobject as it was read: its type and the bytes of its value.
struct Subobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
    std::vector<uint8_t> value;
};

class Stream {
public:
    bool ok = false;                            // the walk reached the end with no unknown or repeated type
    std::vector<Subobject> objects;

    void parse(const std::vector<uint8_t>& bytes) {
        objects.clear();
        ok = false;
        uint64_t seen = 0;
        size_t at = 0;
        while (at < bytes.size()) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
            if (at + sizeof(type) > bytes.size()) return;
            std::memcpy(&type, bytes.data() + at, sizeof(type));
            const Shape s = shape(type);
            const unsigned bit = static_cast<unsigned>(type);
            if (!s.known || bit >= 64 || (seen >> bit) & 1) return;   // "Duplicate pipeline subobject type"
            if (at + s.stride > bytes.size()) return;
            seen |= uint64_t{1} << bit;
            const uint8_t* value = bytes.data() + at + s.value_offset;
            objects.push_back({type, std::vector<uint8_t>(value, value + s.value_size)});
            at += s.stride;
        }
        ok = true;
    }
    const Subobject* find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const noexcept {
        for (const Subobject& o : objects)
            if (o.type == type) return &o;
        return nullptr;
    }
    bool has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const noexcept { return find(type) != nullptr; }
    // The value of one subobject, or the zeroed T when the stream has no such subobject.
    template <class T> T value(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const noexcept {
        T out{};
        const Subobject* o = find(type);
        if (o && o->value.size() == sizeof(T)) std::memcpy(&out, o->value.data(), sizeof(T));
        return out;
    }
};

// ---- One create's state, with no address in it -----------------------------------------------------------------
// What the engine would read of one subobject, with no address left in it: the value's bytes, and behind every
// pointer the value carries, the bytes it points at. Two creates of one payload answer the same digest although
// each create's plan owns its own copies, so a digest tells "the same state arrived" apart from "from the same
// addresses" - which is what a deep copy is for. Read inside the engine's create, where the pointers are live.
void append_bytes(std::vector<uint8_t>& out, const void* bytes, size_t size) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    out.insert(out.end(), p, p + size);
}
void append_name(std::vector<uint8_t>& out, const char* name) {
    append_bytes(out, name ? name : "", name ? std::strlen(name) + 1 : 1);
}
template <class T> T value_of(const Subobject& o) {
    T out{};
    if (o.value.size() == sizeof(T)) std::memcpy(&out, o.value.data(), sizeof(T));
    return out;
}
std::vector<uint8_t> pointer_free(const Subobject& o) {
    std::vector<uint8_t> out;
    switch (o.type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
        const auto code = value_of<D3D12_SHADER_BYTECODE>(o);
        const uint64_t length = code.BytecodeLength;
        append_bytes(out, &length, sizeof(length));
        if (code.pShaderBytecode) append_bytes(out, code.pShaderBytecode, code.BytecodeLength);
        return out;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
        const auto vi = value_of<D3D12_VIEW_INSTANCING_DESC>(o);
        append_bytes(out, &vi.ViewInstanceCount, sizeof(vi.ViewInstanceCount));
        append_bytes(out, &vi.Flags, sizeof(vi.Flags));
        if (vi.pViewInstanceLocations)
            append_bytes(out, vi.pViewInstanceLocations, sizeof(*vi.pViewInstanceLocations) * vi.ViewInstanceCount);
        return out;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
        const auto layout = value_of<D3D12_INPUT_LAYOUT_DESC>(o);
        append_bytes(out, &layout.NumElements, sizeof(layout.NumElements));
        for (UINT i = 0; layout.pInputElementDescs && i < layout.NumElements; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& e = layout.pInputElementDescs[i];
            append_name(out, e.SemanticName);
            append_bytes(out, &e.SemanticIndex, sizeof(e.SemanticIndex));
            append_bytes(out, &e.Format, sizeof(e.Format));
            append_bytes(out, &e.InputSlot, sizeof(e.InputSlot));
            append_bytes(out, &e.AlignedByteOffset, sizeof(e.AlignedByteOffset));
            append_bytes(out, &e.InputSlotClass, sizeof(e.InputSlotClass));
            append_bytes(out, &e.InstanceDataStepRate, sizeof(e.InstanceDataStepRate));
        }
        return out;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
        const auto so = value_of<D3D12_STREAM_OUTPUT_DESC>(o);
        append_bytes(out, &so.NumEntries, sizeof(so.NumEntries));
        append_bytes(out, &so.NumStrides, sizeof(so.NumStrides));
        append_bytes(out, &so.RasterizedStream, sizeof(so.RasterizedStream));
        for (UINT i = 0; so.pSODeclaration && i < so.NumEntries; ++i) {
            const D3D12_SO_DECLARATION_ENTRY& e = so.pSODeclaration[i];
            append_name(out, e.SemanticName);
            append_bytes(out, &e.SemanticIndex, sizeof(e.SemanticIndex));
            append_bytes(out, &e.StartComponent, sizeof(e.StartComponent));
            append_bytes(out, &e.ComponentCount, sizeof(e.ComponentCount));
            append_bytes(out, &e.OutputSlot, sizeof(e.OutputSlot));
            append_bytes(out, &e.Stream, sizeof(e.Stream));
        }
        if (so.pBufferStrides) append_bytes(out, so.pBufferStrides, sizeof(*so.pBufferStrides) * so.NumStrides);
        return out;
    }
    default:
        return o.value;
    }
}

// One engine create, as the digest of its subobjects in stream order.
struct Digest {
    bool read = false;                          // the stream walked end to end with no unknown or repeated type
    bool aligned = true;
    std::vector<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE> types;
    std::vector<std::vector<uint8_t>> values;
    bool operator==(const Digest& other) const {
        return read == other.read && types == other.types && values == other.values;
    }
};
Digest digest_of(const void* bytes, size_t size) {
    Digest d;
    if (!bytes || !size) return d;
    d.aligned = reinterpret_cast<uintptr_t>(bytes) % sizeof(void*) == 0;
    const auto* p = static_cast<const uint8_t*>(bytes);
    Stream s;
    s.parse(std::vector<uint8_t>(p, p + size));
    d.read = s.ok;
    for (const Subobject& o : s.objects) {
        d.types.push_back(o.type);
        d.values.push_back(pointer_free(o));
    }
    return d;
}

// Where two digests part, for the message of a check that finds two creates of one payload unequal.
size_t mismatch_at(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return i;
}
std::string difference(const Digest& a, const Digest& b) {
    if (a.read != b.read) return "one of the two streams did not walk end to end";
    if (a.types.size() != b.types.size())
        return "subobject counts " + std::to_string(a.types.size()) + " and " + std::to_string(b.types.size());
    for (size_t i = 0; i < a.types.size(); ++i) {
        if (a.types[i] != b.types[i])
            return "subobject " + std::to_string(i) + ": type " + std::to_string(static_cast<int>(a.types[i])) +
                   " and type " + std::to_string(static_cast<int>(b.types[i]));
        if (a.values[i] != b.values[i])
            return "subobject type " + std::to_string(static_cast<int>(a.types[i])) + ": " +
                   std::to_string(a.values[i].size()) + " and " + std::to_string(b.values[i].size()) +
                   " bytes, parting at byte " + std::to_string(mismatch_at(a.values[i], b.values[i]));
    }
    return "the same state";
}
// The first digest that is not the first one's, as a message, for a check over many creates of one payload.
std::string first_difference(const std::vector<Digest>& digests) {
    for (size_t i = 1; i < digests.size(); ++i)
        if (!(digests[i] == digests[0])) return "digest " + std::to_string(i) + ": " + difference(digests[i], digests[0]);
    return digests.empty() ? "no digest at all" : "the same state";
}

// ---- Fake engine objects ---------------------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable : 4100)                 // the fakes ignore most of their arguments

template <class I> class Unknown : public I {
public:
    // Atomic, because with the policy on a pool thread takes a reference to a pipeline while the DDI thread that
    // created it has already gone on to the next create.
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
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

class FakeRootSignature final : public Child<ID3D12RootSignature> {};
class FakePipeline final : public Child<ID3D12PipelineState> {
public:
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob**) override { return E_NOTIMPL; }
};

// The engine's pipeline library (D85-D90): it keeps the blob it was created from and records every name it was
// asked to store or load, the stream of a load, and the capacity a serialize was given. A load hands out a
// pipeline of its own, so the test can tell a load from an ordinary create by the object that comes back.
class FakePipelineLibrary final : public Child<ID3D12PipelineLibrary1> {
public:
    FakePipeline pipeline;
    std::vector<uint8_t> blob;                  // what CreatePipelineLibrary was given
    std::vector<std::wstring> stored;           // the names StorePipeline was called with, in order
    std::vector<ID3D12PipelineState*> stored_pipelines;
    std::wstring last_loaded;
    std::vector<uint8_t> last_load_stream;
    Digest last_load_digest;                    // the same stream with no address in it, read inside the load
    unsigned loads = 0, compute_loads = 0, desc_loads = 0, serializes = 0, size_queries = 0;
    SIZE_T serialized_size = 0;                 // what GetSerializedSize answers
    SIZE_T last_serialize_capacity = 0;
    HRESULT store_result = S_OK;
    HRESULT load_result = S_OK;
    HRESULT serialize_result = S_OK;

    HRESULT hand_out(REFIID riid, void** out) {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (riid != __uuidof(ID3D12PipelineState)) return E_NOINTERFACE;
        pipeline.AddRef();
        *out = static_cast<ID3D12PipelineState*>(&pipeline);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState* state) override {
        stored.push_back(name ? name : L"");
        stored_pipelines.push_back(state);
        return store_result;
    }
    // engine-ddi never uses the two description forms for a rasterizing pipeline: a graphics or mesh load goes
    // through LoadPipeline with the same subobject stream the create writes.
    HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                   REFIID riid, void** out) override {
        ++desc_loads;
        last_loaded = name ? name : L"";
        return hand_out(riid, out);
    }
    HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                  REFIID riid, void** out) override {
        ++compute_loads;
        last_loaded = name ? name : L"";
        if (!desc || !desc->CS.BytecodeLength) return E_INVALIDARG;
        if (FAILED(load_result)) {
            if (out) *out = nullptr;
            return load_result;
        }
        return hand_out(riid, out);
    }
    SIZE_T STDMETHODCALLTYPE GetSerializedSize() override {
        ++size_queries;
        return serialized_size;
    }
    HRESULT STDMETHODCALLTYPE Serialize(void* data, SIZE_T size) override {
        ++serializes;
        last_serialize_capacity = size;
        if (FAILED(serialize_result)) return serialize_result;
        if (!data || size < serialized_size) return E_INVALIDARG;
        std::memset(data, 0x5D, serialized_size);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LoadPipeline(LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid,
                                           void** out) override {
        ++loads;
        last_loaded = name ? name : L"";
        last_load_stream.clear();
        last_load_digest = Digest{};
        if (desc && desc->pPipelineStateSubobjectStream && desc->SizeInBytes) {
            const auto* bytes = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
            last_load_stream.assign(bytes, bytes + desc->SizeInBytes);
            last_load_digest = digest_of(desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
        }
        if (FAILED(load_result)) {
            if (out) *out = nullptr;
            return load_result;
        }
        return hand_out(riid, out);
    }
};

// The engine device: root signatures, the stream entry of ID3D12Device2, the compute entry, and a count of the
// older graphics entry. Anything else of ID3D12Device4 is unexpected on this path and counted.
class FakeDevice : public Object<ID3D12Device4> {
public:
    FakeRootSignature root;
    FakePipeline pipeline;
    FakePipelineLibrary library;
    unsigned streams = 0, graphics_descs = 0, compute_descs = 0, unexpected = 0, misaligned = 0;
    unsigned library_creates = 0;
    const void* last_library_blob = nullptr;
    SIZE_T last_library_blob_size = 0;
    HRESULT library_result = S_OK;
    std::vector<uint8_t> last_stream;
    // The view instance locations, read inside the call. VIEW_INSTANCING carries a pointer, and the array behind it
    // belongs to the plan of this create (pipelines.cpp, put_view_instancing), which the synchronous path keeps for
    // the length of the call and the asynchronous one in the job; after the engine's create returns, that pointer
    // is stale either way, exactly as the engine's own create reads it.
    std::vector<D3D12_VIEW_INSTANCE_LOCATION> last_locations;
    Digest last_digest;                         // the last stream with no address in it, read inside the create
    HRESULT stream_result = S_OK;

    template <class T> T odd(T value) {
        ++unexpected;
        return value;
    }
    HRESULT none(void** out) {
        if (out) *out = nullptr;
        return odd(E_NOTIMPL);
    }
    HRESULT hand_out_pipeline(REFIID riid, void** out) {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (riid != __uuidof(ID3D12PipelineState)) return E_NOINTERFACE;
        pipeline.AddRef();
        *out = static_cast<ID3D12PipelineState*>(&pipeline);
        return S_OK;
    }

    // ID3D12Device2: the one pipeline entry engine-ddi uses for a rasterizing pipeline.
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid,
                                                 void** out) override {
        ++streams;
        last_stream.clear();
        last_locations.clear();
        last_digest = Digest{};
        if (desc && desc->pPipelineStateSubobjectStream && desc->SizeInBytes) {
            const auto* bytes = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
            if (reinterpret_cast<uintptr_t>(bytes) % sizeof(void*)) ++misaligned;
            last_stream.assign(bytes, bytes + desc->SizeInBytes);
            last_digest = digest_of(desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
            Stream read;
            read.parse(last_stream);
            const auto vi = read.value<D3D12_VIEW_INSTANCING_DESC>(
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING);
            if (vi.ViewInstanceCount && vi.pViewInstanceLocations)
                last_locations.assign(vi.pViewInstanceLocations,
                                      vi.pViewInstanceLocations + vi.ViewInstanceCount);
        }
        if (FAILED(stream_result)) {
            if (out) *out = nullptr;
            return stream_result;
        }
        return hand_out_pipeline(riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID riid,
                                                         void** out) override {
        ++graphics_descs;
        return hand_out_pipeline(riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID riid,
                                                         void** out) override {
        ++compute_descs;
        return hand_out_pipeline(riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void*, SIZE_T, REFIID riid, void** out) override {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (riid != __uuidof(ID3D12RootSignature)) return E_NOINTERFACE;
        root.AddRef();
        *out = static_cast<ID3D12RootSignature*>(&root);
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override { return 32; }

    // Everything else: not expected here.
    UINT STDMETHODCALLTYPE GetNodeCount() override { return odd(1u); }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                ID3D12PipelineState*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE, void*, UINT) override { return odd(E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                   void** out) override {
        return none(out);
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
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CopyDescriptors(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                           D3D12_DESCRIPTOR_HEAP_TYPE) override {
        odd(0);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE) override {
        odd(0);
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT, UINT, const D3D12_RESOURCE_DESC*) override {
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
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void** out) override {
        return none(out);
    }
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
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*,
                                                     REFIID, void** out) override {
        return none(out);
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*,
                                             UINT*, UINT, D3D12_SUBRESOURCE_TILING*) override {
        odd(0);
    }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return odd(LUID{}); }
    // ID3D12Device1: the pipeline library entry engine-ddi uses for D86.
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void* blob, SIZE_T size, REFIID riid,
                                                    void** out) override {
        ++library_creates;
        last_library_blob = blob;
        last_library_blob_size = size;
        if (out) *out = nullptr;
        if (FAILED(library_result)) return library_result;
        if (!out) return E_INVALIDARG;
        if (riid != __uuidof(ID3D12PipelineLibrary1)) return E_NOINTERFACE;
        library.blob.clear();
        if (blob && size) {
            const auto* bytes = static_cast<const uint8_t*>(blob);
            library.blob.assign(bytes, bytes + size);
        }
        library.AddRef();
        *out = static_cast<ID3D12PipelineLibrary1*>(&library);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const*, const UINT64*, UINT,
                                                                D3D12_MULTIPLE_FENCE_WAIT_FLAGS, HANDLE) override {
        return odd(E_NOTIMPL);
    }
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT, ID3D12Pageable* const*,
                                                   const D3D12_RESIDENCY_PRIORITY*) override {
        return odd(E_NOTIMPL);
    }
    // ID3D12Device3
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void*, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE, REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS, UINT, ID3D12Pageable* const*,
                                                  ID3D12Fence*, UINT64) override {
        return odd(E_NOTIMPL);
    }
    // ID3D12Device4
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID,
                                                 void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC*, REFIID,
                                                             void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                       const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                       REFIID, void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID,
                                          void** out) override {
        return none(out);
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                      REFIID, void** out) override {
        return none(out);
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(
        UINT, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_ALLOCATION_INFO1*) override {
        return odd(D3D12_RESOURCE_ALLOCATION_INFO{});
    }
};

#pragma warning(pop)

// ---- The DDI side ----------------------------------------------------------------------------------------------
D3D12DDI_DEVICE_FUNCS_CORE_0088 g_core{};
DeviceContext* g_context = nullptr;
HRESULT g_reported = S_OK;

void APIENTRY device_error(void*, HRESULT hr) { g_reported = hr; }
void APIENTRY list_error(void*, D3D12DDI_HRTCOMMANDLIST, HRESULT) {}
BOOL APIENTRY device_lost(void*) { return FALSE; }
HRESULT APIENTRY bind_table(void*, D3D12DDI_HRTCOMMANDLIST, uint32_t) { return S_OK; }
DeviceContext* APIENTRY resolve_device(D3D12DDI_HDEVICE) { return g_context; }

// A bare tokenized program of two DWORDs: the version token and the length, which is all the shader intake reads
// of a DXBC program with no signature entry (shader-container.h, ProgramDesc; pipelines.cpp reads pShaderCode[1]).
std::vector<UINT> program(D3D10_SB_TOKENIZED_PROGRAM_TYPE type) {
    return {ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(type, 5, 0), 2};
}

// One device's worth of DDI objects: the context, the engine device behind it, and the handles a pipeline payload
// names. Every handle is a block of the size the matching CalcPrivate slot asks for, as the runtime's would be.
class Fixture {
public:
    FakeDevice own;                             // the engine device of a test that brings none of its own
    FakeDevice& device;
    DeviceContext context;
    D3D12DDI_HDEVICE h{};

    explicit Fixture(FakeDevice* use = nullptr) : device(use ? *use : own) {
        context.hooks = {sizeof(ShellHooks), nullptr, device_error, list_error, device_lost, bind_table, nullptr,
                         nullptr};
        context.device = &device;
        context.device4 = &device;
        for (UINT& n : context.increments) n = 32;
        h.pDrvPrivate = &context;
        g_context = &context;
    }
    ~Fixture() { g_context = nullptr; }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void* storage(SIZE_T size) {
        blocks_.push_back(std::vector<uint8_t>(size ? size : 1, 0));
        return blocks_.back().data();
    }
    D3D12DDI_HROOTSIGNATURE root_signature() {
        D3D12DDI_ROOT_SIGNATURE_0013 rs{};
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 args{D3D12DDI_ROOT_SIGNATURE_VERSION_1_1, {&rs}, 0};
        D3D12DDI_HROOTSIGNATURE out{storage(g_core.pfnCalcPrivateRootSignatureSize(h, &args))};
        if (g_core.pfnCreateRootSignature(h, &args, out) != S_OK) check(false, "fixture: root signature created");
        return out;
    }
    D3D12DDI_HSHADER shader(D3D10_SB_TOKENIZED_PROGRAM_TYPE type) {
        codes_.push_back(program(type));
        D3D12DDIARG_CREATE_SHADER_0026 args{};
        args.pShaderCode = codes_.back().data();
        D3D12DDI_HSHADER out{storage(g_core.pfnCalcPrivateShaderSize(h, &args))};
        g_reported = S_OK;
        switch (type) {
        case D3D10_SB_VERTEX_SHADER:
            g_core.pfnCreateVertexShader(h, &args, out);
            break;
        case D3D10_SB_PIXEL_SHADER:
            g_core.pfnCreatePixelShader(h, &args, out);
            break;
        default:
            g_core.pfnCreateComputeShader(h, &args, out);
            break;
        }
        if (g_reported != S_OK)
            check(false, "fixture: program of type %d created (reported %08lx)", static_cast<int>(type),
                  static_cast<unsigned long>(g_reported));
        return out;
    }
    D3D12DDI_HBLENDSTATE blend_state(D3D12DDI_LIBRARY_REFERENCE_0010 library = {}) {
        D3D12DDI_BLEND_DESC_0010 d{};
        d.LibraryReference = library;
        d.AlphaToCoverageEnable = TRUE;
        for (D3D12DDI_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) rt.RenderTargetWriteMask = 0xF;
        D3D12DDI_HBLENDSTATE out{storage(g_core.pfnCalcPrivateBlendStateSize(h, &d))};
        g_core.pfnCreateBlendState(h, &d, out);
        return out;
    }
    D3D12DDI_HRASTERIZERSTATE rasterizer_state(D3D12DDI_LIBRARY_REFERENCE_0010 library = {}) {
        D3D12DDI_RASTERIZER_DESC_0010 d{};
        d.LibraryReference = library;
        d.FillMode = D3D12DDI_FILL_MODE_WIREFRAME;
        d.CullMode = D3D12DDI_CULL_MODE_FRONT;
        d.ForcedSampleCount = 4;
        D3D12DDI_HRASTERIZERSTATE out{storage(g_core.pfnCalcPrivateRasterizerStateSize(h, &d))};
        g_core.pfnCreateRasterizerState(h, &d, out);
        return out;
    }
    D3D12DDI_HDEPTHSTENCILSTATE depth_state(BOOL depth_bounds, D3D12DDI_LIBRARY_REFERENCE_0010 library = {}) {
        D3D12DDI_DEPTH_STENCIL_DESC_0025 d{};
        d.LibraryReference = library;
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D12DDI_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D12DDI_COMPARISON_FUNC_GREATER;
        d.StencilReadMask = 0x5A;
        d.StencilWriteMask = 0xA5;
        d.DepthBoundsTestEnable = depth_bounds;
        D3D12DDI_HDEPTHSTENCILSTATE out{storage(g_core.pfnCalcPrivateDepthStencilStateSize(h, &d))};
        g_core.pfnCreateDepthStencilState(h, &d, out);
        return out;
    }
    // D85 and D86: the private storage the library record needs, then the create.
    D3D12DDI_HPIPELINELIBRARY pipeline_library(const void* blob, SIZE_T size, HRESULT* hr_out = nullptr) {
        D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010 args{size, blob};
        D3D12DDI_HPIPELINELIBRARY out{storage(g_core.pfnCalcPrivatePipelineLibrarySize(h, &args))};
        const HRESULT hr = g_core.pfnCreatePipelineLibrary(h, &args, out);
        if (hr_out) *hr_out = hr;
        return out;
    }

private:
    std::vector<std::vector<uint8_t>> blocks_;
    std::vector<std::vector<UINT>> codes_;
};

// A graphics payload with every field of interest set to a value of its own, so that a field arriving in the wrong
// subobject is visible.
D3D12DDIARG_CREATE_PIPELINE_STATE_0075 graphics_args(Fixture& f, BOOL depth_bounds) {
    D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a{};
    a.hRootSignature = f.root_signature();
    a.hVertexShader = f.shader(D3D10_SB_VERTEX_SHADER);
    a.hPixelShader = f.shader(D3D10_SB_PIXEL_SHADER);
    a.hBlendState = f.blend_state();
    a.hRasterizerState = f.rasterizer_state();
    a.hDepthStencilState = f.depth_state(depth_bounds);
    a.SampleMask = 0x0F0F0F0F;
    a.IBStripCutValue = D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    a.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    a.NumRenderTargets = 2;
    a.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    a.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
    a.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    a.SampleDesc = {4, 1};
    a.NodeMask = 1;
    return a;
}

HRESULT create(Fixture& f, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a, D3D12DDI_HPIPELINESTATE& out) {
    out.pDrvPrivate = f.storage(g_core.pfnCalcPrivatePipelineStateSize(f.h, &a));
    return g_core.pfnCreatePipelineState(f.h, &a, out, D3D12DDI_HRTPIPELINESTATE{});
}

// ---- BD-158: the asynchronous path -----------------------------------------------------------------------------
// A gate the test shuts to hold every pool thread out of the compile it has claimed, so that a job can be seen
// uncompiled and a consumer can reach one before any worker does. Pool threads only: a consumer that runs a job on
// its own thread never goes through the run hook. Every wait is bounded, so a mistake here fails and never hangs.
class Gate {
public:
    Gate() : event_(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}
    ~Gate() {
        if (event_) CloseHandle(event_);
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    bool ready() const noexcept { return event_ != nullptr; }
    void shut() noexcept {
        if (event_) ResetEvent(event_);
    }
    void release() noexcept {
        if (event_) SetEvent(event_);
    }
    void wait() noexcept {
        if (event_) WaitForSingleObject(event_, 10000);
    }

private:
    HANDLE event_;
};
Gate g_gate;
std::atomic<unsigned> g_runs{0};                // run hook entries: jobs handed to a pool thread
std::atomic<unsigned> g_drains{0};              // drained hook calls, every one of them on a DDI thread

void APIENTRY async_run(void*, PsoBody body, void* job) {
    ++g_runs;
    g_gate.wait();
    body(job);
}
void APIENTRY async_drained(void*) { ++g_drains; }

// The lines the policy's log hook was handed, which is the sink the shell gives engine-ddi. Written on whichever
// thread printed the line; read by the checks below once that thread is accounted for.
std::mutex g_log_lock;
std::vector<std::string> g_log_lines;
void APIENTRY async_log(void*, const char* line) {
    std::lock_guard<std::mutex> held(g_log_lock);
    g_log_lines.emplace_back(line ? line : "");
}
void clear_lines() {
    std::lock_guard<std::mutex> held(g_log_lock);
    g_log_lines.clear();
}
// Summary lines whose reason word is this one ("periodic", "process detach", "the device's destruction").
unsigned summary_lines(const char* reason) {
    std::lock_guard<std::mutex> held(g_log_lock);
    unsigned found = 0;
    for (const std::string& line : g_log_lines)
        if (line.compare(0, 13, "pso summary (") == 0 && line.find(reason) != std::string::npos) ++found;
    return found;
}

// The clock of the periodic line (pso-async.h), installed once before the first device of this program turns the
// policy on. A period therefore passes because the test says it does, and no check waits for a real 30 seconds.
std::atomic<uint64_t> g_fake_ms{1 << 20};
uint64_t fake_clock() noexcept { return g_fake_ms.load(std::memory_order_relaxed); }

// The engine device of these checks: every create of it is safe to make from several threads at once, it keeps the
// digest of each, and it can be made to refuse. Only the two pipeline entries are overridden; every other call
// stays the base's, which counts it as unexpected on this path.
class AsyncDevice final : public FakeDevice {
public:
    std::mutex lock;
    std::vector<Digest> digests;                // one per engine create, in the order the creates finished
    std::atomic<unsigned> creates{0};
    std::atomic<unsigned> computes{0};
    std::atomic<unsigned> unaligned{0};
    std::atomic<long> result{S_OK};             // what a create answers, as an HRESULT

    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid,
                                                 void** out) override {
        Digest d;
        if (desc) d = digest_of(desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
        return finish(std::move(d), riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid,
                                                        void** out) override {
        ++computes;
        Digest d;
        if (desc) {
            d.read = true;
            Subobject cs{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, {}};
            append_bytes(cs.value, &desc->CS, sizeof(desc->CS));
            d.types.push_back(cs.type);
            d.values.push_back(pointer_free(cs));
        }
        return finish(std::move(d), riid, out);
    }

private:
    HRESULT finish(Digest&& d, REFIID riid, void** out) {
        if (!d.aligned) ++unaligned;
        {
            std::lock_guard<std::mutex> held(lock);
            digests.push_back(std::move(d));
        }
        ++creates;
        const HRESULT hr = static_cast<HRESULT>(result.load());
        if (FAILED(hr)) {
            if (out) *out = nullptr;
            return hr;
        }
        return hand_out_pipeline(riid, out);
    }
};

// set_pso_policy on for the length of a scope and off at its end, as the shell's open() and close() do it.
class Async {
public:
    HRESULT hr = E_FAIL;

    Async(DeviceContext* context, uint32_t workers, uint32_t summary_period_ms = 0) : context_(context) {
        PsoPolicy policy{};
        policy.size = sizeof(PsoPolicy);
        policy.enabled = 1;
        policy.workers = workers;
        policy.summary_period_ms = summary_period_ms;
        policy.run = async_run;
        policy.drained = async_drained;
        policy.log = async_log;
        hr = set_pso_policy(context_, &policy);
    }
    ~Async() { off(); }
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;
    // The policy off, which ends every job of this device and joins the pool threads with the last device.
    HRESULT off() noexcept {
        PsoPolicy policy{};
        policy.size = sizeof(PsoPolicy);
        return set_pso_policy(context_, &policy);
    }

private:
    DeviceContext* context_;
};

// Waits for a state another thread reaches, bounded: a check fails on a false answer, where a wait would hang.
template <class Ready> bool until(Ready ready) {
    for (unsigned i = 0; i < 10000 && !ready(); ++i) Sleep(1);
    return ready();
}

// The job of a pipeline the DDI handed out, for a check that has to wait for one state of it (the records are
// engine-ddi's own storage, which internal.h describes).
const PsoJob* job_of(D3D12DDI_HPIPELINESTATE h) noexcept {
    return static_cast<const PipelineRecord*>(h.pDrvPrivate)->job;
}

// Microseconds, for the one check that compares two paths by time and not by what they did.
uint64_t now_us() noexcept {
    LARGE_INTEGER frequency{}, counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = static_cast<uint64_t>(frequency.QuadPart ? frequency.QuadPart : 1);
    return static_cast<uint64_t>(counter.QuadPart) * 1000000 / ticks;
}
} // namespace

int main() {
    // The refusals below log through engine-ddi's stderr sink by design: expected output, sent to stdout in order.
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));
    SetEnvironmentVariableA("AMDGPU_WDDM_LOG", "stderr");       // the sink is off by default (stdio-log.h)

    pso_set_clock(fake_clock);              // before any device of this program turns the policy on (pso-async.h)

    const FillInfo fill{sizeof(FillInfo), resolve_device};
    check(fill_device_core(&g_core, sizeof(g_core), &fill) == S_OK, "core table filled");

    // 1 to 4. A graphics pipeline: the stream entry, a stream the engine can read, and every state of the payload.
    {
        Fixture f;
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        check(hr == S_OK && f.device.streams == 1 && !f.device.graphics_descs && !f.device.unexpected,
              "graphics pipeline: one ID3D12Device2::CreatePipelineState, no CreateGraphicsPipelineState "
              "(hr %08lx, streams %u, graphics descs %u, unexpected %u)",
              static_cast<unsigned long>(hr), f.device.streams, f.device.graphics_descs, f.device.unexpected);

        Stream s;
        s.parse(f.device.last_stream);
        check(s.ok && !f.device.misaligned && s.objects.size() == 13,
              "graphics stream: pointer-aligned, walks end to end with no unknown or repeated type, 13 subobjects "
              "(ok %d, misaligned %u, %zu subobjects, %zu bytes)",
              s.ok ? 1 : 0, f.device.misaligned, s.objects.size(), f.device.last_stream.size());
        check(s.value<ID3D12RootSignature*>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) ==
                      static_cast<ID3D12RootSignature*>(&f.device.root) &&
                  s.value<D3D12_SHADER_BYTECODE>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS).BytecodeLength &&
                  s.value<D3D12_SHADER_BYTECODE>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS).BytecodeLength &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT),
              "graphics stream: the engine's root signature, a vertex and a pixel program, and no stage, stream "
              "output or input layout the payload did not name");
        const auto blend = s.value<D3D12_BLEND_DESC>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND);
        const auto raster = s.value<D3D12_RASTERIZER_DESC>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER);
        check(blend.AlphaToCoverageEnable && blend.RenderTarget[0].RenderTargetWriteMask == 0xF &&
                  raster.FillMode == D3D12_FILL_MODE_WIREFRAME && raster.CullMode == D3D12_CULL_MODE_FRONT &&
                  raster.ForcedSampleCount == 4,
              "graphics stream: BLEND and RASTERIZER carry the state records");
        const auto formats =
            s.value<D3D12_RT_FORMAT_ARRAY>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
        const auto sample = s.value<DXGI_SAMPLE_DESC>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC);
        check(s.value<UINT>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK) == 0x0F0F0F0F &&
                  formats.NumRenderTargets == 2 && formats.RTFormats[0] == DXGI_FORMAT_R8G8B8A8_UNORM &&
                  formats.RTFormats[1] == DXGI_FORMAT_R16G16_FLOAT &&
                  s.value<DXGI_FORMAT>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) ==
                      DXGI_FORMAT_D32_FLOAT &&
                  sample.Count == 4 && sample.Quality == 1 &&
                  s.value<UINT>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK) == 1 &&
                  s.value<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(
                      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE) ==
                      D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF &&
                  s.value<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) ==
                      D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
              "graphics stream: sample mask, formats, sample description, node mask, strip cut value and topology "
              "type as the payload has them");

        const auto depth = s.value<D3D12_DEPTH_STENCIL_DESC1>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1);
        check(!s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL) &&
                  !s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2) && depth.DepthEnable &&
                  depth.DepthFunc == D3D12_COMPARISON_FUNC_GREATER && depth.StencilReadMask == 0x5A &&
                  depth.StencilWriteMask == 0xA5 && !depth.DepthBoundsTestEnable,
              "depth bounds: the depth-stencil state travels as DEPTH_STENCIL1 alone, with DepthBoundsTestEnable "
              "FALSE for a state that does not ask for the bound test");
        check(!s.has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING),
              "view instancing: no VIEW_INSTANCING subobject for a pipeline without view instances");
        g_core.pfnDestroyPipelineState(f.h, pso);
    }

    // 4. A state with DepthBoundsTestEnable: this create used to answer E_NOTIMPL (L52).
    {
        Fixture f;
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, TRUE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        Stream s;
        s.parse(f.device.last_stream);
        const auto depth = s.value<D3D12_DEPTH_STENCIL_DESC1>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1);
        check(hr == S_OK && s.ok && depth.DepthBoundsTestEnable &&
                  depth.DepthFunc == D3D12_COMPARISON_FUNC_GREATER,
              "depth bounds: a pipeline whose state asks for the bound test is created, and DEPTH_STENCIL1 carries "
              "the flag (hr %08lx)", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) g_core.pfnDestroyPipelineState(f.h, pso);
    }

    // 5. View instancing: the locations and the flags reach the subobject (L57).
    {
        Fixture f;
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        const D3D12DDI_VIEW_INSTANCE_LOCATION locations[3] = {{7, 1}, {0, 2}, {5, 3}};
        a.ViewInstancingDesc = {3, locations, D3D12DDI_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        Stream s;
        s.parse(f.device.last_stream);
        const auto vi = s.value<D3D12_VIEW_INSTANCING_DESC>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING);
        const std::vector<D3D12_VIEW_INSTANCE_LOCATION>& read = f.device.last_locations;
        bool same = vi.ViewInstanceCount == 3 && read.size() == 3 &&
                    vi.Flags == D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING;
        for (size_t i = 0; same && i < read.size(); ++i)
            same = read[i].ViewportArrayIndex == locations[i].ViewportArrayIndex &&
                   read[i].RenderTargetArrayIndex == locations[i].RenderTargetArrayIndex;
        check(hr == S_OK && s.ok && s.objects.size() == 14 && same,
              "view instancing: three locations and the masking flag reach VIEW_INSTANCING (hr %08lx, count %u, "
              "%zu subobjects)", static_cast<unsigned long>(hr), vi.ViewInstanceCount, s.objects.size());
        if (SUCCEEDED(hr)) g_core.pfnDestroyPipelineState(f.h, pso);
    }

    // 5. A count the contract does not allow, and a count without an array: refused, and no engine call.
    {
        Fixture f;
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        const D3D12DDI_VIEW_INSTANCE_LOCATION locations[5] = {};
        a.ViewInstancingDesc = {5, locations, D3D12DDI_VIEW_INSTANCING_FLAG_NONE};
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT too_many = create(f, a, pso);
        a.ViewInstancingDesc = {2, nullptr, D3D12DDI_VIEW_INSTANCING_FLAG_NONE};
        const HRESULT no_array = create(f, a, pso);
        check(too_many == E_INVALIDARG && no_array == E_INVALIDARG && !f.device.streams,
              "view instancing: 5 locations and a count without an array are both E_INVALIDARG, and no pipeline "
              "reaches the engine (hr %08lx, %08lx, streams %u)", static_cast<unsigned long>(too_many),
              static_cast<unsigned long>(no_array), f.device.streams);
    }

    // 1. A compute pipeline still takes the compute entry, which has no stream form.
    {
        Fixture f;
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a{};
        a.hRootSignature = f.root_signature();
        a.hComputeShader = f.shader(D3D11_SB_COMPUTE_SHADER);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        check(hr == S_OK && f.device.compute_descs == 1 && !f.device.streams && !f.device.unexpected,
              "compute pipeline: one CreateComputePipelineState and no stream (hr %08lx, compute %u, streams %u)",
              static_cast<unsigned long>(hr), f.device.compute_descs, f.device.streams);
        if (SUCCEEDED(hr)) g_core.pfnDestroyPipelineState(f.h, pso);
    }

    // The engine's own failure on the stream entry is the answer, unchanged: pfnCreatePipelineState is not in the
    // runtime's AllowOutOfMemory category, and E_OUTOFMEMORY is admitted there in any case.
    {
        Fixture f;
        f.device.stream_result = E_OUTOFMEMORY;
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        check(hr == E_OUTOFMEMORY && f.device.streams == 1,
              "graphics pipeline: the engine's failure on the stream entry is the answer (hr %08lx)",
              static_cast<unsigned long>(hr));
    }

    // 6. Pipeline libraries, end to end: create from a blob, store, size, serialize, load, destroy.
    {
        Fixture f;
        const uint8_t stored_blob[] = {1, 2, 3, 4, 5, 6, 7};
        HRESULT created = E_FAIL;
        const D3D12DDI_HPIPELINELIBRARY lib = f.pipeline_library(stored_blob, sizeof(stored_blob), &created);
        check(created == S_OK && f.device.library_creates == 1 &&
                  f.device.last_library_blob == static_cast<const void*>(stored_blob) &&
                  f.device.last_library_blob_size == sizeof(stored_blob) &&
                  f.device.library.blob.size() == sizeof(stored_blob) && f.device.library.blob[6] == 7 &&
                  f.device.library.refs == 2,
              "pipeline library: the blob reaches the engine's CreatePipelineLibrary unchanged, pointer and size, "
              "and the record holds one reference to the engine's library (hr %08lx, creates %u, %zu bytes)",
              static_cast<unsigned long>(created), f.device.library_creates, f.device.library.blob.size());

        // D88: the pipeline at index 0x2A is stored under that index's name, with the engine's own pipeline.
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT made = create(f, a, pso);
        const HRESULT added = g_core.pfnAddPipelineStateToLibrary(f.h, lib, pso, 0x2A);
        check(made == S_OK && added == S_OK && f.device.library.stored.size() == 1 &&
                  f.device.library.stored[0] == L"0000002A" && f.device.library.stored_pipelines.size() == 1 &&
                  f.device.library.stored_pipelines[0] == static_cast<ID3D12PipelineState*>(&f.device.pipeline),
              "pipeline library: AddPipelineStateToLibrary stores the engine's pipeline under the name of its "
              "index (hr %08lx, %zu stored)", static_cast<unsigned long>(added), f.device.library.stored.size());

        // D89 and D90: the engine is handed exactly the capacity the size query answered, and a serialize that
        // no query covers is refused before it reaches the engine - the store above made the blob longer.
        f.device.library.serialized_size = 4096;
        std::vector<uint8_t> blob(4096, 0);
        const HRESULT early = g_core.pfnSerializeLibrary(f.h, lib, blob.data());
        const SIZE_T size = g_core.pfnCalcSerializedLibrarySize(f.h, lib);
        const HRESULT wrote = g_core.pfnSerializeLibrary(f.h, lib, blob.data());
        check(early == E_OUTOFMEMORY && size == 4096 && wrote == S_OK && f.device.library.serializes == 1 &&
                  f.device.library.size_queries == 1 && f.device.library.last_serialize_capacity == 4096 &&
                  blob[0] == 0x5D && blob[4095] == 0x5D,
              "pipeline library: a serialize with no size query behind it is refused and reaches no engine, and "
              "the one after the query writes the whole blob (hr %08lx and %08lx, %u serializes, capacity %zu)",
              static_cast<unsigned long>(early), static_cast<unsigned long>(wrote), f.device.library.serializes,
              static_cast<size_t>(f.device.library.last_serialize_capacity));
        const HRESULT again = g_core.pfnAddPipelineStateToLibrary(f.h, lib, pso, 0x2B);
        const HRESULT stale = g_core.pfnSerializeLibrary(f.h, lib, blob.data());
        check(again == S_OK && stale == E_OUTOFMEMORY && f.device.library.serializes == 1 &&
                  f.device.library.stored[1] == L"0000002B",
              "pipeline library: a store invalidates the recorded size, so the next serialize is refused until "
              "the runtime asks again (hr %08lx)", static_cast<unsigned long>(stale));

        // D89 with D90 across two threads. ID3D12PipelineLibrary is free-threaded and the size query carries the
        // only bound of the caller's buffer, so the remembered size belongs to the thread it was answered to. The
        // other thread here asks for its size (4096) and waits; this thread then stores a pipeline and asks again,
        // which answers the longer 8192; the other thread's serialize must be refused and must never be handed
        // this thread's larger size. The blob below is the longer one on purpose: with one remembered size per
        // library the engine would be told 8192 for a 4096 byte buffer, and a test that overflowed instead of
        // failing would say nothing.
        {
            std::vector<uint8_t> other_blob(8192, 0);
            std::atomic<bool> asked{false};
            std::atomic<bool> go{false};
            SIZE_T other_size = 0;
            HRESULT other_wrote = S_OK;
            std::thread other([&] {
                other_size = g_core.pfnCalcSerializedLibrarySize(f.h, lib);
                asked.store(true);
                while (!go.load()) std::this_thread::yield();
                other_wrote = g_core.pfnSerializeLibrary(f.h, lib, other_blob.data());
            });
            while (!asked.load()) std::this_thread::yield();
            const HRESULT stored_more = g_core.pfnAddPipelineStateToLibrary(f.h, lib, pso, 0x2C);
            f.device.library.serialized_size = 8192;
            const SIZE_T mine = g_core.pfnCalcSerializedLibrarySize(f.h, lib);
            go.store(true);
            other.join();
            check(other_size == 4096 && stored_more == S_OK && mine == 8192 && other_wrote == E_OUTOFMEMORY &&
                      f.device.library.serializes == 1 && f.device.library.last_serialize_capacity == 4096 &&
                      other_blob[0] == 0,
                  "pipeline library: a size query of another thread never becomes this thread's capacity - the "
                  "serialize of the thread whose size a store invalidated is refused (hr %08lx, last capacity "
                  "%zu, %u serializes)", static_cast<unsigned long>(other_wrote),
                  static_cast<size_t>(f.device.library.last_serialize_capacity), f.device.library.serializes);
            f.device.library.serialized_size = 4096;
        }

        // D34 with a LibraryReference: the same payload goes out as a load of the stream the create would have
        // written, out of the library, and the pipeline that comes back is the library's, not the device's. The
        // two streams carry the same state and not the same bytes: every pipeline create copies the programs into
        // a description of its own (BD-158, pso-async.h), so the addresses in the stream are its own as well.
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 load = a;
        HRESULT reopened_hr{};
        const auto reopened = f.pipeline_library(blob.data(), blob.size(), &reopened_hr);
        check(reopened_hr == S_OK && f.device.library.blob == blob,
              "library round trip: serialized bytes reach a newly created library");
        load.LibraryReference.hLibrary = reopened;
        load.LibraryReference.PipelineIndex = 0x2A;
        g_reported = S_OK;
        load.hBlendState = f.blend_state(load.LibraryReference);
        check(g_reported == S_OK, "library reload: blend descriptor recreates without a device error");
        g_reported = S_OK;
        load.hRasterizerState = f.rasterizer_state(load.LibraryReference);
        check(g_reported == S_OK, "library reload: rasterizer descriptor recreates without a device error");
        g_reported = S_OK;
        load.hDepthStencilState = f.depth_state(FALSE, load.LibraryReference);
        check(g_reported == S_OK, "library reload: depth descriptor recreates without a device error");
        D3D12DDIARG_CREATEELEMENTLAYOUT_0010 empty_layout{};
        empty_layout.LibraryReference = load.LibraryReference;
        load.hElementLayout = {f.storage(g_core.pfnCalcPrivateElementLayoutSize(f.h, &empty_layout))};
        g_reported = S_OK;
        g_core.pfnCreateElementLayout(f.h, &empty_layout, load.hElementLayout);
        check(g_reported == S_OK, "library reload: element layout recreates without a device error");
        const unsigned streams_before = f.device.streams;
        D3D12DDI_HPIPELINESTATE loaded{};
        const HRESULT loaded_hr = create(f, load, loaded);
        check(loaded_hr == S_OK && f.device.library.loads == 1 && !f.device.library.desc_loads &&
                  f.device.streams == streams_before && f.device.library.last_loaded == L"0000002A" &&
                  f.device.library.last_load_stream.size() == f.device.last_stream.size() &&
                  f.device.library.last_load_digest == f.device.last_digest &&
                  static_cast<const PipelineRecord*>(loaded.pDrvPrivate)->h.engine ==
                      static_cast<ID3D12PipelineState*>(&f.device.library.pipeline),
              "pipeline library: a create with a LibraryReference loads the index's name out of the library with "
              "the stream of the same payload, and never reaches the device's own entry (hr %08lx, loads %u, "
              "streams %u, %zu stream bytes, %s)", static_cast<unsigned long>(loaded_hr), f.device.library.loads,
              f.device.streams, f.device.library.last_load_stream.size(),
              difference(f.device.library.last_load_digest, f.device.last_digest).c_str());

        // A compute pipeline keeps its own entry on the load path too: the engine's LoadComputePipeline.
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 compute{};
        compute.hRootSignature = f.root_signature();
        compute.hComputeShader = f.shader(D3D11_SB_COMPUTE_SHADER);
        compute.LibraryReference.hLibrary = lib;
        compute.LibraryReference.PipelineIndex = 7;
        const unsigned compute_before = f.device.compute_descs;
        D3D12DDI_HPIPELINESTATE compute_pso{};
        const HRESULT compute_hr = create(f, compute, compute_pso);
        check(compute_hr == S_OK && f.device.library.compute_loads == 1 &&
                  f.device.compute_descs == compute_before && f.device.library.loads == 1 &&
                  f.device.library.last_loaded == L"00000007",
              "pipeline library: a compute create with a LibraryReference takes LoadComputePipeline, not the "
              "stream load and not the device's entry (hr %08lx, compute loads %u)",
              static_cast<unsigned long>(compute_hr), f.device.library.compute_loads);

        // A reference to something that is no live library of this device reaches no engine; the engine's own
        // refusal of a name the library does not hold is the answer, which is what D3D12 shows for either.
        uint8_t not_a_library[64]{};
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 bad = a;
        bad.LibraryReference.hLibrary.pDrvPrivate = not_a_library;
        D3D12DDI_HPIPELINESTATE refused{};
        const HRESULT bad_handle = create(f, bad, refused);
        f.device.library.load_result = E_INVALIDARG;
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 missing = a;
        missing.LibraryReference.hLibrary = lib;
        missing.LibraryReference.PipelineIndex = 0x99;
        const HRESULT no_name = create(f, missing, refused);
        f.device.library.load_result = S_OK;
        check(bad_handle == E_INVALIDARG && no_name == E_INVALIDARG && f.device.library.loads == 2 &&
                  f.device.library.last_loaded == L"00000099" && f.device.streams == streams_before,
              "pipeline library: a reference to no live library is E_INVALIDARG with no engine call, and a name "
              "the library does not hold is the engine's E_INVALIDARG (hr %08lx and %08lx)",
              static_cast<unsigned long>(bad_handle), static_cast<unsigned long>(no_name));

        // D87: the destroy releases the engine's library, and the handle is no library afterwards.
        g_core.pfnDestroyPipelineState(f.h, pso);
        g_core.pfnDestroyPipelineState(f.h, loaded);
        g_core.pfnDestroyPipelineState(f.h, compute_pso);
        g_core.pfnDestroyPipelineLibrary(f.h, reopened);
        const ULONG refs_before = f.device.library.refs;
        g_reported = S_OK;
        g_core.pfnDestroyPipelineLibrary(f.h, lib);
        const HRESULT after_destroy = g_reported;
        g_reported = S_OK;
        g_core.pfnDestroyPipelineLibrary(f.h, lib);
        check(refs_before == 2 && f.device.library.refs == 1 && after_destroy == S_OK &&
                  g_reported == E_INVALIDARG,
              "pipeline library: the destroy releases the engine's library and reports nothing, and the handle is "
              "no library afterwards (reported %08lx then %08lx)", static_cast<unsigned long>(after_destroy),
              static_cast<unsigned long>(g_reported));
    }

    // Library state handles must belong to this device and to a live, valid engine library.
    // A rejected create still leaves a destructible record, exactly as runtime cleanup expects.
    {
        Fixture f;
        const auto lib = f.pipeline_library(nullptr, 0);
        auto* record = static_cast<PipelineLibraryRecord*>(lib.pDrvPrivate);
        const auto good = record->h;
        DeviceContext foreign{};
        for (unsigned mode = 0; mode < 4; ++mode) {
            record->h = good;
            if (mode == 0) record->h.device = &foreign;
            if (mode == 1) record->h.flags |= kRecordInvalid;
            if (mode == 2) record->h.engine = nullptr;
            if (mode == 3) g_core.pfnDestroyPipelineLibrary(f.h, lib);
            const D3D12DDI_LIBRARY_REFERENCE_0010 ref{lib, 0x2A};
            g_reported = S_OK;
            const auto blend = f.blend_state(ref);
            check(g_reported == E_INVALIDARG, "library reference mode %u: blend refuses invalid owner/lifetime", mode);
            g_core.pfnDestroyBlendState(f.h, blend);
            g_reported = S_OK;
            const auto raster = f.rasterizer_state(ref);
            check(g_reported == E_INVALIDARG, "library reference mode %u: raster refuses invalid owner/lifetime", mode);
            g_core.pfnDestroyRasterizerState(f.h, raster);
            g_reported = S_OK;
            const auto depth = f.depth_state(FALSE, ref);
            check(g_reported == E_INVALIDARG, "library reference mode %u: depth refuses invalid owner/lifetime", mode);
            g_core.pfnDestroyDepthStencilState(f.h, depth);
            D3D12DDIARG_CREATEELEMENTLAYOUT_0010 layout{};
            layout.LibraryReference = ref;
            D3D12DDI_HELEMENTLAYOUT h{f.storage(g_core.pfnCalcPrivateElementLayoutSize(f.h, &layout))};
            g_reported = S_OK;
            g_core.pfnCreateElementLayout(f.h, &layout, h);
            check(g_reported == E_INVALIDARG, "library reference mode %u: layout refuses invalid owner/lifetime", mode);
            g_core.pfnDestroyElementLayout(f.h, h);
        }
    }

    // 6. D86 refusals: every engine failure is clamped to one the AllowOutOfMemory category admits, the two codes
    // a stale or foreign blob answers included, and a size with no blob never reaches the engine. The two blob
    // codes are an inference away from being the better diagnosis and the clamp is what a measurement would lift
    // (pipelines.cpp, the comment above create_pipeline_library); until then the device is worth more.
    {
        Fixture f;
        f.device.library_result = D3D12_ERROR_DRIVER_VERSION_MISMATCH;
        HRESULT mismatch = S_OK;
        f.pipeline_library(nullptr, 0, &mismatch);
        f.device.library_result = D3D12_ERROR_ADAPTER_NOT_FOUND;
        HRESULT adapter = S_OK;
        f.pipeline_library(nullptr, 0, &adapter);
        f.device.library_result = E_FAIL;
        HRESULT other = S_OK;
        f.pipeline_library(nullptr, 0, &other);
        f.device.library_result = S_OK;
        const unsigned creates_before = f.device.library_creates;
        HRESULT no_blob = S_OK;
        f.pipeline_library(nullptr, 16, &no_blob);
        check(mismatch == E_OUTOFMEMORY && adapter == E_OUTOFMEMORY && other == E_OUTOFMEMORY &&
                  no_blob == E_OUTOFMEMORY && f.device.library_creates == creates_before,
              "pipeline library: every engine failure of the create is E_OUTOFMEMORY, the two codes of a stale "
              "blob included, and a size without a blob is refused before the engine (hr %08lx, %08lx, %08lx, "
              "%08lx)", static_cast<unsigned long>(mismatch), static_cast<unsigned long>(adapter),
              static_cast<unsigned long>(other), static_cast<unsigned long>(no_blob));
    }

    // 7. BD-158: asynchronous compilation on a pool of threads (set_pso_policy, pso-async.cpp).
    // 7a. The policy's shape. Nothing starts a thread until the shape is the one the boundary names, and one
    // device has one policy.
    {
        Fixture f;
        PsoPolicy good{};
        good.size = sizeof(PsoPolicy);
        good.enabled = 1;
        good.workers = 2;
        good.run = async_run;
        good.drained = async_drained;
        PsoPolicy bad = good;
        bad.size = sizeof(PsoPolicy) - sizeof(uint32_t);
        const HRESULT wrong_size = set_pso_policy(&f.context, &bad);
        bad = good;
        bad.enabled = 2;
        const HRESULT wrong_enabled = set_pso_policy(&f.context, &bad);
        bad = good;
        bad.workers = kMaxPsoWorkers + 1;
        const HRESULT too_many = set_pso_policy(&f.context, &bad);
        bad = good;
        bad.run = nullptr;
        const HRESULT no_run = set_pso_policy(&f.context, &bad);
        bad = good;
        bad.drained = nullptr;
        const HRESULT no_drained = set_pso_policy(&f.context, &bad);
        const HRESULT no_context = set_pso_policy(nullptr, &good);
        const HRESULT no_policy = set_pso_policy(&f.context, nullptr);
        check(wrong_size == E_INVALIDARG && wrong_enabled == E_INVALIDARG && too_many == E_INVALIDARG &&
                  no_run == E_INVALIDARG && no_drained == E_INVALIDARG && no_context == E_INVALIDARG &&
                  no_policy == E_INVALIDARG && pso_pool_workers() == 0,
              "async policy: a wrong size, a wrong enabled, more workers than the pool takes, a missing hook, no "
              "context and no policy are all E_INVALIDARG, and none of them starts a thread (%08lx %08lx %08lx "
              "%08lx %08lx %08lx %08lx, %u threads)",
              static_cast<unsigned long>(wrong_size), static_cast<unsigned long>(wrong_enabled),
              static_cast<unsigned long>(too_many), static_cast<unsigned long>(no_run),
              static_cast<unsigned long>(no_drained), static_cast<unsigned long>(no_context),
              static_cast<unsigned long>(no_policy), pso_pool_workers());

        const HRESULT on = set_pso_policy(&f.context, &good);
        const uint32_t workers = pso_pool_workers();
        const HRESULT again = set_pso_policy(&f.context, &good);
        PsoPolicy off{};
        off.size = sizeof(PsoPolicy);
        const HRESULT off_hr = set_pso_policy(&f.context, &off);
        const HRESULT off_again = set_pso_policy(&f.context, &off);
        check(on == S_OK && workers == 2 && again == E_INVALIDARG && off_hr == S_OK && off_again == S_OK &&
                  pso_pool_workers() == 0,
              "async policy: two workers on, a second policy on one device refused, and the threads joined again "
              "at off, which is also harmless twice (%08lx, %u threads, again %08lx, off %08lx %08lx, %u left)",
              static_cast<unsigned long>(on), workers, static_cast<unsigned long>(again),
              static_cast<unsigned long>(off_hr), static_cast<unsigned long>(off_again), pso_pool_workers());
    }

    // 7b. The create answers before the engine has seen the pipeline, and the state that arrives on the pool
    // thread is the state the synchronous create of the same payload arrives with, view-instance locations and
    // program bytes included - all of it read after the DDI call that named them has returned.
    {
        AsyncDevice device;
        Fixture f(&device);
        g_runs = 0;
        g_drains = 0;
        const D3D12DDI_VIEW_INSTANCE_LOCATION locations[3] = {{7, 1}, {0, 2}, {5, 3}};
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, TRUE);
        a.ViewInstancingDesc = {3, locations, D3D12DDI_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING};

        D3D12DDI_HPIPELINESTATE reference{};
        const HRESULT sync_hr = create(f, a, reference);         // the path as it was before the policy existed
        g_core.pfnDestroyPipelineState(f.h, reference);
        const unsigned synchronous_creates = device.creates.load();

        Async async(&f.context, 1);
        g_gate.shut();                                           // the pool thread stops before it compiles
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        const unsigned at_return = device.creates.load() - synchronous_creates;
        const bool handed_out = until([] { return g_runs.load() == 1; });
        g_gate.release();

        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const HRESULT stored = g_core.pfnAddPipelineStateToLibrary(f.h, library, pso, 0);
        g_core.pfnCalcSerializedLibrarySize(f.h, library);           // where a deferred store is made (D89)
        const PsoAsync* counters = f.context.pso;
        const uint64_t queued = counters ? counters->queued.load() : 0;
        const uint64_t by_worker = counters ? counters->by_worker.load() : 0;
        const uint64_t by_consumer = counters ? counters->by_consumer.load() : 0;
        const uint64_t synchronous = counters ? counters->synchronous.load() : 0;
        check(async.hr == S_OK && sync_hr == S_OK && hr == S_OK && at_return == 0 && handed_out &&
                  stored == S_OK && device.creates.load() == 2 && !device.unaligned.load() &&
                  device.digests.size() == 2 && device.digests[0].read && device.digests[0] == device.digests[1],
              "async create: S_OK with no engine create yet, then the same state on a pool thread as the "
              "synchronous create of the payload (hr %08lx, engine creates at return %u, handed out %d, stored "
              "%08lx, %u creates, %zu digests, %s)",
              static_cast<unsigned long>(hr), at_return, handed_out ? 1 : 0, static_cast<unsigned long>(stored),
              device.creates.load(), device.digests.size(), first_difference(device.digests).c_str());
        check(queued == 1 && by_worker == 1 && by_consumer == 0 && synchronous == 0 && g_drains.load() >= 1,
              "async create: counted as one queued job run by a pool thread, and the consumer called the drained "
              "hook on its own thread (queued %llu, by a worker %llu, by the consumer %llu, synchronous %llu, "
              "drains %u)", static_cast<unsigned long long>(queued), static_cast<unsigned long long>(by_worker),
              static_cast<unsigned long long>(by_consumer), static_cast<unsigned long long>(synchronous),
              g_drains.load());
        g_core.pfnDestroyPipelineState(f.h, pso);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7c. A game's loading thread: many creates in a row from one thread, then a consumer for each.
    {
        AsyncDevice device;
        Fixture f(&device);
        constexpr unsigned kCount = 64;
        Async async(&f.context, 0);                              // the default: logical processors - 1
        std::vector<D3D12DDI_HPIPELINESTATE> pipelines;
        bool created = true;
        for (unsigned i = 0; i < kCount; ++i) {
            const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
            D3D12DDI_HPIPELINESTATE pso{};
            created = create(f, a, pso) == S_OK && created;
            pipelines.push_back(pso);
        }
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        bool consumed = true;
        for (unsigned i = 0; i < kCount; ++i)
            consumed = g_core.pfnAddPipelineStateToLibrary(f.h, library, pipelines[i], i) == S_OK && consumed;
        g_core.pfnCalcSerializedLibrarySize(f.h, library);           // every deferred store, made here
        const PsoAsync* counters = f.context.pso;
        const uint64_t by_worker = counters ? counters->by_worker.load() : 0;
        const uint64_t by_consumer = counters ? counters->by_consumer.load() : 0;
        const uint64_t compile_failures = counters ? counters->failures.load() : 0;
        bool same = device.digests.size() == kCount;
        for (size_t i = 0; same && i < device.digests.size(); ++i)
            same = device.digests[i].read && device.digests[i] == device.digests[0];
        check(created && consumed && device.creates.load() == kCount && by_worker + by_consumer == kCount &&
                  !compile_failures && same && pso_pool_workers() >= 1,
              "async creates in a row: %u creates, %u engine creates, %llu by a worker and %llu by the consumer, "
              "every stream the same state (created %d, consumed %d, %u pool threads, %s)",
              kCount, device.creates.load(), static_cast<unsigned long long>(by_worker),
              static_cast<unsigned long long>(by_consumer), created ? 1 : 0, consumed ? 1 : 0,
              pso_pool_workers(), first_difference(device.digests).c_str());
        for (const D3D12DDI_HPIPELINESTATE& pso : pipelines) g_core.pfnDestroyPipelineState(f.h, pso);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7d. The same from four threads at once, which is the shape of a game that creates pipelines from its own
    // worker threads. The payloads and the private storage are made first, so that the threads do nothing but
    // call the slot.
    {
        AsyncDevice device;
        Fixture f(&device);
        struct Slot {
            D3D12DDIARG_CREATE_PIPELINE_STATE_0075 args{};
            D3D12DDI_HPIPELINESTATE pso{};
            HRESULT hr = E_FAIL;
        };
        constexpr unsigned kThreads = 4;
        constexpr unsigned kCount = 64;
        std::vector<Slot> slots(kCount);
        for (Slot& s : slots) {
            s.args = graphics_args(f, FALSE);
            s.pso.pDrvPrivate = f.storage(g_core.pfnCalcPrivatePipelineStateSize(f.h, &s.args));
        }
        Async async(&f.context, 0);
        {
            std::vector<std::thread> threads;
            for (unsigned t = 0; t < kThreads; ++t)
                threads.emplace_back([&slots, &f, t] {
                    for (size_t i = t; i < slots.size(); i += kThreads)
                        slots[i].hr = g_core.pfnCreatePipelineState(f.h, &slots[i].args, slots[i].pso,
                                                                    D3D12DDI_HRTPIPELINESTATE{});
                });
            for (std::thread& t : threads) t.join();
        }
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        bool ok = true;
        for (unsigned i = 0; i < kCount; ++i)
            ok = slots[i].hr == S_OK &&
                 g_core.pfnAddPipelineStateToLibrary(f.h, library, slots[i].pso, i) == S_OK && ok;
        g_core.pfnCalcSerializedLibrarySize(f.h, library);
        bool same = device.digests.size() == kCount;
        for (size_t i = 0; same && i < device.digests.size(); ++i)
            same = device.digests[i].read && device.digests[i] == device.digests[0];
        check(ok && same && device.creates.load() == kCount && !device.unexpected,
              "async creates from %u threads: %u pipelines created and consumed, %u engine creates, every stream "
              "the same state (ok %d, unexpected %u, %s)",
              kThreads, kCount, device.creates.load(), ok ? 1 : 0, device.unexpected,
              first_difference(device.digests).c_str());
        for (const Slot& s : slots) g_core.pfnDestroyPipelineState(f.h, s.pso);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7e. A pipeline destroyed before anything used it: the compile that nobody waits for is not made at all.
    {
        AsyncDevice device;
        Fixture f(&device);
        g_runs = 0;
        Async async(&f.context, 1);
        g_gate.shut();
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 first_args = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE first{};
        const HRESULT first_hr = create(f, first_args, first);
        const bool parked = until([] { return g_runs.load() == 1; });   // the only worker holds the first job
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 spare_args = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE spare{};
        const HRESULT spare_hr = create(f, spare_args, spare);
        g_core.pfnDestroyPipelineState(f.h, spare);
        const PsoAsync* counters = f.context.pso;
        const uint64_t aborted = counters ? counters->aborted.load() : 0;
        g_gate.release();
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const HRESULT stored = g_core.pfnAddPipelineStateToLibrary(f.h, library, first, 0);
        g_core.pfnCalcSerializedLibrarySize(f.h, library);
        check(first_hr == S_OK && spare_hr == S_OK && parked && aborted == 1 && stored == S_OK &&
                  device.creates.load() == 1,
              "async destroy before use: the queued job ends with no engine create, and the other pipeline is "
              "unaffected (hr %08lx %08lx, parked %d, ended without a create %llu, stored %08lx, %u engine "
              "creates)", static_cast<unsigned long>(first_hr), static_cast<unsigned long>(spare_hr),
              parked ? 1 : 0, static_cast<unsigned long long>(aborted), static_cast<unsigned long>(stored),
              device.creates.load());
        g_core.pfnDestroyPipelineState(f.h, first);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7f. A consumer that arrives before any worker took the job makes the compile on its own thread, which is
    // what keeps a one-core machine and a busy pool from waiting for a queue.
    {
        AsyncDevice device;
        Fixture f(&device);
        g_runs = 0;
        Async async(&f.context, 1);
        g_gate.shut();
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 held_args = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE held{};
        const HRESULT held_hr = create(f, held_args, held);
        const bool parked = until([] { return g_runs.load() == 1; });
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 mine_args = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE mine{};
        const HRESULT mine_hr = create(f, mine_args, mine);
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const HRESULT stored = g_core.pfnAddPipelineStateToLibrary(f.h, library, mine, 0);
        g_core.pfnCalcSerializedLibrarySize(f.h, library);           // the store, and with it the compile, here
        const unsigned creates_after = device.creates.load();
        const PsoAsync* counters = f.context.pso;
        const uint64_t by_consumer = counters ? counters->by_consumer.load() : 0;
        const uint64_t waits = counters ? counters->waits.load() : 0;
        g_gate.release();
        check(held_hr == S_OK && mine_hr == S_OK && parked && stored == S_OK && by_consumer == 1 && !waits &&
                  creates_after == 1 && g_runs.load() == 1,
              "async consumer before worker: the consumer compiles the queued job itself and waits for nothing "
              "(stored %08lx, by the consumer %llu, waits %llu, engine creates %u, run hooks %u)",
              static_cast<unsigned long>(stored), static_cast<unsigned long long>(by_consumer),
              static_cast<unsigned long long>(waits), creates_after, g_runs.load());
        g_core.pfnDestroyPipelineState(f.h, mine);
        g_core.pfnDestroyPipelineState(f.h, held);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7g. A compile that fails after the create answered S_OK: every consumer gets a failure of its own, the
    // engine is asked once, and the pipeline is never handed out.
    {
        AsyncDevice device;
        Fixture f(&device);
        device.result = E_INVALIDARG;
        Async async(&f.context, 1);
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        const bool compiled = until([&] { return !pso_pending(job_of(pso)); });
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const HRESULT first = g_core.pfnAddPipelineStateToLibrary(f.h, library, pso, 0);
        const HRESULT second = g_core.pfnAddPipelineStateToLibrary(f.h, library, pso, 1);
        const PsoAsync* counters = f.context.pso;
        const uint64_t compile_failures = counters ? counters->failures.load() : 0;
        check(hr == S_OK && compiled && first == E_OUTOFMEMORY && second == E_OUTOFMEMORY &&
                  compile_failures == 1 && device.creates.load() == 1 && device.library.stored.empty(),
              "async failure after S_OK: the create answered S_OK, both consumers answer the code a creation slot "
              "admits, the engine was asked once and the library stored nothing (hr %08lx, compiled %d, %08lx, "
              "%08lx, failures %llu, %u engine creates, %zu stored)", static_cast<unsigned long>(hr),
              compiled ? 1 : 0, static_cast<unsigned long>(first), static_cast<unsigned long>(second),
              static_cast<unsigned long long>(compile_failures), device.creates.load(),
              device.library.stored.size());
        g_core.pfnDestroyPipelineState(f.h, pso);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7h. The device's destruction with a job still queued: the policy off ends it without an engine create, and
    // a consumer that comes afterwards is answered a failure rather than "no pipeline at all", which would draw
    // with whatever the list had bound.
    {
        AsyncDevice device;
        Fixture f(&device);
        g_runs = 0;
        Async async(&f.context, 1);
        g_gate.shut();
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 held_args = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE held{};
        const HRESULT held_hr = create(f, held_args, held);
        const bool parked = until([] { return g_runs.load() == 1; });
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 stranded_args = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE stranded{};
        const HRESULT stranded_hr = create(f, stranded_args, stranded);
        const auto* record = static_cast<const PipelineRecord*>(stranded.pDrvPrivate);
        // The policy off on another thread: it ends the queued job, then joins the pool thread this test holds.
        std::thread off([&async] { (void)async.off(); });
        const bool ended = record->job && until([record] {
            return record->job->state.load(std::memory_order_acquire) == static_cast<uint32_t>(PsoState::Done);
        });
        g_gate.release();
        off.join();
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const HRESULT stored = g_core.pfnAddPipelineStateToLibrary(f.h, library, stranded, 0);
        check(held_hr == S_OK && stranded_hr == S_OK && parked && ended && stored == E_OUTOFMEMORY &&
                  device.creates.load() == 1 && f.context.pso == nullptr && pso_pool_workers() == 0,
              "async device destruction: the queued job ends with no engine create and its consumer is refused "
              "(hr %08lx %08lx, parked %d, ended %d, stored %08lx, %u engine creates, %u pool threads)",
              static_cast<unsigned long>(held_hr), static_cast<unsigned long>(stranded_hr), parked ? 1 : 0,
              ended ? 1 : 0, static_cast<unsigned long>(stored), device.creates.load(), pso_pool_workers());
        g_core.pfnDestroyPipelineState(f.h, stranded);
        g_core.pfnDestroyPipelineState(f.h, held);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7i. A store of a pipeline whose compile is still running: the store answers at once and reaches the engine
    // when the blob is collected, and a size another thread held before it does not become a capacity.
    {
        AsyncDevice device;
        Fixture f(&device);
        device.library.serialized_size = 4096;
        Async async(&f.context, 1);
        g_gate.shut();                                               // no compile finishes while the gate is shut
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const SIZE_T early_size = g_core.pfnCalcSerializedLibrarySize(f.h, library);
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT created = create(f, a, pso);
        const HRESULT stored = g_core.pfnAddPipelineStateToLibrary(f.h, library, pso, 9);
        const size_t at_store = device.library.stored.size();
        const unsigned creates_at_store = device.creates.load();
        std::vector<uint8_t> blob(4096, 0);
        g_gate.release();
        // This serialize makes the deferred store and is then refused: the size it was given before the store was
        // deferred describes a shorter blob than the library now holds.
        const HRESULT stale = g_core.pfnSerializeLibrary(f.h, library, blob.data());
        const SIZE_T size = g_core.pfnCalcSerializedLibrarySize(f.h, library);
        const HRESULT wrote = g_core.pfnSerializeLibrary(f.h, library, blob.data());
        check(created == S_OK && stored == S_OK && !at_store && !creates_at_store && early_size == 4096 &&
                  stale == E_OUTOFMEMORY && size == 4096 && wrote == S_OK &&
                  device.library.stored.size() == 1 && device.library.stored[0] == L"00000009" &&
                  device.creates.load() == 1,
              "async deferred store: the store answers S_OK with no engine call at all, the size a thread held "
              "before it is no capacity any more, and the collected blob carries the pipeline (created %08lx, "
              "stored %08lx, stored at the call %zu, engine creates at the call %u, stale serialize %08lx, size "
              "%zu, serialize %08lx, %zu stored, %u engine creates)",
              static_cast<unsigned long>(created), static_cast<unsigned long>(stored), at_store, creates_at_store,
              static_cast<unsigned long>(stale), static_cast<size_t>(size), static_cast<unsigned long>(wrote),
              device.library.stored.size(), device.creates.load());
        g_core.pfnDestroyPipelineState(f.h, pso);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7j. The pipeline's destroy is the last chance of a store that still waits, and a library that is destroyed
    // with stores waiting compiles none of them.
    {
        AsyncDevice device;
        Fixture f(&device);
        Async async(&f.context, 1);
        g_gate.shut();
        const D3D12DDI_HPIPELINELIBRARY kept = f.pipeline_library(nullptr, 0);
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE destroyed_first{};
        const HRESULT created = create(f, a, destroyed_first);
        const HRESULT stored = g_core.pfnAddPipelineStateToLibrary(f.h, kept, destroyed_first, 3);
        g_gate.release();
        g_core.pfnDestroyPipelineState(f.h, destroyed_first);        // the store is made here, with the pipeline
        const size_t after_destroy = device.library.stored.size();
        const unsigned creates_after_destroy = device.creates.load();

        const D3D12DDI_HPIPELINELIBRARY given_up = f.pipeline_library(nullptr, 0);
        g_gate.shut();
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 b = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE dropped{};
        const HRESULT created_dropped = create(f, b, dropped);
        const HRESULT stored_dropped = g_core.pfnAddPipelineStateToLibrary(f.h, given_up, dropped, 4);
        g_core.pfnDestroyPipelineLibrary(f.h, given_up);              // nothing of it is ever read
        const unsigned creates_after_library = device.creates.load();
        g_gate.release();
        check(created == S_OK && stored == S_OK && after_destroy == 1 && creates_after_destroy == 1 &&
                  device.library.stored[0] == L"00000003" && created_dropped == S_OK && stored_dropped == S_OK &&
                  creates_after_library == 1,
              "async deferred store: a destroy of the pipeline makes the store it still owes, and a destroy of the "
              "library drops it without a compile (stored after the destroy %zu, engine creates %u, then %u after "
              "the library's destroy)", after_destroy, creates_after_destroy, creates_after_library);
        g_core.pfnDestroyPipelineState(f.h, dropped);
        g_core.pfnDestroyPipelineLibrary(f.h, kept);
    }

    // 7k. The warm path: a create whose engine answers at once, which is what the engine's own disk cache makes of
    // it (BD-158: the warm start of Death Stranding is a cache hit for every pipeline). The pool must not make
    // that path slower in a way a loading screen would notice. Reported, not gated: this machine is not the lab,
    // and a time is not a contract.
    {
        constexpr unsigned kCount = 256;
        uint64_t off_us = 0, on_us = 0;
        unsigned off_creates = 0, on_creates = 0;
        for (unsigned pass = 0; pass < 2; ++pass) {
            AsyncDevice device;
            Fixture f(&device);
            std::vector<D3D12DDIARG_CREATE_PIPELINE_STATE_0075> args(kCount);
            std::vector<D3D12DDI_HPIPELINESTATE> pipelines(kCount);
            for (unsigned i = 0; i < kCount; ++i) {
                args[i] = graphics_args(f, FALSE);
                pipelines[i].pDrvPrivate = f.storage(g_core.pfnCalcPrivatePipelineStateSize(f.h, &args[i]));
            }
            const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
            Async async(&f.context, pass ? 0u : 1u);
            if (!pass) (void)async.off();                            // pass 0: the path as it was, on this thread
            const uint64_t start = now_us();
            for (unsigned i = 0; i < kCount; ++i) {
                g_core.pfnCreatePipelineState(f.h, &args[i], pipelines[i], D3D12DDI_HRTPIPELINESTATE{});
                g_core.pfnAddPipelineStateToLibrary(f.h, library, pipelines[i], i);
            }
            g_core.pfnCalcSerializedLibrarySize(f.h, library);        // every store, deferred or not, is made
            const uint64_t spent = now_us() - start;
            if (pass) {
                on_us = spent;
                on_creates = device.creates.load();
            } else {
                off_us = spent;
                off_creates = device.creates.load();
            }
            for (const D3D12DDI_HPIPELINESTATE& pso : pipelines) g_core.pfnDestroyPipelineState(f.h, pso);
            g_core.pfnDestroyPipelineLibrary(f.h, library);
        }
        check(off_creates == kCount && on_creates == kCount,
              "warm path: %u creates and stores with the policy off in %llu us, with it on in %llu us "
              "(%llu and %llu us each; engine creates %u and %u)", kCount,
              static_cast<unsigned long long>(off_us), static_cast<unsigned long long>(on_us),
              static_cast<unsigned long long>(off_us / kCount), static_cast<unsigned long long>(on_us / kCount),
              off_creates, on_creates);
    }

    // 7l. The periodic counter line (BD-158, trials 598-600: Death Stranding never destroys its device, so the
    // summary of the destruction is never reached in a game). One line a period while the device compiles, and
    // nothing at all while it only binds what is already compiled.
    {
        AsyncDevice device;
        Fixture f(&device);
        constexpr uint32_t kPeriod = 1000;
        Async async(&f.context, 1, kPeriod);
        const D3D12DDI_HPIPELINELIBRARY library = f.pipeline_library(nullptr, 0);
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE first{}, second{}, third{};
        const HRESULT first_hr = create(f, a, first);
        const HRESULT second_hr = create(f, a, second);
        const bool compiled = until([&] { return !pso_pending(job_of(first)) && !pso_pending(job_of(second)); });
        clear_lines();
        // Inside the period: a consumer of a compiled pipeline, with every compile of this device already counted.
        g_core.pfnAddPipelineStateToLibrary(f.h, library, first, 0);
        const unsigned inside = summary_lines("periodic");
        // The period passes: the next consumer writes the line, which is the only one for all of this work.
        g_fake_ms += kPeriod;
        g_core.pfnAddPipelineStateToLibrary(f.h, library, second, 1);
        const unsigned after_period = summary_lines("periodic");
        // Another period with no compilation at all: a bind of what is compiled writes nothing.
        g_fake_ms += kPeriod;
        g_core.pfnAddPipelineStateToLibrary(f.h, library, first, 2);
        const unsigned idle = summary_lines("periodic");
        // New work in a period that has passed: the line comes back.
        const HRESULT third_hr = create(f, a, third);
        const unsigned working = summary_lines("periodic");
        check(first_hr == S_OK && second_hr == S_OK && third_hr == S_OK && compiled && inside == 0 &&
                  after_period == 1 && idle == 1 && working == 2,
              "async periodic summary: no line inside the period, one when it has passed, none for a period of "
              "binds alone, and one again when a create arrives (compiled %d, lines %u, %u, %u, %u)",
              compiled ? 1 : 0, inside, after_period, idle, working);
        g_core.pfnDestroyPipelineState(f.h, third);
        g_core.pfnDestroyPipelineState(f.h, second);
        g_core.pfnDestroyPipelineState(f.h, first);
        g_core.pfnDestroyPipelineLibrary(f.h, library);
    }

    // 7m. The last print site: a process that unloads the DLL without destroying its device (the Death Stranding
    // exit) still writes the counters, and the call changes nothing else - no join, no policy off, no pool thread
    // touched - so that the shell may make it from DllMain.
    {
        AsyncDevice device;
        Fixture f(&device);
        Async async(&f.context, 1);
        const D3D12DDIARG_CREATE_PIPELINE_STATE_0075 a = graphics_args(f, FALSE);
        D3D12DDI_HPIPELINESTATE pso{};
        const HRESULT hr = create(f, a, pso);
        const bool compiled = until([&] { return !pso_pending(job_of(pso)); });
        const uint32_t workers_before = pso_pool_workers();
        clear_lines();
        const uint32_t written = pso_detach_summary();                // the device is never destroyed before this
        const unsigned lines = summary_lines("process detach");
        const bool still_on = f.context.pso != nullptr;
        const uint32_t workers_after = pso_pool_workers();
        g_core.pfnDestroyPipelineState(f.h, pso);
        const HRESULT off = async.off();
        const uint32_t after_off = pso_detach_summary();              // nothing is left to write
        check(hr == S_OK && compiled && written == 1 && lines == 1 && still_on && workers_before == 1 &&
                  workers_after == 1 && off == S_OK && after_off == 0,
              "async detach summary: one line for the device that was never destroyed, the policy and the pool "
              "untouched, and nothing written once the policy is off (hr %08lx, compiled %d, written %u, lines %u, "
              "still on %d, pool %u then %u, off %08lx, after the policy off %u)",
              static_cast<unsigned long>(hr), compiled ? 1 : 0, written, lines, still_on ? 1 : 0, workers_before,
              workers_after, static_cast<unsigned long>(off), after_off);
    }

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
