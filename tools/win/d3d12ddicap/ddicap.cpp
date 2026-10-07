// SPDX-License-Identifier: MIT
// d3d12ddicap: prints the D3D12 runtime's DDI description of every ray tracing state object an application creates,
// on this PC, with no driver of ours. The program loads the WARP user-mode driver (d3d10warp.dll) before the runtime
// does, points WARP's OpenAdapter12 export at a wrapper, and the wrapper replaces pfnCreateStateObject and
// pfnAddToStateObject in the device table the runtime asks WARP to fill. Each wrapper prints the description the
// runtime built (d3d12umddi.h, D3D12DDIARG_CREATE_STATE_OBJECT_0054) and then calls WARP's own slot.
// The application side builds state objects in the shape we infer for Unreal Engine 4.26 (one COLLECTION per shader,
// then a RAYTRACING_PIPELINE of the shader and pipeline configurations, the global root signature and the
// EXISTING_COLLECTION subobjects). INFERENCE: the engine source was not available; the shape is read from trial 465,
// whose failing link had 8 subobjects. It also builds the shape of the project's own client. Console only: no window,
// nothing resident; it exits when the cases are done.
//
// Usage: d3d12ddicap.exe [case...]   cases: ue426 ue426-hitnames ue426-exportlist client-collection all (default all)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12umddi.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
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

// One collection the way UE 4.26 builds it (CreateRayTracingStateObject, D3D12_STATE_OBJECT_TYPE_COLLECTION):
// the library with renamed exports, the shader config, the hit group if any, the pipeline config, the global root
// signature, one local root signature and one association of it per name in associate.
struct Ue426Collection {
    std::vector<std::wstring> names, renames;  // export Name and ExportToRename, in pairs
    const void* code;
    size_t size;
    std::wstring hit_group;                    // empty: no hit group
    std::wstring closest, anyhit;              // the hit group's imports
    std::vector<std::wstring> associate;       // the names the local root signature is associated with
    ID3D12RootSignature* local;
};

HRESULT ue426_collection(ID3D12Device5* device, const Signatures& sig, const Ue426Collection& c, const char* what,
                         ComPtr<ID3D12StateObject>& out) {
    std::vector<D3D12_EXPORT_DESC> exports;
    for (size_t i = 0; i < c.names.size(); ++i)
        exports.push_back({c.names[i].c_str(), c.renames[i].c_str(), D3D12_EXPORT_FLAG_NONE});
    const D3D12_DXIL_LIBRARY_DESC library{{c.code, c.size}, static_cast<UINT>(exports.size()), exports.data()};
    const D3D12_RAYTRACING_SHADER_CONFIG shader_config{24, 8};
    const D3D12_HIT_GROUP_DESC group{c.hit_group.c_str(), D3D12_HIT_GROUP_TYPE_TRIANGLES,
                                     c.anyhit.empty() ? nullptr : c.anyhit.c_str(),
                                     c.closest.empty() ? nullptr : c.closest.c_str(), nullptr};
    const D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    const D3D12_GLOBAL_ROOT_SIGNATURE global{sig.global.Get()};
    const D3D12_LOCAL_ROOT_SIGNATURE local{c.local};
    std::vector<LPCWSTR> names;
    for (const std::wstring& n : c.associate) names.push_back(n.c_str());
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> associations(names.size());
    std::vector<D3D12_STATE_SUBOBJECT> s;
    s.reserve(16);
    s.push_back({D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library});
    s.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config});
    if (!c.hit_group.empty()) s.push_back({D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &group});
    s.push_back({D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config});
    s.push_back({D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global});
    const size_t local_index = s.size();
    s.push_back({D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local});
    for (size_t i = 0; i < names.size(); ++i) {
        associations[i] = {&s[local_index], 1, &names[i]};
        s.push_back({D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &associations[i]});
    }
    return api_create(device, what, D3D12_STATE_OBJECT_TYPE_COLLECTION, s, out);
}

// The UE 4.26 link: shader config, pipeline config, global root signature and the collections, nothing else
// (8 subobjects with 5 collections in trial 465). export_list: each import names its exports (UE does not, INFERENCE).
HRESULT ue426_case(ID3D12Device5* device, const Signatures& sig, bool hit_group_names, bool export_list) {
    Ue426Collection rgs{{L"RGS_00000001"}, {L"MainRGS"}, g_ue426_rgs, sizeof(g_ue426_rgs), L"", L"", L"",
                        {L"RGS_00000001"}, sig.empty.Get()};
    Ue426Collection ms{{L"MS_00000002"}, {L"MainMS"}, g_ue426_ms, sizeof(g_ue426_ms), L"", L"", L"",
                       {L"MS_00000002"}, sig.local.Get()};
    Ue426Collection hit{{L"CHS_00000003", L"AHS_00000003"}, {L"MainCHS", L"MainAHS"}, g_ue426_hit, sizeof(g_ue426_hit),
                        L"HitGroup_00000003", L"CHS_00000003", L"AHS_00000003",
                        hit_group_names ? std::vector<std::wstring>{L"HitGroup_00000003"}
                                        : std::vector<std::wstring>{L"CHS_00000003", L"AHS_00000003"},
                        sig.local.Get()};
    ComPtr<ID3D12StateObject> c_rgs, c_ms, c_hit, pipeline;
    HRESULT hr = ue426_collection(device, sig, rgs, "COLLECTION ue426 raygen", c_rgs);
    if (SUCCEEDED(hr)) hr = ue426_collection(device, sig, ms, "COLLECTION ue426 miss", c_ms);
    if (SUCCEEDED(hr)) hr = ue426_collection(device, sig, hit, "COLLECTION ue426 hit group", c_hit);
    if (FAILED(hr)) return hr;
    D3D12_EXPORT_DESC rgs_export{L"RGS_00000001", nullptr, D3D12_EXPORT_FLAG_NONE};
    D3D12_EXPORT_DESC ms_export{L"MS_00000002", nullptr, D3D12_EXPORT_FLAG_NONE};
    D3D12_EXPORT_DESC hit_export{L"HitGroup_00000003", nullptr, D3D12_EXPORT_FLAG_NONE};
    const D3D12_EXISTING_COLLECTION_DESC imports[3]{{c_rgs.Get(), export_list ? 1u : 0u, export_list ? &rgs_export : nullptr},
                                                    {c_ms.Get(), export_list ? 1u : 0u, export_list ? &ms_export : nullptr},
                                                    {c_hit.Get(), export_list ? 1u : 0u, export_list ? &hit_export : nullptr}};
    const D3D12_RAYTRACING_SHADER_CONFIG shader_config{24, 8};
    const D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    const D3D12_GLOBAL_ROOT_SIGNATURE global{sig.global.Get()};
    const std::vector<D3D12_STATE_SUBOBJECT> s{
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &imports[0]},
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &imports[1]},
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &imports[2]}};
    hr = api_create(device, "RAYTRACING_PIPELINE ue426 link", D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, s, pipeline);
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D12StateObjectProperties> properties;
        pipeline.As(&properties);
        for (LPCWSTR name : {L"RGS_00000001", L"MS_00000002", L"HitGroup_00000003"})
            printf("api GetShaderIdentifier %ls: %s\n", name, properties && properties->GetShaderIdentifier(name) ? "found" : "null");
    }
    return hr;
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

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!hook_warp()) {
        printf("could not point WARP's OpenAdapter12 at the wrapper\n");
        return 2;
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        printf("no WARP adapter\n");
        return 2;
    }
    ComPtr<ID3D12Device5> device;
    HRESULT hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    printf("api D3D12CreateDevice (WARP): hr %08lx, core table wrapped: %s\n", static_cast<unsigned long>(hr),
           core_table_seen ? "yes" : "no");
    if (FAILED(hr) || !core_table_seen) return 2;
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
    std::vector<std::string> cases;
    for (int i = 1; i < argc; ++i) cases.emplace_back(argv[i]);
    if (cases.empty() || (cases.size() == 1 && cases[0] == "all"))
        cases = {"ue426", "ue426-hitnames", "ue426-exportlist", "client-collection"};
    int failures = 0;
    for (const std::string& c : cases) {
        printf("== case %s\n", c.c_str());
        created.clear();        // the state objects of the previous case are gone, and their descriptions with them
        if (c == "ue426") hr = ue426_case(device.Get(), sig, false, false);
        else if (c == "ue426-hitnames") hr = ue426_case(device.Get(), sig, true, false);
        else if (c == "ue426-exportlist") hr = ue426_case(device.Get(), sig, false, true);
        else if (c == "client-collection") hr = client_collection_case(device.Get(), sig);
        else {
            printf("unknown case\n");
            hr = E_INVALIDARG;
        }
        printf("== case %s: hr %08lx\n", c.c_str(), static_cast<unsigned long>(hr));
        failures += FAILED(hr) ? 1 : 0;
    }
    return failures ? 1 : 0;
}
