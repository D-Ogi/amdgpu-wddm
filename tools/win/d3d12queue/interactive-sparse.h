// SPDX-License-Identifier: MIT
// Sparse variant of the interactive client (build.ps1 -Sparse). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then performs this round trip, so commands and receipts keep
// their form.
//
// A reserved buffer of four tiles gets its memory from a heap through the queue's UpdateTileMappings: tiles
// 0 to 3 of the buffer are heap tiles 2 to 5. A pattern goes UPLOAD -> reserved buffer -> READBACK and every
// byte is compared. The destination starts as the pattern's complement, as in the plain copy. Only mapped
// tiles are written and read: what an unmapped tile reads as is not asked here.
inline HRESULT sparse(Session& s){
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    constexpr UINT64 tile=D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    constexpr UINT tiles=4,heap_tiles=8,first_heap_tile=2;
    constexpr UINT64 size=tiles*tile;
    ID3D12Device* device=s.device.Get();
    {   // What the runtime says about tiles; the receipt is the round trip, not this answer.
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};char label[64]{};
        HRESULT query=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options));
        sprintf_s(label,"Reported TiledResourcesTier %u",SUCCEEDED(query)?static_cast<unsigned>(options.TiledResourcesTier):0u);
        s.event("after",label,query);
    }
    unsigned char seed[4]{};
    if(BCryptGenRandom(nullptr,seed,sizeof(seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return E_FAIL;
    // The tile index enters the byte, so a tile that reads another tile's memory fails the comparison.
    const auto expected=[&seed,tile](size_t i){
        return static_cast<unsigned char>(((i*37+11)^(i>>3))+seed[i&3]+seed[(i>>7)&3]+static_cast<unsigned char>(i/tile)*29);};
    {char label[48]{};sprintf_s(label,"Pattern seed %02x%02x%02x%02x",seed[0],seed[1],seed[2],seed[3]);s.event("after",label);}

    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    HRESULT hr=s.api("CreateCommittedResource UPLOAD",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.upload));});if(FAILED(hr))return hr;
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});if(FAILED(hr))return hr;
    hr=s.api("CreateReservedResource 4 tiles",[&]{return device->CreateReservedResource(&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.middle));});if(FAILED(hr))return hr;
    D3D12_HEAP_DESC memory{};memory.SizeInBytes=heap_tiles*tile;memory.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;
    memory.Properties.CreationNodeMask=memory.Properties.VisibleNodeMask=1;
    memory.Alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;memory.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> tile_heap;
    hr=s.api("CreateHeap 8 tiles",[&]{return device->CreateHeap(&memory,IID_PPV_ARGS(&tile_heap));});if(FAILED(hr))return hr;
    s.extra[0]=tile_heap;

    void* data=nullptr;D3D12_RANGE empty{0,0};D3D12_RANGE whole{0,static_cast<SIZE_T>(size)};
    hr=s.api("Map READBACK prefill",[&]{return s.readback->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(size_t i=0;i<size;++i)static_cast<unsigned char*>(data)[i]=static_cast<unsigned char>(~expected(i));
    s.event("before","Unmap READBACK prefill");s.readback->Unmap(0,&whole);s.event("after","Unmap READBACK prefill");
    data=nullptr;hr=s.api("Map UPLOAD",[&]{return s.upload->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(size_t i=0;i<size;++i)static_cast<unsigned char*>(data)[i]=expected(i);
    s.event("before","Unmap UPLOAD");s.upload->Unmap(0,&whole);s.event("after","Unmap UPLOAD");

    hr=s.api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));});if(FAILED(hr))return hr;
    hr=s.api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));});if(FAILED(hr))return hr;
    hr=s.api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence));});if(FAILED(hr))return hr;

    // From here the queue holds work that names the heap: nothing is released before the fence says so.
    const D3D12_TILED_RESOURCE_COORDINATE start{0,0,0,0};
    D3D12_TILE_REGION_SIZE region{};region.NumTiles=tiles;
    const D3D12_TILE_RANGE_FLAGS range=D3D12_TILE_RANGE_FLAG_NONE;const UINT heap_start=first_heap_tile,count=tiles;
    s.pending=true;
    s.event("before","UpdateTileMappings 4 tiles to heap tiles 2-5");
    s.queue->UpdateTileMappings(s.middle.Get(),1,&start,&region,tile_heap.Get(),1,&range,&heap_start,&count,D3D12_TILE_MAPPING_FLAG_NONE);
    s.event("after","UpdateTileMappings 4 tiles to heap tiles 2-5");
    hr=s.api("GetDeviceRemovedReason after mapping",[&]{return device->GetDeviceRemovedReason();});if(FAILED(hr))return hr;

    s.event("before","CopyBufferRegion UPLOAD to RESERVED");s.list->CopyBufferRegion(s.middle.Get(),0,s.upload.Get(),0,size);s.event("after","CopyBufferRegion UPLOAD to RESERVED");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=s.middle.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    s.event("before","ResourceBarrier COPY_DEST to COPY_SOURCE");s.list->ResourceBarrier(1,&barrier);s.event("after","ResourceBarrier COPY_DEST to COPY_SOURCE");
    s.event("before","CopyBufferRegion RESERVED to READBACK");s.list->CopyBufferRegion(s.readback.Get(),0,s.middle.Get(),0,size);s.event("after","CopyBufferRegion RESERVED to READBACK");
    hr=s.api("Close CommandList",[&]{return s.list->Close();});if(FAILED(hr))return hr;
    ID3D12CommandList* commands[]={s.list.Get()};
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
    size_t equal=0,first=size;
    for(size_t i=0;i<size;++i){if(static_cast<const unsigned char*>(data)[i]==expected(i))++equal;else if(first==size)first=i;}
    s.event("before","Unmap READBACK");s.readback->Unmap(0,&empty);s.event("after","Unmap READBACK");
    {char label[96]{};sprintf_s(label,"Reserved buffer %llu of %llu bytes equal, first difference %lld",
        static_cast<unsigned long long>(equal),static_cast<unsigned long long>(size),first==size?-1ll:static_cast<long long>(first));
     s.event("after",label,equal==size?S_OK:E_FAIL);}
    hr=equal==size?S_OK:E_FAIL;s.copy_success=equal==size;return hr;
}
