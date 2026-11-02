/* vk_wbs_hang: a timeline semaphore wait-before-signal test for a Vulkan ICD (BD-200).
   Usage: vk_wbs_hang_64.exe [--icd <absolute path of the ICD DLL>] [results file]
   With --icd the DLL is loaded directly through vk_icdGetInstanceProcAddr (interface version 5), without the
   Vulkan loader, so an ICD that no loader manifest names (the hosted ICD) can be measured. Without --icd the
   program goes through vulkan-1.dll.
   Three cases: 1. GPU signal -> CPU wait, 2. vkQueueSubmit wait(1)+signal(2), then the host signals 1 and waits
   for 2, 3. the same through vkQueueSubmit2 (SKIP when the device has no vkQueueSubmit2). If a submit blocks the
   application thread on the wait, the host signal never comes: the 20 s watchdog of the case fires and the last
   "step:" line names the blocked call. The release of the wait must come from the host (see host_signal).
   stdout and stderr are unbuffered, a "step:" line comes before every Vulkan call, and every function pointer is
   checked: a NULL one prints "PROBE FAIL <name> NULL" and the program ends with exit 2 without the call. Core
   1.2/1.3 names fall back to their KHR names. Each loaded module that exports vk_icdGetInstanceProcAddr is printed
   as "icd module: <first 8 hex of its SHA-256> <path>", so the result names the ICD that ran.
   Exit 0 = PASS ("PROBE PASS 3/3"), 1 = FAIL, 2 = missing function or test error ("PROBE FAIL ..."), 3 = HANG: a
   case wait timed out (10 s) or the 20 s watchdog of a case fired ("PROBE HANG ...").
   Build: build.ps1 next to this file (clang-cl, x86 and x64). */
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>

static HMODULE vkdll;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfi;
static const char *g_results_path;

static void write_result(const char *line) {
    if (!g_results_path) return;
    FILE *f = fopen(g_results_path, "a");
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
}

static void probe_line(const char *line) {
    puts(line);
    write_result(line);
}

static void step(const char *what) { printf("step: %s\n", what); }

/* Instance-level and device-level lookups with a NULL check. alt is the KHR name or NULL. */
static PFN_vkVoidFunction iload(VkInstance inst, const char *name, const char *alt) {
    printf("step: vkGetInstanceProcAddr %s\n", name);
    PFN_vkVoidFunction f = gipa(inst, name);
    if (!f && alt) { printf("step: vkGetInstanceProcAddr %s\n", alt); f = gipa(inst, alt); }
    if (!f) { char b[160]; snprintf(b, sizeof b, "PROBE FAIL %s NULL", name); probe_line(b); }
    return f;
}
static PFN_vkVoidFunction dload(const char *name, const char *alt) {
    printf("step: vkGetDeviceProcAddr %s\n", name);
    PFN_vkVoidFunction f = gdpa(g_dev, name);
    if (!f && alt) { printf("step: vkGetDeviceProcAddr %s\n", alt); f = gdpa(g_dev, alt); }
    if (!f) { char b[160]; snprintf(b, sizeof b, "PROBE FAIL %s NULL", name); probe_line(b); }
    return f;
}
#define ILOAD(fn) PFN_##fn fn = (PFN_##fn)iload(g_inst, #fn, NULL); if (!fn) return 2;
#define ILOAD2(fn, alt) PFN_##fn fn = (PFN_##fn)iload(g_inst, #fn, alt); if (!fn) return 2;
#define DLOAD(fn) PFN_##fn fn = (PFN_##fn)dload(#fn, NULL); if (!fn) return 2;
#define DLOAD2(fn, alt) PFN_##fn fn = (PFN_##fn)dload(#fn, alt); if (!fn) return 2;

static void print_loaded_icds(void) {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) { puts("icd module: EnumProcessModules failed"); return; }
    if (needed > sizeof mods) needed = sizeof mods;
    int found = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        if (!GetProcAddress(mods[i], "vk_icdGetInstanceProcAddr")) continue;
        char path[MAX_PATH] = "";
        if (!GetModuleFileNameA(mods[i], path, sizeof path)) continue;
        char hex[17] = "????????????????";
        BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; UCHAR d[32];
        HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE && BCryptOpenAlgorithmProvider(&alg, L"SHA256", NULL, 0) == 0) {
            if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
                static BYTE buf[65536]; DWORD n;
                while (ReadFile(fh, buf, sizeof buf, &n, NULL) && n) BCryptHashData(h, buf, n, 0);
                BCryptFinishHash(h, d, sizeof d, 0);
                BCryptDestroyHash(h);
                for (int k = 0; k < 8; k++) sprintf(hex + 2 * k, "%02X", d[k]);
            }
            BCryptCloseAlgorithmProvider(alg, 0);
        }
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        printf("icd module: %.8s %s\n", hex, path);
        found++;
    }
    if (!found) puts("icd module: none loaded");
}

static DWORD WINAPI watchdog_thread(LPVOID param) {
    int case_num = (int)(intptr_t)param;
    Sleep(20000);
    char buf[64];
    snprintf(buf, sizeof buf, "PROBE HANG case-%d", case_num);
    probe_line(buf);
    ExitProcess(3);
    return 0;
}
static HANDLE start_watchdog(int c) { return CreateThread(NULL, 0, watchdog_thread, (LPVOID)(intptr_t)c, 0, NULL); }
static void stop_watchdog(HANDLE h) { if (h) { TerminateThread(h, 0); CloseHandle(h); } }

static int init_vulkan(void) {
    step("vkGetInstanceProcAddr(NULL, vkCreateInstance)");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { probe_line("PROBE FAIL vkCreateInstance NULL"); return 2; }
    step("vkGetInstanceProcAddr(NULL, vkEnumerateInstanceVersion)");
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t ver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) { step("vkEnumerateInstanceVersion"); vkEnumerateInstanceVersion(&ver); }
    else puts("vkEnumerateInstanceVersion NULL: a 1.0 loader");
    printf("loader instance version: %u.%u.%u\n", VK_VERSION_MAJOR(ver), VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver));

    /* VK_KHR_get_physical_device_properties2 for the KHR fallback of vkGetPhysicalDeviceFeatures2. */
    static const char *iexts[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    uint32_t niext = 0;
    step("vkGetInstanceProcAddr(NULL, vkEnumerateInstanceExtensionProperties)");
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (vkEnumerateInstanceExtensionProperties) {
        VkExtensionProperties ep[128]; uint32_t n = 128;
        step("vkEnumerateInstanceExtensionProperties");
        vkEnumerateInstanceExtensionProperties(NULL, &n, ep);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, iexts[0])) niext = 1;
    }

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vk_wbs_hang";
    ai.apiVersion = ver >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iexts;
    step("vkCreateInstance");
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    printf("vkCreateInstance: %d\n", r);
    if (r != VK_SUCCESS) { probe_line("PROBE FAIL vkCreateInstance"); return 2; }

    ILOAD(vkEnumeratePhysicalDevices);
    ILOAD(vkGetPhysicalDeviceProperties);
    ILOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    ILOAD(vkCreateDevice);
    ILOAD(vkEnumerateDeviceExtensionProperties);
    ILOAD2(vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2KHR");

    uint32_t cnt = 0;
    step("vkEnumeratePhysicalDevices (count)");
    r = vkEnumeratePhysicalDevices(g_inst, &cnt, NULL);
    printf("vkEnumeratePhysicalDevices: %d count %u\n", r, cnt);
    if (r != VK_SUCCESS || !cnt) { probe_line("PROBE FAIL no physical device"); return 2; }
    cnt = 1;
    step("vkEnumeratePhysicalDevices (first)");
    r = vkEnumeratePhysicalDevices(g_inst, &cnt, &g_phys);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !g_phys) { probe_line("PROBE FAIL vkEnumeratePhysicalDevices"); return 2; }
    print_loaded_icds();

    VkPhysicalDeviceProperties pdp;
    step("vkGetPhysicalDeviceProperties");
    vkGetPhysicalDeviceProperties(g_phys, &pdp);
    printf("device: %s (VEN_%04X DEV_%04X) apiVersion %u.%u.%u\n", pdp.deviceName, pdp.vendorID, pdp.deviceID,
           VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion), VK_VERSION_PATCH(pdp.apiVersion));

    uint32_t qfc = 0;
    step("vkGetPhysicalDeviceQueueFamilyProperties");
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qfc, NULL);
    VkQueueFamilyProperties qfp[16];
    if (qfc > 16) qfc = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qfc, qfp);
    g_qfi = 0;
    for (uint32_t i = 0; i < qfc; i++)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfi = i; break; }
    printf("queue family %u of %u\n", g_qfi, qfc);

    int has_ts = 0, has_s2 = 0;
    {
        VkExtensionProperties ep[512]; uint32_t n = 512;
        step("vkEnumerateDeviceExtensionProperties");
        vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ep);
        for (uint32_t i = 0; i < n; i++) {
            if (!strcmp(ep[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) has_ts = 1;
            if (!strcmp(ep[i].extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) has_s2 = 1;
        }
        printf("device extensions: %u, timeline_semaphore %d, synchronization2 %d\n", n, has_ts, has_s2);
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceSynchronization2Features s2f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    s2f.pNext = &tsf;
    VkPhysicalDeviceFeatures2 feat2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    feat2.pNext = &s2f;
    step("vkGetPhysicalDeviceFeatures2");
    vkGetPhysicalDeviceFeatures2(g_phys, &feat2);
    printf("timelineSemaphore=%d synchronization2=%d\n", tsf.timelineSemaphore, s2f.synchronization2);
    if (!tsf.timelineSemaphore) { probe_line("PROBE FAIL timelineSemaphore not supported"); return 2; }
    if (!has_s2) s2f.synchronization2 = VK_FALSE;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    dqci.queueFamilyIndex = g_qfi;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &prio;
    const char *dexts[2]; uint32_t nd = 0;
    if (has_ts) dexts[nd++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    if (has_s2 && s2f.synchronization2) dexts[nd++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &s2f;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = nd;
    dci.ppEnabledExtensionNames = dexts;
    step("vkCreateDevice");
    r = vkCreateDevice(g_phys, &dci, NULL, &g_dev);
    printf("vkCreateDevice: %d\n", r);
    if (r != VK_SUCCESS) { probe_line("PROBE FAIL vkCreateDevice"); return 2; }

    step("vkGetInstanceProcAddr vkGetDeviceProcAddr");
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(g_inst, "vkGetDeviceProcAddr");
    if (!gdpa) { probe_line("PROBE FAIL vkGetDeviceProcAddr NULL"); return 2; }
    DLOAD(vkGetDeviceQueue);
    step("vkGetDeviceQueue");
    vkGetDeviceQueue(g_dev, g_qfi, 0, &g_queue);
    if (!g_queue) { probe_line("PROBE FAIL vkGetDeviceQueue returned no queue"); return 2; }
    puts("init: OK");
    return 0;
}

/* Device functions used by the cases, loaded once after init. */
static PFN_vkCreateSemaphore pCreateSemaphore;
static PFN_vkDestroySemaphore pDestroySemaphore;
static PFN_vkWaitSemaphores pWaitSemaphores;
static PFN_vkGetSemaphoreCounterValue pGetSemaphoreCounterValue;
static PFN_vkQueueSubmit pQueueSubmit;
static PFN_vkSignalSemaphore pSignalSemaphore;
static PFN_vkQueueSubmit2 pQueueSubmit2;   /* optional */

static int load_case_functions(void) {
    if (!(pCreateSemaphore = (PFN_vkCreateSemaphore)dload("vkCreateSemaphore", NULL))) return 2;
    if (!(pDestroySemaphore = (PFN_vkDestroySemaphore)dload("vkDestroySemaphore", NULL))) return 2;
    if (!(pWaitSemaphores = (PFN_vkWaitSemaphores)dload("vkWaitSemaphores", "vkWaitSemaphoresKHR"))) return 2;
    if (!(pGetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)dload("vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR"))) return 2;
    if (!(pQueueSubmit = (PFN_vkQueueSubmit)dload("vkQueueSubmit", NULL))) return 2;
    if (!(pSignalSemaphore = (PFN_vkSignalSemaphore)dload("vkSignalSemaphore", "vkSignalSemaphoreKHR"))) return 2;
    step("vkGetDeviceProcAddr vkQueueSubmit2 (optional)");
    pQueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(g_dev, "vkQueueSubmit2");
    if (!pQueueSubmit2) { step("vkGetDeviceProcAddr vkQueueSubmit2KHR (optional)"); pQueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(g_dev, "vkQueueSubmit2KHR"); }
    printf("vkQueueSubmit2: %s\n", pQueueSubmit2 ? "present" : "absent (case 3 skipped)");
    return 0;
}

static VkSemaphore create_timeline(uint64_t initial) {
    VkSemaphoreTypeCreateInfo stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    stci.initialValue = initial;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &stci;
    VkSemaphore sem = VK_NULL_HANDLE;
    step("vkCreateSemaphore (timeline)");
    VkResult r = pCreateSemaphore(g_dev, &sci, NULL, &sem);
    if (r != VK_SUCCESS) { printf("  vkCreateSemaphore: %d\n", r); return VK_NULL_HANDLE; }
    return sem;
}

static VkResult wait_value(VkSemaphore sem, uint64_t v, uint64_t *counter) {
    VkSemaphoreWaitInfo swi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    swi.semaphoreCount = 1;
    swi.pSemaphores = &sem;
    swi.pValues = &v;
    printf("step: vkWaitSemaphores value %llu (10 s)\n", (unsigned long long)v);
    VkResult r = pWaitSemaphores(g_dev, &swi, 10000000000ULL);
    *counter = 0;
    step("vkGetSemaphoreCounterValue");
    pGetSemaphoreCounterValue(g_dev, sem, counter);
    printf("  vkWaitSemaphores(%llu): %d counter: %llu\n", (unsigned long long)v, r, (unsigned long long)*counter);
    return r;
}

/* The signal that releases a wait-before-signal comes from the host (vkSignalSemaphore). A later submit on the
   same queue is no valid release: queue submission order gives no forward-progress guarantee there, and a
   conforming driver times out on it as well. */
static VkResult host_signal(VkSemaphore sem, uint64_t v) {
    VkSemaphoreSignalInfo ssi = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    ssi.semaphore = sem;
    ssi.value = v;
    printf("step: vkSignalSemaphore value %llu (host)\n", (unsigned long long)v);
    VkResult r = pSignalSemaphore(g_dev, &ssi);
    printf("  vkSignalSemaphore(%llu): %d\n", (unsigned long long)v, r);
    return r;
}

static int case1_one_to_n(void) {
    puts("case-1: GPU signal -> CPU vkWaitSemaphores");
    HANDLE wd = start_watchdog(1);
    VkSemaphore sem = create_timeline(0);
    if (!sem) { stop_watchdog(wd); return 2; }
    uint64_t sigVal = 1;
    VkTimelineSemaphoreSubmitInfo tssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues = &sigVal;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &tssi;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem;
    step("vkQueueSubmit signal(1)");
    VkResult r = pQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    printf("  vkQueueSubmit: %d\n", r);
    uint64_t val = 0;
    if (r == VK_SUCCESS) r = wait_value(sem, 1, &val);
    step("vkDestroySemaphore");
    pDestroySemaphore(g_dev, sem, NULL);
    stop_watchdog(wd);
    return (r == VK_SUCCESS && val >= 1) ? 0 : 1;
}

static int case2_wbs_same_queue(void) {
    puts("case-2: wait-before-signal, vkQueueSubmit wait(1)+signal(2), host signal(1)");
    HANDLE wd = start_watchdog(2);
    VkSemaphore sem = create_timeline(0);
    if (!sem) { stop_watchdog(wd); return 2; }
    uint64_t waitVal = 1, sigVal = 2;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tssi.waitSemaphoreValueCount = 1;
    tssi.pWaitSemaphoreValues = &waitVal;
    tssi.signalSemaphoreValueCount = 1;
    tssi.pSignalSemaphoreValues = &sigVal;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &tssi;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem;
    si.pWaitDstStageMask = &waitStage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem;
    step("vkQueueSubmit wait(1)+signal(2)");
    VkResult r = pQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    printf("  submit wait(1)+signal(2): %d\n", r);
    if (r == VK_SUCCESS) r = host_signal(sem, 1);

    uint64_t val = 0;
    if (r == VK_SUCCESS) r = wait_value(sem, 2, &val);
    step("vkDestroySemaphore");
    pDestroySemaphore(g_dev, sem, NULL);
    stop_watchdog(wd);
    if (r == VK_TIMEOUT) { puts("  HANG: timeout - the defect"); return 3; }
    return (r == VK_SUCCESS && val >= 2) ? 0 : 1;
}

static int case3_wbs_submit2(void) {
    puts("case-3: wait-before-signal, vkQueueSubmit2 wait(1)+signal(2), host signal(1)");
    if (!pQueueSubmit2) { puts("  SKIP: no vkQueueSubmit2"); return -1; }
    HANDLE wd = start_watchdog(3);
    VkSemaphore sem = create_timeline(0);
    if (!sem) { stop_watchdog(wd); return 2; }
    VkSemaphoreSubmitInfo waitSsi = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    waitSsi.semaphore = sem; waitSsi.value = 1; waitSsi.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo sigSsi = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    sigSsi.semaphore = sem; sigSsi.value = 2; sigSsi.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si2.waitSemaphoreInfoCount = 1; si2.pWaitSemaphoreInfos = &waitSsi;
    si2.signalSemaphoreInfoCount = 1; si2.pSignalSemaphoreInfos = &sigSsi;
    step("vkQueueSubmit2 wait(1)+signal(2)");
    VkResult r = pQueueSubmit2(g_queue, 1, &si2, VK_NULL_HANDLE);
    printf("  submit2 wait(1)+signal(2): %d\n", r);
    if (r == VK_SUCCESS) r = host_signal(sem, 1);

    uint64_t val = 0;
    if (r == VK_SUCCESS) r = wait_value(sem, 2, &val);
    step("vkDestroySemaphore");
    pDestroySemaphore(g_dev, sem, NULL);
    stop_watchdog(wd);
    if (r == VK_TIMEOUT) { puts("  HANG: timeout - the defect"); return 3; }
    return (r == VK_SUCCESS && val >= 2) ? 0 : 1;
}

static void cleanup(void) {
    if (g_dev && gdpa) {
        PFN_vkDeviceWaitIdle f1 = (PFN_vkDeviceWaitIdle)gdpa(g_dev, "vkDeviceWaitIdle");
        PFN_vkDestroyDevice f2 = (PFN_vkDestroyDevice)gdpa(g_dev, "vkDestroyDevice");
        if (f1) { step("vkDeviceWaitIdle"); f1(g_dev); }
        if (f2) { step("vkDestroyDevice"); f2(g_dev, NULL); }
    }
    if (g_inst) {
        PFN_vkDestroyInstance f3 = (PFN_vkDestroyInstance)gipa(g_inst, "vkDestroyInstance");
        if (f3) { step("vkDestroyInstance"); f3(g_inst, NULL); }
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("vk_wbs_hang v5 - BD-200 wait-before-signal hang test, bitness=%d\n", (int)(sizeof(void *) * 8));
    const char *icd_path = NULL;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--icd") && a + 1 < argc) icd_path = argv[++a];
        else if (!g_results_path) g_results_path = argv[a];
    }

    if (icd_path) {
        /* Direct ICD load, without the Vulkan loader (the vkfillcheck --icd route). */
        wchar_t wide[MAX_PATH * 2];
        if (!MultiByteToWideChar(CP_ACP, 0, icd_path, -1, wide, MAX_PATH * 2) ||
            !(wide[0] && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/'))) {
            probe_line("PROBE FAIL --icd needs an absolute path");
            return 2;
        }
        printf("step: LoadLibraryEx %s\n", icd_path);
        vkdll = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!vkdll) { char b[96]; snprintf(b, sizeof b, "PROBE FAIL LoadLibrary(--icd) error %lu", GetLastError()); probe_line(b); return 2; }
        step("GetProcAddress vk_icdGetInstanceProcAddr");
        gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vkdll, "vk_icdGetInstanceProcAddr");
        if (!gipa) { probe_line("PROBE FAIL vk_icdGetInstanceProcAddr NULL"); return 2; }
        step("GetProcAddress vk_icdNegotiateLoaderICDInterfaceVersion");
        PFN_vkNegotiateLoaderICDInterfaceVersion negotiate =
            (PFN_vkNegotiateLoaderICDInterfaceVersion)GetProcAddress(vkdll, "vk_icdNegotiateLoaderICDInterfaceVersion");
        uint32_t iv = 5;
        if (negotiate) { step("vk_icdNegotiateLoaderICDInterfaceVersion"); VkResult nr = negotiate(&iv); printf("icd interface: %d version %u\n", nr, iv); }
        else puts("icd interface: no negotiation export");
        char p[MAX_PATH] = "";
        GetModuleFileNameA(vkdll, p, sizeof p);
        printf("icd (direct): %s\n", p);
    } else {
        step("LoadLibrary vulkan-1.dll");
        vkdll = LoadLibraryA("vulkan-1.dll");
        if (!vkdll) { probe_line("PROBE FAIL LoadLibrary(vulkan-1.dll)"); return 2; }
        char p[MAX_PATH] = "";
        GetModuleFileNameA(vkdll, p, sizeof p);
        printf("loader: %s\n", p);
        step("GetProcAddress vkGetInstanceProcAddr");
        gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vkdll, "vkGetInstanceProcAddr");
        if (!gipa) { probe_line("PROBE FAIL vkGetInstanceProcAddr NULL"); return 2; }
    }
    int ir = init_vulkan();
    if (!ir) ir = load_case_functions();
    if (ir) { char b[64]; snprintf(b, sizeof b, "PROBE FAIL init (exit %d)", ir); probe_line(b); cleanup(); return 2; }

    int f1 = case1_one_to_n();
    printf("case-1: %s\n", f1 == 0 ? "PASS" : f1 == 2 ? "ERROR" : "FAIL");
    write_result(f1 == 0 ? "case-1: PASS" : f1 == 2 ? "case-1: ERROR" : "case-1: FAIL");
    if (f1 == 2) { probe_line("PROBE FAIL case-1 error"); cleanup(); return 2; }

    int f2 = case2_wbs_same_queue();
    { const char *t = f2 == 3 ? "case-2: TIMEOUT" : f2 == 2 ? "case-2: ERROR" : f2 ? "case-2: FAIL" : "case-2: PASS"; puts(t); write_result(t); }

    int f3 = case3_wbs_submit2();
    if (f3 == -1) { puts("case-3: SKIP"); write_result("case-3: SKIP"); f3 = 0; }
    else {
        const char *t = f3 == 3 ? "case-3: TIMEOUT" : f3 == 2 ? "case-3: ERROR" : f3 ? "case-3: FAIL" : "case-3: PASS"; puts(t); write_result(t);
    }

    cleanup();
    int hangs = (f2 == 3) + (f3 == 3);
    int errors = (f2 == 2) + (f3 == 2);
    int fails = (f1 != 0) + (f2 == 1) + (f3 == 1);
    char probe[80];
    if (errors) snprintf(probe, sizeof probe, "PROBE FAIL %d case error(s)", errors);
    else if (hangs) snprintf(probe, sizeof probe, "PROBE HANG %d hang(s) %d fail(s)", hangs, fails);
    else if (fails) snprintf(probe, sizeof probe, "PROBE FAIL %d failure(s)", fails);
    else snprintf(probe, sizeof probe, "PROBE PASS 3/3");
    probe_line(probe);
    return errors ? 2 : hangs ? 3 : fails ? 1 : 0;
}
