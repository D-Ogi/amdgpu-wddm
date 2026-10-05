// SPDX-License-Identifier: MIT
// Reset churn variant of the interactive client (build.ps1 -ResetChurn). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then runs the churn, so commands and receipts keep their form.
//
// Trial 172 lost the device inside a RADV callback under ResetCommandList while several game threads reset and
// recorded lists: the address of a buffer object freed on one thread was mapped on another while the free callback
// was still in flight, and the hosted bridge judged that map malformed (hosted-dispatch.cpp, which now marks such an
// extent as being freed). This variant makes the same traffic without the game, and checks on the way that every draw
// reads the constants the CPU gave it (the 171 corruption hypothesis).
//
// Recording threads (THREADS) each own one command list and one command allocator at a time. Per batch, once the
// fence of the previous batch proved its completion, a thread resets its allocator and its list and records DRAWS
// draws, each after SetGraphicsRoot32BitConstants (32 values) and SetGraphicsRootConstantBufferView into its UPLOAD
// ring. The engine turns those into push constants and a push descriptor that RADV writes into its command buffer's
// upload buffer object on every draw; that object and the command stream's IB grow by doubling while the list
// records, and the objects grown out of are freed at the next allocator reset. RADV keeps the largest objects across
// a reset, so the growth would stop after a few batches: a thread therefore replaces its allocator after RENEW lists
// (0: never). The new allocator's ResetCommandList allocates a command buffer and maps its IB; the old allocator is
// released in the middle of the new recording, after a draw chosen per batch and thread from the run's seed, so that
// over the run one thread's frees land among the other threads' resets and growth.
//
// Constants round trip. The pixel program of each draw copies the 32 root constants and the 16 words of its root
// CBV into its own 192-byte slot of the thread's UAV buffer; the slot index is root constant 0. The list ends by
// copying the UAV buffer into the thread's READBACK buffer. After the batch's fence the main thread compares every
// word with the value the recording thread wrote: root constants are passed by value, the CBV words through the
// UPLOAD ring, which is mapped once and has one region per batch in turn, a region being written again only after
// the fence of the batch that last used it. Every word is unique to seed, thread, batch, draw and word, so a stale,
// misplaced or foreign value never passes. A mismatch is traced with thread, batch, draw, offsets, expected and
// actual value and, when the actual value is one this run wrote in the last four batches, where it came from.
//
// The main thread executes the batch's lists in one ExecuteCommandLists, signals its fence and waits for it, as a
// game's frame loop does, then reads GetDeviceRemovedReason; a removal ends the run and is traced with its batch.
// Batches start until SECONDS after the verb began and never in the last 10 s before the client's deadline, and at
// most BATCHES of them. The run passes when a batch ran, every word compared equal, no thread failed and the device
// was not removed. No window is made.
#include "resetchurn-programs.h"
#include "resetchurn-oracle.h"

namespace resetchurn {
static_assert(cbv_stride==D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,"root CBVs at the placement alignment");

// The main thread releases a batch with go, the recording threads count themselves ready; one lock for all of it.
struct Control {
    SRWLOCK lock=SRWLOCK_INIT;CONDITION_VARIABLE changed=CONDITION_VARIABLE_INIT;
    UINT64 go{};bool stop{};unsigned ready{};HRESULT failure{S_OK};int failed{-1};
    UINT64 completed{};   // batches whose fence proved completion when this batch was released
};
struct Worker {
    unsigned index{};
    ComPtr<ID3D12CommandAllocator> allocator,retiring;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> ring,slots,readback,target;unsigned char* ring_data{};D3D12_CPU_DESCRIPTOR_HANDLE view{};
    unsigned uses{};                 // lists recorded on the current allocator
    UINT64 region_batch[regions]{};  // batch + 1 that last wrote each ring region, 0 for none
    UINT64 lists{},renewals{},resets{};
};
// Everything the recording threads touch. It stays allocated for the rest of the process when a thread does not end
// or GPU completion of its objects is unproven.
struct Shared {
    Session& s;UINT64 seed{};
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;ComPtr<ID3D12DescriptorHeap> views;
    ComPtr<ID3D12Fence> fence;HANDLE event{};UINT64 value{};
    Control control;Worker workers[threads];
    explicit Shared(Session& session):s(session){}
    Shared(const Shared&)=delete;Shared& operator=(const Shared&)=delete;
    ~Shared(){if(event)CloseHandle(event);}
};

inline D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES properties{};properties.Type=type;properties.CreationNodeMask=properties.VisibleNodeMask=1;return properties;
}
inline D3D12_RESOURCE_DESC buffer(UINT64 size,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;
    desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;return desc;
}

// Root signature, pipeline, fence and each thread's list, ring, slots, readback and 1x1 target, on the main thread.
inline HRESULT setup(Shared& sh){
    Session& s=sh.s;ID3D12Device* device=s.device.Get();
    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.ShaderRegister=0;
    parameters[0].Constants.Num32BitValues=root_words;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[1].Descriptor.ShaderRegister=1;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor.ShaderRegister=1;
    for(auto& parameter:parameters)parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=3;signature.pParameters=parameters;
    ComPtr<ID3DBlob> blob,errors;
    HRESULT hr=s.api("Reset churn D3D12SerializeRootSignature",[&]{return serialize(&signature,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    hr=s.api("Reset churn CreateRootSignature",[&]{return device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&sh.root));});if(FAILED(hr))return hr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=sh.root.Get();
    pipeline.VS={g_resetchurn_vs,sizeof(g_resetchurn_vs)};pipeline.PS={g_resetchurn_ps,sizeof(g_resetchurn_ps)};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pipeline.SampleMask=0xffffffffu;
    pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable=TRUE;pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R32_UINT;pipeline.SampleDesc.Count=1;
    hr=s.api("Reset churn CreateGraphicsPipelineState",[&]{return device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&sh.pipeline));});if(FAILED(hr))return hr;
    D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=threads;
    hr=s.api("Reset churn CreateDescriptorHeap RTV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&sh.views));});if(FAILED(hr))return hr;
    hr=s.api("Reset churn CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&sh.fence));});if(FAILED(hr))return hr;
    sh.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!sh.event)return HRESULT_FROM_WIN32(GetLastError());
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(unsigned i=0;i<threads;++i){
        Worker& w=sh.workers[i];w.index=i;const int thread=static_cast<int>(i);
        const auto step=[&](const char* label,auto&& call){s.event("before",label,S_OK,thread);const HRESULT code=call();s.event("after",label,code,thread);return code;};
        D3D12_HEAP_PROPERTIES properties=heap(D3D12_HEAP_TYPE_UPLOAD);D3D12_RESOURCE_DESC desc=buffer(ring_bytes);
        hr=step("Reset churn CreateCommittedResource UPLOAD ring",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&w.ring));});if(FAILED(hr))return hr;
        // Mapped once for the whole run: the recording thread writes a region only while no submission reads it.
        void* data=nullptr;const D3D12_RANGE none{0,0};
        hr=step("Reset churn Map UPLOAD ring",[&]{return w.ring->Map(0,&none,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        w.ring_data=static_cast<unsigned char*>(data);
        // Buffers decay to COMMON after each ExecuteCommandLists; every list moves the slots to UNORDERED_ACCESS itself.
        properties=heap(D3D12_HEAP_TYPE_DEFAULT);desc=buffer(slots_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr=step("Reset churn CreateCommittedResource DEFAULT slots",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&w.slots));});if(FAILED(hr))return hr;
        properties=heap(D3D12_HEAP_TYPE_READBACK);desc=buffer(slots_bytes);
        hr=step("Reset churn CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&w.readback));});if(FAILED(hr))return hr;
        properties=heap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=1;texture.Height=1;texture.DepthOrArraySize=1;
        texture.MipLevels=1;texture.Format=DXGI_FORMAT_R32_UINT;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cleared{};cleared.Format=DXGI_FORMAT_R32_UINT;
        hr=step("Reset churn CreateCommittedResource 1x1 target",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,&cleared,IID_PPV_ARGS(&w.target));});if(FAILED(hr))return hr;
        w.view=sh.views->GetCPUDescriptorHandleForHeapStart();w.view.ptr+=SIZE_T{i}*increment;
        device->CreateRenderTargetView(w.target.Get(),nullptr,w.view);
        // The list starts closed with nothing recorded, so that every batch resets it the same way.
        hr=step("Reset churn CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&w.allocator));});if(FAILED(hr))return hr;
        hr=step("Reset churn CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,w.allocator.Get(),nullptr,IID_PPV_ARGS(&w.list));});if(FAILED(hr))return hr;
        hr=step("Reset churn Close empty CommandList",[&]{return w.list->Close();});if(FAILED(hr))return hr;
    }
    return S_OK;
}

// One list of one batch on a recording thread. 'completed' batches are proven complete: the region check below is
// the ring's reuse rule made explicit, and it holds by construction as long as batches are not pipelined.
inline HRESULT record(Shared& sh,Worker& w,UINT64 batch,UINT64 completed){
    Session& s=sh.s;char label[160]{};HRESULT hr=S_OK;
    const auto fail=[&](const char* what,HRESULT code){
        sprintf_s(label,"Reset churn batch %llu thread %u: %s",batch,w.index,what);s.event("after",label,code,static_cast<int>(w.index));return code;};
    const UINT region=static_cast<UINT>(batch%regions);
    if(w.region_batch[region]>completed)return fail("UPLOAD ring region still in flight",E_UNEXPECTED);
    if(renew && w.uses>=renew){
        w.retiring=std::move(w.allocator);
        hr=s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&w.allocator));
        if(FAILED(hr))return fail("CreateCommandAllocator",hr);
        w.uses=0;++w.renewals;
    }else{
        hr=w.allocator->Reset();if(FAILED(hr))return fail("command allocator Reset",hr);
        ++w.resets;
    }
    hr=w.list->Reset(w.allocator.Get(),sh.pipeline.Get());if(FAILED(hr))return fail("ResetCommandList",hr);
    ++w.uses;
    const UINT release=w.retiring?release_after(sh.seed,w.index,batch):0;
    ID3D12GraphicsCommandList* list=w.list.Get();
    list->SetGraphicsRootSignature(sh.root.Get());
    list->OMSetRenderTargets(1,&w.view,FALSE,nullptr);
    const D3D12_VIEWPORT viewport{0.0f,0.0f,1.0f,1.0f,0.0f,1.0f};const D3D12_RECT scissor{0,0,1,1};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetGraphicsRootUnorderedAccessView(2,w.slots->GetGPUVirtualAddress());
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=w.slots.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1,&barrier);
    unsigned char* ring=w.ring_data+region*region_bytes;const D3D12_GPU_VIRTUAL_ADDRESS ring_va=w.ring->GetGPUVirtualAddress()+region*region_bytes;
    UINT32 constants[root_words];
    for(UINT d=0;d<draws;++d){
        UINT32 words[cbv_words];
        for(UINT i=0;i<cbv_words;++i)words[i]=value(sh.seed,cbv_part,w.index,batch,d,i);
        std::memcpy(ring+d*cbv_stride,words,sizeof(words));   // one sequential write into write-combined memory
        for(UINT i=0;i<root_words;++i)constants[i]=value(sh.seed,root_part,w.index,batch,d,i);
        list->SetGraphicsRoot32BitConstants(0,root_words,constants,0);
        list->SetGraphicsRootConstantBufferView(1,ring_va+d*cbv_stride);
        list->DrawInstanced(3,1,0,0);
        if(d+1==release)w.retiring.Reset();   // the replaced allocator's last list retired before this batch began
    }
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&barrier);
    list->CopyBufferRegion(w.readback.Get(),0,w.slots.Get(),0,slots_bytes);
    hr=list->Close();if(FAILED(hr))return fail("Close CommandList",hr);
    w.region_batch[region]=batch+1;++w.lists;
    return S_OK;
}

inline void worker(Shared& sh,Worker& w) noexcept {
    Control& c=sh.control;UINT64 seen=0;
    for(;;){
        AcquireSRWLockExclusive(&c.lock);
        while(c.go==seen && !c.stop)SleepConditionVariableSRW(&c.changed,&c.lock,INFINITE,0);
        const bool stop=c.stop;seen=c.go;const UINT64 completed=c.completed;
        ReleaseSRWLockExclusive(&c.lock);
        if(stop)break;
        HRESULT hr=E_UNEXPECTED;
        try{hr=record(sh,w,seen-1,completed);}catch(...){hr=E_OUTOFMEMORY;}
        AcquireSRWLockExclusive(&c.lock);
        ++c.ready;if(FAILED(hr) && SUCCEEDED(c.failure)){c.failure=hr;c.failed=static_cast<int>(w.index);}
        ReleaseSRWLockExclusive(&c.lock);WakeAllConditionVariable(&c.changed);
    }
    char label[160]{};
    sprintf_s(label,"Reset churn thread %u end: lists %llu, renewals %llu, resets %llu",w.index,w.lists,w.renewals,w.resets);
    sh.s.event("after",label,S_OK,static_cast<int>(w.index));
}

// Bounded wait for the batch fence. Completion clears the session's pending flag; anything else leaves it set.
inline HRESULT wait(Shared& sh,UINT64 target){
    Session& s=sh.s;
    HRESULT hr=sh.fence->SetEventOnCompletion(target,sh.event);if(FAILED(hr))return hr;
    const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+fence_ms);DWORD status=WAIT_TIMEOUT;
    while(GetTickCount64()<end && !s.abort_requested() && status==WAIT_TIMEOUT)status=WaitForSingleObject(sh.event,20);
    const UINT64 completed=sh.fence->GetCompletedValue();
    if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
    if(completed<target)return s.abort_requested()?HRESULT_FROM_WIN32(ERROR_CANCELLED):HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    s.pending=false;return S_OK;
}

struct Totals {
    UINT64 batches{},lists{},draws{},words{},mismatches{},mismatched_draws{};unsigned removals{},record_failures{},reported{};
    ULONGLONG batch_ms{},batch_max{},record_ms{},submit_ms{},verify_ms{};
};
// Every word of one thread's slots against the values its recording wrote.
inline HRESULT verify(Shared& sh,Worker& w,UINT64 batch,Totals& t){
    Session& s=sh.s;void* data=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(slots_bytes)},none{0,0};
    HRESULT hr=w.readback->Map(0,&range,&data);if(SUCCEEDED(hr) && !data)hr=E_POINTER;
    if(FAILED(hr)){char label[96]{};sprintf_s(label,"Reset churn batch %llu thread %u: Map READBACK",batch,w.index);s.event("after",label,hr);return hr;}
    const UINT32* words=static_cast<const UINT32*>(data);
    for(UINT d=0;d<draws;++d){
        bool bad=false;
        for(UINT i=0;i<slot_words;++i){
            const UINT32 want=slot_value(sh.seed,w.index,batch,d,i),got=words[UINT64{d}*slot_words+i];
            if(got==want)continue;
            bad=true;++t.mismatches;
            if(t.reported>=report_limit)continue;
            ++t.reported;
            const bool root=i<root_words;const UINT word=root?i:i-root_words;
            char upload[24]{},source[96]{},label[400]{};
            if(root)strcpy_s(upload,"none");
            else sprintf_s(upload,"%llu",(batch%regions)*region_bytes+d*cbv_stride+word*4ull);
            const Source from=identify(sh.seed,got,batch);
            if(from.found)sprintf_s(source,"from thread %u batch %llu draw %u %s word %u",from.thread,from.batch,from.draw,
                from.part==root_part?"root":"cbv",from.word);
            else strcpy_s(source,got<draws?"a slot index":"unknown");
            sprintf_s(label,"Reset churn mismatch thread %u batch %llu draw %u %s word %u, readback offset %llu, upload offset %s, "
                "expected %08x, actual %08x, actual is %s",w.index,batch,d,root?"root":"cbv",word,(UINT64{d}*slot_words+i)*4,upload,
                want,got,source);
            s.event("after",label,E_FAIL);
        }
        if(bad)++t.mismatched_draws;
    }
    t.words+=UINT64{draws}*slot_words;
    w.readback->Unmap(0,&none);
    return S_OK;
}
}  // namespace resetchurn

inline HRESULT resetchurn_run(Session& s){
    using namespace resetchurn;
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    const ULONGLONG begin=GetTickCount64();
    const ULONGLONG last_start=s.deadline>begin+margin_ms?s.deadline-margin_ms:begin;
    const ULONGLONG churn_end=(std::min)(begin+budget_ms,last_start);
    Shared* shared=new(std::nothrow) Shared(s);if(!shared)return E_OUTOFMEMORY;
    Shared& sh=*shared;Control& c=sh.control;
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&sh.seed),sizeof(sh.seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){delete shared;return E_FAIL;}
    char label[400]{};
    sprintf_s(label,"Reset churn seed %016llx, threads %u, batches %llu, starts until %llu ms, draws per list %u, renew %u, "
        "root words %u, cbv words %u, ring regions %u",sh.seed,threads,planned,churn_end-begin,draws,renew,root_words,cbv_words,regions);
    s.event("after",label);
    HRESULT result=setup(sh);
    if(FAILED(result)){s.event("after","Reset churn setup failed",result);delete shared;return result;}
    s.event("after","Reset churn setup done",S_OK);

    std::thread handles[threads];unsigned launched=0;
    try{for(;launched<threads;++launched){Worker* w=&sh.workers[launched];handles[launched]=std::thread([&sh,w]{worker(sh,*w);});}}
    catch(...){result=E_OUTOFMEMORY;s.event("after","Reset churn starting threads",result);}
    Totals t;bool budget_hit=false,stuck=false;
    try{
        for(UINT64 b=0;b<planned && SUCCEEDED(result);++b){
            if(GetTickCount64()>=churn_end){budget_hit=true;break;}
            if(s.abort_requested()){result=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
            const ULONGLONG start=GetTickCount64();
            AcquireSRWLockExclusive(&c.lock);c.go=b+1;c.ready=0;c.completed=b;ReleaseSRWLockExclusive(&c.lock);
            WakeAllConditionVariable(&c.changed);
            // Every thread reports back, failed or not, before the batch goes on or the run ends.
            const ULONGLONG ready_end=(std::min)(start+record_ms,s.deadline);
            AcquireSRWLockExclusive(&c.lock);
            for(ULONGLONG now=GetTickCount64();c.ready<threads && now<ready_end;now=GetTickCount64())
                SleepConditionVariableSRW(&c.changed,&c.lock,static_cast<DWORD>(ready_end-now),0);
            const unsigned ready=c.ready;const HRESULT failure=c.failure;const int failed=c.failed;
            ReleaseSRWLockExclusive(&c.lock);
            if(ready<threads){stuck=true;result=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                sprintf_s(label,"Reset churn batch %llu: %u of %u threads ended their recording within %llu ms",b,ready,threads,record_ms);
                s.event("after",label,result);break;}
            if(FAILED(failure)){
                ++t.record_failures;result=failure;
                sprintf_s(label,"Reset churn batch %llu: recording failed on thread %d",b,failed);s.event("after",label,failure);
                const HRESULT reason=s.device->GetDeviceRemovedReason();
                if(FAILED(reason)){++t.removals;result=reason;
                    sprintf_s(label,"Reset churn device removed at batch %llu: reason %08lx",b,static_cast<unsigned long>(reason));s.event("after",label,reason);}
                break;
            }
            const ULONGLONG recorded=GetTickCount64();
            ID3D12CommandList* lists[threads]{};for(unsigned i=0;i<threads;++i)lists[i]=sh.workers[i].list.Get();
            s.pending=true;
            s.queue->ExecuteCommandLists(threads,lists);
            HRESULT hr=s.queue->Signal(sh.fence.Get(),++sh.value);
            if(SUCCEEDED(hr))hr=wait(sh,sh.value);
            const HRESULT reason=s.device->GetDeviceRemovedReason();
            if(FAILED(reason)){++t.removals;result=reason;
                sprintf_s(label,"Reset churn device removed at batch %llu: reason %08lx",b,static_cast<unsigned long>(reason));s.event("after",label,reason);break;}
            if(FAILED(hr)){result=hr;sprintf_s(label,"Reset churn batch %llu: fence %llu not reached",b,sh.value);s.event("after",label,hr);break;}
            const ULONGLONG fenced=GetTickCount64();
            for(unsigned i=0;i<threads && SUCCEEDED(result);++i)result=verify(sh,sh.workers[i],b,t);
            if(FAILED(result))break;
            const ULONGLONG verified=GetTickCount64();
            ++t.batches;t.lists+=threads;t.draws+=UINT64{threads}*draws;
            t.record_ms+=recorded-start;t.submit_ms+=fenced-recorded;t.verify_ms+=verified-fenced;
            t.batch_ms+=verified-start;t.batch_max=(std::max)(t.batch_max,verified-start);
            if(t.batches%progress_every==0){
                sprintf_s(label,"Reset churn progress: batch %llu, lists %llu, mismatches %llu, %llu ms",t.batches,t.lists,t.mismatches,verified-begin);
                s.event("after",label,t.mismatches?E_FAIL:S_OK);
            }
        }
    }catch(...){if(SUCCEEDED(result))result=E_OUTOFMEMORY;s.event("after","Reset churn exception",result);}

    // The threads end at the stop flag; one that does not within 10 s stays behind with everything it can reach.
    AcquireSRWLockExclusive(&c.lock);c.stop=true;ReleaseSRWLockExclusive(&c.lock);WakeAllConditionVariable(&c.changed);
    bool joined=true;const ULONGLONG join_end=GetTickCount64()+join_ms;
    for(unsigned i=0;i<launched;++i){
        const ULONGLONG now=GetTickCount64();
        if(WaitForSingleObject(static_cast<HANDLE>(handles[i].native_handle()),now<join_end?static_cast<DWORD>(join_end-now):0)==WAIT_OBJECT_0)
            handles[i].join();
        else joined=false;
    }
    if(!joined){
        for(unsigned i=0;i<launched;++i)if(handles[i].joinable())handles[i].detach();
        s.pending=true;s.detached=true;
        s.event("after","Reset churn recording thread did not end; its objects stay for process teardown",HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }
    if(stuck && SUCCEEDED(result))result=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    if(SUCCEEDED(result) && !t.batches){result=HRESULT_FROM_WIN32(ERROR_NO_DATA);s.event("after","Reset churn incomplete: no batch ran",result);}
    const bool clean=SUCCEEDED(result) && t.batches && !t.mismatches && !t.removals && !t.record_failures && !s.pending && joined;
    const UINT64 n=t.batches?t.batches:1;
    UINT64 renewals=0,resets=0;for(const Worker& w:sh.workers){renewals+=w.renewals;resets+=w.resets;}
    sprintf_s(label,"Reset churn: batches %llu of %llu, threads %u, draws per list %u, lists %llu, draws %llu, words %llu, mismatches %llu, "
        "mismatched draws %llu, removals %u, record failures %u, renewals %llu, resets %llu, time budget %s, %llu ms, "
        "batch ms mean %llu max %llu, record %llu, submit %llu, verify %llu",
        t.batches,planned,threads,draws,t.lists,t.draws,t.words,t.mismatches,t.mismatched_draws,t.removals,t.record_failures,renewals,resets,
        budget_hit?"reached":"not reached",GetTickCount64()-begin,t.batch_ms/n,t.batch_max,t.record_ms/n,t.submit_ms/n,t.verify_ms/n);
    const HRESULT hr=clean?S_OK:FAILED(result)?result:E_FAIL;
    s.event("after",label,hr);
    // Objects whose GPU completion is unproven, or that a detached thread may still use, stay for process teardown.
    if(joined && !s.pending)delete shared;
    s.copy_success=clean;return hr;
}
