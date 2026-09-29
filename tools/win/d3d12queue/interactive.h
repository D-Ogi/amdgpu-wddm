// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <bcrypt.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace interactive {
using Microsoft::WRL::ComPtr;
enum class AdapterMode {Invalid,Bc250,Warp};
inline AdapterMode adapter_mode(const char* text) noexcept {
    if(!text)return AdapterMode::Invalid;
    if(!std::strcmp(text,"--interactive"))return AdapterMode::Bc250;
    if(!std::strcmp(text,"--interactive-warp"))return AdapterMode::Warp;
    return AdapterMode::Invalid;
}
enum class Verb {Invalid,CreateDevice,CreateQueue,Copy,Status,Exit,Abort};
struct Command {unsigned sequence{};Verb verb{Verb::Invalid};};
inline const char* name(Verb verb){
    switch(verb){case Verb::CreateDevice:return "create-device";case Verb::CreateQueue:return "create-queue";
    case Verb::Copy:return "copy";case Verb::Status:return "status";case Verb::Exit:return "exit";case Verb::Abort:return "abort";default:return "invalid";}
}
inline Command parse(const std::string& text,unsigned expected){
    if(text.empty() || text.size()>80 || !expected || expected>64)return {};
    size_t i=0;unsigned sequence=0;
    while(i<text.size() && text[i]>='0' && text[i]<='9'){
        sequence=sequence*10+static_cast<unsigned>(text[i++]-'0');if(sequence>64)return {};
    }
    if(!i || sequence!=expected || i>=text.size() || text[i++]!=' ')return {};
    size_t start=i;while(i<text.size() && text[i]!='\r' && text[i]!='\n')++i;
    const auto word=text.substr(start,i-start);
    if(i<text.size() && text[i]=='\r')++i;
    if(i<text.size() && text[i]=='\n')++i;
    if(i!=text.size())return {};
    for(auto verb:{Verb::CreateDevice,Verb::CreateQueue,Verb::Copy,Verb::Status,Verb::Exit,Verb::Abort})
        if(word==name(verb))return {sequence,verb};
    return {};
}
inline unsigned seconds(const char* text){
    if(!text || !*text)return 0;unsigned value=0;
    for(;*text;++text){if(*text<'0' || *text>'9')return 0;value=value*10+static_cast<unsigned>(*text-'0');if(value>150)return 0;}
    return value;
}
inline std::string hr_text(HRESULT hr){char text[16]{};sprintf_s(text,"%08lx",static_cast<unsigned long>(hr));return text;}
inline std::filesystem::path numbered(const std::filesystem::path& root,const char* prefix,unsigned sequence,const char* suffix){
    char file[80]{};sprintf_s(file,"%s-%06u.%s",prefix,sequence,suffix);return root/file;
}
inline bool publish(const std::filesystem::path& path,const std::string& value){
    auto temporary=path;temporary+=L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;bool ok=WriteFile(file,value.data(),static_cast<DWORD>(value.size()),&written,nullptr) && written==value.size() && FlushFileBuffers(file);
    CloseHandle(file);return ok && MoveFileW(temporary.c_str(),path.c_str());
}
struct Session {
    std::filesystem::path root;
    AdapterMode mode{AdapterMode::Bc250};
    ULONGLONG start{},deadline{};
    HANDLE trace{INVALID_HANDLE_VALUE};
    HMODULE runtime{};
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> upload,readback,middle;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    ComPtr<IUnknown> extra[8];   // objects of a build variant, released with the rest
    unsigned sequence{};bool copy_success{},pending{},io_failed{};
    ~Session() noexcept {
        // An exception while formatting/publishing a receipt must not release
        // resources with unproven GPU retirement during stack unwinding.
        if(pending){list.Detach();allocator.Detach();upload.Detach();readback.Detach();middle.Detach();
            for(auto& object:extra)object.Detach();
            fence.Detach();queue.Detach();device.Detach();adapter.Detach();factory.Detach();}
    }

    void event(const char* phase,const char* api,HRESULT hr=S_OK){
        char line[512]{};int length=sprintf_s(line,"{\"sequence\":%u,\"elapsed_ms\":%llu,\"phase\":\"%s\",\"api\":\"%s\",\"hr\":\"%08lx\"}\n",sequence,GetTickCount64()-start,phase,api,static_cast<unsigned long>(hr));
        DWORD written=0;if(length<0 || !WriteFile(trace,line,static_cast<DWORD>(length),&written,nullptr) || written!=static_cast<DWORD>(length) || !FlushFileBuffers(trace))io_failed=true;
        std::fputs(line,stdout);std::fflush(stdout);
    }
    template<class F> HRESULT api(const char* label,F&& function){event("before",label);HRESULT hr=function();event("after",label,hr);return hr;}
    bool abort_requested() const{return GetFileAttributesW((root/L"abort.request").c_str())!=INVALID_FILE_ATTRIBUTES;}
    HRESULT create_device(){
        if(mode!=AdapterMode::Bc250 && mode!=AdapterMode::Warp)return E_INVALIDARG;
        if(device)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if(!runtime){wchar_t path[MAX_PATH]{};if(!GetSystemDirectoryW(path,MAX_PATH) || wcscat_s(path,L"\\d3d12.dll"))return E_FAIL;
            runtime=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!runtime)return HRESULT_FROM_WIN32(GetLastError());}
        auto proc=GetProcAddress(runtime,"D3D12CreateDevice");decltype(&D3D12CreateDevice) create=nullptr;
        static_assert(sizeof(create)==sizeof(proc));std::memcpy(&create,&proc,sizeof(create));if(!create)return E_NOINTERFACE;
        factory.Reset();adapter.Reset();HRESULT hr=api("CreateDXGIFactory1",[&]{return CreateDXGIFactory1(IID_PPV_ARGS(&factory));});if(FAILED(hr))return hr;
        if(mode==AdapterMode::Warp){
            hr=api("EnumWarpAdapter",[&]{return factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));});
            if(FAILED(hr))return hr;
        } else for(UINT index=0;;++index){ComPtr<IDXGIAdapter1> candidate;hr=api("EnumAdapters1",[&]{return factory->EnumAdapters1(index,&candidate);});
            if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return hr;DXGI_ADAPTER_DESC1 desc{};
            hr=api("GetDesc1",[&]{return candidate->GetDesc1(&desc);});if(FAILED(hr))return hr;
            if(desc.VendorId==0x1002 && desc.DeviceId==0x13fe){adapter=candidate;break;}}
        if(!adapter)return DXGI_ERROR_NOT_FOUND;
        std::printf("runtime=system32/d3d12.dll adapter=%s\n",mode==AdapterMode::Warp?"WARP":"BC-250");std::fflush(stdout);
#ifdef INTERACTIVE_FEATURE_LEVEL_12_1
        hr=api("D3D12CreateDevice FL12_1",[&]{return create(adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&device));});
#else
        hr=api("D3D12CreateDevice FL11_0",[&]{return create(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));});
#endif
        if(SUCCEEDED(hr))observe_features();
        return hr;
    }
    // Observation only: what the runtime reports. No result of these queries changes a receipt.
    void observe_features(){
        char label[64]{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        HRESULT hr=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options));
        if(FAILED(hr))event("after","Options query failed",hr);
        else{
            sprintf_s(label,"Reported TiledResourcesTier %u",static_cast<unsigned>(options.TiledResourcesTier));event("after",label,hr);
            sprintf_s(label,"Reported ResourceBindingTier %u",static_cast<unsigned>(options.ResourceBindingTier));event("after",label,hr);
            sprintf_s(label,"Reported ConservativeRasterizationTier %u",static_cast<unsigned>(options.ConservativeRasterizationTier));event("after",label,hr);
        }
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};levels.NumFeatureLevels=4;levels.pFeatureLevelsRequested=requested;
        hr=device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&levels,sizeof(levels));
        if(FAILED(hr))event("after","Feature level query failed",hr);
        else{sprintf_s(label,"Reported MaxSupportedFeatureLevel %04x",static_cast<unsigned>(levels.MaxSupportedFeatureLevel));event("after",label,hr);}
    }
    HRESULT create_queue(){
        if(!device)return E_UNEXPECTED;if(queue)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        return api("CreateCommandQueue DIRECT",[&]{return device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));});
    }
    HRESULT copy(){
        if(!queue || pending || copy_success)return E_UNEXPECTED;
        constexpr UINT64 size=4096;
        D3D12_RESOURCE_DESC resource{};resource.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;resource.Width=size;
        resource.Height=1;resource.DepthOrArraySize=1;resource.MipLevels=1;resource.SampleDesc.Count=1;resource.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        HRESULT hr=api("CreateCommittedResource UPLOAD",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload));});if(FAILED(hr))return hr;
        heap.Type=D3D12_HEAP_TYPE_READBACK;
        hr=api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));});if(FAILED(hr))return hr;
        // The pattern differs per run and the destination starts as its complement, so bytes left
        // from an earlier run, or a destination that aliases the source, cannot pass the comparison.
        unsigned char seed[4]{};
        if(BCryptGenRandom(nullptr,seed,sizeof(seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return E_FAIL;
        const auto expected=[&seed](size_t i){return static_cast<unsigned char>(((i*37+11)^(i>>3))+seed[i&3]+seed[(i>>7)&3]);};
        {char label[48]{};sprintf_s(label,"Pattern seed %02x%02x%02x%02x",seed[0],seed[1],seed[2],seed[3]);event("after",label);}
#ifdef INTERACTIVE_DEFAULT_HEAP
        heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        hr=api("CreateCommittedResource DEFAULT",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&middle));});if(FAILED(hr))return hr;
#endif
        void* data=nullptr;D3D12_RANGE empty{0,0};D3D12_RANGE whole{0,size};
        hr=api("Map READBACK prefill",[&]{return readback->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        for(size_t i=0;i<size;++i)static_cast<unsigned char*>(data)[i]=static_cast<unsigned char>(~expected(i));
        event("before","Unmap READBACK prefill");readback->Unmap(0,&whole);event("after","Unmap READBACK prefill");
        data=nullptr;hr=api("Map UPLOAD",[&]{return upload->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        for(size_t i=0;i<size;++i)static_cast<unsigned char*>(data)[i]=expected(i);
        D3D12_RANGE written{0,size};event("before","Unmap UPLOAD");upload->Unmap(0,&written);event("after","Unmap UPLOAD");
        data=nullptr;hr=api("Map READBACK before submit",[&]{return readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        bool untouched=true;for(size_t i=0;i<size;++i)if(static_cast<const unsigned char*>(data)[i]!=static_cast<unsigned char>(~expected(i))){untouched=false;break;}
        readback->Unmap(0,&empty);event("after","Destination holds complement before submit",untouched?S_OK:E_FAIL);
        if(!untouched)return E_FAIL;
        hr=api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));});if(FAILED(hr))return hr;
        hr=api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list));});if(FAILED(hr))return hr;
        hr=api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));});if(FAILED(hr))return hr;
#ifdef INTERACTIVE_DEFAULT_HEAP
        // The bytes can reach READBACK only through GPU memory that has no CPU mapping.
        event("before","CopyBufferRegion UPLOAD to DEFAULT");list->CopyBufferRegion(middle.Get(),0,upload.Get(),0,size);event("after","CopyBufferRegion UPLOAD to DEFAULT");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=middle.Get();
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        event("before","ResourceBarrier COPY_DEST to COPY_SOURCE");list->ResourceBarrier(1,&barrier);event("after","ResourceBarrier COPY_DEST to COPY_SOURCE");
        event("before","CopyBufferRegion DEFAULT to READBACK");list->CopyBufferRegion(readback.Get(),0,middle.Get(),0,size);event("after","CopyBufferRegion DEFAULT to READBACK");
#else
        event("before","CopyBufferRegion 4096");list->CopyBufferRegion(readback.Get(),0,upload.Get(),0,size);event("after","CopyBufferRegion 4096");
#endif
        hr=api("Close CommandList",[&]{return list->Close();});if(FAILED(hr))return hr;
        ID3D12CommandList* commands[]={list.Get()};pending=true;
        event("before","ExecuteCommandLists");queue->ExecuteCommandLists(1,commands);event("after","ExecuteCommandLists");
        hr=api("Queue Signal 1",[&]{return queue->Signal(fence.Get(),1);});if(FAILED(hr))return hr;
        HANDLE completed_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completed_event)return HRESULT_FROM_WIN32(GetLastError());
        hr=api("SetEventOnCompletion 1",[&]{return fence->SetEventOnCompletion(1,completed_event);});
        if(SUCCEEDED(hr)){
            ULONGLONG end=(std::min)(deadline,GetTickCount64()+5000);DWORD wait=WAIT_TIMEOUT;
            event("before","WaitForFence bounded");
            while(GetTickCount64()<end && !abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(completed_event,20);
            event("after","WaitForFence bounded",wait==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            event("before","GetCompletedValue");UINT64 completed=fence->GetCompletedValue();event("after","GetCompletedValue",completed>=1 && completed!=UINT64_MAX?S_OK:E_FAIL);
            if(completed==UINT64_MAX)hr=DXGI_ERROR_DEVICE_REMOVED;
            else if(completed<1)hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            else pending=false;
        }
        CloseHandle(completed_event);if(FAILED(hr))return hr;
        data=nullptr;D3D12_RANGE range{0,size};hr=api("Map READBACK",[&]{return readback->Map(0,&range,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        bool equal=true;for(size_t i=0;i<size;++i)if(static_cast<const unsigned char*>(data)[i]!=expected(i)){equal=false;break;}
        event("before","Unmap READBACK");readback->Unmap(0,&empty);event("after","Unmap READBACK");
        hr=equal?S_OK:E_FAIL;event("after","Compare 4096 exact bytes",hr);copy_success=equal;return hr;
    }
    std::string result(const char* verb,HRESULT hr)const{
        return "{\"schema\":1,\"sequence\":"+std::to_string(sequence)+",\"command\":\""+verb+"\",\"success\":"+(SUCCEEDED(hr)?"true":"false")+",\"hr\":\""+hr_text(hr)+"\",\"elapsed_ms\":"+std::to_string(GetTickCount64()-start)+",\"state\":{\"device\":"+(device?"true":"false")+",\"queue\":"+(queue?"true":"false")+"},\"copy_success\":"+(copy_success?"true":"false")+",\"gpu_pending\":"+(pending?"true":"false")+"}\n";
    }
};
#if defined(INTERACTIVE_DRAW) + defined(INTERACTIVE_SCENE) + defined(INTERACTIVE_PRESENT) + defined(INTERACTIVE_SPARSE) > 1
#error one variant of the copy verb per build
#endif
#ifdef INTERACTIVE_DRAW
#include "interactive-draw.h"
#endif
#ifdef INTERACTIVE_SCENE
#include "interactive-scene.h"
#endif
#ifdef INTERACTIVE_PRESENT
#include "interactive-present.h"
#endif
#ifdef INTERACTIVE_SPARSE
#include "interactive-sparse.h"
#endif
inline int run(const char* directory,unsigned duration,AdapterMode mode=AdapterMode::Bc250){
    if(!directory || !*directory || !duration || duration>150 ||
       (mode!=AdapterMode::Bc250 && mode!=AdapterMode::Warp))return 2;
    Session session;session.mode=mode;session.root=std::filesystem::absolute(directory);session.start=GetTickCount64();session.deadline=session.start+duration*1000ull;
    if(!std::filesystem::is_directory(session.root))return 2;
    if(std::filesystem::exists(session.root/L"session.json"))return 2;
    for(unsigned i=1;i<=64;++i)if(std::filesystem::exists(numbered(session.root,"result",i,"json")))return 2;
    session.trace=CreateFileW((session.root/L"trace.jsonl").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(session.trace==INVALID_HANDLE_VALUE)return 2;
#ifdef INTERACTIVE_RADV_EXPERIMENTAL
    // Diagnostic build only: the hosted ICD reads this from the process environment.
#define INTERACTIVE_TEXT2(token) #token
#define INTERACTIVE_TEXT(token) INTERACTIVE_TEXT2(token)
    session.event("after","Process RADV_EXPERIMENTAL=" INTERACTIVE_TEXT(INTERACTIVE_RADV_EXPERIMENTAL),
        SetEnvironmentVariableA("RADV_EXPERIMENTAL",INTERACTIVE_TEXT(INTERACTIVE_RADV_EXPERIMENTAL))?S_OK:HRESULT_FROM_WIN32(GetLastError()));
#endif
    {   // What the hosted ICD will read, in every build: absent, or the bounded text.
        char value[40]{};char label[80]{};SetLastError(ERROR_SUCCESS);
        const DWORD length=GetEnvironmentVariableA("RADV_EXPERIMENTAL",value,sizeof(value));
        if(!length && GetLastError()==ERROR_ENVVAR_NOT_FOUND)strcpy_s(label,"Effective RADV_EXPERIMENTAL absent");
        else if(length>=sizeof(value))strcpy_s(label,"Effective RADV_EXPERIMENTAL longer than 39");
        else{for(char& c:value)if(c && !((c>='a' && c<='z') || (c>='0' && c<='9') || c==',' || c=='_'))c='?';
            sprintf_s(label,"Effective RADV_EXPERIMENTAL=%s",value);}
        session.event("after",label);
    }
    HRESULT terminal=S_OK;const char* reason="exit";bool finished=false;
    for(unsigned seq=1;seq<=64 && !finished;++seq){
        session.sequence=seq;const auto command_path=numbered(session.root,"command",seq,"txt");
        while(GetFileAttributesW(command_path.c_str())==INVALID_FILE_ATTRIBUTES && GetTickCount64()<session.deadline && !session.abort_requested())Sleep(25);
        if(session.abort_requested()){terminal=HRESULT_FROM_WIN32(ERROR_CANCELLED);reason="abort";break;}
        if(GetTickCount64()>=session.deadline){terminal=HRESULT_FROM_WIN32(WAIT_TIMEOUT);reason="deadline";break;}
        HANDLE file=CreateFileW(command_path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        char bytes[81]{};DWORD count=0;bool read=file!=INVALID_HANDLE_VALUE && ReadFile(file,bytes,sizeof(bytes),&count,nullptr);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        Command command=read?parse(std::string(bytes,count),seq):Command{};HRESULT hr=E_INVALIDARG;
        session.event("before",name(command.verb));
        switch(command.verb){
        case Verb::CreateDevice:hr=session.create_device();break;
        case Verb::CreateQueue:hr=session.create_queue();break;
#if defined(INTERACTIVE_DRAW)
        case Verb::Copy:hr=draw(session);break;
#elif defined(INTERACTIVE_SCENE)
        case Verb::Copy:hr=scene(session);break;
#elif defined(INTERACTIVE_PRESENT)
        case Verb::Copy:hr=present(session);break;
#elif defined(INTERACTIVE_SPARSE)
        case Verb::Copy:hr=sparse(session);break;
#else
        case Verb::Copy:hr=session.copy();break;
#endif
        case Verb::Status:hr=session.device?session.api("GetDeviceRemovedReason",[&]{return session.device->GetDeviceRemovedReason();}):S_OK;break;
        case Verb::Exit:hr=S_OK;finished=true;break;
        case Verb::Abort:hr=HRESULT_FROM_WIN32(ERROR_CANCELLED);terminal=hr;reason="abort";finished=true;break;
        default:break;}
        session.event("after",name(command.verb),hr);
        if(!publish(numbered(session.root,"result",seq,"json"),session.result(name(command.verb),hr)) || session.io_failed){terminal=E_FAIL;reason="io-failure";break;}
    }
    if(!publish(session.root/L"session.json",session.result(reason,terminal)))terminal=E_FAIL;
    CloseHandle(session.trace);session.trace=INVALID_HANDLE_VALUE;
    // Pending GPU work retains resources until process teardown. Do not call
    // Release on resources whose GPU retirement was not proven.
    if(session.pending)ExitProcess(3);
    if(session.runtime){session.list.Reset();session.allocator.Reset();session.upload.Reset();session.readback.Reset();session.middle.Reset();for(auto& object:session.extra)object.Reset();session.fence.Reset();session.queue.Reset();session.device.Reset();session.adapter.Reset();session.factory.Reset();FreeLibrary(session.runtime);session.runtime=nullptr;}
    return FAILED(terminal)?3:0;
}
}
