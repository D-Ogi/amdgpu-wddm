// SPDX-License-Identifier: MIT
//
// sharecellvk - the RADV producer and the D3D12 consumer: the other direction of sharecell12, over
// the same two handle types the b27 Vulkan WSI DXGI present route uses.
//
// The route itself only ever moves bytes this way: RADV renders, the D3D12 queue copies the result
// into a swap-chain buffer and presents it. sharecell12 measures the pair with D3D12 writing and
// RADV reading; this client measures RADV writing and D3D12 reading, which is the direction the
// route depends on. The objects are the same kind in both clients, because the route creates them
// that way: the TEXTURE always belongs to D3D12 and is imported by RADV
// (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT), and the TIMELINE always belongs to RADV and
// is opened by D3D12 (VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT through
// ID3D12Device::OpenSharedHandle). Only which side writes changes.
//
// Per round: RADV copies a known pattern from a host-visible buffer into the imported image and
// signals the round's ODD value; the D3D12 queue waits that value, copies the shared texture into a
// READBACK buffer and signals the EVEN value; RADV waits the even value on its own queue before the
// next round writes the image again. The CPU compares the readback texel by texel, with the round's
// own constant, so a stale image fails instead of passing.
//
// What a pass says: RADV's writes to an imported D3D12 texture become visible to the D3D12 queue
// when the shared timeline says so, on this GPU. What it does not say: anything about a swap chain,
// a present or DWM.
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
    ID3D12Resource* texture{};    // the SHARED texture both stacks see
    ID3D12Resource* readback{};   // where the consumer copies it, for the CPU to compare
    ID3D12Fence* shared{};        // the RADV timeline, opened as an ID3D12Fence
    UINT64 readback_pitch{};
    UINT64 readback_offset{};
    void* mapped{};

    void release()
    {
        if (mapped && readback) { readback->Unmap(0, nullptr); mapped = nullptr; }
        if (shared) { shared->Release(); shared = nullptr; }
        if (readback) { readback->Release(); readback = nullptr; }
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
    VkBuffer upload{};
    VkDeviceMemory upload_memory{};
    VkSemaphore timeline{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    void* mapped{};

    void release()
    {
        if (api.DestroyDevice && device != VK_NULL_HANDLE) {
            if (api.DeviceWaitIdle) api.DeviceWaitIdle(device);
            if (mapped) { api.UnmapMemory(device, upload_memory); mapped = nullptr; }
            if (pool) { api.DestroyCommandPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
            if (timeline) { api.DestroySemaphore(device, timeline, nullptr); timeline = VK_NULL_HANDLE; }
            if (upload) { api.DestroyBuffer(device, upload, nullptr); upload = VK_NULL_HANDLE; }
            if (upload_memory) { api.FreeMemory(device, upload_memory, nullptr); upload_memory = VK_NULL_HANDLE; }
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
    app.pApplicationName = "bc250-sharecellvk";
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

// Here the producer is RADV, so the image leaves UNDEFINED on the way into its FIRST write. That one
// transition may discard the contents, which is correct: nothing has been written yet. Afterwards
// the image stays in GENERAL for every round, because a later transition out of UNDEFINED could
// discard the pattern the round just wrote.
bool run_round(d3d12_side& d3d, vk_side& vk, const options& o, uint32_t round)
{
    const uint64_t odd = producer_value(round);
    const uint64_t even = consumer_value(round);
    char what[192];

    // The producer's pattern goes into the Vulkan upload buffer, tightly packed.
    pattern_fill(vk.mapped, o.width, o.height, o.width * 4u, round);

    if (!check_vk(vk.api.ResetCommandBuffer(vk.cmd, 0), "Vulkan command buffer reset"))
        return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk(vk.api.BeginCommandBuffer(vk.cmd, &begin), "Vulkan command buffer begun"))
        return false;

    VkImageMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before.srcAccessMask = 0;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before.oldLayout = round == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
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
    vk.api.CmdCopyBufferToImage(vk.cmd, vk.upload, vk.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);

    // The write must be available to the other API's reads, which the semaphore signal then makes
    // visible to them. VK_QUEUE_FAMILY_EXTERNAL is the release half of the ownership transfer the
    // next reader completes implicitly.
    VkImageMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = 0;
    after.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    after.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after.image = vk.image;
    after.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.api.CmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &after);
    if (!check_vk(vk.api.EndCommandBuffer(vk.cmd), "Vulkan command buffer ended"))
        return false;

    // Round 0 has nothing to wait for; every later round waits the previous round's even value, so
    // the consumer's copy is finished before the producer writes the image again. That wait is the
    // image reuse made safe, and it is this client's second direction of the round trip.
    const uint64_t previous_even = round ? consumer_value(round - 1) : 0;
    const uint64_t wait_values[] = {previous_even};
    const uint64_t signal_values[] = {odd};
    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = round ? 1u : 0u;
    timeline_info.pWaitSemaphoreValues = round ? wait_values : nullptr;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = signal_values;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timeline_info;
    submit.waitSemaphoreCount = round ? 1u : 0u;
    submit.pWaitSemaphores = round ? &vk.timeline : nullptr;
    submit.pWaitDstStageMask = round ? &stage : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &vk.timeline;
    if (round) {
        std::snprintf(what, sizeof(what),
                      "Vulkan queue waits %llu (the consumer's last value) and signals %llu (odd)",
                      static_cast<unsigned long long>(previous_even),
                      static_cast<unsigned long long>(odd));
    } else {
        std::snprintf(what, sizeof(what),
                      "Vulkan queue writes the image and signals %llu (odd)",
                      static_cast<unsigned long long>(odd));
    }
    if (!check_vk(vk.api.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE), what))
        return false;

    // The consumer waits that odd value ON ITS QUEUE and copies the shared texture out.
    const HRESULT waited = d3d.queue->Wait(d3d.shared, odd);
    observe_device_reason(d3d.device, "device after queue Wait");
    observe_completed(d3d.shared, "D3D12 fence after queue Wait");
    if (!check_hr(waited, "D3D12 queue waits the producer's odd value") ||
        !check_hr(d3d.allocator->Reset(), "D3D12 allocator reset") ||
        !check_hr(d3d.list->Reset(d3d.allocator, nullptr), "D3D12 command list reset"))
        return false;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d3d.texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    d3d.list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = d3d.texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = d3d.readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = d3d.readback_offset;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width = o.width;
    destination.PlacedFootprint.Footprint.Height = o.height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = UINT(d3d.readback_pitch);
    d3d.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    d3d.list->ResourceBarrier(1, &barrier);

    if (!check_hr(d3d.list->Close(), "D3D12 command list closed"))
        return false;
    ID3D12CommandList* lists[] = {d3d.list};
    d3d.queue->ExecuteCommandLists(1, lists);
    if (!check_hr(observe_device_reason(d3d.device, "device after ExecuteCommandLists"),
                  "the D3D12 device is live right after ExecuteCommandLists"))
        return false;

    std::snprintf(what, sizeof(what), "D3D12 queue signals the shared timeline to %llu (even)",
                  static_cast<unsigned long long>(even));
    const HRESULT signalled = d3d.queue->Signal(d3d.shared, even);
    observe_device_reason(d3d.device, "device after queue Signal");
    observe_completed(d3d.shared, "D3D12 fence after queue Signal");
    if (!check_hr(signalled, what))
        return false;

    // The CPU waits for that even value through the RADV side of the same object: bounded, never
    // INFINITE. A timeout here is a result and not a reason to read the buffer anyway.
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &vk.timeline;
    const uint64_t even_values[] = {even};
    wait.pValues = even_values;
    std::snprintf(what, sizeof(what),
                  "the RADV side of the timeline reached %llu, the consumer's value, inside 2000 ms",
                  static_cast<unsigned long long>(even));
    if (!check_vk(vk.api.WaitSemaphores(vk.device, &wait, 2000000000ull), what))
        return false;

    uint64_t counter = 0;
    if (check_vk(vk.api.GetSemaphoreCounterValue(vk.device, vk.timeline, &counter),
                 "the RADV side reads the timeline's value back")) {
        std::printf("INFO RADV timeline after D3D12 completion value=%llu hex=0x%016llx\n",
                    static_cast<unsigned long long>(counter),
                    static_cast<unsigned long long>(counter));
        std::snprintf(what, sizeof(what), "that value is %llu or more and belongs to the consumer",
                      static_cast<unsigned long long>(even));
        check(counter >= even && schedule_is_consumer(counter), what);
    }

    const pattern_diff diff = pattern_compare(static_cast<uint8_t*>(d3d.mapped) + d3d.readback_offset,
                                              o.width, o.height, uint32_t(d3d.readback_pitch), round);
    std::snprintf(what, sizeof(what),
                  "round %u: every texel RADV wrote arrived in the D3D12 readback", round);
    if (!check(diff.ok(), what) && !negative_control) {
        std::printf("      first mismatch at %u,%u: want %08x have %08x, %llu texels differ\n",
                    diff.first_x, diff.first_y, diff.want, diff.have,
                    static_cast<unsigned long long>(diff.mismatches));
    }
    return true;
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
        return kExitNoDevice;
    }
    if (!make_vulkan_instance(vk, &why)) {
        std::printf("SKIP %s\n", why.c_str());
        vk.release();
        return kExitNoDevice;
    }

    LUID luid{};
    const bool has_luid = physical_device_luid(vk.api, vk.physical, &luid, &device_name);
    std::printf("INFO Vulkan device '%s', LUID %s\n", device_name.c_str(),
                has_luid ? "reported" : "not reported");
    if (!check(has_luid || o.adapter != UINT32_MAX,
               "the Vulkan device reports a LUID, or an adapter index was given")) {
        vk.release();
        return kExitFailed;
    }

    IDXGIAdapter1* adapter = pick_adapter(d3d_api, o, has_luid ? &luid : nullptr, &adapter_name);
    if (!adapter) {
        std::printf("SKIP no DXGI adapter matches the Vulkan device; pass --adapter N to name one\n");
        vk.release();
        return kExitNoDevice;
    }
    std::printf("INFO DXGI adapter '%s'\n", adapter_name.c_str());

    step("the D3D12 device, queue and command list");
    {
        const HRESULT created =
            d3d_api.D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d.device));
        observe_hr(created, "D3D12CreateDevice");
        adapter->Release();
        if (FAILED(created)) {
            std::printf("SKIP D3D12CreateDevice refused this adapter: hr=0x%08lx\n",
                        static_cast<unsigned long>(created));
            vk.release();
            return kExitNoDevice;
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

    step("the shared texture and the readback buffer the consumer copies into");
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

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0;
        d3d.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        d3d.readback_pitch = footprint.Footprint.RowPitch;
        d3d.readback_offset = footprint.Offset;
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        readback_heap.CreationNodeMask = 1;
        readback_heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = total ? total : UINT64(d3d.readback_pitch) * o.height;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.Format = DXGI_FORMAT_UNKNOWN;
        readback_desc.SampleDesc = {1, 0};
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!check_hr(d3d.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                          &readback_desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&d3d.readback)),
                      "the D3D12 readback buffer was created") ||
            !check_hr(d3d.readback->Map(0, nullptr, &d3d.mapped),
                      "the D3D12 readback buffer is mapped")) {
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

        // V6 of the audit, in this client too: the handle is ours on BOTH outcomes of the import.
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
                    static_cast<unsigned long long>(d3d.readback_pitch),
                    static_cast<unsigned long long>(reqs.size));
    }

    step("the upload buffer RADV writes the pattern from");
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = VkDeviceSize(o.width) * o.height * 4u;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check_vk(vk.api.CreateBuffer(vk.device, &buffer_info, nullptr, &vk.upload),
                      "the upload buffer was created")) {
            exit_code = kExitFailed;
            goto done;
        }
        VkMemoryRequirements reqs{};
        vk.api.GetBufferMemoryRequirements(vk.device, vk.upload, &reqs);
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
        if (!check_vk(vk.api.AllocateMemory(vk.device, &allocate, nullptr, &vk.upload_memory),
                      "the upload memory was allocated") ||
            !check_vk(vk.api.BindBufferMemory(vk.device, vk.upload, vk.upload_memory, 0),
                      "the upload memory is bound") ||
            !check_vk(vk.api.MapMemory(vk.device, vk.upload_memory, 0, VK_WHOLE_SIZE, 0, &vk.mapped),
                      "the upload memory is mapped")) {
            exit_code = kExitFailed;
            goto done;
        }
    }

    step(o.origin == fence_origin::d3d12 ?
         "the shared timeline: D3D12 creates and reopens SHARED, RADV imports D3D12_FENCE" :
         "the shared timeline: RADV exports it as a D3D12_FENCE, D3D12 opens it");
    observe_graphics_modules();
    {
        if (o.origin == fence_origin::d3d12) {
            if (!create_d3d12_origin(vk.api, vk.device, d3d.device, &vk.timeline, &d3d.shared)) {
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

    step("the rounds: RADV writes, odd signal, D3D12 copies out, even signal, readback, image reuse");
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
    check(rounds_run == o.rounds,
          "every round passed on the same image, the same memory and the same timeline");

done:
    if (vk.api.DeviceWaitIdle && vk.device != VK_NULL_HANDLE)
        vk.api.DeviceWaitIdle(vk.device);
    vk.release();
    d3d.release();
    if (failures)
        exit_code = kExitFailed;
    std::printf("sharecellvk radv-producer -> d3d12-consumer: %d checks, %d failed%s\n", checks,
                failures, negative_control ? " (negative control)" : "");
    return exit_code;
}

}  // namespace

int main(int argc, char** argv)
{
    const options o = parse_options(argc, argv);
    if (o.help) {
        std::printf("sharecellvk - a RADV producer and a D3D12 consumer over one shared texture and\n"
                    "one shared timeline, the handle types the b27 Vulkan WSI DXGI route uses. This\n"
                    "is the direction the route itself depends on.\n\n"
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
