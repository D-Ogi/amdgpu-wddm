// SPDX-License-Identifier: MIT
// engine-ddi: work graphs tier 1.0 (DirectX-Specs WorkGraphs.md, "DDI"; WDK 10.0.28000 d3d12umddi.h, build 0108), on
// the engine's ID3D12Device5::CreateStateObject, ID3D12StateObjectProperties1, ID3D12WorkGraphProperties and
// ID3D12GraphicsCommandList10.
//
// The DDI description is not the API's. The runtime validates the application's D3D12_WORK_GRAPH_DESC, applies every
// override and population rule, and gives the driver the final graph (WorkGraphs.md, D3D12DDI_WORK_GRAPH_DESC_0108):
// every node with its final properties, and the entry points. The entry point index that DispatchGraph uses, also in
// GPU memory, is the position in pEntrypoints. So the API description given to the engine is:
//   - every DDI node as an explicitly defined node, its final properties as overrides (final name, program entry,
//     local root arguments table index, shared input, dispatch grids, and per output the final target name, sparse
//     nodes and record budgets);
//   - pEntrypoints in the DDI order;
//   - the engine's flag kWorkGraphFlagDdiOrder: the engine takes the entry points in that order, assigns no table
//     index, and refuses an override that it does not apply and that differs from the shader (a rename, for one).
// ADD_TO_EXISTING_WORK_GRAPH (AddToStateObject) is not implemented and refused.
//
// DispatchGraph reads its CPU inputs during the call (WorkGraphs.md, DispatchGraph: the records may be freed when the
// call returns). With deferred replay the engine reads them later, so the slot copies them: the bytes that the records
// of an input span, (NumRecords - 1) * stride plus the entry point's record size. The engine gives that size
// (GetEntrypointRecordSizeInBytes), so SetProgram remembers the program for the list. The GPU inputs are addresses.
#include "work-graphs.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "replay.h"
#include <climits>
#include <cstring>
#include <map>
#include <mutex>

namespace engine_ddi {

#define ENGINE_DDI_SAME(D, dm, A, am) static_assert(offsetof(D, dm) == offsetof(A, am), #D "::" #dm)
static_assert(sizeof(D3D12DDI_PROGRAM_IDENTIFIER_0108) == sizeof(D3D12_PROGRAM_IDENTIFIER), "program identifier");
static_assert(sizeof(D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108) == sizeof(D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS),
              "memory requirements");
ENGINE_DDI_SAME(D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108, MinSizeInBytes, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS,
                MinSizeInBytes);
ENGINE_DDI_SAME(D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108, MaxSizeInBytes, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS,
                MaxSizeInBytes);
ENGINE_DDI_SAME(D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108, SizeGranularityInBytes,
                D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS, SizeGranularityInBytes);
static_assert(sizeof(D3D12DDI_NODE_CPU_INPUT_0108) == sizeof(D3D12_NODE_CPU_INPUT), "CPU input");
ENGINE_DDI_SAME(D3D12DDI_NODE_CPU_INPUT_0108, EntrypointIndex, D3D12_NODE_CPU_INPUT, EntrypointIndex);
ENGINE_DDI_SAME(D3D12DDI_NODE_CPU_INPUT_0108, NumRecords, D3D12_NODE_CPU_INPUT, NumRecords);
ENGINE_DDI_SAME(D3D12DDI_NODE_CPU_INPUT_0108, pRecords, D3D12_NODE_CPU_INPUT, pRecords);
ENGINE_DDI_SAME(D3D12DDI_NODE_CPU_INPUT_0108, RecordStrideInBytes, D3D12_NODE_CPU_INPUT, RecordStrideInBytes);
#undef ENGINE_DDI_SAME
static_assert(D3D12DDI_STATE_SUBOBJECT_TYPE_WORK_GRAPH == static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH),
              "work graph subobject");
static_assert(D3D12DDI_DISPATCH_MODE_NODE_CPU_INPUT_0108 == static_cast<int>(D3D12_DISPATCH_MODE_NODE_CPU_INPUT) &&
                  D3D12DDI_DISPATCH_MODE_NODE_GPU_INPUT_0108 == static_cast<int>(D3D12_DISPATCH_MODE_NODE_GPU_INPUT) &&
                  D3D12DDI_DISPATCH_MODE_MULTI_NODE_CPU_INPUT_0108 ==
                      static_cast<int>(D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT) &&
                  D3D12DDI_DISPATCH_MODE_MULTI_NODE_GPU_INPUT_0108 ==
                      static_cast<int>(D3D12_DISPATCH_MODE_MULTI_NODE_GPU_INPUT),
              "dispatch modes");
static_assert(D3D12DDI_SET_WORK_GRAPH_FLAG_INITIALIZE_0108 == static_cast<int>(D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE),
              "set work graph flags");

namespace {
// Bounds on what one description counts: nodes, entry points, outputs of one node. D3D12 allows at most 0xffffff
// nodes in a graph (WorkGraphs.md, "Node count limits"); 65536 is far above what an application builds and keeps
// every count within 32 bits.
constexpr UINT kMaxCount = 1u << 16;
// A DispatchGraph with CPU input copies at most this many bytes of records. The spec limits a record to 32 KiB;
// 256 MiB is above any input that a CPU call gives and keeps the copy within the replay ring's 32-bit sizes.
constexpr UINT64 kMaxInputBytes = UINT64{256} << 20;

HRESULT refuse(const char* why, HRESULT hr) noexcept {
    log_refusal("CreateStateObject: work graph: %s (hr %08lx)", why, static_cast<unsigned long>(hr));
    return hr;
}

// The final name of a node, from its properties.
const D3D12DDI_NODE_ID_0108* final_name(const D3D12DDI_NODE_0108* node) noexcept {
    if (!node || node->NodeType != D3D12DDI_NODE_TYPE_SHADER_0108) return nullptr;
    const D3D12DDI_SHADER_NODE_0108& s = node->Shader;
    switch (s.PropertiesType) {
    case D3D12DDI_NODE_PROPERTIES_TYPE_BROADCASTING_LAUNCH_0108:
        return s.pBroadcastingLaunchNodeProperties ? &s.pBroadcastingLaunchNodeProperties->FinalName : nullptr;
    case D3D12DDI_NODE_PROPERTIES_TYPE_COALESCING_LAUNCH_0108:
        return s.pCoalescingLaunchNodeProperties ? &s.pCoalescingLaunchNodeProperties->FinalName : nullptr;
    case D3D12DDI_NODE_PROPERTIES_TYPE_THREAD_LAUNCH_0108:
        return s.pThreadLaunchNodeProperties ? &s.pThreadLaunchNodeProperties->FinalName : nullptr;
    default: return nullptr;
    }
}

// The nodes of a DDI list, at most count + 1 of them: a list longer than its count is refused.
bool walk(const D3D12DDI_NODE_LIST_ENTRY_0108* list, UINT count, std::vector<const D3D12DDI_NODE_0108*>& out) {
    out.clear();
    for (; list; list = list->pNext) {
        if (out.size() == count || !list->pNode) return false;
        out.push_back(list->pNode);
    }
    return out.size() == count;
}

struct Builder {
    WorkGraphTranslation& t;
    LPCWSTR name(LPCWSTR s) { return t.names.emplace_back(s).c_str(); }
    const D3D12_NODE_ID* id(const D3D12DDI_NODE_ID_0108& n) { return &t.ids.emplace_back(D3D12_NODE_ID{name(n.Name), n.ArrayIndex}); }
    const UINT* value(const UINT* p) { return p ? &t.values.emplace_back(*p) : nullptr; }
    const UINT* grid(const UINT* p) { return p ? t.grids.emplace_back(std::array<UINT, 3>{p[0], p[1], p[2]}).data() : nullptr; }
    const BOOL* flag(BOOL b) { return &t.flags.emplace_back(b); }
    HRESULT outputs(UINT count, const D3D12DDI_NODE_OUTPUT_0108* in, UINT* out_count,
                    const D3D12_NODE_OUTPUT_OVERRIDES** out) {
        *out_count = 0;
        *out = nullptr;
        if (count > kMaxCount || (count && !in)) return refuse("output count out of range, or no array", E_INVALIDARG);
        if (!count) return S_OK;
        auto& v = t.outputs.emplace_back(count);
        for (UINT i = 0; i < count; ++i) {
            const D3D12DDI_NODE_OUTPUT_0108& o = in[i];
            if (!o.FinalName.Name) return refuse("output without a final name", E_INVALIDARG);
            v[i] = {o.OutputIndex, id(o.FinalName), flag(o.bAllowSparseNodes), value(o.pMaxRecords),
                    value(o.pMaxRecordsSharedWithOutputIndex)};
        }
        *out_count = count;
        *out = v.data();
        return S_OK;
    }
};

// The common part of the three property types, as overrides of one layout.
template <class P, class O> HRESULT common(Builder& b, const P& p, O& o) {
    if (!p.FinalName.Name) return refuse("node without a final name", E_INVALIDARG);
    o.pLocalRootArgumentsTableIndex = b.value(p.pLocalRootArgumentsTableIndex);
    o.pProgramEntry = b.flag(p.bProgramEntry);
    o.pNewName = b.id(p.FinalName);
    o.pShareInputOf = nullptr;
    if (p.pShareInputOfNode) {
        const D3D12DDI_NODE_ID_0108* source = final_name(p.pShareInputOfNode);
        if (!source || !source->Name) return refuse("shared input of a node without a final name", E_INVALIDARG);
        o.pShareInputOf = b.id(*source);
    }
    return b.outputs(p.NumOutputs, p.pOutputs, &o.NumOutputOverrides, &o.pOutputOverrides);
}
} // namespace

HRESULT translate_work_graph(const D3D12DDI_WORK_GRAPH_DESC_0108& in, WorkGraphTranslation& t,
                             D3D12_WORK_GRAPH_DESC& out) noexcept try {
    if (!in.ProgramName) return refuse("no program name", E_INVALIDARG);
    if (in.Flags & D3D12DDI_WORK_GRAPH_FLAG_ADD_TO_EXISTING_WORK_GRAPH)
        return refuse("an addition to an existing work graph is not implemented", E_NOTIMPL);
    if (in.Flags) return refuse("unknown work graph flags", E_NOTIMPL);
    if (in.NumNodes > kMaxCount || in.NumEntrypoints > kMaxCount || !in.NumNodes || !in.NumEntrypoints)
        return refuse("node or entry point count out of range", E_INVALIDARG);
    std::vector<const D3D12DDI_NODE_0108*> nodes, entries;
    if (!walk(in.pNodes, in.NumNodes, nodes)) return refuse("node list does not match NumNodes", E_INVALIDARG);
    if (!walk(in.pEntrypoints, in.NumEntrypoints, entries))
        return refuse("entry point list does not match NumEntrypoints", E_INVALIDARG);

    Builder b{t};
    t.program = in.ProgramName;
    t.entrypoints.resize(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const D3D12DDI_NODE_ID_0108* n = final_name(entries[i]);
        if (!n || !n->Name) return refuse("entry point without a final name", E_INVALIDARG);
        t.entrypoints[i] = {b.name(n->Name), n->ArrayIndex};
    }
    // Sized once: the node overrides point into these two vectors.
    size_t broadcasting = 0, coalescing = 0;
    for (const D3D12DDI_NODE_0108* n : nodes) {
        if (n->NodeType != D3D12DDI_NODE_TYPE_SHADER_0108 || !n->Shader.Shader)
            return refuse("node of an unknown type or without a shader", E_INVALIDARG);
        if (n->Shader.PropertiesType == D3D12DDI_NODE_PROPERTIES_TYPE_BROADCASTING_LAUNCH_0108) ++broadcasting;
        else ++coalescing;
    }
    t.broadcasting.resize(broadcasting);
    t.coalescing.resize(coalescing);
    t.nodes.resize(nodes.size());
    size_t nb = 0, nc = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const D3D12DDI_SHADER_NODE_0108& s = nodes[i]->Shader;
        D3D12_NODE& node = t.nodes[i];
        node.NodeType = D3D12_NODE_TYPE_SHADER;
        node.Shader.Shader = b.name(s.Shader);
        HRESULT hr = S_OK;
        switch (s.PropertiesType) {
        case D3D12DDI_NODE_PROPERTIES_TYPE_BROADCASTING_LAUNCH_0108: {
            const auto* p = s.pBroadcastingLaunchNodeProperties;
            if (!p) return refuse("broadcasting node without properties", E_INVALIDARG);
            D3D12_BROADCASTING_LAUNCH_OVERRIDES& o = t.broadcasting[nb++];
            hr = common(b, *p, o);
            o.pDispatchGrid = b.grid(p->pDispatchGrid);
            o.pMaxDispatchGrid = b.grid(p->pMaxDispatchGrid);
            node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_BROADCASTING_LAUNCH;
            node.Shader.pBroadcastingLaunchOverrides = &o;
            break;
        }
        case D3D12DDI_NODE_PROPERTIES_TYPE_COALESCING_LAUNCH_0108: {
            const auto* p = s.pCoalescingLaunchNodeProperties;
            if (!p) return refuse("coalescing node without properties", E_INVALIDARG);
            D3D12_COALESCING_LAUNCH_OVERRIDES& o = t.coalescing[nc++];
            hr = common(b, *p, o);
            node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_COALESCING_LAUNCH;
            node.Shader.pCoalescingLaunchOverrides = &o;
            break;
        }
        case D3D12DDI_NODE_PROPERTIES_TYPE_THREAD_LAUNCH_0108: {
            const auto* p = s.pThreadLaunchNodeProperties;
            if (!p) return refuse("thread node without properties", E_INVALIDARG);
            // D3D12_THREAD_LAUNCH_OVERRIDES has the layout of D3D12_COALESCING_LAUNCH_OVERRIDES (asserted below).
            D3D12_COALESCING_LAUNCH_OVERRIDES& o = t.coalescing[nc++];
            hr = common(b, *p, o);
            node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_THREAD_LAUNCH;
            node.Shader.pThreadLaunchOverrides = reinterpret_cast<const D3D12_THREAD_LAUNCH_OVERRIDES*>(&o);
            break;
        }
        default: return refuse("node of an unknown launch type", E_INVALIDARG);
        }
        if (FAILED(hr)) return hr;
    }
    out = {t.program.c_str(), static_cast<D3D12_WORK_GRAPH_FLAGS>(kWorkGraphFlagDdiOrder),
           static_cast<UINT>(t.entrypoints.size()), t.entrypoints.data(), static_cast<UINT>(t.nodes.size()),
           t.nodes.data()};
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}
static_assert(sizeof(D3D12_THREAD_LAUNCH_OVERRIDES) == sizeof(D3D12_COALESCING_LAUNCH_OVERRIDES) &&
                  offsetof(D3D12_THREAD_LAUNCH_OVERRIDES, pOutputOverrides) ==
                      offsetof(D3D12_COALESCING_LAUNCH_OVERRIDES, pOutputOverrides),
              "thread and coalescing overrides");

// ---- Program registry -----------------------------------------------------------------------------------------------
namespace {
// object: the engine state object, without a reference; the record removes its programs before it releases it.
struct Program {
    DeviceContext* device = nullptr;
    ID3D12StateObject* object = nullptr;
    std::vector<UINT> record_sizes;             // per entry point index
};
using Key = std::array<UINT64, 4>;
std::mutex registry_lock;
std::map<Key, Program> registry;

Key key_of(const D3D12DDI_PROGRAM_IDENTIFIER_0108& id) noexcept {
    return {id.OpaqueData[0], id.OpaqueData[1], id.OpaqueData[2], id.OpaqueData[3]};
}
// With object: a reference to the engine state object, taken under the lock, for the replay entry to release.
bool find(const Key& key, const DeviceContext* device, Program* out, ID3D12StateObject** object = nullptr) {
    std::lock_guard lock(registry_lock);
    const auto it = registry.find(key);
    if (it == registry.end() || it->second.device != device) return false;
    if (out) *out = it->second;
    if (object) {
        *object = it->second.object;
        (*object)->AddRef();
    }
    return true;
}
template <class I> I* engine_as(IUnknown* object) noexcept {
    I* out = nullptr;
    if (!object || FAILED(object->QueryInterface(__uuidof(I), reinterpret_cast<void**>(&out)))) return nullptr;
    return out;
}
} // namespace

WorkGraphPrograms::~WorkGraphPrograms() {
    std::lock_guard lock(registry_lock);
    for (const auto& key : identifiers) registry.erase(key);
}

std::unique_ptr<WorkGraphPrograms> register_work_graphs(DeviceContext* c, ID3D12StateObject* so, HRESULT* hr) noexcept try {
    *hr = E_NOINTERFACE;
    auto* graphs = engine_as<ID3D12WorkGraphProperties>(so);
    auto* properties = engine_as<ID3D12StateObjectProperties1>(so);
    std::unique_ptr<WorkGraphPrograms> out;
    if (graphs && properties) {
        out = std::make_unique<WorkGraphPrograms>();
        *hr = S_OK;
        const UINT count = graphs->GetNumWorkGraphs();
        for (UINT g = 0; g < count && SUCCEEDED(*hr); ++g) {
            const LPCWSTR name = graphs->GetProgramName(g);
            const D3D12_PROGRAM_IDENTIFIER id = name ? properties->GetProgramIdentifier(name) : D3D12_PROGRAM_IDENTIFIER{};
            const Key key{id.OpaqueData[0], id.OpaqueData[1], id.OpaqueData[2], id.OpaqueData[3]};
            if (key == Key{}) {
                *hr = E_UNEXPECTED;
                break;
            }
            Program program;
            program.device = c;
            program.object = so;
            const UINT entries = graphs->GetNumEntrypoints(g);
            program.record_sizes.resize(entries);
            for (UINT e = 0; e < entries; ++e) program.record_sizes[e] = graphs->GetEntrypointRecordSizeInBytes(g, e);
            std::lock_guard lock(registry_lock);
            if (!registry.emplace(key, std::move(program)).second) {
                *hr = E_UNEXPECTED;
                break;
            }
            out->identifiers.push_back(key);
        }
        if (FAILED(*hr)) out.reset();               // the destructor removes what was registered
    }
    if (graphs) graphs->Release();
    if (properties) properties->Release();
    if (*hr == E_NOINTERFACE) log_refusal("CreateStateObject: work graph: the engine object has no work graph properties");
    return out;
} catch (const std::bad_alloc&) {
    *hr = E_OUTOFMEMORY;
    return nullptr;
}

D3D12DDI_PROGRAM_IDENTIFIER_0108 work_graph_identifier(const StateObjectRecord& record, LPCWSTR name) noexcept {
    D3D12DDI_PROGRAM_IDENTIFIER_0108 out{};
    if (!record.work_graph || !name) return out;
    if (auto* properties = engine_as<ID3D12StateObjectProperties1>(record.h.engine)) {
        const D3D12_PROGRAM_IDENTIFIER id = properties->GetProgramIdentifier(name);
        properties->Release();
        std::memcpy(&out, &id, sizeof(out));
    }
    return out;
}

namespace {
void APIENTRY memory_requirements(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR name,
                                  D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108* out) {
    if (!out) return;
    *out = {};
    const auto* r = record_of<StateObjectRecord>(h.pDrvPrivate, Tag::StateObject);
    if (!r || !r->work_graph || !name) {
        log_line("GetWorkGraphMemoryRequirements: not a live work graph state object record");
        return;
    }
    if (auto* graphs = engine_as<ID3D12WorkGraphProperties>(r->h.engine)) {
        const UINT index = graphs->GetWorkGraphIndex(name);
        if (index != UINT_MAX) {
            D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS api{};
            graphs->GetWorkGraphMemoryRequirements(index, &api);
            std::memcpy(out, &api, sizeof(*out));
        }
        graphs->Release();
    }
}

void report(CommandListRecord* l, const char* slot, HRESULT hr) noexcept {
    log_line("%s: hr %08lx", slot, static_cast<unsigned long>(hr));
    l->h.device->report_list(l->rt, hr);
}

// The bytes the records of one CPU input span; false when they are more than kMaxInputBytes.
bool input_bytes(UINT records, UINT64 stride, UINT size, UINT64* out) noexcept {
    *out = 0;
    if (!records || !size) return true;         // empty records have no bytes
    const UINT64 span = UINT64{records - 1} * stride;
    if (stride && span / stride != records - 1) return false;
    *out = span + size;
    return *out <= kMaxInputBytes;
}

// The replay entry holds the inputs with pRecords the offset of their records in the entry's copy of the bytes; the
// call makes each a pointer again (the entry's own memory, written once).
void APIENTRY dispatch_graph(D3D12DDI_HCOMMANDLIST h, const D3D12DDI_DISPATCH_GRAPH_DESC_0108* desc) {
    constexpr const char* kSlot = "DispatchGraph";
    CommandListRecord* l = list_of(h, kSlot);
    if (!l) return;
    if (!desc) return report(l, kSlot, E_INVALIDARG);
    switch (desc->Mode) {
    case D3D12DDI_DISPATCH_MODE_NODE_GPU_INPUT_0108:
    case D3D12DDI_DISPATCH_MODE_MULTI_NODE_GPU_INPUT_0108: {
        D3D12_DISPATCH_GRAPH_DESC api{};
        api.Mode = static_cast<D3D12_DISPATCH_MODE>(desc->Mode);
        api.NodeGPUInput = desc->NodeGPUInput;           // the same member for both modes: an address
        if (auto* l10 = list_as<ID3D12GraphicsCommandList10>(l))
            record(l, [=](ID3D12GraphicsCommandList*) {
                l10->DispatchGraph(&api);
                l10->Release();
            });
        return;
    }
    case D3D12DDI_DISPATCH_MODE_NODE_CPU_INPUT_0108:
    case D3D12DDI_DISPATCH_MODE_MULTI_NODE_CPU_INPUT_0108: break;
    default: return report(l, kSlot, E_INVALIDARG);
    }
    Program program;
    if (!find(Key{l->work_graph[0], l->work_graph[1], l->work_graph[2], l->work_graph[3]}, l->h.device, &program))
        return report(l, kSlot, E_INVALIDARG);          // no SetProgram of a live work graph on this list
    const bool multi = desc->Mode == D3D12DDI_DISPATCH_MODE_MULTI_NODE_CPU_INPUT_0108;
    const UINT count = multi ? desc->MultiNodeCPUInput.NumNodeInputs : 1;
    if (multi && count && !desc->MultiNodeCPUInput.pNodeInputs) return report(l, kSlot, E_INVALIDARG);
    const auto input = [desc, multi](UINT i) -> const D3D12DDI_NODE_CPU_INPUT_0108& {
        if (!multi) return desc->NodeCPUInput;
        return *reinterpret_cast<const D3D12DDI_NODE_CPU_INPUT_0108*>(
            reinterpret_cast<const uint8_t*>(desc->MultiNodeCPUInput.pNodeInputs) +
            i * desc->MultiNodeCPUInput.NodeInputStrideInBytes);
    };
    try {
        std::vector<D3D12_NODE_CPU_INPUT> inputs(count);
        UINT64 total = 0;
        for (UINT i = 0; i < count; ++i) {
            const D3D12DDI_NODE_CPU_INPUT_0108& in = input(i);
            if (in.EntrypointIndex >= program.record_sizes.size() || (in.NumRecords && !in.pRecords &&
                                                                     program.record_sizes[in.EntrypointIndex]))
                return report(l, kSlot, E_INVALIDARG);
            UINT64 bytes = 0;
            if (!input_bytes(in.NumRecords, in.RecordStrideInBytes, program.record_sizes[in.EntrypointIndex], &bytes) ||
                total + bytes > kMaxInputBytes)
                return report(l, kSlot, E_OUTOFMEMORY);
            inputs[i] = {in.EntrypointIndex, in.NumRecords, reinterpret_cast<void*>(static_cast<uintptr_t>(total)),
                         in.RecordStrideInBytes};
            total += (bytes + 15) & ~UINT64{15};
        }
        // At least one block, so that the entry's copy is never null: every pRecords becomes a pointer into it.
        std::vector<uint8_t> records(static_cast<size_t>(total ? total : 16));
        for (UINT i = 0; i < count; ++i) {
            const D3D12DDI_NODE_CPU_INPUT_0108& in = input(i);
            UINT64 bytes = 0;
            input_bytes(in.NumRecords, in.RecordStrideInBytes, program.record_sizes[in.EntrypointIndex], &bytes);
            if (bytes && in.pRecords) std::memcpy(records.data() + reinterpret_cast<uintptr_t>(inputs[i].pRecords), in.pRecords,
                                   static_cast<size_t>(bytes));
        }
        auto* l10 = list_as<ID3D12GraphicsCommandList10>(l);
        if (!l10) return;
        record(l, [=](ID3D12GraphicsCommandList*, const D3D12_NODE_CPU_INPUT* copied, const uint8_t* bytes) {
            auto* fixed = const_cast<D3D12_NODE_CPU_INPUT*>(copied);
            for (UINT i = 0; i < count; ++i)
                fixed[i].pRecords = const_cast<uint8_t*>(bytes) + reinterpret_cast<uintptr_t>(fixed[i].pRecords);
            D3D12_DISPATCH_GRAPH_DESC api{};
            if (multi) {
                api.Mode = D3D12_DISPATCH_MODE_MULTI_NODE_CPU_INPUT;
                api.MultiNodeCPUInput = {count, fixed, sizeof(D3D12_NODE_CPU_INPUT)};
            } else {
                api.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
                api.NodeCPUInput = fixed[0];
            }
            l10->DispatchGraph(&api);
            l10->Release();
        }, in(inputs.data(), inputs.size()), in(records.data(), records.size()));
    } catch (const std::bad_alloc&) {
        report(l, kSlot, E_OUTOFMEMORY);
    }
}
} // namespace

void work_graph_set_program(CommandListRecord* l, const D3D12DDI_SET_WORK_GRAPH_DESC_0108& desc) noexcept {
    constexpr const char* kSlot = "SetProgram";
    const Key key = key_of(desc.ProgramIdentifier);
    if (desc.Flags & ~D3D12DDI_SET_WORK_GRAPH_FLAG_INITIALIZE_0108) return report(l, kSlot, E_INVALIDARG);
    // The pending replay owns a reference to the engine state object, as the generic program's SetProgram does.
    ID3D12StateObject* object = nullptr;
    if (!find(key, l->h.device, nullptr, &object)) return report(l, kSlot, E_INVALIDARG);
    std::memcpy(l->work_graph, key.data(), sizeof(l->work_graph));
    D3D12_SET_PROGRAM_DESC api{};
    api.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    std::memcpy(&api.WorkGraph.ProgramIdentifier, &desc.ProgramIdentifier, sizeof(api.WorkGraph.ProgramIdentifier));
    api.WorkGraph.Flags = static_cast<D3D12_SET_WORK_GRAPH_FLAGS>(desc.Flags);
    api.WorkGraph.BackingMemory = {desc.BackingMemory.StartAddress, desc.BackingMemory.SizeInBytes};
    api.WorkGraph.NodeLocalRootArgumentsTable = {desc.NodeLocalRootArgumentsTable.StartAddress,
                                                 desc.NodeLocalRootArgumentsTable.SizeInBytes,
                                                 desc.NodeLocalRootArgumentsTable.StrideInBytes};
    auto* l10 = list_as<ID3D12GraphicsCommandList10>(l);
    if (!l10) {
        object->Release();
        return;
    }
    record(l, [=](ID3D12GraphicsCommandList*) {
        l10->SetProgram(&api);
        l10->Release();
        object->Release();
    });
}

void fill_work_graph_entries(native12::ModernCoreEntries* core, native12::ModernListEntries* lists) noexcept {
    if (core) core->pfnGetWorkGraphMemoryRequirements = memory_requirements;
    if (lists) lists->pfnDispatchGraph = dispatch_graph;
}

} // namespace engine_ddi
#endif
