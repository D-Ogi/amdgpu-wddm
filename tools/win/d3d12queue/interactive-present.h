// SPDX-License-Identifier: MIT
// Present variant of the interactive client (build.ps1 -Present). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then creates a window and a flip-model swap chain on the queue,
// clears each of the two back buffers to its own colour and presents it, then resizes the chain. Commands and
// receipts keep their form; success means both Present calls and the device status returned S_OK.
//
// The client proves what the API returned, not what reached the screen: the pixel witness is a screenshot taken
// by the operator. Every step is traced with its HRESULT so a driver refusal can be placed between DDI records.

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

    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=side;desc.Height=side;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=buffers;
    desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> chain1;ComPtr<IDXGISwapChain3> chain;
    hr=s.api("CreateSwapChainForHwnd FLIP_DISCARD B8G8R8A8 2 buffers",[&]{return s.factory->CreateSwapChainForHwnd(s.queue.Get(),window,&desc,nullptr,nullptr,&chain1);});
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

    // Frame 1 is opaque orange, frame 2 opaque teal (R, G, B, A as the clear takes them).
    const FLOAT colours[buffers][4]{{1.0f,0.5f,0.0f,1.0f},{0.0f,0.5f,0.5f,1.0f}};
    for(UINT frame_index=0;frame_index<buffers && SUCCEEDED(hr);++frame_index){
        const UINT current=chain->GetCurrentBackBufferIndex();
        {char label[48]{};sprintf_s(label,"Frame %u back buffer index %u",frame_index+1,current);s.event("after",label,current<buffers?S_OK:E_UNEXPECTED);}
        if(current>=buffers){hr=E_UNEXPECTED;break;}
        if(frame_index){hr=s.api("Reset CommandAllocator",[&]{return s.allocator->Reset();});if(FAILED(hr))break;}
        hr=s.api("Reset CommandList",[&]{return s.list->Reset(s.allocator.Get(),nullptr);});if(FAILED(hr))break;
        D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{current}*increment;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=back[current].Get();
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        s.list->ResourceBarrier(1,&barrier);s.event("after","ResourceBarrier PRESENT to RENDER_TARGET");
        s.list->OMSetRenderTargets(1,&view,FALSE,nullptr);s.event("after","OMSetRenderTargets");
        s.list->ClearRenderTargetView(view,colours[frame_index],0,nullptr);s.event("after","ClearRenderTargetView");
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
        s.list->ResourceBarrier(1,&barrier);s.event("after","ResourceBarrier RENDER_TARGET to PRESENT");
        hr=s.api("Close CommandList",[&]{return s.list->Close();});if(FAILED(hr))break;
        ID3D12CommandList* commands[]={s.list.Get()};s.pending=true;
        s.event("before","ExecuteCommandLists");s.queue->ExecuteCommandLists(1,commands);s.event("after","ExecuteCommandLists");
        hr=retire(frame_index*2+1);if(FAILED(hr))break;
        char label[32]{};sprintf_s(label,"Present %u interval 1",frame_index+1);
        s.pending=true;
        hr=s.api(label,[&]{return chain->Present(1,0);});
        const HRESULT removed=device->GetDeviceRemovedReason();
        s.event("after","GetDeviceRemovedReason after Present",removed);
        if(SUCCEEDED(hr) && FAILED(removed))hr=removed;
        if(FAILED(hr))break;
        hr=retire(frame_index*2+2);if(FAILED(hr))break;
        // The operator's screenshot needs the frame to stay up for a moment.
        const ULONGLONG hold=(std::min)(s.deadline,GetTickCount64()+1500);
        while(GetTickCount64()<hold && !s.abort_requested()){MSG message{};while(PeekMessageW(&message,window,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(20);}
        sprintf_s(label,"Frame %u held",frame_index+1);s.event("after",label);
    }
    CloseHandle(completed_event);
    if(FAILED(hr))return hr;

    for(auto& buffer:back)buffer.Reset();
    hr=s.api("ResizeBuffers 128x128",[&]{return chain->ResizeBuffers(buffers,128,128,DXGI_FORMAT_B8G8R8A8_UNORM,0);});
    if(SUCCEEDED(hr)){
        ComPtr<ID3D12Resource> resized;
        hr=s.api("GetBuffer 0 after resize",[&]{return chain->GetBuffer(0,IID_PPV_ARGS(&resized));});
        if(SUCCEEDED(hr))describe(s,"Resized buffer 0",resized.Get());
    }
    const HRESULT removed=device->GetDeviceRemovedReason();
    s.event("after","GetDeviceRemovedReason after resize",removed);
    if(SUCCEEDED(hr) && FAILED(removed))hr=removed;
    s.event("after","Two frames presented and chain resized",hr);
    s.copy_success=SUCCEEDED(hr);return hr;
}
