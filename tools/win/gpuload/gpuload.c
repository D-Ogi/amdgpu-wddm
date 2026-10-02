/*
 * gpuload: a sustained, self-checking GPU compute load through Vulkan, for clock governor trials (DEFECTS BD-050).
 *
 * One process, one queue, one kernel (shaders/spin.comp: integer ALU chains, no memory traffic beyond one store per
 * invocation). Batches of one dispatch each are kept --inflight deep on the queue for --seconds, so the GPU never
 * waits for the CPU between them. Every batch is timed by a top-of-pipe and a bottom-of-pipe timestamp, and its
 * iteration count follows the measured rate, so a batch stays about --batch-ms long while the clock under it moves.
 * Every batch is checked: GL_CHECKS_PER_BATCH of its invocations are compared with the CPU reference (a new seed per
 * batch), so a run that reports load is a run in which the GPU did the work.
 *
 * Busy self-report: the share of GPU time with a batch in flight, from the timestamps alone (gpuload_logic.h
 * GlBusyAdd), once a second and for the whole run. It is the load's own view; the clock governor samples
 * GRBM_STATUS.GUI_ACTIVE, which also sees other clients.
 *
 * Runs headless (no window, no swap chain), so it works from an SSH session in session 0. vulkan-1.dll is loaded at
 * run time; the installed ICD does the rest. The stop file (--stop-file) ends the run early: the controlling script
 * creates it, gpuload notices within one batch, drains the queue and exits 4.
 *
 * Exit: 0 ran for --seconds and every check matched, 1 a check failed, 2 usage, 3 Vulkan or system error (device lost,
 * fence wait over GL_FENCE_TIMEOUT_MS), 4 stopped by the stop file. The last stdout line is always the result line
 * (GlFormatResult) once the device exists.
 */
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpuload_logic.h"
#include "spin_spv.h"

#define GL_MAX_INFLIGHT 4u
#define GL_FENCE_TIMEOUT_MS 5000u   /* a batch aims at --batch-ms (default 20); 5 s means the GPU is not coming back */

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) \
    X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCreateQueryPool) X(vkDestroyQueryPool) \
    X(vkGetQueryPoolResults) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;

typedef struct { uint32_t iters; uint32_t seed; uint32_t n; } Push;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    const volatile uint32_t *words;
    VkDescriptorSet set;
    VkCommandBuffer cb;
    VkFence fence;
    uint64_t batch;
    uint32_t iters;
} Slot;

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memProps;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static VkQueryPool queryPool;
static Slot slots[GL_MAX_INFLIGHT];
static uint32_t tsBits;
static double tsPeriodNs;
static LARGE_INTEGER qpcFreq;

/* What the result line reports; Fail() prints it too. */
static struct {
    int device;              /* the device exists: from here on every exit prints a result line */
    double startMs;
    uint64_t batches, checked, mismatches;
    uint64_t work, span;     /* GPU ticks over the whole run */
    double minWindowPct;
    uint32_t iters;
} run = { 0, 0.0, 0, 0, 0, 0, 0, 101.0, 0 };

static double NowMs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpcFreq.QuadPart;
}

static double TicksMs(uint64_t ticks) { return (double)ticks * tsPeriodNs / 1e6; }

static void PrintResult(const char *result, const char *stop)
{
    char line[512];
    double pct = run.span ? 100.0 * (double)run.work / (double)run.span : 0.0;
    double minPct = run.minWindowPct <= 100.0 ? run.minWindowPct : pct;
    GlFormatResult(line, sizeof(line), result, stop, run.startMs > 0.0 ? (NowMs() - run.startMs) / 1000.0 : 0.0,
                   run.batches, run.checked, run.mismatches, pct, minPct, TicksMs(run.work), run.iters);
    printf("%s\n", line);
    fflush(stdout);
}

static void Fail(const char *what, VkResult r)
{
    fprintf(stderr, "gpuload: %s failed: VkResult %d\n", what, (int)r);
    fflush(stderr);
    if (run.device) PrintResult("error", r == VK_TIMEOUT ? "fence-timeout" : (r == VK_ERROR_DEVICE_LOST ? "device-lost" : "vulkan"));
    exit(GL_EXIT_ERROR);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static void Missing(const char *name)
{
    fprintf(stderr, "gpuload: %s not available\n", name);
    exit(GL_EXIT_ERROR);
}
#define LOAD_INSTANCE(f) if (!(f = (PFN_##f)getInstanceProc(instance, #f))) Missing(#f);
#define LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) Missing(#f);

static int FindType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* The output buffer of a slot: host-visible and coherent (cached when offered), mapped for its whole life; the host
 * reads GL_CHECKS_PER_BATCH words of it per batch. */
static void MakeSlotBuffer(Slot *s, uint32_t n)
{
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, hc = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                ca = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    void *map;
    int t;
    bi.size = (VkDeviceSize)n * 4u;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK(vkCreateBuffer(dev, &bi, NULL, &s->buffer));
    vkGetBufferMemoryRequirements(dev, s->buffer, &req);
    if ((t = FindType(req.memoryTypeBits, hv | hc | ca)) < 0 && (t = FindType(req.memoryTypeBits, hv | hc)) < 0) {
        fprintf(stderr, "gpuload: no host-visible coherent memory type (bits 0x%X)\n", req.memoryTypeBits);
        exit(GL_EXIT_ERROR);
    }
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    VK(vkAllocateMemory(dev, &ai, NULL, &s->memory));
    VK(vkBindBufferMemory(dev, s->buffer, s->memory, 0));
    VK(vkMapMemory(dev, s->memory, 0, VK_WHOLE_SIZE, 0, &map));
    s->words = (const volatile uint32_t *)map;
}

/* Records and submits batch `batch` on slot s: timestamp, dispatch, the write made visible to the host, timestamp. */
static void Submit(Slot *s, uint32_t slotIndex, uint64_t batch, uint32_t iters, uint32_t n, uint32_t groups)
{
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    Push pc;
    pc.iters = iters;
    pc.seed = GlSeed(batch);
    pc.n = n;
    s->batch = batch;
    s->iters = iters;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(s->cb, &cbi));
    vkCmdResetQueryPool(s->cb, queryPool, 2u * slotIndex, 2u);
    vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 2u * slotIndex);
    vkCmdBindPipeline(s->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(s->cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &s->set, 0, NULL);
    vkCmdPushConstants(s->cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(s->cb, groups, 1, 1);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
    vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 2u * slotIndex + 1u);
    VK(vkEndCommandBuffer(s->cb));
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cb;
    VK(vkQueueSubmit(queue, 1, &si, s->fence));
}

/* Waits for slot s, returns its two raw timestamps and checks its words. Returns the number of mismatched words. */
static uint32_t Retire(Slot *s, uint32_t slotIndex, uint32_t n, int negative, uint64_t ts[2])
{
    uint32_t bad = 0;
    VkResult r = vkWaitForFences(dev, 1, &s->fence, VK_TRUE, (uint64_t)GL_FENCE_TIMEOUT_MS * 1000000ull);
    if (r != VK_SUCCESS) Fail("vkWaitForFences", r);
    VK(vkResetFences(dev, 1, &s->fence));
    VK(vkGetQueryPoolResults(dev, queryPool, 2u * slotIndex, 2u, 2u * sizeof(uint64_t), ts, sizeof(uint64_t),
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    for (uint32_t j = 0; j < GL_CHECKS_PER_BATCH; j++) {
        uint32_t i = GlCheckIndex(n, s->batch, j);
        /* The negative control expects one iteration more than the GPU ran: every check must then fail. */
        uint32_t want = GlExpected(GlSeed(s->batch), i, s->iters + (negative ? 1u : 0u)), got = s->words[i];
        if (got != want) {
            if (run.mismatches + bad < 4)
                fprintf(stderr, "gpuload: batch %llu invocation %u: 0x%08X, expected 0x%08X\n",
                        (unsigned long long)s->batch, i, got, want);
            bad++;
        }
    }
    return bad;
}

static void Usage(void)
{
    fprintf(stderr, "usage: gpuload [--seconds S] [--batch-ms M] [--inflight K] [--groups G] [--device N]\n"
                    "               [--stop-file PATH] [--negative-control]\n"
                    "       gpuload --format-sample\n"
                    "  S 1..600 (20), M 2..200 (20), K 1..%u (2), G 1..65535 (1024)\n", GL_MAX_INFLIGHT);
    exit(GL_EXIT_USAGE);
}

static uint32_t Number(const char *text, uint32_t lo, uint32_t hi)
{
    char *end = NULL;
    unsigned long v = strtoul(text, &end, 10);
    if (!end || *end || text[0] == '-' || v < lo || v > hi) Usage();
    return (uint32_t)v;
}

/* One line of each kind, from the formatting code the run uses: the DPM kit's parser test reads these. */
static int FormatSample(void)
{
    char line[512];
    GlFormatTick(line, sizeof(line), 3.0, 151, 48211, 19.87, 99.4);
    printf("%s\n", line);
    GlFormatResult(line, sizeof(line), "ok", "duration", 30.0, 1502, 1502, 0, 99.1, 97.8, 29811.5, 51034);
    printf("%s\n", line);
    return GL_EXIT_OK;
}

int main(int argc, char **argv)
{
    uint32_t seconds = 20, batchMs = 20, inflight = 2, groups = 1024, deviceIndex = 0;
    const char *stopFile = NULL;
    int negative = 0;
    HMODULE vulkan;
    VkInstance instance;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice phys[16];
    uint32_t physCount = 16, familyCount = 16, family = UINT32_MAX;
    VkQueueFamilyProperties families[16];
    VkPhysicalDeviceProperties props;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dsl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout setLayout;
    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkComputePipelineCreateInfo cp = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModule module;
    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GL_MAX_INFLIGHT };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPool descPool;
    VkCommandPoolCreateInfo cpc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool;
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkQueryPoolCreateInfo qpc = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    uint32_t n, iters = 256, head = 0, count = 0;
    uint64_t base = 0, next = 0, ts[2], winWork = 0, winSpan = 0, winBatches = 0;
    double endMs, lastReportMs, lastStopCheckMs = 0.0;
    const char *stop = "duration";
    GlBusy busy = { 0, 0 };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--format-sample")) return FormatSample();
        if (!strcmp(a, "--negative-control")) { negative = 1; continue; }
        if (i + 1 >= argc) Usage();
        if (!strcmp(a, "--seconds")) seconds = Number(argv[++i], 1, 600);
        else if (!strcmp(a, "--batch-ms")) batchMs = Number(argv[++i], 2, 200);
        else if (!strcmp(a, "--inflight")) inflight = Number(argv[++i], 1, GL_MAX_INFLIGHT);
        else if (!strcmp(a, "--groups")) groups = Number(argv[++i], 1, 65535);
        else if (!strcmp(a, "--device")) deviceIndex = Number(argv[++i], 0, 15);
        else if (!strcmp(a, "--stop-file")) stopFile = argv[++i];
        else Usage();
    }
    n = groups * GL_GROUP_SIZE;
    QueryPerformanceFrequency(&qpcFreq);
    if (stopFile && GetFileAttributesA(stopFile) != INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "gpuload: the stop file %s exists before the start\n", stopFile);
        return GL_EXIT_USAGE;
    }

    vulkan = LoadLibraryA("vulkan-1.dll");
    if (!vulkan) { fprintf(stderr, "gpuload: vulkan-1.dll not found\n"); return GL_EXIT_ERROR; }
    getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan, "vkGetInstanceProcAddr");
    if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");
    app.pApplicationName = "gpuload";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)

    VK(vkEnumeratePhysicalDevices(instance, &physCount, phys));
    if (deviceIndex >= physCount) { fprintf(stderr, "gpuload: %u devices, index %u\n", physCount, deviceIndex); return GL_EXIT_USAGE; }
    vkGetPhysicalDeviceProperties(phys[deviceIndex], &props);
    vkGetPhysicalDeviceMemoryProperties(phys[deviceIndex], &memProps);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[deviceIndex], &familyCount, families);
    /* The graphics queue first: the ring the games and the desktop use, and the one vkcompute ran on. */
    for (int pass = 0; pass < 2 && family == UINT32_MAX; pass++)
        for (uint32_t f = 0; f < familyCount && family == UINT32_MAX; f++) {
            VkQueueFlags want = pass == 0 ? (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) : VK_QUEUE_COMPUTE_BIT;
            if ((families[f].queueFlags & want) == want && families[f].timestampValidBits) family = f;
        }
    if (family == UINT32_MAX) { fprintf(stderr, "gpuload: no compute queue family with timestamps\n"); return GL_EXIT_ERROR; }
    tsBits = families[family].timestampValidBits;
    tsPeriodNs = props.limits.timestampPeriod;
    printf("gpuload device \"%s\" vendor 0x%04X device 0x%04X driver 0x%08X api %u.%u.%u\n", props.deviceName,
           props.vendorID, props.deviceID, props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    printf("gpuload queue family %u flags 0x%X, timestamps %u bits, period %.3f ns\n", family,
           families[family].queueFlags, tsBits, tsPeriodNs);
    fflush(stdout);

    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(phys[deviceIndex], &dci, NULL, &dev));
    DEVICE_FUNCS(LOAD_DEVICE)
    run.device = 1;
    vkGetDeviceQueue(dev, family, 0, &queue);

    dsl.bindingCount = 1;
    dsl.pBindings = &binding;
    VK(vkCreateDescriptorSetLayout(dev, &dsl, NULL, &setLayout));
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &setLayout;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &range;
    VK(vkCreatePipelineLayout(dev, &plc, NULL, &layout));
    smc.codeSize = sizeof(spin_spv);
    smc.pCode = spin_spv;
    VK(vkCreateShaderModule(dev, &smc, NULL, &module));
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    cp.layout = layout;
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &pipeline));
    vkDestroyShaderModule(dev, module, NULL);
    dpc.maxSets = GL_MAX_INFLIGHT;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &poolSize;
    VK(vkCreateDescriptorPool(dev, &dpc, NULL, &descPool));
    cpc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpc.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &cpc, NULL, &pool));
    qpc.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpc.queryCount = 2u * inflight;
    VK(vkCreateQueryPool(dev, &qpc, NULL, &queryPool));
    for (uint32_t s = 0; s < inflight; s++) {
        VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorBufferInfo info;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        MakeSlotBuffer(&slots[s], n);
        dsa.descriptorPool = descPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &setLayout;
        VK(vkAllocateDescriptorSets(dev, &dsa, &slots[s].set));
        info.buffer = slots[s].buffer;
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;
        w.dstSet = slots[s].set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        cba.commandPool = pool;
        cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cba.commandBufferCount = 1;
        VK(vkAllocateCommandBuffers(dev, &cba, &slots[s].cb));
        VK(vkCreateFence(dev, &fci, NULL, &slots[s].fence));
    }

    /* Calibration: one batch at a time on slot 0 until a batch takes at least half of --batch-ms. Checked like any
     * other batch; not counted in the busy share. */
    for (int step = 0; step < 12; step++) {
        double ms;
        uint32_t bad;
        Submit(&slots[0], 0, next, iters, n, groups);
        if ((bad = Retire(&slots[0], 0, n, negative, ts)) != 0) {
            run.mismatches += bad;
            run.iters = iters;
            PrintResult("FAILED", "mismatch");
            return GL_EXIT_CHECK;
        }
        next++;
        ms = TicksMs(GlTicks(ts[0], ts[1], tsBits));
        printf("gpuload calibrate iters=%u batch_ms=%.3f\n", iters, ms);
        fflush(stdout);
        if (ms >= 0.5 * batchMs && ms <= 2.0 * batchMs) break;
        iters = GlCalibrateIters(iters, ms, (double)batchMs);
    }
    printf("gpuload start seconds=%u batch_ms=%u inflight=%u groups=%u invocations=%u iters=%u\n", seconds, batchMs,
           inflight, groups, n, iters);
    fflush(stdout);

    run.startMs = lastReportMs = NowMs();
    endMs = run.startMs + 1000.0 * seconds;
    for (;;) {
        double now = NowMs();
        /* Keep the queue --inflight deep while the time lasts and nobody asked to stop. */
        if (stopFile && now - lastStopCheckMs >= 100.0) {
            lastStopCheckMs = now;
            if (!strcmp(stop, "duration") && GetFileAttributesA(stopFile) != INVALID_FILE_ATTRIBUTES) {
                stop = "stop-file";
                endMs = now;
            }
        }
        while (count < inflight && now < endMs) {
            uint32_t s = (head + count) % inflight;
            Submit(&slots[s], s, next++, iters, n, groups);
            count++;
        }
        if (count == 0) break;
        {
            Slot *s = &slots[head];
            uint64_t work, span;
            uint32_t bad = Retire(s, head, n, negative, ts);
            if (!run.batches) base = ts[0];
            GlBusyAdd(&busy, GlTicks(base, ts[0], tsBits), GlTicks(base, ts[1], tsBits), &work, &span);
            run.batches++;
            run.checked++;
            run.work += work;
            run.span += span;
            winWork += work;
            winSpan += span;
            winBatches++;
            run.iters = s->iters;
            head = (head + 1) % inflight;
            count--;
            if (bad) {
                run.mismatches += bad;
                stop = "mismatch";
                endMs = NowMs();  /* drain what is in flight, submit nothing more */
            } else {
                iters = GlNextIters(iters, s->iters, TicksMs(work), (double)batchMs);
            }
        }
        now = NowMs();
        if (now - lastReportMs >= 1000.0) {
            char line[256];
            double pct = winSpan ? 100.0 * (double)winWork / (double)winSpan : 0.0;
            if (winSpan && pct < run.minWindowPct) run.minWindowPct = pct;
            GlFormatTick(line, sizeof(line), (now - run.startMs) / 1000.0, run.batches, iters,
                         winBatches ? TicksMs(winWork) / (double)winBatches : 0.0, pct);
            printf("%s\n", line);
            fflush(stdout);
            lastReportMs = now;
            winWork = winSpan = winBatches = 0;
        }
    }

    VK(vkDeviceWaitIdle(dev));
    if (run.mismatches) PrintResult("FAILED", "mismatch");
    else if (!strcmp(stop, "stop-file")) PrintResult("stopped", stop);
    else PrintResult("ok", stop);
    vkDestroyQueryPool(dev, queryPool, NULL);
    for (uint32_t s = 0; s < inflight; s++) {
        vkDestroyFence(dev, slots[s].fence, NULL);
        vkUnmapMemory(dev, slots[s].memory);
        vkDestroyBuffer(dev, slots[s].buffer, NULL);
        vkFreeMemory(dev, slots[s].memory, NULL);
    }
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDescriptorPool(dev, descPool, NULL);
    vkDestroyPipeline(dev, pipeline, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, setLayout, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    if (run.mismatches) return GL_EXIT_CHECK;
    return !strcmp(stop, "stop-file") ? GL_EXIT_STOPPED : GL_EXIT_OK;
}
