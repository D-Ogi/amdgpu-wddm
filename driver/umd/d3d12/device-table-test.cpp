// SPDX-License-Identifier: MIT
// Host-only composition test against the actual native engine-ddi library.
// No engine, runtime device, Vulkan loader or GPU is opened.
#include <windows.h>
#include "device-table.h"
#include "queue-ddi.h"
#include "fence-ddi.h"
#include "ddi-entry.h"
#include "entry-owner.h"
#include "engine-ddi/ddi-modern-list.h"
#include "engine-ddi/internal.h"        // StateObjectRecord and DeviceContext, to stand in for a created state object
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <type_traits>

namespace {
engine_ddi::DeviceContext* APIENTRY resolve(D3D12DDI_HDEVICE) { return nullptr; }
// A distinct body: with /O2 the linker folds identical functions (/OPT:ICF), and two resolvers with one
// address would pass the engine's "one process-wide resolver" refusal this test relies on.
unsigned other_resolve_calls;
engine_ddi::DeviceContext* APIENTRY other_resolve(D3D12DDI_HDEVICE) { ++other_resolve_calls; return nullptr; }
void APIENTRY present(D3D12DDI_HCOMMANDLIST, D3D12DDI_HCOMMANDQUEUE,
    const D3D12DDIARG_PRESENT_0001*, D3D12DDI_PRESENT_0051*, D3D12DDI_PRESENT_CONTEXTS_0051*, D3D12DDI_PRESENT_HWQUEUES_0051*) {}

// Test-only implementations of the 12 shell slots that have no production DDI
// wrapper yet. They are never invoked, and never linked into the driver.
D3D12DDI_DEVICE_FUNCS_CORE_0088 shell_entries() {
    D3D12DDI_DEVICE_FUNCS_CORE_0088 t{};
    native12::install_queue_entries(t);
    native12::install_fence_entries(t);
    t.pfnMakeResident = [](D3D12DDI_HDEVICE, D3D12DDIARG_MAKERESIDENT_0001*) -> HRESULT { return E_NOTIMPL; };
    t.pfnEvict = [](D3D12DDI_HDEVICE, const D3D12DDIARG_EVICT*) -> HRESULT { return E_NOTIMPL; };
    t.pfnOfferResources = [](D3D12DDI_HDEVICE, const D3D12DDIARG_OFFERRESOURCES*) -> HRESULT { return E_NOTIMPL; };
    t.pfnReclaimResources = [](D3D12DDI_HDEVICE, D3D12DDIARG_RECLAIMRESOURCES_0001*) -> HRESULT { return E_NOTIMPL; };
    t.pfnGetImplicitPhysicalAdapterMask = [](D3D12DDI_HDEVICE) -> UINT { return 0; };
    t.pfnGetPresentPrivateDriverDataSize = [](D3D12DDI_HDEVICE, const D3D12DDIARG_PRESENT_0001*) -> UINT { return 0; };
    t.pfnQueryNodeMap = [](D3D12DDI_HDEVICE, UINT, UINT*) {};
    t.pfnGetDebugAllocationInfo = [](D3D12DDI_HDEVICE, D3D12DDI_HANDLE_AND_TYPE, UINT*,
        D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012*, UINT*, D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014*) {};
    t.pfnCalcPrivateSchedulingGroupSize = [](D3D12DDI_HDEVICE, const D3D12DDIARG_CREATESCHEDULINGGROUP_0050*) -> SIZE_T { return 0; };
    t.pfnCreateSchedulingGroup = [](D3D12DDI_HDEVICE, const D3D12DDIARG_CREATESCHEDULINGGROUP_0050*,
        D3D12DDI_HSCHEDULINGGROUP_0050, D3D12DDI_HRTSCHEDULINGGROUP_0050) -> HRESULT { return E_NOTIMPL; };
    t.pfnDestroySchedulingGroup = [](D3D12DDI_HDEVICE, D3D12DDI_HSCHEDULINGGROUP_0050) {};
    t.pfnSetBackgroundProcessingMode = [](D3D12DDI_HDEVICE, D3D12DDI_BACKGROUND_PROCESSING_MODE_0062,
        D3D12DDI_MEASUREMENTS_ACTION_0062, BOOL*) {};
    return t;
}
template<class T> bool equal(const T& a, const T& b) {
    static_assert(std::is_trivially_copyable_v<T>);
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}
// x64 Windows ABI gate only: inspect pointer representations, never call through
// integer storage. Each slot remains assigned and invoked through its WDK type.
template<class T> bool all_nonzero(const T& table) {
    static_assert(sizeof(void*) == sizeof(uintptr_t));
    static_assert(sizeof(T) % sizeof(uintptr_t) == 0);
    uintptr_t words[sizeof(T) / sizeof(uintptr_t)]{};
    std::memcpy(words, &table, sizeof(table));
    for (auto word : words) if (!word) return false;
    return true;
}

// The state object slots (GetShaderIdentifier, GetShaderStackSize, GetPipelineStackSize, SetPipelineStackSize) carry
// the state object as their only handle. The entry thunk resolves their owner from it through the production
// overloads of entry-owner.h, which native-tables.cpp's EntryPolicy uses, and the real engine_ddi::state_object_shell;
// this policy adds no resolver of its own. The originals record that they ran in the owner's scope.
struct Owner {
    unsigned calls{}, list_errors{};
    D3D12DDI_HRTCOMMANDLIST last_list{};
    HRESULT list_error{};
};
thread_local Owner* current{};
unsigned denials{}, stray{};
HRESULT last_denial{};
struct StateObjectPolicy : native12::EntryOwner<Owner> {
    class Scope {
        Owner* previous_;
    public:
        explicit Scope(Owner& owner) noexcept : previous_(current) { current = &owner; }
        ~Scope() { current = previous_; }
        bool entered() const noexcept { return true; }
    };
    static void failure(Owner*, HRESULT hr) noexcept { ++denials; last_denial = hr; }
};
int identifier_marker{};
void entered() noexcept { if (current) ++current->calls; else ++stray; }
void* APIENTRY shader_identifier(D3D12DDI_HSTATEOBJECT_0054, LPCWSTR) { entered(); return &identifier_marker; }
UINT APIENTRY shader_stack_size(D3D12DDI_HSTATEOBJECT_0054, LPCWSTR) { entered(); return 7; }
UINT APIENTRY pipeline_stack_size(D3D12DDI_HSTATEOBJECT_0054) { entered(); return 9; }
void APIENTRY set_pipeline_stack_size(D3D12DDI_HSTATEOBJECT_0054, UINT) { entered(); }
// A failed expectation ends the test with exit code 1 and a FAILED line, never an abort dialog.
void expect(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAILED: device table entries: %s (calls outside a scope %u, denials %u)\n", what, stray, denials);
    std::exit(1);
}

// A live record and a failed create's inert one name their device: the four wrapped slots reach the originals in
// the owner's scope. Storage without a record (zeroed, or poisoned by DestroyStateObject) and a null handle resolve
// no owner: each slot is denied with E_INVALIDARG before any original runs, and answers null, 0 or nothing.
void test_state_object_entries(const D3D12DDI_DEVICE_FUNCS_CORE_0088& composed) {
    auto source = composed;
    source.pfnGetShaderIdentifier = shader_identifier;
    source.pfnGetShaderStackSize = shader_stack_size;
    source.pfnGetPipelineStackSize = pipeline_stack_size;
    source.pfnSetPipelineStackSize = set_pipeline_stack_size;
    D3D12DDI_DEVICE_FUNCS_CORE_0088 wrapped{};
    expect(native12::DdiEntryTables<StateObjectPolicy>::wrap_core(source, &wrapped) == S_OK, "wrap_core");
    expect(wrapped.pfnGetShaderIdentifier && wrapped.pfnGetShaderStackSize && wrapped.pfnGetPipelineStackSize &&
           wrapped.pfnSetPipelineStackSize && wrapped.pfnGetShaderIdentifier != shader_identifier,
           "wrap_core wraps the four slots");
    if (!wrapped.pfnGetShaderIdentifier || !wrapped.pfnGetShaderStackSize || !wrapped.pfnGetPipelineStackSize ||
        !wrapped.pfnSetPipelineStackSize) return;       // expect has exited; this tells /analyze so

    Owner owner;
    engine_ddi::DeviceContext context;          // no engine: state_object_shell reads hooks.shell only
    context.hooks.shell = &owner;
    engine_ddi::StateObjectRecord live{{engine_ddi::Tag::StateObject, 0, nullptr, &context}, nullptr, nullptr, {}, true};
    engine_ddi::StateObjectRecord inert{{engine_ddi::Tag::StateObject, engine_ddi::kRecordInvalid, nullptr, &context},
                                        nullptr, nullptr, {}, false};
    for (engine_ddi::StateObjectRecord* record : {&live, &inert}) {
        const D3D12DDI_HSTATEOBJECT_0054 h{record};
        const unsigned before = owner.calls;
        expect(wrapped.pfnGetShaderIdentifier(h, L"raygen") == &identifier_marker, "GetShaderIdentifier reaches the original");
        expect(wrapped.pfnGetShaderStackSize(h, L"raygen") == 7, "GetShaderStackSize reaches the original");
        expect(wrapped.pfnGetPipelineStackSize(h) == 9, "GetPipelineStackSize reaches the original");
        wrapped.pfnSetPipelineStackSize(h, 64);
        expect(owner.calls == before + 4 && !stray && !current && !denials,
               "a record naming a device: four originals run in the owner's scope, none denied");
    }

    engine_ddi::StateObjectRecord destroyed = live;
    engine_ddi::poison(destroyed.h);
    alignas(engine_ddi::StateObjectRecord) unsigned char zeroed[sizeof(engine_ddi::StateObjectRecord)]{};
    for (void* storage : {static_cast<void*>(&destroyed), static_cast<void*>(zeroed), static_cast<void*>(nullptr)}) {
        const D3D12DDI_HSTATEOBJECT_0054 h{storage};
        const unsigned before = owner.calls, denied_before = denials;
        expect(!wrapped.pfnGetShaderIdentifier(h, L"raygen"), "GetShaderIdentifier denied, null");
        expect(wrapped.pfnGetShaderStackSize(h, L"raygen") == 0, "GetShaderStackSize denied, 0");
        expect(wrapped.pfnGetPipelineStackSize(h) == 0, "GetPipelineStackSize denied, 0");
        wrapped.pfnSetPipelineStackSize(h, 64);
        expect(owner.calls == before && !stray && denials == denied_before + 4 && last_denial == E_INVALIDARG && !current,
               "no record: four denials with E_INVALIDARG, no original run");
    }
    std::puts("PASSED: state object slots through entry-owner.h's production resolvers reach the originals in the owner scope "
              "for a live and an inert record (state_object_shell), denied with E_INVALIDARG for a destroyed record, zeroed "
              "storage and a null handle; no runtime/GPU");
}

#if defined(D3D12DDI_BUILD_VERSION_0119)
engine_ddi::CommandListRecord* expected_list{};
void APIENTRY scoped_draw(D3D12DDI_HCOMMANDLIST list, UINT vertices, UINT instances, UINT first_vertex,
                         UINT first_instance) {
    expect(list.pDrvPrivate == expected_list, "0114 original receives the driver record, not the API header");
    expect(vertices == 3 && instances == 5 && first_vertex == 7 && first_instance == 11,
           "0114 draw preserves all scalar arguments");
    entered();
}
void APIENTRY scoped_list_error(void* shell, D3D12DDI_HRTCOMMANDLIST list, HRESULT hr) {
    auto* owner = static_cast<Owner*>(shell);
    expect(current == owner, "0114 refusal reaches the runtime hook within its owner's scope");
    expect(expected_list && list.handle == expected_list->rt.handle,
           "0114 refusal reports the command list's runtime handle");
    ++owner->list_errors;
    owner->last_list = list;
    owner->list_error = hr;
}
void APIENTRY modern_list_error(D3D12DDI_HCOMMANDLIST list, HRESULT hr) {
    // Same bridge as native-tables.cpp: the real engine library selects the
    // owning context and forwards the runtime list handle to its shell hook.
    engine_ddi::report_command_list_error(list, hr);
}

void test_api_command_list_entries(const engine_ddi::FillInfo& info) {
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 legacy{};
    expect(native12::compose_list_0092(&legacy, sizeof(legacy), 1, present, info) == S_OK,
           "compose source for API command-list test");
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0094 base{};
    expect(engine_ddi::make_list_0094(legacy, &base), "make typed 0094 source");
    base.pfnDrawInstanced = scoped_draw;
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0114 source{}, wrapped{};
    expect(native12::upgrade_list(base, &source, 1, modern_list_error), "make typed 0114 source");
    expect(native12::DdiEntryTables<StateObjectPolicy>::wrap_list(1, source, &wrapped) == S_OK,
           "wrap 0114 with production EntryOwner resolver");
    expect(wrapped.pfnDrawInstanced && wrapped.pfnSetProgram && wrapped.pfnDispatchGraph,
           "0114 wrapper supplies all tested entries");
    if (!wrapped.pfnDrawInstanced || !wrapped.pfnSetProgram || !wrapped.pfnDispatchGraph) return;
    D3D12DDI_SET_PROGRAM_DESC_0108 program{};
    D3D12DDI_DISPATCH_GRAPH_DESC_0108 graph{};

    Owner owner;
    engine_ddi::DeviceContext context;
    context.hooks.shell = &owner;
    context.hooks.report_list_error = scoped_list_error;
    int live_runtime{}, inert_runtime{};
    engine_ddi::CommandListRecord live{}, inert{}, destroyed{}, orphan{};
    live.h = {engine_ddi::Tag::CommandList, 0, nullptr, &context};
    live.rt = {&live_runtime};
    inert.h = {engine_ddi::Tag::CommandList, engine_ddi::kRecordInvalid, nullptr, &context};
    inert.rt = {&inert_runtime};
    destroyed.h = live.h;
    engine_ddi::poison(destroyed.h);
    orphan.h = {engine_ddi::Tag::CommandList, 0, nullptr, nullptr};
    const unsigned starting_denials = denials;
    for (auto* record : {&live, &inert}) {
        expected_list = record;
        D3D12DDI_RUNTIME_BYPASS_HEADER api_object{nullptr, record};
        const D3D12DDI_API_HCOMMANDLIST api{&api_object};
        expect(native12::EntryOwner<Owner>::resolve(api) == &owner,
               "API command-list handle resolves its live or inert record's owner");
        const unsigned before = owner.calls, errors_before = owner.list_errors;
        wrapped.pfnDrawInstanced(api, 3, 5, 7, 11);
        expect(owner.calls == before + 1 && !current && !stray && denials == starting_denials,
               "wrapped 0114 draw enters and exits the resolved owner's scope");
        wrapped.pfnSetProgram(api, &program);
        wrapped.pfnDispatchGraph(api, &graph);
        expect(owner.list_errors == errors_before + 2 && owner.list_error == E_NOTIMPL &&
                   owner.last_list.handle == record->rt.handle && !current && denials == starting_denials,
               "unsupported 0114 commands report E_NOTIMPL through the real engine list-error bridge");
    }

    alignas(engine_ddi::CommandListRecord) unsigned char zeroed[sizeof(engine_ddi::CommandListRecord)]{};
    D3D12DDI_RUNTIME_BYPASS_HEADER destroyed_api{nullptr, &destroyed};
    D3D12DDI_RUNTIME_BYPASS_HEADER zeroed_api{nullptr, zeroed};
    D3D12DDI_RUNTIME_BYPASS_HEADER orphan_api{nullptr, &orphan};
    D3D12DDI_RUNTIME_BYPASS_HEADER null_driver_api{nullptr, nullptr};
    for (auto* api_object : {&destroyed_api, &zeroed_api, &orphan_api, &null_driver_api,
                            static_cast<D3D12DDI_RUNTIME_BYPASS_HEADER*>(nullptr)}) {
        const D3D12DDI_API_HCOMMANDLIST api{api_object};
        expect(!native12::EntryOwner<Owner>::resolve(api), "invalid API command-list handle resolves no owner");
        const unsigned before = owner.calls, errors_before = owner.list_errors, denials_before = denials;
        wrapped.pfnDrawInstanced(api, 3, 5, 7, 11);
        wrapped.pfnSetProgram(api, &program);
        wrapped.pfnDispatchGraph(api, &graph);
        expect(owner.calls == before && owner.list_errors == errors_before && denials == denials_before + 3 &&
                   last_denial == E_INVALIDARG && !current && !stray,
               "invalid API handle is denied before conversion, engine calls or list-error callbacks");
    }
    expected_list = nullptr;
    std::puts("PASSED: wrapped 0114 API command lists use production ownership and engine list-error reporting; "
              "live/inert records enter scope, destroyed/zeroed/orphan/null records are denied; no runtime/GPU");
}
#endif
} // namespace

int main() {
    const engine_ddi::FillInfo info{sizeof(info), resolve};
    const auto shell = shell_entries();
    auto core = shell;
    const auto before = core;
    using native12::compose_core_0092;
    using native12::compose_list_0092;
    assert(compose_core_0092(nullptr, sizeof(core), shell, info) == E_INVALIDARG);
    assert(compose_core_0092(&core, sizeof(core) - 8, shell, info) == E_INVALIDARG && equal(core, before));
    auto bad_info = info;
    bad_info.resolve = nullptr;
    assert(compose_core_0092(&core, sizeof(core), shell, bad_info) == E_INVALIDARG && equal(core, before));
    bad_info = info;
    --bad_info.size;
    assert(compose_core_0092(&core, sizeof(core), shell, bad_info) == E_INVALIDARG && equal(core, before));
    unsigned missing_checked = 0;
    std::apply([&](auto... member) {
        const auto check_missing = [&](auto field) {
            auto missing = shell;
            missing.*field = nullptr;
            assert(compose_core_0092(&core, sizeof(core), missing, info) == E_NOTIMPL);
            assert(equal(core, before));
            ++missing_checked;
        };
        (check_missing(member), ...);
    }, native12::table_detail::shell_core_members);
    assert(missing_checked == 18);
    assert(compose_core_0092(&core, sizeof(core), shell, info) == S_OK);
    assert(all_nonzero(core));
    std::apply([&](auto... member) { assert(((core.*member == shell.*member) && ...)); },
        native12::table_detail::shell_core_members);
    assert(core.pfnCalcPrivateCommandQueueSize == native12::queue_size);
    assert(core.pfnCreateFence == native12::fence_create);
    D3D12DDIARG_CREATE_FENCE invalid_fence{};
    assert(core.pfnCalcPrivateFenceSize({}, &invalid_fence) == sizeof(native12::FenceState));
    // Alias-safe: all input reads finish before the output commit.
    auto alias = shell;
    assert(compose_core_0092(&alias, sizeof(alias), alias, info) == S_OK && equal(alias, core));
    // A second process-wide resolver is rejected by the real engine library.
    const auto completed = core;
    bad_info = info;
    bad_info.resolve = other_resolve;
    assert(FAILED(compose_core_0092(&core, sizeof(core), shell, bad_info)) && equal(core, completed));

    for (UINT number = 0; number < 2; ++number) {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 list{};
        list.pfnPresent = present;
        const auto unchanged = list;
        assert(compose_list_0092(nullptr, sizeof(list), number, present, info) == E_INVALIDARG);
        assert(compose_list_0092(&list, sizeof(list) - 8, number, present, info) == E_INVALIDARG && equal(list, unchanged));
        assert(compose_list_0092(&list, sizeof(list), 2, present, info) == E_INVALIDARG && equal(list, unchanged));
        assert(compose_list_0092(&list, sizeof(list), number, nullptr, info) == E_NOTIMPL && equal(list, unchanged));
        assert(FAILED(compose_list_0092(&list, sizeof(list), number, present, bad_info)) && equal(list, unchanged));
        assert(compose_list_0092(&list, sizeof(list), number, present, info) == S_OK);
        assert(all_nonzero(list) && list.pfnPresent == present);
    }
    std::puts("PASSED: core122/list70x2 composition,18 missing shell entries refused, output preserved on failure; no runtime/GPU");
    test_state_object_entries(completed);
    assert(!other_resolve_calls);
#if defined(D3D12DDI_BUILD_VERSION_0119)
    test_api_command_list_entries(info);
#endif
}
