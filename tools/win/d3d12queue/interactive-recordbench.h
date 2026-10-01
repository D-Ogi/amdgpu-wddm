// SPDX-License-Identifier: MIT
// Record bench variant of the interactive client (build.ps1 -RecordBench). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then runs the bench, so commands and receipts keep their form.
//
// It measures what the UMD's deferred-replay experiment changes: the CPU time a recording thread spends in
// command-list calls, per frame. The trial runner sets AMDGPU_WDDM_D3D12_EXPERIMENT from BC250_TRIAL_EXPERIMENT; the
// client only reports the value it sees, so one binary runs both arms of the A/B.
//
// A frame is LISTS command lists of DRAWS draws each, recorded, executed in EXECUTES ExecuteCommandLists calls, a
// fence signalled and waited for, and every word the draws wrote compared. Each draw changes state as a game does:
// the pipeline every 4 draws (two pipelines that differ in their pixel program), the descriptor table every 8 (4
// tables in a shader-visible heap), 8 root constants and a root CBV into the list's UPLOAD ring every draw, and every
// group of 16 draws its render target (two 1x1 targets in turn), viewport, scissor and vertex buffer; even draws are
// indexed. The pixel program writes everything it read into the draw's own slot of the list's UAV buffer, the list
// copies the slots and both targets into its READBACK buffer, and after the fence the main thread compares every word
// with what the recording wrote (recordbench-oracle.h): a call replayed late, out of order or not at all shows as a
// wrong word, traced with where the word came from.
//
// Three phases share the time budget, each counting its frames after WARMUP:
//   burst: the main thread records the lists back to back;
//   interleaved: the main thread spins WORK_US microseconds of stand-in application work after each group of 16
//     draws, as a game's main thread interleaves its own work with recording; engine work moved to another thread
//     can overlap it;
//   threaded: LISTS threads record one list each, with the same stand-in work, while the main thread waits.
// Per frame: the recording thread's cycles over its recording (QueryThreadCycleTime) less those of its stand-in work,
// which is the API's share (the "api cpu"), the submission's cycles, and the wall time (QPC) of recording,
// submission, GPU wait and verification. Per phase: the main thread's and the process's CPU time (GetThreadTimes,
// GetProcessTimes) over the counted frames, so CPU time moved to other threads stays visible. The trace gets two
// summary lines per phase and one for the run; recordbench-frames.csv in the session directory gets one row per
// counted frame, written after each phase, never inside a measured span. No window is made.
#include "recordbench-programs.h"
#include "recordbench-oracle.h"

namespace recordbench {
static_assert(cbv_stride==D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,"root CBVs at the placement alignment");
static_assert(target_stride==D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,"target copies at the placement alignment");

inline UINT64 qpc(){LARGE_INTEGER now{};QueryPerformanceCounter(&now);return static_cast<UINT64>(now.QuadPart);}
inline UINT64 cycles(){ULONG64 value=0;QueryThreadCycleTime(GetCurrentThread(),&value);return value;}
inline UINT64 filetime(const FILETIME& time){return (UINT64{time.dwHighDateTime}<<32)|time.dwLowDateTime;}
struct CpuTimes {UINT64 thread{},process{};};   // kernel plus user, 100 ns units
inline CpuTimes cpu_times(){
    FILETIME created{},exited{},kernel{},user{};CpuTimes times;
    if(GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))times.thread=filetime(kernel)+filetime(user);
    if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))times.process=filetime(kernel)+filetime(user);
    return times;
}

// The threaded phase: the main thread releases a frame with go, the recording threads count themselves ready.
struct Control {
    SRWLOCK lock=SRWLOCK_INIT;CONDITION_VARIABLE changed=CONDITION_VARIABLE_INIT;
    UINT64 go{};bool stop{};unsigned ready{};HRESULT failure{S_OK};int failed{-1};
};
struct ListSet {
    unsigned index{};
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> ring,slots,readback,targets[2];unsigned char* ring_data{};
    D3D12_CPU_DESCRIPTOR_HANDLE views[2]{};
    UINT64 api_cycles{},work_cycles{};          // the threaded phase: its recording thread's last frame, under the lock
};
// One counted frame.
struct Sample {
    unsigned phase{};UINT64 frame{};
    UINT64 record_cycles{},work_cycles{},submit_cycles{},api_max{},api_sum{};
    double record_us{},submit_us{},wait_us{},verify_us{},frame_us{};
};
// Everything the recording threads touch. It stays allocated for the rest of the process when a thread does not end
// or GPU completion of its objects is unproven.
struct Shared {
    Session& s;UINT64 seed{};double qpf{},cycles_per_us{};
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipelines[2];
    ComPtr<ID3D12DescriptorHeap> rtvs,heap;ComPtr<ID3D12Resource> tables,vertices;
    D3D12_GPU_DESCRIPTOR_HANDLE table_base{};UINT table_increment{};
    D3D12_VERTEX_BUFFER_VIEW vbv[2]{};D3D12_INDEX_BUFFER_VIEW ibv{};
    ComPtr<ID3D12Fence> fence;HANDLE event{};UINT64 value{};
    Control control;ListSet sets[lists];
    std::vector<Sample> samples;HANDLE csv{INVALID_HANDLE_VALUE};
    explicit Shared(Session& session):s(session){}
    Shared(const Shared&)=delete;Shared& operator=(const Shared&)=delete;
    ~Shared(){if(event)CloseHandle(event);if(csv!=INVALID_HANDLE_VALUE)CloseHandle(csv);}
};
struct Totals {
    UINT64 frames{},words{},mismatches{},mismatched_draws{};unsigned removals{},failures{},reported{},phases{};
};

// Cycles of QueryThreadCycleTime per microsecond, from five 20 ms spins: the largest ratio is the one least shortened
// by preemption. Windows does not promise a constant cycle rate (boost and power states move it), so the run measures
// it at its start and again at its end, and the microseconds in the summary lines are cycles at the first rate: compare
// arms by them on one machine, and take the phase's GetThreadTimes figure as the cross-check in time units.
inline double calibrate(double qpf){
    double best=0;
    for(int i=0;i<5;++i){
        const UINT64 c0=cycles(),q0=qpc(),end=q0+static_cast<UINT64>(qpf*0.02);
        UINT64 now=q0;while(now<end){YieldProcessor();now=qpc();}
        const double ratio=static_cast<double>(cycles()-c0)/((now-q0)*1e6/qpf);
        if(ratio>best)best=ratio;
    }
    return best;
}
// Stand-in application work: the thread spins for 'us' microseconds of wall time; its cycles go to *spent.
inline void stand_in(const Shared& sh,UINT us,UINT64* spent){
    const UINT64 c0=cycles(),end=qpc()+static_cast<UINT64>(sh.qpf*us/1e6);
    while(qpc()<end)YieldProcessor();
    *spent+=cycles()-c0;
}

inline D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES properties{};properties.Type=type;properties.CreationNodeMask=properties.VisibleNodeMask=1;return properties;
}
inline D3D12_RESOURCE_DESC buffer(UINT64 size,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;
    desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;return desc;
}
inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=resource;
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;return barrier;
}

// Root signature, both pipelines, the tables, the vertex and index data, the fence and each list's ring, slots,
// readback, targets, allocator and list, on the main thread.
inline HRESULT setup(Shared& sh){
    Session& s=sh.s;ID3D12Device* device=s.device.Get();
    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_CBV;range.NumDescriptors=1;range.BaseShaderRegister=2;
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.ShaderRegister=0;
    parameters[0].Constants.Num32BitValues=root_words;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[1].Descriptor.ShaderRegister=1;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[2].DescriptorTable.NumDescriptorRanges=1;
    parameters[2].DescriptorTable.pDescriptorRanges=&range;
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor.ShaderRegister=1;
    for(auto& parameter:parameters)parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=4;signature.pParameters=parameters;
    signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob,errors;
    HRESULT hr=s.api("Record bench D3D12SerializeRootSignature",[&]{return serialize(&signature,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    hr=s.api("Record bench CreateRootSignature",[&]{return device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&sh.root));});if(FAILED(hr))return hr;
    const D3D12_INPUT_ELEMENT_DESC element{"TAG",0,DXGI_FORMAT_R32_UINT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=sh.root.Get();pipeline.InputLayout={&element,1};
    pipeline.VS={g_recordbench_vs,sizeof(g_recordbench_vs)};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;pipeline.SampleMask=0xffffffffu;
    pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable=TRUE;pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R32_UINT;pipeline.SampleDesc.Count=1;
    pipeline.PS={g_recordbench_ps_a,sizeof(g_recordbench_ps_a)};
    hr=s.api("Record bench CreateGraphicsPipelineState A",[&]{return device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&sh.pipelines[0]));});if(FAILED(hr))return hr;
    pipeline.PS={g_recordbench_ps_b,sizeof(g_recordbench_ps_b)};
    hr=s.api("Record bench CreateGraphicsPipelineState B",[&]{return device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&sh.pipelines[1]));});if(FAILED(hr))return hr;

    // The tables: one CBV each in a shader-visible heap, over 256-byte blocks of an UPLOAD buffer written once.
    D3D12_HEAP_PROPERTIES properties=heap(D3D12_HEAP_TYPE_UPLOAD);D3D12_RESOURCE_DESC desc=buffer(tables*cbv_stride);
    hr=s.api("Record bench CreateCommittedResource UPLOAD tables",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&sh.tables));});if(FAILED(hr))return hr;
    void* data=nullptr;const D3D12_RANGE none{0,0};
    hr=s.api("Record bench Map tables",[&]{return sh.tables->Map(0,&none,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT t=0;t<tables;++t){UINT32 words[64]{};for(UINT w=0;w<table_words;++w)words[w]=table_value(sh.seed,t,w);
        std::memcpy(static_cast<unsigned char*>(data)+t*cbv_stride,words,sizeof(words));}
    sh.tables->Unmap(0,nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap_desc.NumDescriptors=tables;
    heap_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=s.api("Record bench CreateDescriptorHeap CBV shader-visible",[&]{return device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&sh.heap));});if(FAILED(hr))return hr;
    sh.table_increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sh.table_base=sh.heap->GetGPUDescriptorHandleForHeapStart();
    for(UINT t=0;t<tables;++t){
        D3D12_CONSTANT_BUFFER_VIEW_DESC view{sh.tables->GetGPUVirtualAddress()+t*cbv_stride,static_cast<UINT>(cbv_stride)};
        D3D12_CPU_DESCRIPTOR_HANDLE cpu=sh.heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T{t}*sh.table_increment;
        device->CreateConstantBufferView(&view,cpu);
    }
    // Vertex buffers at 0 and 256 (three vertices of one tag each), the index buffer at 512.
    desc=buffer(1024);
    hr=s.api("Record bench CreateCommittedResource UPLOAD vertices",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&sh.vertices));});if(FAILED(hr))return hr;
    data=nullptr;hr=s.api("Record bench Map vertices",[&]{return sh.vertices->Map(0,&none,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT b=0;b<2;++b){const UINT32 vertex[3]{tags[b],tags[b],tags[b]};std::memcpy(static_cast<unsigned char*>(data)+b*256,vertex,sizeof(vertex));}
    const UINT16 indices[4]{0,1,2,0};std::memcpy(static_cast<unsigned char*>(data)+512,indices,sizeof(indices));
    sh.vertices->Unmap(0,nullptr);
    const D3D12_GPU_VIRTUAL_ADDRESS vertices=sh.vertices->GetGPUVirtualAddress();
    for(UINT b=0;b<2;++b)sh.vbv[b]={vertices+b*256,12,4};
    sh.ibv={vertices+512,sizeof(indices),DXGI_FORMAT_R16_UINT};

    D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=2*lists;
    hr=s.api("Record bench CreateDescriptorHeap RTV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&sh.rtvs));});if(FAILED(hr))return hr;
    hr=s.api("Record bench CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&sh.fence));});if(FAILED(hr))return hr;
    sh.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!sh.event)return HRESULT_FROM_WIN32(GetLastError());
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(unsigned i=0;i<lists;++i){
        ListSet& l=sh.sets[i];l.index=i;const int thread=static_cast<int>(i);
        const auto step=[&](const char* label,auto&& call){s.event("before",label,S_OK,thread);const HRESULT code=call();s.event("after",label,code,thread);return code;};
        properties=heap(D3D12_HEAP_TYPE_UPLOAD);desc=buffer(ring_bytes);
        hr=step("Record bench CreateCommittedResource UPLOAD ring",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&l.ring));});if(FAILED(hr))return hr;
        // Mapped once for the whole run: frames do not overlap, so the recording writes it only while no submission reads it.
        data=nullptr;hr=step("Record bench Map UPLOAD ring",[&]{return l.ring->Map(0,&none,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        l.ring_data=static_cast<unsigned char*>(data);
        // Buffers decay to COMMON after each ExecuteCommandLists; every list moves the slots to UNORDERED_ACCESS itself.
        properties=heap(D3D12_HEAP_TYPE_DEFAULT);desc=buffer(slots_bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr=step("Record bench CreateCommittedResource DEFAULT slots",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&l.slots));});if(FAILED(hr))return hr;
        properties=heap(D3D12_HEAP_TYPE_READBACK);desc=buffer(readback_bytes);
        hr=step("Record bench CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&l.readback));});if(FAILED(hr))return hr;
        properties=heap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=1;texture.Height=1;texture.DepthOrArraySize=1;
        texture.MipLevels=1;texture.Format=DXGI_FORMAT_R32_UINT;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cleared{};cleared.Format=DXGI_FORMAT_R32_UINT;
        for(UINT t=0;t<2;++t){
            hr=step("Record bench CreateCommittedResource 1x1 target",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,&cleared,IID_PPV_ARGS(&l.targets[t]));});if(FAILED(hr))return hr;
            l.views[t]=sh.rtvs->GetCPUDescriptorHandleForHeapStart();l.views[t].ptr+=SIZE_T{2*i+t}*increment;
            device->CreateRenderTargetView(l.targets[t].Get(),nullptr,l.views[t]);
        }
        // The list starts closed with nothing recorded, so that every frame resets it the same way.
        hr=step("Record bench CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&l.allocator));});if(FAILED(hr))return hr;
        hr=step("Record bench CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,l.allocator.Get(),nullptr,IID_PPV_ARGS(&l.list));});if(FAILED(hr))return hr;
        hr=step("Record bench Close empty CommandList",[&]{return l.list->Close();});if(FAILED(hr))return hr;
    }
    return S_OK;
}

// One list of one frame, on the recording thread; 'work' microseconds of stand-in work after each group of draws.
// No trace line unless something fails: the trace writes through to the disk.
inline HRESULT record_list(Shared& sh,ListSet& l,unsigned phase,UINT64 frame,UINT work,UINT64* work_cycles){
    Session& s=sh.s;char label[160]{};HRESULT hr=S_OK;
    const auto fail=[&](const char* what,HRESULT code){
        sprintf_s(label,"Record bench %s frame %llu list %u: %s",phase_name(phase),frame,l.index,what);
        s.event("after",label,code,static_cast<int>(l.index));return code;};
    hr=l.allocator->Reset();if(FAILED(hr))return fail("command allocator Reset",hr);
    hr=l.list->Reset(l.allocator.Get(),sh.pipelines[0].Get());if(FAILED(hr))return fail("ResetCommandList",hr);
    ID3D12GraphicsCommandList* list=l.list.Get();
    ID3D12DescriptorHeap* heaps[]={sh.heap.Get()};
    list->SetDescriptorHeaps(1,heaps);
    list->SetGraphicsRootSignature(sh.root.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetIndexBuffer(&sh.ibv);
    list->SetGraphicsRootUnorderedAccessView(3,l.slots->GetGPUVirtualAddress());
    const D3D12_RESOURCE_BARRIER begin=transition(l.slots.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1,&begin);
    const D3D12_GPU_VIRTUAL_ADDRESS ring_va=l.ring->GetGPUVirtualAddress();
    UINT32 constants[root_words],words[cbv_words];
    for(UINT d=0;d<draws;++d){
        if(d%group==0){
            if(d && work)stand_in(sh,work,work_cycles);
            // Both viewports and scissors cover the target's one pixel; the second pair only differs.
            const UINT b=binding_of(d);const FLOAT extent=b?2.0f:1.0f;
            const D3D12_VIEWPORT viewport{0.0f,0.0f,extent,extent,0.0f,1.0f};const D3D12_RECT scissor{0,0,b?4:1,b?4:1};
            list->OMSetRenderTargets(1,&l.views[b],FALSE,nullptr);
            list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
            list->IASetVertexBuffers(0,1,&sh.vbv[b]);
        }
        if(d%pipeline_every==0)list->SetPipelineState(sh.pipelines[pipeline_of(d)].Get());
        if(d%table_every==0)list->SetGraphicsRootDescriptorTable(2,{sh.table_base.ptr+UINT64{table_of(d)}*sh.table_increment});
        for(UINT i=0;i<cbv_words;++i)words[i]=value(sh.seed,cbv_part,phase,l.index,frame,d,i);
        std::memcpy(l.ring_data+d*cbv_stride,words,sizeof(words));   // one sequential write into write-combined memory
        for(UINT i=0;i<root_words;++i)constants[i]=value(sh.seed,root_part,phase,l.index,frame,d,i);
        list->SetGraphicsRoot32BitConstants(0,root_words,constants,0);
        list->SetGraphicsRootConstantBufferView(1,ring_va+d*cbv_stride);
        if(d%2==0)list->DrawIndexedInstanced(3,1,0,0,0);
        else list->DrawInstanced(3,1,0,0);
    }
    if(work)stand_in(sh,work,work_cycles);
    const D3D12_RESOURCE_BARRIER end[3]{
        transition(l.slots.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(l.targets[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(l.targets[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(3,end);
    list->CopyBufferRegion(l.readback.Get(),0,l.slots.Get(),0,slots_bytes);
    for(UINT t=0;t<2;++t){
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=l.readback.Get();
        destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint.Offset=target_offset+t*target_stride;
        destination.PlacedFootprint.Footprint={DXGI_FORMAT_R32_UINT,1,1,1,D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=l.targets[t].Get();
        source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=0;
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    }
    const D3D12_RESOURCE_BARRIER back[2]{
        transition(l.targets[0].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET),
        transition(l.targets[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list->ResourceBarrier(2,back);
    hr=list->Close();if(FAILED(hr))return fail("Close CommandList",hr);
    return S_OK;
}

// A recording thread of the threaded phase: one list per frame, its cycles less its stand-in work kept for the main
// thread.
inline void recorder(Shared& sh,ListSet& l) noexcept {
    Control& c=sh.control;UINT64 seen=0;
    for(;;){
        AcquireSRWLockExclusive(&c.lock);
        while(c.go==seen && !c.stop)SleepConditionVariableSRW(&c.changed,&c.lock,INFINITE,0);
        const bool stop=c.stop;seen=c.go;
        ReleaseSRWLockExclusive(&c.lock);
        if(stop)break;
        UINT64 work=0;HRESULT hr=E_UNEXPECTED;const UINT64 start=cycles();
        try{hr=record_list(sh,l,threaded,seen-1,work_us,&work);}catch(...){hr=E_OUTOFMEMORY;}
        const UINT64 spent=cycles()-start;
        AcquireSRWLockExclusive(&c.lock);
        l.api_cycles=spent>work?spent-work:0;l.work_cycles=work;
        ++c.ready;if(FAILED(hr) && SUCCEEDED(c.failure)){c.failure=hr;c.failed=static_cast<int>(l.index);}
        ReleaseSRWLockExclusive(&c.lock);WakeAllConditionVariable(&c.changed);
    }
}

// Bounded wait for the frame's fence. Completion clears the session's pending flag; anything else leaves it set.
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

// Every word of one list's slots and both targets against the values its recording wrote.
inline HRESULT verify(Shared& sh,ListSet& l,unsigned phase,UINT64 frame,Totals& t){
    Session& s=sh.s;void* data=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(readback_bytes)},none{0,0};
    HRESULT hr=l.readback->Map(0,&range,&data);if(SUCCEEDED(hr) && !data)hr=E_POINTER;
    if(FAILED(hr)){char label[96]{};sprintf_s(label,"Record bench frame %llu list %u: Map READBACK",frame,l.index);s.event("after",label,hr);return hr;}
    const UINT32* words=static_cast<const UINT32*>(data);
    const auto report=[&](const char* what,UINT draw,UINT word,UINT64 offset,UINT32 want,UINT32 got){
        if(t.reported>=report_limit)return;
        ++t.reported;
        char source[96]{},label[400]{};
        const Source from=identify(sh.seed,got,phase,frame);
        if(from.found && from.part==table_part)sprintf_s(source,"table %u word %u",from.draw,from.word);
        else if(from.found)sprintf_s(source,"from list %u frame %llu draw %u %s word %u",from.list,from.frame,from.draw,
            from.part==root_part?"root":"cbv",from.word);
        else strcpy_s(source,got<draws?"a slot index":"unknown");
        sprintf_s(label,"Record bench mismatch %s frame %llu list %u draw %u %s word %u, readback offset %llu, expected %08x, "
            "actual %08x, actual is %s",phase_name(phase),frame,l.index,draw,what,word,offset,want,got,source);
        s.event("after",label,E_FAIL);
    };
    for(UINT d=0;d<draws;++d){
        bool bad=false;
        for(UINT i=0;i<slot_words;++i){
            const UINT32 want=slot_value(sh.seed,phase,l.index,frame,d,i),got=words[UINT64{d}*slot_words+i];
            if(got==want)continue;
            bad=true;++t.mismatches;
            report("slot",d,i,(UINT64{d}*slot_words+i)*4,want,got);
        }
        if(bad)++t.mismatched_draws;
    }
    for(UINT target=0;target<2;++target){
        const UINT64 offset=target_offset+target*target_stride;
        UINT32 got=0;std::memcpy(&got,static_cast<const unsigned char*>(data)+offset,sizeof(got));
        const UINT32 want=target_value(sh.seed,phase,l.index,frame,target);
        if(got!=want){++t.mismatches;report("target",last_draw(target),target,offset,want,got);}
    }
    t.words+=UINT64{draws}*slot_words+2;
    l.readback->Unmap(0,&none);
    return S_OK;
}

struct Stat {double mean{},p50{},p95{},max{};};
inline Stat stat(std::vector<double> values){
    Stat result;if(values.empty())return result;
    std::sort(values.begin(),values.end());
    double sum=0;for(const double value:values)sum+=value;
    const size_t n=values.size();
    result.mean=sum/n;result.p50=values[(n-1)/2];result.p95=values[(std::min)(n-1,static_cast<size_t>(0.95*(n-1)+0.5))];
    result.max=values.back();
    return result;
}

// The phase's rows of recordbench-frames.csv, written in one call after the phase. A failure is traced; the summary
// lines in the trace do not depend on the file.
inline void write_rows(Shared& sh,size_t first){
    Session& s=sh.s;
    if(sh.csv==INVALID_HANDLE_VALUE){
        sh.csv=CreateFileW((s.root/L"recordbench-frames.csv").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(sh.csv==INVALID_HANDLE_VALUE){s.event("after","Record bench frames file not created",HRESULT_FROM_WIN32(GetLastError()));return;}
        const char header[]="phase,frame,record_cycles,work_cycles,submit_cycles,api_cycles_max,api_cycles_sum,"
            "record_us,submit_us,wait_us,verify_us,frame_us\n";
        DWORD written=0;
        if(!WriteFile(sh.csv,header,sizeof(header)-1,&written,nullptr) || written!=sizeof(header)-1)
            s.event("after","Record bench frames file header",HRESULT_FROM_WIN32(GetLastError()));
    }
    std::string rows;char row[320]{};
    for(size_t i=first;i<sh.samples.size();++i){
        const Sample& m=sh.samples[i];
        sprintf_s(row,"%s,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f\n",phase_name(m.phase),m.frame,m.record_cycles,
            m.work_cycles,m.submit_cycles,m.api_max,m.api_sum,m.record_us,m.submit_us,m.wait_us,m.verify_us,m.frame_us);
        rows+=row;
    }
    DWORD written=0;
    if(!WriteFile(sh.csv,rows.data(),static_cast<DWORD>(rows.size()),&written,nullptr) || written!=rows.size() || !FlushFileBuffers(sh.csv))
        s.event("after","Record bench frames file rows",HRESULT_FROM_WIN32(GetLastError()));
}

// One phase until 'end': frames of recording, submission, the fence and verification. Returns the first failure;
// a mismatch is counted in t, not returned. *joined is false when a recording thread did not end.
inline HRESULT run_phase(Shared& sh,unsigned phase,ULONGLONG end,Totals& t,bool* joined){
    Session& s=sh.s;Control& c=sh.control;HRESULT result=S_OK;char label[400]{};
    *joined=true;
    AcquireSRWLockExclusive(&c.lock);c.go=0;c.stop=false;c.ready=0;c.failure=S_OK;c.failed=-1;ReleaseSRWLockExclusive(&c.lock);
    std::thread handles[lists];unsigned launched=0;
    if(phase==threaded){
        try{for(;launched<lists;++launched){ListSet* l=&sh.sets[launched];handles[launched]=std::thread([&sh,l]{recorder(sh,*l);});}}
        catch(...){result=E_OUTOFMEMORY;s.event("after","Record bench starting threads",result);}
    }
    const size_t first=sh.samples.size();
    CpuTimes counted_from{};UINT64 counted_qpc=0,frame=0;
    try{
        for(;SUCCEEDED(result);++frame){
            if(GetTickCount64()>=end)break;
            if(s.abort_requested()){result=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
            if(frame==warmup){counted_from=cpu_times();counted_qpc=qpc();}
            Sample m;m.phase=phase;m.frame=frame;
            const UINT64 q0=qpc(),c0=cycles();
            if(phase==threaded){
                AcquireSRWLockExclusive(&c.lock);c.go=frame+1;c.ready=0;ReleaseSRWLockExclusive(&c.lock);
                WakeAllConditionVariable(&c.changed);
                const ULONGLONG ready_end=(std::min)(GetTickCount64()+record_ms,s.deadline);
                AcquireSRWLockExclusive(&c.lock);
                for(ULONGLONG now=GetTickCount64();c.ready<lists && now<ready_end;now=GetTickCount64())
                    SleepConditionVariableSRW(&c.changed,&c.lock,static_cast<DWORD>(ready_end-now),0);
                const unsigned ready=c.ready;const HRESULT failure=c.failure;const int failed=c.failed;
                for(const ListSet& l:sh.sets){m.api_max=(std::max)(m.api_max,l.api_cycles);m.api_sum+=l.api_cycles;m.work_cycles+=l.work_cycles;}
                ReleaseSRWLockExclusive(&c.lock);
                if(ready<lists){result=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                    sprintf_s(label,"Record bench threaded frame %llu: %u of %u threads ended their recording within %llu ms",frame,ready,lists,record_ms);
                    s.event("after",label,result);break;}
                if(FAILED(failure)){++t.failures;result=failure;
                    sprintf_s(label,"Record bench threaded frame %llu: recording failed on thread %d",frame,failed);s.event("after",label,failure);break;}
            }else{
                const UINT work=phase==interleaved?work_us:0;
                for(ListSet& l:sh.sets){result=record_list(sh,l,phase,frame,work,&m.work_cycles);if(FAILED(result)){++t.failures;break;}}
                if(FAILED(result))break;
            }
            const UINT64 q1=qpc(),c1=cycles();
            ID3D12CommandList* all[lists]{};for(unsigned i=0;i<lists;++i)all[i]=sh.sets[i].list.Get();
            s.pending=true;
            for(unsigned e=0;e<executes;++e){
                const unsigned from=e*lists/executes,to=(e+1)*lists/executes;
                s.queue->ExecuteCommandLists(to-from,all+from);
            }
            HRESULT hr=s.queue->Signal(sh.fence.Get(),++sh.value);
            const UINT64 q2=qpc(),c2=cycles();
            if(SUCCEEDED(hr))hr=wait(sh,sh.value);
            const UINT64 q3=qpc();
            const HRESULT reason=s.device->GetDeviceRemovedReason();
            if(FAILED(reason)){++t.removals;result=reason;
                sprintf_s(label,"Record bench device removed in %s frame %llu: reason %08lx",phase_name(phase),frame,static_cast<unsigned long>(reason));
                s.event("after",label,reason);break;}
            if(FAILED(hr)){result=hr;sprintf_s(label,"Record bench %s frame %llu: fence %llu not reached",phase_name(phase),frame,sh.value);
                s.event("after",label,hr);break;}
            for(ListSet& l:sh.sets){result=verify(sh,l,phase,frame,t);if(FAILED(result))break;}
            if(FAILED(result))break;
            const UINT64 q4=qpc();
            if(frame<warmup)continue;
            const auto us=[&sh](UINT64 ticks){return ticks*1e6/sh.qpf;};
            if(phase!=threaded){m.record_cycles=c1-c0;m.api_max=m.api_sum=m.record_cycles>m.work_cycles?m.record_cycles-m.work_cycles:0;}
            m.submit_cycles=c2-c1;
            m.record_us=us(q1-q0);m.submit_us=us(q2-q1);m.wait_us=us(q3-q2);m.verify_us=us(q4-q3);m.frame_us=us(q4-q0);
            sh.samples.push_back(m);++t.frames;
        }
    }catch(...){if(SUCCEEDED(result))result=E_OUTOFMEMORY;s.event("after","Record bench exception",result);}
    const CpuTimes counted_to=cpu_times();const UINT64 end_qpc=qpc();

    // The threads end at the stop flag; one that does not within 10 s stays behind with everything it can reach.
    AcquireSRWLockExclusive(&c.lock);c.stop=true;ReleaseSRWLockExclusive(&c.lock);WakeAllConditionVariable(&c.changed);
    const ULONGLONG join_end=GetTickCount64()+join_ms;
    for(unsigned i=0;i<launched;++i){
        const ULONGLONG now=GetTickCount64();
        if(WaitForSingleObject(static_cast<HANDLE>(handles[i].native_handle()),now<join_end?static_cast<DWORD>(join_end-now):0)==WAIT_OBJECT_0)
            handles[i].join();
        else *joined=false;
    }
    if(!*joined){
        for(unsigned i=0;i<launched;++i)if(handles[i].joinable())handles[i].detach();
        s.pending=true;s.detached=true;
        s.event("after","Record bench recording thread did not end; its objects stay for process teardown",HRESULT_FROM_WIN32(WAIT_TIMEOUT));
    }

    // Two summary lines: CPU, then wall time. Cycles become microseconds at the calibrated rate.
    try{
        const size_t n=sh.samples.size()-first;
        std::vector<double> api,api_sum,work,submit,record,submit_wall,wait_us,verify_us,frame_us;
        for(size_t i=first;i<sh.samples.size();++i){
            const Sample& m=sh.samples[i];const double rate=sh.cycles_per_us;
            api.push_back(m.api_max/rate);api_sum.push_back(m.api_sum/rate);work.push_back(m.work_cycles/rate);
            submit.push_back(m.submit_cycles/rate);record.push_back(m.record_us);submit_wall.push_back(m.submit_us);
            wait_us.push_back(m.wait_us);verify_us.push_back(m.verify_us);frame_us.push_back(m.frame_us);
        }
        const Stat a=stat(api),as=stat(api_sum),w=stat(work),sc=stat(submit),r=stat(record),sw=stat(submit_wall),gw=stat(wait_us),
            v=stat(verify_us),f=stat(frame_us);
        sprintf_s(label,"Record bench %s: frames %llu after %u warm-up, api cpu us mean %.1f p50 %.1f p95 %.1f max %.1f, all threads "
            "mean %.1f, work cpu us mean %.1f, submit cpu us mean %.1f",phase_name(phase),static_cast<UINT64>(n),warmup,a.mean,a.p50,
            a.p95,a.max,as.mean,w.mean,sc.mean);
        s.event("after",label,FAILED(result)?result:S_OK);
        // GetThreadTimes and GetProcessTimes count 100 ns units; over the counted frames, in ms per frame.
        const bool spans=n && counted_qpc;const double per_frame=spans?1e4*n:1;
        sprintf_s(label,"Record bench %s wall us: record mean %.1f p50 %.1f p95 %.1f, submit mean %.1f, gpu wait mean %.1f, verify "
            "mean %.1f, frame mean %.1f p50 %.1f p95 %.1f; cpu ms per frame: main thread %.3f, process %.3f, over %.0f ms",
            phase_name(phase),r.mean,r.p50,r.p95,sw.mean,gw.mean,v.mean,f.mean,f.p50,f.p95,
            spans?(counted_to.thread-counted_from.thread)/per_frame:0.0,spans?(counted_to.process-counted_from.process)/per_frame:0.0,
            spans?(end_qpc-counted_qpc)*1e3/sh.qpf:0.0);
        s.event("after",label,FAILED(result)?result:S_OK);
        write_rows(sh,first);
    }catch(...){if(SUCCEEDED(result))result=E_OUTOFMEMORY;s.event("after","Record bench summary",E_OUTOFMEMORY);}
    return result;
}
}  // namespace recordbench

inline HRESULT recordbench_run(Session& s){
    using namespace recordbench;
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    const ULONGLONG begin=GetTickCount64();
    const ULONGLONG last_start=s.deadline>begin+margin_ms?s.deadline-margin_ms:begin;
    const ULONGLONG bench_end=(std::min)(begin+budget_ms,last_start);
    Shared* shared=new(std::nothrow) Shared(s);if(!shared)return E_OUTOFMEMORY;
    Shared& sh=*shared;
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&sh.seed),sizeof(sh.seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){delete shared;return E_FAIL;}
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);sh.qpf=static_cast<double>(frequency.QuadPart);
    sh.cycles_per_us=calibrate(sh.qpf);
    // What the UMD reads once per process: absent, or the bounded text.
    char experiment[64]{};
    {   char text[96]{};SetLastError(ERROR_SUCCESS);
        const DWORD length=GetEnvironmentVariableA("AMDGPU_WDDM_D3D12_EXPERIMENT",text,sizeof(text));
        if(!length)strcpy_s(experiment,GetLastError()==ERROR_ENVVAR_NOT_FOUND?"absent":"empty");
        else if(length>=sizeof(experiment))strcpy_s(experiment,"longer than 63");
        else{for(char& c:text)if(c && !((c>='a' && c<='z') || (c>='0' && c<='9') || c==',' || c=='-'))c='?';
            strcpy_s(experiment,text);}
    }
    char label[400]{};
    sprintf_s(label,"Record bench seed %016llx, lists %u, draws per list %u, executes %u, work us per group %u, phases until %llu ms, "
        "cycles per us %.1f, AMDGPU_WDDM_D3D12_EXPERIMENT %s",sh.seed,lists,draws,executes,work_us,bench_end-begin,sh.cycles_per_us,experiment);
    s.event("after",label);
    HRESULT result=sh.cycles_per_us>0?setup(sh):E_FAIL;
    if(FAILED(result)){s.event("after","Record bench setup failed",result);delete shared;return result;}
    s.event("after","Record bench setup done",S_OK);
    try{sh.samples.reserve(4096);}catch(...){}

    // A phase counts as measured once it counted a frame past its warm-up; the run passes only with all three measured.
    Totals t;bool joined=true;unsigned phases=0;
    for(unsigned phase=burst;phase<phase_count && SUCCEEDED(result) && joined;++phase){
        const ULONGLONG now=GetTickCount64();
        if(now>=bench_end)break;
        const ULONGLONG end=now+(bench_end-now)/(phase_count-phase);
        const UINT64 before=t.frames;
        result=run_phase(sh,phase,end,t,&joined);
        if(t.frames>before)++phases;
    }
    const bool clean=SUCCEEDED(result) && phases==phase_count && !t.mismatches && !t.removals && !t.failures && !s.pending && joined;
    const double end_rate=calibrate(sh.qpf);
    sprintf_s(label,"Record bench: phases measured %u of %u, frames %llu, words %llu, mismatches %llu, mismatched draws %llu, removals %u, "
        "failures %u, %llu ms, cycles per us at the end %.1f, AMDGPU_WDDM_D3D12_EXPERIMENT %s",phases,static_cast<unsigned>(phase_count),
        t.frames,t.words,t.mismatches,t.mismatched_draws,t.removals,t.failures,GetTickCount64()-begin,end_rate,experiment);
    const HRESULT hr=clean?S_OK:FAILED(result)?result:E_FAIL;
    s.event("after",label,hr);
    // Objects whose GPU completion is unproven, or that a detached thread may still use, stay for process teardown.
    if(joined && !s.pending)delete shared;
    s.copy_success=clean;return hr;
}
