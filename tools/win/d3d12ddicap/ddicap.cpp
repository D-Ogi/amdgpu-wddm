// SPDX-License-Identifier: MIT
// d3d12ddicap: prints the D3D12 runtime's DDI description of every ray tracing state object an application creates,
// on this PC, with no driver of ours. The program loads the WARP user-mode driver (d3d10warp.dll) before the runtime
// does, points WARP's OpenAdapter12 export at a wrapper, and the wrapper replaces pfnCreateStateObject and
// pfnAddToStateObject in the device table the runtime asks WARP to fill. Each wrapper prints the description the
// runtime built (d3d12umddi.h, D3D12DDIARG_CREATE_STATE_OBJECT_0054) and then calls WARP's own slot.
// The application side builds state objects the way Unreal Engine 4.26 does (D3D12RHI of 4.26.1-release, read for
// facts only): one COLLECTION per shader, then a RAYTRACING_PIPELINE that links the collections, or an
// AddToStateObject that adds new collections to a base pipeline. It also builds the shape of the project's own
// client. Console only: no window, nothing resident; it exits when the cases are done.
//
// Usage: d3d12ddicap.exe [--hardware[=<adapter substring>]] [--idle-seconds=<n>] [case...]
//   cases: ue426 ue426-additions ue426-basic ue426-add client-collection all (default all)
//   --hardware: no WARP hook; the cases run on the first hardware adapter (or the first whose description holds the
//   substring) through its own driver, and print the API results and the device removed reason only. This is the
//   lab client of The Ascent's trial 465 (README.md).
//   --idle-seconds: sleep that long before the first link (ue426 cases) or the addition (ue426-add).
#include <windows.h>
#include <d3d12.h>
#include <cstdlib>
#include <dxgi1_6.h>
#include <d3d12umddi.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ue426-rgs.h"
#include "ue426-ms.h"
#include "ue426-hit.h"

using Microsoft::WRL::ComPtr;

namespace {
// ---- The DDI side: the wrappers around WARP's slots ----------------------------------------------------------------
PFND3D12DDI_OPENADAPTER warp_open = nullptr;
PFND3D12DDI_FILLDDITTABLE warp_fill = nullptr;
PFND3D12DDI_CREATE_STATE_OBJECT_0054 warp_create = nullptr;
PFND3D12DDI_ADD_TO_STATE_OBJECT_0072 warp_add = nullptr;
bool core_table_seen = false;

// Every state object the runtime created through the wrapper: its driver handle, its description (the runtime keeps
// it alive as long as the state object, Raytracing.md "Collection lifetimes") and the number this program gave it.
struct Created {
    void* handle;
    D3D12DDIARG_CREATE_STATE_OBJECT_0054 args;      // a copy: the runtime may pass the structure itself on its stack
    int number;
};
std::vector<Created> created;
int next_number = 1;

const char* type_name(D3D12DDI_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
    case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: return "STATE_OBJECT_CONFIG";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: return "GLOBAL_ROOT_SIGNATURE";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: return "LOCAL_ROOT_SIGNATURE";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK: return "NODE_MASK";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: return "DXIL_LIBRARY";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: return "EXISTING_COLLECTION";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: return "RAYTRACING_SHADER_CONFIG";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: return "RAYTRACING_PIPELINE_CONFIG";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP: return "HIT_GROUP";
    case D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY: return "SHADER_EXPORT_SUMMARY";
    default: return "?";
    }
}

const wchar_t* w(LPCWSTR s) { return s ? s : L"(null)"; }

// Where a subobject pointer of a summary points: "#i" into this description, "S<n>#i" into the description of an
// earlier state object n (an imported collection or a parent), or "outside".
std::string locate(const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a, const D3D12DDI_STATE_SUBOBJECT_0054* p) {
    char text[64];
    auto in = [&](const D3D12DDIARG_CREATE_STATE_OBJECT_0054& d) -> int {
        for (UINT i = 0; i < d.NumSubobjects; ++i)
            if (&d.pSubobjects[i] == p) return static_cast<int>(i);
        return -1;
    };
    int i = in(a);
    if (i >= 0) {
        snprintf(text, sizeof(text), "#%d %s", i, type_name(p->Type));
        return text;
    }
    for (const Created& c : created) {
        i = in(c.args);
        if (i >= 0) {
            snprintf(text, sizeof(text), "S%d#%d %s", c.number, i, type_name(p->Type));
            return text;
        }
    }
    // Not a subobject of any description: the same description pointer, then, in this one or an earlier one?
    auto same_desc = [&](const D3D12DDIARG_CREATE_STATE_OBJECT_0054& d) -> int {
        for (UINT k = 0; k < d.NumSubobjects; ++k)
            if (d.pSubobjects[k].Type == p->Type && d.pSubobjects[k].pDesc == p->pDesc) return static_cast<int>(k);
        return -1;
    };
    if (p && (i = same_desc(a)) >= 0) {
        snprintf(text, sizeof(text), "outside, pDesc of #%d %s", i, type_name(p->Type));
        return text;
    }
    for (const Created& c : created)
        if (p && (i = same_desc(c.args)) >= 0) {
            snprintf(text, sizeof(text), "outside, pDesc of S%d#%d %s", c.number, i, type_name(p->Type));
            return text;
        }
    snprintf(text, sizeof(text), "outside, own pDesc (type %s)", p ? type_name(p->Type) : "-");
    return text;
}

int number_of(const void* handle) {
    for (const Created& c : created)
        if (c.handle == handle) return c.number;
    return 0;
}

void print_description(const char* slot, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& a, int number,
                       const void* parent) {
    printf("ddi %s S%d: type %u, %u subobjects", slot, number, static_cast<UINT>(a.Type), a.NumSubobjects);
    if (parent) printf(", grows S%d", number_of(parent));
    printf("\n");
    for (UINT i = 0; i < a.NumSubobjects; ++i) {
        const D3D12DDI_STATE_SUBOBJECT_0054& s = a.pSubobjects[i];
        printf("  #%u %s (0x%x)", i, type_name(s.Type), static_cast<UINT>(s.Type));
        switch (s.Type) {
        case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
            printf(" flags 0x%x\n", static_cast<UINT>(static_cast<const D3D12DDI_STATE_OBJECT_CONFIG_0054*>(s.pDesc)->Flags));
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            printf(" root signature %p\n", static_cast<const D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054*>(s.pDesc)->hGlobalRootSignature.pDrvPrivate);
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            printf(" root signature %p\n", static_cast<const D3D12DDI_LOCAL_ROOT_SIGNATURE_0054*>(s.pDesc)->hLocalRootSignature.pDrvPrivate);
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK:
            printf(" mask %u\n", static_cast<const D3D12DDI_NODE_MASK_0054*>(s.pDesc)->NodeMask);
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
            const auto& l = *static_cast<const D3D12DDI_DXIL_LIBRARY_DESC_0054*>(s.pDesc);
            printf(" first word 0x%08x, %u exports\n", l.pDXILLibrary ? l.pDXILLibrary[0] : 0u, l.NumExports);
            for (UINT e = 0; e < l.NumExports; ++e)
                printf("    export Name %ls ExportToRename %ls\n", w(l.pExports[e].Name), w(l.pExports[e].ExportToRename));
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
            const auto& c = *static_cast<const D3D12DDI_EXISTING_COLLECTION_DESC_0054*>(s.pDesc);
            printf(" collection S%d, %u exports\n", number_of(c.hExistingCollection.pDrvPrivate), c.NumExports);
            for (UINT e = 0; e < c.NumExports; ++e)
                printf("    export Name %ls ExportToRename %ls\n", w(c.pExports[e].Name), w(c.pExports[e].ExportToRename));
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
            const auto& c = *static_cast<const D3D12DDI_RAYTRACING_SHADER_CONFIG_0054*>(s.pDesc);
            printf(" payload %u attributes %u\n", c.MaxPayloadSizeInBytes, c.MaxAttributeSizeInBytes);
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
            const auto& c = *static_cast<const D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075*>(s.pDesc);
            printf(" depth %u flags 0x%x (as _0075)\n", c.MaxTraceRecursionDepth, static_cast<UINT>(c.Flags));
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
            const auto& g = *static_cast<const D3D12DDI_HIT_GROUP_DESC_0054*>(s.pDesc);
            printf(" %ls type %u anyhit %ls closest %ls intersection %ls summary flags 0x%x\n", w(g.HitGroupExport),
                   static_cast<UINT>(g.Type), w(g.AnyHitShaderImport), w(g.ClosestHitShaderImport),
                   w(g.IntersectionShaderImport), static_cast<UINT>(g.SummaryFlags));
            break;
        }
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY: {
            const auto& f = *static_cast<const D3D12DDI_FUNCTION_SUMMARY_0054*>(s.pDesc);
            printf(" %u exports, overall flags 0x%x\n", f.NumExportedFunctions, static_cast<UINT>(f.OverallFlags));
            for (UINT e = 0; e < f.NumExportedFunctions; ++e) {
                const D3D12DDI_FUNCTION_SUMMARY_NODE_0054& n = f.pSummaries[e];
                printf("    export unmangled %ls mangled %ls flags 0x%x, %u associated:\n", w(n.ExportNameUnmangled),
                       w(n.ExportNameMangled), static_cast<UINT>(n.Flags), n.NumAssociatedSubobjects);
                for (UINT k = 0; k < n.NumAssociatedSubobjects; ++k)
                    printf("      %s\n", locate(a, n.ppAssociatedSubobjects[k]).c_str());
            }
            break;
        }
        default:
            printf("\n");
            break;
        }
    }
    fflush(stdout);
}

HRESULT APIENTRY create_state_object(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_STATE_OBJECT_0054* args,
                                     D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 rt) {
    const int number = next_number++;
    print_description("CreateStateObject", *args, number, nullptr);
    const HRESULT hr = warp_create(device, args, h, rt);
    printf("ddi CreateStateObject S%d: WARP hr %08lx\n", number, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) created.push_back({h.pDrvPrivate, *args, number});
    return hr;
}

HRESULT APIENTRY add_to_state_object(D3D12DDI_HDEVICE device, const D3D12DDIARG_ADD_TO_STATE_OBJECT_0072* args,
                                     D3D12DDI_HSTATEOBJECT_0054 h, D3D12DDI_HRTSTATEOBJECT_0054 rt) {
    const int number = next_number++;
    const D3D12DDIARG_CREATE_STATE_OBJECT_0054 as_create{args->Type, args->NumSubobjects, args->pSubobjects};
    print_description("AddToStateObject", as_create, number, args->StateObjectToGrowFrom.pDrvPrivate);
    const HRESULT hr = warp_add(device, args, h, rt);
    printf("ddi AddToStateObject S%d: WARP hr %08lx\n", number, static_cast<unsigned long>(hr));
    return hr;
}

HRESULT APIENTRY fill_table(D3D12DDI_HADAPTER adapter, D3D12DDI_TABLE_TYPE type, void* table, SIZE_T size, UINT version,
                            D3D12DDI_HRTTABLE rt) {
    const HRESULT hr = warp_fill(adapter, type, table, size, version, rt);
    if (SUCCEEDED(hr) && type == D3D12DDI_TABLE_TYPE_DEVICE_CORE) {
        auto* core = static_cast<D3D12DDI_DEVICE_FUNCS_CORE_0072*>(table);
        constexpr size_t create_end = offsetof(D3D12DDI_DEVICE_FUNCS_CORE_0072, pfnCreateStateObject) + sizeof(void*);
        constexpr size_t add_end = offsetof(D3D12DDI_DEVICE_FUNCS_CORE_0072, pfnAddToStateObject) + sizeof(void*);
        printf("ddi FillDDITable DEVICE_CORE: %zu bytes, interface 0x%x\n", static_cast<size_t>(size), version);
        if (size >= create_end && core->pfnCreateStateObject) {
            warp_create = core->pfnCreateStateObject;
            core->pfnCreateStateObject = create_state_object;
            core_table_seen = true;
        }
        if (size >= add_end && core->pfnAddToStateObject) {
            warp_add = core->pfnAddToStateObject;
            core->pfnAddToStateObject = add_to_state_object;
        }
    }
    return hr;
}

HRESULT APIENTRY open_adapter(D3D12DDIARG_OPENADAPTER* args) {
    const HRESULT hr = warp_open(args);
    if (SUCCEEDED(hr) && args->pAdapterFuncs && args->pAdapterFuncs->pfnFillDDITable) {
        warp_fill = args->pAdapterFuncs->pfnFillDDITable;
        args->pAdapterFuncs->pfnFillDDITable = fill_table;
    }
    printf("ddi OpenAdapter12 (WARP): hr %08lx\n", static_cast<unsigned long>(hr));
    return hr;
}

// Points the OpenAdapter12 export of the loaded WARP image at open_adapter: the export table holds 32-bit offsets
// from the image base, so the entry names a small jump placed above the image.
bool hook_warp() {
    HMODULE warp = LoadLibraryW(L"d3d10warp.dll");
    if (!warp) return false;
    auto* base = reinterpret_cast<BYTE*>(warp);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    DWORD* entry = nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        if (!strcmp(reinterpret_cast<const char*>(base + names[i]), "OpenAdapter12")) entry = &functions[ordinals[i]];
    if (!entry) return false;
    warp_open = reinterpret_cast<PFND3D12DDI_OPENADAPTER>(base + *entry);
    const uintptr_t end = reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
    BYTE* stub = nullptr;
    for (uintptr_t at = (end + 0xFFFF) & ~uintptr_t{0xFFFF}; !stub && at < end + 0x40000000; at += 0x10000)
        stub = static_cast<BYTE*>(VirtualAlloc(reinterpret_cast<void*>(at), 4096, MEM_RESERVE | MEM_COMMIT,
                                               PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    static const BYTE jump[6] = {0xFF, 0x25, 0, 0, 0, 0};     // jmp qword ptr [rip+0]
    memcpy(stub, jump, sizeof(jump));
    const void* target = reinterpret_cast<const void*>(&open_adapter);
    memcpy(stub + sizeof(jump), &target, sizeof(target));
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &old)) return false;
    *entry = static_cast<DWORD>(stub - base);
    VirtualProtect(entry, sizeof(*entry), old, &old);
    return GetProcAddress(warp, "OpenAdapter12") == reinterpret_cast<FARPROC>(stub);
}

// ---- The application side -------------------------------------------------------------------------------------------
HRESULT root_signature(ID3D12Device* device, int kind, ComPtr<ID3D12RootSignature>& out) {
    // kind 0: global (SRV t0, UAV u0); 1: local (one constant at b0 space1); 2: empty local.
    D3D12_ROOT_PARAMETER parameters[2]{};
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.pParameters = parameters;
    if (kind == 0) {
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        desc.NumParameters = 2;
    } else if (kind == 1) {
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 1, 1};
        desc.NumParameters = 1;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    } else {
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    }
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
    if (FAILED(hr)) return hr;
    return device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
}

struct Signatures {
    ComPtr<ID3D12RootSignature> global, local, empty;
};

HRESULT api_create(ID3D12Device5* device, const char* what, D3D12_STATE_OBJECT_TYPE type,
                   const std::vector<D3D12_STATE_SUBOBJECT>& subobjects, ComPtr<ID3D12StateObject>& out) {
    const D3D12_STATE_OBJECT_DESC desc{type, static_cast<UINT>(subobjects.size()), subobjects.data()};
    printf("api CreateStateObject %s: %zu subobjects\n", what, subobjects.size());
    fflush(stdout);
    const HRESULT hr = device->CreateStateObject(&desc, IID_PPV_ARGS(&out));
    printf("api CreateStateObject %s: hr %08lx\n", what, static_cast<unsigned long>(hr));
    fflush(stdout);
    return hr;
}

// ---- Unreal Engine 4.26 (4.26.1-release, Engine/Source/Runtime/D3D12RHI/Private/D3D12RayTracing.cpp) --------------
// The shapes below follow the engine's CreateRayTracingStateObject, FD3D12RayTracingPipelineCache and
// FD3D12RayTracingPipelineState. Facts only, no engine code. Shader names have the engine's form "<prefix>_<16 hex
// digits of the shader hash>"; the hashes here are made up.
enum class UeKind { RayGen, Miss, HitGroup };

// One shader as the pipeline cache compiles it into its own COLLECTION.
struct UeShader {
    UeKind kind;
    std::wstring primary;                      // RayGen_<hash>, Miss_<hash> or HitGroup_<hash>
    std::wstring closest, anyhit;              // a hit group's CHS_<hash> and AHS_<hash>; anyhit may be empty
    ID3D12RootSignature* local;                // a ray generation shader: the empty local root signature
};

// The renamed library exports of a shader (the export list of the collection), and the original entry points.
void ue_exports(const UeShader& shader, std::vector<std::wstring>& renamed, std::vector<std::wstring>& original) {
    if (shader.kind == UeKind::HitGroup) {
        renamed = {shader.closest};
        original = {L"MainCHS"};
        if (!shader.anyhit.empty()) {
            renamed.push_back(shader.anyhit);
            original.push_back(L"MainAHS");
        }
    } else {
        renamed = {shader.primary};
        original = {shader.kind == UeKind::RayGen ? L"MainRGS" : L"MainMS"};
    }
}

// The subobjects of CreateRayTracingStateObject in the engine's order, for a COLLECTION (one library, the shader's
// exports, a hit group for a hit shader, one local root signature) or a link (no library, no exports, no local root
// signature, the collections). The shader configuration's association names every export; for a link it names none
// (an explicit default). One local root signature association per export. The flags are those of the
// STATE_OBJECT_CONFIG: ALLOW_STATE_OBJECT_ADDITIONS when the engine found RaytracingTier 1.1 and ID3D12Device7
// (D3D12Adapter.cpp, GRHISupportsRayTracingPSOAdditions), else none.
HRESULT ue_state_object(ID3D12Device5* device, const Signatures& sig, const UeShader* shader,
                        const std::vector<ID3D12StateObject*>& collections, D3D12_STATE_OBJECT_FLAGS flags,
                        const char* what, ComPtr<ID3D12StateObject>& out) {
    std::vector<std::wstring> renamed, original;
    if (shader) ue_exports(*shader, renamed, original);
    std::vector<D3D12_EXPORT_DESC> exports;
    std::vector<LPCWSTR> names;
    for (size_t i = 0; i < renamed.size(); ++i) {
        exports.push_back({renamed[i].c_str(), original[i].c_str(), D3D12_EXPORT_FLAG_NONE});
        names.push_back(renamed[i].c_str());
    }
    const void* code = !shader ? nullptr
                       : shader->kind == UeKind::RayGen ? static_cast<const void*>(g_ue426_rgs)
                       : shader->kind == UeKind::Miss   ? static_cast<const void*>(g_ue426_ms)
                                                        : static_cast<const void*>(g_ue426_hit);
    const size_t size = !shader ? 0
                        : shader->kind == UeKind::RayGen ? sizeof(g_ue426_rgs)
                        : shader->kind == UeKind::Miss   ? sizeof(g_ue426_ms)
                                                         : sizeof(g_ue426_hit);
    const D3D12_DXIL_LIBRARY_DESC library{{code, size}, static_cast<UINT>(exports.size()), exports.data()};
    const D3D12_RAYTRACING_SHADER_CONFIG shader_config{24, 8};      // the initializer's default payload, 2 floats
    const bool hit = shader && shader->kind == UeKind::HitGroup;
    const D3D12_HIT_GROUP_DESC group{hit ? shader->primary.c_str() : nullptr, D3D12_HIT_GROUP_TYPE_TRIANGLES,
                                     hit && !shader->anyhit.empty() ? shader->anyhit.c_str() : nullptr,
                                     hit ? shader->closest.c_str() : nullptr, nullptr};
    const D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};      // RAY_TRACING_MAX_ALLOWED_RECURSION_DEPTH
    const D3D12_STATE_OBJECT_CONFIG config{flags};
    const D3D12_GLOBAL_ROOT_SIGNATURE global{sig.global.Get()};
    const D3D12_LOCAL_ROOT_SIGNATURE local{shader ? shader->local : nullptr};
    std::vector<D3D12_EXISTING_COLLECTION_DESC> imports;
    for (ID3D12StateObject* c : collections) imports.push_back({c, 0, nullptr});
    // Sized once: the associations point into the array.
    std::vector<D3D12_STATE_SUBOBJECT> s(5 + (shader ? 2 + (hit ? 1 : 0) + names.size() : 0) + imports.size());
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> associations(names.size());
    size_t i = 0;
    if (shader) s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library};
    const size_t shader_config_index = i;
    s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config};
    const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shader_config_association{
        &s[shader_config_index], static_cast<UINT>(names.size()), names.empty() ? nullptr : names.data()};
    s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &shader_config_association};
    if (hit) s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group};
    s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config};
    s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config};
    s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global};
    if (shader) {
        const size_t local_index = i;
        s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local};
        for (size_t e = 0; e < names.size(); ++e) {
            associations[e] = {&s[local_index], 1, &names[e]};
            s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &associations[e]};
        }
    }
    for (const D3D12_EXISTING_COLLECTION_DESC& c : imports) s[i++] = {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &c};
    if (i != s.size()) return E_UNEXPECTED;
    return api_create(device, what,
                      shader ? D3D12_STATE_OBJECT_TYPE_COLLECTION : D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, s, out);
}

// The engine's addition onto a base pipeline (FD3D12RayTracingPipelineState with a BasePipeline): a
// STATE_OBJECT_CONFIG with ALLOW_STATE_OBJECT_ADDITIONS and the collections the base does not have, nothing else.
HRESULT ue_addition(ID3D12Device5* device, ID3D12StateObject* base, const std::vector<ID3D12StateObject*>& collections,
                    const char* what, ComPtr<ID3D12StateObject>& out) {
    ComPtr<ID3D12Device7> device7;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device7)))) {
        printf("api %s: no ID3D12Device7\n", what);
        return E_NOINTERFACE;
    }
    const D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    std::vector<D3D12_EXISTING_COLLECTION_DESC> imports;
    for (ID3D12StateObject* c : collections) imports.push_back({c, 0, nullptr});
    std::vector<D3D12_STATE_SUBOBJECT> s{{D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config}};
    for (const D3D12_EXISTING_COLLECTION_DESC& c : imports) s.push_back({D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &c});
    const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, static_cast<UINT>(s.size()), s.data()};
    printf("api AddToStateObject %s: %zu subobjects\n", what, s.size());
    const HRESULT hr = device7->AddToStateObject(&desc, base, IID_PPV_ARGS(&out));
    printf("api AddToStateObject %s: hr %08lx\n", what, static_cast<unsigned long>(hr));
    return hr;
}

// Prints whether each name has a shader identifier in the state object; returns the count of null identifiers among
// the names expected.
int identifiers(ID3D12StateObject* object, const char* what, const std::vector<std::wstring>& expected,
                const std::vector<std::wstring>& absent = {}) {
    ComPtr<ID3D12StateObjectProperties> properties;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&properties)))) return static_cast<int>(expected.size());
    int missing = 0;
    for (const std::wstring& name : expected) {
        const bool found = properties->GetShaderIdentifier(name.c_str()) != nullptr;
        printf("api %s GetShaderIdentifier %ls: %s\n", what, name.c_str(), found ? "found" : "null");
        missing += found ? 0 : 1;
    }
    for (const std::wstring& name : absent) {
        const bool found = properties->GetShaderIdentifier(name.c_str()) != nullptr;
        printf("api %s GetShaderIdentifier %ls: %s (expected null)\n", what, name.c_str(), found ? "found" : "null");
        missing += found ? 1 : 0;
    }
    printf("api %s GetPipelineStackSize %llu\n", what, static_cast<unsigned long long>(properties->GetPipelineStackSize()));
    return missing;
}

// The shaders of the cases. The occlusion and intersection pipelines are those FD3D12BasicRayTracingPipeline creates
// at InitRayTracing (a ray generation shader, the default miss shader, a closest hit shader); the extra ones stand for
// a later material pipeline.
struct UeShaders {
    UeShader occlusion_rgs, intersection_rgs, default_ms, default_chs, intersection_chs, material_hit, material_ms;
    explicit UeShaders(const Signatures& sig)
        : occlusion_rgs{UeKind::RayGen, L"RayGen_0000000000000001", L"", L"", sig.empty.Get()},
          intersection_rgs{UeKind::RayGen, L"RayGen_0000000000000002", L"", L"", sig.empty.Get()},
          default_ms{UeKind::Miss, L"Miss_0000000000000003", L"", L"", sig.local.Get()},
          default_chs{UeKind::HitGroup, L"HitGroup_0000000000000004", L"CHS_0000000000000004", L"", sig.local.Get()},
          intersection_chs{UeKind::HitGroup, L"HitGroup_0000000000000005", L"CHS_0000000000000005", L"",
                           sig.local.Get()},
          material_hit{UeKind::HitGroup, L"HitGroup_0000000000000006", L"CHS_0000000000000006",
                       L"AHS_0000000000000006", sig.local.Get()},
          material_ms{UeKind::Miss, L"Miss_0000000000000007", L"", L"", sig.local.Get()} {}
};

// One pipeline as FD3D12RayTracingPipelineState links it without a base: a collection per shader (the cache's, or a
// new one), then the link of 8 subobjects for three collections (trial 465: 8 subobjects, {24, 8}).
struct UeCache {
    std::vector<std::pair<const UeShader*, ComPtr<ID3D12StateObject>>> entries;
    HRESULT get(ID3D12Device5* device, const Signatures& sig, const UeShader& shader, D3D12_STATE_OBJECT_FLAGS flags,
                ID3D12StateObject** out) {
        for (auto& e : entries)
            if (e.first == &shader) {
                *out = e.second.Get();
                return S_OK;
            }
        ComPtr<ID3D12StateObject> collection;
        char what[64];
        snprintf(what, sizeof(what), "COLLECTION %ls", shader.primary.c_str());
        const HRESULT hr = ue_state_object(device, sig, &shader, {}, flags, what, collection);
        if (FAILED(hr)) return hr;
        *out = collection.Get();
        entries.emplace_back(&shader, collection);
        return S_OK;
    }
};

HRESULT ue_pipeline(ID3D12Device5* device, const Signatures& sig, UeCache& cache, std::vector<const UeShader*> shaders,
                    D3D12_STATE_OBJECT_FLAGS flags, const char* what, ComPtr<ID3D12StateObject>& out) {
    std::vector<ID3D12StateObject*> collections;
    for (const UeShader* shader : shaders) {
        ID3D12StateObject* c = nullptr;
        const HRESULT hr = cache.get(device, sig, *shader, flags, &c);
        if (FAILED(hr)) return hr;
        collections.push_back(c);
    }
    return ue_state_object(device, sig, nullptr, collections, flags, what, out);
}

void idle(ID3D12Device5* device, DWORD idle_ms) {
    if (!idle_ms) return;
    printf("api idle %lu ms before the link\n", static_cast<unsigned long>(idle_ms));
    Sleep(idle_ms);
    printf("api device removed reason after the idle: %08lx\n", static_cast<unsigned long>(device->GetDeviceRemovedReason()));
}

// ue426 (flags none) and ue426-additions (ALLOW_STATE_OBJECT_ADDITIONS): the occlusion pipeline. ue426-basic: both
// pipelines of InitRayTracing with additions, the second one taking the cached miss collection.
HRESULT ue426_case(ID3D12Device5* device, const Signatures& sig, D3D12_STATE_OBJECT_FLAGS flags, bool both,
                   DWORD idle_ms) {
    const UeShaders u(sig);
    UeCache cache;
    for (const UeShader* shader : {&u.occlusion_rgs, &u.default_ms, &u.default_chs}) {
        ID3D12StateObject* c = nullptr;
        const HRESULT hr = cache.get(device, sig, *shader, flags, &c);
        if (FAILED(hr)) return hr;
    }
    idle(device, idle_ms);
    ComPtr<ID3D12StateObject> occlusion, intersection;
    HRESULT hr = ue_pipeline(device, sig, cache, {&u.occlusion_rgs, &u.default_ms, &u.default_chs}, flags,
                             "RAYTRACING_PIPELINE occlusion", occlusion);
    if (FAILED(hr)) return hr;
    int missing = identifiers(occlusion.Get(), "occlusion", {u.occlusion_rgs.primary, u.default_ms.primary,
                                                             u.default_chs.primary});
    if (both) {
        hr = ue_pipeline(device, sig, cache, {&u.intersection_rgs, &u.default_ms, &u.intersection_chs}, flags,
                         "RAYTRACING_PIPELINE intersection", intersection);
        if (FAILED(hr)) return hr;
        missing += identifiers(intersection.Get(), "intersection",
                               {u.intersection_rgs.primary, u.default_ms.primary, u.intersection_chs.primary});
    }
    return missing ? E_FAIL : S_OK;
}

// ue426-add: the occlusion pipeline with additions as the base, then the engine's addition of two new collections (a
// hit group with an any hit shader and a miss shader). The grown object has every export; the base keeps its own.
HRESULT ue426_add_case(ID3D12Device5* device, const Signatures& sig, DWORD idle_ms) {
    const UeShaders u(sig);
    UeCache cache;
    constexpr D3D12_STATE_OBJECT_FLAGS flags = D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
    ComPtr<ID3D12StateObject> base, grown;
    HRESULT hr = ue_pipeline(device, sig, cache, {&u.occlusion_rgs, &u.default_ms, &u.default_chs}, flags,
                             "RAYTRACING_PIPELINE base", base);
    if (FAILED(hr)) return hr;
    ID3D12StateObject* hit = nullptr;
    ID3D12StateObject* miss = nullptr;
    hr = cache.get(device, sig, u.material_hit, flags, &hit);
    if (SUCCEEDED(hr)) hr = cache.get(device, sig, u.material_ms, flags, &miss);
    if (FAILED(hr)) return hr;
    idle(device, idle_ms);
    hr = ue_addition(device, base.Get(), {hit, miss}, "grown", grown);
    if (FAILED(hr)) return hr;
    int missing = identifiers(grown.Get(), "grown",
                              {u.occlusion_rgs.primary, u.default_ms.primary, u.default_chs.primary,
                               u.material_hit.primary, u.material_ms.primary});
    missing += identifiers(base.Get(), "base", {u.occlusion_rgs.primary, u.default_ms.primary, u.default_chs.primary},
                           {u.material_hit.primary, u.material_ms.primary});
    // The engine reads the identifiers again from the grown object after the base is gone (the cache trims it).
    base.Reset();
    missing += identifiers(grown.Get(), "grown after the base's release",
                           {u.occlusion_rgs.primary, u.material_hit.primary, u.material_ms.primary});
    return missing ? E_FAIL : S_OK;
}

// The project's client shape (tools/win/d3d12queue -RayCollection): one collection holding everything, without
// local root signatures or renames, then a pipeline of the collection, the global root signature and the pipeline
// config.
HRESULT client_collection_case(ID3D12Device5* device, const Signatures& sig) {
    D3D12_EXPORT_DESC exports[2]{{L"MainCHS", nullptr, D3D12_EXPORT_FLAG_NONE}, {L"MainAHS", nullptr, D3D12_EXPORT_FLAG_NONE}};
    const D3D12_DXIL_LIBRARY_DESC library{{g_ue426_hit, sizeof(g_ue426_hit)}, 2, exports};
    const D3D12_DXIL_LIBRARY_DESC library_rgs{{g_ue426_rgs, sizeof(g_ue426_rgs)}, 0, nullptr};
    const D3D12_DXIL_LIBRARY_DESC library_ms{{g_ue426_ms, sizeof(g_ue426_ms)}, 0, nullptr};
    const D3D12_HIT_GROUP_DESC group{L"group", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"MainAHS", L"MainCHS", nullptr};
    const D3D12_RAYTRACING_SHADER_CONFIG shader_config{24, 8};
    const D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    const D3D12_GLOBAL_ROOT_SIGNATURE global{sig.global.Get()};
    const D3D12_LOCAL_ROOT_SIGNATURE local{sig.local.Get()};
    const std::vector<D3D12_STATE_SUBOBJECT> s{
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}, {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library_rgs},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library_ms}, {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local}};
    ComPtr<ID3D12StateObject> collection, pipeline;
    HRESULT hr = api_create(device, "COLLECTION client", D3D12_STATE_OBJECT_TYPE_COLLECTION, s, collection);
    if (FAILED(hr)) return hr;
    const D3D12_EXISTING_COLLECTION_DESC existing{collection.Get(), 0, nullptr};
    const std::vector<D3D12_STATE_SUBOBJECT> p{{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
                                               {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
                                               {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}};
    return api_create(device, "RAYTRACING_PIPELINE client from collection", D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                      p, pipeline);
}
} // namespace

// The first hardware adapter, or the first whose description holds want; null when there is none.
ComPtr<IDXGIAdapter1> hardware_adapter(IDXGIFactory4* factory, const std::wstring& want) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!want.empty() && !wcsstr(desc.Description, want.c_str())) continue;
        printf("api adapter %u: %ls, vendor 0x%04x device 0x%04x\n", i, desc.Description, desc.VendorId, desc.DeviceId);
        return adapter;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool hardware = false;
    std::wstring want;
    DWORD idle_ms = 0;
    std::vector<std::string> cases;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--hardware") {
            hardware = true;
        } else if (!a.compare(0, 11, "--hardware=")) {
            hardware = true;
            want.assign(a.begin() + 11, a.end());
        } else if (!a.compare(0, 15, "--idle-seconds=")) {
            idle_ms = static_cast<DWORD>(std::strtoul(a.c_str() + 15, nullptr, 10) * 1000u);
        } else {
            cases.push_back(a);
        }
    }
    if (!hardware && !hook_warp()) {
        printf("could not point WARP's OpenAdapter12 at the wrapper\n");
        return 2;
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        printf("no DXGI factory\n");
        return 2;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (hardware) {
        ComPtr<IDXGIAdapter1> found = hardware_adapter(factory.Get(), want);
        if (found) found.As(&adapter);
    } else {
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    }
    if (!adapter) {
        printf("no %s adapter\n", hardware ? "matching hardware" : "WARP");
        return 2;
    }
    ComPtr<ID3D12Device5> device;
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    printf("api D3D12CreateDevice (%s): hr %08lx, core table wrapped: %s\n", hardware ? "hardware" : "WARP",
           static_cast<unsigned long>(hr), core_table_seen ? "yes" : "no");
    if (FAILED(hr) || (!hardware && !core_table_seen)) return 2;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    printf("api RaytracingTier %d\n", static_cast<int>(options5.RaytracingTier));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) return 3;
    Signatures sig;
    if (FAILED(root_signature(device.Get(), 0, sig.global)) || FAILED(root_signature(device.Get(), 1, sig.local)) ||
        FAILED(root_signature(device.Get(), 2, sig.empty))) {
        printf("root signatures failed\n");
        return 2;
    }
    if (cases.empty() || (cases.size() == 1 && cases[0] == "all"))
        cases = {"ue426", "ue426-additions", "ue426-basic", "ue426-add", "client-collection"};
    int failures = 0;
    for (const std::string& c : cases) {
        printf("== case %s\n", c.c_str());
        created.clear();        // the state objects of the previous case are gone, and their descriptions with them
        constexpr D3D12_STATE_OBJECT_FLAGS additions = D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
        if (c == "ue426") hr = ue426_case(device.Get(), sig, D3D12_STATE_OBJECT_FLAG_NONE, false, idle_ms);
        else if (c == "ue426-additions") hr = ue426_case(device.Get(), sig, additions, false, idle_ms);
        else if (c == "ue426-basic") hr = ue426_case(device.Get(), sig, additions, true, idle_ms);
        else if (c == "ue426-add") hr = ue426_add_case(device.Get(), sig, idle_ms);
        else if (c == "client-collection") hr = client_collection_case(device.Get(), sig);
        else {
            printf("unknown case\n");
            hr = E_INVALIDARG;
        }
        printf("== case %s: hr %08lx, device removed reason %08lx\n", c.c_str(), static_cast<unsigned long>(hr),
               static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        failures += FAILED(hr) ? 1 : 0;
    }
    return failures ? 1 : 0;
}
