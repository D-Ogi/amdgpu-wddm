// SPDX-License-Identifier: MIT
#include "present-outputs.h"
#include "present-copy.h"
#include "present-copy-watch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

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

    // BD-133: AddedGpuWork is the one output the copy changes, and only when a copy was recorded.
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0x40000100,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,true},
                        &result,&contexts,&queues)==S_OK);
    assert(result.AddedGpuWork==TRUE && result.BroadcastSrcAllocation[0]==0x40000100);
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0x40000100,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,false},
                        &result,&contexts,&queues)==S_OK);
    assert(result.AddedGpuWork==FALSE);
    // A refused present reports no added work whatever the request said.
    std::memset(&result,0xa5,sizeof(result));
    assert(fill_present({0,0,0,0,context,false,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE,true},
                        &result,&contexts,&queues)==E_INVALIDARG);
    assert(result.AddedGpuWork==FALSE);
    std::puts("PASSED: AddedGpuWork is set only for a present that recorded a copy");

    // BD-133: the admission rule of a Present source that is not a linear primary (present-copy.h). The
    // description is a swap-chain back buffer's: one 2D texture, one mip, one slice, one sample.
    auto back_buffer=[](DXGI_FORMAT format,UINT64 width=1280,UINT height=720){
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc={1,0};
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        return d;
    };
    // primary: whether the destination has the primary form to fall back to (present_copy_primary_form).
    struct Admitted {DXGI_FORMAT format;unsigned bytes;unsigned d3dddi;bool blt;bool primary;};
    const Admitted admitted[]={
        {DXGI_FORMAT_B8G8R8A8_UNORM,4,D3DDDIFMT_A8R8G8B8,true,true},
        {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,4,D3DDDIFMT_A8R8G8B8,true,false},   // the sRGB view of the same storage
        {DXGI_FORMAT_R8G8B8A8_UNORM,4,D3DDDIFMT_A8B8G8R8,true,true},
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,4,D3DDDIFMT_A8B8G8R8,true,false},
        // 10-bit and FP16: admitted and copied, and the kernel driver's GPU present cannot blit either
        // (WddmLinearColorFormat). The architecture is not 8-bit only; that one component is.
        {DXGI_FORMAT_R10G10B10A2_UNORM,4,D3DDDIFMT_A2B10G10R10,false,true},
        {DXGI_FORMAT_R16G16B16A16_FLOAT,8,D3DDDIFMT_A16B16G16R16F,false,true},
        {DXGI_FORMAT_A8_UNORM,1,D3DDDIFMT_A8,false,true},
        // The D3D9 back buffer: D3D9On12 gives D3DFMT_X8R8G8B8 as B8G8R8X8_UNORM, which lab arms B and C
        // (2026-10-12 16:54Z) saw refused with "format has no linear surface", format 88. Its destination
        // carries LB7A X8R8G8B8, which the kernel driver's GPU present blits, and both formats have the primary
        // form, because neither form's record names a DXGI format (present-copy.h, present_copy_primary_form).
        {DXGI_FORMAT_B8G8R8X8_UNORM,4,D3DDDIFMT_X8R8G8B8,true,true},
        {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,4,D3DDDIFMT_X8R8G8B8,true,true},
    };
    for(const auto& a:admitted){
        PresentCopyShape shape{};
        const D3D12_RESOURCE_DESC1 d=back_buffer(a.format);
        assert(present_copy_admit(d,&shape)==PresentCopy::Admitted);
        assert(shape.width==1280 && shape.height==720 && shape.format==a.format);
        assert(shape.bytes_per_pixel==a.bytes && shape.d3dddi==a.d3dddi);
        assert(shape.kernel_blt_format==a.blt);
        assert(present_copy_kernel_blt_format(a.d3dddi)==a.blt);
        // The row the destination's memory request and the engine's linear image are sized from is the row the
        // rule admitted, and the fallback form exists exactly where the table above says.
        const AMDGPU_WDDM_SURFACE_FORMAT* row=present_copy_format(static_cast<uint32_t>(a.format));
        assert(row && row->d3dddi==a.d3dddi && row->bytes_per_pixel==a.bytes);
        assert(present_copy_primary_form(static_cast<uint32_t>(a.format))==a.primary);
        // The destination's description is the source's geometry and format, on a GPU-only L1 heap that is
        // neither a primary nor system-wide coherent, with no size of its own.
        PresentCopyRequest request{};
        present_copy_description(shape,&request);
        assert(!request.heap.ByteSize && !request.heap.Alignment);
        assert(request.heap.CPUPageProperty==D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
        assert(request.heap.MemoryPool==D3D12DDI_MEMORY_POOL_L1);
        assert(request.heap.Flags==D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES);
        assert(request.heap.CreationNodeMask==1 && request.heap.VisibleNodeMask==1);
        assert(request.resource.ResourceType==D3D12DDI_RT_TEXTURE2D);
        assert(request.resource.Width==shape.width && request.resource.Height==shape.height);
        assert(request.resource.DepthOrArraySize==1 && request.resource.MipLevels==1);
        assert(request.resource.Format==a.format && request.resource.SampleDesc.Count==1);
        assert(request.resource.Layout==D3D12DDI_TL_UNDEFINED);
        assert(request.resource.Flags==D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET);
        assert(request.resource.InitialBarrierLayout==D3D12DDI_BARRIER_LAYOUT_COMMON);
        // The primary form differs in the heap's PRIMARY flag and in nothing else.
        PresentCopyRequest as_primary=request;
        present_copy_as_primary(&as_primary);
        assert(as_primary.heap.Flags==(D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES|D3D12DDI_HEAP_FLAG_PRIMARY));
        as_primary.heap.Flags=request.heap.Flags;
        assert(!std::memcmp(&as_primary,&request,sizeof(request)));
    }
    // The negative controls, one per reason, each with the back buffer's description changed in one way.
    {
        PresentCopyShape shape{};
        D3D12_RESOURCE_DESC1 d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);
        d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        assert(present_copy_admit(d,&shape)==PresentCopy::NotTexture2D);
        assert(!shape.width && !shape.bytes_per_pixel && !shape.kernel_blt_format);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        assert(present_copy_admit(d,&shape)==PresentCopy::NotTexture2D);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.MipLevels=2;
        assert(present_copy_admit(d,&shape)==PresentCopy::Subresources);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.DepthOrArraySize=2;
        assert(present_copy_admit(d,&shape)==PresentCopy::Subresources);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.SampleDesc={4,0};
        assert(present_copy_admit(d,&shape)==PresentCopy::Multisampled);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.SampleDesc={1,1};
        assert(present_copy_admit(d,&shape)==PresentCopy::Multisampled);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,0,720),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,1280,0),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,8193,720),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,1280,8193),&shape)==PresentCopy::Edge);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM,8192,8192),&shape)==PresentCopy::Admitted);
        // A format the wire does not compose has no linear surface to copy into.
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_R32G32B32A32_FLOAT),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_R8G8B8A8_TYPELESS),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_UNKNOWN),&shape)==PresentCopy::Format);
        // The X8 row is admitted for the two DXGI formats that store its bytes and for no other: not the typeless
        // family of B8G8R8X8, and not the other D3D9 back-buffer formats (D3DFMT_R5G6B5, X1R5G5B5, A1R5G5B5),
        // which have no row at all.
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8X8_TYPELESS),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B5G6R5_UNORM),&shape)==PresentCopy::Format);
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B5G5R5A1_UNORM),&shape)==PresentCopy::Format);
        assert(!shape.width && !shape.bytes_per_pixel && !shape.d3dddi);
        assert(!present_copy_format(DXGI_FORMAT_B8G8R8X8_TYPELESS) && !present_copy_format(DXGI_FORMAT_B5G6R5_UNORM));
        assert(!present_copy_primary_form(DXGI_FORMAT_B8G8R8X8_TYPELESS));
        assert(!present_copy_primary_form(DXGI_FORMAT_R32G32B32A32_FLOAT));
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        assert(present_copy_admit(d,&shape)==PresentCopy::DepthStencil);
        // Its own reason, and not the depth-stencil one: a refusal line that named the wrong flag would send the
        // next reader after the wrong resource (review finding 7).
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags|=D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        assert(present_copy_admit(d,&shape)==PresentCopy::DenyShaderResource);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);
        d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL|D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        assert(present_copy_admit(d,&shape)==PresentCopy::DepthStencil);
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        assert(present_copy_admit(d,&shape)==PresentCopy::CrossAdapter);
        // A source that also carries the other view flags is still a source: they change nothing.
        d=back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM);
        d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        assert(present_copy_admit(d,&shape)==PresentCopy::Admitted);
        // Every reason has a name of its own, and the rule writes nothing without an answer.
        const char* names[static_cast<unsigned>(PresentCopy::Count)]={};
        for(unsigned i=0;i<static_cast<unsigned>(PresentCopy::Count);++i){
            names[i]=present_copy_text(static_cast<PresentCopy>(i));
            assert(names[i] && names[i][0]);
            for(unsigned j=0;j<i;++j)assert(std::strcmp(names[i],names[j]));
        }
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8A8_UNORM),nullptr)==PresentCopy::Admitted);
        present_copy_description(shape,nullptr);present_copy_as_primary(nullptr);
    }
    std::puts("PASSED: the Present copy admits every composed back-buffer format and refuses every other source");

    // BD-133, the D3D9 back buffer: the X8 row reaches the copy and nothing else. The table is not changed, so every
    // lookup by DXGI number that another component makes - the wire, the scan-out rule, both shells' composed
    // surfaces - still finds no row for 88 or 93; the copy's own lookup is the one that finds it.
    {
        const AMDGPU_WDDM_SURFACE_FORMAT* x8=present_copy_x8_row();
        assert(x8 && x8->d3dddi==AMDGPU_WDDM_D3DDDI_X8R8G8B8 && x8->bytes_per_pixel==4 && !x8->dxgi &&
               !x8->dxgi_srgb && !(x8->policy&AMDGPU_WDDM_SURFACE_COMPOSED) &&
               (x8->policy&AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY));
        for(const DXGI_FORMAT f:{DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB}){
            assert(!Bc250SharedSurfaceFormat(static_cast<unsigned long>(f)));
            assert(!amdgpu_wddm_surface_format_by_dxgi(static_cast<unsigned>(f)));
            assert(present_copy_own_format(static_cast<uint32_t>(f))==x8);
            assert(present_copy_format(static_cast<uint32_t>(f))==x8);
        }
        // A composed format is never the driver's own: its row stays the wire's, so its records do not change.
        for(const DXGI_FORMAT f:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_A8_UNORM}){
            assert(!present_copy_own_format(static_cast<uint32_t>(f)));
            assert(present_copy_format(static_cast<uint32_t>(f))==Bc250SharedSurfaceFormat(static_cast<unsigned long>(f)));
        }
        assert(!present_copy_own_format(DXGI_FORMAT_UNKNOWN) && !present_copy_own_format(DXGI_FORMAT_B8G8R8X8_TYPELESS));
        // The copy stays a byte copy: the destination's description has the source's own format, 88 for 88.
        PresentCopyShape shape{};
        assert(present_copy_admit(back_buffer(DXGI_FORMAT_B8G8R8X8_UNORM,256,256),&shape)==PresentCopy::Admitted);
        PresentCopyRequest request{};
        present_copy_description(shape,&request);
        assert(request.resource.Format==DXGI_FORMAT_B8G8R8X8_UNORM && request.resource.Width==256 &&
               request.resource.Height==256);
    }
    std::puts("PASSED: the D3D9 X8R8G8B8 back buffer (B8G8R8X8 and its sRGB view) is copied through the X8 row alone");

    // BD-133, review finding 8: the stage 3 -> stage 6 decision of pfnPresent, with both answers substituted.
    {
        struct Calls {
            HRESULT direct_hr,copy_hr;
            D3DKMT_HANDLE direct_allocation,copy_allocation;
            unsigned direct_calls,copy_calls;
        };
        Calls calls{};
        auto direct=[](void* state,D3DKMT_HANDLE* allocation) noexcept {
            auto& c=*static_cast<Calls*>(state);++c.direct_calls;
            if(c.direct_hr==S_OK)*allocation=c.direct_allocation;
            return c.direct_hr;
        };
        auto copy=[](void* state,D3DKMT_HANDLE* allocation) noexcept {
            auto& c=*static_cast<Calls*>(state);++c.copy_calls;
            if(c.copy_hr==S_OK)*allocation=c.copy_allocation;
            return c.copy_hr;
        };
        const PresentSourceHooks hooks{&calls,direct,copy};
        PresentSources from{};
        unsigned stage=0;
        // A linear primary: stage 3 answers, stage 6 is never asked, no added work.
        calls={S_OK,E_FAIL,0x40000100,0x40000200,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage)==S_OK);
        assert(stage==3 && from.source==0x40000100 && !from.added_gpu_work);
        assert(calls.direct_calls==1 && calls.copy_calls==0);
        // A source stage 3 refuses: stage 6 answers, its allocation goes to the runtime, added work is reported.
        calls={E_INVALIDARG,S_OK,0,0x40000200,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage)==S_OK);
        assert(stage==6 && from.source==0x40000200 && from.added_gpu_work);
        assert(calls.direct_calls==1 && calls.copy_calls==1);
        // Both refuse: the status is stage 6's, the source stays 0 and no added work is claimed. fill_present
        // then refuses as well, which is what leaves every output of the DDI zero.
        calls={E_INVALIDARG,E_OUTOFMEMORY,0,0,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage)==E_OUTOFMEMORY);
        assert(stage==6 && !from.source && !from.added_gpu_work);
        assert(fill_present(from,&result,&contexts,&queues)==E_INVALIDARG);
        // A copy that answers S_OK and names no allocation is the one case neither side could be trusted on.
        calls={E_INVALIDARG,S_OK,0,0,0,0};
        from=PresentSources{};
        assert(present_source(hooks,from,stage)==E_UNEXPECTED);
        assert(stage==6 && !from.source && !from.added_gpu_work);
        // A present is never attempted without both answers.
        from=PresentSources{};
        assert(present_source({&calls,nullptr,copy},from,stage)==E_INVALIDARG);
        assert(present_source({&calls,direct,nullptr},from,stage)==E_INVALIDARG);
    }
    std::puts("PASSED: Present asks the copy only when the zero-copy source is refused");

    // BD-133, review finding 3: the list a present recorded a copy on must be executed on the queue whose
    // present context that present answered with (present-copy-watch.h).
    {
        PresentCopyWatchOf<2> watch;
        int list_a=0,list_b=0,list_c=0;
        void* const queue_a=reinterpret_cast<void*>(UINT_PTR{0x2001});
        void* const queue_b=reinterpret_cast<void*>(UINT_PTR{0x2002});
        assert(watch.idle() && !watch.marks() && !watch.mismatches() && !watch.dropped());
        // An unmarked list is the ordinary case of every other ExecuteCommandLists, and never a mismatch.
        assert(watch.check(&list_a,queue_a)==PresentCopyExecute::NotMarked);
        assert(watch.check(nullptr,queue_a)==PresentCopyExecute::NotMarked);
        watch.mark(nullptr,queue_a);
        assert(watch.idle() && !watch.marks());
        // The pairing the whole ordering rests on, and its one failure.
        watch.mark(&list_a,queue_a);
        assert(!watch.idle() && watch.marks()==1);
        assert(watch.check(&list_a,queue_a)==PresentCopyExecute::Matched);
        assert(watch.idle() && !watch.mismatches());
        // The mark is taken by the execute, so the same list executed again is unmarked.
        assert(watch.check(&list_a,queue_a)==PresentCopyExecute::NotMarked);
        watch.mark(&list_b,queue_a);
        assert(watch.check(&list_b,queue_b)==PresentCopyExecute::Mismatch);
        assert(watch.mismatches()==1 && watch.idle());
        // Two presents on one list leave one mark, with the context of the last.
        watch.mark(&list_a,queue_a);
        watch.mark(&list_a,queue_b);
        assert(watch.check(&list_a,queue_b)==PresentCopyExecute::Matched);
        assert(watch.idle() && watch.mismatches()==1 && watch.marks()==4);
        // A full table never fails a present: the oldest mark goes and says so, and the newest is still right.
        watch.mark(&list_a,queue_a);
        watch.mark(&list_b,queue_a);
        assert(!watch.dropped());
        watch.mark(&list_c,queue_b);
        assert(watch.dropped()==1);
        assert(watch.check(&list_c,queue_b)==PresentCopyExecute::Matched);
        assert(watch.check(&list_b,queue_a)==PresentCopyExecute::Matched);
        assert(watch.check(&list_a,queue_a)==PresentCopyExecute::NotMarked);   // the dropped one
        assert(watch.idle() && watch.mismatches()==1);
        // The shell's own instance is the same class and starts empty.
        assert(present_copy_watch().idle() && !present_copy_watch().mismatches());
    }
    std::puts("PASSED: a recorded Present copy is checked against the queue its list is executed on");
    return 0;
}
