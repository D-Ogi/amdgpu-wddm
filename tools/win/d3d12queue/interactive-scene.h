// SPDX-License-Identifier: MIT
// Scene variant of the interactive client (build.ps1 -Scene). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then performs this scene, so commands and receipts keep their form.
//
// Four indexed triangles, each covering a 64x64 R32_UINT render target, drawn with a depth buffer (LESS) and a
// source texture read through a descriptor table. The pixel program writes
//   seed ^ tag ^ source[x, y]        with source[x, y] = (seed * 2654435761) ^ ((y << 16) | x)
//
//   draw  scissor   depth  tag        result
//   1     whole     0.5    A0000000   everywhere
//   2     x < 32    0.25   B0000000   passes on the left half
//   3     y < 32    0.75   C0000000   fails everywhere
//   4     y >= 32   0.4    D0000000   passes only where draw 1 remains, so the depth of draw 2 was written
//
// The vertex buffer holds the four triangles in another order than the draws (slots 1, 3, 0, 2) and only the
// index buffer names the right one, so a draw that ignored the indices would use another draw's tag and depth.
//
// Every word is computed on the CPU and compared; nothing depends on another renderer's output.
#include "scene-programs.h"

inline HRESULT scene(Session& s){
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    constexpr UINT side=64,half=32;
    unsigned char random[4]{};
    if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return E_FAIL;
    UINT32 seed=0;std::memcpy(&seed,random,sizeof(seed));
    const auto source_word=[seed](UINT x,UINT y){return (seed*2654435761u)^((y<<16)|x);};
    const auto tag_at=[](UINT x,UINT y){return x<32u?0xB0000000u:y>=32u?0xD0000000u:0xA0000000u;};
    const auto expected=[&](UINT x,UINT y){return seed^tag_at(x,y)^source_word(x,y);};
    {char label[48]{};sprintf_s(label,"Pattern seed %08x",seed);s.event("after",label);}
    ID3D12Device* device=s.device.Get();
    HRESULT hr=S_OK;

    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=1;
    parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges=1;parameters[1].DescriptorTable.pDescriptorRanges=&range;
    parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
    signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob,errors;
    hr=s.api("D3D12SerializeRootSignature",[&]{return serialize(&signature,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    ComPtr<ID3D12RootSignature> root;
    hr=s.api("CreateRootSignature",[&]{return device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));});if(FAILED(hr))return hr;
    s.extra[0]=root;

    const D3D12_INPUT_ELEMENT_DESC elements[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TAG",0,DXGI_FORMAT_R32_UINT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();
    pipeline.VS={g_scene_vs,sizeof(g_scene_vs)};pipeline.PS={g_scene_ps,sizeof(g_scene_ps)};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask=0xffffffffu;
    pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable=TRUE;
    pipeline.DepthStencilState.DepthEnable=TRUE;pipeline.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    pipeline.InputLayout={elements,2};pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=DXGI_FORMAT_R32_UINT;pipeline.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    pipeline.SampleDesc.Count=1;
    ComPtr<ID3D12PipelineState> state;
    hr=s.api("CreateGraphicsPipelineState",[&]{return device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&state));});if(FAILED(hr))return hr;
    s.extra[1]=state;

    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=side;texture.Height=side;
    texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=DXGI_FORMAT_R32_UINT;texture.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> source,depth;
    hr=s.api("CreateCommittedResource SOURCE",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&source));});if(FAILED(hr))return hr;
    s.extra[2]=source;
    D3D12_RESOURCE_DESC target=texture;target.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cleared{};cleared.Format=DXGI_FORMAT_R32_UINT;
    hr=s.api("CreateCommittedResource TARGET",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,D3D12_RESOURCE_STATE_RENDER_TARGET,&cleared,IID_PPV_ARGS(&s.middle));});if(FAILED(hr))return hr;
    D3D12_RESOURCE_DESC depth_desc=texture;depth_desc.Format=DXGI_FORMAT_D32_FLOAT;depth_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE far_plane{};far_plane.Format=DXGI_FORMAT_D32_FLOAT;far_plane.DepthStencil.Depth=1.0f;
    hr=s.api("CreateCommittedResource DEPTH",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&depth_desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&far_plane,IID_PPV_ARGS(&depth));});if(FAILED(hr))return hr;
    s.extra[3]=depth;

    ComPtr<ID3D12DescriptorHeap> target_views,depth_views,shader_views;
    D3D12_DESCRIPTOR_HEAP_DESC views{};views.NumDescriptors=1;
    views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr=s.api("CreateDescriptorHeap RTV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&target_views));});if(FAILED(hr))return hr;
    s.extra[4]=target_views;
    views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hr=s.api("CreateDescriptorHeap DSV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&depth_views));});if(FAILED(hr))return hr;
    s.extra[5]=depth_views;
    views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;views.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=s.api("CreateDescriptorHeap SRV shader visible",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&shader_views));});if(FAILED(hr))return hr;
    s.extra[6]=shader_views;
    const D3D12_CPU_DESCRIPTOR_HANDLE target_view=target_views->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE depth_view=depth_views->GetCPUDescriptorHandleForHeapStart();
    s.event("before","CreateRenderTargetView");device->CreateRenderTargetView(s.middle.Get(),nullptr,target_view);s.event("after","CreateRenderTargetView");
    s.event("before","CreateDepthStencilView");device->CreateDepthStencilView(depth.Get(),nullptr,depth_view);s.event("after","CreateDepthStencilView");
    s.event("before","CreateShaderResourceView");device->CreateShaderResourceView(source.Get(),nullptr,shader_views->GetCPUDescriptorHandleForHeapStart());s.event("after","CreateShaderResourceView");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT rows=0;UINT64 row_bytes=0,total=0;
    s.event("before","GetCopyableFootprints");device->GetCopyableFootprints(&texture,0,1,0,&placed,&rows,&row_bytes,&total);s.event("after","GetCopyableFootprints");
    if(rows!=side || row_bytes!=side*4ull || placed.Offset!=0 || placed.Footprint.RowPitch<side*4 || !total || total>(1u<<20))return E_UNEXPECTED;
    // The CPU loops below address row y at y * RowPitch: the last word must lie inside the reported total.
    if(UINT64{side-1}*placed.Footprint.RowPitch+UINT64{side}*4>total)return E_UNEXPECTED;
    const UINT pitch=placed.Footprint.RowPitch;

    struct Vertex {float x,y,z;UINT32 tag;};static_assert(sizeof(Vertex)==16);
    struct Pass {float z;UINT32 tag;D3D12_RECT scissor;};
    const Pass passes[4]{
        {0.5f,0xA0000000u,{0,0,static_cast<LONG>(side),static_cast<LONG>(side)}},
        {0.25f,0xB0000000u,{0,0,static_cast<LONG>(half),static_cast<LONG>(side)}},
        {0.75f,0xC0000000u,{0,0,static_cast<LONG>(side),static_cast<LONG>(half)}},
        {0.4f,0xD0000000u,{0,static_cast<LONG>(half),static_cast<LONG>(side),static_cast<LONG>(side)}}};
    Vertex corners[12]{};UINT16 indices[12]{};
    constexpr UINT slot_of[4]{1,3,0,2};
    for(UINT pass=0;pass<4;++pass){
        const UINT first=slot_of[pass]*3;
        corners[first+0]={-1.0f,-1.0f,passes[pass].z,passes[pass].tag};
        corners[first+1]={-1.0f,3.0f,passes[pass].z,passes[pass].tag};
        corners[first+2]={3.0f,-1.0f,passes[pass].z,passes[pass].tag};
        for(UINT corner=0;corner<3;++corner)indices[pass*3+corner]=static_cast<UINT16>(first+2-corner);
    }
    const UINT64 vertex_offset=(total+15)&~15ull,index_offset=vertex_offset+sizeof(corners);
    const UINT64 upload_bytes=index_offset+sizeof(indices);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=upload_bytes;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    hr=s.api("CreateCommittedResource UPLOAD",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.upload));});if(FAILED(hr))return hr;
    buffer.Width=total;heap.Type=D3D12_HEAP_TYPE_READBACK;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});if(FAILED(hr))return hr;

    void* data=nullptr;const D3D12_RANGE empty{0,0};const D3D12_RANGE whole{0,static_cast<SIZE_T>(total)};
    hr=s.api("Map READBACK prefill",[&]{return s.readback->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const UINT32 word=~expected(x,y);std::memcpy(static_cast<unsigned char*>(data)+y*pitch+x*4,&word,4);}
    s.readback->Unmap(0,&whole);s.event("after","Unmap READBACK prefill");
    data=nullptr;hr=s.api("Map UPLOAD",[&]{return s.upload->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){const UINT32 word=source_word(x,y);std::memcpy(static_cast<unsigned char*>(data)+y*pitch+x*4,&word,4);}
    std::memcpy(static_cast<unsigned char*>(data)+vertex_offset,corners,sizeof(corners));
    std::memcpy(static_cast<unsigned char*>(data)+index_offset,indices,sizeof(indices));
    {const D3D12_RANGE written{0,static_cast<SIZE_T>(upload_bytes)};s.upload->Unmap(0,&written);s.event("after","Unmap UPLOAD");}
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
    D3D12_TEXTURE_COPY_LOCATION texels{};texels.pResource=s.upload.Get();
    texels.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;texels.PlacedFootprint=placed;
    D3D12_TEXTURE_COPY_LOCATION filled{};filled.pResource=source.Get();filled.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&filled,0,0,0,&texels,nullptr);step("CopyTextureRegion UPLOAD to SOURCE");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=source.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1,&barrier);step("ResourceBarrier COPY_DEST to PIXEL_SHADER_RESOURCE");
    list->SetGraphicsRootSignature(root.Get());step("SetGraphicsRootSignature");
    list->SetPipelineState(state.Get());step("SetPipelineState");
    ID3D12DescriptorHeap* bound[]{shader_views.Get()};
    list->SetDescriptorHeaps(1,bound);step("SetDescriptorHeaps");
    list->SetGraphicsRoot32BitConstant(0,seed,0);step("SetGraphicsRoot32BitConstant");
    list->SetGraphicsRootDescriptorTable(1,shader_views->GetGPUDescriptorHandleForHeapStart());step("SetGraphicsRootDescriptorTable");
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);step("IASetPrimitiveTopology");
    const D3D12_GPU_VIRTUAL_ADDRESS base=s.upload->GetGPUVirtualAddress();
    const D3D12_VERTEX_BUFFER_VIEW vertices{base+vertex_offset,sizeof(corners),sizeof(Vertex)};
    list->IASetVertexBuffers(0,1,&vertices);step("IASetVertexBuffers");
    const D3D12_INDEX_BUFFER_VIEW index_view{base+index_offset,sizeof(indices),DXGI_FORMAT_R16_UINT};
    list->IASetIndexBuffer(&index_view);step("IASetIndexBuffer");
    const D3D12_VIEWPORT viewport{0.0f,0.0f,static_cast<FLOAT>(side),static_cast<FLOAT>(side),0.0f,1.0f};
    list->RSSetViewports(1,&viewport);step("RSSetViewports");
    list->OMSetRenderTargets(1,&target_view,FALSE,&depth_view);step("OMSetRenderTargets with depth");
    const FLOAT zero[4]{};
    list->ClearRenderTargetView(target_view,zero,0,nullptr);step("ClearRenderTargetView");
    list->ClearDepthStencilView(depth_view,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);step("ClearDepthStencilView");
    for(UINT pass=0;pass<4;++pass){
        list->RSSetScissorRects(1,&passes[pass].scissor);
        list->DrawIndexedInstanced(3,1,pass*3,0,0);
        char label[40]{};sprintf_s(label,"DrawIndexedInstanced pass %u",pass+1);step(label);
    }
    barrier.Transition.pResource=s.middle.Get();
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&barrier);step("ResourceBarrier RENDER_TARGET to COPY_SOURCE");
    D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=s.readback.Get();
    destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=placed;
    D3D12_TEXTURE_COPY_LOCATION drawn{};drawn.pResource=s.middle.Get();drawn.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination,0,0,0,&drawn,nullptr);step("CopyTextureRegion TARGET to READBACK");
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
