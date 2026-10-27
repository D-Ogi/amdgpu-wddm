// SPDX-License-Identifier: MIT
#include "ddi-ladder.h"
#include <array>
#include <windows.h>
#include <d3d12umddi.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "device-state.h"
static unsigned queryMode;
static HRESULT APIENTRY query(HANDLE adapter,const D3DDDICB_QUERYADAPTERINFO* request) {
    assert(adapter==reinterpret_cast<HANDLE>(UINT_PTR(0x1234)));
    // The kernel driver writes an optional trailer only when all of it fits, so the asked-for size is the
    // whole contract between the two halves: a buffer sized to an older trailer silently reads zeros where
    // a newer one would be. This check refuses that, and the scan-out trailer written below would overrun.
    assert(request->PrivateDriverDataSize==BC250_SCANOUT_CAPS_TOTAL);
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
    // A driver that writes the last trailer the contract defines, where the kernel driver puts it: the
    // buffer above is proven to hold a complete one, and the open is proven to carry it to the contract.
    bc250_scanout_caps scanout{BC250_SCANOUT_CAPS_MAGIC,BC250_SCANOUT_CAPS_VERSION,
        sizeof(bc250_scanout_caps),BC250_SCANOUT_CAPS_DIRECT_FLIP,1920,1200};
    memcpy(bytes+BC250_SCANOUT_CAPS_OFFSET,&scanout,sizeof(scanout));
    return S_OK;
}
static void APIENTRY error(D3D10DDI_HRTDEVICE,HRESULT) {}
// Required non-null callbacks in a refused CreateDevice must never run.
// Keep their exact ABI instead of casting an unrelated function pointer.
template<class Fn> struct UnexpectedCallback;
template<class R,class... A> struct UnexpectedCallback<R(APIENTRY*)(A...)> {
    static R APIENTRY call(A...) { std::abort(); }
};
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
    // Agility 721 has additional table types whose layouts are not in WDK 28000.2526.
    // Unknown tables must fail with the entire destination and both canaries unchanged.
    for(UINT type:{28u,29u,31u,32u,37u,38u,0xffffu})
        for(SIZE_T size:{SIZE_T(0),SIZE_T(1),SIZE_T(8)})
            for(UINT number:{0u,1u})
                rejected_table<UINT64>(funcs,adapter,static_cast<D3D12DDI_TABLE_TYPE>(type),size,number,{},E_NOTIMPL);
    using Core=D3D12DDI_DEVICE_FUNCS_CORE_0088;
    using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
    using Queue=D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001;
    using Extended=D3D12DDI_EXTENDED_FEATURES_FUNCS_0021;
    constexpr auto coreType=D3D12DDI_TABLE_TYPE_DEVICE_CORE;
    constexpr auto listType=D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D;
    constexpr auto queueType=D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D;
    constexpr auto extendedType=D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES;
    const D3D12DDI_HRTTABLE runtime[2]={
        {reinterpret_cast<HANDLE>(UINT_PTR(0x56780001u))},
        {reinterpret_cast<HANDLE>(UINT_PTR(0x56780002u))}};
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
    // M15.14 increment 2: the scan-out trailer the query wrote reached the adapter's contract, where the
    // allocation path reads it. A missing or malformed trailer is allocation-request-test's table; here
    // the one thing proven is that the open carries a complete one through.
    {
        const auto& scanout=static_cast<native12::Adapter*>(a.hAdapter.pDrvPrivate)->contract.scanout;
        assert(scanout.flags==BC250_SCANOUT_CAPS_DIRECT_FLIP && scanout.post_width==1920 &&
               scanout.post_height==1200 && scanout.version==BC250_SCANOUT_CAPS_VERSION);
    }
    constexpr unsigned offered=native12::kDdiDefault-91;
    UINT32 count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==offered);
    std::array<UINT64,offered+1> versions{}; versions.fill(0xabcdef);
    count=offered-1;
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,versions.data())==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    assert(count==offered);for(auto v:versions)assert(v==0xabcdef);
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,versions.data())==S_OK);
    for(unsigned i=0;i<offered;++i)
        assert(versions[i]==((UINT64{D3D12DDI_INTERFACE_VERSION_R8}<<32)|(UINT64{native12::kDdiDefault-i}<<16)));
    assert(versions.back()==0xabcdef);
    // The newest build offered: 0122 in a shell built with the WDK 10.0.28000 header (both flavours), 0094 otherwise.
#if defined(D3D12DDI_BUILD_VERSION_0119)
    static_assert(native12::kDdiDefault==122);
    assert(versions[0]==0xC0050007a0000ull);
    assert(versions[1]==0xC005000790000ull && versions[2]==native12::kDdiSupported0120 &&
           versions[3]==D3D12DDI_SUPPORTED_0119 && versions[4]==D3D12DDI_SUPPORTED_0118 &&
           versions[offered-1]==D3D12DDI_SUPPORTED_0092);
    puts("the version list is 0122, 0121, 0120 ... 0092");
#else
    static_assert(native12::kDdiMax==94);
    assert(versions[0]==D3D12DDI_SUPPORTED_0094 && versions[offered-1]==D3D12DDI_SUPPORTED_0092);
    puts("the version list is 0094, 0093, 0092");
#endif
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && defined(D3D12DDI_BUILD_VERSION_0119)
    // Lab build: AMDGPU_WDDM_LAB_DDI_MAX sets the newest build offered and accepted; a value outside 92..kDdiMax is
    // ignored.
    assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX","118"));
    count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==118-91);
    versions.fill(0xabcdef);
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,versions.data())==S_OK);
    assert(versions[0]==D3D12DDI_SUPPORTED_0118 && versions[count-1]==D3D12DDI_SUPPORTED_0092 &&
           versions[count]==0xabcdef);
    {
        D3D12DDIARG_CALCPRIVATEDEVICESIZE capped{};capped.Interface=D3D12DDI_INTERFACE_VERSION_R8;
        capped.Version=D3D12DDI_BUILD_VERSION_0118<<16;assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&capped));
        capped.Version=D3D12DDI_BUILD_VERSION_0119<<16;assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&capped)==0);
    }
    for(const char* value:{"91","123","1x","","12345678"}){
        assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX",value));
        count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==offered);
    }
    for(const char* value:{"121","122"}) {
        assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX",value));
        std::array<UINT64,native12::kDdiMax-90> diagnostic{};
        diagnostic.fill(0xabcdef);
        count=static_cast<UINT32>(diagnostic.size());
        assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,diagnostic.data())==S_OK);
        const unsigned newest=static_cast<unsigned>(std::strtoul(value,nullptr,10));
        assert(count==newest-91 && diagnostic[count]==0xabcdef);
        assert(diagnostic[0]==((UINT64{D3D12DDI_INTERFACE_VERSION_R8}<<32)|(UINT64{newest}<<16)));
        D3D12DDIARG_CALCPRIVATEDEVICESIZE selected{};
        selected.Interface=D3D12DDI_INTERFACE_VERSION_R8;selected.Version=newest<<16;
        assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&selected));
    }
    assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX",nullptr));
    count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==offered);
    puts("lab build: AMDGPU_WDDM_LAB_DDI_MAX=118 offers 0118 ... 0092 and refuses 0119; other values are ignored");
#elif !defined(AMDGPU_WDDM_LAB_FLAVOUR)
    // Release build: the variable has no effect.
    assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX","93"));
    count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==offered);
    assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX",nullptr));
    puts("release build: AMDGPU_WDDM_LAB_DDI_MAX has no effect");
#endif
    UINT64 guard[4]={0xabcdef,0x123456,D3D12DDI_SUPPORTED_0092,0x888};
    assert(funcs.pfnCreateDevice(a.hAdapter,nullptr)==E_INVALIDARG);
    D3D12DDIARG_CALCPRIVATEDEVICESIZE sizeArgs{};
    assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs)==0);
    // The three offered builds are accepted, the build before them and the one after are not.
    sizeArgs.Interface=D3D12DDI_INTERFACE_VERSION_R8;
    sizeArgs.Version=D3D12DDI_BUILD_VERSION_0094<<16;assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs));
    sizeArgs.Version=D3D12DDI_BUILD_VERSION_0093<<16;assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs));
    sizeArgs.Version=D3D12DDI_BUILD_VERSION_0091<<16;assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs)==0);
    sizeArgs.Version=(native12::kDdiMax+1)<<16;assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs)==0);
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
    auto& state=*static_cast<native12::Adapter*>(a.hAdapter.pDrvPrivate);
    assert(state.devices.load()==0);
    // A CreateDevice refused by its checks records no version, and a command list table asked for before any
    // version is known is refused (E_UNEXPECTED), not given a barrier layout by default.
    assert(state.ddi_version.load()==0);
    {
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 list{};
        assert(funcs.pfnFillDDITable(a.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,&list,sizeof(list),1,
            {reinterpret_cast<HANDLE>(UINT_PTR(0x56780002u))})==E_UNEXPECTED && !list.pfnBarrier);
        assert(!state.list_tables[1].handle);
    }
    // The first version stays; the same one again is accepted, another is refused and changes nothing.
    assert(native12::adopt_ddi_version(state,D3D12DDI_BUILD_VERSION_0094<<16));
    assert(native12::adopt_ddi_version(state,D3D12DDI_BUILD_VERSION_0094<<16));
    assert(!native12::adopt_ddi_version(state,D3D12DDI_BUILD_VERSION_0092<<16) &&
           state.ddi_version.load()==(D3D12DDI_BUILD_VERSION_0094<<16));
    // CreateDevice with every callback it checks and another version: refused before any engine is made.
    um.pfnAllocateCb=&UnexpectedCallback<decltype(um.pfnAllocateCb)>::call;
    um.pfnDeallocateCb=&UnexpectedCallback<decltype(um.pfnDeallocateCb)>::call;
    um.pfnSetCommandListDDITableCb=&UnexpectedCallback<decltype(um.pfnSetCommandListDDITableCb)>::call;
    um.pfnSetCommandListErrorCb=&UnexpectedCallback<decltype(um.pfnSetCommandListErrorCb)>::call;
    device.Version=D3D12DDI_BUILD_VERSION_0092<<16;
    assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_NOINTERFACE && state.devices.load()==0 &&
           state.ddi_version.load()==(D3D12DDI_BUILD_VERSION_0094<<16));
    ::operator delete(storage);

    assert(funcs.pfnFillDDITable(a.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,guard,sizeof(guard),0,{})==E_INVALIDARG);
    assert(guard[2]==D3D12DDI_SUPPORTED_0092 && guard[3]==0x888);
    publication_tests(funcs,a.hAdapter);
    // A second adapter in the same process that negotiated another build: its command list tables have the 0092
    // layout and their own wrappers, and are published although the first adapter's 0094 tables already are
    // (before the per-layout stores, the second fill was refused with E_UNEXPECTED). The first adapter's tables are
    // still published with the same entries afterwards.
    {
        using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
        const D3D12DDI_HRTTABLE first[2]={
            {reinterpret_cast<HANDLE>(UINT_PTR(0x56780001u))},{reinterpret_cast<HANDLE>(UINT_PTR(0x56780002u))}};
        const D3D12DDI_HRTTABLE second[2]={
            {reinterpret_cast<HANDLE>(UINT_PTR(0x56790001u))},{reinterpret_cast<HANDLE>(UINT_PTR(0x56790002u))}};
        D3D12DDI_ADAPTERFUNCS funcs_b{};D3D12DDIARG_OPENADAPTER b{};b.pAdapterFuncs=&funcs_b;
        b.pAdapterCallbacks=&callbacks;b.hRTAdapter.handle=reinterpret_cast<HANDLE>(UINT_PTR(0x1234));
        assert(open(&b)==S_OK && b.hAdapter.pDrvPrivate && b.hAdapter.pDrvPrivate!=a.hAdapter.pDrvPrivate);
        auto& state_b=*static_cast<native12::Adapter*>(b.hAdapter.pDrvPrivate);
        assert(native12::adopt_ddi_version(state_b,D3D12DDI_BUILD_VERSION_0092<<16));
        for(UINT i=0;i<2;++i){
            const List a94=published_table<List>(funcs,a.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,i,first[i]);
            const List b92=published_table<List>(funcs_b,b.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,i,second[i]);
            assert(b92.pfnBarrier && b92.pfnBarrier!=a94.pfnBarrier && b92.pfnDispatch!=a94.pfnDispatch);
            assert(state_b.list_tables[i].handle==second[i].handle);
            const List a94_again=published_table<List>(funcs,a.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,i,first[i]);
            const List b92_again=published_table<List>(funcs_b,b.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,i,second[i]);
            assert(!memcmp(&a94_again,&a94,sizeof(List)) && !memcmp(&b92_again,&b92,sizeof(List)));
        }
        assert(funcs_b.pfnCloseAdapter(b.hAdapter)==S_OK);
    }
    // Reproduce the first Agility 1.619.6 / 0119 table requests in their observed order.
    // The lab r1 trace stopped at type 27: refusing this table aborts CreateDevice.
    // This is the known required prefix, not a claim about requests beyond that failure.
#if defined(D3D12DDI_BUILD_VERSION_0119)
    {
        D3D12DDI_ADAPTERFUNCS f{};D3D12DDIARG_OPENADAPTER a2{};
        a2.pAdapterFuncs=&f;a2.pAdapterCallbacks=&callbacks;a2.hRTAdapter=a.hRTAdapter;
        assert(open(&a2)==S_OK);
        auto& state2=*static_cast<native12::Adapter*>(a2.hAdapter.pDrvPrivate);
        assert(native12::adopt_ddi_version(state2,119u<<16));
        using C=D3D12DDI_DEVICE_FUNCS_CORE_0119;
        using L=D3D12DDI_COMMAND_LIST_FUNCS_3D_0114;
        using Q=D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001;
        using E=D3D12DDI_EXTENDED_FEATURES_FUNCS_0096;
        // Recorded x64 sizes were 1024,600,600,56,32. Typed sizeof also covers x86.
#if defined(_WIN64)
        static_assert(sizeof(C)==1024 && sizeof(L)==600 && sizeof(Q)==56 && sizeof(E)==32);
#endif
        auto core=published_table<C>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE);
        assert(core.pfnCreatePipelineState && core.pfnCreateStateObject);
        for(unsigned number=0;number!=2;++number) {
            const D3D12DDI_HRTTABLE rt{reinterpret_cast<HANDLE>(UINT_PTR(0x71000000u+number))};
            auto list=published_table<L>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,number,rt);
            assert(list.pfnDispatch && list.pfnDrawInstanced);
        }
        auto queue=published_table<Q>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D);
        assert(queue.pfnExecuteCommandLists);
        auto ext=published_table<E>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_0096_EXTENDED_FEATURES);
        assert(ext.pfnGetSupportedExtendedFeatures && ext.pfnGetSupportedExtendedFeatureVersions &&
            ext.pfnEnableExtendedFeature && ext.pfnSetExtendedFeatureCallbacks);
        assert(state2.devices.load()==0);
        assert(f.pfnCloseAdapter(a2.hAdapter)==S_OK);
        puts("Agility 1.619.6 / 0119 required table prefix 0,1#0,1#1,2,27 passed");
    }
#endif
    // Every negotiated build selects its exact core/list layout. Surround each output with canaries,
    // check every typed slot and refill, and reject wrong sizes without overwriting the destination.
    for(unsigned build=92;build<=native12::kDdiMax;++build) {
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && defined(D3D12DDI_BUILD_VERSION_0119)
        char selected[8]{};sprintf_s(selected,"%u",build);
        assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX",selected));
#endif
        D3D12DDI_ADAPTERFUNCS f{};D3D12DDIARG_OPENADAPTER a2{};
        a2.pAdapterFuncs=&f;a2.pAdapterCallbacks=&callbacks;a2.hRTAdapter=a.hRTAdapter;
        assert(open(&a2)==S_OK);
        auto& state2=*static_cast<native12::Adapter*>(a2.hAdapter.pDrvPrivate);
        assert(native12::adopt_ddi_version(state2,build<<16));
        sizeArgs.Version=build<<16;assert(f.pfnCalcPrivateDeviceSize(a2.hAdapter,&sizeArgs));
        assert(native12::with_ddi_layout(build,[&](auto layout)->HRESULT {
            using C=typename decltype(layout)::Core;using L=typename decltype(layout)::List;
            auto core=published_table<C>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE);
            native12::DdiMembers<C>::each([&](auto m,auto){assert(core.*decltype(m)::value);});
            rejected_table<C>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,sizeof(C)-1,0,{});
            for(unsigned number=0;number!=2;++number) {
                D3D12DDI_HRTTABLE rt{reinterpret_cast<HANDLE>(UINT_PTR(0x60000000u+build*16+number))};
                auto list=published_table<L>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,number,rt);
                native12::DdiMembers<L>::each([&](auto m,auto){assert(list.*decltype(m)::value);});
                auto repeat=published_table<L>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,number,rt);
                native12::DdiMembers<L>::each([&](auto m,auto){assert(list.*decltype(m)::value==repeat.*decltype(m)::value);});
                rejected_table<L>(f,a2.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D,sizeof(L)-1,number,rt);
            }
            return S_OK;
        })==S_OK);
        auto queue=published_table<D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001>(
            f,a2.hAdapter,D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D);
        assert(queue.pfnExecuteCommandLists);
#if defined(D3D12DDI_BUILD_VERSION_0119)
        using Extended96=D3D12DDI_EXTENDED_FEATURES_FUNCS_0096;
        constexpr auto ext96Type=D3D12DDI_TABLE_TYPE_0096_EXTENDED_FEATURES;
        if(build>=96) {
            auto ext=published_table<Extended96>(f,a2.hAdapter,ext96Type);
            assert(ext.pfnGetSupportedExtendedFeatures && ext.pfnGetSupportedExtendedFeatureVersions &&
                ext.pfnEnableExtendedFeature && ext.pfnSetExtendedFeatureCallbacks);
            rejected_table<Extended96>(f,a2.hAdapter,ext96Type,sizeof(Extended96)-1,0,{});
            rejected_table<Extended96>(f,a2.hAdapter,ext96Type,sizeof(Extended96)+1,0,{});
            rejected_table<Extended96>(f,a2.hAdapter,ext96Type,sizeof(Extended96),1,{});
        } else {
            rejected_table<Extended96>(f,a2.hAdapter,ext96Type,sizeof(Extended96),0,{},E_NOTIMPL);
        }
#endif
        auto ext=published_table<D3D12DDI_EXTENDED_FEATURES_FUNCS_0021>(
            f,a2.hAdapter,D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES);
        assert(ext.pfnGetSupportedExtendedFeatures);
        assert(f.pfnCloseAdapter(a2.hAdapter)==S_OK);
    }
    assert(SetEnvironmentVariableA("AMDGPU_WDDM_LAB_DDI_MAX",nullptr));
    assert(funcs.pfnCloseAdapter(a.hAdapter)==S_OK);
    Sleep(200); // longer than a recent-launch worker takes on the host
    assert(GetFileAttributesA(store.c_str())==INVALID_FILE_ATTRIBUTES);
    FreeLibrary(dll);puts("adapter export/negotiation/publication/fail-closed tests passed, no recent-launch note");return 0;
}
