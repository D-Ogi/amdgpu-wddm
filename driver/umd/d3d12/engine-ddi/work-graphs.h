// SPDX-License-Identifier: MIT
// Work graphs tier 1.0 on the system route (work-graphs.cpp): the WORK_GRAPH subobject of CreateStateObject, the
// program identifiers and memory requirements of CORE_0108, SetProgram and DispatchGraph of COMMAND_LIST_3D_0108.
#pragma once
#include "internal.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "ddi-modern-core.h"
#include "ddi-modern-list.h"
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace engine_ddi {

// The engine's flag for a graph of the DDI (vkd3d-proton fork, libs/vkd3d/workgraphs.c,
// VKD3D_WORK_GRAPH_FLAG_DDI_ORDER_AMDGPU_WDDM): the entry points are pEntrypoints in that order, every node is an
// explicitly defined node with its final properties. D3D12 defines no such flag; an engine without it refuses the
// graph with E_NOTIMPL.
constexpr UINT kWorkGraphFlagDdiOrder = 0x80000000u;

// The API description of one WORK_GRAPH subobject and everything it points to. The deques keep the addresses of their
// elements when they grow; the vectors are sized once, before a pointer into them is taken.
struct WorkGraphTranslation {
    std::wstring program;
    std::deque<std::wstring> names;
    std::deque<D3D12_NODE_ID> ids;
    std::deque<UINT> values;
    std::deque<BOOL> flags;
    std::deque<std::array<UINT, 3>> grids;
    std::vector<D3D12_NODE_ID> entrypoints;
    std::vector<D3D12_NODE> nodes;
    std::vector<D3D12_BROADCASTING_LAUNCH_OVERRIDES> broadcasting;
    std::vector<D3D12_COALESCING_LAUNCH_OVERRIDES> coalescing;   // also the thread launch nodes (same layout)
    std::deque<std::vector<D3D12_NODE_OUTPUT_OVERRIDES>> outputs;
};

// D3D12DDI_WORK_GRAPH_DESC_0108 into out, with its storage in t. Nothing reaches the engine here.
HRESULT translate_work_graph(const D3D12DDI_WORK_GRAPH_DESC_0108& in, WorkGraphTranslation& t,
                             D3D12_WORK_GRAPH_DESC& out) noexcept;

// The programs of one work graph state object, known by their engine program identifiers until the state object is
// destroyed. SetProgram and DispatchGraph find a program by its identifier.
struct WorkGraphPrograms {
    std::vector<std::array<UINT64, 4>> identifiers;
    ~WorkGraphPrograms();
};

// After the engine created the state object: the programs of so. Null with *hr set when the engine answers
// inconsistently.
std::unique_ptr<WorkGraphPrograms> register_work_graphs(DeviceContext* c, ID3D12StateObject* so,
                                                        HRESULT* hr) noexcept;

// GetProgramIdentifier of a work graph state object record; zero for an unknown name.
D3D12DDI_PROGRAM_IDENTIFIER_0108 work_graph_identifier(const StateObjectRecord& record, LPCWSTR name) noexcept;

// SetProgram with D3D12DDI_PROGRAM_TYPE_WORK_GRAPH_0108.
void work_graph_set_program(CommandListRecord* list, const D3D12DDI_SET_WORK_GRAPH_DESC_0108& desc) noexcept;

// The CORE_0108 and COMMAND_LIST_3D_0108 entries of this file.
void fill_work_graph_entries(native12::ModernCoreEntries* core, native12::ModernListEntries* lists) noexcept;

} // namespace engine_ddi
#endif
