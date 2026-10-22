/*
 * vkmembw: GPU memory bandwidth through Vulkan compute, timed by GPU timestamps and checked on the CPU.
 *
 * Three kernels over buffers of --mib MiB with 16-byte elements and grid-stride loops (shaders/):
 *   write  src[i] = pattern(i)    bytes counted: size
 *   copy   dst[i] = src[i]        bytes counted: 2 * size
 *   read   acc[t] += src[i]       bytes counted: size + 16 per invocation
 * pattern(i) = (4i, 4i+1, 4i+2, 4i+3) ^ seed, one uint32 per lane.
 *
 * Each kernel runs --iters times in one command buffer: a full barrier, a bottom-of-pipe timestamp, the dispatch
 * and a second bottom-of-pipe timestamp. The command buffer's submit-to-fence time is also taken on the CPU as a
 * cross-check of the GPU clock. After timing, the host compares every source and destination word against a
 * fresh pattern and checks the read (the per-lane sum mod 2^32 of all per-invocation results against the sum
 * of the pattern, computed on the CPU). This checks the final batch output, not each timed dispatch.
 * A failed content check fails the run.
 *
 * --warmup-ms keeps the GPU busy with copies before the measurement, so a load-driven clock governor has raised
 * the clock by the time the timed dispatches run (0 measures from wherever the governor stands).
 * --negative-control runs the timed copy and read one element short; both checks must then fail (exit 1).
 *
 * Placements: "local" is a DEVICE_LOCAL memory type without HOST_VISIBLE when one exists; "host" is a
 * HOST_VISIBLE | HOST_COHERENT type without DEVICE_LOCAL. On a unified-memory part both can be the same DRAM,
 * reached by the GPU through different paths.
 *
 * vulkan-1.dll is loaded at run time, so the program needs no import library.
 * Exit: 0 every check passed, 1 a check failed, 2 usage, 3 Vulkan or system error.
 */
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

#include "write_spv.h"
#include "copy_spv.h"
#include "read_spv.h"

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
    X(vkCmdPipelineBarrier) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCmdCopyBuffer) X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkFreeCommandBuffers) X(vkCmdFillBuffer)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memProps;
static VkCommandPool pool;
static VkFence fence;
static double tsPeriodNs;
static uint64_t tsMask;
static LARGE_INTEGER qpcFreq;

static void Fail(const char *what, VkResult r)
{
    fprintf(stderr, "vkmembw: %s failed: VkResult %d\n", what, (int)r);
    exit(3);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static void Missing(const char *name)
{
    fprintf(stderr, "vkmembw: %s not available\n", name);
    exit(3);
}
#define LOAD_INSTANCE(f) if (!(f = (PFN_##f)getInstanceProc(instance, #f))) Missing(#f);
#define LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) Missing(#f);

static double NowMs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpcFreq.QuadPart;
}

typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize size; uint32_t type; } Buf;

static int FindType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return (int)i;
    }
    return -1;
}

enum { PLACE_LOCAL, PLACE_HOST, PLACE_READBACK };

static int PickType(uint32_t bits, int place)
{
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                hc = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ca = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    int t = -1;
    if (place == PLACE_LOCAL) {
        if ((t = FindType(bits, dl, hv)) < 0) t = FindType(bits, dl, 0);
    } else if (place == PLACE_HOST) {
        if ((t = FindType(bits, hv | hc, dl | ca)) < 0 && (t = FindType(bits, hv | hc, dl)) < 0) t = FindType(bits, hv, dl);
    } else {
        if ((t = FindType(bits, hv | hc | ca, 0)) < 0) t = FindType(bits, hv | hc, 0);
    }
    return t;
}

static Buf MakeBuf(VkDeviceSize size, int place)
{
    Buf b = { VK_NULL_HANDLE, VK_NULL_HANDLE, size, 0 };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK(vkCreateBuffer(dev, &bi, NULL, &b.buffer));
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    t = PickType(req.memoryTypeBits, place);
    if (t < 0) { fprintf(stderr, "vkmembw: no memory type for placement %d (bits 0x%X)\n", place, req.memoryTypeBits); exit(3); }
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    VK(vkAllocateMemory(dev, &ai, NULL, &b.memory));
    VK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
    b.type = (uint32_t)t;
    return b;
}

static void FreeBuf(Buf *b)
{
    if (b->buffer) vkDestroyBuffer(dev, b->buffer, NULL);
    if (b->memory) vkFreeMemory(dev, b->memory, NULL);
    b->buffer = VK_NULL_HANDLE; b->memory = VK_NULL_HANDLE;
}

static void BarrierAll(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

static void BarrierToHost(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static VkCommandBuffer Begin(void)
{
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkCommandBuffer cb;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &ai, &cb));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

// Submits, waits and frees the command buffer; returns the CPU time from just before vkQueueSubmit to the fence.
static double SubmitWait(VkCommandBuffer cb)
{
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    double t0, t1;
    VK(vkEndCommandBuffer(cb));
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    t0 = NowMs();
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    t1 = NowMs();
    VK(vkResetFences(dev, 1, &fence));
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    return t1 - t0;
}

typedef struct { VkPipelineLayout layout; VkPipeline pipeline; const char *name; } Kernel;
typedef struct { uint32_t n; uint32_t seed; } Push;

static void Dispatch(VkCommandBuffer cb, const Kernel *k, VkDescriptorSet set, const Push *pc, uint32_t groups)
{
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, k->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*pc), pc);
    vkCmdDispatch(cb, groups, 1, 1);
}

static int CompareDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// Runs one kernel `iters` times in one command buffer and prints its line.
static void Measure(const Kernel *k, VkDescriptorSet set, const Push *pc, uint32_t groups, uint32_t iters,
                    VkQueryPool qp, double bytes, int skip)
{
    VkCommandBuffer cb = Begin();
    uint64_t *ts = (uint64_t *)calloc(2 * (size_t)iters, sizeof(uint64_t));
    double *ms = (double *)calloc(iters, sizeof(double));
    double cpu, sum = 0;
    if (!ts || !ms) { fprintf(stderr, "vkmembw: out of host memory\n"); exit(3); }
    vkCmdResetQueryPool(cb, qp, 0, 2 * iters);
    for (uint32_t i = 0; i < iters; i++) {
        BarrierAll(cb);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * i);
        if (!skip) Dispatch(cb, k, set, pc, groups);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * i + 1);
    }
    cpu = SubmitWait(cb);
    VK(vkGetQueryPoolResults(dev, qp, 0, 2 * iters, 2 * (size_t)iters * sizeof(uint64_t), ts, sizeof(uint64_t),
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    for (uint32_t i = 0; i < iters; i++) {
        ms[i] = (double)((ts[2 * i + 1] - ts[2 * i]) & tsMask) * tsPeriodNs / 1e6;
        sum += ms[i];
    }
    qsort(ms, iters, sizeof(double), CompareDouble);
    {
        double med = ms[iters / 2], best = ms[0];
        printf("  %-6s n=%u gpu_ms min %.3f median %.3f max %.3f sum %.1f | GB/s median %.1f best %.1f | cpu_ms %.1f "
               "(%.3f per dispatch)\n", k->name, iters, best, med, ms[iters - 1], sum,
               bytes / (med * 1e-3) / 1e9, bytes / (best * 1e-3) / 1e9, cpu, cpu / iters);
    }
    free(ts);
    free(ms);
}

/* Full buffer readback, in bounded host-memory chunks, outside all timed intervals. */
static uint32_t CheckBuffer(const Buf *buffer, const Buf *rb, VkDeviceSize chunk, uint32_t seed)
{
    uint32_t mismatches = 0;
    for (VkDeviceSize offset = 0; offset < buffer->size; offset += chunk) {
        VkDeviceSize bytes = buffer->size - offset;
        if (bytes > chunk) bytes = chunk;
        VkCommandBuffer cb = Begin();
        VkBufferCopy region = { offset, 0, bytes };
        const uint32_t *words;
        BarrierAll(cb);
        vkCmdCopyBuffer(cb, buffer->buffer, rb->buffer, 1, &region);
        BarrierToHost(cb);
        SubmitWait(cb);
        VK(vkMapMemory(dev, rb->memory, 0, VK_WHOLE_SIZE, 0, (void **)&words));
        mismatches += CheckWords(words, offset, (size_t)bytes, seed);
        vkUnmapMemory(dev, rb->memory);
    }
    return mismatches;
}

static void Usage(void)
{
    fprintf(stderr, "usage: vkmembw [--device N] [--mib N] [--iters N] [--groups N] [--warmup-ms N] "
                    "[--placement local|host|both] [--negative-control|--negative-noop-write|"
                    "--negative-noop-copy|--negative-noop-read|--negative-corrupt-copy]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    uint32_t deviceIndex = 0, mib = 256, iters = 20, groups = 4096, warmupMs = 2000;
    int placeFirst = PLACE_LOCAL, placeLast = PLACE_HOST, failed = 0, negative = 0;
    const uint32_t seed = 0x9E3779B9u;
    HMODULE vulkan;
    VkInstance instance;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice phys[16];
    uint32_t physCount = 16, familyCount = 16, family = UINT32_MAX;
    VkQueueFamilyProperties families[16];
    VkPhysicalDeviceProperties props;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkDescriptorSetLayoutBinding bindings[2];
    VkDescriptorSetLayoutCreateInfo dsl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout setLayout;
    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    Kernel kernels[3];
    const uint32_t *code[3] = { write_spv, copy_spv, read_spv };
    size_t codeSize[3] = { sizeof(write_spv), sizeof(copy_spv), sizeof(read_spv) };
    const char *names[3] = { "write", "copy", "read" };
    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32 };
    VkDescriptorPoolCreateInfo dpc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPool descPool;
    VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkQueryPoolCreateInfo qpc = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    VkQueryPool queryPool;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *controls[] = { "--negative-control", "--negative-noop-write", "--negative-noop-copy",
                                   "--negative-noop-read", "--negative-corrupt-copy" };
        int selected = 0;
        for (int k = 0; k < 5; k++) if (!strcmp(a, controls[k])) selected = k + 1;
        if (selected) { if (negative) Usage(); negative = selected; continue; }
        if (i + 1 >= argc) Usage();
        if (!strcmp(a, "--device")) deviceIndex = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--mib")) mib = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--iters")) iters = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--groups")) groups = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--warmup-ms")) warmupMs = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--placement")) {
            const char *p = argv[++i];
            if (!strcmp(p, "local")) placeFirst = placeLast = PLACE_LOCAL;
            else if (!strcmp(p, "host")) placeFirst = placeLast = PLACE_HOST;
            else if (!strcmp(p, "both")) { placeFirst = PLACE_LOCAL; placeLast = PLACE_HOST; }
            else Usage();
        } else Usage();
    }
    // 4 * element index must fit the shaders' uint: mib < 4096 keeps it below 2^30.
    if (mib < 4 || mib > 4095 || iters < 1 || iters > 1000 || groups < 1 || groups > 65535) Usage();
    QueryPerformanceFrequency(&qpcFreq);

    vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) { fprintf(stderr, "vkmembw: vulkan-1.dll not found\n"); return 3; }
    getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan, "vkGetInstanceProcAddr");
    if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");

    app.pApplicationName = "vkmembw";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)

    VK(vkEnumeratePhysicalDevices(instance, &physCount, phys));
    if (deviceIndex >= physCount) { fprintf(stderr, "vkmembw: %u devices, index %u\n", physCount, deviceIndex); return 2; }
    vkGetPhysicalDeviceProperties(phys[deviceIndex], &props);
    vkGetPhysicalDeviceMemoryProperties(phys[deviceIndex], &memProps);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[deviceIndex], &familyCount, families);
    for (uint32_t f = 0; f < familyCount && family == UINT32_MAX; f++)
        if ((families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && families[f].timestampValidBits) family = f;
    if (family == UINT32_MAX) { fprintf(stderr, "vkmembw: no compute queue family with timestamps\n"); return 3; }
    tsPeriodNs = props.limits.timestampPeriod;
    tsMask = families[family].timestampValidBits >= 64 ? ~0ull : ((1ull << families[family].timestampValidBits) - 1);

    printf("device        %s (vendor 0x%04X device 0x%04X) driver 0x%08X api %u.%u.%u\n", props.deviceName,
           props.vendorID, props.deviceID, props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    printf("queue family  %u flags 0x%X, timestamps %u valid bits, period %.3f ns\n", family,
           families[family].queueFlags, families[family].timestampValidBits, tsPeriodNs);
    printf("memory        %u types, %u heaps\n", memProps.memoryTypeCount, memProps.memoryHeapCount);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        printf("  type %2u heap %u flags 0x%03X\n", i, memProps.memoryTypes[i].heapIndex,
               memProps.memoryTypes[i].propertyFlags);
    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
        printf("  heap %u size %llu MiB flags 0x%X\n", i, (unsigned long long)(memProps.memoryHeaps[i].size >> 20),
               memProps.memoryHeaps[i].flags);

    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(phys[deviceIndex], &dci, NULL, &dev));
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(dev, family, 0, &queue);

    for (int b = 0; b < 2; b++) {
        bindings[b].binding = (uint32_t)b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[b].pImmutableSamplers = NULL;
    }
    dsl.bindingCount = 2;
    dsl.pBindings = bindings;
    VK(vkCreateDescriptorSetLayout(dev, &dsl, NULL, &setLayout));
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &setLayout;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &range;
    VK(vkCreatePipelineLayout(dev, &plc, NULL, &layout));
    for (int k = 0; k < 3; k++) {
        VkShaderModuleCreateInfo smc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        VkShaderModule module;
        smc.codeSize = codeSize[k];
        smc.pCode = code[k];
        VK(vkCreateShaderModule(dev, &smc, NULL, &module));
        cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = module;
        cp.stage.pName = "main";
        cp.layout = layout;
        VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &kernels[k].pipeline));
        vkDestroyShaderModule(dev, module, NULL);
        kernels[k].layout = layout;
        kernels[k].name = names[k];
    }
    dpc.maxSets = 16;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &poolSize;
    VK(vkCreateDescriptorPool(dev, &dpc, NULL, &descPool));
    cpc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpc.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &cpc, NULL, &pool));
    VK(vkCreateFence(dev, &fci, NULL, &fence));
    qpc.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpc.queryCount = 2 * iters;
    VK(vkCreateQueryPool(dev, &qpc, NULL, &queryPool));

    for (int place = placeFirst; place <= placeLast; place++) {
        const VkDeviceSize size = (VkDeviceSize)mib << 20, accSize = (VkDeviceSize)groups * 256 * 16;
        const uint32_t n = (uint32_t)(size / 16), threads = groups * 256;
        const VkDeviceSize sample = 4u << 20;
        Buf src = MakeBuf(size, place), dst = MakeBuf(size, place), acc = MakeBuf(accSize, place);
        Buf rb = MakeBuf(sample > accSize ? sample : accSize, PLACE_READBACK);
        VkDescriptorSetLayout layouts[3] = { setLayout, setLayout, setLayout };
        VkDescriptorSetAllocateInfo dsa = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorSet sets[3]; // write: (dst, src); copy: (src, dst); read: (src, acc)
        Buf *pairs[3][2] = { { &dst, &src }, { &src, &dst }, { &src, &acc } };
        const uint32_t freshSeed = seed ^ 1u;
        Push pc = { n, seed }, fresh = { n, freshSeed }, timed = { negative == 1 ? n - 1 : n, freshSeed };
        VkCommandBuffer cb;
        double warmEnd, cpuWarm = 0;
        uint32_t warmBatches = 0, writeMismatches, copyMismatches;
        uint32_t want[4] = { 0, 0, 0, 0 }, got[4] = { 0, 0, 0, 0 };
        const uint32_t *map;

        dsa.descriptorPool = descPool;
        dsa.descriptorSetCount = 3;
        dsa.pSetLayouts = layouts;
        VK(vkAllocateDescriptorSets(dev, &dsa, sets));
        for (int s = 0; s < 3; s++) {
            VkDescriptorBufferInfo info[2];
            VkWriteDescriptorSet w[2];
            for (int b = 0; b < 2; b++) {
                info[b].buffer = pairs[s][b]->buffer;
                info[b].offset = 0;
                info[b].range = VK_WHOLE_SIZE;
                memset(&w[b], 0, sizeof(w[b]));
                w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[b].dstSet = sets[s];
                w[b].dstBinding = (uint32_t)b;
                w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[b].pBufferInfo = &info[b];
            }
            vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
        }
        printf("placement %s: src/dst type %u (heap %u, flags 0x%03X), readback type %u (flags 0x%03X)\n",
               place == PLACE_LOCAL ? "local" : "host", src.type, memProps.memoryTypes[src.type].heapIndex,
               memProps.memoryTypes[src.type].propertyFlags, rb.type, memProps.memoryTypes[rb.type].propertyFlags);
        printf("  buffers %u MiB x2, %u elements of 16 bytes, %u groups x 256 = %u invocations, %u iterations\n", mib,
               n, groups, threads, iters);

        // Seed the source, then warm up with copies until --warmup-ms has passed.
        cb = Begin();
        Dispatch(cb, &kernels[0], sets[0], &pc, groups);
        SubmitWait(cb);
        warmEnd = NowMs() + warmupMs;
        while (NowMs() < warmEnd) {
            cb = Begin();
            for (int i = 0; i < 8; i++) { BarrierAll(cb); Dispatch(cb, &kernels[1], sets[1], &pc, groups); }
            cpuWarm += SubmitWait(cb);
            warmBatches++;
        }
        if (warmupMs) printf("  warmup %u batches of 8 copies, %.0f ms\n", warmBatches, cpuWarm);

        // A different write epoch and poisoned outputs prevent warmup data from satisfying the final checks.
        if (negative) printf("  negative control kind %d: expected content-check failure, exit 1\n", negative);
        cb = Begin();
        BarrierAll(cb);
        vkCmdFillBuffer(cb, dst.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cb, acc.buffer, 0, VK_WHOLE_SIZE, 0);
        SubmitWait(cb);
        Measure(&kernels[0], sets[0], &fresh, groups, iters, queryPool, (double)size, negative == 2);
        Measure(&kernels[1], sets[1], &timed, groups, iters, queryPool, 2.0 * (double)size, negative == 3);
        Measure(&kernels[2], sets[2], &timed, groups, iters, queryPool, (double)size + (double)accSize, negative == 4);
        if (negative == 5) {
            cb = Begin();
            BarrierAll(cb);
            vkCmdFillBuffer(cb, dst.buffer, size / 2, 16, 0);
            SubmitWait(cb);
        }

        // Exact final source/destination checks, then the sum of all read results. None is timed.
        writeMismatches = CheckBuffer(&src, &rb, sample, freshSeed);
        copyMismatches = CheckBuffer(&dst, &rb, sample, freshSeed);
        cb = Begin();
        BarrierAll(cb);
        {
            VkBufferCopy accRegion = { 0, 0, accSize };
            vkCmdCopyBuffer(cb, acc.buffer, rb.buffer, 1, &accRegion);
        }
        BarrierToHost(cb);
        SubmitWait(cb);
        VK(vkMapMemory(dev, rb.memory, 0, VK_WHOLE_SIZE, 0, (void **)&map));
        for (uint64_t t = 0; t < threads; t++)
            for (uint32_t lane = 0; lane < 4; lane++) got[lane] += map[t * 4 + lane];
        vkUnmapMemory(dev, rb.memory);
        for (uint64_t e = 0; e < n; e++)
            for (uint32_t lane = 0; lane < 4; lane++) want[lane] += Pattern(e, lane, freshSeed);
        {
            int readOk = !memcmp(want, got, sizeof(want));
            printf("  check write %s (%u words), copy %s (%u words), read sum %s (%08X %08X %08X %08X)\n",
                   writeMismatches ? "FAILED" : "ok", writeMismatches, copyMismatches ? "FAILED" : "ok",
                   copyMismatches, readOk ? "ok" : "FAILED", got[0], got[1], got[2], got[3]);
            if (writeMismatches || copyMismatches || !readOk) failed = 1;
        }
        VK(vkDeviceWaitIdle(dev));
        FreeBuf(&src); FreeBuf(&dst); FreeBuf(&acc); FreeBuf(&rb);
    }

    vkDestroyQueryPool(dev, queryPool, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDescriptorPool(dev, descPool, NULL);
    for (int k = 0; k < 3; k++) vkDestroyPipeline(dev, kernels[k].pipeline, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, setLayout, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    printf("result %s\n", failed ? "FAILED" : "ok");
    return failed ? 1 : 0;
}
