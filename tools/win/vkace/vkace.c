/*
 * vkace: the compute queue family of the BC-250 Vulkan driver, step by step, on a real device.
 *
 * Why this client exists. bc250kmd runs COMPUTE work on WDDM node 2, a MEC compute queue, and says so in
 * its caps. RADV then shows a second queue family, compute and transfer only, whose queue submits to that
 * node (docs/design/compute-node.md). Before this change every submission of the device went to the
 * graphics ring. Applications that see a compute family use it at once: vkd3d-proton puts D3D12 compute
 * and copy queues there and DXVK takes an async compute queue. This client checks, before any game does,
 * that the family is what the driver says it is, that it computes the same words as the graphics queue,
 * that work on the two queues is ordered by timeline semaphores in both directions, that the two queues
 * run at the same time, and that two threads can drive them.
 *
 * Every check is a word-by-word comparison with a CPU oracle. One compute shader (shaders/stage.comp) does
 * all the GPU work: data[i] = rounds of a mixing function over (i ^ seed) in mode 0, or over
 * (data[i] ^ seed) in mode 1. A chain of mode 1 stages gives a result that depends on the order of the
 * stages, so a missing or a broken cross-queue wait shows as wrong words, not only as a hang.
 *
 * The steps (--step N runs one; with no --step, steps 1 to 5 run in order):
 *   1 families   the queue families: family 0 graphics + compute + transfer, and one compute-only family
 *                with one queue, compute + transfer, no sparse binding, timestamps, and no presentation.
 *   2 same       the same shader on both families into two buffers; both equal the oracle and each other.
 *                Then a read-modify-write stage on the compute family.
 *   3 chain      graphics -> compute -> graphics -> CPU on one timeline semaphore, eight times: in
 *                submission order, and with the compute stage submitted first, so that its wait comes
 *                before the graphics signal it waits for. (Stage C must stay behind stage A: both run on
 *                the graphics queue in submission order, and C waits for B, which waits for A.)
 *   4 overlap    one long dispatch on each queue, alone and then both at once, with timestamps: the
 *                times alone, the wall time together and how long the two dispatches overlapped. The
 *                results of the run together are checked on sampled words.
 *   5 threads    two threads, one per queue, walk one timeline in turns: the graphics thread signals the
 *                odd values and the compute thread the even ones, each stage reads the stage before it.
 *                Both threads submit all their work at once, so most waits come before their signals.
 *   6 absent     the negative control, for a driver or KMD with node 2 closed: no compute-only family,
 *                and the graphics family still computes the oracle.
 *
 * The ICD comes from --icd (the DLL is loaded directly through vk_icdGetInstanceProcAddr, with no Vulkan
 * loader), from --manifest (handed to the loader) or from the loader's own selection. An elevated process,
 * which every SSH session on the lab is, has the loader ignore VK_DRIVER_FILES: --icd is the lab route.
 * The "icd module" lines name the driver that ran, with its SHA-256.
 *
 * Every line is flushed when it is written, so a hang leaves the last step in the log. A watchdog ends the
 * process at the deadline (--deadline, 170 s at most). A GPU wait that times out ends the process too,
 * without device teardown, because a teardown can wait for the same work.
 *
 * Exit: 0 every check passed, 1 a check failed, 2 usage, 3 Vulkan or system error, 4 deadline, 5 the
 * device has no compute-only family (--any-device only).
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stage_spv.h"

#define BC250_VENDOR_ID 0x1002u
#define BC250_DEVICE_ID 0x13FEu
#define GROUP 64u
#define NONE UINT32_MAX

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdWriteTimestamp) \
    X(vkCmdResetQueryPool) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;
static PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR presentSupport;

/* ---- output -------------------------------------------------------------------------------------- */

static LARGE_INTEGER qpcFreq, qpcStart;
static unsigned deadlineSec = 170;
static unsigned fenceMs = 10000;
static HANDLE deadlineEvent;
static CRITICAL_SECTION outLock;
static volatile LONG checks, failures;
static int step = 0;

static double NowMs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qpcStart.QuadPart) * 1000.0 / (double)qpcFreq.QuadPart;
}

static void Emit(const char *kind, const char *fmt, va_list ap)
{
    char text[1024];
    vsnprintf(text, sizeof text, fmt, ap);
    EnterCriticalSection(&outLock);
    printf("%-4s %8.1f ms step %d: %s\n", kind, NowMs(), step, text);
    fflush(stdout);
    LeaveCriticalSection(&outLock);
}

static void Info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Emit("INFO", fmt, ap);
    va_end(ap);
}

static int Check(int ok, const char *fmt, ...)
{
    va_list ap;
    InterlockedIncrement(&checks);
    if (!ok) InterlockedIncrement(&failures);
    va_start(ap, fmt);
    Emit(ok ? "PASS" : "FAIL", fmt, ap);
    va_end(ap);
    return ok;
}

/* The end of the process with no device teardown: work that did not finish can make vkDestroyDevice wait. */
static __declspec(noreturn) void Exit(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Emit(code == 0 ? "INFO" : "FAIL", fmt, ap);
    va_end(ap);
    printf("vkace result=%s checks=%ld failures=%ld exit=%d\n", code == 0 ? "pass" : "fail", checks, failures, code);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    ExitProcess((UINT)code);
}

static __declspec(noreturn) void Fail(const char *what, VkResult r)
{
    InterlockedIncrement(&failures);
    Exit(r == VK_ERROR_DEVICE_LOST ? 1 : 3, "%s returned %d%s", what, r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
}

#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static __declspec(noreturn) void Missing(const char *name)
{
    Exit(3, "the driver has no %s", name);
}

#define LOAD_INSTANCE(f) if (!(f = (PFN_##f)getInstanceProc(instance, #f))) Missing(#f);
#define LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) Missing(#f);

static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(deadlineEvent, deadlineSec * 1000u) == WAIT_TIMEOUT) {
        fprintf(stderr, "vkace: deadline of %u s exceeded, ending the process\n", deadlineSec);
        Exit(4, "deadline of %u s exceeded", deadlineSec);
    }
    return 0;
}

/* ---- the ICD witness ----------------------------------------------------------------------------- */

static int Sha256File(const wchar_t *path, char out[65])
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32], buffer[65536];
    DWORD read;
    int ok = 1;
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) != 0) {
        ok = 0;
    } else {
        while (ReadFile(file, buffer, sizeof buffer, &read, NULL) && read)
            if (BCryptHashData(hash, buffer, read, 0) != 0) { ok = 0; break; }
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) != 0) ok = 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (ok)
        for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", digest[i]);
    return ok;
}

static void PrintIcdModules(void)
{
    HMODULE modules[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return;
    if (needed > sizeof modules) needed = sizeof modules;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t path[MAX_PATH * 2];
        char sha[65];
        if (!GetProcAddress(modules[i], "vk_icdGetInstanceProcAddr")) continue;
        if (!GetModuleFileNameW(modules[i], path, (DWORD)(sizeof path / sizeof path[0]))) continue;
        printf("icd module    %ls sha256 %s\n", path, Sha256File(path, sha) ? sha : "unavailable");
    }
    fflush(stdout);
}

/* ---- the oracle ---------------------------------------------------------------------------------- */

static uint32_t Mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* shaders/stage.comp on the CPU. */
static uint32_t Stage(uint32_t in, uint32_t seed, uint32_t rounds)
{
    uint32_t v = in ^ seed;
    for (uint32_t r = 0; r < rounds; r++) v = Mix(v + r);
    return v;
}

static uint32_t Seed(uint32_t n)
{
    return Mix(0xACE00000u + n);
}

/* ---- the device ---------------------------------------------------------------------------------- */

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memProps;
static float timestampPeriod;
static uint32_t gfxFam = NONE, cmpFam = NONE, familyCount;
static VkQueueFamilyProperties families[16];
static VkQueue gfxQ, cmpQ;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout layout;
static VkPipeline pipeline;
static VkDescriptorPool dpool;
static int anyDevice, isBc250;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *map;
    uint32_t count;
    VkDescriptorSet set;
} Buf;

typedef struct {
    uint32_t seed, count, rounds, mode;
} Params;

static int FindType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* A host-visible, coherent storage buffer of count words, shared by both families. */
static Buf MakeBuf(uint32_t count)
{
    Buf b;
    memset(&b, 0, sizeof b);
    b.count = count;
    uint32_t fams[2] = { gfxFam, cmpFam };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)count * 4;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (cmpFam != NONE) {
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = 2;
        bi.pQueueFamilyIndices = fams;
    } else {
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VK(vkCreateBuffer(dev, &bi, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    int type = FindType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) Exit(3, "no host-visible coherent memory type for a storage buffer");
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    VK(vkAllocateMemory(dev, &ai, NULL, &b.memory));
    VK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
    VK(vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, (void **)&b.map));

    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VK(vkAllocateDescriptorSets(dev, &dai, &b.set));
    VkDescriptorBufferInfo dbi = { b.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = b.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    return b;
}

static void FreeBuf(Buf *b)
{
    vkDestroyBuffer(dev, b->buffer, NULL);
    vkFreeMemory(dev, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

static void Sentinel(Buf *b)
{
    for (uint32_t i = 0; i < b->count; i++) b->map[i] = 0xDEADBEEFu ^ i;
}

static VkCommandPool MakePool(uint32_t family)
{
    VkCommandPool pool;
    VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &ci, NULL, &pool));
    return pool;
}

static VkCommandBuffer BeginCmd(VkCommandPool pool)
{
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

/* One dispatch of stage.comp over the buffer. With a query pool, timestamps 0 and 1 bracket it. */
static void RecordStage(VkCommandBuffer cmd, const Buf *b, Params p, VkQueryPool ts)
{
    p.count = b->count;
    if (ts) {
        vkCmdResetQueryPool(cmd, ts, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &b->set, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (b->count + GROUP - 1) / GROUP, 1, 1);
    if (ts) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 1);
}

/* The shader writes of the command buffer become visible to the host. */
static void EndCmd(VkCommandBuffer cmd, int hostRead)
{
    if (hostRead) {
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                             NULL, 0, NULL);
    }
    VK(vkEndCommandBuffer(cmd));
}

static VkSemaphore MakeTimeline(void)
{
    VkSemaphore s;
    VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    ci.pNext = &ti;
    VK(vkCreateSemaphore(dev, &ci, NULL, &s));
    return s;
}

static VkFence MakeFence(void)
{
    VkFence f;
    VkFenceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK(vkCreateFence(dev, &ci, NULL, &f));
    return f;
}

static VkQueryPool MakeTimestamps(void)
{
    VkQueryPool q;
    VkQueryPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = 2;
    VK(vkCreateQueryPool(dev, &ci, NULL, &q));
    return q;
}

/* One submission: an optional timeline wait (wait == NULL: none), an optional timeline signal, a fence. */
static void Submit(VkQueue q, VkCommandBuffer cmd, VkSemaphore wait, uint64_t waitValue, VkSemaphore signal,
                   uint64_t signalValue, VkFence fence, const char *what)
{
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkTimelineSemaphoreSubmitInfo ti = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &ti;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
        ti.waitSemaphoreValueCount = 1;
        ti.pWaitSemaphoreValues = &waitValue;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
        ti.signalSemaphoreValueCount = 1;
        ti.pSignalSemaphoreValues = &signalValue;
    }
    VkResult r = vkQueueSubmit(q, 1, &si, fence);
    if (r != VK_SUCCESS) Fail(what, r);
}

static void WaitFence(VkFence f, const char *what)
{
    VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, (uint64_t)fenceMs * 1000000ull);
    if (r == VK_TIMEOUT) {
        InterlockedIncrement(&failures);
        Exit(1, "%s: no completion in %u ms", what, fenceMs);
    }
    if (r != VK_SUCCESS) Fail(what, r);
}

static void WaitTimeline(VkSemaphore s, uint64_t value, unsigned ms, const char *what)
{
    VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s;
    wi.pValues = &value;
    VkResult r = vkWaitSemaphores(dev, &wi, (uint64_t)ms * 1000000ull);
    if (r == VK_TIMEOUT) {
        uint64_t now = 0;
        vkGetSemaphoreCounterValue(dev, s, &now);
        InterlockedIncrement(&failures);
        Exit(1, "%s: the timeline is at %llu, not %llu, after %u ms", what, (unsigned long long)now,
             (unsigned long long)value, ms);
    }
    if (r != VK_SUCCESS) Fail(what, r);
}

/* Words of the buffer that differ from expect[] (or, with expect NULL, from the stage of i). */
static uint32_t Compare(const Buf *b, const uint32_t *expect, uint32_t *first)
{
    uint32_t bad = 0;
    *first = NONE;
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->map[i] != expect[i]) {
            if (*first == NONE) *first = i;
            bad++;
        }
    }
    return bad;
}

static double Timestamps(VkQueryPool q, uint64_t out[2])
{
    VK(vkGetQueryPoolResults(dev, q, 0, 2, 2 * sizeof(uint64_t), out, sizeof(uint64_t),
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    return (double)(out[1] - out[0]) * timestampPeriod / 1e6;
}

static const char *FamilyFlags(VkQueueFlags f, char *out)
{
    out[0] = 0;
    if (f & VK_QUEUE_GRAPHICS_BIT) strcat(out, "graphics ");
    if (f & VK_QUEUE_COMPUTE_BIT) strcat(out, "compute ");
    if (f & VK_QUEUE_TRANSFER_BIT) strcat(out, "transfer ");
    if (f & VK_QUEUE_SPARSE_BINDING_BIT) strcat(out, "sparse ");
    if (f & VK_QUEUE_PROTECTED_BIT) strcat(out, "protected ");
    if (f & ~(VkQueueFlags)(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT |
              VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_PROTECTED_BIT)) strcat(out, "other ");
    size_t n = strlen(out);
    if (n) out[n - 1] = 0;
    return out;
}

/* ---- step 1: the families ------------------------------------------------------------------------ */

static void Step1(void)
{
    step = 1;
    char flags[96];
    for (uint32_t i = 0; i < familyCount; i++) {
        const char *present = "unknown";
        if (presentSupport) present = presentSupport(phys, i) ? "yes" : "no";
        Info("family %u: %s, %u queues, timestamp bits %u, presents %s", i, FamilyFlags(families[i].queueFlags, flags),
             families[i].queueCount, families[i].timestampValidBits, present);
    }
    if (!isBc250) {
        Info("not a BC-250: the checks of the BC-250 families are skipped");
        return;
    }
    const VkQueueFlags gct = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    Check(gfxFam == 0 && (families[0].queueFlags & gct) == gct, "family 0 has graphics, compute and transfer");
    if (!Check(cmpFam != NONE, "a compute-only family is present (%u families)", familyCount)) return;
    const VkQueueFlags ct = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    Check((families[cmpFam].queueFlags & ~(VkQueueFlags)VK_QUEUE_PROTECTED_BIT) == ct,
          "family %u has compute and transfer only: %s", cmpFam, FamilyFlags(families[cmpFam].queueFlags, flags));
    Check(families[cmpFam].queueCount == 1, "family %u has one queue (%u)", cmpFam, families[cmpFam].queueCount);
    Check(families[cmpFam].timestampValidBits > 0, "family %u has timestamps (%u bits)", cmpFam,
          families[cmpFam].timestampValidBits);
    if (presentSupport) {
        Check(presentSupport(phys, gfxFam) == VK_TRUE, "family %u presents", gfxFam);
        Check(presentSupport(phys, cmpFam) == VK_FALSE, "family %u does not present", cmpFam);
    } else {
        Info("VK_KHR_win32_surface is absent: presentation is not checked");
    }
}

/* ---- step 2: the same shader on both families ---------------------------------------------------- */

static void Step2(void)
{
    step = 2;
    const uint32_t n = 1u << 20;
    const Params p = { Seed(2), 0, 16, 0 };
    Buf a = MakeBuf(n), b = MakeBuf(n);
    uint32_t *expect = malloc((size_t)n * 4);
    if (!expect) Exit(3, "out of host memory");
    Sentinel(&a);
    Sentinel(&b);
    uint32_t same = 0;
    for (uint32_t i = 0; i < n; i++) {
        expect[i] = Stage(i, p.seed, p.rounds);
        same += expect[i] == (0xDEADBEEFu ^ i);
    }
    Check(same < 4, "the oracle differs from the sentinel almost everywhere (%u words equal)", same);

    VkCommandPool gp = MakePool(gfxFam), cp = MakePool(cmpFam);
    VkFence fg = MakeFence(), fc = MakeFence();
    VkCommandBuffer cg = BeginCmd(gp), cc = BeginCmd(cp);
    RecordStage(cg, &a, p, VK_NULL_HANDLE);
    EndCmd(cg, 1);
    RecordStage(cc, &b, p, VK_NULL_HANDLE);
    EndCmd(cc, 1);
    double t0 = NowMs();
    Submit(gfxQ, cg, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, fg, "vkQueueSubmit graphics");
    Submit(cmpQ, cc, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, fc, "vkQueueSubmit compute");
    WaitFence(fg, "the graphics dispatch");
    WaitFence(fc, "the compute dispatch");
    Info("both dispatches of %u words done in %.1f ms", n, NowMs() - t0);

    uint32_t first, bad = Compare(&a, expect, &first);
    Check(!bad, "family %u computes the oracle: %u of %u words differ (first %d: 0x%08X, expected 0x%08X)", gfxFam,
          bad, n, (int)first, first == NONE ? 0 : a.map[first], first == NONE ? 0 : expect[first]);
    bad = Compare(&b, expect, &first);
    Check(!bad, "family %u computes the oracle: %u of %u words differ (first %d: 0x%08X, expected 0x%08X)", cmpFam,
          bad, n, (int)first, first == NONE ? 0 : b.map[first], first == NONE ? 0 : expect[first]);
    Check(!memcmp(a.map, b.map, (size_t)n * 4), "the two families wrote the same %u words", n);

    /* A stage that reads what the compute queue wrote before. */
    const Params q = { Seed(20), 0, 8, 1 };
    for (uint32_t i = 0; i < n; i++) expect[i] = Stage(expect[i], q.seed, q.rounds);
    VkFence f2 = MakeFence();
    VkCommandBuffer c2 = BeginCmd(cp);
    RecordStage(c2, &b, q, VK_NULL_HANDLE);
    EndCmd(c2, 1);
    Submit(cmpQ, c2, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, f2, "vkQueueSubmit compute read-modify-write");
    WaitFence(f2, "the compute read-modify-write");
    bad = Compare(&b, expect, &first);
    Check(!bad, "a read-modify-write stage on family %u: %u of %u words differ (first %d)", cmpFam, bad, n, (int)first);

    vkDestroyFence(dev, fg, NULL);
    vkDestroyFence(dev, fc, NULL);
    vkDestroyFence(dev, f2, NULL);
    vkDestroyCommandPool(dev, gp, NULL);
    vkDestroyCommandPool(dev, cp, NULL);
    FreeBuf(&a);
    FreeBuf(&b);
    free(expect);
}

/* ---- step 3: A -> B -> C -> CPU over the two queues, both directions ------------------------------ */

static void Step3(void)
{
    step = 3;
    const uint32_t n = 1u << 18, rounds[3] = { 256, 64, 64 };
    Buf x = MakeBuf(n);
    uint32_t *expect = malloc((size_t)n * 4);
    if (!expect) Exit(3, "out of host memory");
    VkSemaphore t = MakeTimeline();
    VkCommandPool gp = MakePool(gfxFam), cp = MakePool(cmpFam);
    /* Bit 0: the middle stage B is submitted first, so its wait comes before the signal it waits for.
     * Bit 1: the swapped direction, compute -> graphics -> compute, so graphics waits for compute. */
    for (uint32_t iter = 0; iter < 16; iter++) {
        const int early = iter & 1, swapped = (iter >> 1) & 1;
        VkQueue qa = swapped ? cmpQ : gfxQ, qb = swapped ? gfxQ : cmpQ;
        VkCommandPool pa_pool = swapped ? cp : gp, pb_pool = swapped ? gp : cp;
        const char *nameA = swapped ? "vkQueueSubmit compute stage A" : "vkQueueSubmit graphics stage A";
        const char *nameB = swapped ? "vkQueueSubmit graphics stage B" : "vkQueueSubmit compute stage B";
        const char *nameC = swapped ? "vkQueueSubmit compute stage C" : "vkQueueSubmit graphics stage C";
        const uint64_t base = (uint64_t)iter * 3;
        const Params pa = { Seed(300 + iter * 3), 0, rounds[0], 0 };
        const Params pb = { Seed(301 + iter * 3), 0, rounds[1], 1 };
        const Params pc = { Seed(302 + iter * 3), 0, rounds[2], 1 };
        Sentinel(&x);
        for (uint32_t i = 0; i < n; i++)
            expect[i] = Stage(Stage(Stage(i, pa.seed, pa.rounds), pb.seed, pb.rounds), pc.seed, pc.rounds);

        VkCommandBuffer ca = BeginCmd(pa_pool), cb = BeginCmd(pb_pool), cc = BeginCmd(pa_pool);
        RecordStage(ca, &x, pa, VK_NULL_HANDLE);
        EndCmd(ca, 0);
        RecordStage(cb, &x, pb, VK_NULL_HANDLE);
        EndCmd(cb, 0);
        RecordStage(cc, &x, pc, VK_NULL_HANDLE);
        EndCmd(cc, 1);
        double t0 = NowMs();
        if (early) Submit(qb, cb, t, base + 1, t, base + 2, VK_NULL_HANDLE, nameB);
        Submit(qa, ca, VK_NULL_HANDLE, 0, t, base + 1, VK_NULL_HANDLE, nameA);
        if (!early) Submit(qb, cb, t, base + 1, t, base + 2, VK_NULL_HANDLE, nameB);
        Submit(qa, cc, t, base + 2, t, base + 3, VK_NULL_HANDLE, nameC);
        WaitTimeline(t, base + 3, fenceMs, "the chain");
        const double ms = NowMs() - t0;
        uint64_t value = 0;
        VK(vkGetSemaphoreCounterValue(dev, t, &value));
        uint32_t first, bad = Compare(&x, expect, &first);
        Check(!bad && value == base + 3, "chain %u (%s, %s): timeline %llu, %u of %u words differ (first %d), %.1f ms",
              iter, swapped ? "compute->graphics->compute" : "graphics->compute->graphics",
              early ? (swapped ? "graphics wait submitted first" : "compute wait submitted first") : "in order",
              (unsigned long long)value, bad, n, (int)first, ms);
    }
    VK(vkDeviceWaitIdle(dev));
    vkDestroySemaphore(dev, t, NULL);
    vkDestroyCommandPool(dev, gp, NULL);
    vkDestroyCommandPool(dev, cp, NULL);
    FreeBuf(&x);
    free(expect);
}

/* ---- step 4: overlap ----------------------------------------------------------------------------- */

typedef struct {
    double gpuMs, wallMs;
    uint64_t ts[2];
} Run;

/* One long dispatch on one queue, or (both) one on each queue submitted back to back. */
static void RunLong(int both, VkQueue q, uint32_t family, Buf *bufs, uint32_t rounds, Run out[2])
{
    VkCommandPool pools[2] = { MakePool(both ? gfxFam : family), both ? MakePool(cmpFam) : VK_NULL_HANDLE };
    VkQueue queues[2] = { both ? gfxQ : q, cmpQ };
    VkQueryPool ts[2] = { MakeTimestamps(), MakeTimestamps() };
    VkFence fences[2] = { MakeFence(), MakeFence() };
    VkCommandBuffer cmd[2];
    const int count = both ? 2 : 1;
    for (int k = 0; k < count; k++) {
        const Params p = { Seed(400 + (uint32_t)k), 0, rounds, 0 };
        cmd[k] = BeginCmd(pools[k]);
        RecordStage(cmd[k], &bufs[k], p, ts[k]);
        EndCmd(cmd[k], 1);
    }
    double t0 = NowMs();
    for (int k = 0; k < count; k++)
        Submit(queues[k], cmd[k], VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, fences[k], "vkQueueSubmit long dispatch");
    for (int k = 0; k < count; k++) {
        WaitFence(fences[k], "a long dispatch");
        out[k].wallMs = NowMs() - t0;
    }
    for (int k = 0; k < count; k++) {
        out[k].gpuMs = Timestamps(ts[k], out[k].ts);
        vkDestroyQueryPool(dev, ts[k], NULL);
        vkDestroyFence(dev, fences[k], NULL);
        vkDestroyCommandPool(dev, pools[k], NULL);
    }
    if (!both) {
        vkDestroyQueryPool(dev, ts[1], NULL);
        vkDestroyFence(dev, fences[1], NULL);
    }
}

static uint32_t SampleBad(const Buf *b, uint32_t seed, uint32_t rounds, uint32_t *sampled)
{
    uint32_t bad = 0;
    *sampled = 0;
    for (uint32_t i = 0; i < b->count; i += 4093) {
        (*sampled)++;
        bad += b->map[i] != Stage(i, seed, rounds);
    }
    return bad;
}

static void Step4(void)
{
    step = 4;
    const uint32_t n = 1u << 20;
    Buf bufs[2] = { MakeBuf(n), MakeBuf(n) };
    Run r[2];

    /* Calibration: about 80 ms of GPU time for one dispatch on the graphics queue. */
    uint32_t rounds = 256;
    RunLong(0, gfxQ, gfxFam, bufs, rounds, r);
    if (r[0].gpuMs > 0.01) {
        double scaled = rounds * 80.0 / r[0].gpuMs;
        rounds = scaled < 256.0 ? 256u : scaled > 65536.0 ? 65536u : (uint32_t)scaled;
    }
    Info("calibration: 256 rounds took %.2f ms of GPU time; the long dispatch is %u rounds", r[0].gpuMs, rounds);

    Run g[2], c[2], both[2];
    RunLong(0, gfxQ, gfxFam, bufs, rounds, g);
    RunLong(0, cmpQ, cmpFam, bufs, rounds, c);
    Check(g[0].gpuMs > 0 && c[0].gpuMs > 0, "alone: graphics %.2f ms, compute %.2f ms of GPU time (wall %.1f, %.1f ms)",
          g[0].gpuMs, c[0].gpuMs, g[0].wallMs, c[0].wallMs);

    Sentinel(&bufs[0]);
    Sentinel(&bufs[1]);
    RunLong(1, VK_NULL_HANDLE, 0, bufs, rounds, both);
    uint32_t sampled, bad0 = SampleBad(&bufs[0], Seed(400), rounds, &sampled);
    uint32_t bad1 = SampleBad(&bufs[1], Seed(401), rounds, &sampled);
    Check(!bad0 && !bad1, "together: both results equal the oracle on %u sampled words each (%u, %u differ)", sampled,
          bad0, bad1);
    const uint64_t s0 = both[0].ts[0], e0 = both[0].ts[1], s1 = both[1].ts[0], e1 = both[1].ts[1];
    const uint64_t lo = s0 > s1 ? s0 : s1, hi = e0 < e1 ? e0 : e1;
    const double overlapMs = hi > lo ? (double)(hi - lo) * timestampPeriod / 1e6 : 0.0;
    const double wall = both[0].wallMs > both[1].wallMs ? both[0].wallMs : both[1].wallMs;
    Info("together: graphics %.2f ms, compute %.2f ms of GPU time, wall %.1f ms; alone in sum %.2f ms of GPU time",
         both[0].gpuMs, both[1].gpuMs, wall, g[0].gpuMs + c[0].gpuMs);
    Info("together: the two dispatches overlapped for %.2f ms (compute starts %+.2f ms after graphics)", overlapMs,
         ((double)s1 - (double)s0) * timestampPeriod / 1e6);
    FreeBuf(&bufs[0]);
    FreeBuf(&bufs[1]);
}

/* ---- step 5: two threads ------------------------------------------------------------------------- */

#define TURNS 16u

typedef struct {
    VkQueue queue;
    uint32_t family;
    int odd; /* the graphics thread: odd timeline values */
    Buf *buf;
    VkSemaphore timeline;
    HANDLE go;
    uint32_t rounds;
    VkResult result;
    const char *failed;
} Walker;

static DWORD WINAPI Walk(LPVOID arg)
{
    Walker *w = arg;
    VkCommandPool pool;
    VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.queueFamilyIndex = w->family;
    if ((w->result = vkCreateCommandPool(dev, &ci, NULL, &pool)) != VK_SUCCESS) { w->failed = "vkCreateCommandPool"; return 1; }
    VkCommandBuffer cmd[TURNS];
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = TURNS;
    if ((w->result = vkAllocateCommandBuffers(dev, &ai, cmd)) != VK_SUCCESS) { w->failed = "vkAllocateCommandBuffers"; return 1; }
    for (uint32_t k = 1; k <= TURNS; k++) {
        const uint32_t value = w->odd ? 2 * k - 1 : 2 * k;
        const Params p = { Seed(500 + value), 0, w->rounds, value == 1 ? 0u : 1u };
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((w->result = vkBeginCommandBuffer(cmd[k - 1], &bi)) != VK_SUCCESS) { w->failed = "vkBeginCommandBuffer"; return 1; }
        RecordStage(cmd[k - 1], w->buf, p, VK_NULL_HANDLE);
        if (value == 2 * TURNS) {
            VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd[k - 1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                 &mb, 0, NULL, 0, NULL);
        }
        if ((w->result = vkEndCommandBuffer(cmd[k - 1])) != VK_SUCCESS) { w->failed = "vkEndCommandBuffer"; return 1; }
    }
    WaitForSingleObject(w->go, INFINITE);
    for (uint32_t k = 1; k <= TURNS; k++) {
        const uint64_t value = w->odd ? 2 * k - 1 : 2 * k, waitValue = value - 1;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkTimelineSemaphoreSubmitInfo ti = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.pNext = &ti;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd[k - 1];
        if (waitValue) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &w->timeline;
            si.pWaitDstStageMask = &stage;
            ti.waitSemaphoreValueCount = 1;
            ti.pWaitSemaphoreValues = &waitValue;
        }
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &w->timeline;
        ti.signalSemaphoreValueCount = 1;
        ti.pSignalSemaphoreValues = &value;
        if ((w->result = vkQueueSubmit(w->queue, 1, &si, VK_NULL_HANDLE)) != VK_SUCCESS) { w->failed = "vkQueueSubmit"; return 1; }
    }
    return 0;
}

static void Step5(void)
{
    step = 5;
    const uint32_t n = 1u << 16, rounds = 64;
    Buf x = MakeBuf(n);
    Sentinel(&x);
    uint32_t *expect = malloc((size_t)n * 4);
    if (!expect) Exit(3, "out of host memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = i;
        for (uint32_t value = 1; value <= 2 * TURNS; value++) v = Stage(v, Seed(500 + value), rounds);
        expect[i] = v;
    }
    VkSemaphore t = MakeTimeline();
    HANDLE go = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!go) Exit(3, "CreateEvent failed (%lu)", GetLastError());
    Walker w[2] = {
        { gfxQ, gfxFam, 1, &x, t, go, rounds, VK_SUCCESS, NULL },
        { cmpQ, cmpFam, 0, &x, t, go, rounds, VK_SUCCESS, NULL },
    };
    HANDLE th[2];
    for (int k = 0; k < 2; k++)
        if (!(th[k] = CreateThread(NULL, 0, Walk, &w[k], 0, NULL))) Exit(3, "CreateThread failed (%lu)", GetLastError());
    double t0 = NowMs();
    SetEvent(go);
    if (WaitForMultipleObjects(2, th, TRUE, fenceMs) != WAIT_OBJECT_0) {
        InterlockedIncrement(&failures);
        Exit(1, "the submitting threads did not finish in %u ms", fenceMs);
    }
    const double submitMs = NowMs() - t0;
    for (int k = 0; k < 2; k++)
        if (w[k].failed) Fail(w[k].failed, w[k].result);
    WaitTimeline(t, 2 * TURNS, fenceMs, "the two-thread walk");
    const double ms = NowMs() - t0;
    uint32_t first, bad = Compare(&x, expect, &first);
    Check(!bad, "two threads, %u turns each: %u of %u words differ from the %u stages in order (first %d); "
                "submitted in %.1f ms, done in %.1f ms", TURNS, bad, n, 2 * TURNS, (int)first, submitMs, ms);
    VK(vkDeviceWaitIdle(dev));
    CloseHandle(th[0]);
    CloseHandle(th[1]);
    CloseHandle(go);
    vkDestroySemaphore(dev, t, NULL);
    FreeBuf(&x);
    free(expect);
}

/* ---- step 6: the negative control ---------------------------------------------------------------- */

static void Step6(void)
{
    step = 6;
    char flags[96];
    for (uint32_t i = 0; i < familyCount; i++)
        Info("family %u: %s, %u queues", i, FamilyFlags(families[i].queueFlags, flags), families[i].queueCount);
    Check(cmpFam == NONE, "no compute-only family (%u families)", familyCount);
    const uint32_t n = 1u << 16;
    const Params p = { Seed(6), 0, 16, 0 };
    Buf a = MakeBuf(n);
    Sentinel(&a);
    VkCommandPool gp = MakePool(gfxFam);
    VkFence f = MakeFence();
    VkCommandBuffer cmd = BeginCmd(gp);
    RecordStage(cmd, &a, p, VK_NULL_HANDLE);
    EndCmd(cmd, 1);
    Submit(gfxQ, cmd, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, 0, f, "vkQueueSubmit graphics");
    WaitFence(f, "the graphics dispatch");
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) bad += a.map[i] != Stage(i, p.seed, p.rounds);
    Check(!bad, "family %u still computes the oracle (%u of %u words differ)", gfxFam, bad, n);
    vkDestroyFence(dev, f, NULL);
    vkDestroyCommandPool(dev, gp, NULL);
    FreeBuf(&a);
}

/* ---- main ---------------------------------------------------------------------------------------- */

static void Usage(FILE *f, int code)
{
    fprintf(f,
            "usage: vkace [--icd DLL | --manifest JSON] [--step N] [--any-device] [--deadline S] [--fence-ms MS]\n"
            "  --icd DLL       load this ICD DLL directly (absolute path), without the Vulkan loader\n"
            "  --manifest JSON hand this ICD manifest to the loader (ignored by an elevated process)\n"
            "  --step N        run step N only: 1 families, 2 same, 3 chain, 4 overlap, 5 threads, 6 absent\n"
            "                  (the negative control: no compute-only family). Default: steps 1 to 5\n"
            "  --any-device    use the first device when no BC-250 is present (the BC-250 checks of step 1\n"
            "                  are skipped); exit 5 when it has no compute-only family\n"
            "  --deadline S    end the process after S seconds, 5 to 170 (default 170)\n"
            "  --fence-ms MS   the bound of one GPU wait, 100 to 60000 (default 10000)\n");
    exit(code);
}

int main(int argc, char **argv)
{
    const char *icdPath = NULL, *manifestPath = NULL;
    int only = 0;
    HMODULE library;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) Usage(stdout, 0);
        if (i + 1 >= argc && (!strcmp(a, "--icd") || !strcmp(a, "--manifest") || !strcmp(a, "--step") ||
                              !strcmp(a, "--deadline") || !strcmp(a, "--fence-ms"))) Usage(stderr, 2);
        if (!strcmp(a, "--icd")) icdPath = argv[++i];
        else if (!strcmp(a, "--manifest")) manifestPath = argv[++i];
        else if (!strcmp(a, "--step")) only = atoi(argv[++i]);
        else if (!strcmp(a, "--deadline")) deadlineSec = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--fence-ms")) fenceMs = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--any-device")) anyDevice = 1;
        else Usage(stderr, 2);
    }
    if ((icdPath && manifestPath) || only < 0 || only > 6 || deadlineSec < 5 || deadlineSec > 170 || fenceMs < 100 ||
        fenceMs > 60000) Usage(stderr, 2);

    InitializeCriticalSection(&outLock);
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);
    if (only) printf("vkace step %d, deadline %u s, GPU wait bound %u ms\n", only, deadlineSec, fenceMs);
    else printf("vkace steps 1-5, deadline %u s, GPU wait bound %u ms\n", deadlineSec, fenceMs);
    fflush(stdout);
    if (!(deadlineEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) Exit(3, "CreateEvent failed (%lu)", GetLastError());
    if (!CreateThread(NULL, 0, Watchdog, NULL, 0, NULL)) Exit(3, "CreateThread failed (%lu)", GetLastError());

    if (icdPath) {
        wchar_t wide[MAX_PATH * 2];
        PFN_vkNegotiateLoaderICDInterfaceVersion negotiate;
        uint32_t icdVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        if (!MultiByteToWideChar(CP_ACP, 0, icdPath, -1, wide, (int)(sizeof wide / sizeof wide[0])) ||
            !(wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/'))) {
            fprintf(stderr, "vkace: --icd needs an absolute path, got %s\n", icdPath);
            return 2;
        }
        library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) Exit(3, "cannot load %s (error %lu)", icdPath, GetLastError());
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vk_icdGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vk_icdGetInstanceProcAddr");
        negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)GetProcAddress(library, "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (negotiate && negotiate(&icdVersion) == VK_SUCCESS) printf("icd interface %u\n", icdVersion);
        else printf("icd interface unavailable (the module answers no negotiation)\n");
    } else {
        if (manifestPath) {
            SetEnvironmentVariableA("VK_DRIVER_FILES", manifestPath);
            SetEnvironmentVariableA("VK_ICD_FILENAMES", manifestPath);
            printf("manifest      %s\n", manifestPath);
        }
        library = LoadLibraryA("vulkan-1.dll");
        if (!library) Exit(3, "vulkan-1.dll not found");
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    }
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enumExt =
        (PFN_vkEnumerateInstanceExtensionProperties)getInstanceProc(NULL, "vkEnumerateInstanceExtensionProperties");

    /* VK_KHR_win32_surface, when the driver has it, answers which family presents. */
    const char *exts[2];
    uint32_t extCount = 0;
    if (enumExt) {
        VkExtensionProperties props[64];
        uint32_t n = 64;
        int surface = 0, win32 = 0;
        if (enumExt(NULL, &n, props) >= 0) {
            for (uint32_t i = 0; i < n; i++) {
                surface |= !strcmp(props[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME);
                win32 |= !strcmp(props[i].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
            }
        }
        if (surface && win32) {
            exts[extCount++] = VK_KHR_SURFACE_EXTENSION_NAME;
            exts[extCount++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        }
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vkace";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extCount;
    ici.ppEnabledExtensionNames = exts;
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)
    if (extCount)
        presentSupport = (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)getInstanceProc(
            instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
    PrintIcdModules();

    VkPhysicalDevice list[8];
    uint32_t count = 8;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, list);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) Fail("vkEnumeratePhysicalDevices", r);
    for (uint32_t i = 0; i < count && !phys; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        if (p.vendorID == BC250_VENDOR_ID && p.deviceID == BC250_DEVICE_ID) { phys = list[i]; isBc250 = 1; }
    }
    if (!phys && anyDevice && count) phys = list[0];
    if (!phys) Exit(3, "no BC-250 among %u devices (--any-device takes another one)", count);
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(phys, &props);
    timestampPeriod = props.properties.limits.timestampPeriod;
    printf("device        %s (vendor 0x%04X device 0x%04X), driver '%s' '%s', api %u.%u.%u, timestamp period %.3f ns\n",
           props.properties.deviceName, props.properties.vendorID, props.properties.deviceID, driver.driverName,
           driver.driverInfo, VK_API_VERSION_MAJOR(props.properties.apiVersion),
           VK_API_VERSION_MINOR(props.properties.apiVersion), VK_API_VERSION_PATCH(props.properties.apiVersion),
           timestampPeriod);
    fflush(stdout);
    if (props.properties.apiVersion < VK_API_VERSION_1_2) Exit(3, "the device offers Vulkan below 1.2");
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    familyCount = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, families);
    for (uint32_t i = 0; i < familyCount; i++) {
        const VkQueueFlags f = families[i].queueFlags;
        if (gfxFam == NONE && (f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT)) gfxFam = i;
        if (cmpFam == NONE && (f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) cmpFam = i;
    }
    if (gfxFam == NONE) Exit(3, "no graphics and compute family");
    if (cmpFam == NONE && anyDevice && !isBc250 && only != 6) Exit(5, "this device has no compute-only family");

    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    if (!f12.timelineSemaphore) Exit(3, "the device has no timeline semaphores");
    VkPhysicalDeviceVulkan12Features want12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    want12.timelineSemaphore = VK_TRUE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci[2] = { { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO },
                                       { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO } };
    qci[0].queueFamilyIndex = gfxFam;
    qci[0].queueCount = 1;
    qci[0].pQueuePriorities = &priority;
    qci[1].queueFamilyIndex = cmpFam;
    qci[1].queueCount = 1;
    qci[1].pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &want12;
    dci.queueCreateInfoCount = cmpFam != NONE ? 2 : 1;
    dci.pQueueCreateInfos = qci;
    VK(vkCreateDevice(phys, &dci, NULL, &dev));
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(dev, gfxFam, 0, &gfxQ);
    if (cmpFam != NONE) vkGetDeviceQueue(dev, cmpFam, 0, &cmpQ);
    printf("queues        graphics family %u, compute family %s\n", gfxFam, cmpFam != NONE ? "present" : "absent");
    if (cmpFam != NONE) printf("compute fam   %u\n", cmpFam);
    fflush(stdout);

    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlci.bindingCount = 1;
    dlci.pBindings = &binding;
    VK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    VkShaderModule module;
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = sizeof stage_spv;
    smci.pCode = stage_spv;
    VK(vkCreateShaderModule(dev, &smci, NULL, &module));
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
    vkDestroyShaderModule(dev, module, NULL);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 64;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));

    if (only == 6) {
        Step6();
    } else {
        if (!only || only == 1) Step1();
        if (only != 1 && cmpFam == NONE) {
            step = only;
            InterlockedIncrement(&failures);
            Exit(1, "no compute-only family: steps 2 to 5 need one");
        }
        if (!only || only == 2) Step2();
        if (!only || only == 3) Step3();
        if (!only || only == 4) Step4();
        if (!only || only == 5) Step5();
    }

    VK(vkDeviceWaitIdle(dev));
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    SetEvent(deadlineEvent);
    printf("vkace result=%s checks=%ld failures=%ld exit=%d\n", failures ? "fail" : "pass", checks, failures,
           failures ? 1 : 0);
    fflush(stdout);
    return failures ? 1 : 0;
}
