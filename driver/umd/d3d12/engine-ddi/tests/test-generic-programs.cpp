// SPDX-License-Identifier: MIT
#include "test-engine-limits.h"
#include "modern-core-harness.h"
#include "ddi-modern-list.h"
#include "replay.h"
#include "fixture-generic-vs.h"
#include "fixture-generic-ps.h"
#include "fixture-generic-added.h"
#include "shader-container/shader-container.h"
#include "shader-container/dxil-metadata.h"
#include <algorithm>

namespace harness {
#if defined(D3D12DDI_BUILD_VERSION_0119)
namespace {
std::vector<UINT> generic_payload(const BYTE* bytes, size_t size, bool raw) {
    std::vector<UINT> words((size + 3) / 4); std::memcpy(words.data(), bytes, size);
    if (!raw) return words;
    if (words.size() < 8) return {};
    for (UINT i = 0; i != words[7]; ++i) {
        const UINT offset = words[8 + i] / 4;
        if (words[offset] == 0x4c495844u) return std::vector<UINT>(words.begin() + offset + 2, words.begin() + offset + 2 + words[offset + 1] / 4);
    }
    return {};
}
struct GenericRecords {
    Env& env; Device& device;
    void* root = nullptr;
    std::vector<D3D12DDI_HSTATEOBJECT_0054> states;
    ~GenericRecords() {
        for (auto h : states) if (h.pDrvPrivate) env.core.pfnDestroyStateObject(device.h(), h);
        if (root) env.core.pfnDestroyRootSignature(device.h(), D3D12DDI_HROOTSIGNATURE{root});
    }
    void destroy(size_t index) { env.core.pfnDestroyStateObject(device.h(), states[index]); states[index] = {}; }
};
}
void test_generic_programs(Env& env, Device& device) {
    using namespace engine_limits;
    const UINT before_errors = device.shell.list_errors;
    D3D12DDI_DEVICE_FUNCS_CORE_0119 core{};
    check(modern_core_table(env, core), "generic programs: modern CORE table");
    const auto modern = engine_ddi::modern_list_entries();
    check(modern.pfnSetProgram && core.pfnGetProgramIdentifier, "generic programs: production callbacks present");
    if (!modern.pfnSetProgram || !core.pfnGetProgramIdentifier) return;
    Objects objects;
    GenericRecords records{env, device};
    D3D12DDI_ROOT_PARAMETER_0013 parameter{};
    parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    parameter.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    const D3D12DDI_ROOT_SIGNATURE_0013 root_desc{1, &parameter, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 root_args{};
    root_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1; root_args.pRootSignature_1_1 = &root_desc;
    records.root = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &root_args));
    HRESULT hr = records.root ? env.core.pfnCreateRootSignature(device.h(), &root_args, D3D12DDI_HROOTSIGNATURE{records.root}) : E_OUTOFMEMORY;
    check(hr == S_OK, "generic programs: root signature"); if (FAILED(hr)) return;
    auto* root_record = static_cast<engine_ddi::RootSignatureRecord*>(records.root);
    auto* engine_root = static_cast<ID3D12RootSignature*>(root_record->h.engine);
    D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054 global{D3D12DDI_HROOTSIGNATURE{records.root}};
    D3D12DDI_STATE_OBJECT_CONFIG_0054 config{D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12DDI_RASTERIZER_DESC_0102 raster{};
    raster.FillMode = D3D12DDI_FILL_MODE_SOLID; raster.CullMode = D3D12DDI_CULL_MODE_NONE; raster.DepthClipEnable = TRUE;
    D3D12DDI_DEPTH_STENCIL_DESC_0095 depth{};
    depth.DepthFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS;
    depth.FrontFace = depth.BackFace = {D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_COMPARISON_FUNC_ALWAYS};
    auto vs = generic_payload(g_generic_vs, sizeof(g_generic_vs), true);
    auto ps = generic_payload(g_generic_ps, sizeof(g_generic_ps), false);
    engine_ddi::shader_container::Container reconstructed;
    engine_ddi::shader_container::dxil::Signatures signature;
    const auto reconstructed_result = engine_ddi::shader_container::BuildGenericContainer(vs.data(), vs.size(), &reconstructed, &signature);
    engine_ddi::shader_container::StreamOutputElement so_semantic;
    const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY so_entry{0, 0, 0, 15};
    const auto so_result = engine_ddi::shader_container::StreamOutputSemantic(reconstructed, so_entry, &so_semantic);
    check(reconstructed_result && so_result && so_semantic.semantic.name == "SV_Position" && so_semantic.componentCount == 4,
          "generic programs: raw DXIL reconstructs stream-output semantic and four components");
    D3D12DDI_EXPORT_DESC_0054 vs_export{L"collection_vertex", L"*", D3D12DDI_EXPORT_FLAG_NONE};
    D3D12DDI_EXPORT_DESC_0054 ps_export{L"collection_pixel", L"*", D3D12DDI_EXPORT_FLAG_NONE};
    D3D12DDI_DXIL_LIBRARY_DESC_0054 vs_lib{vs.data(), 1, &vs_export}, ps_lib{ps.data(), 1, &ps_export};
    LPCWSTR exports[] = {L"vertex", L"pixel"};
    D3D12DDI_STATE_SUBOBJECT_0054 subobjects[] = {
        {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &vs_lib}, {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ps_lib},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &topology},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER, &raster}, {D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, &depth},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM, nullptr},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_SERIALIZED_ROOT_SIGNATURE, nullptr},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_SERIALIZED_ROOT_SIGNATURE, nullptr}};
    const D3D12DDI_STATE_SUBOBJECT_0054* states[] = {&subobjects[4], &subobjects[5], &subobjects[6]};
    D3D12DDI_GENERIC_PROGRAM_DESC_0108 program{L"first", 2, exports, 3, states}; subobjects[7].pDesc = &program;
    D3D12DDI_ROOT_SIGNATURE_0100 serialized_root_desc{1, &parameter, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    D3D12DDI_GLOBAL_SERIALIZED_ROOT_SIGNATURE_0114 serialized_roots[2]{};
    for (auto& s : serialized_roots) { s.RootSignature.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_2; s.RootSignature.pRootSignature_1_2 = &serialized_root_desc; }
    subobjects[8].pDesc = &serialized_roots[0]; subobjects[9].pDesc = &serialized_roots[1];
    const D3D12DDI_STATE_SUBOBJECT_0054* roots_a[] = {&subobjects[8], nullptr}, *roots_b[] = {&subobjects[9], nullptr};
    const D3D12DDI_FUNCTION_SUMMARY_NODE_0054 root_nodes[] = {
        {L"vertex", L"vertex", 2, roots_a, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE},
        {L"pixel", L"pixel", 2, roots_b, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE}};
    const D3D12DDI_FUNCTION_SUMMARY_0054 root_summary{2, root_nodes, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE};
    D3D12DDIARG_CREATE_STATE_OBJECT_0054 desc{D3D12DDI_STATE_OBJECT_TYPE_EXECUTABLE, 10, subobjects};
    auto create_state = [&](const D3D12DDIARG_CREATE_STATE_OBJECT_0054& d) {
        D3D12DDI_HSTATEOBJECT_0054 h{env.storage.alloc(core.pfnCalcPrivateStateObjectSize(device.h(), &d))};
        hr = h.pDrvPrivate ? core.pfnCreateStateObject(device.h(), &d, h, D3D12DDI_HRTSTATEOBJECT_0054{}) : E_OUTOFMEMORY;
        records.states.push_back(h); return h;
    };
    constexpr auto unresolved_bindings = D3D12DDI_EXPORT_SUMMARY_FLAG_UNRESOLVED_RESOURCE_BINDINGS;
    D3D12DDI_FUNCTION_SUMMARY_NODE_0054 collection_nodes[] = {
        {L"collection_vertex", L"collection_vertex", 0, nullptr, D3D12DDI_EXPORT_SUMMARY_FLAG_NONE},
        {L"collection_pixel", L"collection_pixel", 0, nullptr, unresolved_bindings}};
    const D3D12DDI_FUNCTION_SUMMARY_0054 collection_summary{2, collection_nodes, unresolved_bindings};
    D3D12DDI_STATE_OBJECT_CONFIG_0054 collection_config{D3D12DDI_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS};
    D3D12DDI_STATE_SUBOBJECT_0054 collection_objects[] = {{D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &collection_summary},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &vs_lib}, {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ps_lib},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &collection_config}};
    const D3D12DDI_STATE_SUBOBJECT_0054* collection_associations[] = {&collection_objects[3]};
    for (auto& node : collection_nodes) { node.NumAssociatedSubobjects = 1; node.ppAssociatedSubobjects = collection_associations; }
    const D3D12DDIARG_CREATE_STATE_OBJECT_0054 collection_desc{D3D12DDI_STATE_OBJECT_TYPE_COLLECTION, 4, collection_objects};
    // A valid-looking config outside the submitted array is not a valid association.
    const D3D12DDI_STATE_SUBOBJECT_0054 foreign_config{
        D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &collection_config};
    collection_associations[0] = &foreign_config;
    D3D12DDI_HSTATEOBJECT_0054 rejected{
        env.storage.alloc(core.pfnCalcPrivateStateObjectSize(device.h(), &collection_desc))};
    check(rejected.pDrvPrivate != nullptr, "generic programs: negative association allocation");
    if (!rejected.pDrvPrivate) return;
    hr = core.pfnCreateStateObject(device.h(), &collection_desc, rejected, {});
    // The DDI admits a restricted failure set; invalid descriptions map to E_OUTOFMEMORY.
    check(hr == E_OUTOFMEMORY, "generic programs: foreign config association rejected");
    env.core.pfnDestroyStateObject(device.h(), rejected);
    collection_associations[0] = &collection_objects[3];
    auto collection = create_state(collection_desc);
    checkf(hr == S_OK, "generic programs: raw DXIL/container collection with unresolved root bindings (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    roots_a[1] = roots_b[1] = &collection_objects[3];
    std::fill(vs.begin(), vs.end(), 0xddddddddu); std::vector<UINT>().swap(vs);
    std::fill(ps.begin(), ps.end(), 0xddddddddu); std::vector<UINT>().swap(ps);
    D3D12DDI_EXPORT_DESC_0054 imports[] = {{L"vertex", L"collection_vertex", D3D12DDI_EXPORT_FLAG_NONE}, {L"pixel", L"collection_pixel", D3D12DDI_EXPORT_FLAG_NONE}};
    const D3D12DDI_EXISTING_COLLECTION_DESC_0054 imported{collection, 2, imports};
    subobjects[2] = {D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &imported};
    subobjects[3] = {D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY, &root_summary};
    auto parent = create_state(desc);
    checkf(hr == S_OK, "generic programs: renamed collection exports with separate equivalent serialized roots (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    records.destroy(0);
    const auto parent_id = core.pfnGetProgramIdentifier(parent, L"first");
    check(parent_id.OpaqueData[0] != 0, "generic programs: named identifier after collection destroy");
    // Shaders are inherited by name. Graphics-state definitions must be local to the addition (WorkGraphs:2923).
    auto added = generic_payload(g_generic_added, sizeof(g_generic_added), true);
    D3D12DDI_EXPORT_DESC_0054 added_export{L"pixel_added", L"*", D3D12DDI_EXPORT_FLAG_NONE};
    D3D12DDI_DXIL_LIBRARY_DESC_0054 added_lib{added.data(), 1, &added_export};
    LPCWSTR added_exports[] = {L"vertex", L"pixel_added"};
    D3D12DDI_STATE_SUBOBJECT_0054 additions[] = {{D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &added_lib},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &topology}, {D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER, &raster},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, &depth}, {D3D12DDI_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM, nullptr},
        {D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config}};
    const D3D12DDI_STATE_SUBOBJECT_0054* added_states[] = {&additions[2], &additions[3], &additions[4]};
    D3D12DDI_GENERIC_PROGRAM_DESC_0108 added_program{L"second", 2, added_exports, 3, added_states}; additions[5].pDesc = &added_program;
    D3D12DDIARG_ADD_TO_STATE_OBJECT_0072 addition{D3D12DDI_STATE_OBJECT_TYPE_EXECUTABLE, 7, additions, parent};
    D3D12DDI_HSTATEOBJECT_0054 child{env.storage.alloc(core.pfnCalcPrivateAddToStateObjectSize(device.h(), &addition))};
    hr = child.pDrvPrivate ? core.pfnAddToStateObject(device.h(), &addition, child, {}) : E_OUTOFMEMORY;
    records.states.push_back(child);
    checkf(hr == S_OK, "generic programs: AddToStateObject inherits exports and pipelines (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    std::fill(added.begin(), added.end(), 0xddddddddu); std::vector<UINT>().swap(added);
    records.destroy(1);
    const auto first_id = core.pfnGetProgramIdentifier(child, L"first"), second_id = core.pfnGetProgramIdentifier(child, L"second");
    check(first_id.OpaqueData[0] == parent_id.OpaqueData[0] && second_id.OpaqueData[0] && second_id.OpaqueData[0] != first_id.OpaqueData[0],
          "generic programs: inherited identifier survives parent destroy, new program differs");
    ID3D12Resource *output = nullptr, *readback = nullptr;
    hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_DEFAULT, 4096, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_READBACK, 4096, D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    ID3D12PipelineState* plain[2]{};
    for (UINT i = 0; i != 2 && SUCCEEDED(hr); ++i) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = engine_root;
        p.VS = {g_generic_vs, sizeof(g_generic_vs)};
        p.PS = i ? D3D12_SHADER_BYTECODE{g_generic_added, sizeof(g_generic_added)} : D3D12_SHADER_BYTECODE{g_generic_ps, sizeof(g_generic_ps)};
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
        p.SampleMask = UINT_MAX; p.SampleDesc.Count = 1; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        hr = env.engine->CreateGraphicsPipelineState(&p, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&plain[i])); objects.hold(plain[i]);
    }
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    if (SUCCEEDED(hr)) hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    Recording recording;
    if (SUCCEEDED(hr)) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, recording);
    checkf(hr == S_OK, "generic programs: readback and command setup (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) { if (queue) engine_ddi::destroy_engine_queue(queue); return; }
    auto* list_record = static_cast<engine_ddi::CommandListRecord*>(recording.list);
    auto* engine_list = list_record->list();
    auto& table = env.lists[1]; const auto list = recording.hlist();
    const D3D12_VIEWPORT viewport{0, 0, 16, 16, 0, 1}; const D3D12_RECT scissor{0, 0, 16, 16};
    HANDLE setup_entered = nullptr, setup_release = nullptr;
    std::atomic<DWORD> setup_wait{WAIT_OBJECT_0};
    if (device.context->replay && engine_ddi::replay_ring(device.context->replay)) {
        setup_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        setup_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(setup_entered && setup_release, "generic programs: deferred setup ordering events");
        if (setup_entered && setup_release) {
            // Force default state to remain pending while the fixture submits its actual setup. This makes
            // an accidental direct engine setup reproducibly lose its viewport, rather than racing the worker.
            auto* wait_result = &setup_wait;
            engine_ddi::record(list_record, [setup_entered, setup_release, wait_result](ID3D12GraphicsCommandList*) {
                SetEvent(setup_entered);
                wait_result->store(WaitForSingleObject(setup_release, 5000), std::memory_order_release);
            });
            table.pfnRsSetViewports(list, 0, nullptr);
            table.pfnRsSetScissorRects(list, 0, nullptr);
            table.pfnIaSetTopology(list, D3D12DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
            engine_ddi::wake_for(device.context->replay, list_record->replay_tail.load(std::memory_order_relaxed));
            check(WaitForSingleObject(setup_entered, 5000) == WAIT_OBJECT_0,
                  "generic programs: worker held before pending default viewport/scissor/topology");
        }
    }
    const auto configure = [viewport, scissor, engine_root](ID3D12GraphicsCommandList* engine) {
        engine->RSSetViewports(1, &viewport); engine->RSSetScissorRects(1, &scissor);
        engine->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        engine->SetGraphicsRootSignature(engine_root);
    };
    // open_recording queues runtime defaults. Setup belongs on that same ordered timeline.
    engine_ddi::record(list_record, configure);
    if (setup_release) SetEvent(setup_release);
    engine_ddi::drain_list(list_record, engine_ddi::Drain::Direct);
    check(setup_wait.load(std::memory_order_acquire) == WAIT_OBJECT_0,
          "generic programs: deferred setup gate released without timeout");
    if (setup_entered) CloseHandle(setup_entered);
    if (setup_release) CloseHandle(setup_release);
    for (UINT arm = 0; arm != 4; ++arm) {
        engine_ddi::drain_list(static_cast<engine_ddi::CommandListRecord*>(recording.list), engine_ddi::Drain::Direct);
        engine_list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + arm * 1024);
        if (!(arm & 1)) engine_list->SetPipelineState(plain[arm / 2]);
        else { D3D12DDI_SET_PROGRAM_DESC_0108 set{}; set.Type = D3D12DDI_PROGRAM_TYPE_GENERIC_PIPELINE_0108;
            set.GenericPipeline.ProgramIdentifier = arm == 1 ? first_id : second_id; modern.pfnSetProgram(list, &set); }
        table.pfnDrawInstanced(list, 3, 1, 0, 0);
    }
    engine_ddi::drain_list(static_cast<engine_ddi::CommandListRecord*>(recording.list), engine_ddi::Drain::Direct);
    transition(engine_list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    engine_list->CopyBufferRegion(readback, 0, output, 0, 4096);
    table.pfnCloseCommandList(list);
    hr = engine_ddi::execute_command_lists(queue, 1, &list);
    const bool complete = hr == S_OK && wait_queue_idle(env, queue, "generic programs");
    check(complete && device.shell.list_errors == before_errors, "generic programs: plain and generic draws complete without list errors");
    void* mapped = nullptr;
    if (complete) hr = readback->Map(0, nullptr, &mapped);
    if (complete && SUCCEEDED(hr)) {
        save_readback_artifact("generic-programs", mapped, 16, 64, 64, DXGI_FORMAT_R32_UINT);
        const auto* words = static_cast<const UINT*>(mapped); UINT bad = 0;
        for (UINT i = 0; i != 256; ++i) bad += words[i] != 0x10000000u + i || words[256 + i] != words[i] ||
            words[512 + i] != 0x20000000u + i || words[768 + i] != words[512 + i];
        checkf(!bad, "generic programs: two programs match plain PSOs texel-for-texel (%u mismatches)", bad);
        readback->Unmap(0, nullptr);
    } else check(false, "generic programs: readback Map");
    destroy_recording(env, device, recording);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired, "generic programs: queue retired");
    Recording validation;
    hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, validation);
    check(hr == S_OK, "generic programs: validation recorder");
    if (FAILED(hr)) return;
    D3D12DDI_SET_PROGRAM_DESC_0108 set{}; set.Type = D3D12DDI_PROGRAM_TYPE_GENERIC_PIPELINE_0108;
    set.GenericPipeline.ProgramIdentifier = first_id;
    set.GenericPipeline.ProgramIdentifier.OpaqueData[0] = UINT64_MAX;
    UINT errors = device.shell.list_errors;
    modern.pfnSetProgram(validation.hlist(), &set);
    check(device.shell.list_errors == errors + 1 && device.shell.last_list_error == E_INVALIDARG,
          "generic programs: arbitrary identifier refused without dereferencing a pointer");
    Device foreign;
    hr = open_device(env, foreign);
    if (SUCCEEDED(hr)) {
        Recording foreign_rec;
        hr = open_recording(env, foreign, D3D12DDI_COMMAND_QUEUE_FLAG_3D, foreign_rec);
        if (SUCCEEDED(hr)) {
            set.GenericPipeline.ProgramIdentifier = first_id;
            modern.pfnSetProgram(foreign_rec.hlist(), &set);
            check(foreign.shell.list_errors == 1 && foreign.shell.last_list_error == E_INVALIDARG,
                  "generic programs: cross-device identifier refused");
            destroy_recording(env, foreign, foreign_rec);
        } else check(false, "generic programs: cross-device recorder");
        uint32_t live = 0;
        hr = engine_ddi::destroy_device_context(foreign.context, &live);
        check(hr == S_OK && !live, "generic programs: foreign context cleanup");
        foreign.context = foreign.sd.context = nullptr;
    } else check(false, "generic programs: foreign context");
    set.GenericPipeline.ProgramIdentifier = first_id;
    modern.pfnSetProgram(validation.hlist(), &set);
    records.destroy(2); // Drains the queued SetProgram before releasing the last program owner.
    errors = device.shell.list_errors;
    modern.pfnSetProgram(validation.hlist(), &set);
    check(device.shell.list_errors == errors + 1 && device.shell.last_list_error == E_INVALIDARG,
          "generic programs: identifier becomes stale after final owner destroy");
    destroy_recording(env, device, validation);
    // The shared positive-path fixture must not inherit our two intentional invalid-ID refusals.
    // Check the exact total before retiring only those expected observer events; preserve prior errors.
    check(device.shell.list_errors == before_errors + 2,
          "generic programs: exactly two intentional identifier refusals on the shared context");
    if (device.shell.list_errors == before_errors + 2) device.shell.list_errors = before_errors;
}
#else
void test_generic_programs(Env&, Device&) {}
#endif
}
