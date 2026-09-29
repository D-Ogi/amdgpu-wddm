// SPDX-License-Identifier: MIT
// Draw variant of the interactive client (build.ps1 -Draw). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then performs this draw, so commands and receipts keep their form.
//
// It repeats at the API what engine-ddi's harness does at the DDI (tests/test-shaders.cpp, round trip 3), with
// the same fxc programs: one triangle covers a 64x64 R32_UINT render target and the pixel program writes
//   seed ^ tag ^ ((y << 16) | x) ^ (pair.y << 8) ^ pair.x
// with tag B0250000 from the vertex buffer, pair (51, 3) from the vertex program and the seed from a root
// constant. Every word is computed on the CPU and compared; nothing depends on another renderer's output.
#include "../../../driver/umd/d3d12/engine-ddi/tests/fixture-gfx.h"

inline HRESULT draw(Session& s){
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    constexpr UINT side=64;
    unsigned char random[4]{};
    if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return E_FAIL;
    UINT32 seed=0;std::memcpy(&seed,random,sizeof(seed));
    const auto expected=[seed](UINT x,UINT y){return seed^0xB0250000u^((y<<16)|x)^(3u<<8)^0x51u;};
    {char label[48]{};sprintf_s(label,"Pattern seed %08x",seed);s.event("after",label);}
    ID3D12Device* device=s.device.Get();

    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues=1;parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=1;signature.pParameters=&parameter;
    signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob,errors;
    HRESULT hr=s.api("D3D12SerializeRootSignature",[&]{return serialize(&signature,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    ComPtr<ID3D12RootSignature> root;
    hr=s.api("CreateRootSignature",[&]{return device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));});if(FAILED(hr))return hr;
    s.extra[0]=root;

    const D3D12_INPUT_ELEMENT_DESC elements[]{
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TAG",0,DXGI_FORMAT_R32_UINT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();
    pipeline.VS={g_fixture_vs,sizeof(g_fixture_vs)};pipeline.PS={g_fixture_ps,sizeof(g_fixture_ps)};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask=0xffffffffu;
    pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable=TRUE;
    pipeline.InputLayout={elements,2};pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R32_UINT;pipeline.SampleDesc.Count=1;
    ComPtr<ID3D12PipelineState> state;
    hr=s.api("CreateGraphicsPipelineState",[&]{return device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&state));});if(FAILED(hr))return hr;
    s.extra[1]=state;

    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=side;texture.Height=side;
    texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=DXGI_FORMAT_R32_UINT;texture.SampleDesc.Count=1;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cleared{};cleared.Format=DXGI_FORMAT_R32_UINT;
    hr=s.api("CreateCommittedResource TARGET",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,&cleared,IID_PPV_ARGS(&s.middle));});if(FAILED(hr))return hr;
    D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> view_heap;
    hr=s.api("CreateDescriptorHeap RTV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&view_heap));});if(FAILED(hr))return hr;
    s.extra[2]=view_heap;
    const D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();
    s.event("before","CreateRenderTargetView");device->CreateRenderTargetView(s.middle.Get(),nullptr,view);s.event("after","CreateRenderTargetView");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT rows=0;UINT64 row_bytes=0,total=0;
    s.event("before","GetCopyableFootprints");device->GetCopyableFootprints(&texture,0,1,0,&placed,&rows,&row_bytes,&total);s.event("after","GetCopyableFootprints");
    if(rows!=side || row_bytes!=side*4ull || placed.Offset!=0 || placed.Footprint.RowPitch<side*4 || !total || total>(1u<<20))return E_UNEXPECTED;
    const UINT pitch=placed.Footprint.RowPitch;

    struct Vertex {float x,y;UINT32 tag;};static_assert(sizeof(Vertex)==12);
    const Vertex corners[3]{{-1.0f,-1.0f,0xB0250000u},{-1.0f,3.0f,0xB0250000u},{3.0f,-1.0f,0xB0250000u}};
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizeof(corners);
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    hr=s.api("CreateCommittedResource VERTICES",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.upload));});if(FAILED(hr))return hr;
    buffer.Width=total;heap.Type=D3D12_HEAP_TYPE_READBACK;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});if(FAILED(hr))return hr;

    void* data=nullptr;const D3D12_RANGE empty{0,0};const D3D12_RANGE whole{0,static_cast<SIZE_T>(total)};
    hr=s.api("Map READBACK prefill",[&]{return s.readback->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const UINT32 word=~expected(x,y);std::memcpy(static_cast<unsigned char*>(data)+y*pitch+x*4,&word,4);}
    s.readback->Unmap(0,&whole);s.event("after","Unmap READBACK prefill");
    data=nullptr;hr=s.api("Map VERTICES",[&]{return s.upload->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    std::memcpy(data,corners,sizeof(corners));
    {const D3D12_RANGE written{0,sizeof(corners)};s.upload->Unmap(0,&written);s.event("after","Unmap VERTICES");}
    data=nullptr;hr=s.api("Map READBACK before submit",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    bool untouched=true;
    for(UINT y=0;y<side && untouched;++y)for(UINT x=0;x<side;++x){UINT32 word=0;std::memcpy(&word,static_cast<const unsigned char*>(data)+y*pitch+x*4,4);if(word!=~expected(x,y)){untouched=false;break;}}
    s.readback->Unmap(0,&empty);s.event("after","Destination holds complement before submit",untouched?S_OK:E_FAIL);
    if(!untouched)return E_FAIL;

    hr=s.api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));});if(FAILED(hr))return hr;
    hr=s.api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));});if(FAILED(hr))return hr;
    hr=s.api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence));});if(FAILED(hr))return hr;
    ID3D12GraphicsCommandList* list=s.list.Get();
    const auto step=[&s](const char* label){s.event("after",label);};
    list->SetGraphicsRootSignature(root.Get());step("SetGraphicsRootSignature");
    list->SetPipelineState(state.Get());step("SetPipelineState");
    list->SetGraphicsRoot32BitConstant(0,seed,0);step("SetGraphicsRoot32BitConstant");
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);step("IASetPrimitiveTopology");
    const D3D12_VERTEX_BUFFER_VIEW vertices{s.upload->GetGPUVirtualAddress(),sizeof(corners),sizeof(Vertex)};
    list->IASetVertexBuffers(0,1,&vertices);step("IASetVertexBuffers");
    const D3D12_VIEWPORT viewport{0.0f,0.0f,static_cast<FLOAT>(side),static_cast<FLOAT>(side),0.0f,1.0f};
    list->RSSetViewports(1,&viewport);step("RSSetViewports");
    const D3D12_RECT scissor{0,0,static_cast<LONG>(side),static_cast<LONG>(side)};
    list->RSSetScissorRects(1,&scissor);step("RSSetScissorRects");
    list->OMSetRenderTargets(1,&view,FALSE,nullptr);step("OMSetRenderTargets");
    const FLOAT zero[4]{};
    list->ClearRenderTargetView(view,zero,0,nullptr);step("ClearRenderTargetView");
    list->DrawInstanced(3,1,0,0);step("DrawInstanced 3");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=s.middle.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&barrier);step("ResourceBarrier RENDER_TARGET to COPY_SOURCE");
    D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=s.readback.Get();
    destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=placed;
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=s.middle.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);step("CopyTextureRegion TARGET to READBACK");
    hr=s.api("Close CommandList",[&]{return list->Close();});if(FAILED(hr))return hr;

    ID3D12CommandList* commands[]={list};s.pending=true;
    s.event("before","ExecuteCommandLists");s.queue->ExecuteCommandLists(1,commands);s.event("after","ExecuteCommandLists");
    hr=s.api("Queue Signal 1",[&]{return s.queue->Signal(s.fence.Get(),1);});if(FAILED(hr))return hr;
    HANDLE completed_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completed_event)return HRESULT_FROM_WIN32(GetLastError());
    hr=s.api("SetEventOnCompletion 1",[&]{return s.fence->SetEventOnCompletion(1,completed_event);});
    if(SUCCEEDED(hr)){
        const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD wait=WAIT_TIMEOUT;
        s.event("before","WaitForFence bounded");
        while(GetTickCount64()<end && !s.abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(completed_event,20);
        s.event("after","WaitForFence bounded",wait==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        const UINT64 completed=s.fence->GetCompletedValue();
        s.event("after","GetCompletedValue",completed>=1 && completed!=UINT64_MAX?S_OK:E_FAIL);
        if(completed==UINT64_MAX)hr=DXGI_ERROR_DEVICE_REMOVED;
        else if(completed<1)hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        else s.pending=false;
    }
    CloseHandle(completed_event);if(FAILED(hr))return hr;

    data=nullptr;hr=s.api("Map READBACK",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    unsigned different=0;UINT first_x=0,first_y=0;UINT32 first_word=0;
    for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){
        UINT32 word=0;std::memcpy(&word,static_cast<const unsigned char*>(data)+y*pitch+x*4,4);
        if(word!=expected(x,y) && !different++){first_x=x;first_y=y;first_word=word;}
    }
    s.readback->Unmap(0,&empty);
    if(different){char label[96]{};sprintf_s(label,"Words different %u first x %u y %u got %08x want %08x",different,first_x,first_y,first_word,expected(first_x,first_y));s.event("after",label,E_FAIL);}
    hr=different?E_FAIL:S_OK;s.event("after","Compare 4096 exact words",hr);s.copy_success=!different;return hr;
}
