// SPDX-License-Identifier: MIT
#include "paging.h"
#include <cassert>
#include <cstdio>
static UINT64 completed;
static bool asynchronous=true,failFree,failDestroy;
static unsigned maps,frees,destroys,waits;
static HRESULT wait_result=S_OK;
static bool lose_during_wait;
static HANDLE expected_context=reinterpret_cast<HANDLE>(UINT_PTR(0x10000031u));
static HANDLE owner=reinterpret_cast<HANDLE>(UINT_PTR(27));
static HRESULT APIENTRY create(HANDLE d,D3DDDICB_CREATEPAGINGQUEUE* a){
    assert(d==owner);a->hPagingQueue=5;a->hSyncObject=6;a->FenceValueCPUVirtualAddress=&completed;return S_OK;
}
static HRESULT APIENTRY destroy(HANDLE d,const D3DDDI_DESTROYPAGINGQUEUE* a){
    assert(d==owner && a->hPagingQueue==5);++destroys;return failDestroy?E_FAIL:S_OK;
}
static HRESULT APIENTRY map(HANDLE d,D3DDDI_MAPGPUVIRTUALADDRESS* a){
    assert(d==owner && a->hPagingQueue==5 && a->hAllocation==9 && a->SizeInPages==16 && a->Protection.Write);
    ++maps;a->VirtualAddress=0x200010000ull;a->PagingFenceValue=7;
    return asynchronous?E_PENDING:S_OK;
}
static HRESULT APIENTRY free_va(HANDLE d,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){
    assert(d==owner && a->BaseAddress==0x200010000ull && a->Size==65536);++frees;return failFree?E_FAIL:S_OK;
}
static unsigned makes,evicts,cpu_waits;
static HRESULT make_result=E_PENDING,evict_result=S_OK,cpu_wait_result=S_OK;
static UINT64 make_fence=11,cpu_waited;
static bool cpu_wait_completes=true;
static HRESULT APIENTRY make_resident(HANDLE d,D3DDDI_MAKERESIDENT* a){
    assert(d==owner && a->hPagingQueue==5 && a->NumAllocations==1 && a->AllocationList[0]==9 && a->Flags.Value==1 && a->Flags.CantTrimFurther);
    ++makes;a->PagingFenceValue=make_result==E_PENDING?make_fence:0;return make_result;
}
static HRESULT APIENTRY evict(HANDLE d,D3DDDICB_EVICT* a){
    assert(d==owner && a->NumAllocations==1 && a->AllocationList[0]==9 && !a->Flags.Value);++evicts;return evict_result;
}
static HRESULT APIENTRY wait_cpu(HANDLE d,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* a){
    assert(d==owner && a->ObjectCount==1 && a->ObjectHandleArray[0]==6 && !a->hAsyncEvent);
    ++cpu_waits;cpu_waited=a->FenceValueArray[0];
    if(cpu_wait_result==S_OK && cpu_wait_completes) completed=cpu_waited;
    return cpu_wait_result;
}
static HRESULT APIENTRY wait_gpu(HANDLE d,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a){
    assert(d==owner && a->hContext==expected_context && a->ObjectCount==1);
    assert(a->ObjectHandleArray[0]==6 && a->MonitoredFenceValueArray[0]==7);
    ++waits;if(lose_during_wait) completed=UINT64_MAX;
    // The mock only queues the wait. It deliberately does not advance the fence.
    return wait_result;
}
// ---- BD-101: the alignment of a mapping's GPU virtual address ------------------------------------------------
// Own callbacks, so that the cases below can name their own sizes and addresses without touching the
// expectations of the mocks above.
static UINT64 reserve_base=0x300040000ull;      // 64 KiB aligned, deliberately not 4 MiB aligned
static UINT64 reserved_size,mapped_base,freed_base,freed_size;
static unsigned reserves,aligned_maps,aligned_frees;
static HRESULT reserve_result=S_OK,aligned_map_result=S_OK;
static UINT64 map_answers_address;              // 0: the mock answers the base it was given
static bool reserve_gives_nothing;              // an acceptance that places no range
static HRESULT APIENTRY reserve_a(HANDLE d,D3DDDI_RESERVEGPUVIRTUALADDRESS* a){
    // Nothing of the reservation is pinned: no base, no window, and a size that is a whole number of 64 KiB
    // pages, as D3DDDI_RESERVEGPUVIRTUALADDRESS requires.
    assert(d==owner && !a->BaseAddress && !a->MinimumAddress && !a->MaximumAddress);
    assert(a->Size && !(a->Size&(65536-1)));
    ++reserves;reserved_size=a->Size;
    if(FAILED(reserve_result)) return reserve_result;
    a->VirtualAddress=reserve_gives_nothing?0:reserve_base;
    return reserve_result;
}
static HRESULT APIENTRY map_a(HANDLE d,D3DDDI_MAPGPUVIRTUALADDRESS* a){
    assert(d==owner && a->hPagingQueue==5 && a->hAllocation==9 && a->Protection.Write);
    ++aligned_maps;mapped_base=a->BaseAddress;
    if(FAILED(aligned_map_result)) return aligned_map_result;
    a->VirtualAddress=map_answers_address?map_answers_address:
        (a->BaseAddress?a->BaseAddress:0x400000000ull);
    a->PagingFenceValue=0;
    return S_OK;
}
static HRESULT APIENTRY free_a(HANDLE d,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){
    assert(d==owner);++aligned_frees;freed_base=a->BaseAddress;freed_size=a->Size;return S_OK;
}
static void alignment_cases(){
    D3DDDI_DEVICECALLBACKS cb{};cb.pfnCreatePagingQueueCb=create;cb.pfnDestroyPagingQueueCb=destroy;
    cb.pfnMapGpuVirtualAddressCb=map_a;cb.pfnFreeGpuVirtualAddressCb=free_a;
    cb.pfnReserveGpuVirtualAddressCb=reserve_a;
    native12::PagingDomain d({owner},cb);assert(d.open()==S_OK);
    UINT64 address=0;
    constexpr UINT64 k64=65536,k4M=4ull<<20,bytes=46137344;   // the 4x MSAA target of BD-101, 44 MiB
    // 64 KiB: the request of every heap of this driver before BD-101. No reservation is taken, the map is
    // asked for no base, and the release frees the mapped range alone - the behaviour this change must not
    // alter.
    {
        native12::GpuMapping m;completed=0;reserves=0;aligned_maps=0;aligned_frees=0;mapped_base=1;
        assert(d.map(9,k64,m,k64)==S_OK && !reserves && aligned_maps==1 && !mapped_base);
        assert(d.ready(m,&address)==S_OK && address==0x400000000ull && !d.reservations());
        assert(d.unmap_after_gpu_retirement(m)==S_OK && aligned_frees==1);
        assert(freed_base==0x400000000ull && freed_size==k64);
    }
    // 4 MiB (D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT): one reservation of bytes + alignment, rounded
    // to 64 KiB, the map at the first 4 MiB boundary inside it, and the address the engine is given is that
    // boundary. The reservation base is 64 KiB aligned and not 4 MiB aligned, so the offset is not zero.
    {
        native12::GpuMapping m;completed=0;reserves=0;aligned_maps=0;aligned_frees=0;
        assert(d.map(9,bytes,m,k4M)==S_OK);
        assert(reserves==1 && reserved_size==bytes+k4M && d.reservations()==1 && !d.reservation_failures());
        const UINT64 aligned=(reserve_base+k4M-1)&~(k4M-1);
        assert(aligned>reserve_base && aligned+bytes<=reserve_base+reserved_size);
        assert(aligned_maps==1 && mapped_base==aligned);
        assert(d.ready(m,&address)==S_OK && address==aligned && !(address&(k4M-1)));
        // One free, of the whole reservation: the two pieces outside the mapping go back with it.
        assert(d.unmap_after_gpu_retirement(m)==S_OK && aligned_frees==1);
        assert(freed_base==reserve_base && freed_size==bytes+k4M);
        // The second release of the same mapping does nothing.
        assert(d.unmap_after_gpu_retirement(m)==S_OK && aligned_frees==1);
    }
    // A reservation base the runtime placed on a 4 KiB boundary, which no page of the DDI forbids: the
    // aligned window is still inside the range, because the span is bytes plus the alignment.
    {
        const UINT64 saved=reserve_base;reserve_base=UINT64_C(0x300041000);
        native12::GpuMapping m;completed=0;reserves=0;aligned_maps=0;aligned_frees=0;
        assert(d.map(9,bytes,m,k4M)==S_OK && reserves==1 && reserved_size==bytes+k4M);
        const UINT64 aligned=(reserve_base+k4M-1)&~(k4M-1);
        assert(mapped_base==aligned && d.ready(m,&address)==S_OK && address==aligned);
        assert(d.unmap_after_gpu_retirement(m)==S_OK && aligned_frees==1);
        assert(freed_base==reserve_base && freed_size==bytes+k4M);
        reserve_base=saved;
    }
    // Negative controls.
    {
        native12::GpuMapping m;completed=0;
        // An alignment that is not a power of two is refused before any callback.
        reserves=0;aligned_maps=0;
        assert(d.map(9,k64,m,96)==E_INVALIDARG && !reserves && !aligned_maps);
        // A refused reservation: the request fails with the runtime's own code, no map is attempted, and
        // nothing is freed.
        reserve_result=E_OUTOFMEMORY;reserves=0;aligned_maps=0;aligned_frees=0;
        assert(d.map(9,bytes,m,k4M)==E_OUTOFMEMORY && reserves==1 && !aligned_maps && !aligned_frees);
        reserve_result=S_OK;
        // An acceptance that placed no range: nothing to map and nothing to free.
        reserve_gives_nothing=true;reserves=0;aligned_maps=0;aligned_frees=0;
        assert(d.map(9,bytes,m,k4M)==E_UNEXPECTED && reserves==1 && !aligned_maps && !aligned_frees);
        reserve_gives_nothing=false;
        // A refused map gives the reservation back at once.
        aligned_map_result=E_OUTOFMEMORY;reserves=0;aligned_maps=0;aligned_frees=0;
        assert(d.map(9,bytes,m,k4M)==E_OUTOFMEMORY && reserves==1 && aligned_maps==1);
        assert(aligned_frees==1 && freed_base==reserve_base && freed_size==bytes+k4M);
        aligned_map_result=S_OK;
        // An accepted map that answers another address than the base it was given is not a mapping: the
        // reservation goes back and the mapping is left empty, so the caller's release has nothing to do.
        map_answers_address=reserve_base;reserves=0;aligned_maps=0;aligned_frees=0;
        assert(d.map(9,bytes,m,k4M)==E_UNEXPECTED && reserves==1 && aligned_maps==1 && aligned_frees==1);
        assert(d.ready(m,&address)==E_INVALIDARG && !address);
        assert(d.unmap_after_gpu_retirement(m)==S_OK && aligned_frees==1);
        map_answers_address=0;
        // Without the reservation callback an alignment above the granularity cannot be asked for at all,
        // and no map is attempted on a promise the domain cannot keep.
        D3DDDI_DEVICECALLBACKS bare=cb;bare.pfnReserveGpuVirtualAddressCb=nullptr;
        native12::PagingDomain nr({owner},bare);native12::GpuMapping n;
        assert(nr.open()==S_OK);reserves=0;aligned_maps=0;
        assert(nr.map(9,bytes,n,k4M)==E_UNEXPECTED && !reserves && !aligned_maps);
        // The same domain still maps a 64 KiB request.
        completed=0;assert(nr.map(9,k64,n,k64)==S_OK && aligned_maps==1);
        assert(nr.unmap_after_gpu_retirement(n)==S_OK && nr.close()==S_OK);
    }
    assert(d.close()==S_OK);
}

int main(){
    D3DDDI_DEVICECALLBACKS cb{};cb.pfnCreatePagingQueueCb=create;cb.pfnDestroyPagingQueueCb=destroy;
    cb.pfnMapGpuVirtualAddressCb=map;cb.pfnFreeGpuVirtualAddressCb=free_va;
    cb.pfnWaitForSynchronizationObjectFromGpuCb=wait_gpu;
    native12::PagingDomain a({owner},cb),b({owner},cb);assert(a.open()==S_OK && b.open()==S_OK);
    native12::GpuMapping m;UINT64 address=99;
    assert(a.map(9,65536,m)==S_OK);assert(a.ready(m,&address)==E_PENDING && !address);
    assert(a.address_for_context(m,expected_context,&address)==S_OK && address==0x200010000ull && waits==1);
    assert(completed==0 && a.ready(m,&address)==E_PENDING && !address);
    // Each independent context gets its own ordering; no global ready-state cache.
    expected_context=reinterpret_cast<HANDLE>(UINT_PTR(0x20000031u));
    assert(a.address_for_context(m,expected_context,&address)==S_OK && address && waits==2);
    wait_result=E_FAIL;
    assert(a.address_for_context(m,expected_context,&address)==E_FAIL && !address && waits==3);
    wait_result=S_FALSE;
    assert(a.address_for_context(m,expected_context,&address)==E_UNEXPECTED && !address && waits==4);
    wait_result=S_OK;lose_during_wait=true;
    assert(a.address_for_context(m,expected_context,&address)==D3DDDIERR_DEVICEREMOVED && !address && waits==5);
    lose_during_wait=false;completed=0;
    assert(a.address_for_context(m,nullptr,&address)==E_INVALIDARG && !address && waits==5);
    assert(b.address_for_context(m,expected_context,&address)==E_INVALIDARG && !address && waits==5);
    assert(a.close()==E_PENDING && !destroys);
    assert(a.unmap_after_gpu_retirement(m)==E_PENDING && !frees);
    assert(b.ready(m,&address)==E_INVALIDARG && !address);
    auto count=maps;assert(a.map(9,65536,m)==E_UNEXPECTED && maps==count);
    completed=7;assert(a.ready(m,&address)==S_OK && address==0x200010000ull);
    auto wait_count=waits;
    assert(a.address_for_context(m,expected_context,&address)==S_OK && address && waits==wait_count);
    failFree=true;assert(a.unmap_after_gpu_retirement(m)==E_FAIL);
    assert(a.ready(m,&address)==S_OK && address);failFree=false;
    assert(a.unmap_after_gpu_retirement(m)==S_OK);count=frees;
    assert(a.unmap_after_gpu_retirement(m)==S_OK && frees==count);
    // S_OK must ignore the callback's unrelated fence output.
    asynchronous=false;completed=0;assert(a.map(9,65536,m)==S_OK);
    assert(a.ready(m,&address)==S_OK && address);
    completed=UINT64_MAX;assert(a.ready(m,&address)==D3DDDIERR_DEVICEREMOVED && !address);
    completed=0;assert(a.unmap_after_gpu_retirement(m)==S_OK);
    failDestroy=true;assert(a.close()==E_FAIL);failDestroy=false;assert(a.close()==S_OK);
    assert(b.close()==S_OK);
    native12::PagingDomain expired({owner},cb);assert(expired.open()==S_OK);
    assert(expired.map(9,65536,m)==S_OK);expired.invalidate_runtime();count=frees;
    assert(expired.ready(m,&address)==E_UNEXPECTED && !address);
    assert(expired.unmap_after_gpu_retirement(m)==E_UNEXPECTED && frees==count);
    wait_count=waits;
    assert(expired.address_for_context(m,expected_context,&address)==E_UNEXPECTED && !address && waits==wait_count);
    cb.pfnWaitForSynchronizationObjectFromGpuCb=nullptr;
    native12::PagingDomain no_wait({owner},cb);native12::GpuMapping pending;
    asynchronous=true;completed=0;assert(no_wait.open()==S_OK && no_wait.map(9,65536,pending)==S_OK);
    assert(no_wait.address_for_context(pending,expected_context,&address)==E_UNEXPECTED && !address && waits==wait_count);
    completed=7;assert(no_wait.unmap_after_gpu_retirement(pending)==S_OK && no_wait.close()==S_OK);
    // The domain's own residency reference (trial 153): its pending value joins the mapping's,
    // the CPU wait asks for the largest, and the reference is released before the VA.
    cb.pfnWaitForSynchronizationObjectFromGpuCb=wait_gpu;cb.pfnMakeResidentCb=make_resident;cb.pfnEvictCb=evict;
    cb.pfnWaitForSynchronizationObjectFromCpuCb=wait_cpu;
    native12::PagingDomain r({owner},cb);assert(r.open()==S_OK);
    native12::GpuMapping held;asynchronous=true;completed=0;
    assert(r.map(9,65536,held)==S_OK && r.make_resident(held)==S_OK && makes==1);
    assert(r.ready(held,&address)==E_PENDING && !address);
    completed=7;assert(r.ready(held,&address)==E_PENDING && !address);   // the map alone is not enough
    // A GPU wait on a context also asks for the residency's value.
    expected_context=reinterpret_cast<HANDLE>(UINT_PTR(0x10000031u));
    {
        auto gpu_wait=[](HANDLE d,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a)->HRESULT {
            assert(d==owner && a->ObjectCount==1 && a->ObjectHandleArray[0]==6 && a->MonitoredFenceValueArray[0]==11);
            return S_OK;
        };
        D3DDDI_DEVICECALLBACKS g=cb;g.pfnWaitForSynchronizationObjectFromGpuCb=gpu_wait;
        native12::PagingDomain gq({owner},g);native12::GpuMapping gm;completed=0;
        assert(gq.open()==S_OK && gq.map(9,65536,gm)==S_OK && gq.make_resident(gm)==S_OK);
        assert(gq.address_for_context(gm,expected_context,&address)==S_OK && address==0x200010000ull);
        completed=11;assert(gq.unmap_after_gpu_retirement(gm)==S_OK && gq.close()==S_OK);completed=7;
    }
    assert(r.make_resident(held)==E_UNEXPECTED && makes==2);                  // one reference per mapping
    // Not ready yet: nothing is freed or evicted.
    auto e0=evicts,f0=frees;assert(r.unmap_after_gpu_retirement(held)==E_PENDING && evicts==e0 && frees==f0);
    assert(r.wait_ready(held,&address)==S_OK && address==0x200010000ull && cpu_waited==11 && completed==11);
    // A wait that fails or returns before the value is reached is no readiness.
    completed=7;cpu_wait_result=E_FAIL;assert(r.wait_ready(held,&address)==E_FAIL && !address);cpu_wait_result=S_OK;
    cpu_wait_completes=false;assert(r.wait_ready(held,&address)==E_UNEXPECTED && !address);cpu_wait_completes=true;
    completed=UINT64_MAX;assert(r.wait_ready(held,&address)==D3DDDIERR_DEVICEREMOVED && !address);
    // Ready: no wait at all.
    completed=11;auto w0=cpu_waits;assert(r.wait_ready(held,&address)==S_OK && cpu_waits==w0);
    // A failed eviction is counted and does not hold the VA. A failed free keeps the mapping, and
    // its retry does not evict again.
    evict_result=E_FAIL;e0=evicts;f0=frees;failFree=true;
    assert(r.unmap_after_gpu_retirement(held)==E_FAIL && evicts==e0+1 && frees==f0+1 && r.evict_failures()==1);
    evict_result=S_OK;failFree=false;
    assert(r.unmap_after_gpu_retirement(held)==S_OK && evicts==e0+1 && frees==f0+2 && r.evict_failures()==1);
    // Immediately resident: no fence to add; the mapping's own value still counts.
    make_result=S_OK;completed=0;
    assert(r.map(9,65536,held)==S_OK && r.make_resident(held)==S_OK && r.ready(held,&address)==E_PENDING);
    completed=7;assert(r.ready(held,&address)==S_OK && r.unmap_after_gpu_retirement(held)==S_OK);
    // A refusal takes nothing: no eviction later. A malformed acceptance still holds the reference.
    make_result=E_OUTOFMEMORY;e0=evicts;
    assert(r.map(9,65536,held)==S_OK && r.make_resident(held)==E_OUTOFMEMORY);
    assert(r.unmap_after_gpu_retirement(held)==S_OK && evicts==e0);
    make_result=E_PENDING;make_fence=0;
    assert(r.map(9,65536,held)==S_OK && r.make_resident(held)==E_UNEXPECTED);
    assert(r.unmap_after_gpu_retirement(held)==S_OK && evicts==e0+1);make_fence=11;
    // A lower residency value never lowers the mapping's.
    make_fence=3;completed=0;assert(r.map(9,65536,held)==S_OK && r.make_resident(held)==S_OK);
    assert(r.wait_ready(held,&address)==S_OK && cpu_waited==7);make_fence=11;
    assert(r.unmap_after_gpu_retirement(held)==S_OK);
    // Without the callbacks, or unmapped, there is no reference to take.
    native12::GpuMapping none;assert(r.make_resident(none)==E_INVALIDARG);
    assert(r.close()==S_OK);
    D3DDDI_DEVICECALLBACKS bare=cb;bare.pfnMakeResidentCb=nullptr;
    native12::PagingDomain nr({owner},bare);assert(nr.open()==S_OK && nr.map(9,65536,held)==S_OK);
    assert(nr.make_resident(held)==E_UNEXPECTED);completed=7;
    assert(nr.unmap_after_gpu_retirement(held)==S_OK && nr.close()==S_OK);
    alignment_cases();
    puts("paging ownership, context GPU waits, pending VA, loss and callback lifetime gates passed; "
         "own residency reference (CantTrimFurther) joins the fence, CPU wait for the largest value, evicted before "
         "the VA is freed, a failed eviction does not hold the VA; BD-101: 64 KiB maps as before, 4 MiB maps at an "
         "aligned offset inside a reservation of bytes+alignment and frees the whole reservation, and every refusal "
         "of that route gives the reservation back");
}
