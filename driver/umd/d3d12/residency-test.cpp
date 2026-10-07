// SPDX-License-Identifier: MIT
#include "residency.h"
#include <array>
#include <cassert>
#include <cstdio>

static HANDLE device=reinterpret_cast<HANDLE>(UINT_PTR(0x1234));
static HANDLE queue=reinterpret_cast<HANDLE>(UINT_PTR(0x10002345u));
static std::array<D3DKMT_HANDLE,3> handles{7,9,11};
static HRESULT make_status=S_OK,evict_status=S_OK;
static UINT64 fence=42;
static bool malformed_count;
static unsigned makes,evicts;
static HRESULT APIENTRY make(D3D12DDI_HRTDEVICE d,D3D12DDI_HRTPAGINGQUEUE q,D3DDDI_MAKERESIDENT* a){
    assert(d.handle==device && q.handle==queue);
    assert(a->hPagingQueue==0 && a->NumAllocations==handles.size() && !a->PriorityList);
    assert(a->Flags.CantTrimFurther && !a->Flags.MustSucceed && !a->Flags.Reserved);
    for(size_t i=0;i<handles.size();++i) assert(a->AllocationList[i]==handles[i]);
    ++makes;a->PagingFenceValue=fence;a->NumBytesToTrim=65536;
    if(malformed_count) --a->NumAllocations;
    return make_status;
}
static HRESULT APIENTRY evict(D3D12DDI_HRTDEVICE d,const D3DDDICB_EVICT* a){
    assert(d.handle==device && a->NumAllocations==handles.size());
    for(size_t i=0;i<handles.size();++i) assert(a->AllocationList[i]==handles[i]);
    assert(a->Flags.NotWrittenTo && !a->Flags.EvictOnlyIfNecessary && !a->Flags.Reserved);
    ++evicts;return evict_status;
}
int main(){
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 cb{};cb.pfnMakeResidentCb=make;cb.pfnEvictCb=evict;
    native12::RuntimeResidency residency({device},cb);cb={};
    D3DDDI_MAKERESIDENT_FLAGS flags{};flags.CantTrimFurther=1;
    D3DDDI_EVICT_FLAGS evict_flags{};evict_flags.NotWrittenTo=1;
    native12::ResidencyResult result;
    using State=native12::ResidencyState;
    assert(residency.make_resident({queue},handles,flags,result)==S_OK);
    assert(result.state==State::Ready && !result.paging_fence && !result.bytes_to_trim && makes==1);
    make_status=E_PENDING;
    assert(residency.make_resident({queue},handles,flags,result)==E_PENDING);
    assert(result.state==State::Pending && result.paging_fence==42 && result.callback_status==E_PENDING);
    assert(!result.bytes_to_trim && makes==2 && evicts==0);
    make_status=E_OUTOFMEMORY;
    assert(residency.make_resident({queue},handles,flags,result)==E_OUTOFMEMORY);
    assert(result.state==State::Failed && result.bytes_to_trim==65536 && !result.paging_fence);
    assert(makes==3 && !evicts); // No retry, split batch or unnecessary rollback.
    make_status=D3DDDIERR_DEVICEREMOVED;
    assert(residency.make_resident({queue},handles,flags,result)==D3DDDIERR_DEVICEREMOVED);
    assert(result.state==State::Failed && !result.paging_fence && !result.bytes_to_trim);
    make_status=S_OK;malformed_count=true;
    assert(residency.make_resident({queue},handles,flags,result)==E_UNEXPECTED);
    assert(result.state==State::Unknown && result.callback_status==S_OK && !result.paging_fence);
    malformed_count=false;make_status=E_PENDING;
    for(UINT64 invalid: {UINT64(0),UINT64_MAX}){
        fence=invalid;
        assert(residency.make_resident({queue},handles,flags,result)==E_UNEXPECTED);
        assert(result.state==State::Unknown && result.callback_status==E_PENDING && !result.paging_fence);
    }
    make_status=S_FALSE;
    assert(residency.make_resident({queue},handles,flags,result)==E_UNEXPECTED && result.state==State::Unknown);
    auto before=makes;
    assert(residency.make_resident({},handles,flags,result)==E_INVALIDARG);
    assert(residency.make_resident({queue},{},flags,result)==E_INVALIDARG);
    std::array<D3DKMT_HANDLE,2> bad{7,0};
    assert(residency.make_resident({queue},bad,flags,result)==E_INVALIDARG);
    flags.Reserved=1;assert(residency.make_resident({queue},handles,flags,result)==E_INVALIDARG);flags.Reserved=0;
    assert(makes==before && result.state==State::Failed && !result.paging_fence);
    evict_status=E_FAIL;
    assert(residency.evict_after_gpu_retirement(handles,evict_flags)==E_FAIL && evicts==1);
    evict_status=S_OK;
    assert(residency.evict_after_gpu_retirement(handles,evict_flags)==S_OK && evicts==2);
    // Two successful makes are two OS references; the bridge must not deduplicate.
    make_status=S_OK;
    assert(residency.make_resident({queue},handles,flags,result)==S_OK);
    assert(residency.make_resident({queue},handles,flags,result)==S_OK && makes==before+2);
    residency.invalidate_runtime();before=makes;
    assert(residency.make_resident({queue},handles,flags,result)==E_UNEXPECTED && makes==before);
    assert(residency.evict_after_gpu_retirement(handles,evict_flags)==E_UNEXPECTED && evicts==2);
    assert(result.state==State::Failed && !result.paging_fence);
    puts("native residency batch, pending fence, OOM and callback lifetime gates passed");
}
