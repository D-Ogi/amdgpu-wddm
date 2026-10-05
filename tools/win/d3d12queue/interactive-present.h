// SPDX-License-Identifier: MIT
// Present variant of the interactive client (build.ps1 -Present). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then creates a window and a flip-model swap chain on the queue,
// clears each of the two back buffers to its own colour, copies it to a READBACK buffer and presents it, then
// resizes the chain and does the same once more at the new size. Commands and receipts keep their form;
// success means the three Present calls and the device status returned S_OK and every texel of the three
// copies is the cleared colour.
//
// The client proves what the API returned and what the back buffer held before Present, not what reached the
// screen: that witness is a screenshot taken by the operator. Every step is traced with its HRESULT so a driver
// refusal can be placed between DDI records.

// The back buffer format (build.ps1 -PresentFormat). A 10-bit chain is composed onto the desktop like an 8-bit
// one; the monitor's depth does not enter.
#ifdef INTERACTIVE_PRESENT_RGB10A2
inline constexpr DXGI_FORMAT present_format=DXGI_FORMAT_R10G10B10A2_UNORM;
inline constexpr const char* present_format_name="R10G10B10A2";
#else
inline constexpr DXGI_FORMAT present_format=DXGI_FORMAT_B8G8R8A8_UNORM;
inline constexpr const char* present_format_name="B8G8R8A8";
#endif

inline LRESULT CALLBACK present_window_proc(HWND window,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(window,message,w,l);}

inline void describe(Session& s,const char* name,ID3D12Resource* resource){
    const D3D12_RESOURCE_DESC d=resource->GetDesc();char label[160]{};
    sprintf_s(label,"%s dimension %d width %llu height %u depth %u mips %u format %d samples %u layout %d flags %x alignment %llu",
        name,static_cast<int>(d.Dimension),d.Width,d.Height,static_cast<unsigned>(d.DepthOrArraySize),static_cast<unsigned>(d.MipLevels),
        static_cast<int>(d.Format),d.SampleDesc.Count,static_cast<int>(d.Layout),static_cast<unsigned>(d.Flags),d.Alignment);
    s.event("after",label);
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    const HRESULT hr=resource->GetHeapProperties(&heap,&flags);
    sprintf_s(label,"%s heap type %d page %d pool %d flags %x",name,static_cast<int>(heap.Type),static_cast<int>(heap.CPUPageProperty),
        static_cast<int>(heap.MemoryPoolPreference),static_cast<unsigned>(flags));
    s.event("after",label,hr);
}

inline HRESULT present(Session& s){
    if(!s.queue || !s.factory || s.pending || s.copy_success)return E_UNEXPECTED;
    constexpr UINT side=256,buffers=2;
    ID3D12Device* device=s.device.Get();
    HRESULT hr=S_OK;

    WNDCLASSEXW window_class{};window_class.cbSize=sizeof(window_class);window_class.lpfnWndProc=present_window_proc;
    window_class.hInstance=GetModuleHandleW(nullptr);window_class.lpszClassName=L"amdgpu_wddm_d3d12_queue_present";
    if(!RegisterClassExW(&window_class)){hr=HRESULT_FROM_WIN32(GetLastError());s.event("after","RegisterClassEx",hr);return hr;}
    RECT frame{0,0,static_cast<LONG>(side),static_cast<LONG>(side)};
    AdjustWindowRect(&frame,WS_OVERLAPPEDWINDOW,FALSE);
    const HWND window=CreateWindowExW(0,window_class.lpszClassName,L"amdgpu-wddm D3D12 present",WS_OVERLAPPEDWINDOW,64,64,
        frame.right-frame.left,frame.bottom-frame.top,nullptr,nullptr,window_class.hInstance,nullptr);
    if(!window){hr=HRESULT_FROM_WIN32(GetLastError());s.event("after","CreateWindowEx",hr);return hr;}
    s.event("after","CreateWindowEx 256x256");
    ShowWindow(window,SW_SHOWNOACTIVATE);s.event("after","ShowWindow without activation");

    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=side;desc.Height=side;desc.Format=present_format;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=buffers;
    desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> chain1;ComPtr<IDXGISwapChain3> chain;
    char create_label[64]{};sprintf_s(create_label,"CreateSwapChainForHwnd FLIP_DISCARD %s 2 buffers",present_format_name);
    hr=s.api(create_label,[&]{return s.factory->CreateSwapChainForHwnd(s.queue.Get(),window,&desc,nullptr,nullptr,&chain1);});
    if(FAILED(hr)){s.event("after","GetDeviceRemovedReason after swap chain",device->GetDeviceRemovedReason());return hr;}
    hr=s.api("MakeWindowAssociation",[&]{return s.factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);});if(FAILED(hr))return hr;
    hr=s.api("QueryInterface IDXGISwapChain3",[&]{return chain1.As(&chain);});if(FAILED(hr))return hr;
    s.extra[0]=chain;

    D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=buffers;
    ComPtr<ID3D12DescriptorHeap> view_heap;
    hr=s.api("CreateDescriptorHeap RTV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&view_heap));});if(FAILED(hr))return hr;
    s.extra[1]=view_heap;
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> back[buffers];
    for(UINT i=0;i<buffers;++i){
        char label[32]{};sprintf_s(label,"GetBuffer %u",i);
        hr=s.api(label,[&]{return chain->GetBuffer(i,IID_PPV_ARGS(&back[i]));});if(FAILED(hr))return hr;
        sprintf_s(label,"Buffer %u",i);describe(s,label,back[i].Get());
        D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{i}*increment;
        device->CreateRenderTargetView(back[i].Get(),nullptr,view);s.event("after","CreateRenderTargetView");
    }

    hr=s.api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));});if(FAILED(hr))return hr;
    hr=s.api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));});if(FAILED(hr))return hr;
    hr=s.api("Close CommandList initial",[&]{return s.list->Close();});if(FAILED(hr))return hr;
    hr=s.api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence));});if(FAILED(hr))return hr;
    HANDLE completed_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completed_event)return HRESULT_FROM_WIN32(GetLastError());
    const auto retire=[&](UINT64 value)->HRESULT{
        HRESULT wait_hr=s.api("Queue Signal",[&]{return s.queue->Signal(s.fence.Get(),value);});if(FAILED(wait_hr))return wait_hr;
        wait_hr=s.api("SetEventOnCompletion",[&]{return s.fence->SetEventOnCompletion(value,completed_event);});if(FAILED(wait_hr))return wait_hr;
        const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD wait=WAIT_TIMEOUT;
        while(GetTickCount64()<end && !s.abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(completed_event,20);
        s.event("after","WaitForFence bounded",wait==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        const UINT64 completed=s.fence->GetCompletedValue();
        if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(completed<value)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        s.pending=false;return S_OK;
    };

    // Frame 1 is opaque orange, frame 2 opaque teal, frame 3 (after the resize) opaque magenta: R, G, B, A as
    // the clear takes them, and the same in B8G8R8A8 memory read as one little-endian word. 0.5 is 127.5 of 255:
    // either neighbour is a correct conversion, so each channel at 0.5 admits 0x7f and 0x80 (half_mask), and
    // the word found is traced.
    constexpr UINT frames=3;
#ifdef INTERACTIVE_PRESENT_RGB10A2
    // The 10-bit chain: red in the low ten bits, alpha in the top two. Orange-ish, teal-ish and magenta, with
    // 0.4 and 0.6 of 1023 at 409.2 and 613.8, so no rounding tie decides a word and each frame differs from the
    // others in every colour channel it sets.
    const FLOAT colours[frames][4]{{1.0f,0.4f,0.0f,1.0f},{0.0f,0.6f,0.6f,1.0f},{1.0f,0.0f,1.0f,1.0f}};
    const UINT32 exact[frames]{0xc00667ffu,0xe6699800u,0xfff003ffu};
    const auto admitted=[&](UINT frame,UINT32 word){return word==exact[frame];};
#else
    const FLOAT colours[frames][4]{{1.0f,0.5f,0.0f,1.0f},{0.0f,0.5f,0.5f,1.0f},{1.0f,0.0f,1.0f,1.0f}};
    const UINT32 exact[frames]{0xffff8000u,0xff008080u,0xffff00ffu};
    const UINT32 half_mask[frames]{0x0000ff00u,0x0000ffffu,0u};
    const auto admitted=[&](UINT frame,UINT32 word){
        if((word&~half_mask[frame])!=(exact[frame]&~half_mask[frame]))return false;
        for(UINT shift=0;shift<32;shift+=8){
            if(!((half_mask[frame]>>shift)&0xffu))continue;
            const UINT32 channel=(word>>shift)&0xffu;if(channel!=0x7fu && channel!=0x80u)return false;
        }
        return true;
    };
#endif
    const ULONGLONG holds[frames]{5000,2000,3000};
    // One READBACK buffer for a whole back buffer of the first size, used by every frame in turn.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT64 total=0;
    const auto footprint=[&](ID3D12Resource* texture,UINT edge)->HRESULT{
        const D3D12_RESOURCE_DESC d=texture->GetDesc();UINT rows=0;UINT64 row_bytes=0;placed={};total=0;
        device->GetCopyableFootprints(&d,0,1,0,&placed,&rows,&row_bytes,&total);
        const bool good=total && rows==edge && row_bytes==UINT64{edge}*4 && placed.Footprint.Width==edge &&
            placed.Footprint.Height==edge && placed.Footprint.RowPitch>=edge*4;
        char label[144]{};sprintf_s(label,"GetCopyableFootprints %ux%u offset %llu row pitch %u rows %u row bytes %llu total %llu",
            edge,edge,placed.Offset,placed.Footprint.RowPitch,rows,row_bytes,total);
        s.event("after",label,good?S_OK:E_UNEXPECTED);
        return good?S_OK:E_UNEXPECTED;
    };
    hr=footprint(back[0].Get(),side);if(FAILED(hr))return hr;
    const UINT64 readback_bytes=total;
    D3D12_HEAP_PROPERTIES readback_heap{};readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};readback_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;readback_desc.Width=total;
    readback_desc.Height=1;readback_desc.DepthOrArraySize=1;readback_desc.MipLevels=1;readback_desc.SampleDesc.Count=1;
    readback_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});if(FAILED(hr))return hr;
    HRESULT compared[frames]{E_PENDING,E_PENDING,E_PENDING};
    // Each Present status is kept as returned: a success code other than S_OK (occluded, for one) is traced and
    // the frame is still retired, but it does not count as a presented frame.
    HRESULT presented[frames]{E_PENDING,E_PENDING,E_PENDING};
    // One frame on the chain's current back buffer, whose edge is given: clear, copy, compare, Present, hold.
    const auto draw=[&](UINT frame_index,UINT edge)->HRESULT{
        const UINT current=chain->GetCurrentBackBufferIndex();
        {char label[48]{};sprintf_s(label,"Frame %u back buffer index %u",frame_index+1,current);s.event("after",label,current<buffers?S_OK:E_UNEXPECTED);}
        if(current>=buffers)return E_UNEXPECTED;
        HRESULT result=S_OK;
        if(frame_index){result=s.api("Reset CommandAllocator",[&]{return s.allocator->Reset();});if(FAILED(result))return result;}
        result=s.api("Reset CommandList",[&]{return s.list->Reset(s.allocator.Get(),nullptr);});if(FAILED(result))return result;
        D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{current}*increment;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=back[current].Get();
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        s.list->ResourceBarrier(1,&barrier);s.event("after","ResourceBarrier PRESENT to RENDER_TARGET");
        s.list->OMSetRenderTargets(1,&view,FALSE,nullptr);s.event("after","OMSetRenderTargets");
        s.list->ClearRenderTargetView(view,colours[frame_index],0,nullptr);s.event("after","ClearRenderTargetView");
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        s.list->ResourceBarrier(1,&barrier);s.event("after","ResourceBarrier RENDER_TARGET to COPY_SOURCE");
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=back[current].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=s.readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=placed;
        s.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);s.event("after","CopyTextureRegion BACK BUFFER to READBACK");
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
        s.list->ResourceBarrier(1,&barrier);s.event("after","ResourceBarrier COPY_SOURCE to PRESENT");
        result=s.api("Close CommandList",[&]{return s.list->Close();});if(FAILED(result))return result;
        ID3D12CommandList* commands[]={s.list.Get()};s.pending=true;
        s.event("before","ExecuteCommandLists");s.queue->ExecuteCommandLists(1,commands);s.event("after","ExecuteCommandLists");
        result=retire(frame_index*2+1);if(FAILED(result))return result;
        {
            // What the back buffer holds before it is presented: every texel, against the cleared colour.
            const D3D12_RANGE whole{0,static_cast<SIZE_T>(total)};void* data=nullptr;
            result=s.api("Map READBACK",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(result))return result;
            if(!data)return E_POINTER;
            const auto* bytes=static_cast<const BYTE*>(data)+placed.Offset;
            UINT32 first=0;memcpy(&first,bytes,sizeof(first));
            UINT64 same=0,good=0;
            for(UINT y=0;y<edge;++y)for(UINT x=0;x<edge;++x){
                UINT32 word=0;memcpy(&word,bytes+SIZE_T{y}*placed.Footprint.RowPitch+SIZE_T{x}*4,sizeof(word));
                same+=word==first;good+=admitted(frame_index,word);
            }
            const D3D12_RANGE none{0,0};s.readback->Unmap(0,&none);
            compared[frame_index]=same==UINT64{edge}*edge && good==UINT64{edge}*edge?S_OK:E_FAIL;
            char found[112]{};sprintf_s(found,"Frame %u back buffer first word %08x expected %08x equal to first %llu admitted %llu of %u",
                frame_index+1,first,exact[frame_index],same,good,edge*edge);
            s.event("after",found,compared[frame_index]);
        }
        char label[32]{};sprintf_s(label,"Present %u interval 1",frame_index+1);
        s.pending=true;
        result=s.api(label,[&]{return chain->Present(1,0);});presented[frame_index]=result;
        const HRESULT removed=device->GetDeviceRemovedReason();
        s.event("after","GetDeviceRemovedReason after Present",removed);
        if(SUCCEEDED(result) && FAILED(removed))result=removed;
        if(FAILED(result))return result;
        result=retire(frame_index*2+2);if(FAILED(result))return result;
        // The operator's screenshot needs the frame to stay up: captures come several seconds apart.
        const ULONGLONG hold=(std::min)(s.deadline,GetTickCount64()+holds[frame_index]);
        while(GetTickCount64()<hold && !s.abort_requested()){MSG message{};while(PeekMessageW(&message,window,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(20);}
        sprintf_s(label,"Frame %u held",frame_index+1);s.event("after",label);
        return S_OK;
    };
    for(UINT frame_index=0;frame_index<buffers && SUCCEEDED(hr);++frame_index)hr=draw(frame_index,side);
    if(FAILED(hr)){CloseHandle(completed_event);return hr;}

    constexpr UINT resized=128;
    for(auto& buffer:back)buffer.Reset();
    hr=s.api("ResizeBuffers 128x128",[&]{return chain->ResizeBuffers(buffers,resized,resized,present_format,0);});
    for(UINT i=0;i<buffers && SUCCEEDED(hr);++i){
        char label[48]{};sprintf_s(label,"GetBuffer %u after resize",i);
        hr=s.api(label,[&]{return chain->GetBuffer(i,IID_PPV_ARGS(&back[i]));});if(FAILED(hr))break;
        sprintf_s(label,"Resized buffer %u",i);describe(s,label,back[i].Get());
        D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{i}*increment;
        device->CreateRenderTargetView(back[i].Get(),nullptr,view);s.event("after","CreateRenderTargetView after resize");
    }
    HRESULT removed=device->GetDeviceRemovedReason();
    s.event("after","GetDeviceRemovedReason after resize",removed);
    if(SUCCEEDED(hr) && FAILED(removed))hr=removed;
    // The third frame, at the new size, in the window the first two were shown in.
    if(SUCCEEDED(hr))hr=footprint(back[0].Get(),resized);
    if(SUCCEEDED(hr) && total>readback_bytes)hr=E_UNEXPECTED;
    if(SUCCEEDED(hr))hr=draw(2,resized);
    CloseHandle(completed_event);
    removed=device->GetDeviceRemovedReason();
    s.event("after","GetDeviceRemovedReason after frame 3",removed);
    if(SUCCEEDED(hr) && FAILED(removed))hr=removed;
    if(SUCCEEDED(hr))for(const HRESULT status:presented)if(status!=S_OK){hr=E_FAIL;break;}
    if(SUCCEEDED(hr))for(const HRESULT status:compared)if(status!=S_OK){hr=E_FAIL;break;}
    {char label[96]{};sprintf_s(label,"Back buffer compares %08lx %08lx %08lx",static_cast<unsigned long>(compared[0]),static_cast<unsigned long>(compared[1]),static_cast<unsigned long>(compared[2]));s.event("after",label,hr);}
    {char label[96]{};sprintf_s(label,"Present statuses %08lx %08lx %08lx",static_cast<unsigned long>(presented[0]),static_cast<unsigned long>(presented[1]),static_cast<unsigned long>(presented[2]));s.event("after",label,hr);}
    s.event("after","Three frames exact and presented with S_OK, the third after the resize",hr);
    s.copy_success=hr==S_OK;return hr;
}
