/*
 * vrsprobe: does the GPU shade coarse fragments when Vulkan asks for a fragment shading rate?
 *
 * Built for the gfx1013 (BC-250) VRS question: the GFX10.1 headers that describe the chip name no VRS
 * register, and the lab ICD of scratch\vrs-probe (RADV_BC250_VRS_PROBE) programs the GFX10.3 VRS state
 * anyway. This client gives one answer per arm: one full-viewport triangle, one draw, one submit.
 *
 * The fragment shader takes a unique id from one atomic counter and writes it, its FragCoord and
 * (with the extension) its ShadingRateKHR to every pixel it covers. Coarse 2x2 shading therefore shows
 * as (a) a counter of about a quarter of the pixels and (b) aligned 2x2 pixel blocks that share one id.
 * Fine shading shows as one id per pixel. The analysis needs no knowledge of the driver: the same client
 * on a GPU with known VRS is the positive control of the method.
 *
 * The ICD comes from --icd (loaded directly through vk_icdGetInstanceProcAddr, no loader, no manifest),
 * as in tools\win\vkfillcheck, because an elevated SSH session on the lab ignores VK_DRIVER_FILES. Without
 * --icd the system Vulkan loader is used (the positive control on a development PC).
 *
 * Exit: 0 the arm ran and its readback is complete (the RESULT lines carry the answer), 1 the arm ran
 * but the readback is incomplete, 2 usage, 3 Vulkan or system error (a missing feature included),
 * 4 deadline, 5 device lost or a fence that did not signal.
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

#include "vrsprobe_spv.h"

#define BC250_VENDOR_ID 0x1002u
#define BC250_DEVICE_ID 0x13FEu

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkGetQueryPoolResults)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;
static PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR getRates;

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memProps;
static HANDLE deadlineEvent;
static uint32_t deadlineSec = 30;
static uint32_t fenceTimeoutMs = 5000;
static LARGE_INTEGER qpcFreq;
static double startMs;

static double NowMs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpcFreq.QuadPart;
}

/* One line per step, unbuffered, so a hang shows the last step reached. */
static void Step(const char *what)
{
    printf("step %-28s t=%.1f ms\n", what, NowMs() - startMs);
}

static void Fail(const char *what, VkResult r)
{
    if (r == VK_ERROR_DEVICE_LOST) {
        printf("DEVICE_LOST at %s\n", what);
        fprintf(stderr, "vrsprobe: %s: VK_ERROR_DEVICE_LOST\n", what);
        exit(5);
    }
    fprintf(stderr, "vrsprobe: %s failed: VkResult %d\n", what, (int)r);
    exit(3);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static void Missing(const char *name)
{
    fprintf(stderr, "vrsprobe: %s not available\n", name);
    exit(3);
}
#define LOAD_INSTANCE(f) if (!(f = (PFN_##f)getInstanceProc(instance, #f))) Missing(#f);
#define LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) Missing(#f);

/* A hung GPU must not hang the arm: the watchdog ends the process after the deadline. */
static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(deadlineEvent, deadlineSec * 1000u) == WAIT_TIMEOUT) {
        printf("DEADLINE %u s exceeded, ending the process\n", deadlineSec);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    return 0;
}

static int Sha256File(const wchar_t *path, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    static unsigned char buffer[65536];
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

/* The route witness: every loaded module that exports vk_icdGetInstanceProcAddr, with its hash. */
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

/* ---- the arms ----------------------------------------------------------------------------------- */

enum { ATT_NONE, ATT_DEPTH, ATT_NODEPTH };

typedef struct {
    const char *name;
    int needsFsr;          /* needs VK_KHR_fragment_shading_rate */
    int fsrState;          /* chains VkPipelineFragmentShadingRateStateCreateInfoKHR */
    uint32_t fw, fh;       /* pipeline fragment size */
    VkFragmentShadingRateCombinerOpKHR op0, op1;
    int vsRate;            /* 0: plain VS; else the PrimitiveShadingRateKHR value the VS writes */
    int att;               /* ATT_*: rate attachment, left half 1x1 and right half 2x2 */
    const char *expect;    /* expected shape of the whole image, or "left=1x1,right=2x2" */
    const char *what;
} Arm;

#define KEEP VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR
#define REPL VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR
static const Arm kArms[] = {
    { "plain",    0, 0, 1, 1, KEEP, KEEP, 0, ATT_NONE,    "1x1", "no shading rate state, no rate read (works without the extension)" },
    { "pipe1x1",  1, 1, 1, 1, KEEP, KEEP, 0, ATT_NONE,    "1x1", "pipeline rate 1x1" },
    { "pipe2x2",  1, 1, 2, 2, KEEP, KEEP, 0, ATT_NONE,    "2x2", "pipeline rate 2x2" },
    { "pipe2x1",  1, 1, 2, 1, KEEP, KEEP, 0, ATT_NONE,    "2x1", "pipeline rate 2 wide, 1 high" },
    { "pipe1x2",  1, 1, 1, 2, KEEP, KEEP, 0, ATT_NONE,    "1x2", "pipeline rate 1 wide, 2 high" },
    { "prim2x2",  1, 1, 1, 1, REPL, KEEP, 5, ATT_NONE,    "2x2", "vertex shader writes 2x2, combiner 0 REPLACE" },
    { "primkeep", 1, 1, 1, 1, KEEP, KEEP, 5, ATT_NONE,    "1x1", "vertex shader writes 2x2, combiner 0 KEEP (control: rate discarded)" },
    { "att2x2",   1, 1, 1, 1, KEEP, REPL, 0, ATT_DEPTH,   "left=1x1,right=2x2", "rate attachment, depth attachment bound, combiner 1 REPLACE" },
    { "attnd2x2", 1, 1, 1, 1, KEEP, REPL, 0, ATT_NODEPTH, "left=1x1,right=2x2", "rate attachment, no depth attachment, combiner 1 REPLACE" },
};
#define ARM_COUNT (sizeof(kArms) / sizeof(kArms[0]))

/* ---- analysis ----------------------------------------------------------------------------------- */

typedef struct { uint32_t x0, y0, x1, y1; } Rect;

static int Near(double v, double want) { return v > want * 0.98 && v < want * 1.02; }

/* px: W x H texels of uvec4 (id, fragcoord, rate, marker). total: the counter after the draw. Prints
 * one RESULT line for the region and returns 1 when its shape equals 'expect'. */
static int Analyze(const char *arm, const char *region, Rect r, const uint32_t *px, uint32_t W, uint32_t total,
                   const char *expect, uint8_t *seen)
{
    uint32_t pixels = (r.x1 - r.x0) * (r.y1 - r.y0), written = 0, distinct = 0, badId = 0;
    uint32_t n22 = 0, u22 = 0, n21 = 0, u21 = 0, n12 = 0, u12 = 0;
    uint32_t rateVal[4] = { 0 }, rateCnt[4] = { 0 }, rateOther = 0;
    memset(seen, 0, total ? total : 1);
    for (uint32_t y = r.y0; y < r.y1; y++)
        for (uint32_t x = r.x0; x < r.x1; x++) {
            const uint32_t *t = px + 4 * ((size_t)y * W + x);
            if (t[3] != 0xA5A5A5A5u) continue;
            written++;
            if (t[0] >= total) { badId++; continue; }
            if (!seen[t[0]]) { seen[t[0]] = 1; distinct++; }
            int k;
            for (k = 0; k < 4; k++)
                if (rateCnt[k] && rateVal[k] == t[2]) { rateCnt[k]++; break; }
            if (k == 4) {
                for (k = 0; k < 4 && rateCnt[k]; k++) {}
                if (k < 4) { rateVal[k] = t[2]; rateCnt[k] = 1; } else rateOther++;
            }
        }
#define ID(xx, yy) (px[4 * ((size_t)(yy) * W + (xx))])
    for (uint32_t y = r.y0; y < r.y1; y += 2)
        for (uint32_t x = r.x0; x < r.x1; x += 2) {
            n22++;
            if (ID(x, y) == ID(x + 1, y) && ID(x, y) == ID(x, y + 1) && ID(x, y) == ID(x + 1, y + 1)) u22++;
        }
    for (uint32_t y = r.y0; y < r.y1; y++)
        for (uint32_t x = r.x0; x < r.x1; x += 2) { n21++; if (ID(x, y) == ID(x + 1, y)) u21++; }
    for (uint32_t y = r.y0; y < r.y1; y += 2)
        for (uint32_t x = r.x0; x < r.x1; x++) { n12++; if (ID(x, y) == ID(x, y + 1)) u12++; }
#undef ID
    double ratio = pixels ? (double)distinct / (double)pixels : 0.0;
    const char *shape = "mixed";
    if (written != pixels || badId) shape = "incomplete";
    else if (Near(ratio, 1.0) && u21 == 0 && u12 == 0) shape = "1x1";
    else if (Near(ratio, 0.25) && u22 == n22) shape = "2x2";
    else if (Near(ratio, 0.5) && u21 == n21 && u12 == 0) shape = "2x1";
    else if (Near(ratio, 0.5) && u12 == n12 && u21 == 0) shape = "1x2";
    char rates[160];
    int n = 0;
    rates[0] = 0;
    for (int k = 0; k < 4 && rateCnt[k]; k++)
        n += snprintf(rates + n, sizeof(rates) - (size_t)n, "%s0x%X:%u", k ? "," : "", rateVal[k], rateCnt[k]);
    if (rateOther) snprintf(rates + n, sizeof(rates) - (size_t)n, ",other:%u", rateOther);
    int match = !strcmp(shape, expect);
    printf("RESULT arm=%s region=%s expect=%s shape=%s match=%d pixels=%u written=%u distinct_ids=%u bad_ids=%u "
           "ratio=%.4f uniform2x2=%u/%u uniform2x1=%u/%u uniform1x2=%u/%u rate_read=%s\n",
           arm, region, expect, shape, match, pixels, written, distinct, badId, ratio, u22, n22, u21, n21, u12, n12,
           rates);
    return match;
}

/* Host self-test of the analysis: synthetic 1x1, 2x2, 2x1 images and an incomplete one must be classified
 * as such. Run by the build, so the classification is known to be able to say "2x2" before the lab
 * depends on it. */
static int SelfTest(void)
{
    enum { W = 32, H = 32 };
    static uint32_t px[W * H * 4];
    static uint8_t seen[W * H];
    const struct { uint32_t bw, bh; const char *shape; int hole; } cases[] = {
        { 1, 1, "1x1", 0 }, { 2, 2, "2x2", 0 }, { 2, 1, "2x1", 0 }, { 1, 2, "1x2", 0 }, { 2, 2, "incomplete", 1 },
    };
    int bad = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t ids = 0;
        for (uint32_t by = 0; by < H; by += cases[c].bh)
            for (uint32_t bx = 0; bx < W; bx += cases[c].bw, ids++)
                for (uint32_t y = by; y < by + cases[c].bh; y++)
                    for (uint32_t x = bx; x < bx + cases[c].bw; x++) {
                        uint32_t *t = px + 4 * (y * W + x);
                        t[0] = ids; t[1] = 0; t[2] = 0; t[3] = 0xA5A5A5A5u;
                    }
        if (cases[c].hole) px[4 * (5 * W + 7) + 3] = 0xFFFFFFFFu;
        Rect all = { 0, 0, W, H };
        if (!Analyze("selftest", "all", all, px, W, ids, cases[c].shape, seen)) bad++;
    }
    printf("selftest %s (%d of %u cases misclassified)\n", bad ? "FAIL" : "PASS", bad,
           (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return bad ? 1 : 0;
}

/* ---- Vulkan helpers ----------------------------------------------------------------------------- */

static uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "vrsprobe: no memory type with flags 0x%x in 0x%x\n", want, bits);
    exit(3);
}

typedef struct { VkBuffer b; VkDeviceMemory m; void *map; } Buf;

static Buf MakeBuf(VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buf r;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bi.size = size;
    bi.usage = usage;
    VK(vkCreateBuffer(dev, &bi, NULL, &r.b));
    vkGetBufferMemoryRequirements(dev, r.b, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory(dev, &ai, NULL, &r.m));
    VK(vkBindBufferMemory(dev, r.b, r.m, 0));
    VK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.map));
    memset(r.map, 0, (size_t)size);
    return r;
}

static VkImage MakeImage(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    VkImage img;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory mem;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VK(vkCreateImage(dev, &ii, NULL, &img));
    vkGetImageMemoryRequirements(dev, img, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(vkAllocateMemory(dev, &ai, NULL, &mem));
    VK(vkBindImageMemory(dev, img, mem, 0));
    return img;
}

static VkImageView MakeView(VkImage img, VkFormat format, VkImageAspectFlags aspect)
{
    VkImageView v;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK(vkCreateImageView(dev, &vi, NULL, &v));
    return v;
}

static void ImageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
                         VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                         VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkShaderModule Module(const uint32_t *code, size_t bytes)
{
    VkShaderModule m;
    VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = bytes;
    si.pCode = code;
    VK(vkCreateShaderModule(dev, &si, NULL, &m));
    return m;
}

static int HasExtension(const VkExtensionProperties *ext, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ext[i].extensionName, name)) return 1;
    return 0;
}

static void Usage(FILE *out, int code)
{
    fprintf(out,
            "usage: vrsprobe --arm NAME [--icd ABSOLUTE_PATH] [--size N] [--any-device] [--deadline SEC]\n"
            "                [--fence-timeout-ms N] [--dump FILE]\n"
            "       vrsprobe --caps [--icd ...]     print the shading rate capabilities and exit\n"
            "       vrsprobe --self-test            check the image analysis on synthetic images (no GPU)\n"
            "       vrsprobe --list-arms\n"
            "  --size N            render target N x N, N a multiple of 32 from 64 to 1024 (default 256)\n"
            "  --deadline SEC      end the process after SEC seconds (default 30, at most 170)\n"
            "  --fence-timeout-ms  wait at most this long for the one submission (default 5000)\n"
            "  --dump FILE         write the raw W x H x uvec4 readback to FILE\n"
            "exit: 0 complete readback, 1 incomplete readback, 2 usage, 3 Vulkan/system error,\n"
            "      4 deadline, 5 device lost or fence timeout\n");
    exit(code);
}

int main(int argc, char **argv)
{
    const char *icdPath = NULL, *armName = NULL, *dumpPath = NULL;
    int anyDevice = 0, caps = 0;
    uint32_t size = 256;
    HMODULE library;
    VkInstance instance;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice phys[16], pd;
    uint32_t physCount = 16, deviceIndex = UINT32_MAX;
    const Arm *arm = NULL;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--help")) Usage(stdout, 0);
        else if (!strcmp(a, "--self-test")) return SelfTest();
        else if (!strcmp(a, "--list-arms")) {
            for (size_t k = 0; k < ARM_COUNT; k++)
                printf("%-9s expect %-19s %s\n", kArms[k].name, kArms[k].expect, kArms[k].what);
            return 0;
        }
        else if (!strcmp(a, "--caps")) caps = 1;
        else if (!strcmp(a, "--any-device")) anyDevice = 1;
        else if (!strcmp(a, "--icd") && more) icdPath = argv[++i];
        else if (!strcmp(a, "--arm") && more) armName = argv[++i];
        else if (!strcmp(a, "--dump") && more) dumpPath = argv[++i];
        else if (!strcmp(a, "--size") && more) size = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--deadline") && more) deadlineSec = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--fence-timeout-ms") && more) fenceTimeoutMs = (uint32_t)strtoul(argv[++i], NULL, 10);
        else Usage(stderr, 2);
    }
    if (!caps) {
        if (!armName) Usage(stderr, 2);
        for (size_t k = 0; k < ARM_COUNT; k++)
            if (!strcmp(kArms[k].name, armName)) arm = &kArms[k];
        if (!arm) { fprintf(stderr, "vrsprobe: unknown arm %s (--list-arms)\n", armName); return 2; }
    }
    if (size < 64 || size > 1024 || size % 32 || deadlineSec < 5 || deadlineSec > 170 || fenceTimeoutMs < 100 ||
        fenceTimeoutMs > 60000)
        Usage(stderr, 2);

    QueryPerformanceFrequency(&qpcFreq);
    startMs = NowMs();
    if (!(deadlineEvent = CreateEventW(NULL, TRUE, FALSE, NULL)) || !CreateThread(NULL, 0, Watchdog, NULL, 0, NULL)) {
        fprintf(stderr, "vrsprobe: watchdog setup failed (%lu)\n", GetLastError());
        return 3;
    }
    {
        char v[64];
        const char *names[] = { "RADV_BC250_VRS_PROBE", "RADV_BC250_VRS_NOOP", "MESA_SHADER_CACHE_DISABLE", "RADV_DEBUG" };
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            printf("env %-26s %s\n", names[k], GetEnvironmentVariableA(names[k], v, sizeof(v)) ? v : "(unset)");
    }
    if (arm) printf("arm %s: %s; expect %s; size %ux%u\n", arm->name, arm->what, arm->expect, size, size);

    Step("load-icd");
    if (icdPath) {
        wchar_t wide[MAX_PATH * 2];
        PFN_vkNegotiateLoaderICDInterfaceVersion negotiate;
        uint32_t icdVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        if (MultiByteToWideChar(CP_ACP, 0, icdPath, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) == 0 ||
            !(wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/'))) {
            fprintf(stderr, "vrsprobe: --icd needs an absolute path, got %s\n", icdPath);
            return 2;
        }
        library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) { fprintf(stderr, "vrsprobe: cannot load %s (error %lu)\n", icdPath, GetLastError()); return 3; }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vk_icdGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vk_icdGetInstanceProcAddr");
        negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)GetProcAddress(library,
                                                                             "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (negotiate && negotiate(&icdVersion) == VK_SUCCESS) printf("icd interface %u\n", icdVersion);
    } else {
        library = LoadLibraryA("vulkan-1.dll");
        if (!library) { fprintf(stderr, "vrsprobe: vulkan-1.dll not found\n"); return 3; }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    }
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");
    app.pApplicationName = "vrsprobe";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    Step("create-instance");
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)
    getRates = (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)getInstanceProc(
        instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
    PrintIcdModules();

    VK(vkEnumeratePhysicalDevices(instance, &physCount, phys));
    for (uint32_t i = 0; i < physCount; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys[i], &p);
        int bc250 = p.vendorID == BC250_VENDOR_ID && p.deviceID == BC250_DEVICE_ID;
        printf("device %u      %s (vendor 0x%04X device 0x%04X) driver 0x%08X api %u.%u.%u%s\n", i, p.deviceName,
               p.vendorID, p.deviceID, p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), bc250 ? "  <- BC-250" : "");
        if (deviceIndex == UINT32_MAX && (bc250 || anyDevice)) deviceIndex = i;
    }
    if (deviceIndex == UINT32_MAX) {
        fprintf(stderr, "vrsprobe: no BC-250 among %u devices (--any-device for another GPU)\n", physCount);
        return 3;
    }
    pd = phys[deviceIndex];
    vkGetPhysicalDeviceMemoryProperties(pd, &memProps);

    /* ---- capabilities ---- */
    static VkExtensionProperties ext[512];
    uint32_t extCount = 512;
    VK(vkEnumerateDeviceExtensionProperties(pd, NULL, &extCount, ext));
    int hasFsr = HasExtension(ext, extCount, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsrF = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f13;
    if (hasFsr) f13.pNext = &fsrF;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrP = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    if (hasFsr) p2.pNext = &fsrP;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("caps fsr_extension=%d pipeline=%u primitive=%u attachment=%u dynamic_rendering=%u "
           "pipeline_statistics=%u fragment_atomics=%u\n",
           hasFsr, fsrF.pipelineFragmentShadingRate, fsrF.primitiveFragmentShadingRate,
           fsrF.attachmentFragmentShadingRate, f13.dynamicRendering, f2.features.pipelineStatisticsQuery,
           f2.features.fragmentStoresAndAtomics);
    if (hasFsr) {
        VkFormatProperties fp;
        printf("caps texel_min=%ux%u texel_max=%ux%u max_fragment_size=%ux%u non_trivial_combiners=%u "
               "primitive_multi_viewport=%u\n",
               fsrP.minFragmentShadingRateAttachmentTexelSize.width, fsrP.minFragmentShadingRateAttachmentTexelSize.height,
               fsrP.maxFragmentShadingRateAttachmentTexelSize.width, fsrP.maxFragmentShadingRateAttachmentTexelSize.height,
               fsrP.maxFragmentSize.width, fsrP.maxFragmentSize.height, fsrP.fragmentShadingRateNonTrivialCombinerOps,
               fsrP.primitiveFragmentShadingRateWithMultipleViewports);
        if (getRates) {
            VkPhysicalDeviceFragmentShadingRateKHR rates[16];
            uint32_t n = 16;
            for (uint32_t i = 0; i < n; i++) {
                rates[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
                rates[i].pNext = NULL;
            }
            if (getRates(pd, &n, rates) >= 0)
                for (uint32_t i = 0; i < n; i++)
                    printf("caps rate %ux%u samples 0x%x\n", rates[i].fragmentSize.width, rates[i].fragmentSize.height,
                           rates[i].sampleCounts);
        }
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8_UINT, &fp);
        printf("caps r8_uint_rate_attachment=%d\n",
               (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) != 0);
    }
    if (caps) { SetEvent(deadlineEvent); return 0; }

    if (!f13.dynamicRendering || !f2.features.fragmentStoresAndAtomics) {
        fprintf(stderr, "vrsprobe: the device lacks dynamic rendering or fragment stores and atomics\n");
        return 3;
    }
    if (arm->needsFsr) {
        if (!hasFsr) { fprintf(stderr, "vrsprobe: arm %s needs %s\n", arm->name, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME); return 3; }
        if ((arm->vsRate && !fsrF.primitiveFragmentShadingRate) || (arm->att && !fsrF.attachmentFragmentShadingRate) ||
            !fsrF.pipelineFragmentShadingRate) {
            fprintf(stderr, "vrsprobe: arm %s needs a shading rate feature the device does not offer\n", arm->name);
            return 3;
        }
    }
    uint32_t texel = 8;
    if (arm->att) {
        texel = fsrP.maxFragmentShadingRateAttachmentTexelSize.width;
        if (texel < 8 || texel > 32 || fsrP.maxFragmentShadingRateAttachmentTexelSize.height != texel || size % (2 * texel)) {
            fprintf(stderr, "vrsprobe: texel size %ux%u is not usable at size %u\n", texel,
                    fsrP.maxFragmentShadingRateAttachmentTexelSize.height, size);
            return 3;
        }
    }

    /* ---- device ---- */
    uint32_t familyCount = 16, family = UINT32_MAX;
    VkQueueFamilyProperties families[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, families);
    for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    if (family == UINT32_MAX) { fprintf(stderr, "vrsprobe: no graphics queue\n"); return 3; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsrOn = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features on13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    const char *devExt[1] = { VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME };
    on13.dynamicRendering = VK_TRUE;
    on.features.fragmentStoresAndAtomics = VK_TRUE;
    on.features.pipelineStatisticsQuery = f2.features.pipelineStatisticsQuery;
    on.pNext = &on13;
    if (hasFsr) {
        /* Every rate feature the device offers, also for the 1x1 arms: on GFX10.3 RADV, depth images get
         * their VRS HTILE layout from attachmentFragmentShadingRate alone, so every arm runs on the same
         * device configuration. */
        fsrOn.pipelineFragmentShadingRate = fsrF.pipelineFragmentShadingRate;
        fsrOn.primitiveFragmentShadingRate = fsrF.primitiveFragmentShadingRate;
        fsrOn.attachmentFragmentShadingRate = fsrF.attachmentFragmentShadingRate;
        on13.pNext = &fsrOn;
    }
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.pNext = &on;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = hasFsr ? 1 : 0;
    di.ppEnabledExtensionNames = devExt;
    Step("create-device");
    VK(vkCreateDevice(pd, &di, NULL, &dev));
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(dev, family, 0, &queue);

    /* ---- resources ---- */
    Step("resources");
    const VkFormat colorFormat = VK_FORMAT_R32G32B32A32_UINT, depthFormat = VK_FORMAT_D32_SFLOAT;
    VkImage color = MakeImage(colorFormat, size, size, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView colorView = MakeView(color, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImage depth = VK_NULL_HANDLE, rateImg = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE, rateView = VK_NULL_HANDLE;
    if (arm->att == ATT_DEPTH) {
        depth = MakeImage(depthFormat, size, size, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        depthView = MakeView(depth, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    const uint32_t rw = size / texel, rh = size / texel;
    Buf rateSrc = { 0 };
    if (arm->att) {
        rateImg = MakeImage(VK_FORMAT_R8_UINT, rw, rh,
                            VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        rateView = MakeView(rateImg, VK_FORMAT_R8_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
        rateSrc = MakeBuf((VkDeviceSize)rw * rh, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        /* Left half 1x1 (0), right half 2x2 ((log2 w << 2) | log2 h = 5). */
        for (uint32_t y = 0; y < rh; y++)
            for (uint32_t x = 0; x < rw; x++) ((uint8_t *)rateSrc.map)[y * rw + x] = x < rw / 2 ? 0 : 5;
    }
    Buf counter = MakeBuf(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkDeviceSize readBytes = (VkDeviceSize)size * size * 16;
    Buf readback = MakeBuf(readBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(readback.map, 0xEE, (size_t)readBytes);

    VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout dsl;
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    VK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPool dpool;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet set;
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo dbi = { counter.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    /* ---- pipeline ---- */
    Step("pipeline");
    const int readRate = hasFsr;
    VkShaderModule vs = arm->vsRate ? Module(kVsRate, sizeof(kVsRate)) : Module(kVsPlain, sizeof(kVsPlain));
    VkShaderModule fs = readRate ? Module(kFsRate, sizeof(kFsRate)) : Module(kFsPlain, sizeof(kFsPlain));
    int32_t specRate = arm->vsRate;
    VkSpecializationMapEntry sme = { 0, 0, sizeof(int32_t) };
    VkSpecializationInfo spec = { 1, &sme, sizeof(int32_t), &specRate };
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = arm->vsRate ? &spec : NULL;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { size, size } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsrState = { VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
    fsrState.fragmentSize.width = arm->fw;
    fsrState.fragmentSize.height = arm->fh;
    fsrState.combinerOps[0] = arm->op0;
    fsrState.combinerOps[1] = arm->op1;
    VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    pri.colorAttachmentCount = 1;
    pri.pColorAttachmentFormats = &colorFormat;
    pri.depthAttachmentFormat = arm->att == ATT_DEPTH ? depthFormat : VK_FORMAT_UNDEFINED;
    if (arm->fsrState) pri.pNext = &fsrState;
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.pNext = &pri;
    gpi.flags = arm->att ? VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR : 0;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = arm->att == ATT_DEPTH ? &ds : NULL;
    gpi.pColorBlendState = &cb;
    gpi.layout = layout;
    VkPipeline pipe;
    VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

    VkQueryPool qpool = VK_NULL_HANDLE;
    if (f2.features.pipelineStatisticsQuery) {
        VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qpi.queryCount = 1;
        qpi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        VK(vkCreateQueryPool(dev, &qpi, NULL, &qpool));
    }

    /* ---- the one command buffer ---- */
    Step("record");
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool;
    cpi.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBuffer cmd;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cmd, &cbi));
    if (qpool) vkCmdResetQueryPool(cmd, qpool, 0, 1);
    if (arm->att) {
        VkBufferImageCopy bic = { 0 };
        bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bic.imageSubresource.layerCount = 1;
        bic.imageExtent.width = rw;
        bic.imageExtent.height = rh;
        bic.imageExtent.depth = 1;
        ImageBarrier(cmd, rateImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, rateSrc.b, rateImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        ImageBarrier(cmd, rateImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
                     VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
    }
    ImageBarrier(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (depth)
        ImageBarrier(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = colorView;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.uint32[0] = ca.clearValue.color.uint32[1] = ca.clearValue.color.uint32[2] =
        ca.clearValue.color.uint32[3] = 0xFFFFFFFFu;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = depthView;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingFragmentShadingRateAttachmentInfoKHR rfa = { VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
    rfa.imageView = rateView;
    rfa.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    rfa.shadingRateAttachmentTexelSize.width = texel;
    rfa.shadingRateAttachmentTexelSize.height = texel;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.pNext = arm->att ? &rfa : NULL;
    ri.renderArea = sc;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = depth ? &da : NULL;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    if (qpool) vkCmdBeginQuery(cmd, qpool, 0, 0);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (qpool) vkCmdEndQuery(cmd, qpool, 0);
    vkCmdEndRendering(cmd);
    ImageBarrier(cmd, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy rc = { 0 };
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rc.imageSubresource.layerCount = 1;
    rc.imageExtent.width = size;
    rc.imageExtent.height = size;
    rc.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &rc);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cmd));

    /* ---- one submit, bounded wait ---- */
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    Step("submit");
    double t0 = NowMs();
    VK(vkQueueSubmit(queue, 1, &si, fence));
    VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, (uint64_t)fenceTimeoutMs * 1000000ull);
    if (wr == VK_TIMEOUT) {
        printf("FENCE_TIMEOUT after %u ms: the GPU did not complete the draw\n", fenceTimeoutMs);
        return 5;
    }
    if (wr != VK_SUCCESS) Fail("vkWaitForFences", wr);
    Step("done");
    printf("gpu wait %.2f ms\n", NowMs() - t0);

    uint64_t statFs = 0;
    int haveStat = 0;
    if (qpool && vkGetQueryPoolResults(dev, qpool, 0, 1, sizeof(statFs), &statFs, sizeof(statFs),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        haveStat = 1;
    const uint32_t total = *(volatile uint32_t *)counter.map;
    const uint32_t *px = (const uint32_t *)readback.map;
    printf("counter invocations=%u pixels=%u ratio=%.4f pipeline_stat_fs_invocations=%s%llu\n", total, size * size,
           (double)total / (double)(size * size), haveStat ? "" : "unavailable:", (unsigned long long)statFs);
    if (dumpPath) {
        FILE *f = fopen(dumpPath, "wb");
        if (f) { fwrite(px, 1, (size_t)readBytes, f); fclose(f); printf("dump %s %llu bytes\n", dumpPath, (unsigned long long)readBytes); }
    }
    uint8_t *seen = (uint8_t *)malloc(total ? total : 1);
    if (!seen) return 3;
    int complete = 1;
    if (arm->att) {
        Rect left = { 0, 0, size / 2, size }, right = { size / 2, 0, size, size };
        int a = Analyze(arm->name, "left", left, px, size, total, "1x1", seen);
        int b = Analyze(arm->name, "right", right, px, size, total, "2x2", seen);
        printf("VERDICT arm=%s expect=%s match=%d\n", arm->name, arm->expect, a && b);
    } else {
        Rect all = { 0, 0, size, size };
        int a = Analyze(arm->name, "all", all, px, size, total, arm->expect, seen);
        printf("VERDICT arm=%s expect=%s match=%d\n", arm->name, arm->expect, a);
    }
    for (uint32_t i = 0; i < size * size && complete; i++)
        if (px[4 * i + 3] != 0xA5A5A5A5u) complete = 0;
    {
        /* The first texels of the top-left 4x4 pixels, for a reader of the log. */
        for (uint32_t y = 0; y < 4; y++) {
            printf("texels y=%u", y);
            for (uint32_t x = 0; x < 4; x++) {
                const uint32_t *t = px + 4 * ((size_t)y * size + x);
                printf("  id=%u fc=(%u,%u)/16 rate=0x%X", t[0], t[1] & 0xFFFF, t[1] >> 16, t[2]);
            }
            printf("\n");
        }
    }
    VK(vkDeviceWaitIdle(dev));
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    SetEvent(deadlineEvent);
    printf("exit %d after %.1f ms\n", complete ? 0 : 1, NowMs() - startMs);
    return complete ? 0 : 1;
}
