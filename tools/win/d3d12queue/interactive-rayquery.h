// SPDX-License-Identifier: MIT
// Ray query variant of the interactive client (build.ps1 -RayQuery). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then performs this scene, so commands and receipts keep their form.
//
// The scene of engine-ddi's test-raytracing.cpp through the public API only (ID3D12Device5,
// ID3D12GraphicsCommandList4): a bottom level of one triangle, a top level of one instance, then the cs_6_5 program
// of rayquery.hlsl traces an 8x8 grid of orthographic rays with an inline ray query, 1 for a hit and 2 for a miss.
// Builds and dispatch are recorded on one DIRECT list for the session's DIRECT queue. The READBACK buffer starts
// at a value that is neither 1 nor 2. The 64 words are computed on the CPU from the triangle's edges, which keep a
// margin from every ray, and every word is compared. The device must report raytracing tier 1.1.
#include "rayquery-program.h"

inline HRESULT rayquery(Session& s){
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    constexpr UINT grid=8,words=grid*grid;
    constexpr UINT32 hit=1,miss=2,prefill=0xA5A5A5A5u;
    constexpr UINT64 upload_bytes=1024,instance_offset=256,output_bytes=words*4;
    // The triangle at z = 0.5 in the rays' xy plane; ray (x, y) starts at ((x + 0.5) / 4 - 1, (y + 0.5) / 4 - 1).
    constexpr float triangle[3][3]{{-0.8f,-0.7f,0.5f},{0.1f,-0.7f,0.5f},{-0.7f,0.9f,0.5f}};
    ID3D12Device* device=s.device.Get();
    char label[128]{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    HRESULT hr=s.api("CheckFeatureSupport OPTIONS5",[&]{return device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options5,sizeof(options5));});
    sprintf_s(label,"Reported RaytracingTier %u",SUCCEEDED(hr)?static_cast<unsigned>(options5.RaytracingTier):0u);
    s.event("after",label,hr);if(FAILED(hr))return hr;
    if(options5.RaytracingTier<D3D12_RAYTRACING_TIER_1_1){
        s.event("after","RaytracingTier below 1.1, no ray query attempted",DXGI_ERROR_UNSUPPORTED);return DXGI_ERROR_UNSUPPORTED;}
    {   // Observation only: a pipeline refusal below reads differently without it.
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        const HRESULT query=s.api("CheckFeatureSupport SHADER_MODEL",[&]{return device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model));});
        sprintf_s(label,"Reported HighestShaderModel %x",SUCCEEDED(query)?static_cast<unsigned>(model.HighestShaderModel):0u);s.event("after",label,query);
    }

    // Expected word of each ray from the triangle's edge functions; no ray may lie within 0.05 of an edge line.
    UINT32 expected[words]{};UINT hits=0;bool margin=true;
    for(UINT y=0;y<grid;++y)for(UINT x=0;x<grid;++x){
        const double px=(x+0.5)/4.0-1.0,py=(y+0.5)/4.0-1.0;int positive=0;
        for(int e=0;e<3;++e){
            const float* a=triangle[e];const float* b=triangle[(e+1)%3];
            const double dx=double{b[0]}-a[0],dy=double{b[1]}-a[1],side=dx*(py-a[1])-dy*(px-a[0]);
            if(side*side<0.05*0.05*(dx*dx+dy*dy))margin=false;
            positive+=side>0?1:0;
        }
        expected[y*grid+x]=(positive==0 || positive==3)?hit:miss;hits+=expected[y*grid+x]==hit?1u:0u;
    }
    sprintf_s(label,"Expected pattern %u hits of %u, margin %s",hits,words,margin?"kept":"violated");
    s.event("after",label,margin && hits && hits<words?S_OK:E_UNEXPECTED);if(!margin || !hits || hits==words)return E_UNEXPECTED;

    ComPtr<ID3D12Device5> device5;
    hr=s.api("QueryInterface ID3D12Device5",[&]{return s.device.As(&device5);});if(FAILED(hr))return hr;
    s.extra[0]=device5;

    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=upload_bytes;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    hr=s.api("CreateCommittedResource UPLOAD",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.upload));});if(FAILED(hr))return hr;
    s.event("before","GetGPUVirtualAddress UPLOAD");const D3D12_GPU_VIRTUAL_ADDRESS upload_va=s.upload->GetGPUVirtualAddress();s.event("after","GetGPUVirtualAddress UPLOAD",upload_va?S_OK:E_FAIL);
    if(!upload_va)return E_FAIL;

    // Vertices at 0 and the instance description at 256 of the UPLOAD buffer.
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;geometry.Triangles.IndexFormat=DXGI_FORMAT_UNKNOWN;
    geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;geometry.Triangles.VertexCount=3;
    geometry.Triangles.VertexBuffer={upload_va,sizeof(triangle[0])};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};bottom.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;bottom.NumDescs=1;
    bottom.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;bottom.pGeometryDescs=&geometry;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;top.NumDescs=1;
    top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;top.InstanceDescs=upload_va+instance_offset;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_info{},top_info{};
    s.event("before","GetRaytracingAccelerationStructurePrebuildInfo bottom level");
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&bottom,&bottom_info);
    s.event("after","GetRaytracingAccelerationStructurePrebuildInfo bottom level");
    s.event("before","GetRaytracingAccelerationStructurePrebuildInfo top level");
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&top,&top_info);
    s.event("after","GetRaytracingAccelerationStructurePrebuildInfo top level");
    // A zero or implausibly large answer is refused before any buffer is sized from it.
    const auto sane=[](UINT64 bytes){return bytes && bytes<=(64ull<<20);};
    const bool sized=sane(bottom_info.ResultDataMaxSizeInBytes) && sane(bottom_info.ScratchDataSizeInBytes) &&
                     sane(top_info.ResultDataMaxSizeInBytes) && sane(top_info.ScratchDataSizeInBytes);
    sprintf_s(label,"Prebuild bottom level %llu bytes scratch %llu, top level %llu bytes scratch %llu",
        static_cast<unsigned long long>(bottom_info.ResultDataMaxSizeInBytes),static_cast<unsigned long long>(bottom_info.ScratchDataSizeInBytes),
        static_cast<unsigned long long>(top_info.ResultDataMaxSizeInBytes),static_cast<unsigned long long>(top_info.ScratchDataSizeInBytes));
    s.event("after",label,sized?S_OK:E_FAIL);if(!sized)return E_FAIL;

    // Structures and scratch: DEFAULT with unordered access; one scratch serves both builds in turn.
    heap.Type=D3D12_HEAP_TYPE_DEFAULT;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> blas,tlas,scratch;
    buffer.Width=bottom_info.ResultDataMaxSizeInBytes;
    hr=s.api("CreateCommittedResource bottom level",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,nullptr,IID_PPV_ARGS(&blas));});if(FAILED(hr))return hr;
    s.extra[1]=blas;
    buffer.Width=top_info.ResultDataMaxSizeInBytes;
    hr=s.api("CreateCommittedResource top level",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,nullptr,IID_PPV_ARGS(&tlas));});if(FAILED(hr))return hr;
    s.extra[2]=tlas;
    buffer.Width=(std::max)(bottom_info.ScratchDataSizeInBytes,top_info.ScratchDataSizeInBytes);
    hr=s.api("CreateCommittedResource scratch",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&scratch));});if(FAILED(hr))return hr;
    s.extra[3]=scratch;
    buffer.Width=output_bytes;
    hr=s.api("CreateCommittedResource OUTPUT",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s.middle));});if(FAILED(hr))return hr;
    heap.Type=D3D12_HEAP_TYPE_READBACK;buffer.Flags=D3D12_RESOURCE_FLAG_NONE;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});if(FAILED(hr))return hr;

    s.event("before","GetGPUVirtualAddress bottom level");const D3D12_GPU_VIRTUAL_ADDRESS blas_va=blas->GetGPUVirtualAddress();s.event("after","GetGPUVirtualAddress bottom level");
    constexpr UINT64 alignment=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    sprintf_s(label,"Bottom level GPU address 0x%016llx",static_cast<unsigned long long>(blas_va));
    s.event("after",label,blas_va && !(blas_va%alignment)?S_OK:E_FAIL);if(!blas_va || blas_va%alignment)return E_FAIL;
    s.event("before","GetGPUVirtualAddress top level, scratch, OUTPUT");
    const D3D12_GPU_VIRTUAL_ADDRESS tlas_va=tlas->GetGPUVirtualAddress(),scratch_va=scratch->GetGPUVirtualAddress(),output_va=s.middle->GetGPUVirtualAddress();
    const bool aligned=tlas_va && scratch_va && output_va && !(tlas_va%alignment) && !(scratch_va%alignment);
    s.event("after","GetGPUVirtualAddress top level, scratch, OUTPUT",aligned?S_OK:E_FAIL);if(!aligned)return E_FAIL;

    void* data=nullptr;const D3D12_RANGE empty{0,0};const D3D12_RANGE whole{0,static_cast<SIZE_T>(output_bytes)};
    hr=s.api("Map UPLOAD",[&]{return s.upload->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1.0f;
    instance.InstanceMask=0xFF;instance.AccelerationStructure=blas_va;
    std::memcpy(data,triangle,sizeof(triangle));
    std::memcpy(static_cast<unsigned char*>(data)+instance_offset,&instance,sizeof(instance));
    {const D3D12_RANGE written{0,static_cast<SIZE_T>(upload_bytes)};s.event("before","Unmap UPLOAD");s.upload->Unmap(0,&written);s.event("after","Unmap UPLOAD");}
    data=nullptr;hr=s.api("Map READBACK prefill",[&]{return s.readback->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT i=0;i<words;++i)std::memcpy(static_cast<unsigned char*>(data)+i*4,&prefill,4);
    s.event("before","Unmap READBACK prefill");s.readback->Unmap(0,&whole);s.event("after","Unmap READBACK prefill");
    data=nullptr;hr=s.api("Map READBACK before submit",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    bool untouched=true;
    for(UINT i=0;i<words;++i){UINT32 word=0;std::memcpy(&word,static_cast<const unsigned char*>(data)+i*4,4);if(word!=prefill){untouched=false;break;}}
    s.event("before","Unmap READBACK before submit");s.readback->Unmap(0,&empty);s.event("after","Unmap READBACK before submit");
    s.event("after","Destination holds prefill before submit",untouched?S_OK:E_FAIL);if(!untouched)return E_FAIL;

    // Root signature: the top level as a root SRV t0 and the output as a root UAV u0, both by address.
    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
    ComPtr<ID3DBlob> blob,errors;
    hr=s.api("D3D12SerializeRootSignature",[&]{return serialize(&signature,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    ComPtr<ID3D12RootSignature> root;
    hr=s.api("CreateRootSignature",[&]{return device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));});if(FAILED(hr))return hr;
    s.extra[4]=root;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();pipeline.CS={g_rayquery_cs,sizeof(g_rayquery_cs)};
    ComPtr<ID3D12PipelineState> state;
    hr=s.api("CreateComputePipelineState cs_6_5 ray query",[&]{return device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&state));});if(FAILED(hr))return hr;
    s.extra[5]=state;

    hr=s.api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));});if(FAILED(hr))return hr;
    hr=s.api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));});if(FAILED(hr))return hr;
    ComPtr<ID3D12GraphicsCommandList4> list4;
    hr=s.api("QueryInterface ID3D12GraphicsCommandList4",[&]{return s.list.As(&list4);});if(FAILED(hr))return hr;
    s.extra[6]=list4;
    hr=s.api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence));});if(FAILED(hr))return hr;
    ID3D12GraphicsCommandList4* list=list4.Get();
    const auto record=[&s](const char* what,auto&& call){s.event("before",what);call();s.event("after",what);};

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.DestAccelerationStructureData=blas_va;build.Inputs=bottom;build.ScratchAccelerationStructureData=scratch_va;
    record("BuildRaytracingAccelerationStructure bottom level",[&]{list->BuildRaytracingAccelerationStructure(&build,0,nullptr);});
    // No resource: every unordered access, the scratch's reuse and the bottom level before its reader.
    D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    record("ResourceBarrier UAV after bottom level",[&]{list->ResourceBarrier(1,&uav);});
    build.DestAccelerationStructureData=tlas_va;build.Inputs=top;
    record("BuildRaytracingAccelerationStructure top level",[&]{list->BuildRaytracingAccelerationStructure(&build,0,nullptr);});
    record("ResourceBarrier UAV after top level",[&]{list->ResourceBarrier(1,&uav);});
    record("SetComputeRootSignature",[&]{list->SetComputeRootSignature(root.Get());});
    record("SetPipelineState",[&]{list->SetPipelineState(state.Get());});
    record("SetComputeRootShaderResourceView top level",[&]{list->SetComputeRootShaderResourceView(0,tlas_va);});
    record("SetComputeRootUnorderedAccessView OUTPUT",[&]{list->SetComputeRootUnorderedAccessView(1,output_va);});
    record("Dispatch 1 1 1",[&]{list->Dispatch(1,1,1);});
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=s.middle.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    record("ResourceBarrier UNORDERED_ACCESS to COPY_SOURCE",[&]{list->ResourceBarrier(1,&barrier);});
    record("CopyBufferRegion OUTPUT to READBACK",[&]{list->CopyBufferRegion(s.readback.Get(),0,s.middle.Get(),0,output_bytes);});
    hr=s.api("Close CommandList",[&]{return list->Close();});if(FAILED(hr))return hr;

    ID3D12CommandList* commands[]={s.list.Get()};s.pending=true;
    s.event("before","ExecuteCommandLists");s.queue->ExecuteCommandLists(1,commands);s.event("after","ExecuteCommandLists");
    hr=s.api("Queue Signal 1",[&]{return s.queue->Signal(s.fence.Get(),1);});if(FAILED(hr))return hr;
    HANDLE completed_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completed_event)return HRESULT_FROM_WIN32(GetLastError());
    hr=s.api("SetEventOnCompletion 1",[&]{return s.fence->SetEventOnCompletion(1,completed_event);});
    if(SUCCEEDED(hr)){
        ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD wait=WAIT_TIMEOUT;
        s.event("before","WaitForFence bounded");
        while(GetTickCount64()<end && !s.abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(completed_event,20);
        s.event("after","WaitForFence bounded",wait==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        s.event("before","GetCompletedValue");UINT64 completed=s.fence->GetCompletedValue();s.event("after","GetCompletedValue",completed>=1 && completed!=UINT64_MAX?S_OK:E_FAIL);
        if(completed==UINT64_MAX)hr=DXGI_ERROR_DEVICE_REMOVED;
        else if(completed<1)hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        else s.pending=false;
    }
    CloseHandle(completed_event);if(FAILED(hr))return hr;

    data=nullptr;hr=s.api("Map READBACK",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    UINT equal=0,got=0,first=words;
    for(UINT i=0;i<words;++i){
        UINT32 word=0;std::memcpy(&word,static_cast<const unsigned char*>(data)+i*4,4);
        got+=word==hit?1u:0u;
        if(word==expected[i])++equal;else if(first==words)first=i;
    }
    s.event("before","Unmap READBACK");s.readback->Unmap(0,&empty);s.event("after","Unmap READBACK");
    sprintf_s(label,"Ray query %u of %u words equal, %u hits, first difference %d",equal,words,got,first==words?-1:static_cast<int>(first));
    const bool exact=equal==words && got && got<words;
    s.event("after",label,exact?S_OK:E_FAIL);
    hr=s.api("GetDeviceRemovedReason",[&]{return device->GetDeviceRemovedReason();});if(FAILED(hr))return hr;
    hr=exact?S_OK:E_FAIL;s.copy_success=exact;return hr;
}
