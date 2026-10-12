// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <cstdint>

namespace native12 {
// pfnPresent returns what the runtime's kernel present takes: the allocation of the surface, the allocation of
// the destination when one is named, and the context of the queue. Nothing is recorded into the list and no
// kernel call is made here. Contract: WDK 10.0.26100 d3d12umddi.h D3D12DDIARG_PRESENT_0001,
// D3D12DDI_PRESENT_0051, D3D12DDI_PRESENT_CONTEXTS_0051, D3D12DDI_PRESENT_HWQUEUES_0051.
//
// One surface, its first subresource, a software-scheduled queue: anything else is refused, not guessed.
inline HRESULT check_present(const D3D12DDIARG_PRESENT_0001* args,const D3D12DDI_PRESENT_0051* result,
                             const D3D12DDI_PRESENT_CONTEXTS_0051* contexts) noexcept {
    if(!args || !result || !contexts)return E_INVALIDARG;
    if(args->SurfacesToPresent!=1 || !args->phSurfacesToPresent)return E_INVALIDARG;
    if(!args->phSurfacesToPresent[0].hSurface.pDrvPrivate || args->phSurfacesToPresent[0].SubResourceIndex)
        return E_INVALIDARG;
    if(args->hDstResource.pDrvPrivate ? args->DstSubResourceIndex!=0 : false)return E_INVALIDARG;
    if((args->DirtyRects && !args->pDirtyRects) || (args->PrivateDriverDataSize && !args->pPrivateDriverData))
        return E_INVALIDARG;
    return S_OK;
}
struct PresentSources {
    D3DKMT_HANDLE source{};
    uint64_t source_offset{};
    D3DKMT_HANDLE destination{};                // 0: the call named no destination
    uint64_t destination_offset{};
    HANDLE context{};
    // The per-application VSync setting (docs/design/per-app-graphics-settings.md): when sync_override_valid, the
    // runtime presents with sync_override in place of the application's sync interval.
    bool sync_override_valid{};
    DXGI_DDI_FLIP_INTERVAL_TYPE sync_override{DXGI_DDI_FLIP_INTERVAL_IMMEDIATE};
    // BD-133: this present recorded a copy into the command list the runtime handed to pfnPresent, because the
    // source is not a surface the kernel present can read (present-copy.h). D3D12DDI_PRESENT_0051::AddedGpuWork
    // "specifies whether there is added GPU work" (ref\ddi-display\d3d12umddi.md:42409), which is what the
    // recorded copy is. A zero-copy present of a linear primary leaves it FALSE, exactly as before.
    bool added_gpu_work{};
};
// The surface is the whole allocation from its first byte: an offset is a surface this driver did not make.
inline HRESULT fill_present(const PresentSources& from,D3D12DDI_PRESENT_0051* result,
                            D3D12DDI_PRESENT_CONTEXTS_0051* contexts,
                            D3D12DDI_PRESENT_HWQUEUES_0051* queues) noexcept {
    if(!result || !contexts)return E_INVALIDARG;
    *result={};*contexts={};if(queues)*queues={};
    if(!from.source || from.source_offset || from.destination_offset || !from.context)return E_INVALIDARG;
    result->BroadcastSrcAllocation[0]=from.source;
    result->BroadcastDstAllocation[0]=from.destination;
    result->AddedGpuWork=from.added_gpu_work ? TRUE : FALSE;
    result->BackBufferMultiplicity=1;
    result->SyncIntervalOverrideValid=from.sync_override_valid ? TRUE : FALSE;
    result->SyncIntervalOverride=from.sync_override_valid ? from.sync_override : DXGI_DDI_FLIP_INTERVAL_IMMEDIATE;
    contexts->hContext=from.context;
    contexts->BroadcastContextCount=0;
    return S_OK;
}
}
