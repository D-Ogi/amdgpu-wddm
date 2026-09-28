// Known-content WSI control. Direct ICD loading keeps registry selection unchanged.
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <dwmapi.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

template<class T> T info(VkStructureType type) { T t{}; t.sType=type; return t; }
#define VK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { printf("FAIL %s result=%d\n",#call,r); return 2; } } while(0)
#define IP(n) auto n=reinterpret_cast<PFN_##n>(gipa(instance,#n)); if(!n) { puts("missing " #n); return 3; }
#define DP(n) auto n=reinterpret_cast<PFN_##n>(gdpa(device,#n)); if(!n) { puts("missing " #n); return 3; }
static bool pump(DWORD duration) {
    ULONGLONG end=GetTickCount64()+duration;
    do {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT) return false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(10);
    } while(GetTickCount64()<end);
    return true;
}
static LRESULT CALLBACK windowProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    if(m==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w,m,a,b);
}
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==2 && wcscmp(argv[1],L"--help")==0) {
        puts("wsi-colour-control.exe ABSOLUTE_ICD_DLL\n60 FIFO presents, red/green/blue phases; final hold 10s. Lab interactive session only."); return 0;
    }
    if(argc!=2 || wcslen(argv[1])<4 || argv[1][1]!=L':' || (argv[1][2]!=L'\\' && argv[1][2]!=L'/')) {
        puts("FAIL absolute ICD DLL path required"); return 1;
    }
    HMODULE dll=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll) { printf("FAIL LoadLibrary error=%lu\n",GetLastError()); return 1; }
    // FARPROC is deliberately retyped to the Vulkan entry point signature.
#pragma warning(push)
#pragma warning(disable:4191)
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll,"vk_icdGetInstanceProcAddr"));
#pragma warning(pop)
    if(!gipa) return 1;
    auto negotiate=reinterpret_cast<PFN_vkNegotiateLoaderICDInterfaceVersion>(gipa(nullptr,"vk_icdNegotiateLoaderICDInterfaceVersion"));
    if(!negotiate) { puts("FAIL missing ICD negotiation"); return 1; }
    uint32_t interfaceVersion=7; VK(negotiate(&interfaceVersion));
    printf("icd_interface=%u\n",interfaceVersion);
    if(interfaceVersion<5) return 1;
    wchar_t loaded[32768]{};
    if(!GetModuleFileNameW(dll,loaded,32768)) return 1;
    printf("icd=%ls pid=%lu\n",loaded,GetCurrentProcessId());
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    if(!create) return 1;
    auto app=info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName="BC250 WSI colour control"; app.apiVersion=VK_API_VERSION_1_2;
    const char* ie[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    auto ici=info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ici.pApplicationInfo=&app; ici.enabledExtensionCount=2; ici.ppEnabledExtensionNames=ie;
    VkInstance instance{}; VK(create(&ici,nullptr,&instance));
    IP(vkEnumeratePhysicalDevices); IP(vkGetPhysicalDeviceProperties); IP(vkGetPhysicalDeviceQueueFamilyProperties);
    IP(vkCreateWin32SurfaceKHR); IP(vkGetPhysicalDeviceSurfaceSupportKHR); IP(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IP(vkGetPhysicalDeviceSurfaceFormatsKHR); IP(vkGetPhysicalDeviceSurfacePresentModesKHR); IP(vkCreateDevice); IP(vkDestroySurfaceKHR); IP(vkDestroyInstance);
    if(!SetProcessDPIAware()) { puts("FAIL DPI awareness"); return 1; }
    HINSTANCE module=GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc=windowProc; wc.hInstance=module; wc.lpszClassName=L"BC250WSIColour";
    if(!RegisterClassW(&wc)) return 1;
    RECT rect={0,0,640,480}; const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
    if(!AdjustWindowRect(&rect,style,FALSE)) return 1;
    HWND window=CreateWindowW(wc.lpszClassName,L"BC250 GPU Present colour control",style|WS_VISIBLE,
        320,240,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,module,nullptr);
    if(!window) return 1;
    DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_DONOTROUND;
    HRESULT cornerResult=DwmSetWindowAttribute(window,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
    printf("square_corners_hr=%08lx\n",static_cast<unsigned long>(cornerResult));
    if(FAILED(cornerResult)) return 1;
    auto sci=info<VkWin32SurfaceCreateInfoKHR>(VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR);
    sci.hinstance=module; sci.hwnd=window; VkSurfaceKHR surface{}; VK(vkCreateWin32SurfaceKHR(instance,&sci,nullptr,&surface));
    uint32_t count=0; VK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
    std::vector<VkPhysicalDevice> devices(count); VK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
    VkPhysicalDevice pd{}; unsigned matches=0;
    for(auto d:devices) { VkPhysicalDeviceProperties prop{}; vkGetPhysicalDeviceProperties(d,&prop);
        if(prop.vendorID==0x1002 && prop.deviceID==0x13fe) { pd=d; ++matches; printf("gpu=%s\n",prop.deviceName); }
    }
    if(matches!=1) { puts("FAIL expected exactly one BC250"); return 1; }
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,nullptr);
    std::vector<VkQueueFamilyProperties> qp(nq); vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,qp.data());
    uint32_t family=nq;
    for(uint32_t i=0;i<nq;++i) { VkBool32 supported=VK_FALSE; VK(vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surface,&supported));
        if(supported && (qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) { family=i; break; }
    }
    if(family==nq) return 1;
    float priority=1; auto qci=info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
    const char* de[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto dci=info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci; dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=de;
    VkDevice device{}; VK(vkCreateDevice(pd,&dci,nullptr,&device));
    auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,"vkGetDeviceProcAddr")); if(!gdpa) return 1;
    DP(vkCreateSwapchainKHR); DP(vkGetSwapchainImagesKHR); DP(vkAcquireNextImageKHR); DP(vkQueuePresentKHR);
    DP(vkGetDeviceQueue); DP(vkCreateCommandPool); DP(vkAllocateCommandBuffers); DP(vkResetCommandBuffer);
    DP(vkBeginCommandBuffer); DP(vkCmdPipelineBarrier); DP(vkCmdClearColorImage); DP(vkEndCommandBuffer);
    DP(vkCreateSemaphore); DP(vkCreateFence); DP(vkResetFences); DP(vkQueueSubmit); DP(vkWaitForFences);
    DP(vkDeviceWaitIdle); DP(vkDestroyFence); DP(vkDestroySemaphore); DP(vkDestroyCommandPool);
    DP(vkDestroySwapchainKHR); DP(vkDestroyDevice);
    uint32_t nm=0; VK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&nm,nullptr));
    std::vector<VkPresentModeKHR> modes(nm); VK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd,surface,&nm,modes.data()));
    bool fifo=false; for(auto mode:modes) if(mode==VK_PRESENT_MODE_FIFO_KHR) fifo=true;
    if(!fifo) { puts("FAIL FIFO unavailable"); return 1; }
    VkSurfaceCapabilitiesKHR caps{}; VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd,surface,&caps));
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return 1;
    uint32_t nf=0; VK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&nf,nullptr));
    std::vector<VkSurfaceFormatKHR> formats(nf); VK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&nf,formats.data()));
    VkSurfaceFormatKHR format{}; bool have=false;
    for(auto f:formats) if((f.format==VK_FORMAT_B8G8R8A8_UNORM || f.format==VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { format=f; have=true; break; }
    if(!have || !(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) return 1;
    auto swapci=info<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    swapci.surface=surface; swapci.minImageCount=caps.minImageCount+1;
    if(caps.maxImageCount && swapci.minImageCount>caps.maxImageCount) swapci.minImageCount=caps.maxImageCount;
    swapci.imageFormat=format.format; swapci.imageColorSpace=format.colorSpace;
    swapci.imageExtent=caps.currentExtent;
    if(swapci.imageExtent.width==UINT32_MAX) swapci.imageExtent={640,480};
    if(swapci.imageExtent.width!=640 || swapci.imageExtent.height!=480) { puts("FAIL client extent changed"); return 1; }
    swapci.imageArrayLayers=1; swapci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; swapci.preTransform=caps.currentTransform;
    swapci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; swapci.presentMode=VK_PRESENT_MODE_FIFO_KHR; swapci.clipped=VK_TRUE;
    VkSwapchainKHR swap{}; VK(vkCreateSwapchainKHR(device,&swapci,nullptr,&swap));
    uint32_t ni=0; VK(vkGetSwapchainImagesKHR(device,swap,&ni,nullptr));
    std::vector<VkImage> images(ni); VK(vkGetSwapchainImagesKHR(device,swap,&ni,images.data()));
    auto semci=info<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    VkSemaphore acquired{}; VK(vkCreateSemaphore(device,&semci,nullptr,&acquired));
    // Present wait semaphores belong to images, reused only after reacquisition.
    std::vector<VkSemaphore> rendered(ni);
    for(auto& sem:rendered) VK(vkCreateSemaphore(device,&semci,nullptr,&sem));
    auto poolci=info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    poolci.queueFamilyIndex=family; poolci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{}; VK(vkCreateCommandPool(device,&poolci,nullptr,&pool));
    auto cbai=info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    cbai.commandPool=pool; cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount=1;
    VkCommandBuffer cmd{}; VK(vkAllocateCommandBuffers(device,&cbai,&cmd));
    auto fci=info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO); VkFence fence{}; VK(vkCreateFence(device,&fci,nullptr,&fence));
    VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
    for(unsigned frame=0;frame<60;++frame) {
        if(!pump(100)) { puts("FAIL window closed"); return 1; }
        uint32_t index=0; VK(vkAcquireNextImageKHR(device,swap,5000000000ull,acquired,VK_NULL_HANDLE,&index));
        VK(vkResetCommandBuffer(cmd,0));
        auto begin=info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO); VK(vkBeginCommandBuffer(cmd,&begin));
        auto barrier=info<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=images[index]; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkClearColorValue colour{}; colour.float32[frame/20]=1; colour.float32[3]=1;
        vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&colour,1,&barrier.subresourceRange);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VK(vkEndCommandBuffer(cmd)); VK(vkResetFences(device,1,&fence));
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        auto submit=info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquired; submit.pWaitDstStageMask=&stage;
        submit.commandBufferCount=1; submit.pCommandBuffers=&cmd; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&rendered[index];
        VK(vkQueueSubmit(queue,1,&submit,fence));
        auto present=info<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount=1; present.pWaitSemaphores=&rendered[index]; present.swapchainCount=1;
        present.pSwapchains=&swap; present.pImageIndices=&index;
        VkResult result=vkQueuePresentKHR(queue,&present);
        printf("present frame=%u image=%u rgb=%u,%u,%u result=%d tick_ms=%llu\n",frame,index,
            frame/20==0?255u:0u,frame/20==1?255u:0u,frame/20==2?255u:0u,result,GetTickCount64());
        if(result!=VK_SUCCESS) return 2;
        VK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
        printf("render_fence frame=%u complete=1\n",frame);
    }
    RECT client{}; POINT origin{};
    if(!GetClientRect(window,&client) || !ClientToScreen(window,&origin)) return 1;
    printf("capture_ready rgb=0,0,255 x=%ld y=%ld width=%ld height=%ld hold_ms=10000\n",origin.x,origin.y,client.right,client.bottom);
    if(!pump(10000)) return 1;
    // Teardown may wait in a broken ICD; the lab runner must enforce a process deadline.
    VK(vkDeviceWaitIdle(device));
    vkDestroyFence(device,fence,nullptr); vkDestroySemaphore(device,acquired,nullptr);
    for(auto sem:rendered) vkDestroySemaphore(device,sem,nullptr);
    vkDestroyCommandPool(device,pool,nullptr); vkDestroySwapchainKHR(device,swap,nullptr);
    vkDestroyDevice(device,nullptr); vkDestroySurfaceKHR(instance,surface,nullptr); vkDestroyInstance(instance,nullptr);
    DestroyWindow(window); FreeLibrary(dll);
    puts("PASS 60 presents and render fences; image and Present completion require independent evidence"); return 0;
}
