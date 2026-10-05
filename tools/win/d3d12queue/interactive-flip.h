// SPDX-License-Identifier: MIT
// Fullscreen variant of the interactive client (build.ps1 -Flip). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then creates a window that covers the whole output - borderless,
// or an exclusive-fullscreen chain with -FlipFullscreen - and a flip-model swap chain of -FlipBuffers buffers at
// the output's own size in B8G8R8A8_UNORM, and presents -FlipFrames frames, each cleared to its own exact
// colour. After every frame it compares a probe of the back buffer it is about to present, and at the first and
// the last frame every texel of it. It then reports DXGI's present statistics.
//
// This is the M15.14 client: the case in which a buffer the application owns may be scanned out instead of
// composed. The client proves what the API returned and what the back buffer held before Present. Whether the
// display hardware actually read that buffer is not visible from here at all: that witness is the kernel
// driver's scan-out counters (bc250kmd_cli log summary) around the run, and the operator's screenshot. Nothing
// in this client's receipt should be read as a claim about DirectFlip.
//
// It opens a fullscreen window and takes the foreground. Run it on the lab, never unannounced elsewhere.

#ifndef INTERACTIVE_FLIP_BUFFERS
#define INTERACTIVE_FLIP_BUFFERS 3
#endif
#ifndef INTERACTIVE_FLIP_FRAMES
// 600 frames, ten seconds at 60 Hz. The build default used to be 120, about two seconds, and an ETW
// session on this lab has been seen to lead by 2 to 7.6 s with no events of any process (sessions
// 397-398), so a capture around a two-second client could legitimately contain no present at all.
#define INTERACTIVE_FLIP_FRAMES 600
#endif
#define INTERACTIVE_FLIP_MAX_BUFFERS 3

// Both counts are read from the environment as well, so one binary serves a 2-buffer arm, a 3-buffer arm
// and a longer ETW arm: comparing two chain depths must not mean comparing two executable hashes. A value
// out of range, or text that is not a number, leaves the build default in place.
inline UINT flip_count_from(const char* variable,UINT fallback,UINT low,UINT high){
    char text[16]{};
    const DWORD n=GetEnvironmentVariableA(variable,text,sizeof(text));
    if(!n || n>=sizeof(text))return fallback;
    UINT value=0;
    for(const char* c=text;*c;++c){
        if(*c<'0' || *c>'9')return fallback;
        value=value*10u+UINT(*c-'0');
        if(value>high)return fallback;
    }
    return value>=low?value:fallback;
}

inline LRESULT CALLBACK flip_window_proc(HWND window,UINT message,WPARAM w,LPARAM l){
    // The chain owns what is on screen; nothing is painted from the message loop. WM_CLOSE is ignored: the
    // session ends on its own command, its deadline or abort.request, never because a stray click closed it.
    if(message==WM_CLOSE)return 0;
    return DefWindowProcW(window,message,w,l);
}

// Whatever happens between the window appearing and this function returning, the operator's screen has to
// come back. An early return on an API refusal used to leave a black fullscreen window - and, in the
// -FlipFullscreen build, the display in exclusive fullscreen - until the trial's Stop-Process. The guard
// owns the window class, the window and the fullscreen state from the moment each is taken, so every
// return path undoes them, in the right order, including the ones that return before the frame loop.
struct FlipWindow {
    Session* session=nullptr;
    IDXGISwapChain3* chain=nullptr;      // not owned: the Session's own reference outlives this guard
    HWND window=nullptr;
    const wchar_t* window_class=nullptr;
    HINSTANCE instance=nullptr;
    bool fullscreen=false;
    ~FlipWindow(){
        if(chain && fullscreen && session)
            session->event("after","SetFullscreenState FALSE",chain->SetFullscreenState(FALSE,nullptr));
        if(window){ShowWindow(window,SW_HIDE);DestroyWindow(window);}
        if(window_class && instance)UnregisterClassW(window_class,instance);
    }
};

inline HRESULT flip(Session& s){
    if(!s.queue || !s.factory || s.pending || s.copy_success)return E_UNEXPECTED;
    const UINT buffers=flip_count_from("AMDGPU_WDDM_D3D12_FLIP_BUFFERS",INTERACTIVE_FLIP_BUFFERS,2,INTERACTIVE_FLIP_MAX_BUFFERS);
    const UINT frames=flip_count_from("AMDGPU_WDDM_D3D12_FLIP_FRAMES",INTERACTIVE_FLIP_FRAMES,1,20000);
    constexpr DXGI_FORMAT format=DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D12Device* device=s.device.Get();
    HRESULT hr=S_OK;
    {char label[112]{};sprintf_s(label,"Flip plan: %u buffers, %u frames, present interval 1",buffers,frames);s.event("after",label);}

    // The output's desktop rectangle is the only size at which a scan-out candidate can match the POST mode, so
    // the client never picks a size of its own.
    ComPtr<IDXGIOutput> output;
    hr=s.api("EnumOutputs 0",[&]{return s.adapter->EnumOutputs(0,&output);});if(FAILED(hr))return hr;
    DXGI_OUTPUT_DESC output_desc{};
    hr=s.api("Output GetDesc",[&]{return output->GetDesc(&output_desc);});if(FAILED(hr))return hr;
    const LONG left=output_desc.DesktopCoordinates.left,top=output_desc.DesktopCoordinates.top;
    const UINT width=static_cast<UINT>(output_desc.DesktopCoordinates.right-left);
    const UINT height=static_cast<UINT>(output_desc.DesktopCoordinates.bottom-top);
    {char label[128]{};sprintf_s(label,"Output desktop %ld,%ld %ux%u rotation %d attached %d",
        left,top,width,height,static_cast<int>(output_desc.Rotation),output_desc.AttachedToDesktop?1:0);
     s.event("after",label,width && height && output_desc.AttachedToDesktop?S_OK:E_UNEXPECTED);}
    if(!width || !height || !output_desc.AttachedToDesktop)return E_UNEXPECTED;

    WNDCLASSEXW window_class{};window_class.cbSize=sizeof(window_class);window_class.lpfnWndProc=flip_window_proc;
    window_class.hInstance=GetModuleHandleW(nullptr);window_class.lpszClassName=L"amdgpu_wddm_d3d12_queue_flip";
    window_class.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    FlipWindow guard;guard.session=&s;
    if(!RegisterClassExW(&window_class)){hr=HRESULT_FROM_WIN32(GetLastError());s.event("after","RegisterClassEx",hr);return hr;}
    guard.window_class=window_class.lpszClassName;guard.instance=window_class.hInstance;
    // WS_POPUP with no border, exactly the output's rectangle: the borderless-fullscreen shape a game uses. No
    // WS_EX_TOPMOST, because a window that is merely always on top is not what makes a flip eligible, and a
    // topmost window would also outlive a hung session on the operator's screen.
    const HWND window=CreateWindowExW(0,window_class.lpszClassName,L"amdgpu-wddm D3D12 fullscreen flip",
        WS_POPUP,left,top,static_cast<int>(width),static_cast<int>(height),nullptr,nullptr,window_class.hInstance,nullptr);
    if(!window){hr=HRESULT_FROM_WIN32(GetLastError());s.event("after","CreateWindowEx",hr);return hr;}
    guard.window=window;
    {char label[80]{};sprintf_s(label,"CreateWindowEx WS_POPUP %ux%u covering the output",width,height);s.event("after",label);}
    ShowWindow(window,SW_SHOW);
    s.event("after","SetForegroundWindow",SetForegroundWindow(window)?S_OK:S_FALSE);
    {MSG message{};while(PeekMessageW(&message,window,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}

    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=format;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=buffers;
    // SCALING_NONE and no multisampling: the buffer is the size of the output and needs no stretch on the way
    // to the plane, which is what a scanned-out buffer must be.
    desc.Scaling=DXGI_SCALING_NONE;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
#ifdef INTERACTIVE_FLIP_FULLSCREEN
    desc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
#endif
    ComPtr<IDXGISwapChain1> chain1;ComPtr<IDXGISwapChain3> chain;
    {char label[96]{};sprintf_s(label,"CreateSwapChainForHwnd FLIP_DISCARD B8G8R8A8 %u buffers %ux%u",buffers,width,height);
     hr=s.api(label,[&]{return s.factory->CreateSwapChainForHwnd(s.queue.Get(),window,&desc,nullptr,nullptr,&chain1);});}
    if(FAILED(hr)){s.event("after","GetDeviceRemovedReason after swap chain",device->GetDeviceRemovedReason());return hr;}
    hr=s.api("MakeWindowAssociation",[&]{return s.factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);});if(FAILED(hr))return hr;
    hr=s.api("QueryInterface IDXGISwapChain3",[&]{return chain1.As(&chain);});if(FAILED(hr))return hr;
    s.extra[0]=chain;
    guard.chain=chain.Get();
#ifdef INTERACTIVE_FLIP_FULLSCREEN
    // Exclusive fullscreen on this output. A refusal is traced and the run goes on borderless: the mode list is
    // not this increment's business, and a borderless chain is just as eligible for a flip.
    const HRESULT fullscreen=s.api("SetFullscreenState TRUE",[&]{return chain->SetFullscreenState(TRUE,output.Get());});
    if(SUCCEEDED(fullscreen)){
        guard.fullscreen=true;      // from here the guard owns the restore, on every return path
        hr=s.api("ResizeBuffers after SetFullscreenState",[&]{return chain->ResizeBuffers(buffers,width,height,format,desc.Flags);});
        if(FAILED(hr))return hr;
    }
#endif
    {BOOL state=FALSE;ComPtr<IDXGIOutput> on;
     const HRESULT got=chain->GetFullscreenState(&state,&on);
     char label[64]{};sprintf_s(label,"GetFullscreenState %s",state?"exclusive":"windowed");s.event("after",label,got);}

    D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;views.NumDescriptors=buffers;
    ComPtr<ID3D12DescriptorHeap> view_heap;
    hr=s.api("CreateDescriptorHeap RTV",[&]{return device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&view_heap));});if(FAILED(hr))return hr;
    s.extra[1]=view_heap;
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> back[INTERACTIVE_FLIP_MAX_BUFFERS];
    for(UINT i=0;i<buffers;++i){
        char label[32]{};sprintf_s(label,"GetBuffer %u",i);
        hr=s.api(label,[&]{return chain->GetBuffer(i,IID_PPV_ARGS(&back[i]));});if(FAILED(hr))return hr;
        D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{i}*increment;
        device->CreateRenderTargetView(back[i].Get(),nullptr,view);
        const D3D12_RESOURCE_DESC d=back[i]->GetDesc();
        sprintf_s(label,"Buffer %u %llux%u format %d",i,d.Width,d.Height,static_cast<int>(d.Format));
        s.event("after",label,d.Width==width && d.Height==height && d.Format==format?S_OK:E_UNEXPECTED);
        if(d.Width!=width || d.Height!=height || d.Format!=format)return E_UNEXPECTED;
    }

    hr=s.api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));});if(FAILED(hr))return hr;
    hr=s.api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));});if(FAILED(hr))return hr;
    hr=s.api("Close CommandList initial",[&]{return s.list->Close();});if(FAILED(hr))return hr;
    hr=s.api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence));});if(FAILED(hr))return hr;
    HANDLE completed_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completed_event)return HRESULT_FROM_WIN32(GetLastError());
    UINT64 fence_value=0;
    const auto retire=[&]()->HRESULT{
        const UINT64 value=++fence_value;
        HRESULT wait_hr=s.queue->Signal(s.fence.Get(),value);
        if(SUCCEEDED(wait_hr))wait_hr=s.fence->SetEventOnCompletion(value,completed_event);
        if(FAILED(wait_hr)){s.event("after","Signal or SetEventOnCompletion",wait_hr);return wait_hr;}
        const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD wait=WAIT_TIMEOUT;
        while(GetTickCount64()<end && !s.abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(completed_event,2);
        const UINT64 completed=s.fence->GetCompletedValue();
        if(completed==UINT64_MAX){s.event("after","WaitForFence bounded",DXGI_ERROR_DEVICE_REMOVED);return DXGI_ERROR_DEVICE_REMOVED;}
        if(completed<value){s.event("after","WaitForFence bounded",HRESULT_FROM_WIN32(WAIT_TIMEOUT));return HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        s.pending=false;return S_OK;
    };

    // Frame content: every channel is 0.0 or 1.0, so the clear has no rounding tie and the BGRA8 word of a
    // frame is fully determined. The eight combinations cycle, so a frame that is still the previous one on
    // screen, or a buffer presented twice, differs in at least one channel from what is expected.
    const auto colour_of=[](UINT frame,UINT channel)->FLOAT{return (frame>>channel)&1u?1.0f:0.0f;};
    const auto word_of=[&](UINT frame)->UINT32{
        // B8G8R8A8 in memory, read as one little-endian word: B in the low byte, A in the high one.
        const UINT32 b=colour_of(frame,2)!=0.0f?0xffu:0u,g=colour_of(frame,1)!=0.0f?0xffu:0u,r=colour_of(frame,0)!=0.0f?0xffu:0u;
        return b|(g<<8)|(r<<16)|0xff000000u;
    };

    // One READBACK buffer for a whole back buffer. At 1920x1200 that is about 9 MB; the probe reads a window of
    // it on most frames and the whole image on the first and the last.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT rows=0;UINT64 row_bytes=0,total=0;
    {const D3D12_RESOURCE_DESC d=back[0]->GetDesc();
     device->GetCopyableFootprints(&d,0,1,0,&placed,&rows,&row_bytes,&total);
     const bool good=total && rows==height && row_bytes==UINT64{width}*4 && placed.Footprint.RowPitch>=width*4;
     char label[160]{};sprintf_s(label,"GetCopyableFootprints %ux%u row pitch %u rows %u row bytes %llu total %llu",
        width,height,placed.Footprint.RowPitch,rows,row_bytes,total);
     s.event("after",label,good?S_OK:E_UNEXPECTED);
     if(!good){CloseHandle(completed_event);return E_UNEXPECTED;}}
    D3D12_HEAP_PROPERTIES readback_heap{};readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};readback_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;readback_desc.Width=total;
    readback_desc.Height=1;readback_desc.DepthOrArraySize=1;readback_desc.MipLevels=1;readback_desc.SampleDesc.Count=1;
    readback_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&readback_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});
    if(FAILED(hr)){CloseHandle(completed_event);return hr;}

    UINT presented=0,refused=0,mismatched=0,occluded=0;
    HRESULT first_failure=S_OK;
    UINT indices_seen=0;  // bit per back-buffer index the chain handed out
    const ULONGLONG render_start=GetTickCount64();
    const auto frame=[&](UINT index)->HRESULT{
        const UINT current=chain->GetCurrentBackBufferIndex();
        if(current>=buffers)return E_UNEXPECTED;
        indices_seen|=1u<<current;
        HRESULT result=s.allocator->Reset();
        if(SUCCEEDED(result))result=s.list->Reset(s.allocator.Get(),nullptr);
        if(FAILED(result)){s.event("after","Reset allocator or list",result);return result;}
        D3D12_CPU_DESCRIPTOR_HANDLE view=view_heap->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{current}*increment;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=back[current].Get();
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        s.list->ResourceBarrier(1,&barrier);
        s.list->OMSetRenderTargets(1,&view,FALSE,nullptr);
        const FLOAT colour[4]{colour_of(index,0),colour_of(index,1),colour_of(index,2),1.0f};
        s.list->ClearRenderTargetView(view,colour,0,nullptr);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        s.list->ResourceBarrier(1,&barrier);
        // The whole image on the first and the last frame, a 64-row band elsewhere, its top moving with the
        // frame so a stale readback cannot pass as a fresh one.
        const bool whole=index==0 || index+1==frames;
        const UINT band=(std::min)(64u,height),band_top=whole?0u:(index*37u)%(height-(std::min)(band,height-1u));
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=back[current].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=s.readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=placed;
        const D3D12_BOX box{0u,band_top,0u,width,band_top+band,1u};
        if(whole)s.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        else s.list->CopyTextureRegion(&to,0,band_top,0,&from,&box);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
        s.list->ResourceBarrier(1,&barrier);
        result=s.list->Close();
        if(FAILED(result)){s.event("after","Close CommandList",result);return result;}
        ID3D12CommandList* commands[]={s.list.Get()};s.pending=true;
        s.queue->ExecuteCommandLists(1,commands);
        result=retire();if(FAILED(result))return result;
        {   // What this buffer holds before it is presented.
            const D3D12_RANGE whole_range{0,static_cast<SIZE_T>(total)};void* data=nullptr;
            result=s.readback->Map(0,&whole_range,&data);
            if(FAILED(result) || !data){s.event("after","Map READBACK",FAILED(result)?result:E_POINTER);return FAILED(result)?result:E_POINTER;}
            const auto* bytes=static_cast<const BYTE*>(data)+placed.Offset;
            const UINT32 expected=word_of(index);
            const UINT from_row=whole?0u:band_top,to_row=whole?height:band_top+band;
            UINT64 good=0,texels=0;UINT32 found=expected+1u;
            for(UINT y=from_row;y<to_row;++y)for(UINT x=0;x<width;++x){
                UINT32 w=0;memcpy(&w,bytes+SIZE_T{y}*placed.Footprint.RowPitch+SIZE_T{x}*4,sizeof(w));
                if(!texels)found=w;
                ++texels;good+=w==expected;
            }
            const D3D12_RANGE none{0,0};s.readback->Unmap(0,&none);
            if(good!=texels){
                ++mismatched;
                char label[176]{};sprintf_s(label,"Frame %u buffer %u rows %u..%u first word %08x expected %08x exact %llu of %llu",
                    index+1,current,from_row,to_row,found,expected,good,texels);
                s.event("after",label,E_FAIL);
                if(SUCCEEDED(first_failure))first_failure=E_FAIL;
            }else if(whole){
                char label[144]{};sprintf_s(label,"Frame %u buffer %u every texel of %ux%u is %08x",index+1,current,width,height,expected);
                s.event("after",label);
            }
        }
        s.pending=true;
        result=chain->Present(1,0);
        if(result==S_OK)++presented;
        else{
            ++refused;
            if(result==DXGI_STATUS_OCCLUDED)++occluded;
            char label[64]{};sprintf_s(label,"Frame %u Present interval 1",index+1);s.event("after",label,result);
            if(SUCCEEDED(first_failure) && FAILED(result))first_failure=result;
            if(FAILED(result))return result;
        }
        const HRESULT removed=device->GetDeviceRemovedReason();
        if(FAILED(removed)){s.event("after","GetDeviceRemovedReason after Present",removed);return removed;}
        result=retire();if(FAILED(result))return result;
        MSG message{};while(PeekMessageW(&message,window,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        return S_OK;
    };

    DXGI_FRAME_STATISTICS before_stats{};
    const HRESULT before_hr=chain->GetFrameStatistics(&before_stats);
    UINT before_count=0;const HRESULT before_last=chain->GetLastPresentCount(&before_count);
    {char label[176]{};sprintf_s(label,"Frame statistics before: present %u present refresh %u sync refresh %u last present count %u",
        before_stats.PresentCount,before_stats.PresentRefreshCount,before_stats.SyncRefreshCount,before_count);
     s.event("after",label,SUCCEEDED(before_hr) && SUCCEEDED(before_last)?S_OK:(FAILED(before_hr)?before_hr:before_last));}

    for(UINT index=0;index<frames && SUCCEEDED(hr);++index){
        if(s.abort_requested()){hr=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
        // The session's own deadline ends the run; 2 s are left for the statistics and the teardown.
        if(GetTickCount64()+2000>=s.deadline){
            char label[64]{};sprintf_s(label,"Frames cut short at %u of %u by the deadline",index,frames);s.event("after",label);
            break;
        }
        hr=frame(index);
    }
    const ULONGLONG render_ms=GetTickCount64()-render_start;

    DXGI_FRAME_STATISTICS after_stats{};
    const HRESULT after_hr=chain->GetFrameStatistics(&after_stats);
    UINT after_count=0;const HRESULT after_last=chain->GetLastPresentCount(&after_count);
    {char label[208]{};sprintf_s(label,"Frame statistics after: present %u present refresh %u sync refresh %u last present count %u",
        after_stats.PresentCount,after_stats.PresentRefreshCount,after_stats.SyncRefreshCount,after_count);
     s.event("after",label,SUCCEEDED(after_hr) && SUCCEEDED(after_last)?S_OK:(FAILED(after_hr)?after_hr:after_last));}
    {   // The deltas are what a reader compares with the kernel driver's flip counters over the same window.
        char label[240]{};
        sprintf_s(label,"Presented %u refused %u occluded %u mismatched %u in %llu ms; present delta %u refresh delta %u sync delta %u; back buffer indices %x of %u",
            presented,refused,occluded,mismatched,render_ms,
            after_stats.PresentCount-before_stats.PresentCount,
            after_stats.PresentRefreshCount-before_stats.PresentRefreshCount,
            after_stats.SyncRefreshCount-before_stats.SyncRefreshCount,indices_seen,buffers);
        s.event("after",label);
    }
    {char label[144]{};sprintf_s(label,"Render window %llu ms to %llu ms of the session (%llu ms)",
        render_start-s.start,GetTickCount64()-s.start,render_ms);s.event("after",label);}
    {char label[112]{};sprintf_s(label,"Frames per second %llu.%02llu over %llu ms",
        render_ms?presented*1000ull/render_ms:0ull,render_ms?presented*100000ull/render_ms%100ull:0ull,render_ms);
     s.event("after",label);}

    CloseHandle(completed_event);
    const HRESULT removed=device->GetDeviceRemovedReason();
    s.event("after","GetDeviceRemovedReason at the end",removed);
    if(SUCCEEDED(hr) && FAILED(removed))hr=removed;
    if(SUCCEEDED(hr) && FAILED(first_failure))hr=first_failure;
    // Success is every frame presented with S_OK and exact, and no frame lost to the deadline. An occluded
    // present is not a failure of this driver path, but it is not a presented frame either, so it fails the run
    // and says so in the trace: a covered fullscreen window is a lab condition to fix, not a result.
    if(SUCCEEDED(hr) && (presented!=frames || mismatched || refused))hr=E_FAIL;
    {char label[144]{};sprintf_s(label,"Fullscreen flip: %u of %u frames presented exact at %ux%u in %u buffers",
        presented,frames,width,height,buffers);s.event("after",label,hr);}
    s.copy_success=hr==S_OK;return hr;   // the guard restores the desktop and takes the window down
}
