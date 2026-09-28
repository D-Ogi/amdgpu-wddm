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
static_assert(engine_ddi::kBoundaryRevision == 2, "boundary r2");
static_assert(tags_unique(), "record tags must be unique and distinct from None/Poisoned");
static_assert(sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088) == 976, "core table 0088");
static_assert(sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092) == 560, "command list table 0092");
static_assert(alignof(engine_ddi::RecordHeader) == alignof(void*), "record header alignment");

// Structs that cross the boundary: no implicit padding in the parts the shell fills, x64 sizes pinned.
static_assert(std::is_standard_layout_v<engine_ddi::MemoryRequest>, "MemoryRequest layout");
static_assert(std::is_standard_layout_v<engine_ddi::ImportedMemory>, "ImportedMemory layout");
static_assert(std::is_standard_layout_v<engine_ddi::ShellHooks>, "ShellHooks layout");
static_assert(std::is_standard_layout_v<engine_ddi::EngineCaps>, "EngineCaps layout");
static_assert(sizeof(engine_ddi::MemoryRequest) == 56, "MemoryRequest size");
static_assert(offsetof(engine_ddi::ImportedMemory, gpu_va) == 32, "ImportedMemory.gpu_va offset");
static_assert(sizeof(engine_ddi::ImportedMemory) == 48, "ImportedMemory size");
static_assert(sizeof(engine_ddi::ShellHooks) == 64, "ShellHooks size");

// r2: free_memory returns HRESULT; collect_caps takes an engine-ddi caps result, not an engine device.
static_assert(std::is_same_v<decltype(engine_ddi::ShellHooks::free_memory),
                             HRESULT (APIENTRY*)(void*, const engine_ddi::ImportedMemory*)>, "free_memory");
static_assert(std::is_same_v<decltype(&engine_ddi::collect_caps),
                             HRESULT (*)(const engine_ddi::EngineCaps*, engine_ddi::CapsSnapshot**) noexcept>,
              "collect_caps");

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
                "EngineCaps %zu\n",
                engine_ddi::kBoundaryRevision, std::size(kTags), sizeof(engine_ddi::RecordHeader),
                sizeof(engine_ddi::MemoryRequest), sizeof(engine_ddi::ImportedMemory), sizeof(engine_ddi::EngineCaps));
    return 0;
}
