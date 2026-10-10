/*
 * vksemcheck: one unnamed and one named Win32 export/import pair of a Vulkan timeline semaphore, across two
 * processes, on the installed Vulkan driver.
 *
 * Why this client exists. Release 0.7.216.100-tester.27 changes how the 64-bit system ICD shares a semaphore
 * through a Windows handle (mesa amdgpu-wddm/b29-system-win32-semaphore 2fbc4913):
 *   - an import keeps a duplicate of the NT handle for itself. The Vulkan specification says that an import
 *     of a Windows handle does not transfer the ownership of the handle, so the application's handle stays
 *     the application's. The earlier build kept the application's handle and closed it with the semaphore;
 *   - an export applies the access, the inheritance and the name of VkExportSemaphoreWin32HandleInfoKHR,
 *     where the earlier build handed them to a callback that its sync type did not have;
 *   - a named import maps the Global\ and Local\ namespaces to their BaseNamedObjects directories;
 *   - the default export access is GENERIC_ALL, and only a real sync object of the kernel driver can say
 *     whether D3DKMTShareObjects accepts that right.
 * The host gate of that branch drove these paths against an inert KMT mock. This client drives them on a
 * real device: the parent exports, a child process imports, and the two walk one timeline in both
 * directions, with host signals, queue signals and queue waits on both sides.
 *
 * The walk. The parent owns the odd values of a timeline and the child the even ones, and each side waits
 * for the other's value before it writes its own, so every value that arrives has crossed the process
 * boundary: 1 parent queue signal, 2 child queue signal, 3 parent host signal, 4 child host signal, 5 parent
 * host signal that releases a child queue wait which signals 6, 7 child host signal that releases a parent
 * queue wait which signals 8. Every queue submission carries one vkCmdFillBuffer, so a signal comes from
 * a real GPU submission and not from an empty batch. A wait that ends with no value ends the walk, so a
 * broken pair costs one wait and not eight.
 *
 * The cells.
 *   unnamed  no VkExportSemaphoreWin32HandleInfoKHR, so the driver's default access. The child gets an
 *            inheritable duplicate of the handle, imports it twice: once into a probe semaphore that it
 *            destroys at once (the handle must still be open afterwards), then into the semaphore it walks
 *            with. It then closes the handle, as the specification lets it, and the walk proves that the
 *            driver still holds its own reference.
 *   named    an explicit access of READ_CONTROL | SYNCHRONIZE | D3DDDI_SYNC_OBJECT_WAIT |
 *            D3DDDI_SYNC_OBJECT_MODIFY_STATE (0x00120003), inheritable, and a Local\ name. That access is
 *            neither the earlier default (D3DDDI_SYNC_OBJECT_ALL_ACCESS, 0x001F0003) nor GENERIC_ALL, so
 *            the granted access of the exported handle says whether the request was applied. The child
 *            imports by name, after a name that nobody exported has been refused.
 *
 * The 32-bit build of this client is the control: the 32-bit ICD of tester.27 is the earlier build, so the
 * checks this change is about must fail there. The control ends where the earlier build ends: the named
 * cell can stop the process at its first semaphore, which is the missing callback.
 *
 * It runs from an SSH session 0 shell and from the interactive session. Local\ maps to \BaseNamedObjects in
 * session 0 and to \Sessions\<n>\BaseNamedObjects elsewhere, which is the reason to run it in both.
 *
 * Exit: 0 every check passed, 1 a check failed, 2 usage, 3 Vulkan or system error before the first cell.
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BC250_VENDOR_ID 0x1002u
#define BC250_DEVICE_ID 0x13FEu
#define HANDLE_TYPE VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
/* READ_CONTROL | SYNCHRONIZE | D3DDDI_SYNC_OBJECT_WAIT | D3DDDI_SYNC_OBJECT_MODIFY_STATE */
#define NAMED_ACCESS 0x00120003u
#define CELL_UNNAMED 1u
#define CELL_NAMED 2u
#define PARENT_MARKER 0xB2500001u
#define CHILD_MARKER 0xB2500002u

#define INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkGetSemaphoreWin32HandleKHR) X(vkImportSemaphoreWin32HandleKHR) X(vkQueueSubmit) \
    X(vkWaitSemaphores) X(vkSignalSemaphore) X(vkGetSemaphoreCounterValue) X(vkDeviceWaitIdle) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdFillBuffer)
#define DECLARE(f) static PFN_##f f;
INSTANCE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
static PFN_vkGetInstanceProcAddr getInstanceProc;
static PFN_vkCreateInstance createInstance;

static int g_checks, g_failures, g_child, g_any_device;
static ULONGLONG g_t0;
static FILE *g_out;
static uint64_t g_wait_ns = 10ull * 1000 * 1000 * 1000;

static void Emit(const char *kind, const char *text)
{
    fprintf(g_out, "%s%-4s %6llu ms %s\n", g_child ? "[child] " : "", kind,
            (unsigned long long)(GetTickCount64() - g_t0), text);
    fflush(g_out);
}

static void Info(const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    Emit("INFO", text);
}

static int Check(int ok, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    g_checks++;
    if (!ok) g_failures++;
    Emit(ok ? "PASS" : "FAIL", text);
    return ok;
}

typedef struct {
    HMODULE library;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkBuffer buffer;
    VkDeviceMemory memory;
    volatile uint32_t *mapped;
    VkCommandPool pool;
    VkCommandBuffer cmd;
} Gpu;

/* The whole device: an instance through the Vulkan loader (the application's path), the BC-250, one queue,
 * the timeline feature, and one small host-visible buffer that every submission fills. */
static int GpuOpen(Gpu *g, uint32_t marker)
{
    memset(g, 0, sizeof *g);
    g->library = LoadLibraryA("vulkan-1.dll");
    if (!g->library) { Check(0, "vulkan-1.dll loads (error %lu)", GetLastError()); return 0; }
    getInstanceProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(g->library, "vkGetInstanceProcAddr");
    if (!getInstanceProc) { Check(0, "vulkan-1.dll exports vkGetInstanceProcAddr"); return 0; }
    createInstance = (PFN_vkCreateInstance)getInstanceProc(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "bc250-vksemcheck";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkResult r = createInstance ? createInstance(&ici, NULL, &g->instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) { Check(0, "vkCreateInstance (%d)", r); return 0; }
#define LOAD_INSTANCE(f) if (!(f = (PFN_##f)getInstanceProc(g->instance, #f))) { Check(0, "instance function " #f); return 0; }
    INSTANCE_FUNCS(LOAD_INSTANCE)

    VkPhysicalDevice list[8];
    uint32_t count = 8;
    r = vkEnumeratePhysicalDevices(g->instance, &count, list);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { Check(0, "vkEnumeratePhysicalDevices (%d)", r); return 0; }
    for (uint32_t i = 0; i < count && !g->physical; i++) {
        VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(list[i], &props);
        if (!g_any_device &&
            (props.properties.vendorID != BC250_VENDOR_ID || props.properties.deviceID != BC250_DEVICE_ID)) continue;
        g->physical = list[i];
        Info("device '%s', driver '%s' '%s', api %u.%u.%u, %zu-bit process", props.properties.deviceName,
             driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(props.properties.apiVersion),
             VK_API_VERSION_MINOR(props.properties.apiVersion), VK_API_VERSION_PATCH(props.properties.apiVersion),
             sizeof(void *) * 8);
        if (!Check(props.properties.apiVersion >= VK_API_VERSION_1_2, "the device offers Vulkan 1.2")) return 0;
    }
    if (!Check(g->physical != VK_NULL_HANDLE, "a %s Vulkan device is present (%u devices)",
               g_any_device ? "usable" : "BC-250", count)) return 0;

    VkExtensionProperties ext[512];
    uint32_t extCount = 512, have = 0;
    vkEnumerateDeviceExtensionProperties(g->physical, NULL, &extCount, ext);
    for (uint32_t i = 0; i < extCount; i++)
        if (!strcmp(ext[i].extensionName, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) have = 1;
    if (!Check(have, "the device offers %s", VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) return 0;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &timeline;
    vkGetPhysicalDeviceFeatures2(g->physical, &features);
    if (!Check(timeline.timelineSemaphore, "the device offers timeline semaphores")) return 0;

    VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
    query.pNext = &type;
    query.handleType = HANDLE_TYPE;
    VkExternalSemaphoreProperties external = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(g->physical, &query, &external);
    const VkExternalSemaphoreFeatureFlags both =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if (!Check((external.externalSemaphoreFeatures & both) == both,
               "a timeline semaphore is exportable and importable as OPAQUE_WIN32 (features 0x%x, export 0x%x)",
               external.externalSemaphoreFeatures, external.exportFromImportedHandleTypes)) return 0;

    VkQueueFamilyProperties families[16];
    uint32_t familyCount = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g->physical, &familyCount, families);
    g->family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount && g->family == UINT32_MAX; i++)
        if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) g->family = i;
    if (!Check(g->family != UINT32_MAX, "a graphics or compute queue family")) return 0;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g->family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char *names[] = { VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
    VkPhysicalDeviceTimelineSemaphoreFeatures enable = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    enable.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &enable;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = names;
    r = vkCreateDevice(g->physical, &dci, NULL, &g->device);
    if (!Check(r == VK_SUCCESS, "vkCreateDevice with %s and timeline semaphores (%d)", names[0], r)) return 0;
#define LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(g->device, #f))) { Check(0, "device function " #f); return 0; }
    DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(g->device, g->family, 0, &g->queue);

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g->device, &bci, NULL, &g->buffer) != VK_SUCCESS) { Check(0, "vkCreateBuffer"); return 0; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->device, g->buffer, &req);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g->physical, &mem);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t typeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && typeIndex == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) typeIndex = i;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = typeIndex;
    void *mapped = NULL;
    if (typeIndex == UINT32_MAX || vkAllocateMemory(g->device, &mai, NULL, &g->memory) != VK_SUCCESS ||
        vkBindBufferMemory(g->device, g->buffer, g->memory, 0) != VK_SUCCESS ||
        vkMapMemory(g->device, g->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        Check(0, "a host-visible coherent buffer for the fill");
        return 0;
    }
    g->mapped = (volatile uint32_t *)mapped;
    g->mapped[0] = 0;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = g->family;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    if (vkCreateCommandPool(g->device, &pci, NULL, &g->pool) != VK_SUCCESS) { Check(0, "vkCreateCommandPool"); return 0; }
    cai.commandPool = g->pool;
    if (vkAllocateCommandBuffers(g->device, &cai, &g->cmd) != VK_SUCCESS ||
        vkBeginCommandBuffer(g->cmd, &cbi) != VK_SUCCESS) { Check(0, "a command buffer"); return 0; }
    vkCmdFillBuffer(g->cmd, g->buffer, 0, 4, marker);
    if (vkEndCommandBuffer(g->cmd) != VK_SUCCESS) { Check(0, "vkEndCommandBuffer"); return 0; }
    return 1;
}

static void GpuClose(Gpu *g)
{
    if (g->device) {
        vkDeviceWaitIdle(g->device);
        if (g->pool) vkDestroyCommandPool(g->device, g->pool, NULL);
        if (g->buffer) vkDestroyBuffer(g->device, g->buffer, NULL);
        if (g->memory) vkFreeMemory(g->device, g->memory, NULL);
        vkDestroyDevice(g->device, NULL);
    }
    if (g->instance) vkDestroyInstance(g->instance, NULL);
    if (g->library) FreeLibrary(g->library);
    memset(g, 0, sizeof *g);
}

/* A timeline semaphore at 0, exportable as OPAQUE_WIN32 when asked, with the export attributes `win32`. */
static VkSemaphore Timeline(Gpu *g, int exportable, const VkExportSemaphoreWin32HandleInfoKHR *win32,
                            const char *what)
{
    VkExportSemaphoreCreateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    exp.pNext = win32;
    exp.handleTypes = HANDLE_TYPE;
    VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.pNext = exportable ? &exp : NULL;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &type;
    VkSemaphore s = VK_NULL_HANDLE;
    VkResult r = vkCreateSemaphore(g->device, &sci, NULL, &s);
    Check(r == VK_SUCCESS, "%s: vkCreateSemaphore (%d)", what, r);
    return r == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

static VkResult Import(Gpu *g, VkSemaphore s, HANDLE handle, const wchar_t *name)
{
    VkImportSemaphoreWin32HandleInfoKHR info = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    info.semaphore = s;
    info.handleType = HANDLE_TYPE;
    info.handle = handle;
    info.name = name;
    return vkImportSemaphoreWin32HandleKHR(g->device, &info);
}

/* One submission of the fill command buffer that waits for `wait` (0: no wait) and signals `signal`. */
static VkResult Submit(Gpu *g, VkSemaphore s, uint64_t wait, uint64_t signal)
{
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo t = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    t.waitSemaphoreValueCount = wait ? 1u : 0u;
    t.pWaitSemaphoreValues = &wait;
    t.signalSemaphoreValueCount = 1;
    t.pSignalSemaphoreValues = &signal;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &t;
    si.waitSemaphoreCount = wait ? 1u : 0u;
    si.pWaitSemaphores = &s;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g->cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s;
    return vkQueueSubmit(g->queue, 1, &si, VK_NULL_HANDLE);
}

static VkResult HostSignal(Gpu *g, VkSemaphore s, uint64_t value)
{
    VkSemaphoreSignalInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    info.semaphore = s;
    info.value = value;
    return vkSignalSemaphore(g->device, &info);
}

static int Expect(Gpu *g, VkSemaphore s, uint64_t value, const char *cell, const char *what)
{
    VkSemaphoreWaitInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    info.semaphoreCount = 1;
    info.pSemaphores = &s;
    info.pValues = &value;
    ULONGLONG t = GetTickCount64();
    VkResult r = vkWaitSemaphores(g->device, &info, g_wait_ns);
    uint64_t now = 0;
    vkGetSemaphoreCounterValue(g->device, s, &now);
    return Check(r == VK_SUCCESS, "%s: %s (wait for %llu: %d after %llu ms, counter %llu)", cell, what,
                 (unsigned long long)value, r, (unsigned long long)(GetTickCount64() - t), (unsigned long long)now);
}

static int Did(VkResult r, const char *cell, const char *what)
{
    return Check(r == VK_SUCCESS, "%s: %s (%d)", cell, what, r);
}

/* The walk of one shared timeline, as the header describes it. */
static void Walk(Gpu *g, VkSemaphore s, const char *cell, int parent)
{
    int ok;
    if (parent) {
        ok = Did(Submit(g, s, 0, 1), cell, "parent queue submission signals 1") &&
             Expect(g, s, 2, cell, "parent sees the child's queue signal 2") &&
             Did(HostSignal(g, s, 3), cell, "parent host signal 3") &&
             Expect(g, s, 4, cell, "parent sees the child's host signal 4") &&
             Did(HostSignal(g, s, 5), cell, "parent host signal 5, for the child's queue wait") &&
             Expect(g, s, 6, cell, "the child's queue waited for 5 and signalled 6") &&
             Did(Submit(g, s, 7, 8), cell, "parent queue submission waits for 7 and signals 8") &&
             Expect(g, s, 8, cell, "the parent's queue waited for the child's host signal 7 and signalled 8");
        if (ok) Check(g->mapped[0] == PARENT_MARKER, "%s: the parent's fill reached its buffer (0x%08x)", cell,
                      (unsigned)g->mapped[0]);
    } else {
        ok = Expect(g, s, 1, cell, "child sees the parent's queue signal 1") &&
             Did(Submit(g, s, 0, 2), cell, "child queue submission signals 2") &&
             Expect(g, s, 3, cell, "child sees the parent's host signal 3") &&
             Did(HostSignal(g, s, 4), cell, "child host signal 4") &&
             Did(Submit(g, s, 5, 6), cell, "child queue submission waits for 5 and signals 6") &&
             Expect(g, s, 6, cell, "the child's own queue wait for 5 ended and signalled 6") &&
             Did(HostSignal(g, s, 7), cell, "child host signal 7, for the parent's queue wait") &&
             Expect(g, s, 8, cell, "child sees the parent's queue signal 8");
        if (ok) Check(g->mapped[0] == CHILD_MARKER, "%s: the child's fill reached its buffer (0x%08x)", cell,
                      (unsigned)g->mapped[0]);
    }
    if (!ok) { Info("%s: the walk ends at the first step that failed", cell); return; }
    uint64_t value = 0;
    VkResult r = vkGetSemaphoreCounterValue(g->device, s, &value);
    Check(r == VK_SUCCESS && value == 8, "%s: the counter reads 8 (%llu)", cell, (unsigned long long)value);
}

typedef LONG(NTAPI *NtQueryObjectFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { ULONG Attributes; ACCESS_MASK GrantedAccess; ULONG HandleCount; ULONG PointerCount; ULONG Reserved[10]; } BasicInformation;
typedef struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } CountedString;

/* What the kernel says about one handle: its object type, the access it grants and its inherit flag. */
static void Describe(HANDLE h, const char *what, ACCESS_MASK *access, DWORD *flags)
{
    NtQueryObjectFn query = (NtQueryObjectFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    BasicInformation basic;
    union { CountedString name; unsigned char bytes[1024]; } type;
    memset(&basic, 0, sizeof basic);
    memset(&type, 0, sizeof type);
    ULONG got = 0;
    LONG a = query ? query(h, 0 /* ObjectBasicInformation */, &basic, sizeof basic, &got) : -1;
    LONG b = query ? query(h, 2 /* ObjectTypeInformation */, &type, sizeof type, &got) : -1;
    *flags = 0;
    GetHandleInformation(h, flags);
    *access = a >= 0 ? basic.GrantedAccess : 0;
    int chars = b >= 0 && type.name.Buffer ? type.name.Length / 2 : 1;
    Info("%s: object type %.*ls, granted access 0x%08lx, inherit %d", what, chars,
         b >= 0 && type.name.Buffer ? type.name.Buffer : L"?", (unsigned long)*access,
         (*flags & HANDLE_FLAG_INHERIT) ? 1 : 0);
}

static HANDLE Export(Gpu *g, VkSemaphore s, const char *cell)
{
    VkSemaphoreGetWin32HandleInfoKHR info = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    info.semaphore = s;
    info.handleType = HANDLE_TYPE;
    HANDLE h = NULL;
    VkResult r = vkGetSemaphoreWin32HandleKHR(g->device, &info, &h);
    Check(r == VK_SUCCESS && h != NULL, "%s: vkGetSemaphoreWin32HandleKHR gives a handle (%d)", cell, r);
    return r == VK_SUCCESS ? h : NULL;
}

static int Parent(unsigned cells, const wchar_t *childLog)
{
    Gpu g;
    if (!GpuOpen(&g, PARENT_MARKER)) { GpuClose(&g); return 3; }
    VkSemaphore s1 = VK_NULL_HANDLE, s2 = VK_NULL_HANDLE;
    HANDLE h1 = NULL, h1child = NULL, h2 = NULL;
    ACCESS_MASK access = 0;
    DWORD flags = 0;
    wchar_t name[128];
    swprintf(name, 128, L"Local\\bc250-vksemcheck-%lu-%llu", GetCurrentProcessId(), (unsigned long long)g_t0);

    if (cells & CELL_UNNAMED) {
        s1 = Timeline(&g, 1, NULL, "unnamed");
        h1 = s1 ? Export(&g, s1, "unnamed (the default access, no export attributes)") : NULL;
        if (h1) {
            Describe(h1, "unnamed export", &access, &flags);
            Check(!(flags & HANDLE_FLAG_INHERIT), "unnamed: the default export is not inheritable");
            Check(DuplicateHandle(GetCurrentProcess(), h1, GetCurrentProcess(), &h1child, 0, TRUE,
                                  DUPLICATE_SAME_ACCESS), "unnamed: an inheritable copy of the handle for the child");
        }
    }
    if (cells & CELL_NAMED) {
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        VkExportSemaphoreWin32HandleInfoKHR attributes = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        attributes.pAttributes = &sa;
        attributes.dwAccess = NAMED_ACCESS;
        attributes.name = name;
        Info("named: export as %ls with access 0x%08x, inheritable", name, NAMED_ACCESS);
        s2 = Timeline(&g, 1, &attributes, "named");
        h2 = s2 ? Export(&g, s2, "named") : NULL;
        if (h2) {
            Describe(h2, "named export", &access, &flags);
            Check(access == NAMED_ACCESS, "named: the export grants the access the application asked for "
                  "(0x%08lx, asked 0x%08x)", (unsigned long)access, NAMED_ACCESS);
            Check(flags & HANDLE_FLAG_INHERIT, "named: the export applies bInheritHandle");
        }
    }

    wchar_t self[MAX_PATH], line[2048];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(line, 2048, L"\"%ls\" --child%ls --cells %u --handle 0x%llx --name %ls --t0 %llu --wait-ms %llu --log \"%ls\"",
             self, g_any_device ? L" --any-device" : L"", cells, (unsigned long long)(uintptr_t)h1child, name, (unsigned long long)g_t0,
             (unsigned long long)(g_wait_ns / 1000000), childLog);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL started = CreateProcessW(self, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!Check(started, "the importing child process starts (error %lu)", started ? 0ul : GetLastError())) {
        GpuClose(&g);
        return 1;
    }
    Info("child pid %lu", pi.dwProcessId);
    if (h1child) CloseHandle(h1child);  /* the child holds its own inherited copy from here on */

    if (s1 && h1) Walk(&g, s1, "unnamed", 1);
    if (s2 && h2) Walk(&g, s2, "named", 1);

    DWORD waited = WaitForSingleObject(pi.hProcess, (DWORD)(g_wait_ns / 1000000) * 3 + 20000);
    DWORD code = 259;
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 4);
        WaitForSingleObject(pi.hProcess, 5000);
        Check(0, "the child ends inside its bound (killed)");
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    FILE *log = _wfopen(childLog, L"r");
    if (log) {
        char text[2048];
        while (fgets(text, sizeof text, log)) fputs(text, g_out);
        fclose(log);
    } else {
        Info("no child log at %ls", childLog);
    }
    fflush(g_out);
    Check(code == 0, "the child exits 0 (0x%08lx)", (unsigned long)code);

    if (h1) CloseHandle(h1);
    if (h2) CloseHandle(h2);
    if (s1) vkDestroySemaphore(g.device, s1, NULL);
    if (s2) vkDestroySemaphore(g.device, s2, NULL);
    GpuClose(&g);
    return g_failures ? 1 : 0;
}

static int Child(unsigned cells, HANDLE inherited, const wchar_t *name)
{
    Gpu g;
    if (!GpuOpen(&g, CHILD_MARKER)) { GpuClose(&g); return 3; }
    DWORD flags = 0;
    VkSemaphore s1 = VK_NULL_HANDLE, s2 = VK_NULL_HANDLE;

    if (cells & CELL_UNNAMED) {
        const char *cell = "unnamed";
        if (Check(inherited && GetHandleInformation(inherited, &flags), "%s: the inherited handle is open in the child", cell)) {
            /* Vulkan: importing a payload from a Windows handle does not transfer the ownership of the handle
             * to the implementation. A probe semaphore imports it and is destroyed at once; the handle must
             * still be the application's afterwards. */
            VkSemaphore probe = Timeline(&g, 0, NULL, "unnamed probe");
            if (probe) {
                Did(Import(&g, probe, inherited, NULL), cell, "the probe semaphore imports the handle");
                vkDestroySemaphore(g.device, probe, NULL);
                Check(GetHandleInformation(inherited, &flags), "%s: after the importing semaphore is destroyed "
                      "the handle is still open, so the import took no ownership of it", cell);
            }
            s1 = Timeline(&g, 0, NULL, "unnamed import");
            if (s1 && Did(Import(&g, s1, inherited, NULL), cell, "the semaphore of the walk imports the handle")) {
                Check(CloseHandle(inherited), "%s: the application closes its own handle after the import", cell);
                Walk(&g, s1, cell, 0);
            }
        }
    }
    if (cells & CELL_NAMED) {
        const char *cell = "named";
        wchar_t nobody[128];
        swprintf(nobody, 128, L"Local\\bc250-vksemcheck-nobody-%lu", GetCurrentProcessId());
        VkSemaphore probe = Timeline(&g, 0, NULL, "named probe");
        if (probe) {
            /* The specification names VK_ERROR_INVALID_EXTERNAL_HANDLE for this; the check asks only for a
             * refusal, because another vendor's driver, the positive control of this client, returns
             * VK_ERROR_INITIALIZATION_FAILED. The code is in the line. */
            VkResult r = Import(&g, probe, NULL, nobody);
            Check(r != VK_SUCCESS, "%s: an import by a name that nobody exported is refused (%d%s)", cell, r,
                  r == VK_ERROR_INVALID_EXTERNAL_HANDLE ? ", VK_ERROR_INVALID_EXTERNAL_HANDLE" : "");
            vkDestroySemaphore(g.device, probe, NULL);
        }
        s2 = Timeline(&g, 0, NULL, "named import");
        if (s2 && Check(Import(&g, s2, NULL, name) == VK_SUCCESS, "%s: the import by the name %ls opens the "
                        "parent's semaphore", cell, name))
            Walk(&g, s2, cell, 0);
    }
    if (s1) vkDestroySemaphore(g.device, s1, NULL);
    if (s2) vkDestroySemaphore(g.device, s2, NULL);
    GpuClose(&g);
    return g_failures ? 1 : 0;
}

static int Usage(void)
{
    fprintf(stderr,
            "usage: vksemcheck [--cells unnamed|named|both] [--wait-ms N] [--child-log PATH] [--any-device]\n"
            "  one unnamed and one named Win32 export/import pair of a Vulkan timeline semaphore, across two\n"
            "  processes, on the installed Vulkan driver. Exit 0 pass, 1 a check failed, 2 usage, 3 setup.\n"
            "  --any-device takes the first device of any vendor: the self-test of a build on a PC that has\n"
            "  no BC-250.\n");
    return 2;
}

int wmain(int argc, wchar_t **argv)
{
    g_t0 = GetTickCount64();
    g_out = stdout;
    /* A fault ends the process with its exception code and no dialog: the 32-bit control is expected to
     * fault, and a dialog in a session nobody looks at would hold the process until its bound. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    unsigned cells = CELL_UNNAMED | CELL_NAMED;
    HANDLE inherited = NULL;
    const wchar_t *name = NULL, *log = NULL;
    wchar_t defaultLog[MAX_PATH];
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        const wchar_t *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!wcscmp(a, L"--help")) { Usage(); return 0; }
        else if (!wcscmp(a, L"--child")) g_child = 1;
        else if (!wcscmp(a, L"--any-device")) g_any_device = 1;
        else if (!wcscmp(a, L"--cells") && v) {
            i++;
            if (!wcscmp(v, L"unnamed")) cells = CELL_UNNAMED;
            else if (!wcscmp(v, L"named")) cells = CELL_NAMED;
            else if (!wcscmp(v, L"both")) cells = CELL_UNNAMED | CELL_NAMED;
            else cells = (unsigned)wcstoul(v, NULL, 10);
            if (!cells || cells > 3) return Usage();
        }
        else if (!wcscmp(a, L"--handle") && v) { i++; inherited = (HANDLE)(uintptr_t)_wcstoui64(v, NULL, 16); }
        else if (!wcscmp(a, L"--name") && v) { i++; name = v; }
        else if (!wcscmp(a, L"--t0") && v) { i++; g_t0 = _wcstoui64(v, NULL, 10); }
        else if (!wcscmp(a, L"--wait-ms") && v) { i++; g_wait_ns = _wcstoui64(v, NULL, 10) * 1000000ull; }
        else if ((!wcscmp(a, L"--log") || !wcscmp(a, L"--child-log")) && v) { i++; log = v; }
        else return Usage();
    }
    if (g_wait_ns < 100ull * 1000000 || g_wait_ns > 30000ull * 1000000) return Usage();
    if (g_child) {
        if (!log || ((cells & CELL_NAMED) && !name)) return Usage();
        g_out = _wfopen(log, L"w");
        if (!g_out) return 3;
        int code = Child(cells, inherited, name);
        Info("vksemcheck child: %d checks, %d failed", g_checks, g_failures);
        fclose(g_out);
        return code;
    }
    if (!log) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        swprintf(defaultLog, MAX_PATH, L"%lsvksemcheck-child-%lu.log", temp, GetCurrentProcessId());
        log = defaultLog;
    }
    int code = Parent(cells, log);
    Info("vksemcheck parent: %d checks, %d failed", g_checks, g_failures);
    printf("vksemcheck %s %u-bit: %d checks, %d failed\n", code == 0 ? "PASS" : "FAIL",
           (unsigned)(sizeof(void *) * 8), g_checks, g_failures);
    return code;
}
