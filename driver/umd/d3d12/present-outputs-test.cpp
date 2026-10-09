// SPDX-License-Identifier: MIT
#include "present-outputs.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace native12;
    int surface_storage=0,destination_storage=0;
    D3D12DDI_ARG_PRESENTSURFACE surface{{&surface_storage},0};
    D3D12DDIARG_PRESENT_0001 args{};
    args.phSurfacesToPresent=&surface;args.SurfacesToPresent=1;
    args.VidPnSourceID=D3DDDI_ID_UNINITIALIZED;args.OptimizeForComposition=TRUE;
    D3D12DDI_PRESENT_0051 result;D3D12DDI_PRESENT_CONTEXTS_0051 contexts;D3D12DDI_PRESENT_HWQUEUES_0051 queues;

    assert(check_present(&args,&result,&contexts)==S_OK);
    assert(check_present(nullptr,&result,&contexts)==E_INVALIDARG);
    assert(check_present(&args,nullptr,&contexts)==E_INVALIDARG);
    assert(check_present(&args,&result,nullptr)==E_INVALIDARG);
    auto bad=args;bad.SurfacesToPresent=2;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.SurfacesToPresent=0;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.phSurfacesToPresent=nullptr;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    D3D12DDI_ARG_PRESENTSURFACE second{{&surface_storage},1};
    bad=args;bad.phSurfacesToPresent=&second;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    D3D12DDI_ARG_PRESENTSURFACE none{{nullptr},0};
    bad=args;bad.phSurfacesToPresent=&none;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.hDstResource.pDrvPrivate=&destination_storage;
    assert(check_present(&bad,&result,&contexts)==S_OK);
    bad.DstSubResourceIndex=1;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.DirtyRects=1;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);
    bad=args;bad.PrivateDriverDataSize=4;assert(check_present(&bad,&result,&contexts)==E_INVALIDARG);

    const HANDLE context=reinterpret_cast<HANDLE>(UINT_PTR{0x10000011});
    std::memset(&result,0xa5,sizeof(result));std::memset(&contexts,0xa5,sizeof(contexts));
    std::memset(&queues,0xa5,sizeof(queues));
    assert(fill_present({0x40000100,0,0,0,context},&result,&contexts,&queues)==S_OK);
    assert(result.BroadcastSrcAllocation[0]==0x40000100 && !result.BroadcastDstAllocation[0]);
    assert(!result.AddedGpuWork && result.BackBufferMultiplicity==1 && !result.SyncIntervalOverrideValid);
    for(unsigned i=1;i<=D3DDDI_MAX_BROADCAST_CONTEXT;++i)
        assert(!result.BroadcastSrcAllocation[i] && !result.BroadcastDstAllocation[i]);
    assert(contexts.hContext==context && !contexts.BroadcastContextCount && !contexts.BroadcastContext[0]);
    assert(!queues.BroadcastQueueCount && !queues.hHwQueues[0]);

    assert(fill_present({0x40000100,0,0x40000200,0,context},&result,&contexts,nullptr)==S_OK);
    assert(result.BroadcastDstAllocation[0]==0x40000200);

    // The per-application VSync setting: the override reaches the runtime as given, and only when asked for.
    const DXGI_DDI_FLIP_INTERVAL_TYPE intervals[]={DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,DXGI_DDI_FLIP_INTERVAL_ONE};
    for(const auto interval:intervals){
        std::memset(&result,0xa5,sizeof(result));
        assert(fill_present({0x40000100,0,0,0,context,true,interval},&result,&contexts,nullptr)==S_OK);
        assert(result.SyncIntervalOverrideValid==TRUE && result.SyncIntervalOverride==interval);
    }
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0x40000100,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_ONE},&result,&contexts,nullptr)==S_OK);
    assert(result.SyncIntervalOverrideValid==FALSE && result.SyncIntervalOverride==DXGI_DDI_FLIP_INTERVAL_IMMEDIATE);

    // A refusal leaves every output zero.
    std::memset(&result,0xa5,sizeof(result));std::memset(&contexts,0xa5,sizeof(contexts));
    std::memset(&queues,0xa5,sizeof(queues));
    assert(fill_present({0,0,0,0,context},&result,&contexts,&queues)==E_INVALIDARG);
    assert(!result.BroadcastSrcAllocation[0] && !result.BackBufferMultiplicity && !contexts.hContext);
    assert(!queues.BroadcastQueueCount);
    assert(fill_present({0x40000100,4096,0,0,context},&result,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0x40000200,4096,context},&result,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0,0,nullptr},&result,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0,0,context},nullptr,&contexts,&queues)==E_INVALIDARG);
    assert(fill_present({0x40000100,0,0,0,context},&result,nullptr,&queues)==E_INVALIDARG);
    std::puts("PASSED: present outputs for one surface on a software-scheduled queue, refusals leave zeroes");
    return 0;
}
