// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12umddi.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "device-state.h"
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
static HRESULT APIENTRY destroy_context(D3D12DDI_HRTCOMMANDQUEUE,const D3DDDICB_DESTROYCONTEXT*) {return E_FAIL;}
// Exercise the exported DLL boundary, not the in-process composition helpers.
// Publication alone must not instantiate an engine device or submit GPU work.
template<class T>
static void rejected_table(const D3D12DDI_ADAPTERFUNCS& funcs,D3D12DDI_HADAPTER adapter,
    D3D12DDI_TABLE_TYPE type,SIZE_T size,UINT number,D3D12DDI_HRTTABLE runtime,
    HRESULT expected=E_INVALIDARG) {
    struct Guarded {UINT64 before;T table;UINT64 after;} storage;
    memset(&storage,0xa5,sizeof(storage));
    unsigned char snapshot[sizeof(storage)];memcpy(snapshot,&storage,sizeof(storage));
    assert(funcs.pfnFillDDITable(adapter,type,&storage.table,size,number,runtime)==expected);
    assert(memcmp(snapshot,&storage,sizeof(storage))==0);
}
template<class T>
static T published_table(const D3D12DDI_ADAPTERFUNCS& funcs,D3D12DDI_HADAPTER adapter,
    D3D12DDI_TABLE_TYPE type,UINT number=0,D3D12DDI_HRTTABLE runtime={}) {
    struct Guarded {UINT64 before;T table;UINT64 after;} storage{};
    storage.before=0x123456789abcdef0ULL;storage.after=0xfedcba9876543210ULL;
    assert(funcs.pfnFillDDITable(adapter,type,&storage.table,sizeof(T),number,runtime)==S_OK);
    assert(storage.before==0x123456789abcdef0ULL && storage.after==0xfedcba9876543210ULL);
    return storage.table;
}
static void publication_tests(const D3D12DDI_ADAPTERFUNCS& funcs,D3D12DDI_HADAPTER adapter) {
    using Core=D3D12DDI_DEVICE_FUNCS_CORE_0088;
    using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
    using Queue=D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001;
    using Extended=D3D12DDI_EXTENDED_FEATURES_FUNCS_0021;
    constexpr auto coreType=D3D12DDI_TABLE_TYPE_DEVICE_CORE;
    constexpr auto listType=D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D;
    constexpr auto queueType=D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D;
    constexpr auto extendedType=D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES;
    const D3D12DDI_HRTTABLE runtime[2]={
        {reinterpret_cast<HANDLE>(UINT_PTR(0x123456780001ULL))},
        {reinterpret_cast<HANDLE>(UINT_PTR(0x123456780002ULL))}};
    auto& state=*static_cast<native12::Adapter*>(adapter.pDrvPrivate);
    assert(!state.list_tables[0].handle && !state.list_tables[1].handle);
    rejected_table<Core>(funcs,adapter,coreType,sizeof(Core)-1,0,{});
    rejected_table<Core>(funcs,adapter,coreType,sizeof(Core)+1,0,{});
    rejected_table<Core>(funcs,adapter,coreType,sizeof(Core),1,{});
    auto core=published_table<Core>(funcs,adapter,coreType);
    // Read typed fields from both shell and engine ownership across the table.
    assert(core.pfnCheckFormatSupport && core.pfnCalcPrivateCommandQueueSize &&
        core.pfnCreateCommandQueue && core.pfnDestroyCommandQueue &&
        core.pfnCreateCommandPool && core.pfnCreateCommandList &&
        core.pfnCreateFence && core.pfnDestroyFence &&
        core.pfnCalcPrivateHeapAndResourceSizes && core.pfnCreateHeapAndResource &&
        core.pfnDestroyHeapAndResource && core.pfnMapHeap && core.pfnUnmapHeap &&
        core.pfnMakeResident && core.pfnEvict && core.pfnQueryNodeMap &&
        core.pfnGetPresentPrivateDriverDataSize && core.pfnImplicitShaderCacheControl);
    for(UINT i=0;i<2;++i){
        rejected_table<List>(funcs,adapter,listType,sizeof(List)-1,i,runtime[i]);
        rejected_table<List>(funcs,adapter,listType,sizeof(List)+1,i,runtime[i]);
        rejected_table<List>(funcs,adapter,listType,sizeof(List),i,{});
        assert(!state.list_tables[i].handle);
        auto list=published_table<List>(funcs,adapter,listType,i,runtime[i]);
        assert(list.pfnCloseCommandList && list.pfnResetCommandList &&
            list.pfnCopyBufferRegion && list.pfnResourceCopy && list.pfnResourceBarrier &&
            list.pfnPresent && list.pfnDispatch && list.pfnDrawInstanced &&
            list.pfnSetComputeRootSignature && list.pfnSetGraphicsRootSignature &&
            list.pfnBarrier && list.pfnOmSetAlphaBlendFactor);
        assert(state.list_tables[i].handle==runtime[i].handle);
        auto repeat=published_table<List>(funcs,adapter,listType,i,runtime[i]);
        assert(repeat.pfnCopyBufferRegion==list.pfnCopyBufferRegion && repeat.pfnPresent==list.pfnPresent);
        rejected_table<List>(funcs,adapter,listType,sizeof(List),i,runtime[1-i],E_UNEXPECTED);
        assert(state.list_tables[i].handle==runtime[i].handle);
    }
    rejected_table<List>(funcs,adapter,listType,sizeof(List),2,runtime[0]);
    rejected_table<Queue>(funcs,adapter,queueType,sizeof(Queue)-1,0,{});
    rejected_table<Queue>(funcs,adapter,queueType,sizeof(Queue)+1,0,{});
    rejected_table<Queue>(funcs,adapter,queueType,sizeof(Queue),1,{});
    auto queue=published_table<Queue>(funcs,adapter,queueType);
    assert(queue.pfnExecuteCommandLists && queue.pfnUpdateTileMappings &&
        queue.pfnCopyTileMappings && queue.pfnSignalFence && queue.pfnWaitForFence);
    assert(!queue.pfnUnused && !queue.pfnUnused2);
    rejected_table<Extended>(funcs,adapter,extendedType,sizeof(Extended)-1,0,{});
    rejected_table<Extended>(funcs,adapter,extendedType,sizeof(Extended)+1,0,{});
    rejected_table<Extended>(funcs,adapter,extendedType,sizeof(Extended),1,{});
    auto extended=published_table<Extended>(funcs,adapter,extendedType);
    assert(extended.pfnGetSupportedExtendedFeatures && extended.pfnGetSupportedExtendedFeatureVersions &&
        extended.pfnEnableExtendedFeature && extended.pfnSetExtendedFeatureCallbacks);
    const D3D12DDI_TABLE_TYPE types[]={coreType,listType,queueType,extendedType};
    const SIZE_T sizes[]={sizeof(Core),sizeof(List),sizeof(Queue),sizeof(Extended)};
    for(unsigned i=0;i<4;++i)
        assert(funcs.pfnFillDDITable(adapter,types[i],nullptr,sizes[i],0,runtime[0])==E_INVALIDARG);
    rejected_table<Core>(funcs,{},coreType,sizeof(Core),0,{});
    rejected_table<Core>(funcs,adapter,D3D12DDI_TABLE_TYPE_DXGI,sizeof(Core),0,{},E_NOTIMPL);
    assert(state.devices.load()==0);
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    // A recent-launch note (recent-launch.h) would land below this directory, never in the user's profile. Every
    // CreateDevice below fails, so none may appear (checked at the end).
    char local[MAX_PATH]{};assert(GetFullPathNameA("adapter-test-localappdata",MAX_PATH,local,nullptr));
    CreateDirectoryA(local,nullptr);assert(SetEnvironmentVariableA("LOCALAPPDATA",local));
    const std::string store=std::string(local)+"\\amdgpu-wddm\\recent-launches.txt";
    DeleteFileA(store.c_str());
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
    device.hRTDevice.handle=reinterpret_cast<HANDLE>(UINT_PTR(0x5678));
    device.p12UMCallbacks_0062=&um;device.pKTCallbacks=&km;
    device.Version=0;assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_NOINTERFACE);
    device.Version=sizeArgs.Version;
    // A CPU-only shell is no longer a successful device. Missing allocation
    // callbacks refuse before any engine DLL or GPU is touched on the host.
    assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_INVALIDARG);
    assert(static_cast<native12::Adapter*>(a.hAdapter.pDrvPrivate)->devices.load()==0);
    ::operator delete(storage);

    assert(funcs.pfnFillDDITable(a.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,guard,sizeof(guard),0,{})==E_INVALIDARG);
    assert(guard[1]==0x123456);
    publication_tests(funcs,a.hAdapter);
    assert(funcs.pfnCloseAdapter(a.hAdapter)==S_OK);
    Sleep(200); // longer than a recent-launch worker takes on the host
    assert(GetFileAttributesA(store.c_str())==INVALID_FILE_ATTRIBUTES);
    FreeLibrary(dll);puts("adapter export/negotiation/publication/fail-closed tests passed, no recent-launch note");return 0;
}
