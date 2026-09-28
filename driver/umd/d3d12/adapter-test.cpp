// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12umddi.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "queue-ddi.h"
#include "fence-ddi.h"
static unsigned queryMode;
static HRESULT APIENTRY query(HANDLE adapter,const D3DDDICB_QUERYADAPTERINFO* request) {
    assert(adapter==reinterpret_cast<HANDLE>(UINT_PTR(0x1234)));
    assert(request->PrivateDriverDataSize==BC250_ADAPTER_CAPS_BYTES);
    auto bytes=static_cast<unsigned char*>(request->pPrivateDriverData);
    for(unsigned i=0;i<request->PrivateDriverDataSize;++i)assert(bytes[i]==0);
    if(queryMode==1)return E_ACCESSDENIED;
    bc250_umd_private caps{};caps.magic=BC250_UMD_PRIVATE_MAGIC;
    caps.version=BC250_UMD_PRIVATE_VERSION;caps.size=sizeof(caps);
    caps.submittable_node_mask=1;caps.hw_ip_mask=1u<<AMDGPU_HW_IP_GFX;
    caps.hw_ip[AMDGPU_HW_IP_GFX].available_rings=1;
    if(queryMode==2)caps.magic=0;
    if(queryMode==3)caps.version=2;
    if(queryMode==4)caps.submittable_node_mask=0;
    if(queryMode==5)caps.flags=BC250_UMD_F_UNMEASURED;
    if(queryMode==7)caps.hw_ip[AMDGPU_HW_IP_GFX].available_rings=0;
    if(queryMode==8)caps.size-=4;
    if(queryMode==10)caps.hw_ip_mask=0;
    memcpy(bytes,&caps,sizeof(caps));
    bc250_adapter_identity id{BC250_ADAPTER_IDENTITY_MAGIC,BC250_ADAPTER_IDENTITY_VERSION,
        BC250_ADAPTER_IDENTITY_BYTES,0x1234,0x87654321,0};
    if(queryMode==9)id.reserved=1;
    if(queryMode==11)id.version+=1;
    if(queryMode==12)id.size-=4;
    if(queryMode!=6)memcpy(bytes+BC250_ADAPTER_IDENTITY_OFFSET,&id,sizeof(id));
    return S_OK;
}
static void APIENTRY error(D3D10DDI_HRTDEVICE,HRESULT) {}
static HRESULT APIENTRY create_context(D3D12DDI_HRTCOMMANDQUEUE,D3DDDICB_CREATECONTEXTVIRTUAL* a) {a->hContext=reinterpret_cast<HANDLE>(UINT_PTR(1));return S_OK;}
static unsigned destroys;
static HRESULT APIENTRY destroy_context(D3D12DDI_HRTCOMMANDQUEUE,const D3DDDICB_DESTROYCONTEXT*) {++destroys;return E_FAIL;}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    HMODULE dll=LoadLibraryA(argv[1]);assert(dll);
    auto proc=GetProcAddress(dll,"OpenAdapter12");PFND3D12DDI_OPENADAPTER open=nullptr;
    static_assert(sizeof(open)==sizeof(proc));memcpy(&open,&proc,sizeof(open));assert(open);
    assert(open(nullptr)==E_INVALIDARG);
    D3D12DDI_ADAPTERFUNCS funcs{};D3DDDI_ADAPTERCALLBACKS callbacks{};
    D3D12DDIARG_OPENADAPTER a{};a.pAdapterFuncs=&funcs;
    assert(open(&a)==E_INVALIDARG && !a.hAdapter.pDrvPrivate && !funcs.pfnCreateDevice);
    callbacks.pfnQueryAdapterInfoCb=query;a.pAdapterCallbacks=&callbacks;
    a.hRTAdapter.handle=reinterpret_cast<HANDLE>(UINT_PTR(0x1234));
    for(queryMode=1;queryMode<=12;++queryMode){
        assert(open(&a)==(queryMode==1?E_ACCESSDENIED:E_NOINTERFACE));
        assert(!a.hAdapter.pDrvPrivate && !funcs.pfnCreateDevice);
    }
    queryMode=0;
    assert(open(&a)==S_OK && a.hAdapter.pDrvPrivate);
    assert(static_cast<native12::Adapter*>(a.hAdapter.pDrvPrivate)->contract.luid==0x8765432100001234ULL);
    UINT32 count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==1);
    UINT64 guard[2]={0xabcdef,0x123456};count=0;
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,guard)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    assert(count==1 && guard[0]==0xabcdef && guard[1]==0x123456);
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,guard)==S_OK && guard[0]==D3D12DDI_SUPPORTED_0092 && guard[1]==0x123456);
    assert(funcs.pfnCreateDevice(a.hAdapter,nullptr)==E_INVALIDARG);
    D3D12DDIARG_CALCPRIVATEDEVICESIZE sizeArgs{};
    assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs)==0);
    sizeArgs.Interface=D3D12DDI_INTERFACE_VERSION_R8;sizeArgs.Version=D3D12DDI_BUILD_VERSION_0092<<16;
    SIZE_T size=funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs);assert(size);
    void* storage=::operator new(size);
    D3D12DDIARG_CREATEDEVICE_0003 device{};device.hDrvDevice.pDrvPrivate=storage;
    device.Interface=sizeArgs.Interface;device.Version=sizeArgs.Version;
    assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_INVALIDARG);
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 um{};D3DDDI_DEVICECALLBACKS km{};
    um.pfnSetErrorCb=error;um.pfnCreateContextVirtualCb=create_context;um.pfnDestroyContextCb=destroy_context;
    device.p12UMCallbacks_0062=&um;device.pKTCallbacks=&km;
    device.Version=0;assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_NOINTERFACE);
    device.Version=sizeArgs.Version;assert(funcs.pfnCreateDevice(a.hAdapter,&device)==S_OK);
    assert(funcs.pfnCloseAdapter(a.hAdapter)==E_UNEXPECTED);
    D3D12DDI_DEVICE_FUNCS_CORE_0088 core{};native12::install_queue_entries(core);
    D3D12DDIARG_CREATECOMMANDQUEUE_0050 qargs{};qargs.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    SIZE_T qsize=core.pfnCalcPrivateCommandQueueSize(device.hDrvDevice,&qargs);assert(qsize);
    D3D12DDI_HCOMMANDQUEUE queue{};queue.pDrvPrivate=::operator new(qsize);
    assert(core.pfnCreateCommandQueue(device.hDrvDevice,&qargs,queue,{})==S_OK);
    native12::install_fence_entries(core);
    D3D12DDI_FENCE placement{};placement.FenceValue.BaseAddress=0x200030000ULL;
    D3D12DDIARG_CREATE_FENCE fa{1,&placement};
    auto fs=core.pfnCalcPrivateFenceSize(device.hDrvDevice,&fa);assert(fs);
    D3D12DDI_HFENCE fence{::operator new(fs)};
    assert(core.pfnCreateFence(device.hDrvDevice,fence,&fa)==S_OK);
    placement={};
    assert(static_cast<native12::FenceState*>(fence.pDrvPrivate)->placement.FenceValue.BaseAddress==0x200030000ULL);
    core.pfnDestroyFence(device.hDrvDevice,fence);::operator delete(fence.pDrvPrivate);
    core.pfnDestroyCommandQueue(device.hDrvDevice,queue);::operator delete(queue.pDrvPrivate);
    assert(destroys==1);
    assert(!static_cast<native12::Device*>(storage)->queues.empty());
    funcs.pfnDestroyDevice(device.hDrvDevice);::operator delete(storage);
    assert(destroys==1); // No retry with the expired runtime queue handle.

    assert(funcs.pfnFillDDITable(a.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,guard,sizeof(guard),0,{})==E_NOTIMPL);
    assert(guard[1]==0x123456);
    assert(funcs.pfnCloseAdapter(a.hAdapter)==S_OK);
    FreeLibrary(dll);puts("adapter export/negotiation/fail-closed tests passed");return 0;
}
