// SPDX-License-Identifier: MIT
//
// sharecell12 - the D3D12 producer and the RADV consumer, over the exact handle types the b27 Vulkan
// WSI DXGI present route uses.
//
// The cell:
//   1. D3D12 creates a SHARED committed texture (D3D12_HEAP_FLAG_SHARED, BGRA8), as
//      wsi_dxgi_create_d3d12_resource does, and CreateSharedHandle gives an NT handle for it.
//   2. RADV imports that handle as VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT with a
//      dedicated allocation for a VkImage of the same description, as wsi_create_dxgi_image_mem
//      does. The handle is then CLOSED, on both outcomes: the import takes no ownership of it
//      (Vulkan memory.adoc:2466-2472, the route's own V6 finding).
//   3. RADV creates a timeline semaphore exportable as
//      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, exports its handle, and D3D12 opens it with
//      ID3D12Device::OpenSharedHandle as an ID3D12Fence - the same object the route's blit fences are.
//   4. Per round: D3D12 copies a known pattern into the shared texture and signals the shared
//      timeline to the round's ODD value. RADV waits that value ON ITS QUEUE, copies the image into a
//      host-visible buffer and signals the EVEN value. D3D12 waits the even value on its queue before
//      the next round's copy. The pattern is compared on the CPU, texel by texel.
//   5. The same image, the same memory and the same timeline serve every round with a new round
//      constant, so a stale image reads as the wrong constant instead of passing.
//
// What a pass says: the two stacks share a texture and a timeline over these handle types, in this
// order, on this GPU. What it does not say: anything about a swap chain, a present or DWM. M802
// measured D3D12 against D3D12 and D3D12 against D3D11 and is prerequisite coverage only (the
// independent audit of 2026-10-10, A18 row V4).
//
// Nothing resident, no window. --selftest needs no device at all.
#include "sharecell-common.h"

using namespace sharecell;

namespace {

struct d3d12_side {
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    ID3D12CommandAllocator* allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Resource* texture{};   // the SHARED texture both stacks see
    ID3D12Resource* upload{};    // the staging buffer the pattern is written into
    ID3D12Fence* shared{};       // the RADV timeline, opened as an ID3D12Fence
    UINT64 upload_pitch{};
    UINT64 upload_offset{};

    void release()
    {
        if (shared) { shared->Release(); shared = nullptr; }
        if (upload) { upload->Release(); upload = nullptr; }
        if (texture) { texture->Release(); texture = nullptr; }
        if (list) { list->Release(); list = nullptr; }
        if (allocator) { allocator->Release(); allocator = nullptr; }
        if (queue) { queue->Release(); queue = nullptr; }
        if (device) { device->Release(); device = nullptr; }
    }
};

struct vk_side {
    vk_api api{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = UINT32_MAX;
    VkImage image{};
    VkDeviceMemory imported{};
    VkBuffer readback{};
    VkDeviceMemory readback_memory{};
    VkSemaphore timeline{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    void* mapped{};
    VkDeviceSize readback_pitch{};

    void release()
    {
        if (api.DestroyDevice && device != VK_NULL_HANDLE) {
            if (api.DeviceWaitIdle) api.DeviceWaitIdle(device);
            if (mapped) { api.UnmapMemory(device, readback_memory); mapped = nullptr; }
            if (pool) { api.DestroyCommandPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
            if (timeline) { api.DestroySemaphore(device, timeline, nullptr); timeline = VK_NULL_HANDLE; }
            if (readback) { api.DestroyBuffer(device, readback, nullptr); readback = VK_NULL_HANDLE; }
            if (readback_memory) { api.FreeMemory(device, readback_memory, nullptr); readback_memory = VK_NULL_HANDLE; }
            if (image) { api.DestroyImage(device, image, nullptr); image = VK_NULL_HANDLE; }
            if (imported) { api.FreeMemory(device, imported, nullptr); imported = VK_NULL_HANDLE; }
            api.DestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE && api.DestroyInstance) {
            api.DestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }
};

// The DXGI adapter the Vulkan device is on. A share across two adapters is not what the route does
// and would not measure it, so a LUID that does not match is a refusal and not a fallback.
IDXGIAdapter1* pick_adapter(d3d12_api& d3d, const options& o, const LUID* want_luid,
                            std::string* description)
{
    IDXGIFactory4* factory = nullptr;
    if (FAILED(observe_hr(d3d.CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
                          "CreateDXGIFactory2")))
        return nullptr;
    IDXGIAdapter1* chosen = nullptr;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* adapter = nullptr;
        char query[80];
        std::snprintf(query, sizeof(query), "EnumAdapters1 index=%u", i);
        if (observe_hr(factory->EnumAdapters1(i, &adapter), query) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(observe_hr(adapter->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1"))) {
            adapter->Release();
            continue;
        }
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        const bool index_match = o.adapter != UINT32_MAX && o.adapter == i;
        const bool luid_match = want_luid && !software &&
                                desc.AdapterLuid.LowPart == want_luid->LowPart &&
                                desc.AdapterLuid.HighPart == want_luid->HighPart;
        if (index_match || (o.adapter == UINT32_MAX && luid_match)) {
            char narrow[160] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrow, sizeof(narrow) - 1,
                                nullptr, nullptr);
            *description = narrow;
            chosen = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return chosen;
}

bool make_vulkan_instance(vk_side& vk, std::string* why)
{
    if (!vk.api.load(why))
        return false;

    const char* instance_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bc250-sharecell12";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = uint32_t(sizeof(instance_extensions) / sizeof(char*));
    instance_info.ppEnabledExtensionNames = instance_extensions;
    if (vk.api.CreateInstance(&instance_info, nullptr, &vk.instance) != VK_SUCCESS) {
        *why = "vkCreateInstance refused: this machine offers no Vulkan 1.2 instance with the "
               "external-memory capability extensions";
        return false;
    }
    if (!vk.api.load_instance(vk.instance, why))
        return false;

    uint32_t count = 0;
    if (vk.api.EnumeratePhysicalDevices(vk.instance, &count, nullptr) != VK_SUCCESS || !count) {
        *why = "the Vulkan instance has no physical device";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vk.api.EnumeratePhysicalDevices(vk.instance, &count, devices.data());
    vk.physical = devices[0];
    return true;
}

bool make_vulkan_device(vk_side& vk, std::string* why)
{
    vk.family = transfer_queue_family(vk.api, vk.physical);
    if (vk.family == UINT32_MAX) {
        *why = "the Vulkan device has no queue family that can copy";
        return false;
    }
    const char* device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    };
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = vk.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timeline.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &timeline;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = uint32_t(sizeof(device_extensions) / sizeof(char*));
    device_info.ppEnabledExtensionNames = device_extensions;
    if (vk.api.CreateDevice(vk.physical, &device_info, nullptr, &vk.device) != VK_SUCCESS) {
        *why = "vkCreateDevice refused the external-memory and external-semaphore Win32 extensions";
        return false;
    }
    if (!vk.api.load_device(vk.device, why))
        return false;
    vk.api.GetDeviceQueue(vk.device, vk.family, 0, &vk.queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk.family;
    if (vk.api.CreateCommandPool(vk.device, &pool_info, nullptr, &vk.pool) != VK_SUCCESS) {
        *why = "vkCreateCommandPool refused";
        return false;
    }
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = vk.pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (vk.api.AllocateCommandBuffers(vk.device, &cmd_info, &vk.cmd) != VK_SUCCESS) {
        *why = "vkAllocateCommandBuffers refused";
        return false;
    }
    return true;
}

// The imported image leaves UNDEFINED exactly once, BEFORE the producer writes anything into the
// shared texture. A transition out of UNDEFINED may discard the contents, so a per-round transition
// out of UNDEFINED would be a way to lose the very bytes being measured. From here on the image
// stays in GENERAL, which is the layout an externally written image is read in.
bool settle_image_layout(vk_side& vk)
{
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk(vk.api.BeginCommandBuffer(vk.cmd, &begin), "Vulkan layout command buffer begun"))
        return false;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.api.CmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    if (!check_vk(vk.api.EndCommandBuffer(vk.cmd), "Vulkan layout command buffer ended"))
        return false;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.cmd;
    if (!check_vk(vk.api.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE),
                  "the imported image is taken to GENERAL once, before any producer write"))
        return false;
    return check_vk(vk.api.QueueWaitIdle(vk.queue), "that one submission finished");
}

// The one round: the producer writes and signals odd, the consumer waits odd, copies and signals
// even, and the producer waits even before the next round's write.
bool run_round(d3d12_side& d3d, vk_side& vk, const options& o, uint32_t round)
{
    const uint64_t odd = producer_value(round);
    const uint64_t even = consumer_value(round);
    char what[192];

    // The producer's pattern goes into the upload buffer and is copied into the shared texture.
    void* rows = nullptr;
    D3D12_RANGE nothing{0, 0};
    if (!check_hr(d3d.upload->Map(0, &nothing, &rows), "D3D12 upload buffer mapped"))
        return false;
    pattern_fill(static_cast<uint8_t*>(rows) + d3d.upload_offset, o.width, o.height,
                 uint32_t(d3d.upload_pitch), round);
    d3d.upload->Unmap(0, nullptr);

    if (!check_hr(d3d.allocator->Reset(), "D3D12 allocator reset") ||
        !check_hr(d3d.list->Reset(d3d.allocator, nullptr), "D3D12 command list reset"))
        return false;

    // COMMON is the state a shared resource is in while the other API reads it, so each round takes
    // it to COPY_DEST for the copy and hands it back.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d3d.texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    d3d.list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = d3d.texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = d3d.upload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = d3d.upload_offset;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = o.width;
    source.PlacedFootprint.Footprint.Height = o.height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = UINT(d3d.upload_pitch);
    d3d.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    d3d.list->ResourceBarrier(1, &barrier);

    if (!check_hr(d3d.list->Close(), "D3D12 command list closed"))
        return false;
    ID3D12CommandList* lists[] = {d3d.list};
    d3d.queue->ExecuteCommandLists(1, lists);
    if (!check_hr(observe_device_reason(d3d.device, "device after ExecuteCommandLists"),
                  "the D3D12 device is live right after ExecuteCommandLists"))
        return false;

    std::snprintf(what, sizeof(what), "D3D12 queue signals the shared timeline to %llu (odd)",
                  static_cast<unsigned long long>(odd));
    const HRESULT signalled = d3d.queue->Signal(d3d.shared, odd);
    observe_device_reason(d3d.device, "device after queue Signal");
    observe_completed(d3d.shared, "D3D12 fence after queue Signal");
    if (!check_hr(signalled, what))
        return false;

    // The consumer waits that odd value ON ITS QUEUE, not on the CPU: a GPU-side wait is what the
    // route does, and a CPU wait would not measure it.
    if (!check_vk(vk.api.ResetCommandBuffer(vk.cmd, 0), "Vulkan command buffer reset"))
        return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk(vk.api.BeginCommandBuffer(vk.cmd, &begin), "Vulkan command buffer begun"))
        return false;

    // GENERAL to GENERAL: a visibility barrier that cannot discard what the other API wrote.
    VkImageMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before.srcAccessMask = 0;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    before.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before.image = vk.image;
    before.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.api.CmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &before);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = o.width;
    copy.bufferImageHeight = o.height;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {o.width, o.height, 1};
    vk.api.CmdCopyImageToBuffer(vk.cmd, vk.image, VK_IMAGE_LAYOUT_GENERAL, vk.readback, 1, &copy);

    VkMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vk.api.CmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                              1, &host, 0, nullptr, 0, nullptr);
    if (!check_vk(vk.api.EndCommandBuffer(vk.cmd), "Vulkan command buffer ended"))
        return false;

    const uint64_t wait_values[] = {odd};
    const uint64_t signal_values[] = {even};
    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = 1;
    timeline_info.pWaitSemaphoreValues = wait_values;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = signal_values;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline_info;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &vk.timeline;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &vk.timeline;
    std::snprintf(what, sizeof(what),
                  "Vulkan queue waits %llu and signals %llu (even) on the shared timeline",
                  static_cast<unsigned long long>(odd), static_cast<unsigned long long>(even));
    if (!check_vk(vk.api.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE), what))
        return false;

    // Bounded, and never INFINITE: this client keeps the rule of the route it measures.
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &vk.timeline;
    wait.pValues = signal_values;
    std::snprintf(what, sizeof(what), "the shared timeline reached %llu inside 2000 ms",
                  static_cast<unsigned long long>(even));
    if (!check_vk(vk.api.WaitSemaphores(vk.device, &wait, 2000000000ull), what))
        return false;

    // The round trip: a value RADV signalled, read through the D3D12 side of the same object.
    const UINT64 seen = observe_completed(d3d.shared, "D3D12 fence after RADV completion");
    std::snprintf(what, sizeof(what),
                  "the D3D12 side of the timeline reads %llu or more, the value RADV signalled",
                  static_cast<unsigned long long>(even));
    if (!check(seen != UINT64_MAX && seen >= even, what))
        return false;
    check(schedule_is_consumer(seen), "the value the producer reads back belongs to the consumer");

    // And the pattern itself.
    const pattern_diff diff =
        pattern_compare(vk.mapped, o.width, o.height, uint32_t(vk.readback_pitch), round);
    std::snprintf(what, sizeof(what),
                  "round %u: every texel of the shared texture arrived in the Vulkan readback",
                  round);
    if (!check(diff.ok(), what) && !negative_control) {
        std::printf("      first mismatch at %u,%u: want %08x have %08x, %llu texels differ\n",
                    diff.first_x, diff.first_y, diff.want, diff.have,
                    static_cast<unsigned long long>(diff.mismatches));
    }

    // The producer's next write waits for the consumer's value on its own queue. That wait is what
    // makes the image reuse of the next round safe, and it is the second direction of the round trip.
    std::snprintf(what, sizeof(what), "D3D12 queue waits %llu before the next round",
                  static_cast<unsigned long long>(even));
    const HRESULT waited = d3d.queue->Wait(d3d.shared, even);
    observe_device_reason(d3d.device, "device after queue Wait");
    observe_completed(d3d.shared, "D3D12 fence after queue Wait");
    return check_hr(waited, what);
}

int run(const options& o)
{
    d3d12_api d3d_api{};
    d3d12_side d3d{};
    vk_side vk{};
    std::string why, adapter_name, device_name;
    int exit_code = kExitOk;
    uint32_t rounds_run = 0;

    step("loading the two runtimes");
    if (!d3d_api.load(&why)) {
        std::printf("SKIP %s\n", why.c_str());
        observe_loaded_core("early-exit");
        return kExitNoDevice;
    }
    if (!make_vulkan_instance(vk, &why)) {
        std::printf("SKIP %s\n", why.c_str());
        vk.release();
        observe_loaded_core("early-exit");
        return kExitNoDevice;
    }

    LUID luid{};
    const bool has_luid = physical_device_luid(vk.api, vk.physical, &luid, &device_name);
    std::printf("INFO Vulkan device '%s', LUID %s\n", device_name.c_str(),
                has_luid ? "reported" : "not reported");
    if (!check(has_luid || o.adapter != UINT32_MAX,
               "the Vulkan device reports a LUID, or an adapter index was given")) {
        vk.release();
        observe_loaded_core("early-exit");
        return kExitFailed;
    }

    IDXGIAdapter1* adapter = pick_adapter(d3d_api, o, has_luid ? &luid : nullptr, &adapter_name);
    if (!adapter) {
        std::printf("SKIP no DXGI adapter matches the Vulkan device; pass --adapter N to name one\n");
        vk.release();
        observe_loaded_core("early-exit");
        return kExitNoDevice;
    }
    std::printf("INFO DXGI adapter '%s'\n", adapter_name.c_str());

    step("the D3D12 device, queue and command list");
    {
        const HRESULT created =
            d3d_api.D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d.device));
        observe_hr(created, "D3D12CreateDevice");
        observe_loaded_core("after-D3D12CreateDevice");
        adapter->Release();
        if (FAILED(created)) {
            std::printf("SKIP D3D12CreateDevice refused this adapter: hr=0x%08lx\n",
                        static_cast<unsigned long>(created));
            vk.release();
            observe_loaded_core("early-exit");
            return kExitNoDevice;
        }
        if (!probe_native_feature(d3d.device->GetAdapterLuid())) {
            exit_code = kExitFailed;
            goto done;
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!check_hr(d3d.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&d3d.queue)),
                      "D3D12 direct queue created") ||
            !check_hr(d3d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&d3d.allocator)),
                      "D3D12 command allocator created") ||
            !check_hr(d3d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d.allocator,
                                                    nullptr, IID_PPV_ARGS(&d3d.list)),
                      "D3D12 command list created") ||
            !check_hr(d3d.list->Close(), "D3D12 command list closed once, ready to be reset")) {
            exit_code = kExitFailed;
            goto done;
        }
    }

    step("the shared texture: D3D12_HEAP_FLAG_SHARED, the route's own description");
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = o.width;
        desc.Height = o.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = {1, 0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!check_hr(d3d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&d3d.texture)),
                      "the shared D3D12 texture was created")) {
            exit_code = kExitFailed;
            goto done;
        }

        // The upload buffer, sized by the device's own footprint for that texture.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0;
        d3d.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        d3d.upload_pitch = footprint.Footprint.RowPitch;
        d3d.upload_offset = footprint.Offset;
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        upload_heap.CreationNodeMask = 1;
        upload_heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC upload_desc{};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = total ? total : UINT64(d3d.upload_pitch) * o.height;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.Format = DXGI_FORMAT_UNKNOWN;
        upload_desc.SampleDesc = {1, 0};
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!check_hr(d3d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                                          &upload_desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&d3d.upload)),
                      "the D3D12 upload buffer was created")) {
            exit_code = kExitFailed;
            goto done;
        }
    }

    step("RADV imports that texture as a D3D12_RESOURCE, and closes the handle it was given");
    {
        if (!make_vulkan_device(vk, &why)) {
            std::printf("SKIP %s\n", why.c_str());
            exit_code = kExitNoDevice;
            goto done;
        }

        HANDLE resource_handle = nullptr;
        if (!check_hr(d3d.device->CreateSharedHandle(d3d.texture, nullptr, GENERIC_ALL, nullptr,
                                                     &resource_handle),
                      "CreateSharedHandle gave an NT handle for the shared texture")) {
            exit_code = kExitFailed;
            goto done;
        }

        VkExternalMemoryImageCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext = &external;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent = {o.width, o.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!check_vk(vk.api.CreateImage(vk.device, &image_info, nullptr, &vk.image),
                      "the Vulkan image over the shared texture was created")) {
            CloseHandle(resource_handle);
            exit_code = kExitFailed;
            goto done;
        }

        VkMemoryRequirements reqs{};
        vk.api.GetImageMemoryRequirements(vk.device, vk.image, &reqs);
        VkImportMemoryWin32HandleInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = resource_handle;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &import;
        dedicated.image = vk.image;
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vk.api.GetPhysicalDeviceMemoryProperties(vk.physical, &memory_properties);
        const uint32_t device_type = memory_type_index(memory_properties, reqs.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &dedicated;
        allocate.allocationSize = reqs.size;
        allocate.memoryTypeIndex = device_type != UINT32_MAX ? device_type : 0;
        const VkResult brought_in = vk.api.AllocateMemory(vk.device, &allocate, nullptr, &vk.imported);

        // V6 of the audit, applied to our own client too: the handle is ours on BOTH outcomes.
        CloseHandle(resource_handle);
        resource_handle = nullptr;
        if (!check_vk(brought_in, "RADV imported the D3D12 resource as dedicated memory") ||
            !check_vk(vk.api.BindImageMemory(vk.device, vk.image, vk.imported, 0),
                      "the imported memory is bound to the Vulkan image")) {
            exit_code = kExitFailed;
            goto done;
        }
        std::printf("INFO the shared surface: D3D12 footprint pitch %llu, Vulkan allocation %llu "
                    "bytes\n",
                    static_cast<unsigned long long>(d3d.upload_pitch),
                    static_cast<unsigned long long>(reqs.size));
    }

    step("the readback buffer RADV copies into");
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = VkDeviceSize(o.width) * o.height * 4u;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check_vk(vk.api.CreateBuffer(vk.device, &buffer_info, nullptr, &vk.readback),
                      "the readback buffer was created")) {
            exit_code = kExitFailed;
            goto done;
        }
        VkMemoryRequirements reqs{};
        vk.api.GetBufferMemoryRequirements(vk.device, vk.readback, &reqs);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vk.api.GetPhysicalDeviceMemoryProperties(vk.physical, &memory_properties);
        const uint32_t host_type =
            memory_type_index(memory_properties, reqs.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!check(host_type != UINT32_MAX, "the device has host-visible coherent memory")) {
            exit_code = kExitFailed;
            goto done;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = reqs.size;
        allocate.memoryTypeIndex = host_type;
        if (!check_vk(vk.api.AllocateMemory(vk.device, &allocate, nullptr, &vk.readback_memory),
                      "the readback memory was allocated") ||
            !check_vk(vk.api.BindBufferMemory(vk.device, vk.readback, vk.readback_memory, 0),
                      "the readback memory is bound") ||
            !check_vk(vk.api.MapMemory(vk.device, vk.readback_memory, 0, VK_WHOLE_SIZE, 0,
                                       &vk.mapped),
                      "the readback memory is mapped")) {
            exit_code = kExitFailed;
            goto done;
        }
        vk.readback_pitch = VkDeviceSize(o.width) * 4u;
    }

    step(o.origin == fence_origin::d3d12 ?
         "the shared timeline: D3D12 creates and reopens SHARED, RADV imports D3D12_FENCE" :
         "the shared timeline: RADV exports it as a D3D12_FENCE, D3D12 opens it");
    observe_graphics_modules();
    observe_loaded_core("before-fence-open");
    {
        if (o.origin == fence_origin::d3d12) {
            if (!create_d3d12_origin(vk.api, vk.device, d3d.device, &vk.timeline, &d3d.shared, true)) {
                exit_code = kExitFailed;
                goto done;
            }
        } else {
            VkSemaphoreTypeCreateInfo type{};
            type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue = 0;
            VkExportSemaphoreCreateInfo export_info{};
            export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            export_info.pNext = &type;
            export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            semaphore_info.pNext = &export_info;
            if (!check_vk(vk.api.CreateSemaphore(vk.device, &semaphore_info, nullptr, &vk.timeline),
                          "RADV created an exportable timeline semaphore")) {
                exit_code = kExitFailed;
                goto done;
            }
            HANDLE fence_handle = nullptr;
            uint64_t before_export = UINT64_MAX;
            if (check_vk(vk.api.GetSemaphoreCounterValue(vk.device, vk.timeline, &before_export),
                         "RADV reads the fresh timeline before export")) {
                std::printf("INFO RADV timeline before export value=%llu hex=0x%016llx\n",
                            static_cast<unsigned long long>(before_export),
                            static_cast<unsigned long long>(before_export));
                check(before_export == 0, "the RADV timeline is zero before export");
            }
            VkSemaphoreGetWin32HandleInfoKHR get{};
            get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
            get.semaphore = vk.timeline;
            get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            if (!check_vk(vk.api.GetSemaphoreWin32HandleKHR(vk.device, &get, &fence_handle),
                          "RADV exported it as a D3D12_FENCE handle") ||
                !check(fence_handle != nullptr, "the exported handle is not null")) {
                exit_code = kExitFailed;
                goto done;
            }
            if (!replay_exported_fence(fence_handle, d3d.device->GetAdapterLuid())) {
                CloseHandle(fence_handle);
                exit_code = kExitFailed;
                goto done;
            }
            observe_device_reason(d3d.device, "device before OpenSharedHandle(fence)");
            const HRESULT opened = d3d.device->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&d3d.shared));
            observe_device_reason(d3d.device, "device after OpenSharedHandle(fence)");
            CloseHandle(fence_handle);   // the semaphore handle is the application's, as in the route
            if (!check_hr(opened, "D3D12 opened the exported timeline as an ID3D12Fence")) {
                exit_code = kExitFailed;
                goto done;
            }
        }
        ID3D12Fence1* fence1 = nullptr;
        if (SUCCEEDED(observe_hr(d3d.shared->QueryInterface(IID_PPV_ARGS(&fence1)),
                                 "QueryInterface(ID3D12Fence1) for creation flags"))) {
            std::printf("INFO shared fence creation_flags=0x%08x\n",
                        static_cast<unsigned>(fence1->GetCreationFlags()));
            fence1->Release();
        }
        ID3D12Device* fence_device = nullptr;
        const HRESULT got_device = d3d.shared->GetDevice(IID_PPV_ARGS(&fence_device));
        check_hr(got_device, "ID3D12Fence::GetDevice");
        if (SUCCEEDED(got_device)) {
            const LUID fence_luid = fence_device->GetAdapterLuid();
            const LUID queue_luid = d3d.device->GetAdapterLuid();
            check(fence_luid.LowPart == queue_luid.LowPart &&
                  fence_luid.HighPart == queue_luid.HighPart,
                  "the opened fence and the queue device use the same adapter LUID");
            std::printf("INFO fence device nodes=%u queue device nodes=%u\n",
                        fence_device->GetNodeCount(), d3d.device->GetNodeCount());
            fence_device->Release();
        }
        check(observe_completed(d3d.shared, "fresh D3D12 shared fence") == 0,
              "the freshly shared timeline reads 0 on the D3D12 side");
        uint64_t initial = UINT64_MAX;
        if (check_vk(vk.api.GetSemaphoreCounterValue(vk.device, vk.timeline, &initial),
                     "the RADV side reads the fresh timeline")) {
            std::printf("INFO fresh RADV timeline value=%llu hex=0x%016llx\n",
                        static_cast<unsigned long long>(initial),
                        static_cast<unsigned long long>(initial));
            check(initial == 0, "the freshly shared timeline reads 0 on the RADV side");
        }
        observe_device_reason(d3d.device, "device after initial fence reads");
    }

    step("the imported image leaves UNDEFINED once, before any producer write");
    if (!settle_image_layout(vk)) {
        exit_code = kExitFailed;
        goto done;
    }

    step("the rounds: pattern, odd signal, Vulkan copy, even signal, readback, image reuse");
    for (uint32_t round = 0; round < o.rounds; round++) {
        char line[96];
        std::snprintf(line, sizeof(line), "round %u of %u", round, o.rounds);
        step(line);
        if (!run_round(d3d, vk, o, round)) {
            exit_code = kExitFailed;
            break;
        }
        rounds_run++;
    }
    // Image reuse, stated as its own case: one VkImage, one imported allocation and one D3D12
    // resource carried every round, so a round that had read the image of an earlier round would
    // have failed on that round's constant.
    check(rounds_run == o.rounds,
          "every round passed on the same image, the same memory and the same timeline");

done:
    observe_loaded_core("final-before-release");
    if (vk.api.DeviceWaitIdle && vk.device != VK_NULL_HANDLE)
        vk.api.DeviceWaitIdle(vk.device);
    vk.release();
    d3d.release();
    if (failures)
        exit_code = kExitFailed;
    std::printf("sharecell12 d3d12-producer -> radv-consumer: %d checks, %d failed%s\n", checks,
                failures, negative_control ? " (negative control)" : "");
    return exit_code;
}

}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // Preserve diagnostics if a driver call never returns.
    const options o = parse_options(argc, argv);
    if (o.help) {
        std::printf("sharecell12 - a D3D12 producer and a RADV consumer over one shared texture and\n"
                    "one shared timeline, the handle types the b27 Vulkan WSI DXGI route uses.\n\n"
                    "%s",
                    kUsage);
        return kExitOk;
    }
    if (o.bad_option) {
        std::printf("unknown or bad argument: %s\n\n%s", o.bad.c_str(), kUsage);
        return kExitUsage;
    }
    negative_control = o.negative_control;
    if (o.selftest)
        return run_selftest(o) ? kExitFailed : kExitOk;
    // The pure rules first: a GPU arm whose own pattern or schedule were wrong would otherwise report
    // a sharing failure that it had caused itself. Under --negative-control that pass is left out,
    // because every case is inverted there and the host rules would then "fail" before the GPU part
    // ran at all; the host rules get their own inverted run through --selftest --negative-control.
    if (!o.negative_control && run_selftest(o))
        return kExitFailed;
    checks = 0;
    failures = 0;
    return run(o);
}
