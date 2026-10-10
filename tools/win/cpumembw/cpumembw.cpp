#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <functional>
#include <intrin.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static volatile uint64_t sink;
static void fail(const char *s)
{
    throw std::runtime_error(s);
}
static void require(bool b, const char *s)
{
    if (!b)
        fail(s);
}
static double seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}
struct Options
{
    std::string api = "cpu", heap = "default", type = "all", luid;
    uint32_t adapter = 0;
    size_t bytes = 64u * 1024u * 1024u, chunk = 256u * 1024u;
    double duration = .25;
    DWORD timeout = 150000;
    bool child = false, selftest = false, stall = false, corrupt = false;
};
static uint64_t integer(const std::string &s)
{
    require(!s.empty() && s[0] != '-', "unsigned integer required");
    char *end = nullptr;
    errno = 0;
    auto n = strtoull(s.c_str(), &end, 0);
    require(!errno && end && !*end, "invalid integer");
    return n;
}
static Options options(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string k = argv[i];
        if (k == "--child")
        {
            o.child = true;
            continue;
        }
        if (k == "--self-test")
        {
            o.selftest = true;
            continue;
        }
        if (k == "--test-corrupt")
        {
            o.corrupt = true;
            continue;
        }
        if (k == "--test-stall")
        {
            o.stall = true;
            continue;
        }
        require(i + 1 < argc, "option needs value");
        std::string v = argv[++i];
        if (k == "--api")
            o.api = v;
        else if (k == "--heap")
            o.heap = v;
        else if (k == "--type")
            o.type = v;
        else if (k == "--luid")
            o.luid = v;
        else if (k == "--adapter")
        {
            auto n = integer(v);
            require(n <= UINT32_MAX, "adapter overflow");
            o.adapter = (uint32_t)n;
        }
        else if (k == "--chunk-bytes")
        {
            auto n = integer(v);
            require(n >= 4096 && n <= 1048576 && n % 8 == 0, "chunk must be4096..1048576 divisible by8");
            o.chunk = (size_t)n;
        }
        else if (k == "--bytes")
        {
            auto n = integer(v);
            require(n >= 4096 && n <= 256ull * 1024 * 1024, "bytes must be 4096..268435456");
            o.bytes = (size_t)n;
        }
        else if (k == "--duration")
        {
            char *e = nullptr;
            o.duration = strtod(v.c_str(), &e);
            require(e && !*e && o.duration >= .01 && o.duration <= 2, "duration must be .01..2 seconds");
        }
        else if (k == "--timeout-ms")
        {
            auto n = integer(v);
            require(n >= 100 && n <= 150000, "timeout must be100..150000ms");
            o.timeout = (DWORD)n;
        }
        else
            fail("unknown option");
    }
    require(o.api == "cpu" || o.api == "vulkan" || o.api == "d3d12", "api must be cpu, vulkan or d3d12");
    require(o.heap == "default" || o.heap == "unified", "heap must be default or unified");
    require(!o.corrupt || o.api == "cpu", "corruption test is CPU-only");
    require(o.bytes % 8 == 0, "bytes must be divisible by8");
    if (o.type != "all")
        require(integer(o.type) < 32, "type index must be below32");
    if (!o.luid.empty())
        require(o.luid.size() == 16 && o.luid.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos,
                "LUID requires16 hex digits high then low");
    return o;
}
static std::string luid_text(const void *data)
{
    uint32_t words[2];
    memcpy(words, data, 8);
    char b[20];
    sprintf_s(b, "%08x%08x", words[1], words[0]);
    return b;
}
static std::string quoted(const char *s)
{
    std::string r = "\"";
    for (; *s; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            r += '\\';
        if (c >= 32)
            r += (char)c;
    }
    return r + '"';
}
static uint64_t pattern(size_t i)
{
    return 0x51a729bc8d03e6f4ull ^ ((uint64_t)i * 0x9e3779b97f4a7c15ull);
}
static uint64_t read_words(const void *p, size_t n)
{
    const volatile uint64_t *q = (const volatile uint64_t *)p;
    uint64_t value = 0;
    for (size_t i = 0; i < n / 8; ++i)
        value += q[i];
    return value;
}
static void write_words(void *p, size_t n)
{
    volatile uint64_t *q = (volatile uint64_t *)p;
    for (size_t i = 0; i < n / 8; ++i)
        q[i] = pattern(i);
}
static bool validate(const void *p, size_t n)
{
    const volatile uint64_t *q = (const volatile uint64_t *)p;
    for (size_t i = 0; i < n / 8; ++i)
        if (q[i] != pattern(i))
            return false;
    return true;
}
static void stage(const char *name, int type)
{
    printf("{\"event\":\"stage\",\"name\":%s,\"type\":%d}\n", quoted(name).c_str(), type);
    fflush(stdout);
}
static size_t read_limit(size_t bytes, double pilot)
{
    // Limit each read sweep to about20ms. This is an adaptive size, not a driver-call timeout.
    const double estimate = 4096.0 * .020 / std::max(pilot, .000000001);
    return std::min(bytes, std::max<size_t>(4096, (size_t)std::min(estimate, (double)bytes))) & ~size_t(7);
}
static void measure(void *p, const Options &o, int type, const std::function<void()> &flush,
                    const std::function<void()> &invalidate, Clock::time_point deadline)
{
    stage("cpu-pilot", type);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)))
        printf("{\"event\":\"virtual_query\",\"type\":%d,\"protect\":%lu,\"allocation_protect\":%lu,\"region_bytes\":%"
               "zu}\n",
               type, (unsigned long)mbi.Protect, (unsigned long)mbi.AllocationProtect, (size_t)mbi.RegionSize);
    write_words(p, 4096);
    _mm_sfence();
    flush();
    invalidate();
    auto t0 = Clock::now();
    sink = read_words(p, 4096);
    double pilot = seconds(t0, Clock::now());
    size_t rd = read_limit(o.bytes, pilot);
    printf("{\"event\":\"pilot\",\"type\":%d,\"seconds\":%.9f,\"read_bytes\":%zu,\"allocation_bytes\":%zu}\n", type,
           pilot, rd, o.bytes);
    std::vector<uint64_t> source(o.bytes / 8);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = pattern(i);
    std::vector<uint64_t> readback(rd / 8);
    for (int op = 0; op < 4; ++op)
    {
        if (Clock::now() >= deadline)
            fail("child budget exhausted before phase");
        const char *name = op == 0   ? "sequential-u64-write"
                           : op == 1 ? "memcpy-upload"
                           : op == 2 ? "volatile-u64-read"
                                     : "memcpy-readback";
        stage(name, type);
        if (op >= 2)
            invalidate();
        auto start = Clock::now();
        auto until = std::min(
            deadline, start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration)));
        uint64_t total = 0, checksum = 0;
        size_t offset = 0, span = op >= 2 ? rd : o.bytes;
        do
        {
            size_t chunk = std::min(o.chunk, span - offset);
            if (op == 0)
            {
                auto *q = (volatile uint64_t *)((char *)p + offset);
                for (size_t j = 0; j < chunk / 8; ++j)
                    q[j] = pattern(offset / 8 + j);
            }
            else if (op == 1)
                memcpy((char *)p + offset, (const char *)source.data() + offset, chunk);
            else if (op == 2)
                checksum += read_words((char *)p + offset, chunk);
            else
                memcpy((char *)readback.data() + offset, (char *)p + offset, chunk);
            total += chunk;
            offset += chunk;
            if (offset == span)
                offset = 0;
        } while (Clock::now() < until);
        if (op < 2)
            _mm_sfence(); // Include draining WC stores, exclude API cache maintenance.
        double elapsed = seconds(start, Clock::now());
        sink = checksum;
        if (op == 2)
        {
            uint64_t full = 0, tail = 0;
            size_t tailWords = (size_t)(total % span) / 8;
            for (size_t j = 0; j < span / 8; ++j)
            {
                full += pattern(j);
                if (j < tailWords)
                    tail += pattern(j);
            }
            require(checksum == full * (total / span) + tail, "timed read checksum mismatch");
        }
        if (op == 3)
            require(validate(readback.data(), (size_t)std::min<uint64_t>(total, span)), "memcpy readback mismatch");
        if (op < 2)
        {
            flush();
            invalidate();
            // A short phase may have written only a prefix. Validate exactly initialized bytes.
            size_t initialized = (size_t)std::min<uint64_t>(total, span);
            size_t checked = std::min(rd, initialized);
            if (o.corrupt && op == 0 && checked)
                ((volatile uint64_t *)p)[checked / 8 - 1] ^= 1;
            require(validate(p, checked), "content mismatch");
            printf("{\"event\":\"validation\",\"type\":%d,\"operation\":%s,\"checked_bytes\":%zu,\"initialized_bytes\":"
                   "%zu,\"status\":\"pass\"}\n",
                   type, quoted(name).c_str(), checked, initialized);
            // Before CPU-read phase, initialize its full measured prefix outside timings.
            if (op == 1)
            {
                write_words(p, rd);
                _mm_sfence();
                flush();
                invalidate();
                require(validate(p, rd), "read prefix mismatch");
            }
        }
        printf("{\"event\":\"measurement\",\"type\":%d,\"operation\":%s,\"bytes\":%llu,\"seconds\":%.9f,\"GBps_"
               "decimal\":%.6f,\"span_bytes\":%zu,\"checksum\":%llu}\n",
               type, quoted(name).c_str(), (unsigned long long)total, elapsed, (double)total / elapsed / 1e9, span,
               (unsigned long long)checksum);
        fflush(stdout);
    }
}
struct Module
{
    HMODULE h;
    explicit Module(const wchar_t *name) : h(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        require(h != nullptr, "system runtime load failed");
    }
    ~Module()
    {
        FreeLibrary(h);
    }
    template <class T> T get(const char *name)
    {
        auto p = GetProcAddress(h, name);
        require(p != nullptr, "runtime export missing");
        return reinterpret_cast<T>(p);
    }
};
static void latency(const char *name, int type, Clock::time_point begin)
{
    printf("{\"event\":\"api_latency\",\"name\":%s,\"type\":%d,\"seconds\":%.9f}\n", quoted(name).c_str(), type,
           seconds(begin, Clock::now()));
}
struct OnExit
{
    std::function<void()> action;
    ~OnExit()
    {
        action();
    }
};
static void vkcheck(VkResult r, const char *name)
{
    if (r != VK_SUCCESS)
    {
        fprintf(stderr, "%s VkResult=%d\n", name, r);
        fail("Vulkan call failed");
    }
}
static void vulkan(const Options &o, Clock::time_point deadline)
{
    Module loader(L"vulkan-1.dll");
    auto gipa = loader.get<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    auto create = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    require(create != nullptr, "vkCreateInstance unavailable");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "cpumembw";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VkInstance instance{};
    stage("vkCreateInstance", -1);
    vkcheck(create(&ci, nullptr, &instance), "vkCreateInstance");
#define IFN(n)                                                                                                         \
    auto n = (PFN_##n)gipa(instance, #n);                                                                              \
    require(n != nullptr, #n)
    IFN(vkDestroyInstance);
    OnExit instanceCleanup{[&]() { vkDestroyInstance(instance, nullptr); }};
    IFN(vkEnumeratePhysicalDevices);
    IFN(vkGetPhysicalDeviceProperties2);
    IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkCreateDevice);
    IFN(vkGetDeviceProcAddr);
    uint32_t n = 0;
    vkcheck(vkEnumeratePhysicalDevices(instance, &n, nullptr), "enumerate count");
    std::vector<VkPhysicalDevice> devices(n);
    vkcheck(vkEnumeratePhysicalDevices(instance, &n, devices.data()), "enumerate");
    require(o.adapter < n, "Vulkan adapter index absent");
    auto phy = devices[o.adapter];
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(phy, &props);
    auto luid = id.deviceLUIDValid ? luid_text(id.deviceLUID) : std::string("unavailable");
    if (!o.luid.empty())
        require(_stricmp(o.luid.c_str(), luid.c_str()) == 0, "Vulkan LUID mismatch");
    printf("{\"event\":\"device\",\"api\":\"vulkan\",\"adapter\":%u,\"name\":%s,\"vendor\":%u,\"device\":%u,\"driver_"
           "version\":%u,\"api_version\":%u,\"luid\":%s,\"noncoherent_atom_bytes\":%llu}\n",
           o.adapter, quoted(props.properties.deviceName).c_str(), props.properties.vendorID, props.properties.deviceID,
           props.properties.driverVersion, props.properties.apiVersion, quoted(luid.c_str()).c_str(),
           (unsigned long long)props.properties.limits.nonCoherentAtomSize);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(phy, &memory);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phy, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phy, &qn, qs.data());
    uint32_t q = 0;
    while (q < qn && !qs[q].queueCount)
        ++q;
    require(q < qn, "no Vulkan queue family");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = q;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VkDevice dev{};
    stage("vkCreateDevice", -1);
    vkcheck(vkCreateDevice(phy, &di, nullptr, &dev), "vkCreateDevice");
#define DFN(n)                                                                                                         \
    auto n = (PFN_##n)vkGetDeviceProcAddr(dev, #n);                                                                    \
    require(n != nullptr, #n)
    DFN(vkDestroyDevice);
    OnExit deviceCleanup{[&]() { vkDestroyDevice(dev, nullptr); }};
    DFN(vkCreateBuffer);
    DFN(vkDestroyBuffer);
    DFN(vkGetBufferMemoryRequirements);
    DFN(vkAllocateMemory);
    DFN(vkFreeMemory);
    DFN(vkBindBufferMemory);
    DFN(vkMapMemory);
    DFN(vkUnmapMemory);
    DFN(vkFlushMappedMemoryRanges);
    DFN(vkInvalidateMappedMemoryRanges);
    unsigned measured = 0;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    {
        auto flags = memory.memoryTypes[i].propertyFlags;
        auto heap = memory.memoryTypes[i].heapIndex;
        printf(
            "{\"event\":\"memory_type\",\"type\":%u,\"flags\":%u,\"heap\":%u,\"heap_flags\":%u,\"heap_bytes\":%llu}\n",
            i, flags, heap, memory.memoryHeaps[heap].flags, (unsigned long long)memory.memoryHeaps[heap].size);
        if (o.type != "all" && integer(o.type) != i)
            continue;
        if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)
        {
            printf("{\"event\":\"skip\",\"type\":%u,\"reason\":\"deviceCoherentMemory-feature-not-enabled\"}\n", i);
            continue;
        }
        if (Clock::now() >= deadline)
            fail("child budget exhausted");
        stage("vkCreateBuffer", (int)i);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = o.bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer{};
        vkcheck(vkCreateBuffer(dev, &bi, nullptr, &buffer), "vkCreateBuffer");
        VkDeviceMemory mem{};
        bool mapped = false;
        OnExit allocationCleanup{[&]() {
            if (mapped)
                vkUnmapMemory(dev, mem);
            vkDestroyBuffer(dev, buffer, nullptr);
            if (mem)
                vkFreeMemory(dev, mem, nullptr);
        }};
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, buffer, &req);
        if (!(req.memoryTypeBits & (1u << i)))
        {
            printf("{\"event\":\"skip\",\"type\":%u,\"reason\":\"buffer-incompatible\"}\n", i);
            continue;
        }
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = req.size;
        ma.memoryTypeIndex = i;
        stage("vkAllocateMemory", (int)i);
        auto allocStart = Clock::now();
        vkcheck(vkAllocateMemory(dev, &ma, nullptr, &mem), "vkAllocateMemory");
        latency("vkAllocateMemory", (int)i, allocStart);
        vkcheck(vkBindBufferMemory(dev, buffer, mem, 0), "vkBindBufferMemory");
        void *p = nullptr;
        stage("vkMapMemory", (int)i);
        auto mapStart = Clock::now();
        vkcheck(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
        mapped = true;
        latency("vkMapMemory", (int)i, mapStart);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = mem;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        measure(
            p, o, (int)i,
            [&]() {
                if (!coherent)
                    vkcheck(vkFlushMappedMemoryRanges(dev, 1, &range), "flush");
            },
            [&]() {
                if (!coherent)
                    vkcheck(vkInvalidateMappedMemoryRanges(dev, 1, &range), "invalidate");
            },
            deadline);
        stage("vkCleanup", (int)i);
        ++measured;
    }
    require(measured != 0, "no selected compatible host-visible memory type");
    stage("vkDestroyDevice", -1);
}
static void hr(HRESULT h, const char *operation)
{
    if (FAILED(h))
    {
        fprintf(stderr, "%s HRESULT=0x%08lx\n", operation, (unsigned long)h);
        fail("D3D12 call failed");
    }
}
static void d3d12(const Options &o, Clock::time_point deadline)
{
    Module dxgi(L"dxgi.dll"), d3d(L"d3d12.dll");
    auto factoryFn = dxgi.get<decltype(&CreateDXGIFactory1)>("CreateDXGIFactory1");
    auto deviceFn = d3d.get<decltype(&D3D12CreateDevice)>("D3D12CreateDevice");
    ComPtr<IDXGIFactory1> factory;
    hr(factoryFn(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    hr(factory->EnumAdapters1(o.adapter, &adapter), "EnumAdapters1");
    DXGI_ADAPTER_DESC1 desc{};
    hr(adapter->GetDesc1(&desc), "GetDesc1");
    auto luid = luid_text(&desc.AdapterLuid);
    if (!o.luid.empty())
        require(_stricmp(luid.c_str(), o.luid.c_str()) == 0, "DXGI LUID mismatch");
    char name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    printf(
        "{\"event\":\"device\",\"api\":\"d3d12\",\"adapter\":%u,\"name\":%s,\"vendor\":%u,\"device\":%u,\"luid\":%s}\n",
        o.adapter, quoted(name).c_str(), desc.VendorId, desc.DeviceId, quoted(luid.c_str()).c_str());
    ComPtr<ID3D12Device> device;
    stage("D3D12CreateDevice", -1);
    hr(deviceFn(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    require(o.type == "all" || integer(o.type) < 2, "D3D12 type must be0(upload),1(readback),or all");
    for (unsigned i = 0; i < 2; ++i)
    {
        if (o.type != "all" && integer(o.type) != i)
            continue;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = i ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC r{};
        r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        r.Width = o.bytes;
        r.Height = 1;
        r.DepthOrArraySize = 1;
        r.MipLevels = 1;
        r.SampleDesc.Count = 1;
        r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto custom = device->GetCustomHeapProperties(0, heap.Type);
        printf("{\"event\":\"memory_type\",\"type\":%u,\"heap_name\":\"%s\",\"cpu_page_property\":%u,\"memory_pool\":%"
               "u}\n",
               i, i ? "READBACK" : "UPLOAD", (unsigned)custom.CPUPageProperty, (unsigned)custom.MemoryPoolPreference);
        ComPtr<ID3D12Resource> resource;
        stage("CreateCommittedResource", (int)i);
        auto allocStart = Clock::now();
        hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &r,
                                           i ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr, IID_PPV_ARGS(&resource)),
           "CreateCommittedResource");
        latency("CreateCommittedResource", (int)i, allocStart);
        void *p = nullptr;
        D3D12_RANGE read{0, o.bytes};
        stage("Map", (int)i);
        auto mapStart = Clock::now();
        hr(resource->Map(0, &read, &p), "Map");
        latency("Map", (int)i, mapStart);
        measure(p, o, (int)i, []() {}, []() {}, deadline);
        D3D12_RANGE written{0, o.bytes};
        stage("Unmap", (int)i);
        resource->Unmap(0, &written);
    }
}
static int child(const Options &o)
{
    if (o.stall)
    {
        stage("selftest-stall", -1);
        Sleep(INFINITE);
    }
    require(SetEnvironmentVariableA("radv_enable_unified_heap_on_apu", o.heap == "unified" ? "true" : "false") != 0,
            "heap environment failed");
    printf("{\"event\":\"start\",\"api\":%s,\"heap_policy\":%s,\"bytes\":%zu,\"phase_seconds\":%.3f}\n",
           quoted(o.api.c_str()).c_str(), quoted(o.heap.c_str()).c_str(), o.bytes, o.duration);
    printf("{\"event\":\"method\",\"chunk_bytes\":%zu,\"timing_clock\":\"steady_clock\",\"gpu_commands\":false}\n",
           o.chunk);
    fflush(stdout);
    const DWORD cleanup = std::min<DWORD>(5000, o.timeout / 10);
    auto deadline = Clock::now() + std::chrono::milliseconds(o.timeout - cleanup);
    if (o.api == "vulkan")
        vulkan(o, deadline);
    else if (o.api == "d3d12")
        d3d12(o, deadline);
    else
    {
        std::vector<uint64_t> data(o.bytes / 8);
        measure(data.data(), o, 0, []() {}, []() {}, deadline);
    }
    puts("{\"event\":\"complete\",\"status\":\"pass\"}");
    return 0;
}
struct Handle
{
    HANDLE h = nullptr;
    ~Handle()
    {
        if (h)
            CloseHandle(h);
    }
};
static int supervise(int argc, char **argv, const Options &o)
{
    wchar_t path[32768];
    require(GetModuleFileNameW(nullptr, path, 32768) > 0, "executable path failed");
    std::wstring cmd = L"\"" + std::wstring(path) + L"\" --child";
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        require(arg.find_first_of("\"\\\r\n") == std::string::npos, "invalid argument character");
        cmd += L" \"";
        cmd += std::wstring(arg.begin(), arg.end());
        cmd += L"\"";
    }
    Handle job;
    job.h = CreateJobObjectW(nullptr, nullptr);
    require(job.h != nullptr, "CreateJobObject failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) != 0,
            "job limit failed");
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    auto launch = Clock::now();
    require(CreateProcessW(path, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi) != 0,
            "CreateProcess failed");
    Handle process;
    process.h = pi.hProcess;
    Handle thread;
    thread.h = pi.hThread;
    if (!AssignProcessToJobObject(job.h, process.h))
    {
        TerminateProcess(process.h, 124);
        fail("AssignProcessToJobObject failed");
    }
    require(ResumeThread(thread.h) != DWORD(-1), "ResumeThread failed");
    const DWORD cleanup = std::min<DWORD>(5000, o.timeout / 10);
    DWORD elapsed = (DWORD)(seconds(launch, Clock::now()) * 1000);
    DWORD budget = o.timeout - cleanup;
    DWORD wait = WaitForSingleObject(process.h, budget > elapsed ? budget - elapsed : 0);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateJobObject(job.h, 124);
        DWORD ended = WaitForSingleObject(process.h, cleanup);
        printf("{\"event\":\"watchdog\",\"status\":\"timeout\",\"child_exited\":%s}\n",
               ended == WAIT_OBJECT_0 ? "true" : "false");
        return 124;
    }
    DWORD code = 1;
    require(GetExitCodeProcess(process.h, &code) != 0, "exit status unavailable");
    return (int)code;
}
static int selftest()
{
    std::vector<uint64_t> v(1024);
    write_words(v.data(), v.size() * 8);
    require(validate(v.data(), v.size() * 8), "pattern positive failed");
    v[513] ^= 1;
    require(!validate(v.data(), v.size() * 8), "corruption not detected");
    require(read_limit(65536, 1) == 4096, "slow pilot must clamp");
    require(read_limit(65536, 1e-9) == 65536, "fast pilot must clamp");
    require(luid_text("\x01\0\0\0\x02\0\0\0") == "0000000200000001", "LUID order wrong");
    puts("self-test: pattern, corruption, adaptive bounds and LUID pass");
    return 0;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    try
    {
        auto o = options(argc, argv);
        if (o.selftest)
            return selftest();
        return o.child ? child(o) : supervise(argc, argv, o);
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "cpumembw: %s\n", e.what());
        return 2;
    }
}
