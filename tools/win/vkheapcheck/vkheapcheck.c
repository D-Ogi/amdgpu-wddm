/*
 * vkheapcheck: print the Vulkan memory heaps of one device and their VK_EXT_memory_budget values, and check
 * the heap layout that RADV reports on an APU.
 *
 * RADV has two heap layouts for a device without dedicated VRAM (radv_physical_device_init_mem_types in
 * src/amd/vulkan/radv_physical_device.c). The total T is the GTT size plus the CPU-visible VRAM size:
 *   split    (radv_enable_unified_heap_on_apu false): heap DEVICE_LOCAL = align(2T/3, GART page), and a
 *            second heap without flags that holds the rest.
 *   unified  (radv_enable_unified_heap_on_apu true): one DEVICE_LOCAL heap of size T.
 * On the BC-250 the carve-out is the VRAM and the GTT is the Windows shared system memory segment, so the
 * split layout hides a third of the memory from an application that sizes its working set from the
 * DEVICE_LOCAL heap (BD-028).
 *
 * The program prints each heap (size, flags, budget, usage), allocates --alloc-mib MiB from the first
 * DEVICE_LOCAL memory type, and reads the budget again: the usage of that heap must increase by at least
 * the allocation. That is the positive control of the budget query. With --expect split|unified it also
 * checks the layout above, so that one run can fail by itself; a comparison between two ICDs (the same T
 * on both) is the caller's job, from the "total" line.
 *
 * The ICD comes from --icd (the DLL is loaded directly through vk_icdGetInstanceProcAddr, with no Vulkan
 * loader, no manifest and no registry entry) or from the loader. Direct loading is what the lab needs: an
 * elevated process ignores VK_DRIVER_FILES and VK_ICD_FILENAMES (same route as tools\win\vkfillcheck).
 *
 * Exit: 0 pass, 1 a check failed, 2 usage, 3 Vulkan or system error, 4 deadline.
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
#define MIB (1024.0 * 1024.0)

#define INSTANCE_FUNCS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) X(vkDestroyDevice) X(vkAllocateMemory) X(vkFreeMemory)

#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;
static VkDevice dev;
static HANDLE deadlineEvent;
static uint32_t deadlineSec = 30;

static void Fail(const char *what, VkResult r)
{
    fprintf(stderr, "vkheapcheck: %s failed: VkResult %d\n", what, (int)r);
    exit(3);
}
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) Fail(#call, r_); } while (0)

static void Missing(const char *name)
{
    fprintf(stderr, "vkheapcheck: %s not available\n", name);
    exit(3);
}
#define LOAD_INSTANCE(f) if (!(f = (PFN_##f)getInstanceProc(instance, #f))) Missing(#f);
#define LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) Missing(#f);

/* A hung driver must not hang the gate: the watchdog ends the process after the deadline. */
static DWORD WINAPI Watchdog(LPVOID unused)
{
    (void)unused;
    if (WaitForSingleObject(deadlineEvent, deadlineSec * 1000u + 2000u) == WAIT_TIMEOUT) {
        fprintf(stderr, "vkheapcheck: deadline of %u s exceeded, ending the process\n", deadlineSec);
        fflush(stderr);
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

typedef struct {
    VkPhysicalDeviceMemoryProperties2 props;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
} Query;

static void Read(VkPhysicalDevice phys, Query *q)
{
    memset(q, 0, sizeof(*q));
    q->props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    q->budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    q->props.pNext = &q->budget;
    vkGetPhysicalDeviceMemoryProperties2(phys, &q->props);
}

static void Usage(FILE *out, int code)
{
    fprintf(out,
            "usage: vkheapcheck [--icd DLL] [--device N] [--any-device] [--expect split|unified]\n"
            "                   [--alloc-mib N] [--deadline SEC] [--help]\n"
            "\n"
            "  --icd DLL           load this ICD DLL directly (absolute path), without the Vulkan loader.\n"
            "  --device N          use the physical device at index N instead of searching for the BC-250.\n"
            "  --any-device        accept a device that is not a BC-250 (needed on a development PC).\n"
            "  --expect split      two heaps, one DEVICE_LOCAL of align(2T/3) (RADV APU default before BD-028).\n"
            "  --expect unified    one DEVICE_LOCAL heap (radv_enable_unified_heap_on_apu).\n"
            "  --alloc-mib N       budget control: allocate N MiB of DEVICE_LOCAL memory (default 256, 0 = off).\n"
            "  --deadline SEC      end the run after this many seconds (default 30, 5..170).\n"
            "\n"
            "exit: 0 pass, 1 a check failed, 2 usage, 3 Vulkan or system error, 4 deadline.\n");
    exit(code);
}

int main(int argc, char **argv)
{
    const char *icdPath = NULL, *expect = NULL;
    uint32_t deviceIndex = UINT32_MAX, allocMib = 256;
    int anyDevice = 0, failures = 0;
    HMODULE library;
    VkInstance instance;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice phys[16];
    uint32_t physCount = 16;
    VkPhysicalDeviceProperties props;
    VkExtensionProperties exts[512];
    uint32_t extCount = 512;
    int hasBudget = 0;
    Query before, after;
    const VkPhysicalDeviceMemoryProperties *mp;
    uint64_t total = 0, dlTotal = 0;
    uint32_t dlHeaps = 0, dlIndex = UINT32_MAX;
    HANDLE watchdog;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) Usage(stdout, 0);
        if (!strcmp(a, "--any-device")) { anyDevice = 1; continue; }
        if (i + 1 >= argc) Usage(stderr, 2);
        if (!strcmp(a, "--icd")) icdPath = argv[++i];
        else if (!strcmp(a, "--device")) deviceIndex = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--alloc-mib")) allocMib = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--deadline")) deadlineSec = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--expect")) {
            expect = argv[++i];
            if (strcmp(expect, "split") && strcmp(expect, "unified")) Usage(stderr, 2);
        } else Usage(stderr, 2);
    }
    if (deadlineSec < 5 || deadlineSec > 170 || allocMib > 4096) Usage(stderr, 2);
    if (!(deadlineEvent = CreateEventW(NULL, TRUE, FALSE, NULL)) ||
        !(watchdog = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL))) {
        fprintf(stderr, "vkheapcheck: watchdog setup failed (%lu)\n", GetLastError());
        return 3;
    }
    {
        char env[16];
        DWORD n = GetEnvironmentVariableA("radv_enable_unified_heap_on_apu", env, sizeof(env));
        printf("environment   radv_enable_unified_heap_on_apu=%s\n", n && n < sizeof(env) ? env : "(not set)");
    }

    if (icdPath) {
        wchar_t wide[MAX_PATH * 2];
        PFN_vkNegotiateLoaderICDInterfaceVersion negotiate;
        uint32_t icdVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        if (MultiByteToWideChar(CP_ACP, 0, icdPath, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))) == 0 ||
            !(wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/'))) {
            fprintf(stderr, "vkheapcheck: --icd needs an absolute path, got %s\n", icdPath);
            return 2;
        }
        library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            fprintf(stderr, "vkheapcheck: cannot load %s (error %lu)\n", icdPath, GetLastError());
            return 3;
        }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vk_icdGetInstanceProcAddr");
        if (!getInstanceProc)
            getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vk_icdGetInstanceProcAddr");
        negotiate = (PFN_vkNegotiateLoaderICDInterfaceVersion)GetProcAddress(
            library, "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (negotiate && negotiate(&icdVersion) == VK_SUCCESS) printf("icd interface %u\n", icdVersion);
        else printf("icd interface unavailable (the module answers no negotiation)\n");
    } else {
        library = LoadLibraryA("vulkan-1.dll");
        if (!library) {
            fprintf(stderr, "vkheapcheck: vulkan-1.dll not found\n");
            return 3;
        }
        getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(library, "vkGetInstanceProcAddr");
        if (!getInstanceProc) Missing("vkGetInstanceProcAddr");
    }
    if (!(createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance"))) Missing("vkCreateInstance");

    app.pApplicationName = "vkheapcheck";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    VK(createInstance(&ici, NULL, &instance));
    INSTANCE_FUNCS(LOAD_INSTANCE)
    PrintIcdModules();

    VK(vkEnumeratePhysicalDevices(instance, &physCount, phys));
    if (!physCount) {
        fprintf(stderr, "vkheapcheck: no Vulkan physical device\n");
        return 3;
    }
    for (uint32_t i = 0; i < physCount; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys[i], &p);
        printf("device %u      %s (vendor 0x%04X device 0x%04X type %d) driver 0x%08X%s\n", i, p.deviceName,
               p.vendorID, p.deviceID, (int)p.deviceType, p.driverVersion,
               (p.vendorID == BC250_VENDOR_ID && p.deviceID == BC250_DEVICE_ID) ? "  <- BC-250" : "");
        if (deviceIndex == UINT32_MAX && p.vendorID == BC250_VENDOR_ID && p.deviceID == BC250_DEVICE_ID)
            deviceIndex = i;
    }
    if (deviceIndex == UINT32_MAX) {
        if (!anyDevice) {
            fprintf(stderr, "vkheapcheck: no BC-250 among %u devices; --any-device or --device N\n", physCount);
            return 3;
        }
        deviceIndex = 0;
    }
    if (deviceIndex >= physCount) {
        fprintf(stderr, "vkheapcheck: %u devices, index %u\n", physCount, deviceIndex);
        return 2;
    }
    vkGetPhysicalDeviceProperties(phys[deviceIndex], &props);
    if (!anyDevice && !(props.vendorID == BC250_VENDOR_ID && props.deviceID == BC250_DEVICE_ID)) {
        fprintf(stderr, "vkheapcheck: device %u is not a BC-250; --any-device to accept it\n", deviceIndex);
        return 3;
    }
    VK(vkEnumerateDeviceExtensionProperties(phys[deviceIndex], NULL, &extCount, exts));
    for (uint32_t i = 0; i < extCount; i++)
        if (!strcmp(exts[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) hasBudget = 1;
    printf("using         device %u, %s %s\n", deviceIndex, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
           hasBudget ? "offered" : "NOT offered");
    if (!hasBudget) {
        printf("FAIL          the device does not offer %s\n", VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        failures++;
    }

    Read(phys[deviceIndex], &before);
    mp = &before.props.memoryProperties;
    printf("memory        %u types, %u heaps\n", mp->memoryTypeCount, mp->memoryHeapCount);
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        printf("  type %2u heap %u flags 0x%03X\n", i, mp->memoryTypes[i].heapIndex, mp->memoryTypes[i].propertyFlags);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        const VkMemoryHeap *h = &mp->memoryHeaps[i];
        printf("heap %u size %llu (%.1f MiB) flags 0x%X budget %llu (%.1f MiB) usage %llu (%.1f MiB)\n", i,
               (unsigned long long)h->size, h->size / MIB, h->flags,
               (unsigned long long)before.budget.heapBudget[i], before.budget.heapBudget[i] / MIB,
               (unsigned long long)before.budget.heapUsage[i], before.budget.heapUsage[i] / MIB);
        total += h->size;
        if (h->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            dlTotal += h->size;
            dlHeaps++;
            if (dlIndex == UINT32_MAX) dlIndex = i;
        }
        /* The extension requires a non-zero budget for every heap. A budget above the heap size is legal (the
         * driver estimates it; NVIDIA reports one for its system heap), so it is only noted. */
        if (hasBudget && before.budget.heapBudget[i] == 0) {
            printf("FAIL          heap %u budget is 0\n", i);
            failures++;
        } else if (hasBudget && before.budget.heapBudget[i] > h->size) {
            printf("note          heap %u budget is above the heap size\n", i);
        }
    }
    printf("total heaps=%u bytes=%llu (%.1f MiB) device_local_heaps=%u device_local_bytes=%llu (%.1f MiB) "
           "share=%.4f\n", mp->memoryHeapCount, (unsigned long long)total, total / MIB, dlHeaps,
           (unsigned long long)dlTotal, dlTotal / MIB, total ? (double)dlTotal / (double)total : 0.0);

    if (expect && !strcmp(expect, "unified")) {
        if (!(mp->memoryHeapCount == 1 && dlHeaps == 1)) {
            printf("FAIL          expect unified: want 1 heap, DEVICE_LOCAL; got %u heap(s), %u DEVICE_LOCAL\n",
                   mp->memoryHeapCount, dlHeaps);
            failures++;
        } else printf("expect        unified: PASS\n");
    } else if (expect) {
        /* align64(2T/3, gart page): the GART page is 4 KiB here; 64 KiB of slack covers a larger one. */
        const uint64_t want = total / 3 * 2;
        const uint64_t diff = dlTotal > want ? dlTotal - want : want - dlTotal;
        if (!(mp->memoryHeapCount == 2 && dlHeaps == 1 && diff <= 65536)) {
            printf("FAIL          expect split: want 2 heaps, one DEVICE_LOCAL of about %llu; got %u heap(s), "
                   "%u DEVICE_LOCAL of %llu\n", (unsigned long long)want, mp->memoryHeapCount, dlHeaps,
                   (unsigned long long)dlTotal);
            failures++;
        } else printf("expect        split: PASS (device-local %llu, 2T/3 %llu)\n", (unsigned long long)dlTotal,
                      (unsigned long long)want);
    }

    if (allocMib && hasBudget && dlIndex != UINT32_MAX) {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        const char *budgetExt = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t type = UINT32_MAX;
        uint64_t grew;
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &budgetExt;
        VK(vkCreateDevice(phys[deviceIndex], &dci, NULL, &dev));
        DEVICE_FUNCS(LOAD_DEVICE)
        for (uint32_t i = 0; i < mp->memoryTypeCount && type == UINT32_MAX; i++)
            if (mp->memoryTypes[i].heapIndex == dlIndex &&
                (mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        if (type == UINT32_MAX) Missing("a DEVICE_LOCAL memory type");
        Read(phys[deviceIndex], &before);
        mai.allocationSize = (VkDeviceSize)allocMib << 20;
        mai.memoryTypeIndex = type;
        VK(vkAllocateMemory(dev, &mai, NULL, &memory));
        Read(phys[deviceIndex], &after);
        grew = after.budget.heapUsage[dlIndex] - before.budget.heapUsage[dlIndex];
        printf("budget alloc  %u MiB type %u heap %u: usage %llu -> %llu (+%.1f MiB), budget %llu -> %llu\n",
               allocMib, type, dlIndex, (unsigned long long)before.budget.heapUsage[dlIndex],
               (unsigned long long)after.budget.heapUsage[dlIndex], (double)(int64_t)grew / MIB,
               (unsigned long long)before.budget.heapBudget[dlIndex],
               (unsigned long long)after.budget.heapBudget[dlIndex]);
        if (after.budget.heapUsage[dlIndex] < before.budget.heapUsage[dlIndex] + mai.allocationSize) {
            printf("FAIL          the heap usage grew by less than the allocation\n");
            failures++;
        }
        vkFreeMemory(dev, memory, NULL);
        vkDestroyDevice(dev, NULL);
    }
    vkDestroyInstance(instance, NULL);
    SetEvent(deadlineEvent);
    printf("result        %s (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
