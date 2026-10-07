// SPDX-License-Identifier: MIT
// engine-ddi: ray tracing state objects (D105-D107), the properties of one (D110-D113) and AddToStateObject (D115,
// D116), on the engine's ID3D12Device5, ID3D12Device7 and ID3D12StateObjectProperties. SetPipelineState1 and
// DispatchRays (L63, L64) are in commands.cpp.
//
// The DDI description is not the API's (H = d3d12umddi.h, A = d3d12.h, 10.0.26100; DirectX-Specs Raytracing.md,
// "State object DDIs"): subobjects name root signatures by handle, a DXIL library comes without its size, a hit group
// carries SummaryFlags, there are no association subobjects, and the runtime adds a SHADER_EXPORT_SUMMARY that lists
// for every shader export the subobjects associated with it. Documented (DirectX-Specs 5a4139be,
// Raytracing.md:9490-9498): subobjects defined in DXIL libraries arrive as plain DDI subobjects and are gone from the
// library the driver gets, and every association, defaults included, arrives as an explicit list per export.
// CreateStateObject rebuilds the API description the engine parses (libs/vkd3d/raytracing_pipeline.c,
// d3d12_state_object_parse_subobject):
//   STATE_OBJECT_CONFIG, NODE_MASK, RAYTRACING_SHADER_CONFIG  copied;
//   GLOBAL_ and LOCAL_ROOT_SIGNATURE  the engine root signature of the handle, a live one of this device;
//   DXIL_LIBRARY  a container rebuilt by shader-container BuildLibraryContainer; the export array in place;
//   RAYTRACING_PIPELINE_CONFIG  read as _0075 and passed as the API's PIPELINE_CONFIG1 (type 12);
//   HIT_GROUP  copied field by field, without SummaryFlags;
//   SHADER_EXPORT_SUMMARY  not passed on: one SUBOBJECT_TO_EXPORTS_ASSOCIATION per associated root signature or
//                          configuration, naming every export the summary gives it by the name resolve_exports
//                          chooses (a listed name, else a unique unmangled one, else a unique mangled one);
//   EXISTING_COLLECTION  the engine object of a live COLLECTION of this device, NumExports 0 only (an export list is
//                        E_NOTIMPL for now, translate); any other type is E_INVALIDARG, never passed through.
// Every refusal of either slot is reported to the runtime as E_OUTOFMEMORY (admitted_create_failure, create_record),
// with the real code on a log_refusal line: a create DDI that reports another code costs the application its device.
// INFERENCE until a lab run logs a real description (one line per create, below): that a 0092 driver gets
// RAYTRACING_PIPELINE_CONFIG as _0075 (H:7731-7732 names both layouts); that pDXILLibrary is a DXIL part, as for
// shaders, or else a whole container. pDXILLibrary has no size (H:7820-7825): the length used is the length the
// payload claims, checked for internal consistency (shader-container.h, LibraryPayloadDwords).
// INHERITED, measured (fact M837: tools/win/d3d12ddicap on WARP, D3D12Core.dll 10.0.26100.9278 of the development
// PC; that unit A's 10.0.22621.5415 does the same is an INFERENCE): the summary's subobject pointers point into pSubobjects, except for the exports of an imported
// collection. Those the importer's summary lists with the associations the collection resolved, as pointers into
// that collection's own DDI array; and the runtime leaves out of the importer's array every declared root signature
// and shader configuration that no export of its own takes (a link of collections arrives as the collections and its
// pipeline configuration only). The engine has those associations already, in the collection's engine object, so
// they become no API subobject here (count_associations). A pointer in no array of this create or of what it
// imports is matched by type and pDesc, and else refused.
//
// The engine also takes every declared root signature and configuration as a default for all exports (engine
// 66c98e72: raytracing_pipeline.c:992-1030, parsed at priority DECLARED_STATE_OBJECT 4 by :1316-1326; priorities
// vkd3d_private.h:6502-6508); the synthesized associations are explicit (priority EXPLICIT 6, :1251-1259) and win for
// each export the summary names. An export the summary associates with no local root signature would get a declared
// one in the engine (state_object_common.c:111-137 takes the highest priority match), though the runtime resolved
// its absence (Raytracing.md:9494-9498). So a RAYTRACING_PIPELINE that declares a local root signature and has such
// an export gets two more API subobjects: the context's empty local root signature (a real engine object, the engine
// dereferences it) and a SUBOBJECT_TO_EXPORTS_ASSOCIATION of it with no exports, an explicit default (priority
// EXPLICIT_DEFAULT 5, raytracing_pipeline.c:1257-1259). It outranks every declared default and yields to every
// explicit association, including a hit group's for its shaders: the engine takes a hit group's association only
// when its priority is higher than the shader's own (state_object_common.c:176-201). Explicit empty associations per
// export (priority 6) would tie with a hit group's there and keep the empty one, so the default is the form used.
// A COLLECTION is left as it is: there an absent association may be an unresolved dependency, not an absence.
//
// Lifetime: the runtime owns the DDI description and its bytecode for the life of the state object
// (Raytracing.md:9555), so names and export arrays are used in place. What engine-ddi rebuilds (the API subobjects,
// the association arrays, the library containers) is its own allocation and must stay put until the engine's
// CreateStateObject returns; the engine deep-copies what it keeps (raytracing_pipeline.c,
// d3d12_state_object_pipeline_data_defer). Keeping it until DestroyStateObject is a conservative choice, not an engine
// requirement.
// An imported collection and a grown parent are different: the runtime keeps a collection's DDI object alive while an
// importer lives (Raytracing.md:9661) but destroys a parent while its children live (:9667-9669). So a record never
// points into another record: it holds its own engine references to what it imported or grew from, released at its
// destroy, and its own copy of the names each exposed (StateObjectTranslation::held and exposed). The runtime's DDI
// arrays it inherited are a different matter: the runtime keeps them alive for it (:9661, and :9667 for a parent's
// creation description), so a record keeps their addresses (StateObjectTranslation::described).
#include "shader-container/shader-container.h"      // first: it selects the D3D12 tokenized program format header
#include "internal.h"
#include "replay.h"
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine_ddi {

namespace sc = shader_container;

// ---- Layouts and values -------------------------------------------------------------------------------------------
#define ENGINE_DDI_SAME(D, dm, A, am) static_assert(offsetof(D, dm) == offsetof(A, am), #D "::" #dm)
// H:7813-7818, A:14244-14249: a library's export array reaches the engine in place.
static_assert(sizeof(D3D12DDI_EXPORT_DESC_0054) == sizeof(D3D12_EXPORT_DESC) &&
                  alignof(D3D12DDI_EXPORT_DESC_0054) == alignof(D3D12_EXPORT_DESC),
              "export description");
static_assert(std::is_same_v<decltype(D3D12DDI_EXPORT_DESC_0054::Name), decltype(D3D12_EXPORT_DESC::Name)> &&
                  std::is_same_v<decltype(D3D12DDI_EXPORT_DESC_0054::ExportToRename),
                                 decltype(D3D12_EXPORT_DESC::ExportToRename)>,
              "export names");
ENGINE_DDI_SAME(D3D12DDI_EXPORT_DESC_0054, Name, D3D12_EXPORT_DESC, Name);
ENGINE_DDI_SAME(D3D12DDI_EXPORT_DESC_0054, ExportToRename, D3D12_EXPORT_DESC, ExportToRename);
ENGINE_DDI_SAME(D3D12DDI_EXPORT_DESC_0054, Flags, D3D12_EXPORT_DESC, Flags);
#undef ENGINE_DDI_SAME
static_assert(D3D12DDI_EXPORT_FLAG_NONE == static_cast<int>(D3D12_EXPORT_FLAG_NONE), "export flags");
// Subobject types H:7719 A:14147, state object types H:7900 A:14438, flags H:7764 A:14188, hit group types H:7859
// A:14280, pipeline flags H:7886 A:14307.
static_assert(D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG ==
                      static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE ==
                      static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE ==
                      static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK == static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY == static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION ==
                      static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG ==
                      static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG) &&
                  D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP == static_cast<int>(D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP),
              "subobject types passed with their DDI number");
static_assert(D3D12DDI_STATE_OBJECT_TYPE_COLLECTION == static_cast<int>(D3D12_STATE_OBJECT_TYPE_COLLECTION) &&
                  D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ==
                      static_cast<int>(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE),
              "state object types");
static_assert(D3D12DDI_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS ==
                      static_cast<int>(D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS) &&
                  D3D12DDI_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS ==
                      static_cast<int>(D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS) &&
                  D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS ==
                      static_cast<int>(D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS),
              "state object flags");
static_assert(D3D12DDI_HIT_GROUP_TYPE_TRIANGLES == static_cast<int>(D3D12_HIT_GROUP_TYPE_TRIANGLES) &&
                  D3D12DDI_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE == static_cast<int>(D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE),
              "hit group types");
static_assert(D3D12DDI_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES ==
                      static_cast<int>(D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES) &&
                  D3D12DDI_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES ==
                      static_cast<int>(D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES),
              "pipeline flags");

// The API description of one state object, as the engine reads it. Every vector is sized once, before any pointer
// into it is taken.
struct StateObjectTranslation {
    union Desc {
        D3D12_STATE_OBJECT_CONFIG config;
        D3D12_GLOBAL_ROOT_SIGNATURE global;
        D3D12_LOCAL_ROOT_SIGNATURE local;
        D3D12_NODE_MASK node_mask;
        D3D12_DXIL_LIBRARY_DESC library;
        D3D12_RAYTRACING_SHADER_CONFIG shader_config;
        D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline_config;
        D3D12_HIT_GROUP_DESC hit_group;
        D3D12_EXISTING_COLLECTION_DESC collection;
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association;
    };
    std::vector<Desc> descs;                        // one per API subobject
    std::vector<D3D12_STATE_SUBOBJECT> subobjects;  // the DDI's in order, less the summaries, then the associations
    std::vector<LPCWSTR> names;                     // the exports of every association, association after association
    std::vector<sc::Container> libraries;           // the rebuilt DXIL libraries
    // One engine reference to each imported collection and to the parent of a grown state object, released after the
    // engine's own object (DestroyStateObject releases that first).
    std::vector<IUnknown*> held;
    // The names this state object's shader exports are known to the engine by: the name resolve_exports chose for
    // each summary export, every hit group's, and what an imported collection or the parent exposed. engine-ddi's own
    // copies, read by a later import or growth (never a pointer into runtime memory or another record).
    std::vector<std::wstring> exposed;
    // The DDI subobject arrays the runtime described this state object with: its own create's, then those of every
    // collection it imported and of the parent it grew from, transitively. The runtime keeps each alive while this
    // state object lives (its own: Raytracing.md:9555; an imported collection's: :9661; a parent's creation
    // description, though not the parent's DDI object: :9667). An importer's summary points into them (measured, the
    // INHERITED note at the top of this file). A later importer or child reads them during its create
    // (subobject_index), within those lifetimes.
    struct DdiRange {
        const D3D12DDI_STATE_SUBOBJECT_0054* base;
        UINT count;
    };
    std::vector<DdiRange> described;

    StateObjectTranslation() = default;
    StateObjectTranslation(const StateObjectTranslation&) = delete;
    StateObjectTranslation& operator=(const StateObjectTranslation&) = delete;
    ~StateObjectTranslation() {
        for (IUnknown* object : held) object->Release();
    }
};

namespace {
// Bounds on what a description counts: subobjects, library exports, summary exports (all summaries together),
// associations of one export.
// D3D12 states none; 65536 is far above what a game's state object holds (tens to a few thousand subobjects,
// INFERENCE) and keeps every index within 32 bits.
constexpr UINT kMaxCount = 1u << 16;
// Bound on the export names of all synthesized associations together (8 MiB of pointers): an export has a handful of
// associations (a global and a local root signature, two configurations), so this is 16 per export at kMaxCount.
constexpr size_t kMaxAssociatedNames = size_t{kMaxCount} * 16;
// A DXIL library claiming more than 64 MiB is refused: far above the libraries games ship (INFERENCE), and well
// within the 32-bit sizes of the rebuilt container.
constexpr size_t kMaxLibraryDwords = size_t{16} << 20;
constexpr UINT kStateObjectFlags = D3D12DDI_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
                                   D3D12DDI_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS |
                                   D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
constexpr UINT kPipelineFlags = D3D12DDI_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES |
                                D3D12DDI_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
std::atomic<StateObjectObserver> state_object_observer{};
std::atomic<void*> state_object_observer_user{};
#endif

// The description line of one create, in a fixed buffer: what does not fit is cut off.
struct Text {
    char buffer[480] = "";
    size_t used = 0;
    void add(const char* format, ...) noexcept {
        if (used + 1 >= sizeof(buffer)) return;
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(buffer + used, sizeof(buffer) - used, format, args);
        va_end(args);
        if (written > 0) used = (used + static_cast<size_t>(written) < sizeof(buffer)) ? used + written : sizeof(buffer) - 1;
    }
};

// A refusal goes to the debugger whatever the trace switches say (log_refusal): the create that reports it fails, and
// the reason must be on record without a trace build (The Ascent, lab trial 465).
HRESULT refuse(UINT index, const char* why, HRESULT hr) noexcept {
    log_refusal("CreateStateObject: subobject %u: %s (hr %08lx)", index, why, static_cast<unsigned long>(hr));
    return hr;
}

using DdiRange = StateObjectTranslation::DdiRange;

// The index of p in the array r by address; UINT_MAX when p is not one of its elements.
UINT index_in(const DdiRange& r, const D3D12DDI_STATE_SUBOBJECT_0054* p) noexcept {
    constexpr uintptr_t size = sizeof(D3D12DDI_STATE_SUBOBJECT_0054);
    const auto base = reinterpret_cast<uintptr_t>(r.base);
    const auto at = reinterpret_cast<uintptr_t>(p);
    if (r.base && at >= base && (at - base) % size == 0 && (at - base) / size < r.count)
        return static_cast<UINT>((at - base) / size);
    return UINT_MAX;
}

// What subobject_index answers for a subobject an imported collection (or the parent) declared.
constexpr UINT kInherited = UINT_MAX - 1;       // above every index: kMaxCount bounds NumSubobjects

// The index in pSubobjects of a subobject the summary associates: by address, or else the subobject of the same
// type and description. kInherited for a subobject of an array in inherited (the importer's summary names the
// associations an imported collection resolved, in that collection's own description: measured, top of this file).
// UINT_MAX when there is none. *found is the subobject, of this array or an inherited one, when there is one.
UINT subobject_index(const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a, const std::vector<DdiRange>& inherited,
                     const D3D12DDI_STATE_SUBOBJECT_0054* p, const D3D12DDI_STATE_SUBOBJECT_0054** found) noexcept {
    *found = nullptr;
    if (!p) return UINT_MAX;
    UINT i = index_in({a.pSubobjects, a.NumSubobjects}, p);
    if (i != UINT_MAX) {
        *found = &a.pSubobjects[i];
        return i;
    }
    for (const DdiRange& r : inherited)
        if (index_in(r, p) != UINT_MAX) {
            *found = p;
            return kInherited;
        }
    for (i = 0; i < a.NumSubobjects; ++i)
        if (a.pSubobjects[i].Type == p->Type && a.pSubobjects[i].pDesc == p->pDesc) {
            *found = &a.pSubobjects[i];
            return i;
        }
    for (const DdiRange& r : inherited)
        for (UINT k = 0; k < r.count; ++k)
            if (r.base[k].Type == p->Type && r.base[k].pDesc == p->pDesc) {
                *found = &r.base[k];
                return kInherited;
            }
    return UINT_MAX;
}

// What an association to a subobject of this type becomes: 1 an API association, 0 nothing (the configuration and
// the node mask hold for the whole state object, and the engine reads them as declared), -1 a broken summary.
int association_kind(D3D12DDI_STATE_SUBOBJECT_TYPE type) noexcept {
    switch (type) {
    case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
    case D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: return 1;
    case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
    case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK: return 0;
    default: return -1;
    }
}

ID3D12RootSignature* root_signature(DeviceContext* c, D3D12DDI_HROOTSIGNATURE h) noexcept {
    const auto* r = record_of<RootSignatureRecord>(h.pDrvPrivate, Tag::RootSignature, c);
    return r ? static_cast<ID3D12RootSignature*>(r->h.engine) : nullptr;
}

// The live COLLECTION of this device an EXISTING_COLLECTION subobject names, or null. The runtime keeps an imported
// collection's DDI object alive for the whole create (Raytracing.md:9657-9661).
const StateObjectRecord* collection_of(DeviceContext* c, const void* desc) noexcept {
    const auto& in = *static_cast<const D3D12DDI_EXISTING_COLLECTION_DESC_0054*>(desc);
    const auto* r = record_of<StateObjectRecord>(in.hExistingCollection.pDrvPrivate, Tag::StateObject, c);
    return (r && r->properties && r->h.engine && r->translation && !r->executable) ? r : nullptr;
}

// The context's empty local root signature (no parameters, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE), created
// on first use. A real engine object: the engine dereferences a declared root signature (raytracing_pipeline.c:992-1030).
HRESULT empty_local_root_signature(DeviceContext* c, ID3D12RootSignature** out) noexcept {
    AcquireSRWLockExclusive(&c->empty_local_lock);
    HRESULT hr = S_OK;
    if (!c->empty_local) {
        const D3D12DDI_ROOT_SIGNATURE_0013 empty{0, nullptr, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
        std::vector<uint8_t> blob;
        hr = serialize_root_signature(&empty, blob);
        if (SUCCEEDED(hr))
            hr = c->device->CreateRootSignature(0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature),
                                                reinterpret_cast<void**>(&c->empty_local));
        if (SUCCEEDED(hr) && !c->empty_local) hr = E_UNEXPECTED;
        if (FAILED(hr)) c->empty_local = nullptr;
    }
    *out = c->empty_local;
    ReleaseSRWLockExclusive(&c->empty_local_lock);
    if (FAILED(hr)) log_refusal("CreateStateObject: no empty local root signature (hr %08lx)", static_cast<unsigned long>(hr));
    return hr;
}

// Pass 1 over one summary: every export named, every association within the description or an inherited one and of
// a type that takes one. counts[i] grows by the exports associated with subobject i, total by all of them, unlocal by
// the exports associated with no local root signature. An inherited association is counted nowhere: the engine has it
// already, in the imported collection's engine object (raytracing_pipeline.c, d3d12_state_object_add_collection: the
// library path inherits the collection's configurations, the deferred path copies its associations, explicit ones
// staying explicit). An inherited local root signature still makes its export local.
HRESULT count_associations(const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a, const std::vector<DdiRange>& inherited,
                           UINT index, std::vector<size_t>& counts, size_t& total, size_t& unlocal) noexcept {
    const auto& f = *static_cast<const D3D12DDI_FUNCTION_SUMMARY_0054*>(a.pSubobjects[index].pDesc);
    if (f.NumExportedFunctions > kMaxCount || (f.NumExportedFunctions && !f.pSummaries))
        return refuse(index, "summary export count out of range, or no array", E_INVALIDARG);
    for (UINT e = 0; e < f.NumExportedFunctions; ++e) {
        const D3D12DDI_FUNCTION_SUMMARY_NODE_0054& node = f.pSummaries[e];
        if (!node.ExportNameUnmangled && !node.ExportNameMangled)
            return refuse(index, "summary export without a name", E_INVALIDARG);
        if (node.NumAssociatedSubobjects > kMaxCount || (node.NumAssociatedSubobjects && !node.ppAssociatedSubobjects))
            return refuse(index, "summary association count out of range, or no array", E_INVALIDARG);
        bool local = false;
        for (UINT k = 0; k < node.NumAssociatedSubobjects; ++k) {
            const D3D12DDI_STATE_SUBOBJECT_0054* found = nullptr;
            const UINT target = subobject_index(a, inherited, node.ppAssociatedSubobjects[k], &found);
            if (target == UINT_MAX)
                return refuse(index, "summary association outside the description and every imported one", E_INVALIDARG);
            const int kind = association_kind(found->Type);
            if (kind < 0) return refuse(index, "summary association with a subobject that takes none", E_INVALIDARG);
            if (!kind) continue;
            local = local || found->Type == D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
            if (target == kInherited) continue;
            if (total == kMaxAssociatedNames) return refuse(index, "more associations than the bound", E_INVALIDARG);
            ++counts[target];
            ++total;
        }
        if (!local) ++unlocal;
    }
    return S_OK;
}

// How the association name of a summary export was chosen (resolve_exports).
enum Identity : size_t { kAlias, kMangled, kUnmangled, kIdentities };

// The name each summary export goes by in the synthesized associations, one per export in summary order, into
// identity. The engine matches an association name against an export's plain and its mangled name
// (libs/vkd3d/state_object_common.c, vkd3d_export_equal); an export listed by a library is known to it by that
// listed name alone, with no mangled name (libs/vkd3d-shader/dxil.c, the NumExports branch). The summary gives both
// names (H:7843-7850, Raytracing.md:9611-9619) and promises neither unique: overloads share an unmangled name, and one
// function may be exported several times under renames (Raytracing.md:3497, D3D12_EXPORT_DESC). So, per export:
//   1. its unmangled or mangled name when a library lists it (an exposed alias, kept as listed) or a hit group
//      has it as its name;
//   2. else its unmangled name, when no other export carries that name;
//   3. else its mangled name, when no other export carries that one;
// and E_INVALIDARG before any engine call otherwise, or when no library exposes every export and the export is not
// listed, or when two exports come to one name: a name shared by two exports would associate both, with the other's
// subobjects. An export with one name only goes by it, under the same uniqueness. The listed names of every library
// count, whatever the other libraries do: a library that exposes every export beside one that lists a mangled name
// leaves that name the only one the engine knows the listed export by (dxil.c:2367-2407).
// An imported collection (all of it: pass 1 admits no import list) is a listing source too: the names it exposed at
// its own create (StateObjectTranslation::exposed), which the engine copies into the importer
// (raytracing_pipeline.c:2330-2349), never names reconstructed from the summary. parent is the exposed names of the
// state object an addition grows from: an export of the addition, or a hit group, that carries one of them is
// E_INVALIDARG (Raytracing.md:3785-3786, exports must not collide), before any engine call; the engine checks none.
HRESULT resolve_exports(DeviceContext* c, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a,
                        const std::vector<std::wstring>* parent, std::vector<LPCWSTR>& identity,
                        size_t (&kinds)[kIdentities]) {
    std::unordered_set<std::wstring_view> aliases, inherited;
    if (parent)
        for (const std::wstring& name : *parent) inherited.insert(name);
    bool export_all = false;                    // some library exposes every export (NumExports 0)
    for (UINT i = 0; i < a.NumSubobjects; ++i) {
        if (a.pSubobjects[i].Type == D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
            // A hit group is an export by its own name, the one the engine matches it by (state_object_common.c:
            // 190-191 looks its associations up by HitGroupExport).
            const auto& group = *static_cast<const D3D12DDI_HIT_GROUP_DESC_0054*>(a.pSubobjects[i].pDesc);
            if (group.HitGroupExport && inherited.count(group.HitGroupExport)) {
                log_refusal("CreateStateObject: subobject %u: a hit group named like an export of the parent", i);
                return E_INVALIDARG;
            }
            if (group.HitGroupExport) aliases.insert(group.HitGroupExport);
            continue;
        }
        if (a.pSubobjects[i].Type == D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            if (const StateObjectRecord* collection = collection_of(c, a.pSubobjects[i].pDesc))
                for (const std::wstring& name : collection->translation->exposed) aliases.insert(name);
            continue;
        }
        if (a.pSubobjects[i].Type != D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) continue;
        const auto& library = *static_cast<const D3D12DDI_DXIL_LIBRARY_DESC_0054*>(a.pSubobjects[i].pDesc);
        if (!library.NumExports) {
            export_all = true;
            continue;
        }
        if (library.NumExports > kMaxCount || !library.pExports) continue;  // translate_one refuses these
        for (UINT e = 0; e < library.NumExports; ++e)
            if (library.pExports[e].Name) aliases.insert(library.pExports[e].Name);
    }
    std::vector<const D3D12DDI_FUNCTION_SUMMARY_NODE_0054*> nodes;
    std::unordered_map<std::wstring_view, size_t> carriers;     // exports carrying each name
    for (UINT i = 0; i < a.NumSubobjects; ++i) {
        if (a.pSubobjects[i].Type != D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY) continue;
        const auto& f = *static_cast<const D3D12DDI_FUNCTION_SUMMARY_0054*>(a.pSubobjects[i].pDesc);
        for (UINT e = 0; e < f.NumExportedFunctions; ++e) {
            const D3D12DDI_FUNCTION_SUMMARY_NODE_0054& node = f.pSummaries[e];
            nodes.push_back(&node);
            if (node.ExportNameUnmangled) ++carriers[node.ExportNameUnmangled];
            if (node.ExportNameMangled && (!node.ExportNameUnmangled ||
                                           std::wstring_view(node.ExportNameMangled) != node.ExportNameUnmangled))
                ++carriers[node.ExportNameMangled];
        }
    }
    identity.assign(nodes.size(), nullptr);
    std::unordered_set<std::wstring_view> taken;
    for (size_t k = 0; k < nodes.size(); ++k) {
        const LPCWSTR plain = nodes[k]->ExportNameUnmangled, mangled = nodes[k]->ExportNameMangled;
        const bool plain_listed = plain && aliases.count(plain), mangled_listed = mangled && aliases.count(mangled);
        LPCWSTR chosen = nullptr;
        Identity kind = kAlias;
        if (plain_listed && mangled_listed && std::wstring_view(plain) != mangled) {
            chosen = nullptr;                   // two listed exports in one: ambiguous
        } else if (plain_listed || mangled_listed) {
            chosen = plain_listed ? plain : mangled;
        } else if (!export_all) {
            log_refusal("CreateStateObject: summary export %zu: not among the exports the libraries list", k);
            return E_INVALIDARG;
        } else if (plain && carriers[plain] == 1) {
            chosen = plain;
            kind = kUnmangled;
        } else if (mangled && carriers[mangled] == 1) {
            chosen = mangled;
            kind = kMangled;
        }
        if (!chosen || !taken.insert(chosen).second) {
            log_refusal("CreateStateObject: summary export %zu: its names are shared with another export", k);
            return E_INVALIDARG;
        }
        if ((plain && inherited.count(plain)) || (mangled && inherited.count(mangled))) {
            log_refusal("CreateStateObject: summary export %zu: named like an export of the parent", k);
            return E_INVALIDARG;
        }
        identity[k] = chosen;
        ++kinds[kind];
    }
    return S_OK;
}

// Pass 2 over one summary: the export names, into each association's slice of names (cursor[i] is the next free
// entry of subobject i's slice). identity holds the name of every summary export, from export *next on for this
// summary. Pass 1 has validated everything read here.
void fill_associations(const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a, const std::vector<DdiRange>& inherited,
                       UINT index, std::vector<size_t>& cursor, const std::vector<LPCWSTR>& identity, size_t* next,
                       std::vector<LPCWSTR>& names) noexcept {
    const auto& f = *static_cast<const D3D12DDI_FUNCTION_SUMMARY_0054*>(a.pSubobjects[index].pDesc);
    for (UINT e = 0; e < f.NumExportedFunctions; ++e) {
        const D3D12DDI_FUNCTION_SUMMARY_NODE_0054& node = f.pSummaries[e];
        const LPCWSTR name = identity[(*next)++];
        for (UINT k = 0; k < node.NumAssociatedSubobjects; ++k) {
            const D3D12DDI_STATE_SUBOBJECT_0054* found = nullptr;
            const UINT target = subobject_index(a, inherited, node.ppAssociatedSubobjects[k], &found);
            if (target != kInherited && association_kind(found->Type) > 0) names[cursor[target]++] = name;
        }
    }
}

// One subobject other than the summary, into d; type is its API type. library receives a DXIL library's container
// and is null for every other type.
HRESULT translate_one(DeviceContext* c, const D3D12DDI_STATE_SUBOBJECT_0054& s, UINT index,
                      StateObjectTranslation::Desc& d, D3D12_STATE_SUBOBJECT_TYPE& type, sc::Container* library,
                      Text& text) {
    type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(s.Type);
    switch (s.Type) {
    case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
        const auto& in = *static_cast<const D3D12DDI_STATE_OBJECT_CONFIG_0054*>(s.pDesc);
        if (static_cast<UINT>(in.Flags) & ~kStateObjectFlags) return refuse(index, "unknown state object flags", E_INVALIDARG);
        d.config.Flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(in.Flags);
        return S_OK;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        d.global.pGlobalRootSignature =
            root_signature(c, static_cast<const D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054*>(s.pDesc)->hGlobalRootSignature);
        return d.global.pGlobalRootSignature ? S_OK
                                             : refuse(index, "not a root signature of this device", E_INVALIDARG);
    case D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
        d.local.pLocalRootSignature =
            root_signature(c, static_cast<const D3D12DDI_LOCAL_ROOT_SIGNATURE_0054*>(s.pDesc)->hLocalRootSignature);
        return d.local.pLocalRootSignature ? S_OK : refuse(index, "not a root signature of this device", E_INVALIDARG);
    case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK: {
        const UINT mask = static_cast<const D3D12DDI_NODE_MASK_0054*>(s.pDesc)->NodeMask;
        if (mask > 1) return refuse(index, "node mask of another node", E_INVALIDARG);
        d.node_mask.NodeMask = mask;
        return S_OK;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
        const auto& in = *static_cast<const D3D12DDI_DXIL_LIBRARY_DESC_0054*>(s.pDesc);
        if (!library) return refuse(index, "no container slot", E_UNEXPECTED);
        if (!in.pDXILLibrary) return refuse(index, "no DXIL library", E_INVALIDARG);
        if (in.NumExports > kMaxCount || (in.NumExports && !in.pExports))
            return refuse(index, "library export count out of range, or no array", E_INVALIDARG);
        for (UINT e = 0; e < in.NumExports; ++e)
            if (!in.pExports[e].Name) return refuse(index, "library export without a name", E_INVALIDARG);
        // No DDI field gives the length (H:7820-7825). The one used is the length the payload claims (SizeInUint32, or
        // the container size), checked for internal consistency: BuildLibraryContainer checks the internal sizes and
        // the bitcode bounds against it. That the payload is readable that far rests on the runtime, as for shaders
        // (pipelines.cpp).
        const size_t dwords = sc::LibraryPayloadDwords(in.pDXILLibrary);
        const bool container = in.pDXILLibrary[0] == 0x43425844u;         // "DXBC"
        text.add("; library %s of %zu DWORDs, %u exports", container ? "container" : "DXIL part", dwords, in.NumExports);
        if (!dwords || dwords > kMaxLibraryDwords) return refuse(index, "DXIL library length out of range", E_INVALIDARG);
        const sc::Result r = sc::BuildLibraryContainer(in.pDXILLibrary, dwords, library);
        if (!r) {
            log_refusal("CreateStateObject: subobject %u: %s", index, r.detail.c_str());
            return refuse(index, "no library container", r.hresult());
        }
        d.library.DXILLibrary = {library->bytes.data(), library->bytes.size()};
        d.library.NumExports = in.NumExports;
        d.library.pExports = in.NumExports ? reinterpret_cast<const D3D12_EXPORT_DESC*>(in.pExports) : nullptr;
        return S_OK;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
        const auto& in = *static_cast<const D3D12DDI_RAYTRACING_SHADER_CONFIG_0054*>(s.pDesc);
        d.shader_config = {in.MaxPayloadSizeInBytes, in.MaxAttributeSizeInBytes};
        return S_OK;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
        // _0075, the layout H:7731-7732 names for new drivers (INFERENCE for 0092); the engine takes it as type 12.
        const auto& in = *static_cast<const D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075*>(s.pDesc);
        text.add("; pipeline config depth %u flags 0x%x", in.MaxTraceRecursionDepth, static_cast<UINT>(in.Flags));
        if (static_cast<UINT>(in.Flags) & ~kPipelineFlags) return refuse(index, "unknown pipeline flags", E_INVALIDARG);
        d.pipeline_config = {in.MaxTraceRecursionDepth, static_cast<D3D12_RAYTRACING_PIPELINE_FLAGS>(in.Flags)};
        type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
        return S_OK;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
        const auto& in = *static_cast<const D3D12DDI_HIT_GROUP_DESC_0054*>(s.pDesc);
        if (!in.HitGroupExport ||
            (in.Type != D3D12DDI_HIT_GROUP_TYPE_TRIANGLES && in.Type != D3D12DDI_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE))
            return refuse(index, "hit group without a name or of an unknown type", E_INVALIDARG);
        d.hit_group = {in.HitGroupExport, static_cast<D3D12_HIT_GROUP_TYPE>(in.Type), in.AnyHitShaderImport,
                       in.ClosestHitShaderImport, in.IntersectionShaderImport};
        return S_OK;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
        // The collection's engine object, every export (pass 1 admits no import list); translate takes the reference.
        const StateObjectRecord* collection = collection_of(c, s.pDesc);
        if (!collection) return refuse(index, "not a live collection of this device", E_INVALIDARG);
        d.collection = {static_cast<ID3D12StateObject*>(collection->h.engine), 0, nullptr};
        text.add("; collection import");
        return S_OK;
    }
    default:
        return refuse(index, "not a translated type", E_UNEXPECTED);      // pass 1 let no other type through
    }
}

// The API description of a, into t, with its engine references and exposed names; parent is the live pipeline an
// addition grows from, read during the call only. Nothing reaches the engine here.
HRESULT translate(DeviceContext* c, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a, const StateObjectRecord* parent,
                  StateObjectTranslation& t, Text& text) noexcept try {
    const UINT n = a.NumSubobjects;
    if (n > kMaxCount || (n && !a.pSubobjects)) {
        log_refusal("CreateStateObject: %u subobjects, array %s: refused", n, a.pSubobjects ? "given" : "null");
        return E_INVALIDARG;
    }
    // The DDI arrays of every imported collection (pass 1 refuses an import of anything else) and of the parent, with
    // what each of them inherited: a summary association into one of them is the imported object's own, already the
    // engine's. t keeps them with its own array (described) for a later importer or child.
    std::vector<DdiRange> inherited;
    for (UINT i = 0; i < n; ++i)
        if (a.pSubobjects[i].Type == D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && a.pSubobjects[i].pDesc)
            if (const StateObjectRecord* collection = collection_of(c, a.pSubobjects[i].pDesc))
                inherited.insert(inherited.end(), collection->translation->described.begin(),
                                 collection->translation->described.end());
    if (parent)
        inherited.insert(inherited.end(), parent->translation->described.begin(), parent->translation->described.end());
    t.described.reserve(1 + inherited.size());
    t.described.push_back({a.pSubobjects, n});
    t.described.insert(t.described.end(), inherited.begin(), inherited.end());
    // Pass 1: types, and what the summaries associate.
    std::vector<size_t> counts(n, 0);
    size_t names = 0, exports = 0, unlocal = 0;
    UINT summaries = 0, libraries = 0, locals = 0, imports = 0;
    text.add("; types");
    for (UINT i = 0; i < n; ++i) {
        const D3D12DDI_STATE_SUBOBJECT_0054& s = a.pSubobjects[i];
        if (i < 24) text.add(" 0x%x", static_cast<UINT>(s.Type));
        else if (i == 24) text.add(" ...");
        if (!s.pDesc) return refuse(i, "no description", E_INVALIDARG);
        switch (s.Type) {
        case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP: break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: ++locals; break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: ++libraries; break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
            if (!collection_of(c, s.pDesc)) return refuse(i, "not a live collection of this device", E_INVALIDARG);
            // A restricted import list (NumExports != 0) is valid input, temporarily unsupported: the pinned engine
            // indexes that list with the wrong loop variable for a deferred collection (raytracing_pipeline.c:714-720,
            // d3d12_state_object_add_collection_deferred). Until an engine with the fix is pinned, only an import of
            // every export reaches it.
            if (static_cast<const D3D12DDI_EXISTING_COLLECTION_DESC_0054*>(s.pDesc)->NumExports)
                return refuse(i, "a restricted import list, temporarily unsupported", E_NOTIMPL);
            ++imports;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY: {
            ++summaries;
            const HRESULT hr = count_associations(a, inherited, i, counts, names, unlocal);
            if (FAILED(hr)) return hr;
            exports += static_cast<const D3D12DDI_FUNCTION_SUMMARY_0054*>(s.pDesc)->NumExportedFunctions;
            if (exports > kMaxCount) return refuse(i, "more summary exports than the bound", E_INVALIDARG);
            break;
        }
        default: return refuse(i, "unknown subobject type", E_INVALIDARG);
        }
    }
    std::vector<LPCWSTR> identity;
    size_t kinds[kIdentities]{};
    const HRESULT hr_names =
        resolve_exports(c, a, parent ? &parent->translation->exposed : nullptr, identity, kinds);
    text.add("; summary %zu exports (by listed name %zu, mangled %zu, unmangled %zu), %zu associated names", exports,
             kinds[kAlias], kinds[kMangled], kinds[kUnmangled], names);
    if (FAILED(hr_names)) return hr_names;
    // The names the new state object exposes: its summary exports' and hit groups', then what its imports and its
    // parent expose, each once.
    std::unordered_set<std::wstring_view> exposed;
    const auto expose = [&](std::wstring_view name) {
        if (exposed.insert(name).second) t.exposed.emplace_back(name);
    };
    for (LPCWSTR name : identity) expose(name);
    for (UINT i = 0; i < n; ++i) {
        const D3D12DDI_STATE_SUBOBJECT_0054& s = a.pSubobjects[i];
        if (s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
            if (const LPCWSTR group = static_cast<const D3D12DDI_HIT_GROUP_DESC_0054*>(s.pDesc)->HitGroupExport)
                expose(group);
        } else if (s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            if (const StateObjectRecord* collection = collection_of(c, s.pDesc))
                for (const std::wstring& name : collection->translation->exposed) expose(name);
        }
    }
    if (parent)
        for (const std::wstring& name : parent->translation->exposed) expose(name);
    t.held.reserve(size_t{imports} + (parent ? 1 : 0));
    size_t targets = 0;                        // counts[i] <= kMaxAssociatedNames, within an association's UINT
    for (size_t k : counts) targets += k ? 1 : 0;
    // An executable pipeline declaring a local root signature, with a summary export associated with none: the empty
    // local root signature and its explicit default association keep the engine's declared default away from it.
    // Not for a collection, whose absent association may be an unresolved dependency still to be met.
    ID3D12RootSignature* empty_local = nullptr;
    if (a.Type == D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE && locals && unlocal) {
        text.add("; %zu exports without a local root signature: empty explicit default", unlocal);
        const HRESULT hr = empty_local_root_signature(c, &empty_local);
        if (FAILED(hr)) return hr;
    }

    // Pass 2: the API subobjects, then one association per associated subobject, then the empty local root signature
    // and its default association.
    const size_t total = size_t{n} - summaries + targets + (empty_local ? 2 : 0);
    t.descs.resize(total);
    t.subobjects.resize(total);
    t.names.resize(names);
    t.libraries.resize(libraries);
    std::vector<size_t> api(n, SIZE_MAX);
    size_t out = 0;
    UINT library = 0;
    for (UINT i = 0; i < n; ++i) {
        const D3D12DDI_STATE_SUBOBJECT_0054& s = a.pSubobjects[i];
        if (s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY) continue;
        StateObjectTranslation::Desc& d = t.descs[out];
        D3D12_STATE_SUBOBJECT_TYPE type{};
        sc::Container* container =
            s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY ? &t.libraries[library++] : nullptr;
        const HRESULT hr = translate_one(c, s, i, d, type, container, text);
        if (FAILED(hr)) return hr;
        if (s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            t.held.push_back(d.collection.pExistingCollection);         // within the reserved capacity
            d.collection.pExistingCollection->AddRef();
        }
        t.subobjects[out] = {type, &d};
        api[i] = out++;
    }
    if (parent) {
        t.held.push_back(parent->h.engine);
        parent->h.engine->AddRef();
    }
    std::vector<size_t> cursor(n, 0);
    size_t next = 0;
    for (UINT i = 0; i < n; ++i) {
        if (!counts[i]) continue;
        StateObjectTranslation::Desc& d = t.descs[out];
        d.association = {&t.subobjects[api[i]], static_cast<UINT>(counts[i]), &t.names[next]};
        t.subobjects[out++] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &d};
        cursor[i] = next;
        next += counts[i];
    }
    if (empty_local) {
        StateObjectTranslation::Desc& local = t.descs[out];
        local.local.pLocalRootSignature = empty_local;
        const D3D12_STATE_SUBOBJECT* declared = &t.subobjects[out];
        t.subobjects[out++] = {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local};
        StateObjectTranslation::Desc& d = t.descs[out];
        d.association = {declared, 0, nullptr};         // no exports: an explicit default
        t.subobjects[out++] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &d};
    }
    size_t named = 0;
    for (UINT i = 0; i < n; ++i)
        if (a.pSubobjects[i].Type == D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY)
            fill_associations(a, inherited, i, cursor, identity, &named, t.names);
    return out == total && named == identity.size() ? S_OK : E_UNEXPECTED;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

// ---- D105-D107 ----------------------------------------------------------------------------------------------------
SIZE_T APIENTRY calc_state_object(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_STATE_OBJECT_0054*) {
    return sizeof(StateObjectRecord);
}

// CreateStateObject (grow_from null) and AddToStateObject (the parent's handle): the record in h. An inert record first,
// so that DestroyStateObject is valid whatever happens below.
HRESULT create_record(DeviceContext* c, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& args,
                      const D3D12DDI_HSTATEOBJECT_0054* grow_from, D3D12DDI_HSTATEOBJECT_0054 h,
                      D3D12DDI_HRTSTATEOBJECT_0054 rt) noexcept {
    auto* r = new (h.pDrvPrivate) StateObjectRecord{{Tag::StateObject, kRecordInvalid, nullptr, c}, nullptr, nullptr, rt,
                                                    false};
    const char* const slot = grow_from ? "AddToStateObject" : "CreateStateObject";
    Text text;
    text.add("type %u, %u subobjects", static_cast<UINT>(args.Type), args.NumSubobjects);
    const bool executable = args.Type == D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    HRESULT hr = S_OK;
    const StateObjectRecord* parent = nullptr;
    if (grow_from) {
        // A live RAYTRACING_PIPELINE of this device grows into another (Raytracing.md:3781-3787; the engine refuses
        // any other type, raytracing_pipeline.c:2865-2870). The parent's own flags are the engine's check (:2867).
        parent = record_of<StateObjectRecord>(grow_from->pDrvPrivate, Tag::StateObject, c);
        if (!parent || !parent->properties || !parent->translation || !parent->executable) parent = nullptr;
        text.add("; grows %s", parent ? "a live pipeline" : "no live pipeline of this device");
        hr = !parent || !executable ? E_INVALIDARG : !c->device7 ? E_NOTIMPL : S_OK;
    } else if (!executable && args.Type != D3D12DDI_STATE_OBJECT_TYPE_COLLECTION) {
        hr = args.Type == D3D12DDI_STATE_OBJECT_TYPE_EXECUTABLE ? E_NOTIMPL : E_INVALIDARG;   // work graphs
    } else if (!c->device5) {
        hr = E_NOTIMPL;
    }
    StateObjectTranslation* t = SUCCEEDED(hr) ? make_new<StateObjectTranslation>() : nullptr;
    if (SUCCEEDED(hr) && !t) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = translate(c, args, parent, *t, text);
    ID3D12StateObject* so = nullptr;
    ID3D12StateObjectProperties* properties = nullptr;
    if (SUCCEEDED(hr)) {
        const D3D12_STATE_OBJECT_DESC desc{static_cast<D3D12_STATE_OBJECT_TYPE>(args.Type),
                                           static_cast<UINT>(t->subobjects.size()), t->subobjects.data()};
        auto* const parent_engine = parent ? static_cast<ID3D12StateObject*>(parent->h.engine) : nullptr;
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
        if (const StateObjectObserver observe = state_object_observer.load())
            observe(desc, parent_engine, state_object_observer_user.load());
#endif
        hr = parent ? c->device7->AddToStateObject(&desc, parent_engine, __uuidof(ID3D12StateObject),
                                                   reinterpret_cast<void**>(&so))
                    : c->device5->CreateStateObject(&desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&so));
        if (SUCCEEDED(hr) && !so) hr = E_UNEXPECTED;
        if (SUCCEEDED(hr))
            hr = so->QueryInterface(__uuidof(ID3D12StateObjectProperties), reinterpret_cast<void**>(&properties));
        if (SUCCEEDED(hr) && !properties) hr = E_UNEXPECTED;
    }
    if (SUCCEEDED(hr) && parent) {
        // Deviation from bare forwarding: the new state object starts with its parent's pipeline stack size
        // (Raytracing.md:3777), and the engine gives it the computed default instead (raytracing_pipeline.c:2749-2752,
        // reached through d3d12_rt_state_object_create at :2846), so the bridge copies the parent's current setting.
        const UINT64 size = parent->properties->GetPipelineStackSize();
        properties->SetPipelineStackSize(size);
        text.add("; parent's pipeline stack size %llu", static_cast<unsigned long long>(size));
    }
    // One line per create: what the runtime sent (the INFERENCES at the top of this file) and the answer.
    if (FAILED(hr)) {
        // Both slots are creation functions of the AllowOutOfMemory category (engine-ddi.h, admitted_create_failure):
        // the runtime removes the device for any other failure code, and the application then sees
        // DXGI_ERROR_DEVICE_REMOVED for a refused state object (The Ascent, lab trial 465: UE 4.26 stops with a GPU
        // crash report at CreateStateObject). The real code and the description are on the refusal line.
        const HRESULT admitted = admitted_create_failure(hr);
        log_refusal("%s: %s; hr %08lx reported as %08lx", slot, text.buffer, static_cast<unsigned long>(hr),
                    static_cast<unsigned long>(admitted));
        if (properties) properties->Release();
        if (so) so->Release();
        delete t;
        return admitted;
    }
    log_line("%s: %s; hr %08lx", slot, text.buffer, static_cast<unsigned long>(hr));
    r->h.engine = so;
    r->h.flags = 0;
    r->properties = properties;
    r->translation = t;
    r->executable = executable;
    c->live.fetch_add(1);
    return S_OK;
}

HRESULT APIENTRY create_state_object(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_STATE_OBJECT_0054* args,
                                     D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 rt) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate) {
        log_refusal("CreateStateObject: no device, description or record (hr %08lx)", static_cast<unsigned long>(E_INVALIDARG));
        return admitted_create_failure(E_INVALIDARG);
    }
    return create_record(c, *args, nullptr, h, rt);
}

// ---- D115, D116: AddToStateObject -----------------------------------------------------------------------------------
SIZE_T APIENTRY calc_add_to_state_object(D3D12DDI_HDEVICE, const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072*) {
    return sizeof(StateObjectRecord);
}

// The addition is a description valid on its own (Raytracing.md:3781-3783), translated as a create; the child's record
// holds engine references only (its parent's among them, translate), nothing of the parent's record: the runtime
// destroys a parent while its children live (Raytracing.md:9663-9671).
HRESULT APIENTRY add_to_state_object(D3D12DDI_HDEVICE device, const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072* args,
                                     D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 rt) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate) {
        log_refusal("AddToStateObject: no device, description or record (hr %08lx)", static_cast<unsigned long>(E_INVALIDARG));
        return admitted_create_failure(E_INVALIDARG);
    }
    const D3D12DDIARG_CREATE_STATE_OBJECT_0054 addition{args->Type, args->NumSubobjects, args->pSubobjects};
    return create_record(c, addition, &args->StateObjectToGrowFrom, h, rt);
}

void APIENTRY destroy_state_object(D3D12DDI_HDEVICE device, D3D12DDI_HSTATEOBJECT_0054 h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<StateObjectRecord>(h.pDrvPrivate, Tag::StateObject, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    drain_all(c, Drain::Destroy);
    const bool held = r->h.engine != nullptr;
    if (r->properties) r->properties->Release();
    r->properties = nullptr;
    release_engine(r->h);
    delete r->translation;                      // after the engine's final reference
    r->translation = nullptr;
    poison(r->h);
    if (held) c->live.fetch_sub(1);
}

// ---- D110-D113: no device argument; the record names its device -------------------------------------------------
StateObjectRecord* live_state_object(D3D12DDI_HSTATEOBJECT_0054 h, const char* slot) noexcept {
    auto* r = record_of<StateObjectRecord>(h.pDrvPrivate, Tag::StateObject);
    if (r && r->properties) return r;
    log_line("%s: not a live state object record", slot);
    return nullptr;
}

// The engine answers in 64 bits; the DDI in 32. UINT_MAX, the engine's answer for an unknown export, survives;
// a larger size is logged and answered as UINT_MAX.
UINT narrow_stack_size(UINT64 size, const char* slot) noexcept {
    if (size <= UINT_MAX) return static_cast<UINT>(size);
    log_line("%s: stack size %llu does not fit the DDI's UINT", slot, static_cast<unsigned long long>(size));
    return UINT_MAX;
}

// The engine's identifier lives as long as the state object (raytracing_pipeline.c,
// d3d12_rt_state_object_identifier). NULL for an unknown export, as the engine answers.
void* APIENTRY get_shader_identifier(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR name) {
    StateObjectRecord* r = live_state_object(h, "GetShaderIdentifier");
    return (r && name) ? r->properties->GetShaderIdentifier(name) : nullptr;
}

UINT APIENTRY get_shader_stack_size(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR name) {
    StateObjectRecord* r = live_state_object(h, "GetShaderStackSize");
    if (!r || !name) return UINT_MAX;
    return narrow_stack_size(r->properties->GetShaderStackSize(name), "GetShaderStackSize");
}

UINT APIENTRY get_pipeline_stack_size(D3D12DDI_HSTATEOBJECT_0054 h) {
    StateObjectRecord* r = live_state_object(h, "GetPipelineStackSize");
    return r ? narrow_stack_size(r->properties->GetPipelineStackSize(), "GetPipelineStackSize") : 0;
}

// The engine reads the size when a list records with the state object: with replay on, the calls recorded before
// this one run first.
void APIENTRY set_pipeline_stack_size(D3D12DDI_HSTATEOBJECT_0054 h, UINT size) {
    StateObjectRecord* r = live_state_object(h, "SetPipelineStackSize");
    if (!r) return;
    drain_all(r->h.device, Drain::Stack);
    r->properties->SetPipelineStackSize(size);
}
} // namespace

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
void harness_set_state_object_observer(StateObjectObserver observer, void* user) noexcept {
    state_object_observer_user.store(user);
    state_object_observer.store(observer);
}
#endif

void fill_core_state_objects(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateStateObjectSize = calc_state_object;
    t->pfnCreateStateObject = create_state_object;
    t->pfnDestroyStateObject = destroy_state_object;
    t->pfnGetShaderIdentifier = get_shader_identifier;
    t->pfnGetShaderStackSize = get_shader_stack_size;
    t->pfnGetPipelineStackSize = get_pipeline_stack_size;
    t->pfnSetPipelineStackSize = set_pipeline_stack_size;
    t->pfnCalcPrivateAddToStateObjectSize = calc_add_to_state_object;
    t->pfnAddToStateObject = add_to_state_object;
}

} // namespace engine_ddi
