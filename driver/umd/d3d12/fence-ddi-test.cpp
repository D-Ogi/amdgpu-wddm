// SPDX-License-Identifier: MIT
#include "fence-ddi.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
    native12::Device device;D3D12DDI_HDEVICE hd{&device};
    D3D12DDI_DEVICE_FUNCS_CORE_0088 table{};native12::install_fence_entries(table);
    D3D12DDI_FENCE source{};
    source.FenceValue.BaseAddress=0x200010000ULL;
    source.FenceMonitoredValue.BaseAddress=0x200020000ULL;
    source.Flags=D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE;
    D3D12DDIARG_CREATE_FENCE args{1,&source};
    SIZE_T size=table.pfnCalcPrivateFenceSize(hd,&args);assert(size==sizeof(native12::FenceState));
    void* storage=::operator new(size+16);memset(storage,0xcd,size+16);
    D3D12DDI_HFENCE fence{storage};
    args.FenceCount=0;assert(table.pfnCreateFence(hd,fence,&args)==E_INVALIDARG);
    args.FenceCount=2;assert(table.pfnCalcPrivateFenceSize(hd,&args)==sizeof(native12::FenceState));
    assert(table.pfnCalcPrivateFenceSize(hd,nullptr)==sizeof(native12::FenceState));
    assert(table.pfnCreateFence(hd,fence,&args)==E_NOTIMPL);
    for(SIZE_T i=0;i<size+16;++i)assert(static_cast<unsigned char*>(storage)[i]==0xcd);
    args.FenceCount=1;args.Fences=nullptr;assert(table.pfnCreateFence(hd,fence,&args)==E_INVALIDARG);
    args.Fences=&source;source.Flags=static_cast<D3D12DDI_FENCE_FLAGS>(2);
    assert(table.pfnCreateFence(hd,fence,&args)==E_INVALIDARG);
    source.Flags=D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE;
    assert(table.pfnCreateFence(hd,fence,&args)==S_OK);
    source={};auto state=static_cast<native12::FenceState*>(storage);
    assert(state->device==&device && state->placement.FenceValue.BaseAddress==0x200010000ULL);
    assert(state->placement.FenceMonitoredValue.BaseAddress==0x200020000ULL);
    assert(state->placement.Flags==D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE);
    for(SIZE_T i=size;i<size+16;++i)assert(static_cast<unsigned char*>(storage)[i]==0xcd);
    table.pfnDestroyFence(hd,fence);
    device.lost.store(true);
    assert(table.pfnCalcPrivateFenceSize(hd,&args)==sizeof(native12::FenceState));
    assert(table.pfnCreateFence(hd,fence,&args)==D3DDDIERR_DEVICEREMOVED);
    ::operator delete(storage);
    puts("typed fence placement lifetime tests passed");
}
