/*
 * vkfillcheck: a content gate for vkCmdFillBuffer, vkCmdUpdateBuffer and vkCmdCopyBuffer.
 *
 * RADV implements all three of these commands with its own compute meta shaders. A meta shader built
 * from a wrong key writes the wrong bytes, or writes outside the requested range, and reports nothing:
 * the commands return no status, the queue reports no error, and a store through a wrong address is a
 * GPU fault only when that address happens to be unmapped. Nothing in the Vulkan contract catches it.
 * A byte comparison does, on every run, which is why this client exists.
 *
 * For each shape (operation x size x destination offset x source offset or tail x destination placement):
 *   1. a window of the destination is set to a position-dependent sentinel,
 *   2. one transfer command is recorded into that window,
 *   3. the command buffer is submitted and waited on,
 *   4. the window is read back on the host and every byte is compared with the expected image: the
 *      command's data inside the requested range, the untouched sentinel outside it.
 *
 * The window is dstOffset + size + 256 bytes. The dstOffset bytes before the range and the 256 bytes
 * after it are guard bytes, so a store that leaves the range fails the case even when every byte inside
 * the range is right (RADV's new buffer meta shader has no destination clamp: its bounds rest entirely
 * on the dispatch's thread count). Each case also checks that its expected image differs from its own
 * sentinel image, so a driver that silently drops the command cannot pass by doing nothing.
 *
 * The three commands take their data from three different places, which is the point of testing all of
 * them: vkCmdFillBuffer carries a dword in the command, vkCmdUpdateBuffer carries its bytes in the
 * command buffer, vkCmdCopyBuffer reads another buffer. A defect in one of them does not have to show in
 * the others. A fill also runs with VK_WHOLE_SIZE on a buffer that ends past a dword boundary, where the
 * driver has to round the range down and leave the last bytes alone.
 *
 * Both halves of our ICD need this: the x64 DLL serves 64-bit clients and the x86 DLL serves the WOW64
 * ones, and they are separate builds of the same source. build.ps1 -Arch both makes an x64 and an x86
 * program, and an x86 ICD can only be tested by the x86 one.
 *
 * Destination placement: "host" is a HOST_VISIBLE | HOST_COHERENT memory type, so both the sentinel and
 * the readback are plain CPU writes and reads and the case depends on nothing but the command under
 * test. That makes the host cases the oracle. "local" is a DEVICE_LOCAL type, where the sentinel and the
 * readback need vkCmdCopyBuffer themselves; the sentinel is therefore read back and checked before the
 * operation runs, and a failure there is reported as stage=prefill.
 *
 * The ICD comes from --icd (the DLL is loaded directly through vk_icdGetInstanceProcAddr, with no Vulkan
 * loader, no manifest and no registry entry), from --manifest (a radeon_icd.json handed to the loader),
 * or from the loader's own selection. Direct loading is what the lab needs: an elevated process, which
 * every SSH session on unit A is, silently ignores VK_DRIVER_FILES and VK_ICD_FILENAMES, and our ICD
 * ships as a private DLL next to the UMD rather than as a registered driver.
 *
 * Exit: 0 every case passed, 1 a case failed, 2 usage, 3 Vulkan or system error, 4 deadline.
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

#define BC250_VENDOR_ID 0x1002u
#define BC250_DEVICE_ID 0x13FEu

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateBuffer) \
    X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdFillBuffer) \
    X(vkCmdUpdateBuffer) X(vkCmdCopyBuffer) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkCreateFence) \
    X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;

/* The matrix.
 *
 * RADV derives the shader key from the destination address, not from the Vulkan offset, so the
 * program drives the key through the offsets it asks for and does not predict the key. The fields it
 * aims at, with the way each one is reached (ac_prepare_cs_clear_copy_buffer in
 * src/amd/common/nir/ac_nir_meta_cs_clear_copy_buffer.c):
 *
 *   dwords_per_thread          2 up to 64 KiB, 4 above it, so every size class is represented.
 *   dst_align_offset           dst_va % (dwords_per_thread * 4), so 0..7 for a size up to 64 KiB and
 *                              0..15 above it. vkCmdFillBuffer and vkCmdUpdateBuffer need a dword
 *                              aligned offset, so they reach the multiples of 4 only; vkCmdCopyBuffer
 *                              has no alignment rule at all and the "dst-align" group below sweeps
 *                              every one of the sixteen residues with it.
 *   dst_last_thread_bytes      (dst_align_offset + size) % (dwords_per_thread * 4); the same sweep
 *                              over a 64 KiB + 4 size walks all sixteen values.
 *   dst_single_thread_unaligned one thread, unaligned start and a partial end: an unaligned copy of
 *                              1 to 7 bytes, which is the "single-thread" group.
 *   has_start_thread           set unless the thread-aligned destination is 256-byte aligned, so the
 *                              offsets include 0 and 256 (not set) next to 4 and 252 (set).
 *   src_align_offset           src_offset % 4, so the copies use source offsets 0..3.
 *   is_clear, clear_value_size_is_4  every fill. VK_WHOLE_SIZE adds RADV's "& ~3" rounding of the
 *                              remaining range, which an explicit size cannot reach: the
 *                              "whole-size" group ends each buffer 1 to 3 bytes past a dword so that
 *                              those bytes must survive.
 *
 * src_scalarize_for_sparse and clear_value_size_is_12 are the two key bits left out: the first needs
 * a sparse buffer, and Vulkan has no 12-byte buffer clear value.
 */
#define GUARD 256u
#define MAX_SIZE 1048580u
#define MAX_OFF 256u
#define MAX_UPDATE 65536u
/* Every buffer is this big: the widest window plus room for an unaligned source and for the few extra
 * bytes a split copy reads past the end of its last region's source. */
#define CAP (MAX_OFF + MAX_SIZE + GUARD + 4096u)

enum { OP_FILL, OP_UPDATE, OP_COPY, OP_COPY_PARTS, OP_FILL_WHOLE, OP_COUNT };
static const char *const kOpNames[OP_COUNT] = { "fill", "update", "copy", "copy-parts", "fill-whole" };
enum { PLACE_HOST, PLACE_LOCAL, PLACE_COUNT };
static const char *const kPlaceNames[PLACE_COUNT] = { "host", "local" };

/* The third axis is the source offset of a copy, and the tail of a VK_WHOLE_SIZE fill: the number of
 * bytes by which the buffer ends past a dword boundary. */
static const char *VarName(int op)
{
    return op == OP_FILL_WHOLE ? "tail" : "src";
}

#define OPS(...) (0 | __VA_ARGS__)
#define OP(x) (1u << (x))

typedef struct {
    const char *name;
    uint32_t ops;                   /* which operations this group runs */
    const uint32_t *sizes; uint32_t sizeCount;
    const uint32_t *offs;  uint32_t offCount;
    const uint32_t *vars;  uint32_t varCount;  /* source offsets, or whole-size tails */
} Group;

/* 4 is a single dword, 12 and 252 are not a multiple of the shader's dwords-per-thread, 252 as an
 * offset starts the range inside a 256-byte block, 65536 is the largest vkCmdUpdateBuffer, 65540 is
 * the first size that takes 4 dwords per thread, and 1 MiB + 4 is a multi-workgroup dispatch with a
 * partial last thread. The offsets are all dword aligned, because fills and updates share them. */
static const uint32_t kSizes[] = { 4u, 12u, 64u, 252u, 4096u, 65536u, 65540u, 1048580u };
static const uint32_t kOffsets[] = { 0u, 4u, 252u, 256u };
static const uint32_t kSrcOffsets[] = { 0u, 1u, 2u, 3u };

/* One size in each dwords-per-thread class, every destination residue, two source alignments. */
static const uint32_t kAlignSizes[] = { 12u, 65540u };
static const uint32_t kAlignOffsets[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
                                          8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u };
static const uint32_t kAlignSrc[] = { 0u, 3u };

/* One thread, an unaligned start and a partial end: dst_single_thread_unaligned. */
static const uint32_t kTinySizes[] = { 1u, 2u, 3u, 5u, 7u };
static const uint32_t kTinyOffsets[] = { 1u, 2u, 3u, 5u, 6u, 7u };
static const uint32_t kTinySrc[] = { 0u, 1u };

/* VK_WHOLE_SIZE fills, on buffers that end 1 to 3 bytes past a dword. */
static const uint32_t kWholeSizes[] = { 12u, 4100u };
static const uint32_t kWholeOffsets[] = { 0u, 4u };
static const uint32_t kWholeTails[] = { 1u, 2u, 3u };

#define SPAN(a) a, (uint32_t)(sizeof(a) / sizeof((a)[0]))

static const Group kGroups[] = {
    { "core", OPS(OP(OP_FILL) | OP(OP_UPDATE) | OP(OP_COPY) | OP(OP_COPY_PARTS)),
      SPAN(kSizes), SPAN(kOffsets), SPAN(kSrcOffsets) },
    { "dst-align", OPS(OP(OP_COPY)), SPAN(kAlignSizes), SPAN(kAlignOffsets), SPAN(kAlignSrc) },
    { "single-thread", OPS(OP(OP_COPY)), SPAN(kTinySizes), SPAN(kTinyOffsets), SPAN(kTinySrc) },
    { "whole-size", OPS(OP(OP_FILL_WHOLE)), SPAN(kWholeSizes), SPAN(kWholeOffsets), SPAN(kWholeTails) },
};
#define GROUP_COUNT (uint32_t)(sizeof(kGroups) / sizeof(kGroups[0]))
/* --quick keeps the first two values of each axis of each group: the same shapes, a smoke-sized run. */
#define QUICK_AXIS 2u

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memProps;
static VkCommandPool pool;
static VkFence fence;
static uint32_t fenceTimeoutMs = 10000;
static uint32_t shift;  /* --negative-control: record the operation 4 bytes late, so every case must fail */
static LARGE_INTEGER qpcFreq;
static HANDLE deadlineEvent;
static uint32_t deadlineSec = 60;

static void Fail(const char *what, VkResult r)
{
    fprintf(stderr, "vkfillcheck: %s failed: VkResult %d\n", what, (int)r);
    exit(3);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static void Missing(const char *name)
{
    fprintf(stderr, "vkfillcheck: %s not available\n", name);
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

/* A hung GPU must not hang the gate: the watchdog ends the process once the deadline has passed, even if
 * a wait inside the driver never returns. Standard output is unbuffered, so nothing printed is lost. */
static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(deadlineEvent, (DWORD)(deadlineSec * 1000u + 2000u)) == WAIT_TIMEOUT) {
        fprintf(stderr, "vkfillcheck: deadline of %u s exceeded, ending the process\n", deadlineSec);
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

/* Every loaded module that exports vk_icdGetInstanceProcAddr, with the hash of its file. This is the
 * route witness: it says which driver the run actually exercised, whoever chose it. */
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

typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint32_t type; uint8_t *map; } Buf;

static int FindType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = memProps.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return (int)i;
    }
    return -1;
}

/* host: HOST_VISIBLE | HOST_COHERENT, so no flush or invalidate is needed and the case needs no
 * transfer of its own. local: DEVICE_LOCAL, preferring a type the CPU cannot see at all. */
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

static Buf MakeBuf(VkDeviceSize size, int place)
{
    Buf b = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0, NULL };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK(vkCreateBuffer(dev, &bi, NULL, &b.buffer));
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    t = PickType(req.memoryTypeBits, place);
    if (t < 0) {
        fprintf(stderr, "vkfillcheck: no %s memory type (bits 0x%X)\n", kPlaceNames[place], req.memoryTypeBits);
        exit(3);
    }
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    VK(vkAllocateMemory(dev, &ai, NULL, &b.memory));
    VK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
    b.type = (uint32_t)t;
    if (memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK(vkMapMemory(dev, b.memory, 0, VK_WHOLE_SIZE, 0, (void **)&b.map));
    return b;
}

static void FreeBuf(Buf *b)
{
    if (b->map) vkUnmapMemory(dev, b->memory);
    if (b->buffer) vkDestroyBuffer(dev, b->buffer, NULL);
    if (b->memory) vkFreeMemory(dev, b->memory, NULL);
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->map = NULL;
}

static void BarrierTransfer(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
}

static void BarrierToHost(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static VkCommandBuffer Begin(void)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkCommandBuffer cb;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(dev, &ai, &cb));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

/* A bounded wait: a fence that does not signal means the GPU is gone, and saying so beats hanging. */
static void SubmitWait(VkCommandBuffer cb)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkResult r;
    VK(vkEndCommandBuffer(cb));
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK(vkQueueSubmit(queue, 1, &si, fence));
    r = vkWaitForFences(dev, 1, &fence, VK_TRUE, (uint64_t)fenceTimeoutMs * 1000000ull);
    if (r == VK_TIMEOUT) {
        fprintf(stderr, "vkfillcheck: fence did not signal within %u ms; the GPU is not completing work\n",
                fenceTimeoutMs);
        exit(3);
    }
    if (r != VK_SUCCESS) Fail("vkWaitForFences", r);
    VK(vkResetFences(dev, 1, &fence));
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

/* ---- the patterns --------------------------------------------------------------------------------- */

/* All three are position dependent, so a store that lands at the wrong offset is a mismatch and not a
 * lucky match, and all three carry the case's salt, so data left by the previous case cannot pass. */
static uint8_t Sentinel(uint32_t i, uint32_t salt)
{
    return (uint8_t)(0xA5u ^ (i * 7u) ^ salt);
}

static uint8_t SrcByte(uint32_t i, uint32_t salt)
{
    return (uint8_t)(i * 131u + 17u + salt * 29u);
}

static uint8_t UpdateByte(uint32_t i, uint32_t salt)
{
    return (uint8_t)(i * 37u + 211u + salt * 13u);
}

static uint32_t FillValue(uint32_t salt)
{
    return 0xFEEDF000u | (salt & 0x0FFFu);
}

/* ---- one case ------------------------------------------------------------------------------------- */

typedef struct {
    int op;
    uint32_t size;
    uint32_t off;
    uint32_t var;   /* a copy's source offset, or a VK_WHOLE_SIZE fill's tail */
    int place;
} Case;

/* The host arrays are ordinary memory. Mapped Vulkan memory is only ever written and read with memcpy:
 * a byte-at-a-time loop over a write-combining or uncached mapping costs tens of nanoseconds per byte and
 * turned a 1 MiB case into tens of milliseconds, which is most of a run's time. */
typedef struct {
    uint8_t *want;  /* the expected window after the operation */
    uint8_t *pre;   /* the sentinel window, before the operation */
    uint8_t *got;   /* a copy of the window as the device left it */
    uint8_t *source;/* the source pattern, staged here before it goes to the source buffer */
    uint8_t *data;  /* vkCmdUpdateBuffer's bytes */
    Buf src;        /* host-visible source of the copies */
    Buf up;         /* host-visible staging that seeds a device-local destination */
    Buf rb;         /* host-visible staging that reads a device-local destination back: two windows, the
                     * sentinel at 0 and the result at CAP, so one submission carries both */
    Buf dst[PLACE_COUNT];
} Fixtures;

enum { CASE_PASS, CASE_FAIL, CASE_SKIP };

/* Compares the window and prints the case's line. "written" is how many bytes of the window the
 * operation may change, counted from the destination offset: the requested size, or for a
 * VK_WHOLE_SIZE fill the part of the rest of the buffer that RADV rounds down to a dword.
 * Returns CASE_PASS or CASE_FAIL. */
static int Compare(const Case *c, const uint8_t *got, const uint8_t *want, uint32_t window, uint32_t written,
                   const char *stage, int onlyFail)
{
    uint32_t first = UINT32_MAX, pre = 0, range = 0, post = 0;
    for (uint32_t i = 0; i < window; i++) {
        if (got[i] == want[i]) continue;
        if (first == UINT32_MAX) first = i;
        if (i < c->off) pre++;
        else if (i < c->off + written) range++;
        else post++;
    }
    if (first == UINT32_MAX) {
        if (!onlyFail)
            printf("PASS %-10s size %7u off %3u %s %u dst %-5s\n", kOpNames[c->op], c->size, c->off,
                   VarName(c->op), c->var, kPlaceNames[c->place]);
        return CASE_PASS;
    }
    printf("FAIL %-10s size %7u off %3u %s %u dst %-5s  stage %s, first diff at byte %u (%s, %+d from dstOffset): "
           "got 0x%02X want 0x%02X; bad bytes pre %u range %u post %u\n",
           kOpNames[c->op], c->size, c->off, VarName(c->op), c->var, kPlaceNames[c->place], stage, first,
           first < c->off ? "pre-guard" : (first < c->off + written ? "range" : "post-guard"),
           (int)first - (int)c->off, got[first], want[first], pre, range, post);
    return CASE_FAIL;
}

static int RunCase(const Case *c, Fixtures *f, uint32_t index, int onlyFail)
{
    /* A VK_WHOLE_SIZE fill needs a buffer of its own, because the range it fills is decided by the
     * size of the VkBuffer: the buffer ends c->var bytes past a dword, and those bytes must survive.
     * Every other case uses the shared destination and a window of dstOffset + size + 256. */
    const int whole_fill = c->op == OP_FILL_WHOLE;
    const uint32_t window = whole_fill ? c->off + c->size + GUARD + c->var : c->off + c->size + GUARD;
    const uint32_t written = whole_fill ? ((window - c->off) & ~3u) : c->size;
    const uint32_t salt = index * 2654435761u + 1u;
    Buf own = { VK_NULL_HANDLE, VK_NULL_HANDLE, 0, NULL };
    const Buf *dst = &f->dst[c->place];
    VkBufferCopy regions[3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    uint32_t regionCount = 0, srcEnd = 0;
    VkBufferCopy seed;
    VkCommandBuffer cb;
    int result;

    if (c->op == OP_UPDATE && c->size > MAX_UPDATE) {
        if (!onlyFail)
            printf("SKIP %-10s size %7u off %3u %s %u dst %-5s  vkCmdUpdateBuffer takes at most %u bytes\n",
                   kOpNames[c->op], c->size, c->off, VarName(c->op), c->var, kPlaceNames[c->place], MAX_UPDATE);
        return CASE_SKIP;
    }

    seed.srcOffset = 0;
    seed.dstOffset = 0;
    seed.size = window;

    /* The sentinel image, and the expected image that the operation turns it into. */
    for (uint32_t i = 0; i < window; i++) f->pre[i] = Sentinel(i, salt);
    memcpy(f->want, f->pre, window);

    switch (c->op) {
    case OP_FILL:
    case OP_FILL_WHOLE: {
        const uint32_t value = FillValue(salt);
        for (uint32_t k = 0; k < written; k++) f->want[c->off + k] = (uint8_t)(value >> (8u * (k & 3u)));
        break;
    }
    case OP_UPDATE:
        for (uint32_t k = 0; k < c->size; k++) {
            f->data[k] = UpdateByte(k, salt);
            f->want[c->off + k] = f->data[k];
        }
        break;
    case OP_COPY:
        regions[0].srcOffset = c->var;
        regions[0].dstOffset = c->off;
        regions[0].size = c->size;
        regionCount = 1;
        break;
    case OP_COPY_PARTS: {
        /* Three regions that together cover the range and overlap nowhere in the destination, each
         * reading from a different place in the source, submitted out of order. */
        const uint32_t a = c->size / 3u, b = a, rest = c->size - a - b;
        regions[0].srcOffset = c->var + 2u * a + 2u;
        regions[0].dstOffset = c->off + a + b;
        regions[0].size = rest;
        regions[1].srcOffset = c->var;
        regions[1].dstOffset = c->off;
        regions[1].size = a;
        regions[2].srcOffset = c->var + a + 1u;
        regions[2].dstOffset = c->off + a;
        regions[2].size = b;
        regionCount = 3;
        break;
    }
    default:
        break;
    }

    if (whole_fill) {
        own = MakeBuf(window, c->place);
        dst = &own;
        if (c->place == PLACE_HOST && !own.map) {
            fprintf(stderr, "vkfillcheck: the whole-size destination did not map\n");
            exit(3);
        }
    }

    if (regionCount) {
        for (uint32_t r = 0; r < regionCount; r++) {
            const uint32_t s = (uint32_t)regions[r].srcOffset, d = (uint32_t)regions[r].dstOffset,
                           n = (uint32_t)regions[r].size;
            if (s + n > srcEnd) srcEnd = s + n;
            for (uint32_t k = 0; k < n; k++) f->want[d + k] = SrcByte(s + k, salt);
        }
        for (uint32_t i = 0; i < srcEnd; i++) f->source[i] = SrcByte(i, salt);
        memcpy(f->src.map, f->source, srcEnd);
    }

    /* A case that expects what it already has would pass without the driver doing anything. */
    if (!memcmp(f->pre, f->want, window)) {
        fprintf(stderr, "vkfillcheck: case %u does not change any byte; the matrix is wrong\n", index);
        exit(3);
    }

    /* One submission per case. A device-local destination gets its sentinel, a readback of that sentinel,
     * the operation and a readback of the result in that order, separated by barriers, so the host sees
     * both windows at the fence and can still tell a broken seed from a broken operation. */
    cb = Begin();
    if (c->place == PLACE_HOST) {
        memcpy(dst->map, f->pre, window);
    } else {
        VkBufferCopy readback;
        memcpy(f->up.map, f->pre, window);
        readback.srcOffset = 0;
        readback.dstOffset = 0;
        readback.size = window;
        vkCmdCopyBuffer(cb, f->up.buffer, dst->buffer, 1, &seed);
        BarrierTransfer(cb);
        vkCmdCopyBuffer(cb, dst->buffer, f->rb.buffer, 1, &readback);
        BarrierTransfer(cb);
    }

    for (uint32_t r = 0; r < regionCount; r++) regions[r].dstOffset += shift;
    switch (c->op) {
    case OP_FILL:
        vkCmdFillBuffer(cb, dst->buffer, c->off + shift, c->size, FillValue(salt));
        break;
    case OP_FILL_WHOLE:
        /* The size comes from the buffer, not from here: RADV rounds the rest of it down to a dword
         * (vk_buffer_range(...) & ~3ull in radv_CmdFillBuffer), and the bytes past that must keep
         * their sentinel. */
        vkCmdFillBuffer(cb, dst->buffer, c->off + shift, VK_WHOLE_SIZE, FillValue(salt));
        break;
    case OP_UPDATE:
        vkCmdUpdateBuffer(cb, dst->buffer, c->off + shift, c->size, f->data);
        break;
    default:
        vkCmdCopyBuffer(cb, f->src.buffer, dst->buffer, regionCount, regions);
        break;
    }
    if (c->place == PLACE_LOCAL) {
        VkBufferCopy readback;
        readback.srcOffset = 0;
        readback.dstOffset = CAP;
        readback.size = window;
        BarrierTransfer(cb);
        vkCmdCopyBuffer(cb, dst->buffer, f->rb.buffer, 1, &readback);
    }
    BarrierToHost(cb);
    SubmitWait(cb);

    if (c->place == PLACE_LOCAL) {
        memcpy(f->got, f->rb.map, window);
        if (Compare(c, f->got, f->pre, window, window, "prefill", onlyFail) == CASE_FAIL) {
            if (whole_fill) FreeBuf(&own);
            return CASE_FAIL;
        }
        memcpy(f->got, f->rb.map + CAP, window);
    } else {
        memcpy(f->got, dst->map, window);
    }
    result = Compare(c, f->got, f->want, window, written, "op", onlyFail);
    if (whole_fill) FreeBuf(&own);
    return result;
}

/* ---- driver -------------------------------------------------------------------------------------- */

static void Usage(FILE *out, int code)
{
    fprintf(out,
            "usage: vkfillcheck [--icd DLL | --manifest JSON] [--device N] [--any-device] [--list-devices]\n"
            "                   [--placement host|local|both] [--queue-family N] [--quick] [--only-fail]\n"
            "                   [--negative-control] [--deadline SEC] [--fence-timeout-ms N] [--help]\n"
            "\n"
            "  --icd DLL           load this ICD DLL directly (absolute path), without the Vulkan loader.\n"
            "                      This is the lab path: an elevated process ignores VK_DRIVER_FILES, and\n"
            "                      amdgpu_wddm_radv.dll is a private DLL with no manifest and no registry entry.\n"
            "  --manifest JSON     hand this ICD manifest to the loader (VK_DRIVER_FILES, VK_ICD_FILENAMES).\n"
            "                      Ignored by the loader in an elevated process.\n"
            "  --device N          use the physical device at index N instead of searching for the BC-250.\n"
            "  --any-device        accept a device that is not a BC-250 (needed on a development PC).\n"
            "  --list-devices      print the physical devices and exit.\n"
            "  --placement         which destination memory to test (default both).\n"
            "  --queue-family N    use this queue family instead of the first graphics or compute one.\n"
            "  --quick             a trimmed matrix for a smoke run.\n"
            "  --only-fail         print only the cases that fail.\n"
            "  --negative-control  record every operation 4 bytes late. Every case must then fail, half of\n"
            "                      them in the guard after the range, and the exit code must be 1.\n"
            "  --deadline SEC      end the run after this many seconds (default 60, at most 170).\n"
            "  --fence-timeout-ms  how long to wait for one submission (default 10000).\n"
            "\n"
            "exit: 0 every case passed, 1 a case failed, 2 usage, 3 Vulkan or system error, 4 deadline.\n");
    exit(code);
}

int main(int argc, char **argv)
{
    const char *icdPath = NULL, *manifestPath = NULL;
    uint32_t deviceIndex = UINT32_MAX, forcedFamily = UINT32_MAX;
    int anyDevice = 0, listDevices = 0, onlyFail = 0, quick = 0;
    int placeFirst = PLACE_HOST, placeLast = PLACE_LOCAL;
    HMODULE library;
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
    VkCommandPoolCreateInfo cpc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    Fixtures f;
    uint32_t index = 0, passed = 0, failed = 0, skipped = 0;
    uint32_t opPass[OP_COUNT] = { 0 }, opTotal[OP_COUNT] = { 0 };
    int lateDeadline = 0;
    double start;
    HANDLE watchdog;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) Usage(stdout, 0);
        if (!strcmp(a, "--any-device")) { anyDevice = 1; continue; }
        if (!strcmp(a, "--list-devices")) { listDevices = 1; continue; }
        if (!strcmp(a, "--only-fail")) { onlyFail = 1; continue; }
        if (!strcmp(a, "--quick")) { quick = 1; continue; }
        if (!strcmp(a, "--negative-control")) { shift = 4; continue; }
        if (i + 1 >= argc) Usage(stderr, 2);
        if (!strcmp(a, "--icd")) icdPath = argv[++i];
        else if (!strcmp(a, "--manifest")) manifestPath = argv[++i];
        else if (!strcmp(a, "--device")) deviceIndex = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--queue-family")) forcedFamily = (uint32_t)strtoul(argv[++i], NULL, 10);
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
        fprintf(stderr, "vkfillcheck: --icd and --manifest are alternatives\n");
        return 2;
    }
    if (deadlineSec < 5 || deadlineSec > 170 || fenceTimeoutMs < 100 || fenceTimeoutMs > 120000) Usage(stderr, 2);
    QueryPerformanceFrequency(&qpcFreq);
    start = NowMs();
    if (!(deadlineEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) {
        fprintf(stderr, "vkfillcheck: CreateEvent failed (%lu)\n", GetLastError());
        return 3;
    }
    if (!(watchdog = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL))) {
        fprintf(stderr, "vkfillcheck: CreateThread failed (%lu)\n", GetLastError());
        return 3;
    }

    if (icdPath) {
        /* Direct ICD use: the module's own entry point, no loader, no manifest, no registry. The search
         * flags keep the current directory and PATH out of the dependency search, as the UMD does. */
        wchar_t wide[MAX_PATH * 2];
        PFN_vkNegotiateLoaderICDInterfaceVersion negotiate;
        uint32_t icdVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        if (MultiByteToWideChar(CP_ACP, 0, icdPath, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) == 0) {
            fprintf(stderr, "vkfillcheck: --icd path is not usable\n");
            return 2;
        }
        if (!(wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/'))) {
            fprintf(stderr, "vkfillcheck: --icd needs an absolute path, got %s\n", icdPath);
            return 2;
        }
        library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            fprintf(stderr, "vkfillcheck: cannot load %s (error %lu)\n", icdPath, GetLastError());
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
            fprintf(stderr, "vkfillcheck: vulkan-1.dll not found\n");
            return 3;
        }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    }
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");

    app.pApplicationName = "vkfillcheck";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)
    PrintIcdModules();

    VK(vkEnumeratePhysicalDevices(instance, &physCount, phys));
    if (!physCount) {
        fprintf(stderr, "vkfillcheck: no Vulkan physical device\n");
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
            fprintf(stderr, "vkfillcheck: no BC-250 (vendor 0x%04X device 0x%04X) among %u devices; "
                            "--any-device or --device N to test another one\n",
                    BC250_VENDOR_ID, BC250_DEVICE_ID, physCount);
            return 3;
        }
        deviceIndex = 0;
    }
    if (deviceIndex >= physCount) {
        fprintf(stderr, "vkfillcheck: %u devices, index %u\n", physCount, deviceIndex);
        return 2;
    }
    vkGetPhysicalDeviceProperties(phys[deviceIndex], &props);
    if (!anyDevice && !(props.vendorID == BC250_VENDOR_ID && props.deviceID == BC250_DEVICE_ID)) {
        fprintf(stderr, "vkfillcheck: device %u is not a BC-250; --any-device to accept it\n", deviceIndex);
        return 3;
    }
    vkGetPhysicalDeviceMemoryProperties(phys[deviceIndex], &memProps);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[deviceIndex], &familyCount, families);
    if (forcedFamily != UINT32_MAX) {
        if (forcedFamily >= familyCount ||
            !(families[forcedFamily].queueFlags &
              (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) {
            fprintf(stderr, "vkfillcheck: queue family %u cannot transfer\n", forcedFamily);
            return 2;
        }
        family = forcedFamily;
    } else {
        /* The graphics or compute family, because that is where the applications we care about issue
         * their transfers and where RADV runs its meta shaders. */
        for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
        for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;
        for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; i++)
            if (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) family = i;
    }
    if (family == UINT32_MAX) {
        fprintf(stderr, "vkfillcheck: no queue family can transfer\n");
        return 3;
    }
    printf("using         device %u, queue family %u (flags 0x%X), %u memory types\n", deviceIndex, family,
           families[family].queueFlags, memProps.memoryTypeCount);

    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(phys[deviceIndex], &dci, NULL, &dev));
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(dev, family, 0, &queue);
    cpc.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpc.queueFamilyIndex = family;
    VK(vkCreateCommandPool(dev, &cpc, NULL, &pool));
    VK(vkCreateFence(dev, &fci, NULL, &fence));

    memset(&f, 0, sizeof(f));
    f.want = (uint8_t *)malloc(CAP);
    f.pre = (uint8_t *)malloc(CAP);
    f.got = (uint8_t *)malloc(CAP);
    f.source = (uint8_t *)malloc(CAP);
    f.data = (uint8_t *)malloc(MAX_UPDATE);
    if (!f.want || !f.pre || !f.got || !f.source || !f.data) {
        fprintf(stderr, "vkfillcheck: out of host memory\n");
        return 3;
    }
    f.src = MakeBuf(CAP, PLACE_HOST);
    f.up = MakeBuf(CAP, PLACE_HOST);
    f.rb = MakeBuf(2 * (VkDeviceSize)CAP, PLACE_HOST);
    for (int place = placeFirst; place <= placeLast; place++) f.dst[place] = MakeBuf(CAP, place);
    if (!f.src.map || !f.up.map || !f.rb.map ||
        (placeFirst == PLACE_HOST && !f.dst[PLACE_HOST].map)) {
        fprintf(stderr, "vkfillcheck: a host-visible buffer did not map\n");
        return 3;
    }
    for (int place = placeFirst; place <= placeLast; place++)
        printf("destination   %-5s memory type %u (heap %u, flags 0x%03X)\n", kPlaceNames[place], f.dst[place].type,
               memProps.memoryTypes[f.dst[place].type].heapIndex,
               memProps.memoryTypes[f.dst[place].type].propertyFlags);
    printf("staging       source/seed/readback memory type %u (flags 0x%03X)\n", f.src.type,
           memProps.memoryTypes[f.src.type].propertyFlags);
    for (uint32_t g = 0; g < GROUP_COUNT; g++) {
        const Group *grp = &kGroups[g];
        const uint32_t sc = quick && grp->sizeCount > QUICK_AXIS ? QUICK_AXIS : grp->sizeCount;
        const uint32_t oc = quick && grp->offCount > QUICK_AXIS ? QUICK_AXIS : grp->offCount;
        const uint32_t vc = quick && grp->varCount > QUICK_AXIS ? QUICK_AXIS : grp->varCount;
        printf("group         %-14s %u size(s) x %u destination offset(s) x %u %s value(s)\n", grp->name, sc, oc,
               vc, grp->ops & OP(OP_FILL_WHOLE) ? "tail" : "source offset");
    }
    printf("matrix        guard %u bytes, window up to %u, %u destination placement(s)%s\n", GUARD,
           MAX_OFF + MAX_SIZE + GUARD, (uint32_t)(placeLast - placeFirst + 1), quick ? ", --quick" : "");
    if (shift) printf("negative control: every operation is recorded %u bytes late, every case must fail\n", shift);

    for (int place = placeFirst; place <= placeLast; place++) {
        for (uint32_t g = 0; g < GROUP_COUNT; g++) {
            const Group *grp = &kGroups[g];
            const uint32_t sizeCount = quick && grp->sizeCount > QUICK_AXIS ? QUICK_AXIS : grp->sizeCount;
            const uint32_t offsetCount = quick && grp->offCount > QUICK_AXIS ? QUICK_AXIS : grp->offCount;
            const uint32_t varCount = quick && grp->varCount > QUICK_AXIS ? QUICK_AXIS : grp->varCount;
            for (int op = 0; op < OP_COUNT; op++) {
                if (!(grp->ops & OP(op))) continue;
                for (uint32_t s = 0; s < sizeCount; s++) {
                    for (uint32_t o = 0; o < offsetCount; o++) {
                        /* Only a copy has a source offset to vary, and only a whole-size fill a tail.
                         * The other operations run the group's shapes once. */
                        const uint32_t variants =
                            (op == OP_COPY || op == OP_COPY_PARTS || op == OP_FILL_WHOLE) ? varCount : 1u;
                        for (uint32_t x = 0; x < variants; x++) {
                            Case c;
                            int result;
                            /* A three-way split needs at least a dword per region. */
                            if (op == OP_COPY_PARTS && grp->sizes[s] < 12u) continue;
                            if (NowMs() - start > deadlineSec * 1000.0) {
                                printf("deadline      %u s reached after %u cases, the matrix is incomplete\n",
                                       deadlineSec, index);
                                lateDeadline = 1;
                                goto done;
                            }
                            c.op = op;
                            c.size = grp->sizes[s];
                            c.off = grp->offs[o];
                            c.var = grp->vars[x];
                            c.place = place;
                            result = RunCase(&c, &f, index++, onlyFail);
                            opTotal[op]++;
                            if (result == CASE_PASS) { passed++; opPass[op]++; }
                            else if (result == CASE_FAIL) failed++;
                            else { skipped++; opTotal[op]--; }
                        }
                    }
                }
            }
        }
    }
done:
    VK(vkDeviceWaitIdle(dev));
    printf("cases %u: %u PASS, %u FAIL, %u SKIP", index, passed, failed, skipped);
    for (int op = 0; op < OP_COUNT; op++) printf("%s%s %u/%u", op ? ", " : "  (", kOpNames[op], opPass[op], opTotal[op]);
    printf(")\nelapsed       %.1f s\n", (NowMs() - start) / 1000.0);

    for (int place = placeFirst; place <= placeLast; place++) FreeBuf(&f.dst[place]);
    FreeBuf(&f.rb);
    FreeBuf(&f.up);
    FreeBuf(&f.src);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    free(f.want);
    free(f.pre);
    free(f.got);
    free(f.source);
    free(f.data);
    SetEvent(deadlineEvent);
    CloseHandle(watchdog);
    if (shift) {
        /* Under the negative control a passing case means the comparison itself is broken, which is a
         * worse result than a failing driver and must not look like one. */
        if (passed) {
            printf("negative control BROKEN: %u cases passed although every operation was displaced\n", passed);
            printf("result BROKEN\n");
            return 3;
        }
        printf("negative control: every one of %u cases failed, as it must\n", failed);
    }
    printf("result %s\n", failed ? "FAILED" : (lateDeadline ? "INCOMPLETE" : "ok"));
    if (failed) return 1;
    return lateDeadline ? 4 : 0;
}
