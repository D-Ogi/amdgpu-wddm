// SPDX-License-Identifier: MIT
// Compile gate for engine-ddi.h: self-contained, /W4 /WX clean next to the shell headers, record tags unique.
#include "engine-ddi.h"
#include <cstdio>
#include <iterator>

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
static_assert(tags_unique(), "record tags must be unique and distinct from None/Poisoned");
static_assert(sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088) == 976, "core table 0088");
static_assert(sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092) == 560, "command list table 0092");
static_assert(alignof(engine_ddi::RecordHeader) == alignof(void*), "record header alignment");
} // namespace

int main() {
    std::printf("engine-ddi header r%u: %zu record tags, header %zu bytes\n", engine_ddi::kBoundaryRevision,
                std::size(kTags), sizeof(engine_ddi::RecordHeader));
    return 0;
}
