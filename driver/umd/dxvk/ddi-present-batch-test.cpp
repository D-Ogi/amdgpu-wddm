// SPDX-License-Identifier: MIT
// Production table dispatch and owner initialization; controlled engine/kernel callbacks.
// WARP supplies real COM devices/resources without a window. This is not a hosted-GPU rendering test.
#include "ddi-wddm2.h"
#include "ddi-wddm22.h"
#include "ddi-resource.h"
#include "engine-abi.h"
#include "runtime-surface.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace bc250::umd;
namespace {
#define REQUIRE(x) do { if(!(x)){std::printf("FAIL present batch line %d\n",__LINE__);std::fflush(stdout);std::abort();} } while(0)
DeviceOwner *active;
ID3D11Device *comDevice;
ID3D11DeviceContext *comContext;
ID3D11Resource *lastResource;
UINT lastSubresource;
D3DKMT_HANDLE lastAllocation;
int identity,presentContext,renderA,renderB,opaque;
UINT64 presentCpu=0,renderCpuA=0,renderCpuB=0;
std::string order;
unsigned submissions,callbacks;
bool refuseRetire=false;
ID3D11Resource *retiringResource;
struct Engine final : IBc250DxvkDevice4 {
    ULONG refs=1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override {
        if(id!=__uuidof(IBc250DxvkDevice4)){*out=nullptr;return E_NOINTERFACE;}
        *out=static_cast<IBc250DxvkDevice4 *>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {return --refs;}
    HRESULT STDMETHODCALLTYPE GetD3D11Device(REFIID id,void **out) override {return comDevice->QueryInterface(id,out);}
    HRESULT STDMETHODCALLTYPE GetImmediateContext(REFIID id,void **out) override {return comContext->QueryInterface(id,out);}
    HRESULT STDMETHODCALLTYPE CreateShader(const BC250_DXVK_SHADER_DESC *,REFIID,void **) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const BC250_DXVK_INPUT_LAYOUT *,ID3D11InputLayout **) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetVertexFormat(DXGI_FORMAT,VkFormat *,UINT *) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetImageCreateInfo(const D3D11_TEXTURE2D_DESC1 *,VkImageCreateInfo *) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage(const D3D11_TEXTURE2D_DESC1 *,VkImage,ID3D11Texture2D **) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE WaitForResourceIdle(ID3D11Resource *resource) override {
        REQUIRE(order=="Q" && resource==retiringResource);order+='E';return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsResourceBusy(ID3D11Resource *,UINT) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE SubmitForPresent(ID3D11Resource *source,UINT subresource) override {
        REQUIRE(active->runtime().domain.entered() && source==lastResource && subresource==lastSubresource);
        REQUIRE(order.empty());order+='F';++submissions;
        comContext->Flush();
        // Both contexts publish NEW progress during the single global flush.
        active->bridge().progress[0]={1,61,19,&renderCpuA};
        active->bridge().progress[1]={2,63,29,&renderCpuB};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RotateResourceIdentities(ID3D11Resource *const *,UINT) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Blt(const BC250_DXVK_BLT *) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Blt1(const BC250_DXVK_BLT1 *) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage2(const D3D11_TEXTURE2D_DESC1 *,const VkImageCreateInfo *,VkImage,ID3D11Texture2D **) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE CheckFeatureSupportAtLevel(D3D_FEATURE_LEVEL,D3D11_FEATURE,void *,UINT) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE TrimMemory() override {return S_OK;}
    HRESULT STDMETHODCALLTYPE TakeDeferredError() override {return S_OK;}
} engine;
VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
VkResult VKAPI_PTR createInstance(const VkInstanceCreateInfo *,const VkAllocationCallbacks *,VkInstance *out){*out=reinterpret_cast<VkInstance>(1);return VK_SUCCESS;}
void VKAPI_PTR destroyInstance(VkInstance,const VkAllocationCallbacks *){}
VkResult VKAPI_PTR enumerate(VkInstance,uint32_t *count,VkPhysicalDevice *out){*count=1;if(out)*out=reinterpret_cast<VkPhysicalDevice>(2);return VK_SUCCESS;}
void VKAPI_PTR properties(VkPhysicalDevice,VkPhysicalDeviceProperties2 *out){
    auto *id=static_cast<VkPhysicalDeviceIDProperties *>(out->pNext);UINT64 luid=1;
    std::memcpy(id->deviceLUID,&luid,sizeof(luid));id->deviceLUIDValid=VK_TRUE;
}
VkResult VKAPI_PTR createDevice(VkPhysicalDevice,const VkDeviceCreateInfo *,const VkAllocationCallbacks *,VkDevice *out){*out=reinterpret_cast<VkDevice>(3);return VK_SUCCESS;}
void VKAPI_PTR destroyDevice(VkDevice,const VkAllocationCallbacks *){}
void VKAPI_PTR getQueue(VkDevice,uint32_t,uint32_t,VkQueue *out){*out=reinterpret_cast<VkQueue>(4);}
PFN_vkVoidFunction VKAPI_PTR get(VkInstance,const char *name){
#define ENTRY(n,f) if(!std::strcmp(name,n))return reinterpret_cast<PFN_vkVoidFunction>(f)
    ENTRY("vkCreateInstance",createInstance);ENTRY("vkDestroyInstance",destroyInstance);
    ENTRY("vkEnumeratePhysicalDevices",enumerate);ENTRY("vkGetPhysicalDeviceProperties2",properties);
    ENTRY("vkCreateDevice",createDevice);ENTRY("vkDestroyDevice",destroyDevice);ENTRY("vkGetDeviceQueue",getQueue);
#undef ENTRY
    return nullptr;
}
HRESULT APIENTRY adapter(const BC250_DXVK_VULKAN_INSTANCE *,BC250_DXVK_ADAPTER_INFO *out){out->MaxFeatureLevel=D3D_FEATURE_LEVEL_11_0;return S_OK;}
HRESULT APIENTRY requirements(const BC250_DXVK_VULKAN_INSTANCE *,BC250_DXVK_DEVICE_REQUIREMENTS *out){out->Features=&features;return S_OK;}
void APIENTRY freeRequirements(BC250_DXVK_DEVICE_REQUIREMENTS *){}
HRESULT APIENTRY createEngine(const BC250_DXVK_DEVICE_CREATE_INFO *,IBc250DxvkDevice **out){engine.refs=1;*out=&engine;return S_OK;}
HRESULT APIENTRY createContext(HANDLE,D3DDDICB_CREATECONTEXTVIRTUAL *out){out->hContext=&presentContext;return S_OK;}
HRESULT APIENTRY destroyContext(HANDLE,const D3DDDICB_DESTROYCONTEXT *){return S_OK;}
HRESULT APIENTRY destroySync(HANDLE,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *){return S_OK;}
void APIENTRY setError(D3D10DDI_HRTCORELAYER,HRESULT){REQUIRE(false);}
HRESULT APIENTRY createSync(HANDLE,D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *out){
    REQUIRE(order=="F");order+='C';out->hSyncObject=62;
    out->Info.MonitoredFence.FenceValueCPUVirtualAddress=&presentCpu;return S_OK;
}
HRESULT APIENTRY wait(HANDLE,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *in){
    REQUIRE(order=="FC" || order=="F");REQUIRE(in->hContext==&presentContext && in->ObjectCount==2);
    REQUIRE(in->ObjectHandleArray[0]==61 && in->MonitoredFenceValueArray[0]==19);
    REQUIRE(in->ObjectHandleArray[1]==63 && in->MonitoredFenceValueArray[1]==29);order+='W';return S_OK;
}
HRESULT APIENTRY callback(HANDLE,DXGIDDICB_PRESENT *in){
    REQUIRE(order=="FCW" || order=="FW");REQUIRE(in->hSrcAllocation==lastAllocation && !in->hDstAllocation);
    REQUIRE(in->pDXGIContext==&opaque && in->hContext==&presentContext);++callbacks;order+='P';return S_OK;
}
HRESULT APIENTRY signal(HANDLE,const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *in){
    REQUIRE(order=="FCWP" || order=="FWP");REQUIRE(in->ObjectCount==1 && in->ObjectHandleArray[0]==62);
    presentCpu=in->MonitoredFenceValueArray[0];order+='S';return S_OK;
}
HRESULT APIENTRY retire(HANDLE,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *in){
    REQUIRE(order.empty() && in->ObjectCount==1 && in->ObjectHandleArray[0]==62);
    REQUIRE(in->FenceValueArray[0]==active->bridge().present_value);order+='Q';
    if(refuseRetire)return E_FAIL;
    presentCpu=in->FenceValueArray[0];REQUIRE(SetEvent(in->hAsyncEvent));return S_OK;
}
// BD-308: the OfferResources entry with its wait for the device's work (ddi-dxgi-resources.cpp offer,
// wait_device_work_retired in runtime-bridge.cpp). The wait callback answers as the case asks.
enum class RetireMode { complete, never, refuse, waitFails, lostAfter };
RetireMode retireMode=RetireMode::complete;
UINT64 workCpu=0;
UINT64 workValue=0;
unsigned workWaits,offerCalls;
HANDLE failingEvent=nullptr;
bool failNextWait=false;
HRESULT APIENTRY retireWork(HANDLE,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *in){
    REQUIRE(in->ObjectCount==1 && in->ObjectHandleArray[0]==71 && in->FenceValueArray[0]==workValue);
    REQUIRE(in->hAsyncEvent && !in->Flags.Value);
    ++workWaits;order+='W';
    switch(retireMode){
    case RetireMode::complete: workCpu=workValue;REQUIRE(SetEvent(in->hAsyncEvent));return S_OK;
    case RetireMode::never: return S_OK;                 // the work runs past the bound: no signal
    case RetireMode::refuse: return E_FAIL;
    case RetireMode::waitFails: failingEvent=in->hAsyncEvent;failNextWait=true;return S_OK;
    case RetireMode::lostAfter: workCpu=UINT64_MAX;REQUIRE(SetEvent(in->hAsyncEvent));return S_OK;
    }
    return E_FAIL;
}
HRESULT APIENTRY offerAllocations(HANDLE,const D3DDDICB_OFFERALLOCATIONS *in){
    REQUIRE(in->NumAllocations==1 && in->HandleList && in->HandleList[0]==130 && in->Priority==D3DDDI_OFFER_PRIORITY_NORMAL);
    ++offerCalls;order+='O';return S_OK;
}
// WAIT_FAILED cannot come from a valid event, so the case replaces this executable's import of WaitForSingleObject
// for one call on the event the wait callback was given. The shell code is linked into this executable and calls the
// import through its address table; d3d11.dll and WARP keep their own tables.
using WaitFn=DWORD (WINAPI *)(HANDLE,DWORD);
WaitFn realWait=nullptr;
DWORD WINAPI waitHook(HANDLE h,DWORD ms){
    if(failNextWait && h==failingEvent){failNextWait=false;SetLastError(ERROR_INVALID_HANDLE);return WAIT_FAILED;}
    return realWait(h,ms);
}
WaitFn *importSlot(const char *name){
    auto *base=reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
    const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS *>(base+reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!dir.VirtualAddress)return nullptr;
    for(auto *d=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base+dir.VirtualAddress);d->Name;++d){
        if(!d->OriginalFirstThunk)continue;
        auto *names=reinterpret_cast<IMAGE_THUNK_DATA *>(base+d->OriginalFirstThunk);
        auto *slots=reinterpret_cast<IMAGE_THUNK_DATA *>(base+d->FirstThunk);
        for(;names->u1.AddressOfData;++names,++slots){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            const auto *byName=reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base+names->u1.AddressOfData);
            if(!std::strcmp(reinterpret_cast<const char *>(byName->Name),name))return reinterpret_cast<WaitFn *>(&slots->u1.Function);
        }
    }
    return nullptr;
}
void setImport(WaitFn *slot,WaitFn value){
    DWORD old=0;REQUIRE(VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old));
    *slot=value;DWORD ignored=0;REQUIRE(VirtualProtect(slot,sizeof(*slot),old,&ignored));
}
// The BD-308 note goes to OutputDebugStringA and to the game log; the game log needs the control app's folder, so the
// test reads the debug string. OutputDebugStringA raises DBG_PRINTEXCEPTION_C (or the wide form, which also carries
// the narrow text), and a vectored handler sees it before the handler inside OutputDebugStringA.
constexpr DWORD kPrintException=0x40010006, kPrintExceptionWide=0x4001000A;
// A system can raise both forms for one call, so each form has its own text and the test reads one of them.
std::string narrowText,wideText;
LONG CALLBACK captureDebugText(EXCEPTION_POINTERS *e){
    const EXCEPTION_RECORD *r=e->ExceptionRecord;
    const ULONG_PTR *p=r->ExceptionInformation;
    if(r->ExceptionCode==kPrintException && r->NumberParameters>=2 && p[1]&&p[0])
        narrowText.append(reinterpret_cast<const char *>(p[1]),strnlen(reinterpret_cast<const char *>(p[1]),size_t(p[0])));
    else if(r->ExceptionCode==kPrintExceptionWide && r->NumberParameters>=4 && p[3]&&p[2])
        wideText.append(reinterpret_cast<const char *>(p[3]),strnlen(reinterpret_cast<const char *>(p[3]),size_t(p[2])));
    return EXCEPTION_CONTINUE_SEARCH;
}
const std::string &debugText(){return narrowText.empty() ? wideText : narrowText;}
unsigned countText(const std::string &text,const char *needle){
    unsigned n=0;for(size_t at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+1))++n;return n;
}
ULONG releaseTexture(ID3D11Texture2D *texture){REQUIRE(order=="QE");order+='R';return texture->Release();}
void VKAPI_PTR releaseImage(VkDevice,VkImage,const VkAllocationCallbacks *){REQUIRE(order=="QER");order+='D';}
void VKAPI_PTR releaseMemory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks *){REQUIRE(order=="QERD");order+='M';}
}
void test_present_batches(){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    const HMODULE module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);REQUIRE(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));REQUIRE(create);
    REQUIRE(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&comDevice,nullptr,&comContext)));
    DeviceOwner owner;active=&owner;DdiDeviceHandle storage{&owner};
    D3DDDI_DEVICECALLBACKS kt{};kt.pfnCreateContextVirtualCb=createContext;kt.pfnDestroyContextCb=destroyContext;
    kt.pfnDestroySynchronizationObjectCb=destroySync;kt.pfnCreateSynchronizationObject2Cb=createSync;
    kt.pfnWaitForSynchronizationObjectFromGpuCb=wait;kt.pfnSignalSynchronizationObjectFromGpu2Cb=signal;
    kt.pfnWaitForSynchronizationObjectFromCpuCb=retire;
    D3D10DDI_CORELAYER_DEVICECALLBACKS um{};um.pfnSetErrorCb=setError;
    DXGI_DDI_BASE_CALLBACKS dxgi{};dxgi.pfnPresentCb=callback;
    D3D10DDIARG_CREATEDEVICE args{};args.hRTDevice.handle=&identity;args.pKTCallbacks=&kt;args.pUMCallbacks=&um;
    args.DXGIBaseDDI.pDXGIBaseCallbacks=&dxgi;
    BC250_DXVK_ENGINE_FUNCS funcs{sizeof(funcs),kRequiredEngineAbi,requirements,freeRequirements,adapter,createEngine};
    BC250_DXVK_SHELL_SERVICES services{};services.Size=sizeof(services);
    {RuntimeDomain::Scope scope(owner.runtime().domain);REQUIRE(owner.initialize(args,1,get,funcs,D3D_FEATURE_LEVEL_11_0,services)==S_OK);}
    owner.bridge().contexts[0]=&renderA;owner.bridge().contexts[1]=&renderB;
    DdiResource resources[3]{};DXGI_DDI_ARG_PRESENTSURFACE surfaces[3]{};
    for(UINT i=0;i<3;++i){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=4;desc.Height=4;desc.MipLevels=3;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        ID3D11Texture2D *texture=nullptr;REQUIRE(comDevice->CreateTexture2D(&desc,nullptr,&texture)==S_OK);
        resources[i].object=texture;resources[i].present_allocation=120+i;resources[i].present_subresource=i;
        surfaces[i].hSurface=reinterpret_cast<DXGI_DDI_HRESOURCE>(&resources[i]);surfaces[i].SubResourceIndex=i;
    }
    const auto oldTable=make_dxgi1_4_device_table();const auto newTable=make_dxgi1_6_1_device_table();
    for(UINT version=0;version<2;++version)for(UINT count=1;count<=3;++count){
        // Record work to every source before one batch submission. No map/readback can flush it for us.
        const UINT pixels[16]={0xff000001,0xff000002,0xff000003};
        for(UINT i=0;i<count;++i)comContext->UpdateSubresource(resources[i].object,i,nullptr,pixels,16,0);
        lastResource=resources[count-1].object;lastSubresource=count-1;lastAllocation=120+count-1;
        owner.bridge().progress[0]={1,61,3,&renderCpuA};owner.bridge().progress[1]={2,63,7,&renderCpuB};
        submissions=callbacks=0;order.clear();
        DXGI_DDI_ARG_PRESENT1 a{};a.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&storage);
        a.phSurfacesToPresent=surfaces;a.SurfacesToPresent=count;a.pDXGIContext=&opaque;a.Flags.Blt=1;
        a.FlipInterval=DXGI_DDI_FLIP_INTERVAL_TWO;
        HRESULT hr;
        if(!version)hr=oldTable.pfnPresent1(&a);
        else{
            DXGI1_6_1_DDI_ARG_PRESENT b{};b.hDevice=a.hDevice;b.phSurfacesToPresent=surfaces;b.SurfacesToPresent=count;
            b.pDXGIContext=a.pDXGIContext;b.Flags=a.Flags;b.FlipInterval=a.FlipInterval;
            hr=newTable.pfnPresent1(&b);
        }
        REQUIRE(hr==S_OK && submissions==1 && callbacks==1 && (order=="FCWPS" || order=="FWPS"));
        REQUIRE(!owner.runtime().domain.entered());
    }
    // The first source must remain alive until the single batch Present retires,
    // even though only the last source was translated for the callback.
    RuntimeTexture first{};first.texture=static_cast<ID3D11Texture2D *>(resources[0].object);
    // A non-dispatchable handle is a uint64_t on x86, so a C-style cast is the portable spelling.
    first.image.image=(VkImage)(uintptr_t(123));first.image.memory=(VkDeviceMemory)(uintptr_t(124));
    RuntimeImageDispatch vk{};vk.destroy=releaseImage;vk.memory.free=releaseMemory;
    auto imported=texture_import_dispatch(engine);imported.release=releaseTexture;
    retiringResource=resources[0].object;presentCpu=0;refuseRetire=true;order.clear();
    {
        RuntimeDomain::Scope scope(owner.runtime().domain);
        REQUIRE(close_runtime_texture(owner.bridge(),VK_NULL_HANDLE,vk,imported,first)==E_FAIL);
        REQUIRE(order=="Q" && first.texture==retiringResource && first.image.image && first.image.memory && !first.retained);
        refuseRetire=false;order.clear();
        REQUIRE(close_runtime_texture(owner.bridge(),VK_NULL_HANDLE,vk,imported,first)==S_OK);
        REQUIRE(order=="QERDM" && !first.texture && !first.image.image && !first.image.memory);
    }
    resources[1].object->Release();resources[2].object->Release();
    {RuntimeDomain::Scope scope(owner.runtime().domain);REQUIRE(owner.close()==S_OK);}
    REQUIRE(engine.refs==0 && !owner.has_live_objects());
    comContext->Release();comDevice->Release();FreeLibrary(module);
    std::puts("PASS Present1 batches: both tables, 1/2/3 real resources, LAST allocation/subresource, one global submit, two NEW context fences, one callback, first-source retirement and failure retention");
}
// BD-308: OfferResources waits up to 500 ms for the device's work, then offers. A wait that does not end with S_OK
// still offers and writes one note line; a lost device found before or by the wait removes the device and offers
// nothing.
void test_offer_retirement(){
    PVOID handler=AddVectoredExceptionHandler(1,captureDebugText);REQUIRE(handler);
    OutputDebugStringA("BC250 offer test: control line\n");
    REQUIRE(countText(debugText(),"BC250 offer test: control line")==1); // positive control of the capture
    WaitFn *slot=importSlot("WaitForSingleObject");REQUIRE(slot && *slot);
    realWait=*slot;setImport(slot,waitHook);
    const HMODULE module=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);REQUIRE(module);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));REQUIRE(create);
    REQUIRE(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&comDevice,nullptr,&comContext)));
    DeviceOwner owner;active=&owner;DdiDeviceHandle storage{&owner};
    D3DDDI_DEVICECALLBACKS kt{};kt.pfnCreateContextVirtualCb=createContext;kt.pfnDestroyContextCb=destroyContext;
    kt.pfnDestroySynchronizationObjectCb=destroySync;kt.pfnWaitForSynchronizationObjectFromCpuCb=retireWork;
    kt.pfnOfferAllocationsCb=offerAllocations;
    D3D10DDI_CORELAYER_DEVICECALLBACKS um{};um.pfnSetErrorCb=setError;
    DXGI_DDI_BASE_CALLBACKS dxgi{};dxgi.pfnPresentCb=callback;
    D3D10DDIARG_CREATEDEVICE args{};args.hRTDevice.handle=&identity;args.pKTCallbacks=&kt;args.pUMCallbacks=&um;
    args.DXGIBaseDDI.pDXGIBaseCallbacks=&dxgi;
    BC250_DXVK_ENGINE_FUNCS funcs{sizeof(funcs),kRequiredEngineAbi,requirements,freeRequirements,adapter,createEngine};
    BC250_DXVK_SHELL_SERVICES services{};services.Size=sizeof(services);
    {RuntimeDomain::Scope scope(owner.runtime().domain);REQUIRE(owner.initialize(args,1,get,funcs,D3D_FEATURE_LEVEL_11_0,services)==S_OK);}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=4;desc.Height=4;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    ID3D11Texture2D *texture=nullptr;REQUIRE(comDevice->CreateTexture2D(&desc,nullptr,&texture)==S_OK);
    RuntimeSurface surface;surface.owner=&owner.runtime();surface.phase=SurfacePhase::ready;
    surface.texture.texture=texture;surface.allocation.allocation=130;
    DdiResource resource{};resource.object=texture;resource.runtime_surface=&surface;resource.present_allocation=130;
    DXGI_DDI_HRESOURCE handles[1]={reinterpret_cast<DXGI_DDI_HRESOURCE>(&resource)};
    const auto table=make_dxgi1_4_device_table();REQUIRE(table.pfnOfferResources);
    HostBridge &bridge=owner.bridge();
    struct Case { const char *name; bool work; RetireMode mode; UINT64 before; HRESULT result; const char *order; const char *note; };
    const Case cases[]={
        {"no work in flight",false,RetireMode::complete,0,S_OK,"O",nullptr},
        {"work retires",true,RetireMode::complete,4,S_OK,"WO",nullptr},
        {"work runs past the bound",true,RetireMode::never,4,S_OK,"WO","returned 00000001 after"},
        {"wait callback refused",true,RetireMode::refuse,4,S_OK,"WO","returned 80004005 after"},
        {"event wait failed",true,RetireMode::waitFails,4,S_OK,"WO","returned 80070006 after"},
        {"lost before the wait",true,RetireMode::complete,UINT64_MAX,D3DDDIERR_DEVICEREMOVED,"",nullptr},
        {"lost during the wait",true,RetireMode::lostAfter,4,D3DDDIERR_DEVICEREMOVED,"W",nullptr},
    };
    unsigned notes=0;
    for(const Case &c:cases){
        bridge.device_lost=false;bridge.lost_reported=false;bridge.submission_failed=false;
        for(auto &p:bridge.progress)p={};
        workValue=5;workCpu=c.before;retireMode=c.mode;workWaits=offerCalls=0;order.clear();failNextWait=false;
        if(c.work)bridge.progress[3]={1,71,workValue,&workCpu};
        const std::string before=debugText();
        DXGI_DDI_ARG_OFFERRESOURCES offer{reinterpret_cast<DXGI_DDI_HDEVICE>(&storage),handles,1,D3DDDI_OFFER_PRIORITY_NORMAL};
        const ULONGLONG start=GetTickCount64();
        const HRESULT hr=table.pfnOfferResources(&offer);
        const ULONGLONG waited=GetTickCount64()-start;
        const std::string added=debugText().substr(before.size());
        if(hr!=c.result || order!=c.order){
            std::printf("offer case '%s': hr=%08X order='%s' waited=%llu ms\n",c.name,unsigned(hr),order.c_str(),waited);
            REQUIRE(false);
        }
        REQUIRE(offerCalls==(c.result==S_OK ? 1u : 0u) && workWaits==(std::strchr(c.order,'W') ? 1u : 0u));
        REQUIRE(!failNextWait && !owner.runtime().domain.entered());
        // The removal latches the loss, so later entries of the device fail at once.
        REQUIRE(bridge.device_lost==(c.result==D3DDDIERR_DEVICEREMOVED));
        const unsigned lines=countText(added,"BC250 BD-308: OfferResources");
        if(c.note){
            REQUIRE(lines==1 && countText(added,c.note)==1 && countText(added,"1 allocation(s) offered with work in flight")==1);
            ++notes;
        } else REQUIRE(lines==0);
        if(c.mode==RetireMode::never)REQUIRE(waited>=400 && waited<5000); // the 500 ms bound, with timer slack
        else REQUIRE(waited<5000);
    }
    REQUIRE(notes==3);
    setImport(slot,realWait);
    bridge.device_lost=false;bridge.lost_reported=false;for(auto &p:bridge.progress)p={};
    texture->Release();
    {RuntimeDomain::Scope scope(owner.runtime().domain);REQUIRE(owner.close()==S_OK);}
    REQUIRE(engine.refs==0 && !owner.has_live_objects());
    comContext->Release();comDevice->Release();FreeLibrary(module);
    REQUIRE(RemoveVectoredExceptionHandler(handler));
    std::puts("PASS OfferResources wait (BD-308): no work, retired work, past the bound, refused wait, failed event wait (offered, one note each), lost before and during the wait (removed, no offer)");
}
