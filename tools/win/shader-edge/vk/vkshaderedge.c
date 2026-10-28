/*
 * vkshaderedge: shader edge cases on a Vulkan driver, each one with a result that the GPU computes and the host
 * checks. The programs are HLSL compiled by DXC with -spirv. They use DXC's inline SPIR-V attributes to emit exact
 * instructions, so that a wrong result points at the driver (RADV and ACO for us) and not at a compiler between
 * the test and the driver. Technique and the atomic and clock cases from Erfan Ahmadi, "Inline SPIR-V"
 * (https://erfan-ahmadi.github.io/blog/inline-spirv/).
 *
 * Cases (README.md has the details):
 *   atomic-exact-ssbo, atomic-exact-bda   OpAtomicStore Release, OpAtomicLoad Acquire, OpAtomicIAdd AcquireRelease,
 *                                         OpAtomicCompareExchange, Device scope; storage buffer or device address.
 *   atomic-mp-ssbo, atomic-mp-bda         message passing between workgroups with a Release store and Acquire
 *                                         loads; a seen flag with a stale payload fails.
 *   clock-subgroup, clock-device          OpReadClockKHR (VK_KHR_shader_clock): non-zero and monotonic.
 *   half{3,5,7}-{scalar,vector}           vector<half, N> elements through a device address, strides 6, 10 and
 *                                         14 bytes: the reference for the BD-248 access class.
 *
 * Every case has a negative control (--negative-control, push constant neg = 1) that makes the GPU compute a known
 * wrong result: every case must then fail, which proves that each check can fail.
 *
 * The ICD comes from --icd (the DLL is loaded directly through vk_icdGetInstanceProcAddr, no loader), from
 * --manifest, or from the loader's own selection, as in tools/win/vkfillcheck.
 *
 * Exit: 0 every case passed or was skipped for a missing feature, 1 a case failed (under --negative-control: every
 * case failed, as required), 2 usage, 3 Vulkan or system error or a broken negative control, 4 deadline.
 */
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_atomic_exact_ssbo.h"
#include "vk_atomic_exact_bda.h"
#include "vk_atomic_mp_ssbo.h"
#include "vk_atomic_mp_bda.h"
#include "vk_clock_subgroup.h"
#include "vk_clock_device.h"
#include "vk_half3_scalar.h"
#include "vk_half5_scalar.h"
#include "vk_half7_scalar.h"
#include "vk_half3_vector.h"
#include "vk_half5_vector.h"
#include "vk_half7_vector.h"

#define BC250_VENDOR_ID 0x1002u
#define BC250_DEVICE_ID 0x13FEu

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdCopyBuffer) \
    X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdDispatch) X(vkGetBufferDeviceAddress) X(vkCmdFillBuffer)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;

/* The push constant block of shaders/inline_spirv.hlsli, byte for byte. */
typedef struct {
    uint64_t a0, a1, a2;
    uint32_t n, spin, neg, pad;
} Push;

enum { KIND_EXACT, KIND_MP, KIND_CLOCK, KIND_HALF };
enum { NEED_NONE, NEED_SUBGROUP_CLOCK, NEED_DEVICE_CLOCK, NEED_HALF };

typedef struct {
    const char *name;
    const unsigned char *code;
    size_t size;
    int kind;
    int bda;        /* the program takes its memory from pc.a0 / pc.a1, not from binding 0 */
    int need;
    uint32_t n;     /* threads, or elements of a half case */
    uint32_t spin;  /* bound of the message passing wait, or the work steps between two clock reads */
    uint32_t comps; /* half cases: N */
} Case;

#define CODE(x) x, sizeof(x)
static const Case kCases[] = {
    { "atomic-exact-ssbo", CODE(g_vk_atomic_exact_ssbo), KIND_EXACT, 0, NEED_NONE, 1024, 0, 0 },
    { "atomic-exact-bda", CODE(g_vk_atomic_exact_bda), KIND_EXACT, 1, NEED_NONE, 1024, 0, 0 },
    { "atomic-mp-ssbo", CODE(g_vk_atomic_mp_ssbo), KIND_MP, 0, NEED_NONE, 4096, 1u << 16, 0 },
    { "atomic-mp-bda", CODE(g_vk_atomic_mp_bda), KIND_MP, 1, NEED_NONE, 4096, 1u << 16, 0 },
    { "clock-subgroup", CODE(g_vk_clock_subgroup), KIND_CLOCK, 0, NEED_SUBGROUP_CLOCK, 256, 4096, 0 },
    { "clock-device", CODE(g_vk_clock_device), KIND_CLOCK, 0, NEED_DEVICE_CLOCK, 256, 4096, 0 },
    { "half3-scalar", CODE(g_vk_half3_scalar), KIND_HALF, 1, NEED_HALF, 1000, 0, 3 },
    { "half5-scalar", CODE(g_vk_half5_scalar), KIND_HALF, 1, NEED_HALF, 1000, 0, 5 },
    { "half7-scalar", CODE(g_vk_half7_scalar), KIND_HALF, 1, NEED_HALF, 1000, 0, 7 },
    { "half3-vector", CODE(g_vk_half3_vector), KIND_HALF, 1, NEED_HALF, 1000, 0, 3 },
    { "half5-vector", CODE(g_vk_half5_vector), KIND_HALF, 1, NEED_HALF, 1000, 0, 5 },
    { "half7-vector", CODE(g_vk_half7_vector), KIND_HALF, 1, NEED_HALF, 1000, 0, 7 },
};
#define CASE_COUNT (uint32_t)(sizeof(kCases) / sizeof(kCases[0]))

/* One work buffer serves every case. The storage buffer cases use it from byte 0 through binding 0; the
 * device address cases start at BDA_OFFSET, so that a wrong base address cannot read the right data by luck. */
#define WORK_SIZE (1u << 20)
#define BDA_OFFSET 4096u
/* Half cases: input and output regions, each with guard bytes before and after the elements. */
#define HALF_GUARD 256u
#define HALF_IN 4096u
#define HALF_OUT (512u * 1024u)
#define HALF_SENTINEL 0xA5u

enum { CASE_PASS, CASE_FAIL, CASE_SKIP };
enum { PLACE_HOST, PLACE_LOCAL, PLACE_COUNT };
static const char *const kPlaceNames[PLACE_COUNT] = { "host", "local" };

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memProps;
static VkCommandPool pool;
static VkFence fence;
static VkDescriptorSetLayout setLayout;
static VkPipelineLayout pipeLayout;
static VkDescriptorPool descPool;
static uint32_t fenceTimeoutMs = 10000;
static uint32_t negative;
static int haveSubgroupClock, haveDeviceClock, haveHalf;
static LARGE_INTEGER qpcFreq;
static HANDLE deadlineEvent;
static uint32_t deadlineSec = 60;

static void Fail(const char *what, VkResult r)
{
    fprintf(stderr, "vkshaderedge: %s failed: VkResult %d\n", what, (int)r);
    exit(3);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static void Missing(const char *name)
{
    fprintf(stderr, "vkshaderedge: %s not available\n", name);
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

/* A hung GPU must not hang the gate: the watchdog ends the process once the deadline has passed, even if a wait
 * inside the driver never returns. Standard output is unbuffered, so nothing printed is lost. */
static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(deadlineEvent, (DWORD)(deadlineSec * 1000u + 2000u)) == WAIT_TIMEOUT) {
        fprintf(stderr, "vkshaderedge: deadline of %u s exceeded, ending the process\n", deadlineSec);
        fflush(stderr);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    return 0;
}

/* ---- the ICD and its witness ---------------------------------------------------------------------- */

static int Sha256File(const wchar_t *path, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    unsigned char buffer[65536];
    HANDLE file;
    int ok = 0;
    out[0] = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        DWORD read = 0;
        ok = 1;
        while (ReadFile(file, buffer, sizeof(buffer), &read, NULL) && read)
            if (BCryptHashData(hash, buffer, read, 0) != 0) { ok = 0; break; }
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) ok = 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (ok)
        for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", digest[i]);
    return ok;
}

/* Every loaded module that exports vk_icdGetInstanceProcAddr, with the hash of its file: the route witness. */
static void PrintIcdModules(void)
{
    HMODULE modules[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    if (needed > sizeof(modules)) needed = sizeof(modules);
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t path[MAX_PATH * 2];
        char sha[65];
        if (!GetProcAddress(modules[i], "vk_icdGetInstanceProcAddr")) continue;
        if (!GetModuleFileNameW(modules[i], path, (DWORD)(sizeof(path) / sizeof(path[0])))) continue;
        printf("icd module    %ls sha256 %s\n", path, Sha256File(path, sha) ? sha : "unavailable");
    }
}

/* ---- buffers -------------------------------------------------------------------------------------- */

typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint32_t type; uint8_t *map; VkDeviceAddress address; } Buf;

static int FindType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return (int)i;
    }
    return -1;
}

static int PickType(uint32_t bits, int place)
{
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                hc = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int t;
    if (place == PLACE_LOCAL) {
        if ((t = FindType(bits, dl, hv)) < 0) t = FindType(bits, dl, 0);
        return t;
    }
    if ((t = FindType(bits, hv | hc, dl)) < 0) t = FindType(bits, hv | hc, 0);
    return t;
}

static Buf MakeBuf(VkDeviceSize size, int place, int deviceAddress)
{
    Buf b = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0, NULL, 0 };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    int t;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK(vkCreateBuffer(dev, &bi, NULL, &b.buffer));
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    t = PickType(req.memoryTypeBits, place);
    if (t < 0) {
        fprintf(stderr, "vkshaderedge: no %s memory type (bits 0x%X)\n", kPlaceNames[place], req.memoryTypeBits);
        exit(3);
    }
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (deviceAddress) {
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        ai.pNext = &flags;
    }
    VK(vkAllocateMemory(dev, &ai, NULL, &b.memory));
    VK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
    b.type = (uint32_t)t;
    if (memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK(vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, (void **)&b.map));
    if (deviceAddress) {
        VkBufferDeviceAddressInfo dai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        dai.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(dev, &dai);
    }
    return b;
}

static void FreeBuf(Buf *b)
{
    if (b->map) vkUnmapMemory(dev, b->memory);
    if (b->buffer) vkDestroyBuffer(dev, b->buffer, NULL);
    if (b->memory) vkFreeMemory(dev, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

/* ---- one submission: seed copy, dispatch, readback copy ------------------------------------------- */

typedef struct {
    Buf work;      /* the buffer the programs use, host or device local */
    Buf stage;     /* host-visible seed and readback buffer */
    VkDescriptorSet set;
} Fixture;

static void Barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                    VkAccessFlags dstAccess)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Copies the seed from stage to work, runs the program, copies work back to stage. With seed = 0 the work buffer
 * keeps what the previous submission left in it, which the second clock run needs. */
static int Submit(Fixture *f, VkPipeline pipe, const Push *push, uint32_t groups, int seed)
{
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cb;
    VkBufferCopy region = { 0, 0, WORK_SIZE };
    VkResult r;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &cai, &cb));
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb, &cbi));
    if (seed) {
        vkCmdCopyBuffer(cb, f->stage.buffer, f->work.buffer, 1, &region);
        Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &f->set, 0, NULL);
    vkCmdPushConstants(cb, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), push);
    vkCmdDispatch(cb, groups, 1, 1);
    Barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyBuffer(cb, f->work.buffer, f->stage.buffer, 1, &region);
    Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    VK(vkEndCommandBuffer(cb));
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK(vkResetFences(dev, 1, &fence));
    VK(vkQueueSubmit(queue, 1, &si, fence));
    r = vkWaitForFences(dev, 1, &fence, VK_TRUE, (uint64_t)fenceTimeoutMs * 1000000ull);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
    if (r == VK_TIMEOUT) {
        fprintf(stderr, "vkshaderedge: the fence did not signal within %u ms\n", fenceTimeoutMs);
        exit(3);
    }
    if (r != VK_SUCCESS) Fail("vkWaitForFences", r);
    return 0;
}

static VkPipeline MakePipeline(const Case *c, const char *entry)
{
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModule module;
    VkPipeline pipe;
    /* The headers hold bytes; vkCreateShaderModule needs 4-byte aligned words. */
    uint32_t *words = (uint32_t *)malloc(c->size);
    if (!words) { fprintf(stderr, "vkshaderedge: out of host memory\n"); exit(3); }
    memcpy(words, c->code, c->size);
    smi.codeSize = c->size;
    smi.pCode = words;
    VK(vkCreateShaderModule(dev, &smi, NULL, &module));
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = entry;
    cpi.layout = pipeLayout;
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    vkDestroyShaderModule(dev, module, NULL);
    free(words);
    return pipe;
}

/* ---- checks --------------------------------------------------------------------------------------- */

static uint32_t *Words(Fixture *f, const Case *c) { return (uint32_t *)(f->stage.map + (c->bda ? BDA_OFFSET : 0)); }

static int CheckAtomicExact(const Case *c, const uint32_t *m, char *why, size_t whySize)
{
    const uint32_t n = c->n, counter = n, cas = n + 1, res = n + 1 + n / 64;
    uint8_t *seen = (uint8_t *)calloc(n, 1);
    int ok = 1;
    if (!seen) { fprintf(stderr, "vkshaderedge: out of host memory\n"); exit(3); }
    for (uint32_t t = 0; t < n && ok; t++) {
        if (m[t] != 3 * t + 1) {
            snprintf(why, whySize, "slot %u holds %u, want %u", t, m[t], 3 * t + 1);
            ok = 0;
        } else if (m[res + 3 * t] != 3 * t + 1) {
            snprintf(why, whySize, "thread %u loaded %u after its release store of %u", t, m[res + 3 * t], 3 * t + 1);
            ok = 0;
        } else if (m[res + 3 * t + 1] >= n || seen[m[res + 3 * t + 1]]++) {
            snprintf(why, whySize, "thread %u got counter value %u twice or out of range", t, m[res + 3 * t + 1]);
            ok = 0;
        }
    }
    if (ok && m[counter] != n) {
        snprintf(why, whySize, "counter is %u after %u adds", m[counter], n);
        ok = 0;
    }
    for (uint32_t g = 0; g < n / 64 && ok; g++) {
        uint32_t winners = 0, winner = 0;
        for (uint32_t t = g * 64; t < g * 64 + 64; t++)
            if (m[res + 3 * t + 2] == 0) { winners++; winner = t; }
        if (winners != 1) {
            snprintf(why, whySize, "workgroup %u has %u compare-exchange winners, want 1", g, winners);
            ok = 0;
            break;
        }
        if (m[cas + g] != winner + 1) {
            snprintf(why, whySize, "workgroup %u slot holds %u, its winner %u stored %u", g, m[cas + g], winner,
                     winner + 1);
            ok = 0;
            break;
        }
        for (uint32_t t = g * 64; t < g * 64 + 64 && ok; t++)
            if (t != winner && m[res + 3 * t + 2] != winner + 1) {
                snprintf(why, whySize, "thread %u saw %u in a slot that only ever held 0 and %u", t,
                         m[res + 3 * t + 2], winner + 1);
                ok = 0;
            }
    }
    free(seen);
    return ok;
}

static uint32_t Payload(uint32_t t, uint32_t j) { return ((t * 2654435761u) ^ (j * 0x9E3779B9u + 0x01234567u)) | 1u; }

static int CheckAtomicMp(const Case *c, const uint32_t *m, char *why, size_t whySize, uint32_t *observed)
{
    const uint32_t n = c->n, flags = 4 * n, res = 5 * n;
    uint32_t seen = 0, stale = 0, firstStale = UINT32_MAX;
    for (uint32_t t = 0; t < n; t++) {
        for (uint32_t j = 0; j < 4; j++)
            if (m[4 * t + j] != Payload(t, j)) {
                snprintf(why, whySize, "payload word %u of thread %u is 0x%08X after the dispatch", j, t, m[4 * t + j]);
                return 0;
            }
        if (m[flags + t] != 1) {
            snprintf(why, whySize, "flag of thread %u is %u after the dispatch", t, m[flags + t]);
            return 0;
        }
        if (m[res + t] & 1u) seen++;
        if (m[res + t] >> 8) { stale++; if (firstStale == UINT32_MAX) firstStale = t; }
    }
    *observed = seen;
    if (stale) {
        snprintf(why, whySize, "%u of %u threads saw the release flag with a stale payload (first: thread %u, %u words)",
                 stale, seen, firstStale, m[res + firstStale] >> 8);
        return 0;
    }
    if (!seen) {
        snprintf(why, whySize, "no thread saw its partner's flag within %u loads; nothing was tested", c->spin);
        return 0;
    }
    snprintf(why, whySize, "%u of %u threads saw the flag within %u loads, every payload current", seen, n, c->spin);
    return 1;
}

static uint64_t Clock(const uint32_t *m, uint32_t t, int second)
{
    return (uint64_t)m[5 * t + 2 * second] | ((uint64_t)m[5 * t + 2 * second + 1] << 32);
}

static int CheckClock(const Case *c, const uint32_t *m, char *why, size_t whySize, uint64_t *lo, uint64_t *hi)
{
    uint32_t increased = 0;
    *lo = UINT64_MAX;
    *hi = 0;
    for (uint32_t t = 0; t < c->n; t++) {
        const uint64_t a = Clock(m, t, 0), b = Clock(m, t, 1);
        if (!a || !b) {
            snprintf(why, whySize, "thread %u read a zero clock (0x%llX, 0x%llX)", t, (unsigned long long)a,
                     (unsigned long long)b);
            return 0;
        }
        if (b < a) {
            snprintf(why, whySize, "thread %u: second read 0x%llX is before the first 0x%llX", t,
                     (unsigned long long)b, (unsigned long long)a);
            return 0;
        }
        if (b > a) increased++;
        if (a < *lo) *lo = a;
        if (b > *hi) *hi = b;
    }
    if (!increased) {
        snprintf(why, whySize, "the clock did not advance in any of %u threads over %u work steps", c->n, c->spin);
        return 0;
    }
    snprintf(why, whySize, "advanced in %u of %u threads, span %llu ticks", increased, c->n,
             (unsigned long long)(*hi - *lo));
    return 1;
}

/* binary16 encoding of the values the half cases use. Inputs are (t % 256) + c % 4 / 4 and outputs at most
 * 255.75 * 2 + 6 = 517.5: never more than 11 significant bits, so every value is exact and this truncating
 * conversion is the right one. (t % 512 would give 1025.5, 12 bits, which the GPU rounds.) */
static uint16_t HalfOf(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    if (v == 0.0f) return 0;
    {
        const int exp = (int)((bits >> 23) & 0xFF) - 127 + 15;
        return (uint16_t)(((bits >> 16) & 0x8000u) | ((uint32_t)exp << 10) | ((bits >> 13) & 0x3FFu));
    }
}

static float HalfValue(uint32_t t, uint32_t c) { return (float)(t % 256u) + 0.25f * (float)(c % 4u); }

static void SeedHalf(const Case *c, uint8_t *stage)
{
    const uint32_t stride = 2 * c->comps, span = c->n * c->comps * stride; /* N times the elements: neg control */
    memset(stage + HALF_IN - HALF_GUARD, HALF_SENTINEL, span + 2 * HALF_GUARD);
    memset(stage + HALF_OUT - HALF_GUARD, HALF_SENTINEL, span + 2 * HALF_GUARD);
    for (uint32_t t = 0; t < c->n; t++)
        for (uint32_t k = 0; k < c->comps; k++) {
            const uint16_t h = HalfOf(HalfValue(t, k));
            memcpy(stage + HALF_IN + t * stride + 2 * k, &h, 2);
        }
}

static int CheckHalf(const Case *c, const uint8_t *stage, char *why, size_t whySize)
{
    const uint32_t stride = 2 * c->comps, used = c->n * stride, span = c->n * c->comps * stride;
    const uint8_t *out = stage + HALF_OUT;
    for (uint32_t b = 0; b < HALF_GUARD; b++)
        if (out[(int32_t)b - (int32_t)HALF_GUARD] != HALF_SENTINEL) {
            snprintf(why, whySize, "guard byte %u before the output changed", HALF_GUARD - b);
            return 0;
        }
    for (uint32_t t = 0; t < c->n; t++)
        for (uint32_t k = 0; k < c->comps; k++) {
            const uint16_t want = HalfOf(HalfValue(t, k) * 2.0f + (float)k);
            uint16_t got;
            memcpy(&got, out + t * stride + 2 * k, 2);
            if (got != want) {
                snprintf(why, whySize, "element %u component %u (byte %u): got 0x%04X want 0x%04X", t, k,
                         t * stride + 2 * k, got, want);
                return 0;
            }
        }
    for (uint32_t b = used; b < span + HALF_GUARD; b++)
        if (out[b] != HALF_SENTINEL) {
            snprintf(why, whySize, "byte %u after the %u output bytes changed to 0x%02X", b - used, used, out[b]);
            return 0;
        }
    snprintf(why, whySize, "%u elements of %u bytes exact, %u guard bytes intact", c->n, stride,
             2 * HALF_GUARD + span - used);
    return 1;
}

/* ---- one case ------------------------------------------------------------------------------------- */

static int RunCase(const Case *c, Fixture *f, int place)
{
    char why[256] = "";
    const uint32_t threads = c->n, groups = (threads + 63) / 64;
    const uint64_t base = f->work.address + (c->bda ? BDA_OFFSET : 0);
    Push push;
    VkPipeline pipe;
    int ok = 0;
    double t0 = NowMs();
    const char *entry = c->kind == KIND_EXACT ? "exact_main" : c->kind == KIND_MP ? "mp_main"
                      : c->kind == KIND_CLOCK ? "clock_main" : "half_main";

    if ((c->need == NEED_SUBGROUP_CLOCK && !haveSubgroupClock) || (c->need == NEED_DEVICE_CLOCK && !haveDeviceClock) ||
        (c->need == NEED_HALF && !haveHalf)) {
        printf("SKIP %-18s %-5s the device lacks %s\n", c->name, kPlaceNames[place],
               c->need == NEED_SUBGROUP_CLOCK ? "VK_KHR_shader_clock shaderSubgroupClock"
               : c->need == NEED_DEVICE_CLOCK ? "VK_KHR_shader_clock shaderDeviceClock"
                                              : "shaderFloat16 or storageBuffer16BitAccess");
        return CASE_SKIP;
    }
    memset(&push, 0, sizeof(push));
    push.n = c->n;
    push.spin = c->spin;
    push.neg = negative;
    memset(f->stage.map, 0, WORK_SIZE);
    pipe = MakePipeline(c, entry);

    if (c->kind == KIND_HALF) {
        push.a0 = f->work.address + HALF_IN;
        push.a1 = f->work.address + HALF_OUT;
        SeedHalf(c, f->stage.map);
        Submit(f, pipe, &push, groups, 1);
        ok = CheckHalf(c, f->stage.map, why, sizeof(why));
    } else if (c->kind == KIND_EXACT) {
        push.a0 = base;
        Submit(f, pipe, &push, groups, 1);
        ok = CheckAtomicExact(c, Words(f, c), why, sizeof(why));
        if (ok) snprintf(why, sizeof(why), "%u threads: release/acquire values, %u adds, %u compare-exchange slots",
                         c->n, c->n, c->n / 64);
    } else if (c->kind == KIND_MP) {
        uint32_t observed = 0;
        push.a0 = base;
        Submit(f, pipe, &push, groups, 1);
        ok = CheckAtomicMp(c, Words(f, c), why, sizeof(why), &observed);
    } else {
        uint64_t lo0, hi0, lo1, hi1;
        Submit(f, pipe, &push, groups, 1);
        ok = CheckClock(c, Words(f, c), why, sizeof(why), &lo0, &hi0);
        if (ok && c->need == NEED_DEVICE_CLOCK) {
            /* A second submission after the first one has completed: the device clock must not go back. */
            Submit(f, pipe, &push, groups, 1);
            ok = CheckClock(c, Words(f, c), why, sizeof(why), &lo1, &hi1);
            if (ok && lo1 < hi0) {
                snprintf(why, sizeof(why), "second submission read 0x%llX, before 0x%llX of the first",
                         (unsigned long long)lo1, (unsigned long long)hi0);
                ok = 0;
            } else if (ok) {
                snprintf(why, sizeof(why), "two submissions in order, %llu ticks from the first read to the last",
                         (unsigned long long)(hi1 - lo0));
            }
        }
    }
    vkDestroyPipeline(dev, pipe, NULL);
    printf("%s %-18s %-5s %s (%.1f ms)\n", ok ? "PASS" : "FAIL", c->name, kPlaceNames[place], why, NowMs() - t0);
    return ok ? CASE_PASS : CASE_FAIL;
}

/* ---- main ----------------------------------------------------------------------------------------- */

static void Usage(FILE *out, int code)
{
    fprintf(out,
            "usage: vkshaderedge [--icd DLL | --manifest JSON] [--device N] [--any-device] [--list-devices]\n"
            "                    [--placement host|local|both] [--case NAME] [--negative-control]\n"
            "                    [--deadline SEC] [--fence-timeout-ms N] [--help]\n"
            "\n"
            "  --icd DLL           load this ICD directly (absolute path), without the Vulkan loader.\n"
            "  --manifest JSON     hand this ICD manifest to the Vulkan loader.\n"
            "  --device N          use physical device N (default: the BC-250).\n"
            "  --any-device        accept a device that is not a BC-250 (the reference run on another GPU).\n"
            "  --list-devices      print the physical devices and exit.\n"
            "  --placement         memory of the work buffer (default both).\n"
            "  --case NAME         run only the cases whose name starts with NAME.\n"
            "  --negative-control  every program computes a known wrong result; every case must fail.\n"
            "  --deadline SEC      end the run after this many seconds (default 60, at most 170).\n"
            "  --fence-timeout-ms  how long to wait for one submission (default 10000).\n"
            "\n"
            "exit: 0 every case passed or was skipped, 1 a case failed, 2 usage, 3 Vulkan or system error,\n"
            "      4 deadline.\n");
    exit(code);
}

static int HasExtension(VkPhysicalDevice phys, const char *name)
{
    uint32_t count = 0;
    VkExtensionProperties *list;
    int found = 0;
    VK(vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL));
    list = (VkExtensionProperties *)calloc(count ? count : 1, sizeof(*list));
    if (!list) { fprintf(stderr, "vkshaderedge: out of host memory\n"); exit(3); }
    VK(vkEnumerateDeviceExtensionProperties(phys, NULL, &count, list));
    for (uint32_t i = 0; i < count && !found; i++) found = !strcmp(list[i].extensionName, name);
    free(list);
    return found;
}

int main(int argc, char **argv)
{
    const char *icdPath = NULL, *manifestPath = NULL, *only = NULL;
    uint32_t deviceIndex = UINT32_MAX;
    int anyDevice = 0, listDevices = 0, placeFirst = PLACE_HOST, placeLast = PLACE_LOCAL, lateDeadline = 0;
    HMODULE library;
    VkInstance instance;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice phys[16];
    uint32_t physCount = 16, familyCount = 16, family = UINT32_MAX;
    VkQueueFamilyProperties families[16];
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceShaderClockFeaturesKHR clockFeat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceFeatures2 feat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceShaderClockFeaturesKHR clockOn = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR };
    VkPhysicalDeviceVulkan12Features on12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features on11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceFeatures2 featOn = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    const char *clockExt = VK_KHR_SHADER_CLOCK_EXTENSION_NAME;
    int haveClockExt;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo cpc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dsl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, PLACE_COUNT };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    Fixture fx[PLACE_COUNT];
    uint32_t passed = 0, failed = 0, skipped = 0, ran = 0;
    double start;
    HANDLE watchdog;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) Usage(stdout, 0);
        if (!strcmp(a, "--any-device")) { anyDevice = 1; continue; }
        if (!strcmp(a, "--list-devices")) { listDevices = 1; continue; }
        if (!strcmp(a, "--negative-control")) { negative = 1; continue; }
        if (i + 1 >= argc) Usage(stderr, 2);
        if (!strcmp(a, "--icd")) icdPath = argv[++i];
        else if (!strcmp(a, "--manifest")) manifestPath = argv[++i];
        else if (!strcmp(a, "--device")) deviceIndex = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--case")) only = argv[++i];
        else if (!strcmp(a, "--deadline")) deadlineSec = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--fence-timeout-ms")) fenceTimeoutMs = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--placement")) {
            const char *p = argv[++i];
            if (!strcmp(p, "host")) placeFirst = placeLast = PLACE_HOST;
            else if (!strcmp(p, "local")) placeFirst = placeLast = PLACE_LOCAL;
            else if (!strcmp(p, "both")) { placeFirst = PLACE_HOST; placeLast = PLACE_LOCAL; }
            else Usage(stderr, 2);
        } else Usage(stderr, 2);
    }
    if (icdPath && manifestPath) {
        fprintf(stderr, "vkshaderedge: --icd and --manifest are alternatives\n");
        return 2;
    }
    if (deadlineSec < 5 || deadlineSec > 170 || fenceTimeoutMs < 100 || fenceTimeoutMs > 120000) Usage(stderr, 2);
    QueryPerformanceFrequency(&qpcFreq);
    start = NowMs();
    if (!(deadlineEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) {
        fprintf(stderr, "vkshaderedge: CreateEvent failed (%lu)\n", GetLastError());
        return 3;
    }
    if (!(watchdog = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL))) {
        fprintf(stderr, "vkshaderedge: CreateThread failed (%lu)\n", GetLastError());
        return 3;
    }

    if (icdPath) {
        wchar_t wide[MAX_PATH * 2];
        PFN_vkNegotiateLoaderICDInterfaceVersion negotiate;
        uint32_t icdVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        if (MultiByteToWideChar(CP_ACP, 0, icdPath, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) == 0) {
            fprintf(stderr, "vkshaderedge: --icd path is not usable\n");
            return 2;
        }
        if (!(wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/'))) {
            fprintf(stderr, "vkshaderedge: --icd needs an absolute path, got %s\n", icdPath);
            return 2;
        }
        library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            fprintf(stderr, "vkshaderedge: cannot load %s (error %lu)\n", icdPath, GetLastError());
            return 3;
        }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vk_icdGetInstanceProcAddr");
        if (!getInstanceProc)
            getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vk_icdGetInstanceProcAddr");
        negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)GetProcAddress(
            library, "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (!negotiate)
            negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)getInstanceProc(
                NULL, "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (negotiate && negotiate(&icdVersion) == VK_SUCCESS) printf("icd interface %u\n", icdVersion);
        else printf("icd interface unavailable (the module answers no negotiation)\n");
    } else {
        if (manifestPath) {
            SetEnvironmentVariableA("VK_DRIVER_FILES", manifestPath);
            SetEnvironmentVariableA("VK_ICD_FILENAMES", manifestPath);
            printf("manifest      %s\n", manifestPath);
        }
        library = LoadLibraryA("vulkan-1.dll");
        if (!library) {
            fprintf(stderr, "vkshaderedge: vulkan-1.dll not found\n");
            return 3;
        }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    }
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");

    app.pApplicationName = "vkshaderedge";
    app.apiVersion = VK_API_VERSION_1_2;
    ici.pApplicationInfo = &app;
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)
    PrintIcdModules();

    VK(vkEnumeratePhysicalDevices(instance, &physCount, phys));
    if (!physCount) {
        fprintf(stderr, "vkshaderedge: no Vulkan physical device\n");
        return 3;
    }
    for (uint32_t i = 0; i < physCount; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys[i], &p);
        printf("device %u      %s (vendor 0x%04X device 0x%04X) driver 0x%08X api %u.%u.%u%s\n", i, p.deviceName,
               p.vendorID, p.deviceID, p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               (p.vendorID == BC250_VENDOR_ID && p.deviceID == BC250_DEVICE_ID) ? "  <- BC-250" : "");
        if (deviceIndex == UINT32_MAX && p.vendorID == BC250_VENDOR_ID && p.deviceID == BC250_DEVICE_ID)
            deviceIndex = i;
    }
    if (listDevices) return 0;
    if (deviceIndex == UINT32_MAX) {
        if (!anyDevice) {
            fprintf(stderr, "vkshaderedge: no BC-250 (vendor 0x%04X device 0x%04X) among %u devices; "
                            "--any-device or --device N to test another one\n",
                    BC250_VENDOR_ID, BC250_DEVICE_ID, physCount);
            return 3;
        }
        deviceIndex = 0;
    }
    if (deviceIndex >= physCount) {
        fprintf(stderr, "vkshaderedge: %u devices, index %u\n", physCount, deviceIndex);
        return 2;
    }
    vkGetPhysicalDeviceProperties(phys[deviceIndex], &props);
    if (!anyDevice && !(props.vendorID == BC250_VENDOR_ID && props.deviceID == BC250_DEVICE_ID)) {
        fprintf(stderr, "vkshaderedge: device %u is not a BC-250; --any-device to accept it\n", deviceIndex);
        return 3;
    }
    if (props.apiVersion < VK_API_VERSION_1_2) {
        fprintf(stderr, "vkshaderedge: the device offers Vulkan %u.%u; the programs need 1.2\n",
                VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
        return 3;
    }

    /* Features. Buffer device address and 64-bit integers are required by every device address case and are
     * mandatory for the client; the clock and 16-bit features only decide which cases are skipped. */
    haveClockExt = HasExtension(phys[deviceIndex], clockExt);
    feat.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = haveClockExt ? &clockFeat : NULL;
    vkGetPhysicalDeviceFeatures2(phys[deviceIndex], &feat);
    if (!f12.bufferDeviceAddress || !feat.features.shaderInt64) {
        fprintf(stderr, "vkshaderedge: the device lacks bufferDeviceAddress or shaderInt64\n");
        return 3;
    }
    haveSubgroupClock = haveClockExt && clockFeat.shaderSubgroupClock;
    haveDeviceClock = haveClockExt && clockFeat.shaderDeviceClock;
    haveHalf = f12.shaderFloat16 && f11.storageBuffer16BitAccess;
    printf("features      bufferDeviceAddress 1, shaderInt64 1, shaderFloat16 %u, storageBuffer16BitAccess %u, "
           "%s %s (subgroup %u, device %u)\n", f12.shaderFloat16, f11.storageBuffer16BitAccess, clockExt,
           haveClockExt ? "present" : "absent", haveSubgroupClock, haveDeviceClock);

    vkGetPhysicalDeviceMemoryProperties(phys[deviceIndex], &memProps);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[deviceIndex], &familyCount, families);
    for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;
    if (family == UINT32_MAX) {
        fprintf(stderr, "vkshaderedge: no queue family can run compute programs\n");
        return 3;
    }
    printf("using         device %u, queue family %u (flags 0x%X), %u memory types\n", deviceIndex, family,
           families[family].queueFlags, memProps.memoryTypeCount);

    featOn.features.shaderInt64 = VK_TRUE;
    featOn.pNext = &on11;
    on11.pNext = &on12;
    on11.storageBuffer16BitAccess = haveHalf ? VK_TRUE : VK_FALSE;
    on12.bufferDeviceAddress = VK_TRUE;
    on12.shaderFloat16 = haveHalf ? VK_TRUE : VK_FALSE;
    if (haveSubgroupClock || haveDeviceClock) {
        clockOn.shaderSubgroupClock = haveSubgroupClock ? VK_TRUE : VK_FALSE;
        clockOn.shaderDeviceClock = haveDeviceClock ? VK_TRUE : VK_FALSE;
        on12.pNext = &clockOn;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &clockExt;
    }
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    dci.pNext = &featOn;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(phys[deviceIndex], &dci, NULL, &dev));
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(dev, family, 0, &queue);
    cpc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpc.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &cpc, NULL, &pool));
    VK(vkCreateFence(dev, &fci, NULL, &fence));
    dsl.bindingCount = 1;
    dsl.pBindings = &binding;
    VK(vkCreateDescriptorSetLayout(dev, &dsl, NULL, &setLayout));
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &pipeLayout));
    dpi.maxSets = PLACE_COUNT;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &poolSize;
    VK(vkCreateDescriptorPool(dev, &dpi, NULL, &descPool));

    memset(fx, 0, sizeof(fx));
    for (int place = placeFirst; place <= placeLast; place++) {
        VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorBufferInfo dbi;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        fx[place].work = MakeBuf(WORK_SIZE, place, 1);
        fx[place].stage = MakeBuf(WORK_SIZE, PLACE_HOST, 0);
        if (!fx[place].stage.map) {
            fprintf(stderr, "vkshaderedge: the staging buffer did not map\n");
            return 3;
        }
        dsa.descriptorPool = descPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &setLayout;
        VK(vkAllocateDescriptorSets(dev, &dsa, &fx[place].set));
        dbi.buffer = fx[place].work.buffer;
        dbi.offset = 0;
        dbi.range = VK_WHOLE_SIZE;
        w.dstSet = fx[place].set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        printf("work buffer   %-5s memory type %u (heap %u, flags 0x%03X), %u bytes\n", kPlaceNames[place],
               fx[place].work.type, memProps.memoryTypes[fx[place].work.type].heapIndex,
               memProps.memoryTypes[fx[place].work.type].propertyFlags, WORK_SIZE);
    }
    if (negative) printf("negative control: every program computes a known wrong result, every case must fail\n");

    for (int place = placeFirst; place <= placeLast; place++)
        for (uint32_t i = 0; i < CASE_COUNT; i++) {
            int result;
            if (only && strncmp(kCases[i].name, only, strlen(only))) continue;
            if (NowMs() - start > deadlineSec * 1000.0) {
                printf("deadline      %u s reached after %u cases, the run is incomplete\n", deadlineSec, ran);
                lateDeadline = 1;
                goto done;
            }
            result = RunCase(&kCases[i], &fx[place], place);
            ran++;
            if (result == CASE_PASS) passed++;
            else if (result == CASE_FAIL) failed++;
            else skipped++;
        }
done:
    VK(vkDeviceWaitIdle(dev));
    printf("cases %u: %u PASS, %u FAIL, %u SKIP\nelapsed       %.1f s\n", ran, passed, failed, skipped,
           (NowMs() - start) / 1000.0);
    for (int place = placeFirst; place <= placeLast; place++) {
        FreeBuf(&fx[place].work);
        FreeBuf(&fx[place].stage);
    }
    vkDestroyDescriptorPool(dev, descPool, NULL);
    vkDestroyPipelineLayout(dev, pipeLayout, NULL);
    vkDestroyDescriptorSetLayout(dev, setLayout, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    SetEvent(deadlineEvent);
    CloseHandle(watchdog);
    if (!ran) {
        printf("result NOTHING-RAN (no case matched)\n");
        return 2;
    }
    if (negative) {
        /* Under the negative control a passing case means its check cannot fail, which is worse than a failing
         * driver and must not look like one. */
        if (passed) {
            printf("negative control BROKEN: %u cases passed although every program computed a wrong result\n",
                   passed);
            printf("result BROKEN\n");
            return 3;
        }
        printf("negative control: every one of %u cases that ran failed, as it must\n", failed);
        printf("result FAILED (as required by the negative control)\n");
        return lateDeadline ? 4 : 1;
    }
    printf("result %s\n", failed ? "FAILED" : (lateDeadline ? "INCOMPLETE" : "ok"));
    if (failed) return 1;
    return lateDeadline ? 4 : 0;
}
