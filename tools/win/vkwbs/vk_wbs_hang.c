/* vk_wbs_hang: a timeline semaphore wait-before-signal test for a Vulkan ICD (BD-200).
   Usage: vk_wbs_hang_64.exe [--icd <absolute path of the ICD DLL>] [results file]
   With --icd the DLL is loaded directly through vk_icdGetInstanceProcAddr (interface version 5), without the
   Vulkan loader, so an ICD that no loader manifest names (the hosted ICD) can be measured. Without --icd the
   program goes through vulkan-1.dll.
   Five cases: 1. GPU signal -> CPU wait, 2. vkQueueSubmit wait(1)+signal(2), then the host signals 1 and waits
   for 2, 3. the same through vkQueueSubmit2 (SKIP when the device has no vkQueueSubmit2), 4. the first
   tessellation draw of a queue behind a submit that waits for the host, 4b. the same with the first scratch
   dispatch (see case4_preamble_replacement). If a submit blocks the application thread on the wait, the host
   signal never comes: the 20 s watchdog of the case fires and the last "step:" line names the blocked call. The
   release of the wait must come from the host (see host_signal).
   stdout and stderr are unbuffered, a "step:" line comes before every Vulkan call, and every function pointer is
   checked: a NULL one prints "PROBE FAIL <name> NULL" and the program ends with exit 2 without the call. Core
   1.2/1.3 names fall back to their KHR names. Each loaded module that exports vk_icdGetInstanceProcAddr is printed
   as "icd module: <first 8 hex of its SHA-256> <path>", so the result names the ICD that ran.
   Exit 0 = PASS ("PROBE PASS 5/5"), 1 = FAIL, 2 = missing function or test error ("PROBE FAIL ..."), 3 = HANG: a
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
#include "vk_wbs_spirv.h"

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
    /* Case 4b runs with the number 5. */
    if (case_num == 5) snprintf(buf, sizeof buf, "PROBE HANG case-4b");
    else snprintf(buf, sizeof buf, "PROBE HANG case-%d", case_num);
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

/* Cases 4 and 4b (BD-200 analysis-r1): a submit that makes the queue replace its preamble, while an earlier submit
   that uses the old preamble waits for a host signal. Case 4: submit A is a vertex draw that waits for host value 1;
   submit B is the first tessellation draw of the queue (it needs the tessellation rings) and waits for A. Case 4b:
   the same with a compute dispatch that needs scratch memory in place of the tessellation draw, so a result of both
   shows that the trigger is the preamble replacement, not tessellation. vkQueueSubmit(B) runs on a worker thread
   with a 5 s bound. An ICD that frees the old preamble only after A completes, or that waits for A in the submit,
   blocks there until the host signal: that is the defect (BD-200), and the case reports TIMEOUT. Then the host
   signals value 1 and waits 5 s for value 3. Each case makes its own device, so that its queue has no rings and no
   scratch before submit B. */
typedef struct {
    VkDevice dev;
    VkQueue q;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkWaitSemaphores WaitSemaphores;
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue;
    PFN_vkSignalSemaphore SignalSemaphore;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdDispatch CmdDispatch;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
} CaseDev;

static PFN_vkVoidFunction dload_on(VkDevice d, const char *name, const char *alt) {
    printf("step: vkGetDeviceProcAddr %s\n", name);
    PFN_vkVoidFunction f = gdpa(d, name);
    if (!f && alt) { printf("step: vkGetDeviceProcAddr %s\n", alt); f = gdpa(d, alt); }
    if (!f) { char b[160]; snprintf(b, sizeof b, "PROBE FAIL %s NULL", name); probe_line(b); }
    return f;
}
#define CLOAD(field, fn) if (!(c->field = (PFN_##fn)dload_on(c->dev, #fn, NULL))) return 2;
#define CLOAD2(field, fn, alt) if (!(c->field = (PFN_##fn)dload_on(c->dev, #fn, alt))) return 2;

/* A device with timeline semaphores and, for case 4, tessellation shaders. Returns 0, 2 (error) or -1 (SKIP: the
   device has no tessellationShader). */
static int case_device(CaseDev *c, int tess) {
    memset(c, 0, sizeof *c);
    PFN_vkCreateDevice vkCreateDevice = (PFN_vkCreateDevice)iload(g_inst, "vkCreateDevice", NULL);
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures =
        (PFN_vkGetPhysicalDeviceFeatures)iload(g_inst, "vkGetPhysicalDeviceFeatures", NULL);
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)iload(g_inst, "vkEnumerateDeviceExtensionProperties", NULL);
    if (!vkCreateDevice || !vkGetPhysicalDeviceFeatures || !vkEnumerateDeviceExtensionProperties) return 2;
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    step("vkGetPhysicalDeviceFeatures");
    vkGetPhysicalDeviceFeatures(g_phys, &have);
    if (tess && !have.tessellationShader) return -1;
    want.tessellationShader = tess ? VK_TRUE : VK_FALSE;
    const char *dexts[1]; uint32_t nd = 0;
    {
        static VkExtensionProperties ep[512]; uint32_t n = 512;
        step("vkEnumerateDeviceExtensionProperties");
        vkEnumerateDeviceExtensionProperties(g_phys, NULL, &n, ep);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(ep[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) dexts[nd++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    tsf.timelineSemaphore = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    dqci.queueFamilyIndex = g_qfi;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &tsf;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = nd;
    dci.ppEnabledExtensionNames = dexts;
    dci.pEnabledFeatures = &want;
    step("vkCreateDevice (case device)");
    VkResult r = vkCreateDevice(g_phys, &dci, NULL, &c->dev);
    printf("  vkCreateDevice: %d\n", r);
    if (r != VK_SUCCESS) return 2;
    PFN_vkGetDeviceQueue GetDeviceQueue = (PFN_vkGetDeviceQueue)dload_on(c->dev, "vkGetDeviceQueue", NULL);
    if (!GetDeviceQueue) return 2;
    step("vkGetDeviceQueue (case device)");
    GetDeviceQueue(c->dev, g_qfi, 0, &c->q);
    if (!c->q) return 2;
    CLOAD(DestroyDevice, vkDestroyDevice) CLOAD(DeviceWaitIdle, vkDeviceWaitIdle)
    CLOAD(CreateSemaphore, vkCreateSemaphore) CLOAD(DestroySemaphore, vkDestroySemaphore)
    CLOAD2(WaitSemaphores, vkWaitSemaphores, "vkWaitSemaphoresKHR")
    CLOAD2(GetSemaphoreCounterValue, vkGetSemaphoreCounterValue, "vkGetSemaphoreCounterValueKHR")
    CLOAD2(SignalSemaphore, vkSignalSemaphore, "vkSignalSemaphoreKHR") CLOAD(QueueSubmit, vkQueueSubmit)
    CLOAD(CreateShaderModule, vkCreateShaderModule) CLOAD(DestroyShaderModule, vkDestroyShaderModule)
    CLOAD(CreatePipelineLayout, vkCreatePipelineLayout) CLOAD(DestroyPipelineLayout, vkDestroyPipelineLayout)
    CLOAD(CreateRenderPass, vkCreateRenderPass) CLOAD(DestroyRenderPass, vkDestroyRenderPass)
    CLOAD(CreateFramebuffer, vkCreateFramebuffer) CLOAD(DestroyFramebuffer, vkDestroyFramebuffer)
    CLOAD(CreateGraphicsPipelines, vkCreateGraphicsPipelines) CLOAD(CreateComputePipelines, vkCreateComputePipelines)
    CLOAD(DestroyPipeline, vkDestroyPipeline)
    CLOAD(CreateCommandPool, vkCreateCommandPool) CLOAD(DestroyCommandPool, vkDestroyCommandPool)
    CLOAD(AllocateCommandBuffers, vkAllocateCommandBuffers)
    CLOAD(BeginCommandBuffer, vkBeginCommandBuffer) CLOAD(EndCommandBuffer, vkEndCommandBuffer)
    CLOAD(CmdBeginRenderPass, vkCmdBeginRenderPass) CLOAD(CmdEndRenderPass, vkCmdEndRenderPass)
    CLOAD(CmdBindPipeline, vkCmdBindPipeline) CLOAD(CmdDraw, vkCmdDraw) CLOAD(CmdDispatch, vkCmdDispatch)
    CLOAD(CmdBindDescriptorSets, vkCmdBindDescriptorSets)
    CLOAD(CreateDescriptorSetLayout, vkCreateDescriptorSetLayout) CLOAD(DestroyDescriptorSetLayout, vkDestroyDescriptorSetLayout)
    CLOAD(CreateDescriptorPool, vkCreateDescriptorPool) CLOAD(DestroyDescriptorPool, vkDestroyDescriptorPool)
    CLOAD(AllocateDescriptorSets, vkAllocateDescriptorSets) CLOAD(UpdateDescriptorSets, vkUpdateDescriptorSets)
    CLOAD(CreateBuffer, vkCreateBuffer) CLOAD(DestroyBuffer, vkDestroyBuffer)
    CLOAD(GetBufferMemoryRequirements, vkGetBufferMemoryRequirements)
    CLOAD(AllocateMemory, vkAllocateMemory) CLOAD(FreeMemory, vkFreeMemory) CLOAD(BindBufferMemory, vkBindBufferMemory)
    return 0;
}

static VkShaderModule case_module(CaseDev *c, const uint32_t *code, size_t bytes, const char *name) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    printf("step: vkCreateShaderModule %s\n", name);
    VkResult r = c->CreateShaderModule(c->dev, &ci, NULL, &m);
    if (r != VK_SUCCESS) { printf("  vkCreateShaderModule %s: %d\n", name, r); return VK_NULL_HANDLE; }
    return m;
}

/* A graphics pipeline with rasterizer discard: no attachment, no fragment shader. tesc/tese VK_NULL_HANDLE = the
   vertex-only pipeline of submit A. */
static VkPipeline case_graphics(CaseDev *c, VkPipelineLayout layout, VkRenderPass rp, VkShaderModule vs,
                                VkShaderModule tesc, VkShaderModule tese) {
    VkPipelineShaderStageCreateInfo st[3];
    memset(st, 0, sizeof st);
    uint32_t n = 0;
    VkShaderStageFlagBits stage[3] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT };
    VkShaderModule mod[3] = { vs, tesc, tese };
    for (int i = 0; i < 3; i++) {
        if (!mod[i]) continue;
        st[n].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[n].stage = stage[i];
        st[n].module = mod[i];
        st[n].pName = "main";
        n++;
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = tesc ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineTessellationStateCreateInfo ts = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
    ts.patchControlPoints = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.rasterizerDiscardEnable = VK_TRUE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = n;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pTessellationState = tesc ? &ts : NULL;
    gp.pRasterizationState = &rs;
    gp.layout = layout;
    gp.renderPass = rp;
    gp.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    printf("step: vkCreateGraphicsPipelines %s\n", tesc ? "tessellation" : "vertex");
    VkResult r = c->CreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &gp, NULL, &p);
    if (r != VK_SUCCESS) { printf("  vkCreateGraphicsPipelines: %d\n", r); return VK_NULL_HANDLE; }
    return p;
}

static VkPipeline case_compute(CaseDev *c, VkPipelineLayout layout, VkShaderModule cs, const char *name) {
    VkComputePipelineCreateInfo cp = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = cs;
    cp.stage.pName = "main";
    cp.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    printf("step: vkCreateComputePipelines %s\n", name);
    VkResult r = c->CreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &cp, NULL, &p);
    if (r != VK_SUCCESS) { printf("  vkCreateComputePipelines %s: %d\n", name, r); return VK_NULL_HANDLE; }
    return p;
}

typedef struct { CaseDev *c; const VkSubmitInfo *si; VkResult r; } SubmitJob;
static DWORD WINAPI submit_thread(LPVOID param) {
    SubmitJob *j = (SubmitJob *)param;
    j->r = j->c->QueueSubmit(j->c->q, 1, j->si, VK_NULL_HANDLE);
    return 0;
}
static double ms_since(LARGE_INTEGER t0) {
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    return 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

/* kind 4 = the tessellation draw, kind 5 = the scratch dispatch (case 4b). Returns 0 PASS, 1 FAIL, 2 ERROR,
   3 TIMEOUT, -1 SKIP. The 20 s watchdog of the case covers every step that has no bound of its own. */
static int case4_preamble_replacement(int kind) {
    const char *label = kind == 4 ? "case-4" : "case-4b";
    printf("%s: %s behind a wait-before-signal (submit A waits for host value 1, submit B waits for A)\n", label,
           kind == 4 ? "the first tessellation draw of the queue" : "the first scratch dispatch of the queue");
    HANDLE wd = start_watchdog(kind);
    CaseDev cd, *c = &cd;
    int e = case_device(c, kind == 4);
    if (e == -1) { stop_watchdog(wd); puts("  SKIP: the device has no tessellationShader"); return -1; }
    if (e) { stop_watchdog(wd); return 2; }
    int rc = 2;
    VkShaderModule m0 = VK_NULL_HANDLE, m1 = VK_NULL_HANDLE, m2 = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipeline pa = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cb[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult r;

    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    if (kind == 5) {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslci.bindingCount = 1;
        dslci.pBindings = &b;
        step("vkCreateDescriptorSetLayout");
        if (c->CreateDescriptorSetLayout(c->dev, &dslci, NULL, &dsl) != VK_SUCCESS) goto out;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
    }
    step("vkCreatePipelineLayout");
    if (c->CreatePipelineLayout(c->dev, &plci, NULL, &layout) != VK_SUCCESS) goto out;

    if (kind == 4) {
        VkSubpassDescription sub = { 0 };
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        step("vkCreateRenderPass (no attachment)");
        if (c->CreateRenderPass(c->dev, &rpci, NULL, &rp) != VK_SUCCESS) goto out;
        VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = rp;
        fbci.width = 16; fbci.height = 16; fbci.layers = 1;
        step("vkCreateFramebuffer (no attachment)");
        if (c->CreateFramebuffer(c->dev, &fbci, NULL, &fb) != VK_SUCCESS) goto out;
        if (!(m0 = case_module(c, spv_point_vert, sizeof spv_point_vert, "point.vert"))) goto out;
        if (!(m1 = case_module(c, spv_patch_tesc, sizeof spv_patch_tesc, "patch.tesc"))) goto out;
        if (!(m2 = case_module(c, spv_patch_tese, sizeof spv_patch_tese, "patch.tese"))) goto out;
        if (!(pa = case_graphics(c, layout, rp, m0, VK_NULL_HANDLE, VK_NULL_HANDLE))) goto out;
        if (!(pb = case_graphics(c, layout, rp, m0, m1, m2))) goto out;
    } else {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = 64 * sizeof(uint32_t);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        step("vkCreateBuffer");
        if (c->CreateBuffer(c->dev, &bci, NULL, &buf) != VK_SUCCESS) goto out;
        VkMemoryRequirements mr;
        step("vkGetBufferMemoryRequirements");
        c->GetBufferMemoryRequirements(c->dev, buf, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        for (mai.memoryTypeIndex = 0; mai.memoryTypeIndex < 32 && !(mr.memoryTypeBits & (1u << mai.memoryTypeIndex)); mai.memoryTypeIndex++) {}
        step("vkAllocateMemory");
        if (mai.memoryTypeIndex == 32 || c->AllocateMemory(c->dev, &mai, NULL, &mem) != VK_SUCCESS) goto out;
        step("vkBindBufferMemory");
        if (c->BindBufferMemory(c->dev, buf, mem, 0) != VK_SUCCESS) goto out;
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &ps;
        step("vkCreateDescriptorPool");
        if (c->CreateDescriptorPool(c->dev, &dpci, NULL, &pool) != VK_SUCCESS) goto out;
        VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        step("vkAllocateDescriptorSets");
        if (c->AllocateDescriptorSets(c->dev, &dsai, &set) != VK_SUCCESS) goto out;
        VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &dbi;
        step("vkUpdateDescriptorSets");
        c->UpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
        if (!(m0 = case_module(c, spv_empty_comp, sizeof spv_empty_comp, "empty.comp"))) goto out;
        if (!(m1 = case_module(c, spv_scratch_comp, sizeof spv_scratch_comp, "scratch.comp"))) goto out;
        if (!(pa = case_compute(c, layout, m0, "empty"))) goto out;
        if (!(pb = case_compute(c, layout, m1, "scratch"))) goto out;
    }

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = g_qfi;
    step("vkCreateCommandPool");
    if (c->CreateCommandPool(c->dev, &cpci, NULL, &cpool) != VK_SUCCESS) goto out;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 2;
    step("vkAllocateCommandBuffers");
    if (c->AllocateCommandBuffers(c->dev, &cbai, cb) != VK_SUCCESS) goto out;
    for (int i = 0; i < 2; i++) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        printf("step: record command buffer %c\n", i ? 'B' : 'A');
        if (c->BeginCommandBuffer(cb[i], &bi) != VK_SUCCESS) goto out;
        if (kind == 4) {
            VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rbi.renderPass = rp;
            rbi.framebuffer = fb;
            rbi.renderArea.extent.width = 16;
            rbi.renderArea.extent.height = 16;
            c->CmdBeginRenderPass(cb[i], &rbi, VK_SUBPASS_CONTENTS_INLINE);
            c->CmdBindPipeline(cb[i], VK_PIPELINE_BIND_POINT_GRAPHICS, i ? pb : pa);
            c->CmdDraw(cb[i], i ? 1 : 3, 1, 0, 0);
            c->CmdEndRenderPass(cb[i]);
        } else {
            c->CmdBindPipeline(cb[i], VK_PIPELINE_BIND_POINT_COMPUTE, i ? pb : pa);
            if (i) c->CmdBindDescriptorSets(cb[i], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
            c->CmdDispatch(cb[i], 1, 1, 1);
        }
        if (c->EndCommandBuffer(cb[i]) != VK_SUCCESS) goto out;
    }

    {
        VkSemaphoreTypeCreateInfo stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sci.pNext = &stci;
        step("vkCreateSemaphore (timeline, case device)");
        if (c->CreateSemaphore(c->dev, &sci, NULL, &sem) != VK_SUCCESS) goto out;
    }
    VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    uint64_t waitA = 1, sigA = 2, waitB = 2, sigB = 3;
    VkTimelineSemaphoreSubmitInfo ta = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ta.waitSemaphoreValueCount = 1; ta.pWaitSemaphoreValues = &waitA;
    ta.signalSemaphoreValueCount = 1; ta.pSignalSemaphoreValues = &sigA;
    VkTimelineSemaphoreSubmitInfo tb = ta;
    tb.pWaitSemaphoreValues = &waitB; tb.pSignalSemaphoreValues = &sigB;
    VkSubmitInfo sa = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    sa.pNext = &ta;
    sa.waitSemaphoreCount = 1; sa.pWaitSemaphores = &sem; sa.pWaitDstStageMask = &all;
    sa.commandBufferCount = 1; sa.pCommandBuffers = &cb[0];
    sa.signalSemaphoreCount = 1; sa.pSignalSemaphores = &sem;
    VkSubmitInfo sb = sa;
    sb.pNext = &tb;
    sb.pCommandBuffers = &cb[1];

    step("vkQueueSubmit A wait(1)+signal(2)");
    r = c->QueueSubmit(c->q, 1, &sa, VK_NULL_HANDLE);
    printf("  submit A: %d\n", r);
    if (r != VK_SUCCESS) { rc = 1; goto out; }

    SubmitJob job = { c, &sb, VK_ERROR_UNKNOWN };
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    step("vkQueueSubmit B wait(2)+signal(3) on a worker thread (5 s bound)");
    HANDLE th = CreateThread(NULL, 0, submit_thread, &job, 0, NULL);
    if (!th) goto out;
    int blocked = WaitForSingleObject(th, 5000) == WAIT_TIMEOUT;
    if (blocked) printf("  submit B: still in vkQueueSubmit at %.0f ms (it waits for submit A, which waits for the host)\n", ms_since(t0));
    else printf("  submit B: %d after %.1f ms\n", job.r, ms_since(t0));

    VkSemaphoreSignalInfo ssi = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    ssi.semaphore = sem;
    ssi.value = 1;
    step("vkSignalSemaphore value 1 (host)");
    r = c->SignalSemaphore(c->dev, &ssi);
    printf("  vkSignalSemaphore(1): %d\n", r);
    if (blocked) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        if (WaitForSingleObject(th, 5000) == WAIT_TIMEOUT) {
            /* The device cannot be destroyed while a thread is inside vkQueueSubmit. */
            char b[96];
            snprintf(b, sizeof b, "%s: TIMEOUT submit B still blocked 5 s after the host signal", label);
            puts(b); write_result(b);
            snprintf(b, sizeof b, "PROBE HANG %s", label);
            probe_line(b);
            ExitProcess(3);
        }
        printf("  submit B: %d, %.1f ms after the host signal (it returned only when submit A could complete)\n", job.r, ms_since(t1));
    }
    CloseHandle(th);
    uint64_t v = 3, counter = 0;
    VkSemaphoreWaitInfo swi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    swi.semaphoreCount = 1; swi.pSemaphores = &sem; swi.pValues = &v;
    step("vkWaitSemaphores value 3 (5 s)");
    VkResult rw = c->WaitSemaphores(c->dev, &swi, 5000000000ULL);
    step("vkGetSemaphoreCounterValue");
    c->GetSemaphoreCounterValue(c->dev, sem, &counter);
    printf("  vkWaitSemaphores(3): %d counter: %llu\n", rw, (unsigned long long)counter);
    if (blocked || rw == VK_TIMEOUT) rc = 3;
    else rc = (r == VK_SUCCESS && job.r == VK_SUCCESS && rw == VK_SUCCESS && counter >= 3) ? 0 : 1;

out:
    if (c->dev) {
        step("vkDeviceWaitIdle (case device)");
        c->DeviceWaitIdle(c->dev);
        if (sem) c->DestroySemaphore(c->dev, sem, NULL);
        if (cpool) c->DestroyCommandPool(c->dev, cpool, NULL);
        if (pa) c->DestroyPipeline(c->dev, pa, NULL);
        if (pb) c->DestroyPipeline(c->dev, pb, NULL);
        if (m0) c->DestroyShaderModule(c->dev, m0, NULL);
        if (m1) c->DestroyShaderModule(c->dev, m1, NULL);
        if (m2) c->DestroyShaderModule(c->dev, m2, NULL);
        if (fb) c->DestroyFramebuffer(c->dev, fb, NULL);
        if (rp) c->DestroyRenderPass(c->dev, rp, NULL);
        if (layout) c->DestroyPipelineLayout(c->dev, layout, NULL);
        if (pool) c->DestroyDescriptorPool(c->dev, pool, NULL);
        if (dsl) c->DestroyDescriptorSetLayout(c->dev, dsl, NULL);
        if (buf) c->DestroyBuffer(c->dev, buf, NULL);
        if (mem) c->FreeMemory(c->dev, mem, NULL);
        step("vkDestroyDevice (case device)");
        c->DestroyDevice(c->dev, NULL);
    }
    stop_watchdog(wd);
    if (rc == 3) puts("  HANG: submit B waited for the host signal - the defect");
    return rc;
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
    printf("vk_wbs_hang v6 - BD-200 wait-before-signal hang test, bitness=%d\n", (int)(sizeof(void *) * 8));
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

    int f4[2];
    for (int k = 0; k < 2; k++) {
        const char *label = k ? "case-4b" : "case-4";
        f4[k] = case4_preamble_replacement(k ? 5 : 4);
        char t[48];
        snprintf(t, sizeof t, "%s: %s", label, f4[k] == -1 ? "SKIP" : f4[k] == 3 ? "TIMEOUT" : f4[k] == 2 ? "ERROR" : f4[k] ? "FAIL" : "PASS");
        puts(t); write_result(t);
        if (f4[k] == -1) f4[k] = 0;
    }

    cleanup();
    int hangs = (f2 == 3) + (f3 == 3) + (f4[0] == 3) + (f4[1] == 3);
    int errors = (f2 == 2) + (f3 == 2) + (f4[0] == 2) + (f4[1] == 2);
    int fails = (f1 != 0) + (f2 == 1) + (f3 == 1) + (f4[0] == 1) + (f4[1] == 1);
    char probe[80];
    if (errors) snprintf(probe, sizeof probe, "PROBE FAIL %d case error(s)", errors);
    else if (hangs) snprintf(probe, sizeof probe, "PROBE HANG %d hang(s) %d fail(s)", hangs, fails);
    else if (fails) snprintf(probe, sizeof probe, "PROBE FAIL %d failure(s)", fails);
    else snprintf(probe, sizeof probe, "PROBE PASS 5/5");
    probe_line(probe);
    return errors ? 2 : hangs ? 3 : fails ? 1 : 0;
}
