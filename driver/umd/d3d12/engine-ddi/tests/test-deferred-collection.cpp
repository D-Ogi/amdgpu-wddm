// SPDX-License-Identifier: MIT
// Round trip: a COLLECTION whose compilation the engine must defer (BD-231). The shape is the first collection of the
// vkd3d-proton test test_raytracing_deferred_compilation (tests/d3d12_raytracing.c), which fails on unit A with
// "Failed to create collection" (engine with E_OUTOFMEMORY for every conversion failure) and with "Expected identifier
// in collection" (engine that defers the whole collection) through the installed driver: a library whose ray generation shader "main" writes u0,
// which no root signature of the collection binds; the shader configuration {4, 4}; the pipeline configuration (depth
// 1); a state object configuration with ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS; a second library that
// exports Entry3 only; a local root signature (a table of u1-u4) associated with Entry3; no global root signature.
// Then a link of the collection and a local root signature with a root UAV at u0 that every export takes.
//   E. The engine directly (ID3D12Device5, the API description of the test): the collection is created, "main" has no
//      identifier in it, Entry3 has one (native behaviour, the test's line 3779: only a shader with an unresolved
//      reference has none), and the link has identifiers for main and Entry3, Entry3's the same as in the collection.
//      This is the engine's own deferral, without the shell.
//   D. The same through the DDI, in the form that the D3D12 runtime gives a driver (measured: tools/win/d3d12ddicap
//      case vkd3d-deferred on WARP, D3D12Core.dll of the development PC): the collection's summary associates the
//      shader configuration, the pipeline configuration and the state object configuration with main (flags
//      UNRESOLVED_RESOURCE_BINDINGS), and the local root signature and the same three with Entry3; the link holds the
//      import, both local root signatures, both configurations and a state object configuration, and a summary that
//      points into its own array. The same results as E, and no device error.
//   H. The engine directly: a collection that compiles a miss shader and defers a hit group whose any hit shader
//      writes a UAV the collection does not bind (hit_group_arm).
#include "harness.h"
#include "fixture-deferred-collection.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace harness {

namespace {
constexpr LPCWSTR kMainMangled = L"\x01?main@@YAXXZ";
constexpr LPCWSTR kEntry3Mangled = L"\x01?Entry3@@YAXXZ";

// kind 0: one root UAV (u0 space0); kind 1: one descriptor table of the UAVs u1-u4. Both local.
D3D12DDI_ROOT_SIGNATURE_0013 local_desc(int kind, D3D12DDI_ROOT_PARAMETER_0013& parameter,
                                        D3D12DDI_DESCRIPTOR_RANGE_0013& range) {
    parameter = D3D12DDI_ROOT_PARAMETER_0013{};
    parameter.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    if (kind == 0) {
        parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    } else {
        range = D3D12DDI_DESCRIPTOR_RANGE_0013{};
        range.RangeType = D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 4;
        range.BaseShaderRegister = 1;
        parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range};
    }
    return D3D12DDI_ROOT_SIGNATURE_0013{1, &parameter, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
}

// A state object in fresh private storage; the create's result.
HRESULT create_object(Env& env, Device& device, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& args, void** storage,
                      int* rt) {
    *storage = env.storage.alloc(env.core.pfnCalcPrivateStateObjectSize(device.h(), &args));
    if (!*storage) return E_OUTOFMEMORY;
    return env.core.pfnCreateStateObject(device.h(), &args, D3D12DDI_HSTATEOBJECT_0054{*storage},
                                         D3D12DDI_HRTSTATEOBJECT_0054{rt});
}

template <typename T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// The shader identifier of an export, copied (all zero and false when there is none).
bool identifier(ID3D12StateObject* object, LPCWSTR name, uint8_t (&out)[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]) {
    std::memset(out, 0, sizeof(out));
    ID3D12StateObjectProperties* properties = nullptr;
    if (!object ||
        FAILED(object->QueryInterface(__uuidof(ID3D12StateObjectProperties), reinterpret_cast<void**>(&properties))))
        return false;
    const void* id = properties->GetShaderIdentifier(name);
    if (id) std::memcpy(out, id, sizeof(out));
    properties->Release();
    return id != nullptr;
}

bool has_identifier(ID3D12StateObject* object, LPCWSTR name) {
    uint8_t id[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
    return identifier(object, name, id);
}

bool ddi_identifier(Env& env, void* object, LPCWSTR name, uint8_t (&out)[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]) {
    std::memset(out, 0, sizeof(out));
    const void* id = env.core.pfnGetShaderIdentifier(D3D12DDI_HSTATEOBJECT_0054{object}, name);
    if (id) std::memcpy(out, id, sizeof(out));
    return id != nullptr;
}

// E: the engine directly.
void engine_arm(Env& env) {
    ID3D12Device5* device = nullptr;
    if (FAILED(env.engine->QueryInterface(__uuidof(ID3D12Device5), reinterpret_cast<void**>(&device)))) {
        check(false, "deferred collection E: ID3D12Device5");
        return;
    }
    ID3D12RootSignature* rs[2]{};
    HRESULT hr = S_OK;
    for (int kind = 0; kind < 2 && SUCCEEDED(hr); ++kind) {
        D3D12DDI_ROOT_PARAMETER_0013 parameter;
        D3D12DDI_DESCRIPTOR_RANGE_0013 range;
        const D3D12DDI_ROOT_SIGNATURE_0013 desc = local_desc(kind, parameter, range);
        std::vector<uint8_t> bytes;
        hr = engine_ddi::serialize_root_signature(&desc, bytes);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, bytes.data(), bytes.size(), __uuidof(ID3D12RootSignature),
                                             reinterpret_cast<void**>(&rs[kind]));
    }
    checkf(hr == S_OK, "deferred collection E: the two local root signatures (hr %08lx)", static_cast<unsigned long>(hr));
    ID3D12StateObject *collection = nullptr, *link = nullptr;
    uint8_t entry3_in_collection[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]{};
    if (SUCCEEDED(hr)) {
        D3D12_EXPORT_DESC entry3{L"Entry3", nullptr, D3D12_EXPORT_FLAG_NONE};
        const D3D12_DXIL_LIBRARY_DESC raygen{{g_fixture_deferred_raygen, sizeof(g_fixture_deferred_raygen)}, 0, nullptr};
        const D3D12_DXIL_LIBRARY_DESC bindings{{g_fixture_deferred_bindings, sizeof(g_fixture_deferred_bindings)}, 1,
                                               &entry3};
        const D3D12_RAYTRACING_SHADER_CONFIG shader_config{4, 4};
        const D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
        const D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS};
        const D3D12_LOCAL_ROOT_SIGNATURE local_alt{rs[1]};
        D3D12_STATE_SUBOBJECT s[7] = {
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &raygen},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
            {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &bindings},
            {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local_alt},
            {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, nullptr}};
        LPCWSTR entry3_name = L"Entry3";
        const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&s[5], 1, &entry3_name};
        s[6].pDesc = &association;
        const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, 7, s};
        hr = device->CreateStateObject(&desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&collection));
        checkf(hr == S_OK && collection && !has_identifier(collection, L"main"),
               "deferred collection E: the engine creates the collection and defers main (hr %08lx, main %s)",
               static_cast<unsigned long>(hr), collection && has_identifier(collection, L"main") ? "found" : "null");
        // Native behaviour (the test's line 3779; DXR spec, GetShaderIdentifier): Entry3 has no unresolved reference,
        // so the collection compiles it and gives its identifier.
        const bool entry3_found = identifier(collection, L"Entry3", entry3_in_collection);
        checkf(entry3_found, "deferred collection E: the collection has an identifier for Entry3, which it can compile");
    }
    if (collection) {
        const D3D12_EXISTING_COLLECTION_DESC existing{collection, 0, nullptr};
        const D3D12_LOCAL_ROOT_SIGNATURE local{rs[0]};
        const D3D12_STATE_SUBOBJECT p[2] = {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
                                            {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local}};
        const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 2, p};
        hr = device->CreateStateObject(&desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&link));
        uint8_t entry3_in_link[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
        const bool ids = hr == S_OK && link && has_identifier(link, L"main") && identifier(link, L"Entry3", entry3_in_link);
        checkf(ids, "deferred collection E: the link has identifiers for main and Entry3 (hr %08lx)",
               static_cast<unsigned long>(hr));
        if (ids)
            checkf(!std::memcmp(entry3_in_link, entry3_in_collection, sizeof(entry3_in_link)),
                   "deferred collection E: Entry3 has the same identifier in the link as in the collection");
    }
    release(link);
    release(collection);
    release(rs[0]);
    release(rs[1]);
    release(device);
}

// H: the engine directly, a hit group that the collection cannot compile beside a miss shader that it can. The
// collection: the HITGROUP library (MissResolved, ClosestResolved, AnyUnresolved, which writes u0), a hit group
// "HitGroup" of ClosestResolved and AnyUnresolved, the shader configuration {4, 8}, the pipeline configuration and
// ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS. Expected: the collection is created, MissResolved has an
// identifier, HitGroup has none. The link: the collection, the RAYGEN library (main) and a local root signature with a
// root UAV at u0 for every export of the link. Expected: identifiers for main, MissResolved (the same as in the
// collection) and HitGroup.
void hit_group_arm(Env& env) {
    ID3D12Device5* device = nullptr;
    if (FAILED(env.engine->QueryInterface(__uuidof(ID3D12Device5), reinterpret_cast<void**>(&device)))) {
        check(false, "deferred hit group H: ID3D12Device5");
        return;
    }
    ID3D12RootSignature* rs = nullptr;
    D3D12DDI_ROOT_PARAMETER_0013 parameter;
    D3D12DDI_DESCRIPTOR_RANGE_0013 range;
    const D3D12DDI_ROOT_SIGNATURE_0013 rs_desc = local_desc(0, parameter, range);
    std::vector<uint8_t> bytes;
    HRESULT hr = engine_ddi::serialize_root_signature(&rs_desc, bytes);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, bytes.data(), bytes.size(), __uuidof(ID3D12RootSignature),
                                         reinterpret_cast<void**>(&rs));
    checkf(hr == S_OK, "deferred hit group H: the local root signature (hr %08lx)", static_cast<unsigned long>(hr));
    ID3D12StateObject *collection = nullptr, *link = nullptr;
    uint8_t miss_in_collection[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]{};
    if (SUCCEEDED(hr)) {
        const D3D12_DXIL_LIBRARY_DESC library{{g_fixture_deferred_hitgroup, sizeof(g_fixture_deferred_hitgroup)}, 0,
                                              nullptr};
        const D3D12_HIT_GROUP_DESC hit_group{L"HitGroup", D3D12_HIT_GROUP_TYPE_TRIANGLES, L"AnyUnresolved",
                                             L"ClosestResolved", nullptr};
        const D3D12_RAYTRACING_SHADER_CONFIG shader_config{4, 8};
        const D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
        const D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS};
        const D3D12_STATE_SUBOBJECT s[5] = {
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
            {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_group},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
            {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config}};
        const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, 5, s};
        hr = device->CreateStateObject(&desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&collection));
        const bool miss = hr == S_OK && identifier(collection, L"MissResolved", miss_in_collection);
        const bool group = hr == S_OK && has_identifier(collection, L"HitGroup");
        checkf(hr == S_OK && miss && !group,
               "deferred hit group H: the collection compiles MissResolved and defers HitGroup (hr %08lx, "
               "MissResolved %s, HitGroup %s)",
               static_cast<unsigned long>(hr), miss ? "found" : "null", group ? "found" : "null");
        if (hr != S_OK) release(collection);
    }
    if (collection) {
        const D3D12_EXISTING_COLLECTION_DESC existing{collection, 0, nullptr};
        const D3D12_DXIL_LIBRARY_DESC raygen{{g_fixture_deferred_raygen, sizeof(g_fixture_deferred_raygen)}, 0, nullptr};
        const D3D12_LOCAL_ROOT_SIGNATURE local{rs};
        const D3D12_STATE_SUBOBJECT p[3] = {{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
                                            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &raygen},
                                            {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local}};
        const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 3, p};
        hr = device->CreateStateObject(&desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&link));
        uint8_t miss_in_link[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
        const bool ids = hr == S_OK && has_identifier(link, L"main") && has_identifier(link, L"HitGroup") &&
                         identifier(link, L"MissResolved", miss_in_link);
        checkf(ids, "deferred hit group H: the link has identifiers for main, MissResolved and HitGroup (hr %08lx)",
               static_cast<unsigned long>(hr));
        if (ids)
            checkf(!std::memcmp(miss_in_link, miss_in_collection, sizeof(miss_in_link)),
                   "deferred hit group H: MissResolved has the same identifier in the link as in the collection");
    }
    release(link);
    release(collection);
    release(rs);
    release(device);
}

// D: through the DDI, in the runtime's form (the file comment).
void ddi_arm(Env& env) {
    Device device;
    HRESULT hr = open_device(env, device);
    checkf(hr == S_OK && device.context, "deferred collection D: device context (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    void* rs[2]{};
    for (int kind = 0; kind < 2 && SUCCEEDED(hr); ++kind) {
        D3D12DDI_ROOT_PARAMETER_0013 parameter;
        D3D12DDI_DESCRIPTOR_RANGE_0013 range;
        const D3D12DDI_ROOT_SIGNATURE_0013 desc = local_desc(kind, parameter, range);
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 args{};
        args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
        args.pRootSignature_1_1 = &desc;
        rs[kind] = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &args));
        hr = rs[kind] ? env.core.pfnCreateRootSignature(device.h(), &args, D3D12DDI_HROOTSIGNATURE{rs[kind]})
                      : E_OUTOFMEMORY;
    }
    DdiShader raygen_code, bindings_code;
    const bool stripped = ddi_form(g_fixture_deferred_raygen, sizeof(g_fixture_deferred_raygen), raygen_code) &&
                          ddi_form(g_fixture_deferred_bindings, sizeof(g_fixture_deferred_bindings), bindings_code);
    checkf(hr == S_OK && stripped, "deferred collection D: the two local root signatures and the DDI form of both "
           "libraries (hr %08lx)", static_cast<unsigned long>(hr));
    void* collection = nullptr;
    void* link = nullptr;
    int collection_rt = 0, link_rt = 0;
    // The collection, in WARP's measured order.
    D3D12DDI_EXPORT_DESC_0054 entry3{L"Entry3", nullptr, D3D12DDI_EXPORT_FLAG_NONE};
    const D3D12DDI_DXIL_LIBRARY_DESC_0054 raygen{raygen_code.code.data(), 0, nullptr};
    const D3D12DDI_DXIL_LIBRARY_DESC_0054 bindings{bindings_code.code.data(), 1, &entry3};
    const D3D12DDI_RAYTRACING_SHADER_CONFIG_0054 shader_config{4, 4};
    const D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075 pipeline_config{1, D3D12DDI_RAYTRACING_PIPELINE_FLAG_NONE};
    const D3D12DDI_STATE_OBJECT_CONFIG_0054 config{
        D3D12DDI_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS};
    const D3D12DDI_LOCAL_ROOT_SIGNATURE_0054 local_alt{D3D12DDI_HROOTSIGNATURE{rs[1]}};
    D3D12DDI_FUNCTION_SUMMARY_NODE_0054 nodes[2]{};
    D3D12DDI_FUNCTION_SUMMARY_0054 summary{2, nodes, D3D12DDI_EXPORT_SUMMARY_FLAG_UNRESOLVED_RESOURCE_BINDINGS};
    const D3D12DDI_STATE_SUBOBJECT_0054 s[7] = {
        {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &raygen},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &bindings},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local_alt},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &summary}};
    const D3D12DDI_STATE_SUBOBJECT_0054* main_associated[3] = {&s[2], &s[3], &s[4]};
    const D3D12DDI_STATE_SUBOBJECT_0054* entry3_associated[4] = {&s[5], &s[2], &s[3], &s[4]};
    nodes[0] = {L"main", kMainMangled, 3, main_associated, D3D12DDI_EXPORT_SUMMARY_FLAG_UNRESOLVED_RESOURCE_BINDINGS};
    nodes[1] = {L"Entry3", kEntry3Mangled, 4, entry3_associated, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    const D3D12DDIARG_CREATE_STATE_OBJECT_0054 collection_args{D3D12DDI_STATE_OBJECT_TYPE_COLLECTION, 7, s};
    uint8_t entry3_in_collection[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]{};
    if (hr == S_OK && stripped) {
        hr = create_object(env, device, collection_args, &collection, &collection_rt);
        const bool main_id =
            hr == S_OK && env.core.pfnGetShaderIdentifier(D3D12DDI_HSTATEOBJECT_0054{collection}, L"main") != nullptr;
        checkf(hr == S_OK && !main_id && !device.shell.device_errors,
               "deferred collection D: CreateStateObject of the collection in the runtime's form, main deferred (hr "
               "%08lx, main %s, %u device errors)",
               static_cast<unsigned long>(hr), main_id ? "found" : "null", device.shell.device_errors);
        if (hr == S_OK)
            checkf(ddi_identifier(env, collection, L"Entry3", entry3_in_collection),
                   "deferred collection D: the collection has an identifier for Entry3, which it can compile");
        if (hr != S_OK) collection = nullptr;
    }
    if (collection) {
        // The link, in WARP's measured order: the import, rs, rs_alt, both configurations, the state object
        // configuration, and the summary into this array.
        const D3D12DDI_EXISTING_COLLECTION_DESC_0054 existing{D3D12DDI_HSTATEOBJECT_0054{collection}, 0, nullptr};
        const D3D12DDI_LOCAL_ROOT_SIGNATURE_0054 local{D3D12DDI_HROOTSIGNATURE{rs[0]}};
        D3D12DDI_FUNCTION_SUMMARY_NODE_0054 link_nodes[2]{};
        D3D12DDI_FUNCTION_SUMMARY_0054 link_summary{2, link_nodes, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
        const D3D12DDI_STATE_SUBOBJECT_0054 p[7] = {
            {D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
            {D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local},
            {D3D12DDI_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local_alt},
            {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
            {D3D12DDI_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
            {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
            {D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &link_summary}};
        const D3D12DDI_STATE_SUBOBJECT_0054* link_main[4] = {&p[3], &p[4], &p[5], &p[1]};
        const D3D12DDI_STATE_SUBOBJECT_0054* link_entry3[4] = {&p[2], &p[3], &p[4], &p[5]};
        link_nodes[0] = {L"main", kMainMangled, 4, link_main, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
        link_nodes[1] = {L"Entry3", kEntry3Mangled, 4, link_entry3, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
        const D3D12DDIARG_CREATE_STATE_OBJECT_0054 link_args{D3D12DDI_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 7, p};
        hr = create_object(env, device, link_args, &link, &link_rt);
        const D3D12DDI_HSTATEOBJECT_0054 h{link};
        uint8_t entry3_in_link[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES];
        const bool ids = hr == S_OK && env.core.pfnGetShaderIdentifier(h, L"main") &&
                         ddi_identifier(env, link, L"Entry3", entry3_in_link);
        checkf(ids && !device.shell.device_errors,
               "deferred collection D: the link in the runtime's form has identifiers for main and Entry3 (hr %08lx, "
               "%u device errors)",
               static_cast<unsigned long>(hr), device.shell.device_errors);
        if (ids)
            checkf(!std::memcmp(entry3_in_link, entry3_in_collection, sizeof(entry3_in_link)),
                   "deferred collection D: Entry3 has the same identifier in the link as in the collection");
        if (hr != S_OK) link = nullptr;
    }
    if (link) env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{link});
    if (collection) env.core.pfnDestroyStateObject(device.h(), D3D12DDI_HSTATEOBJECT_0054{collection});
    for (void* r : rs)
        if (r) env.core.pfnDestroyRootSignature(device.h(), D3D12DDI_HROOTSIGNATURE{r});
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0, "deferred collection D: destroy_device_context with no live object (hr %08lx, live %u)",
           static_cast<unsigned long>(hr), live);
}
} // namespace

void test_raytracing_deferred_collection(Env& env) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    const HRESULT hr = env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (hr != S_OK || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        std::printf("SKIP  deferred collection: the engine reports raytracing tier %d on this GPU (hr %08lx)\n",
                    static_cast<int>(options5.RaytracingTier), static_cast<unsigned long>(hr));
        return;
    }
    engine_arm(env);
    hit_group_arm(env);
    ddi_arm(env);
}

} // namespace harness
