// SPDX-License-Identifier: MIT
// Round trip 5: RuntimeBacked heaps, the native driver's only memory mode, over engine ABI 1.2 V10. A stub shell
// plays the native12 shell's allocate_memory and free_memory: it allocates each heap's VkDeviceMemory on the
// engine's own VkDevice (GetVulkanHandles), as INTEGRATION.md asks of the shell, where the real shell imports a
// runtime allocation instead. Its allocation handles are stand-ins: there is no kernel allocation here.
//
// A committed UPLOAD buffer of 128 KiB serves as the heap of a placed buffer at 64 KiB (the committed shape with a
// heap-wide buffer is the INFERENCE of engine-ddi.h for the runtime's heaps). The placed buffer is filled through
// MapHeap, copied into a committed DEFAULT buffer and on into a committed READBACK buffer, and read back through
// MapHeap word for word. Every allocation must come back through free_memory once, after the engine's heap.
#include "harness.h"
#include <algorithm>
#include <cstring>

namespace harness {

namespace {
StubMemory& stub_of(void* shell) { return *static_cast<StubMemory*>(static_cast<Shell*>(shell)->memory); }

// The memory type vkd3d-proton picks for the CUSTOM heap engine-ddi makes from the DDI heap (its
// vkd3d_select_memory_flags for that CPU page property), among the types a buffer can use. V10 takes only such a
// type.
uint32_t pick_type(const StubMemory& m, uint32_t buffer_types, D3D12DDI_CPU_PAGE_PROPERTY cpu) {
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (cpu == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK)
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    else if (cpu == D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE)
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < m.properties.memoryTypeCount; ++i)
        if (((buffer_types >> i) & 1) && (m.properties.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

} // namespace

// allocate_memory: one whole VkDeviceMemory with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, not dedicated, not mapped.
// gpu_va is the device address of a probe buffer bound at offset 0, the stub's stand-in for the completed GPU
// virtual address mapping of a runtime allocation.
HRESULT APIENTRY stub_allocate(void* shell, const engine_ddi::MemoryRequest* request, engine_ddi::ImportedMemory* out) {
    StubMemory& m = stub_of(shell);
    // BD-075: the shell's answer when the runtime refused the ordinary allocation shape of a shared resource.
    // Nothing is allocated and the request is not counted: the create retries with the surface shape.
    if (m.refuse_shareable && (request->flags & engine_ddi::kMemoryShareable) &&
        !(request->flags & engine_ddi::kMemoryLinearSurface)) {
        m.last_flags = request->flags;
        ++m.share_required;
        return engine_ddi::kShareRequired;
    }
    ++m.allocations;
    m.last_byte_size = request->byte_size;
    m.last_flags = request->flags;
    m.last_type_bits = request->memory_type_bits;
    m.last_alignment = request->alignment;
    m.last_row_pitch = request->surface_row_pitch;
    m.last_layout_size = request->surface_layout_size;
    m.dedicated += (request->flags & engine_ddi::kMemoryDedicated) ? 1u : 0u;
    VkBufferCreateInfo probe_info{};
    probe_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    probe_info.size = request->byte_size;
    probe_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    probe_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer probe = VK_NULL_HANDLE;
    if (m.create_buffer(m.device, &probe_info, nullptr, &probe) != VK_SUCCESS) return E_OUTOFMEMORY;
    VkMemoryRequirements needs{};
    m.requirements(m.device, probe, &needs);
    // A request that names memory types (the linear surface) gets one of them.
    const uint32_t allowed = request->memory_type_bits ? request->memory_type_bits : UINT32_MAX;
    const uint32_t type = pick_type(m, needs.memoryTypeBits & allowed, request->heap->CPUPageProperty);

    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext = &flags;
    info.allocationSize = std::max<VkDeviceSize>(request->byte_size, needs.size);
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress va = 0;
    if (type != UINT32_MAX && m.allocate(m.device, &info, nullptr, &memory) == VK_SUCCESS &&
        m.bind(m.device, probe, memory, 0) == VK_SUCCESS) {
        VkBufferDeviceAddressInfo address{};
        address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address.buffer = probe;
        va = m.address(m.device, &address);
    }
    m.destroy_buffer(m.device, probe, nullptr);
    if (memory == VK_NULL_HANDLE) return E_OUTOFMEMORY;
    *out = engine_ddi::ImportedMemory{};
    out->size = sizeof(*out);
    out->memory_type_index = type;
    out->memory = memory;
    out->byte_size = info.allocationSize;
    out->allocation = m.next_allocation++;
    out->gpu_va = va;
    out->cookie = &m;
    m.last_allocation = out->allocation;
    return S_OK;
}

// BD-075: adopt_memory. The real shell maps the handle the runtime opened and imports those pages; a stub with no
// kernel allocation cannot, so it allocates a stand-in of the size and type the request names and reports the
// request's own handle. What this exercises is the open path's arithmetic and its release: no allocate callback, the
// handle the runtime gave, one free at the destroy.
HRESULT APIENTRY stub_adopt(void* shell, const engine_ddi::AdoptRequest* request, engine_ddi::ImportedMemory* out) {
    StubMemory& m = stub_of(shell);
    m.last_adopt_handle = request->allocation;
    m.last_adopt_flags = request->flags;
    m.last_adopt_byte_size = request->byte_size;
    m.last_adopt_alignment = request->alignment;
    m.last_adopt_type_bits = request->memory_type_bits;
    if (m.refuse_adopt) {
        ++m.adopt_refusals;
        return E_OUTOFMEMORY;
    }
    ++m.adoptions;
    VkBufferCreateInfo probe_info{};
    probe_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    probe_info.size = request->byte_size;
    probe_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    probe_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer probe = VK_NULL_HANDLE;
    if (m.create_buffer(m.device, &probe_info, nullptr, &probe) != VK_SUCCESS) return E_OUTOFMEMORY;
    VkMemoryRequirements needs{};
    m.requirements(m.device, probe, &needs);
    const uint32_t type = pick_type(m, needs.memoryTypeBits & request->memory_type_bits,
                                   D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext = &flags;
    info.allocationSize = std::max<VkDeviceSize>(request->byte_size, needs.size);
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress va = 0;
    if (type != UINT32_MAX && m.allocate(m.device, &info, nullptr, &memory) == VK_SUCCESS &&
        m.bind(m.device, probe, memory, 0) == VK_SUCCESS) {
        VkBufferDeviceAddressInfo address{};
        address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address.buffer = probe;
        va = m.address(m.device, &address);
    }
    m.destroy_buffer(m.device, probe, nullptr);
    if (memory == VK_NULL_HANDLE) return E_OUTOFMEMORY;
    *out = engine_ddi::ImportedMemory{};
    out->size = sizeof(*out);
    out->memory_type_index = type;
    out->memory = memory;
    out->byte_size = info.allocationSize;
    out->allocation = request->allocation;      // borrowed: never a handle of the shell's own
    out->gpu_va = va;
    out->cookie = &m;
    m.last_allocation = out->allocation;
    return S_OK;
}

HRESULT APIENTRY stub_free(void* shell, const engine_ddi::ImportedMemory* memory) {
    StubMemory& m = stub_of(shell);
    m.free(m.device, memory->memory, nullptr);
    ++m.frees;
    return S_OK;
}

namespace {
struct Observed {
    uint32_t with_memory = 0;
    uint32_t freed_ok = 0;
};
void observe(void* user, const engine_ddi::ReleaseEvent* event) {
    auto* o = static_cast<Observed*>(user);
    o->with_memory += event->had_memory ? 1u : 0u;
    o->freed_ok += (event->had_memory && event->free_result == S_OK) ? 1u : 0u;
}

} // namespace

bool load_stub(Env& env, StubMemory& m) {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0;
    const HRESULT hr = env.funcs.GetVulkanHandles(env.engine, &instance, &physical, &m.device, &family);
    if (hr != S_OK || !instance || !physical || !m.device) return false;
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        env.gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(env.gipa(instance, "vkGetDeviceProcAddr"));
    if (!properties || !gdpa) return false;
    properties(physical, &m.properties);
    m.allocate = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(m.device, "vkAllocateMemory"));
    m.free = reinterpret_cast<PFN_vkFreeMemory>(gdpa(m.device, "vkFreeMemory"));
    m.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(gdpa(m.device, "vkCreateBuffer"));
    m.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(gdpa(m.device, "vkDestroyBuffer"));
    m.requirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(gdpa(m.device, "vkGetBufferMemoryRequirements"));
    m.bind = reinterpret_cast<PFN_vkBindBufferMemory>(gdpa(m.device, "vkBindBufferMemory"));
    m.address = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(gdpa(m.device, "vkGetBufferDeviceAddress"));
    return m.allocate && m.free && m.create_buffer && m.destroy_buffer && m.requirements && m.bind && m.address;
}

namespace {
constexpr UINT32 pattern(UINT i) { return 0x85ebca6bu * (i + 7); }
} // namespace

void test_runtime_backed(Env& env) {
    constexpr UINT kWords = 16384;
    constexpr UINT64 kBytes = kWords * sizeof(UINT32);     // 64 KiB, the placement alignment of a buffer

    StubMemory m;
    check(load_stub(env, m), "runtime-backed: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    checkf(hr == S_OK && device.context, "runtime-backed: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    Observed observed;
    engine_ddi::harness_set_release_observer(device.context, observe, &observed);

    Buffer pool, src, gpu, dst;
    const HRESULT hr_pool = create_buffer(env, device, HeapKind::Upload, 2 * kBytes, false, pool);
    const HRESULT hr_src = hr_pool == S_OK ? create_placed_buffer(env, device, pool, kBytes, kBytes, src) : E_ABORT;
    const HRESULT hr_gpu = create_buffer(env, device, HeapKind::Default, kBytes, false, gpu);
    const HRESULT hr_dst = create_buffer(env, device, HeapKind::Readback, kBytes, false, dst);
    checkf(hr_pool == S_OK && hr_src == S_OK && hr_gpu == S_OK && hr_dst == S_OK && m.allocations == 3 &&
               m.dedicated == 3,
           "runtime-backed: committed UPLOAD (128 KiB), placed buffer in it at 64 KiB, committed DEFAULT and READBACK, "
           "each committed one on one allocate_memory (hr %08lx %08lx %08lx %08lx, %u allocations)",
           static_cast<unsigned long>(hr_pool), static_cast<unsigned long>(hr_src), static_cast<unsigned long>(hr_gpu),
           static_cast<unsigned long>(hr_dst), m.allocations);
    if (hr_pool != S_OK || hr_src != S_OK || hr_gpu != S_OK || hr_dst != S_OK) return;

    // A heap ByteSize of UINT64_MAX with a resource: the heap gets the size the resource needs, and neither the
    // memory request nor the engine sees UINT64_MAX. Without a resource, and for a heap too small, the create is
    // refused before any memory request - as E_OUTOFMEMORY, the one code a creation function may report for a
    // refusal of its own (BD-075, engine-ddi.h admitted_create_failure); the real E_INVALIDARG stays in the log.
    {
        Buffer sized, alone, tight;
        const uint32_t before = m.allocations;
        const HRESULT hr_sized = create_buffer_in_heap_of(env, device, HeapKind::Upload, 4096, UINT64_MAX, sized);
        const uint64_t asked = m.last_byte_size;
        const uint32_t after = m.allocations;
        void* cpu = nullptr;
        const HRESULT hr_map = hr_sized == S_OK ? env.core.pfnMapHeap(device.h(), sized.hheap(), &cpu) : E_ABORT;
        if (hr_map == S_OK) env.core.pfnUnmapHeap(device.h(), sized.hheap());
        checkf(hr_sized == S_OK && after == before + 1 && asked == kBytes && hr_map == S_OK && cpu,
               "runtime-backed: committed UPLOAD buffer of 4096 bytes with heap ByteSize UINT64_MAX: one memory "
               "request of 64 KiB, heap maps (hr %08lx, map %08lx, %llu bytes asked)",
               static_cast<unsigned long>(hr_sized), static_cast<unsigned long>(hr_map),
               static_cast<unsigned long long>(asked));
        if (hr_sized == S_OK) destroy_buffer(env, device, sized);
        const HRESULT hr_alone = create_heap_alone(env, device, HeapKind::Upload, UINT64_MAX, alone);
        const HRESULT hr_small = create_buffer_in_heap_of(env, device, HeapKind::Upload, 4 * kBytes, kBytes, tight);
        checkf(hr_alone == E_OUTOFMEMORY && hr_small == E_OUTOFMEMORY && m.allocations == after,
               "runtime-backed: a heap alone with ByteSize UINT64_MAX and a heap smaller than its resource: "
               "refused with no memory request (hr %08lx %08lx)",
               static_cast<unsigned long>(hr_alone), static_cast<unsigned long>(hr_small));
    }
    const D3D12DDI_GPU_VIRTUAL_ADDRESS pool_va = env.core.pfnCheckResourceVirtualAddress(device.h(), pool.hres());
    const D3D12DDI_GPU_VIRTUAL_ADDRESS src_va = env.core.pfnCheckResourceVirtualAddress(device.h(), src.hres());
    checkf(pool_va && src_va == pool_va + kBytes,
           "runtime-backed: the placed buffer's GPU VA is its heap's plus 64 KiB (%llx, %llx)",
           static_cast<unsigned long long>(pool_va), static_cast<unsigned long long>(src_va));

    // Residency lookup, as MakeResident will resolve its object list: the committed buffer and its heap, the placed
    // buffer in that heap (the same allocation), and another committed buffer (its own).
    auto lookup = [&](void* handle, D3D12DDI_HANDLETYPE type, D3DKMT_HANDLE* out) {
        return engine_ddi::object_allocation(device.context, D3D12DDI_HANDLE_AND_TYPE{handle, type}, out);
    };
    D3DKMT_HANDLE committed = 0, heap = 0, placed = 0, other = 0;
    const HRESULT hr_c = lookup(pool.resource, D3D12DDI_HT_0012_RESOURCE, &committed);
    const HRESULT hr_h = lookup(pool.heap, D3D12DDI_HT_HEAP, &heap);
    const HRESULT hr_p = lookup(src.resource, D3D12DDI_HT_0012_RESOURCE, &placed);
    const HRESULT hr_o = lookup(gpu.resource, D3D12DDI_HT_0012_RESOURCE, &other);
    checkf(hr_c == S_OK && hr_h == S_OK && hr_p == S_OK && hr_o == S_OK && committed && heap == committed &&
               placed == committed && other && other != committed,
           "runtime-backed: object_allocation gives the committed buffer, its heap and the placed buffer in it one "
           "allocation, another committed buffer its own (%x %x %x %x)",
           committed, heap, placed, other);

    // The placed buffer's words through MapHeap of its heap, at heap offset 64 KiB; the first 64 KiB get other
    // words, so a copy from the wrong offset cannot match.
    void* cpu = nullptr;
    hr = env.core.pfnMapHeap(device.h(), pool.hheap(), &cpu);
    checkf(hr == S_OK && cpu, "runtime-backed: MapHeap of the imported UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK || !cpu) return;
    auto* words = static_cast<UINT32*>(cpu);
    for (UINT i = 0; i < kWords; ++i) {
        words[i] = ~pattern(i);
        words[kWords + i] = pattern(i);
    }
    env.core.pfnUnmapHeap(device.h(), pool.hheap());

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "runtime-backed: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    Recording rec;
    hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    checkf(hr == S_OK, "runtime-backed: pool, recorder and DIRECT list (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr == S_OK) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        D3D12DDIARG_BUFFER_PLACEMENT to{}, from{};
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_dest =
            transition(gpu, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_dest);
        to.BaseAddress.UMD = {gpu.hres(), 0};
        from.BaseAddress.UMD = {src.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), to, from, kBytes);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(gpu, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
        to.BaseAddress.UMD = {dst.hres(), 0};
        from.BaseAddress.UMD = {gpu.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), to, from, kBytes);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors,
               "runtime-backed: placed -> DEFAULT -> READBACK copies executed (hr %08lx)", static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "runtime-backed");

        hr = env.core.pfnMapHeap(device.h(), dst.hheap(), &cpu);
        checkf(hr == S_OK && cpu, "runtime-backed: MapHeap of the imported READBACK heap (hr %08lx)",
               static_cast<unsigned long>(hr));
        if (hr == S_OK && cpu) {
            words = static_cast<UINT32*>(cpu);
            UINT bad = 0;
            for (UINT i = 0; i < kWords; ++i) bad += words[i] != pattern(i) ? 1u : 0u;
            checkf(bad == 0, "runtime-backed: readback matches the placed buffer's words exactly (%u of %u differ)", bad,
                   kWords);
            env.core.pfnUnmapHeap(device.h(), dst.hheap());
        }
    }
    destroy_recording(env, device, rec);

    // The work has retired: each heap's release runs at its last destroy, engine heap first, then free_memory.
    destroy_buffer(env, device, src);
    destroy_buffer(env, device, pool);
    destroy_buffer(env, device, gpu);
    destroy_buffer(env, device, dst);
    // Four allocations: the three buffers above and the one whose heap size was left to the resource.
    checkf(m.frees == 4 && observed.with_memory == 4 && observed.freed_ok == 4,
           "runtime-backed: each allocation came back through free_memory once, after its engine heap (%u freed, "
           "%u releases)",
           m.frees, observed.with_memory);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "runtime-backed: destroy_engine_queue reports Retired");
    engine_ddi::harness_set_release_observer(device.context, nullptr, nullptr);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && !device.shell.device_errors && !device.shell.list_errors,
           "runtime-backed: destroy_device_context S_OK with no live object, no error reported (hr %08lx, %u live, "
           "%u device errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors);
}

} // namespace harness
