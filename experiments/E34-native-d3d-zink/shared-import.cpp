#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
using Microsoft::WRL::ComPtr;
#define HR(x) do{HRESULT r=(x);printf("%s: %08lx\n",#x,r);if(FAILED(r))return 2;}while(0)
#define NT(x) do{NTSTATUS r=(x);printf("%s: %08lx\n",#x,r);if(r<0)return 3;}while(0)
#define VK(x) do{VkResult r=(x);printf("%s: %d\n",#x,r);if(r!=VK_SUCCESS)return 4;}while(0)
#define IP(name) auto name=(PFN_##name)gipa(instance,#name);if(!name){puts("missing " #name);return 5;}
#define DP(name) auto name=(PFN_##name)gdpa(device,#name);if(!name){puts("missing " #name);return 5;}
int main(int argc,char** argv){
 setvbuf(stdout,nullptr,_IONBF,0);
 ComPtr<IDXGIFactory1> f;HR(CreateDXGIFactory1(IID_PPV_ARGS(&f)));LUID luid={};unsigned found=0;
 for(UINT i=0;;i++){ComPtr<IDXGIAdapter1>a;if(f->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d={};HR(a->GetDesc1(&d));if(d.VendorId==0x1002&&d.DeviceId==0x13fe){luid=d.AdapterLuid;found++;}}
 if(found!=1)return 6;
 D3DKMT_OPENADAPTERFROMLUID oa={};oa.AdapterLuid=luid;NT(D3DKMTOpenAdapterFromLuid(&oa));
 D3DKMT_CREATEDEVICE cd={};cd.hAdapter=oa.hAdapter;NT(D3DKMTCreateDevice(&cd));
 struct Surface {UINT magic,version,width,height,pitch,format;UINT64 size;};
 Surface surface={0x4137424c,1,64,64,256,D3DDDIFMT_A8B8G8R8,16384};static_assert(sizeof(Surface)==32);
 D3DDDI_ALLOCATIONINFO2 ai={};ai.pPrivateDriverData=&surface;ai.PrivateDriverDataSize=sizeof(surface);ai.VidPnSourceId=~0u;
 D3DKMT_CREATEALLOCATION ca={};ca.hDevice=cd.hDevice;ca.NumAllocations=1;ca.pAllocationInfo2=&ai;ca.Flags.CreateResource=1;ca.Flags.CreateShared=1;ca.Flags.NtSecuritySharing=1;ca.Flags.NonSecure=1;
 UINT resourceData[]={0x52363245,2,1,0};
 ca.pPrivateDriverData=resourceData;ca.PrivateDriverDataSize=sizeof(resourceData);
 if(argc>1){ca.Flags.CreateShared=0;ca.Flags.NtSecuritySharing=0;}
 printf("create resource=%u shared=%u nt=%u nonsecure=%u\n",ca.Flags.CreateResource,ca.Flags.CreateShared,ca.Flags.NtSecuritySharing,ca.Flags.NonSecure);
 NT(D3DKMTCreateAllocation2(&ca));
 if(argc>1){D3DKMT_DESTROYALLOCATION2 d={};d.hDevice=cd.hDevice;d.hResource=ca.hResource;NT(D3DKMTDestroyAllocation2(&d));D3DKMT_DESTROYDEVICE dd={};dd.hDevice=cd.hDevice;NT(D3DKMTDestroyDevice(&dd));D3DKMT_CLOSEADAPTER a={};a.hAdapter=oa.hAdapter;NT(D3DKMTCloseAdapter(&a));puts("PASS unshared allocation control");return 0;}
 HANDLE shared=nullptr;
 OBJECT_ATTRIBUTES attrs={};attrs.Length=sizeof(attrs);
 NT(D3DKMTShareObjects(1,&ca.hResource,&attrs,SHARED_ALLOCATION_ALL_ACCESS,&shared));
 D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE qi={};qi.hDevice=cd.hDevice;qi.hNtHandle=shared;NT(D3DKMTQueryResourceInfoFromNtHandle(&qi));
 printf("private sizes: allocation=%u resource=%u runtime=%u allocations=%u\n",qi.TotalPrivateDriverDataSize,qi.ResourcePrivateDriverDataSize,qi.PrivateRuntimeDataSize,qi.NumAllocations);
 HMODULE dll=LoadLibraryA("vulkan_radeon.dll");if(!dll){printf("ICD load=%lu\n",GetLastError());return 7;}
 auto gipa=(PFN_vkGetInstanceProcAddr)GetProcAddress(dll,"vk_icdGetInstanceProcAddr");if(!gipa)return 8;
 auto create=(PFN_vkCreateInstance)gipa(nullptr,"vkCreateInstance");VkInstance instance=VK_NULL_HANDLE;
 VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="BC250 native LB7A import";app.apiVersion=VK_API_VERSION_1_2;
 VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;VK(create(&ici,nullptr,&instance));
 IP(vkEnumeratePhysicalDevices);IP(vkGetPhysicalDeviceProperties2);IP(vkGetPhysicalDeviceQueueFamilyProperties);IP(vkGetPhysicalDeviceMemoryProperties);IP(vkCreateDevice);IP(vkDestroyInstance);
 uint32_t count=0;VK(vkEnumeratePhysicalDevices(instance,&count,nullptr));std::vector<VkPhysicalDevice> pds(count);VK(vkEnumeratePhysicalDevices(instance,&count,pds.data()));VkPhysicalDevice pd=VK_NULL_HANDLE;
 for(auto p:pds){VkPhysicalDeviceIDProperties id={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};VkPhysicalDeviceProperties2 props={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&id;vkGetPhysicalDeviceProperties2(p,&props);if(id.deviceLUIDValid&&!memcmp(id.deviceLUID,&luid,8)){pd=p;printf("GPU: %s\n",props.properties.deviceName);}}
 if(!pd)return 9;
 uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,nullptr);std::vector<VkQueueFamilyProperties> qp(nq);vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,qp.data());uint32_t q=0;while(q<nq&&!(qp[q].queueFlags&VK_QUEUE_GRAPHICS_BIT))q++;if(q==nq)return 10;
 float priority=1;VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=q;qci.queueCount=1;qci.pQueuePriorities=&priority;
 const char* ext[]={VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=1;dci.ppEnabledExtensionNames=ext;
 VkDevice device;VK(vkCreateDevice(pd,&dci,nullptr,&device));auto gdpa=(PFN_vkGetDeviceProcAddr)gipa(instance,"vkGetDeviceProcAddr");
 DP(vkCreateImage);DP(vkGetImageMemoryRequirements);DP(vkAllocateMemory);DP(vkBindImageMemory);DP(vkGetDeviceQueue);DP(vkCreateCommandPool);DP(vkAllocateCommandBuffers);DP(vkBeginCommandBuffer);DP(vkCmdPipelineBarrier);DP(vkCmdClearColorImage);DP(vkEndCommandBuffer);DP(vkCreateFence);DP(vkQueueSubmit);DP(vkWaitForFences);DP(vkDestroyFence);DP(vkDestroyCommandPool);DP(vkDestroyImage);DP(vkFreeMemory);DP(vkDestroyDevice);
 VkExternalMemoryImageCreateInfo external={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
 VkImageCreateInfo imageci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};imageci.pNext=&external;imageci.imageType=VK_IMAGE_TYPE_2D;imageci.format=VK_FORMAT_R8G8B8A8_UNORM;imageci.extent={64,64,1};imageci.mipLevels=imageci.arrayLayers=1;imageci.samples=VK_SAMPLE_COUNT_1_BIT;imageci.tiling=VK_IMAGE_TILING_LINEAR;imageci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
 VkImage image;VK(vkCreateImage(device,&imageci,nullptr,&image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,image,&req);printf("image bytes=%llu alignment=%llu types=%x\n",(unsigned long long)req.size,(unsigned long long)req.alignment,req.memoryTypeBits);
 if(req.size>surface.size)return 11;
 VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(pd,&mp);uint32_t mt=0;while(mt<mp.memoryTypeCount&&(!(req.memoryTypeBits&(1u<<mt))||!(mp.memoryTypes[mt].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))mt++;if(mt==mp.memoryTypeCount)return 12;
 VkImportMemoryWin32HandleInfoKHR im={VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;im.handle=shared;
 VkMemoryDedicatedAllocateInfo dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image;im.pNext=&dedicated;
 VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mai.pNext=&im;mai.allocationSize=req.size;mai.memoryTypeIndex=mt;VkDeviceMemory memory;VK(vkAllocateMemory(device,&mai,nullptr,&memory));
 DWORD handleFlags=0;if(!GetHandleInformation(shared,&handleFlags)){puts("FAIL imported handle was consumed");return 13;}puts("NT handle ownership retained");
 VK(vkBindImageMemory(device,image,memory,0));VkQueue queue;vkGetDeviceQueue(device,q,0,&queue);
 VkCommandPoolCreateInfo pci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=q;VkCommandPool pool;VK(vkCreateCommandPool(device,&pci,nullptr,&pool));
 VkCommandBufferAllocateInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbi.commandPool=pool;cbi.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cbi.commandBufferCount=1;VkCommandBuffer cmd;VK(vkAllocateCommandBuffers(device,&cbi,&cmd));
 VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK(vkBeginCommandBuffer(cmd,&begin));
 VkImageMemoryBarrier barrier={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
 VkClearColorValue blue={{0,0,1,1}};vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&blue,1,&barrier.subresourceRange);
 barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,0,nullptr,1,&barrier);VK(vkEndCommandBuffer(cmd));
 VkFenceCreateInfo fci={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;VK(vkCreateFence(device,&fci,nullptr,&fence));VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;VK(vkQueueSubmit(queue,1,&submit,fence));VK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
 D3DKMT_LOCK2 lock={};lock.hDevice=cd.hDevice;lock.hAllocation=ai.hAllocation;NT(D3DKMTLock2(&lock));unsigned bad=0;for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){auto pixel=(const unsigned char*)lock.pData+y*surface.pitch+x*4;if(pixel[0]!=0||pixel[1]!=0||pixel[2]!=255||pixel[3]!=255)bad++;}printf("original WDDM allocation blue mismatches=%u/4096\n",bad);
 D3DKMT_UNLOCK2 unlock={};unlock.hDevice=cd.hDevice;unlock.hAllocation=ai.hAllocation;NT(D3DKMTUnlock2(&unlock));
 vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,memory,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
 if(!CloseHandle(shared))return 14;D3DKMT_DESTROYALLOCATION2 da={};da.hDevice=cd.hDevice;da.hResource=ca.hResource;NT(D3DKMTDestroyAllocation2(&da));D3DKMT_DESTROYDEVICE dd={};dd.hDevice=cd.hDevice;NT(D3DKMTDestroyDevice(&dd));D3DKMT_CLOSEADAPTER close={};close.hAdapter=oa.hAdapter;NT(D3DKMTCloseAdapter(&close));
 puts(bad?"FAIL":"PASS shared WDDM surface written by Vulkan GPU");return bad?15:0;
}
