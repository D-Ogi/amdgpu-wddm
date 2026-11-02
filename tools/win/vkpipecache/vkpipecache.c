#define _CRT_SECURE_NO_WARNINGS
/*
 * vkpipecache: measure Vulkan pipeline compilation and the Mesa disk shader cache.
 *
 * Creates N compute pipelines from one SPIR-V module with different specialization constants
 * (each constant value forces a separate cache entry). Reports pipeline creation time, the
 * VkPipelineCache data size, and the number of files written under MESA_SHADER_CACHE_DIR.
 *
 * The ICD comes from --icd (direct load, no Vulkan loader) or from --manifest (VK_DRIVER_FILES).
 * An elevated process, which every SSH session on the lab is, has the loader ignore VK_DRIVER_FILES:
 * --icd is the lab route.
 *
 * Exit: 0 ok, 1 a pipeline or verification failed, 2 usage, 3 Vulkan or system error.
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

#include "fill_spv.h"

#define MAX_PIPELINES 256

#define X_INSTANCE_FN \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkEnumerateDeviceExtensionProperties)

#define X_DEVICE_FN \
    X(vkDestroyDevice) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreatePipelineCache) \
    X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) X(vkMergePipelineCaches) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)

#define X(fn) static PFN_##fn fn;
X_INSTANCE_FN
X_DEVICE_FN
#undef X

static VkInstance instance;
static VkDevice dev;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(3);
}

static void vkcheck(VkResult r, const char *call) {
    if (r != VK_SUCCESS) die("%s: VkResult %d", call, (int)r);
}
#define VK(call) vkcheck(call, #call)

static void sha256file(const char *path, char out[65]) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    UCHAR digest[32];
    BCryptOpenAlgorithmProvider(&alg, L"SHA256", NULL, 0);
    BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        BYTE buf[65536];
        DWORD n;
        while (ReadFile(f, buf, sizeof buf, &n, NULL) && n)
            BCryptHashData(hash, buf, n, 0);
        CloseHandle(f);
    }
    BCryptFinishHash(hash, digest, sizeof digest, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02X", digest[i]);
}

static void print_loaded_icds(void) {
    HMODULE modules[256];
    DWORD needed;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return;
    DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; i++) {
        if (!GetProcAddress(modules[i], "vk_icdGetInstanceProcAddr")) continue;
        char path[MAX_PATH];
        if (!GetModuleFileNameA(modules[i], path, sizeof path)) continue;
        char hash[65];
        sha256file(path, hash);
        printf("icd module: %.8s %s\n", hash, path);
        fflush(stdout);
    }
}

static int count_dir_files(const char *dir) {
    if (!dir || !dir[0]) return -1;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static double now_ms(void) {
    static double scale = 0.0;
    if (scale == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        scale = 1000.0 / (double)f.QuadPart;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * scale;
}

int main(int argc, char **argv) {
    const char *icdPath = NULL;
    const char *manifestPath = NULL;
    const char *cacheDir = NULL;
    int pipelines = 16;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help")) {
            puts(
                "vkpipecache [--icd DLL | --manifest JSON] [--pipelines N] [--cache-dir DIR]\n"
                "  --icd DLL       load this ICD DLL directly (lab route for elevated sessions)\n"
                "  --manifest JSON hand this ICD manifest to the loader (VK_DRIVER_FILES)\n"
                "  --pipelines N   number of compute pipelines to create (default 16, max 256)\n"
                "  --cache-dir DIR directory to check for Mesa disk cache files (default: $MESA_SHADER_CACHE_DIR)\n"
                "Exit: 0 ok, 1 pipeline or verification failed, 2 usage, 3 Vulkan/system error.");
            return 0;
        }
        if (i + 1 >= argc) { fputs("missing argument value\n", stderr); return 2; }
        if (!strcmp(a, "--icd")) icdPath = argv[++i];
        else if (!strcmp(a, "--manifest")) manifestPath = argv[++i];
        else if (!strcmp(a, "--pipelines")) { pipelines = atoi(argv[++i]); if (pipelines < 1 || pipelines > MAX_PIPELINES) { fprintf(stderr, "--pipelines: 1..%d\n", MAX_PIPELINES); return 2; } }
        else if (!strcmp(a, "--cache-dir")) cacheDir = argv[++i];
        else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
    }

    if (icdPath && manifestPath) { fputs("--icd and --manifest are mutually exclusive\n", stderr); return 2; }

    if (!cacheDir) cacheDir = getenv("MESA_SHADER_CACHE_DIR");

    int filesBefore = count_dir_files(cacheDir);

    PFN_vkGetInstanceProcAddr gipa = NULL;
    PFN_vkCreateInstance vkCreateInstance = NULL;
    HMODULE loaderMod = NULL;

    if (icdPath) {
        HMODULE icd = LoadLibraryA(icdPath);
        if (!icd) die("LoadLibrary(%s): %lu", icdPath, GetLastError());
        typedef PFN_vkGetInstanceProcAddr (*PFN_negotiate)(uint32_t *);
        PFN_negotiate negotiate = (PFN_negotiate)GetProcAddress(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
        if (negotiate) {
            uint32_t ver = 5;
            negotiate(&ver);
        }
        PFN_vkGetInstanceProcAddr icd_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");
        if (!icd_gipa) die("%s: no vk_icdGetInstanceProcAddr", icdPath);
        gipa = icd_gipa;
    } else {
        if (manifestPath) SetEnvironmentVariableA("VK_DRIVER_FILES", manifestPath);
        loaderMod = LoadLibraryA("vulkan-1.dll");
        if (!loaderMod) die("LoadLibrary(vulkan-1.dll): %lu", GetLastError());
        gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loaderMod, "vkGetInstanceProcAddr");
        if (!gipa) die("vulkan-1.dll: no vkGetInstanceProcAddr");
    }

    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) die("vkCreateInstance not found");

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vkpipecache";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VK(vkCreateInstance(&ici, NULL, &instance));

#define X(fn) fn = (PFN_##fn)gipa(instance, #fn); if (!fn) die(#fn " not found");
    X_INSTANCE_FN
#undef X

    uint32_t ndev = 0;
    VK(vkEnumeratePhysicalDevices(instance, &ndev, NULL));
    if (!ndev) die("no Vulkan devices");
    VkPhysicalDevice *pdevs = calloc(ndev, sizeof *pdevs);
    VK(vkEnumeratePhysicalDevices(instance, &ndev, pdevs));

    VkPhysicalDevice pdev = pdevs[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pdev, &props);
    printf("device: %04x:%04x %s (api %u.%u.%u)\n", props.vendorID, props.deviceID, props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    fflush(stdout);
    free(pdevs);

    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nfam, NULL);
    VkQueueFamilyProperties *fams = calloc(nfam, sizeof *fams);
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nfam, fams);
    uint32_t computeFamily = UINT32_MAX;
    for (uint32_t i = 0; i < nfam; i++) {
        if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { computeFamily = i; break; }
    }
    free(fams);
    if (computeFamily == UINT32_MAX) die("no compute queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = computeFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(pdev, &dci, NULL, &dev));

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
    if (!gdpa) die("vkGetDeviceProcAddr not found");
#define X(fn) fn = (PFN_##fn)gdpa(dev, #fn); if (!fn) die(#fn " not found (device)");
    X_DEVICE_FN
#undef X

    print_loaded_icds();

    VkShaderModule module;
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = sizeof fill_spv;
    smci.pCode = fill_spv;
    VK(vkCreateShaderModule(dev, &smci, NULL, &module));

    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    VkDescriptorSetLayout dsl;
    VK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

    VkPipelineCacheCreateInfo pcci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkPipelineCache cache;
    VK(vkCreatePipelineCache(dev, &pcci, NULL, &cache));

    VkPipeline pipes[MAX_PIPELINES];
    double t0 = now_ms();

    for (int i = 0; i < pipelines; i++) {
        uint32_t seed = (uint32_t)(i + 1);
        VkSpecializationMapEntry sme = { 0, 0, sizeof(uint32_t) };
        VkSpecializationInfo si = { 1, &sme, sizeof(uint32_t), &seed };
        VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.stage.pSpecializationInfo = &si;
        cpci.layout = layout;
        VkResult r = vkCreateComputePipelines(dev, cache, 1, &cpci, NULL, &pipes[i]);
        if (r != VK_SUCCESS) {
            printf("FAIL vkCreateComputePipelines[%d]: VkResult %d\n", i, (int)r);
            fflush(stdout);
            for (int j = 0; j < i; j++) vkDestroyPipeline(dev, pipes[j], NULL);
            vkDestroyPipelineCache(dev, cache, NULL);
            vkDestroyPipelineLayout(dev, layout, NULL);
            vkDestroyDescriptorSetLayout(dev, dsl, NULL);
            vkDestroyShaderModule(dev, module, NULL);
            vkDestroyDevice(dev, NULL);
            vkDestroyInstance(instance, NULL);
            return 1;
        }
    }

    double t1 = now_ms();

    size_t cacheSize = 0;
    VK(vkGetPipelineCacheData(dev, cache, &cacheSize, NULL));

    int filesAfter = count_dir_files(cacheDir);

    printf("pipelines: %d created in %.1f ms (%.2f ms/pipeline)\n", pipelines, t1 - t0, (t1 - t0) / pipelines);
    printf("pipeline_cache_bytes: %zu\n", cacheSize);
    printf("cache_dir: %s\n", cacheDir ? cacheDir : "(not set)");
    printf("cache_dir_files_before: %d\n", filesBefore);
    printf("cache_dir_files_after: %d\n", filesAfter);
    printf("cache_dir_files_new: %d\n", (filesBefore >= 0 && filesAfter >= 0) ? filesAfter - filesBefore : -1);
    fflush(stdout);

    for (int i = 0; i < pipelines; i++) vkDestroyPipeline(dev, pipes[i], NULL);
    vkDestroyPipelineCache(dev, cache, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, module, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
