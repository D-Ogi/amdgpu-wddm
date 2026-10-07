// SPDX-License-Identifier: MIT
// Compile gate for engine-ddi.h: self-contained, /W4 /WX clean next to the shell headers, record tags unique,
// boundary revision and the layouts of the structs the shell fills.
#include "engine-ddi.h"
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <type_traits>

namespace {
constexpr engine_ddi::Tag kTags[] = {
    engine_ddi::Tag::Heap, engine_ddi::Tag::Resource, engine_ddi::Tag::DescriptorHeap,
    engine_ddi::Tag::RootSignature, engine_ddi::Tag::Shader, engine_ddi::Tag::ElementLayout,
    engine_ddi::Tag::StateBlend, engine_ddi::Tag::StateDepth, engine_ddi::Tag::StateRaster,
    engine_ddi::Tag::PipelineState, engine_ddi::Tag::CommandPool, engine_ddi::Tag::CommandRecorder,
    engine_ddi::Tag::CommandList, engine_ddi::Tag::QueryHeap, engine_ddi::Tag::CommandSignature,
    engine_ddi::Tag::StateObject,
};
constexpr bool tags_unique() {
    for (size_t i = 0; i < std::size(kTags); ++i) {
        if (kTags[i] == engine_ddi::Tag::None || kTags[i] == engine_ddi::Tag::Poisoned) return false;
        for (size_t j = i + 1; j < std::size(kTags); ++j)
            if (kTags[i] == kTags[j]) return false;
    }
    return true;
}
static_assert(engine_ddi::kBoundaryRevision == 5, "boundary r5");
static_assert(tags_unique(), "record tags must be unique and distinct from None/Poisoned");
static_assert(sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088) == 122 * sizeof(void*), "core table 0088 (976 bytes on x64)");
static_assert(sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092) == 70 * sizeof(void*), "command list table 0092 (560 on x64)");
static_assert(alignof(engine_ddi::RecordHeader) == alignof(void*), "record header alignment");

// Structs that cross the boundary: no implicit padding in the parts the shell fills on x64, x64 and x86 sizes pinned.
// The x86 build (the WoW64 shell) has 4-byte pointers and handles, so MemoryRequest and AdoptRequest get 4 bytes of
// padding before their first uint64_t there; engine-ddi is linked into the shell, so both sides of these structs are
// always one compiler's view of them.
static_assert(std::is_standard_layout_v<engine_ddi::MemoryRequest>, "MemoryRequest layout");
static_assert(std::is_standard_layout_v<engine_ddi::ImportedMemory>, "ImportedMemory layout");
static_assert(std::is_standard_layout_v<engine_ddi::ShellHooks>, "ShellHooks layout");
#ifdef _WIN64
static_assert(sizeof(engine_ddi::MemoryRequest) == 72 &&
                  offsetof(engine_ddi::MemoryRequest, surface_row_pitch) == 56 &&
                  offsetof(engine_ddi::MemoryRequest, surface_layout_size) == 64,
              "MemoryRequest size (r4: the linear surface)");
#else
static_assert(sizeof(engine_ddi::MemoryRequest) == 64 &&
                  offsetof(engine_ddi::MemoryRequest, surface_row_pitch) == 48 &&
                  offsetof(engine_ddi::MemoryRequest, surface_layout_size) == 56,
              "MemoryRequest x86 size (r4: the linear surface)");
#endif
static_assert(offsetof(engine_ddi::ImportedMemory, gpu_va) == 32, "ImportedMemory.gpu_va offset");
static_assert(sizeof(engine_ddi::ImportedMemory) == 48, "ImportedMemory size");
static_assert(sizeof(engine_ddi::ShellHooks) == (sizeof(void*) == 8 ? 72 : 36),
              "ShellHooks size (r5: adopt_memory): a uint32_t and eight pointers");
// r5 (BD-075): the open half of a shared surface. AdoptRequest is the shell's side of pfnOpenHeapAndResource and
// carries the handle the runtime opened, never a size to allocate.
static_assert(std::is_standard_layout_v<engine_ddi::AdoptRequest>, "AdoptRequest layout");
static_assert(sizeof(engine_ddi::AdoptRequest) == 40 &&
                  offsetof(engine_ddi::AdoptRequest, allocation) == 8 + sizeof(void*) &&
                  offsetof(engine_ddi::AdoptRequest, byte_size) == 24 &&
                  offsetof(engine_ddi::AdoptRequest, alignment) == 32,
              "AdoptRequest size (r5: the shared open)");
static_assert(std::is_same_v<decltype(engine_ddi::ShellHooks::adopt_memory),
                             HRESULT (APIENTRY*)(void*, const engine_ddi::AdoptRequest*,
                                                 engine_ddi::ImportedMemory*)>, "adopt_memory");
static_assert(std::is_same_v<decltype(engine_ddi::AdoptRequest::allocation), D3DKMT_HANDLE>, "adopted handle");
// The refusal that asks the shell to retry a create as a shared surface never leaves the DDI as itself: the
// runtime admits only out of memory and device removal from a create or an open.
static_assert(engine_ddi::admitted_create_failure(engine_ddi::kShareRequired) == E_OUTOFMEMORY,
              "kShareRequired is clamped at the DDI");

// r2: free_memory returns HRESULT. r3: the adapter caps path takes the engine's function table (ABI 1.2) and the
// create info of the device to come; build_caps answers from what that query returned.
static_assert(std::is_same_v<decltype(engine_ddi::ShellHooks::free_memory),
                             HRESULT (APIENTRY*)(void*, const engine_ddi::ImportedMemory*)>, "free_memory");
static_assert(std::is_same_v<decltype(&engine_ddi::query_adapter_caps),
                             HRESULT (*)(const BC250_VKD3D_ENGINE_FUNCS*, const BC250_VKD3D_DEVICE_CREATE_INFO*,
                                         engine_ddi::AdapterCaps**) noexcept>,
              "query_adapter_caps");
static_assert(std::is_same_v<decltype(&engine_ddi::free_adapter_caps), void (*)(engine_ddi::AdapterCaps*) noexcept>,
              "free_adapter_caps");
static_assert(std::is_same_v<decltype(&engine_ddi::build_caps),
                             HRESULT (*)(const engine_ddi::AdapterCaps*, uint32_t, const D3D12DDIARG_GETCAPS*) noexcept>,
              "build_caps");
// Added within r3, additively: the shell's memory architecture policy of GetCaps 1002. A zero-initialized policy is
// all Default; the struct the shell fills has no implicit padding.
static_assert(std::is_same_v<decltype(&engine_ddi::set_memory_architecture_policy),
                             HRESULT (*)(engine_ddi::AdapterCaps*, const engine_ddi::MemoryArchitecturePolicy*) noexcept>,
              "set_memory_architecture_policy");
static_assert(std::is_same_v<std::underlying_type_t<engine_ddi::PolicyBool>, uint32_t> &&
                  static_cast<uint32_t>(engine_ddi::PolicyBool::Default) == 0 &&
                  static_cast<uint32_t>(engine_ddi::PolicyBool::False) == 1 &&
                  static_cast<uint32_t>(engine_ddi::PolicyBool::True) == 2,
              "PolicyBool values");
static_assert(std::is_standard_layout_v<engine_ddi::MemoryArchitecturePolicy> &&
                  std::is_trivially_copyable_v<engine_ddi::MemoryArchitecturePolicy>,
              "MemoryArchitecturePolicy layout");
static_assert(sizeof(engine_ddi::PolicyTier) == 8 && sizeof(engine_ddi::MemoryArchitecturePolicy) == 32 &&
                  offsetof(engine_ddi::MemoryArchitecturePolicy, heap_serialization_tier) == 16 &&
                  offsetof(engine_ddi::MemoryArchitecturePolicy, resource_serialization_tier) == 24,
              "MemoryArchitecturePolicy size and offsets");
static_assert(sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041) == 20 && D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1 == 1 &&
                  D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2 == 2,
              "GetCaps 1002 payload and the highest tiers at 0092");
// Added within r4, additively: the retire hand-off policy. Four uint32_t, no padding.
static_assert(std::is_same_v<decltype(&engine_ddi::set_retire_policy),
                             HRESULT (*)(engine_ddi::DeviceContext*, const engine_ddi::RetirePolicy*) noexcept>,
              "set_retire_policy");
static_assert(std::is_standard_layout_v<engine_ddi::RetirePolicy> && sizeof(engine_ddi::RetirePolicy) == 16 &&
                  offsetof(engine_ddi::RetirePolicy, age_bound_ms) == 12,
              "RetirePolicy size and offsets");
// Added within r4, additively: the two-phase release policy. Two uint32_t, no padding.
static_assert(std::is_same_v<decltype(&engine_ddi::set_release_policy),
                             HRESULT (*)(engine_ddi::DeviceContext*, const engine_ddi::ReleasePolicy*) noexcept>,
              "set_release_policy");
static_assert(std::is_standard_layout_v<engine_ddi::ReleasePolicy> && sizeof(engine_ddi::ReleasePolicy) == 8 &&
                  offsetof(engine_ddi::ReleasePolicy, two_phase) == 4,
              "ReleasePolicy size and offsets");
// Added within r4, additively: deferred command-list replay. Four uint32_t, then four pointers (log: replay-hitch).
static_assert(std::is_same_v<decltype(&engine_ddi::set_replay_policy),
                             HRESULT (*)(engine_ddi::DeviceContext*, const engine_ddi::ReplayPolicy*) noexcept>,
              "set_replay_policy");
static_assert(std::is_same_v<engine_ddi::ReplayBody, void (APIENTRY*)(void*)>, "ReplayBody");
static_assert(std::is_standard_layout_v<engine_ddi::ReplayPolicy> &&
                  sizeof(engine_ddi::ReplayPolicy) == 16 + 4 * sizeof(void*) &&
                  offsetof(engine_ddi::ReplayPolicy, ring_bytes) == 12 && offsetof(engine_ddi::ReplayPolicy, shell) == 16 &&
                  offsetof(engine_ddi::ReplayPolicy, worker) == 16 + sizeof(void*) &&
                  offsetof(engine_ddi::ReplayPolicy, drained) == 16 + 2 * sizeof(void*) &&
                  offsetof(engine_ddi::ReplayPolicy, log) == 16 + 3 * sizeof(void*),
              "ReplayPolicy size and offsets (48 bytes on x64, 32 on x86)");
// Shell-facing calls added within r3: the owner of a command list, the queue close result, the residency lookup.
static_assert(std::is_same_v<decltype(&engine_ddi::command_list_shell), void* (*)(D3D12DDI_HCOMMANDLIST) noexcept>,
              "command_list_shell");
static_assert(std::is_same_v<decltype(&engine_ddi::state_object_shell),
                             void* (*)(D3D12DDI_HSTATEOBJECT_0054) noexcept>,
              "state_object_shell");
static_assert(std::is_same_v<decltype(&engine_ddi::destroy_engine_queue),
                             engine_ddi::QueueClose (*)(engine_ddi::EngineQueue*) noexcept>,
              "destroy_engine_queue");
static_assert(std::is_same_v<decltype(&engine_ddi::object_allocation),
                             HRESULT (*)(engine_ddi::DeviceContext*, D3D12DDI_HANDLE_AND_TYPE, D3DKMT_HANDLE*) noexcept>,
              "object_allocation");
// Tiled resources, added within r3: the engine parts of the shell's queue slots Q3 and Q4 take the EngineQueue and
// then exactly the rest of the slot's arguments (PFND3D12DDI_UPDATETILEMAPPINGS, PFND3D12DDI_COPYTILEMAPPINGS).
static_assert(std::is_same_v<decltype(&engine_ddi::update_tile_mappings),
                             HRESULT (*)(engine_ddi::EngineQueue*, D3D12DDI_HRESOURCE, UINT,
                                         const D3D12DDI_TILED_RESOURCE_COORDINATE*, const D3D12DDI_TILE_REGION_SIZE*,
                                         D3D12DDI_HHEAP, UINT, const D3D12DDI_TILE_RANGE_FLAGS*, const UINT*,
                                         const UINT*, D3D12DDI_TILE_MAPPING_FLAGS) noexcept>,
              "update_tile_mappings");
static_assert(std::is_same_v<PFND3D12DDI_UPDATETILEMAPPINGS,
                             VOID (APIENTRY*)(D3D12DDI_HCOMMANDQUEUE, D3D12DDI_HRESOURCE, UINT,
                                              const D3D12DDI_TILED_RESOURCE_COORDINATE*,
                                              const D3D12DDI_TILE_REGION_SIZE*, D3D12DDI_HHEAP, UINT,
                                              const D3D12DDI_TILE_RANGE_FLAGS*, const UINT*, const UINT*,
                                              D3D12DDI_TILE_MAPPING_FLAGS)>,
              "Q3 slot the shell forwards");
static_assert(std::is_same_v<decltype(&engine_ddi::copy_tile_mappings),
                             HRESULT (*)(engine_ddi::EngineQueue*, D3D12DDI_HRESOURCE,
                                         const D3D12DDI_TILED_RESOURCE_COORDINATE*, D3D12DDI_HRESOURCE,
                                         const D3D12DDI_TILED_RESOURCE_COORDINATE*, const D3D12DDI_TILE_REGION_SIZE*,
                                         D3D12DDI_TILE_MAPPING_FLAGS) noexcept>,
              "copy_tile_mappings");
static_assert(std::is_same_v<PFND3D12DDI_COPYTILEMAPPINGS,
                             VOID (APIENTRY*)(D3D12DDI_HCOMMANDQUEUE, D3D12DDI_HRESOURCE,
                                              const D3D12DDI_TILED_RESOURCE_COORDINATE*, D3D12DDI_HRESOURCE,
                                              const D3D12DDI_TILED_RESOURCE_COORDINATE*,
                                              const D3D12DDI_TILE_REGION_SIZE*, D3D12DDI_TILE_MAPPING_FLAGS)>,
              "Q4 slot the shell forwards");
// The engine header this revision is built against is ABI 1.3 (linear images, V13).
static_assert(BC250_VKD3D_ENGINE_ABI_VERSION == ((1u << 16) | 3u), "engine ABI 1.3 header");
#ifdef _WIN64
static_assert(sizeof(BC250_VKD3D_FEATURE_QUERY) == 24 && sizeof(BC250_VKD3D_ENGINE_FUNCS) == 80 &&
                  sizeof(BC250_VKD3D_LINEAR_IMAGE_INFO) == 48,
              "ABI 1.3 x64 sizes");
// r4 (V12): the create info carries InstanceMode, which engine-ddi's callers set to PRIVATE.
static_assert(sizeof(BC250_VKD3D_DEVICE_CREATE_INFO) == 48 &&
                  offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, InstanceMode) == 40 && BC250_VKD3D_INSTANCE_MODE_PRIVATE == 1u,
              "ABI 1.2 r4 create info");
#else
// The x86 engine (WoW64) is built from the same header, which pins its sizes only for x64. The function table is
// two UINT32 and nine pointers.
static_assert(sizeof(BC250_VKD3D_FEATURE_QUERY) == 20 && sizeof(BC250_VKD3D_ENGINE_FUNCS) == 44 &&
                  sizeof(BC250_VKD3D_LINEAR_IMAGE_INFO) == 48,
              "ABI 1.3 x86 sizes");
static_assert(sizeof(BC250_VKD3D_DEVICE_CREATE_INFO) == 36 &&
                  offsetof(BC250_VKD3D_DEVICE_CREATE_INFO, InstanceMode) == 32 && BC250_VKD3D_INSTANCE_MODE_PRIVATE == 1u,
              "ABI 1.2 r4 create info, x86");
#endif
// The two GetCaps payloads of M768 (d3d12umddi.h 10.0.26100): 1074 is 8 bytes, 1007 is the 4-byte level itself.
static_assert(sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081) == 8 && sizeof(D3D12DDI_3DPIPELINELEVEL) == 4,
              "GetCaps 1074 and 1007 payloads");

// The fourth argument of CreateHeapAndResource 0088 is the runtime owner that MemoryRequest carries.
static_assert(std::is_same_v<PFND3D12DDI_CREATEHEAPANDRESOURCE_0088,
                             HRESULT (APIENTRY*)(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001*, D3D12DDI_HHEAP,
                                                 D3D12DDI_HRTRESOURCE, const D3D12DDIARG_CREATERESOURCE_0088*,
                                                 const D3D12DDI_CLEAR_VALUES*, D3D12DDI_HPROTECTEDRESOURCESESSION_0030,
                                                 D3D12DDI_HRESOURCE)>, "CreateHeapAndResource 0088");
static_assert(std::is_same_v<decltype(engine_ddi::MemoryRequest::rt_owner), D3D12DDI_HRTRESOURCE>, "rt_owner");
} // namespace

int main() {
    std::printf("engine-ddi header r%u: %zu record tags, header %zu bytes, MemoryRequest %zu, ImportedMemory %zu, "
                "engine ABI %u.%u\n",
                engine_ddi::kBoundaryRevision, std::size(kTags), sizeof(engine_ddi::RecordHeader),
                sizeof(engine_ddi::MemoryRequest), sizeof(engine_ddi::ImportedMemory), BC250_VKD3D_ENGINE_ABI_MAJOR,
                BC250_VKD3D_ENGINE_ABI_MINOR);
    return 0;
}
