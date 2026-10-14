/*
 * vkpresent: a Vulkan client that presents frames through the registered ICD and proves each one.
 *
 * Every other Vulkan client of this workspace is a compute client (vkfillcheck, vkmembw, vksemcheck): it
 * never creates a surface, so none of them touches the window-system part of the ICD. The present route of
 * our Vulkan driver (radv_wddm2_wsi_route.h in the Mesa fork: "gdi", "dxgi", "dxgi-composition") therefore
 * had no client of ours at all, and its lab arms borrowed vkcube and Quake II RTX. Neither of those says in
 * one machine-readable line which ICD file the loader picked, which route the ICD then took, and whether the
 * pixels of each presented frame are the ones this program asked for. This client does.
 *
 * Two jobs:
 *   1. the 32-bit arm of the Vulkan ICD slot. The release registers a 32-bit ICD for the WOW64 half of the
 *      machine (HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers and VulkanDriverNameWow on the display
 *      class key). An x86 build of this program is the only client we have that enters that half, and the
 *      loader's own selection is what the arm tests, so this client never loads an ICD by path.
 *   2. a minimal instrument for the DXGI present route (BD-105), where every borrowed client freezes before
 *      its first present. One process, one window, one clear, one present, and a line per frame.
 *
 * What each frame does: acquire an image, clear it to a colour that depends on the frame number inside a
 * render pass (loadOp CLEAR, so no pipeline and no shader is involved), copy the cleared image back to a
 * host buffer when the surface allows a transfer source, present, and wait for the frame's fence. The
 * readback is compared with the colour that was asked for, so a driver that drops the clear, presents a
 * stale image or hands out an image of another frame fails the case instead of passing it quietly.
 *
 * The route is not set here. The switch belongs to the machine: the REG_SZ value WsiRoute under
 * HKLM\SOFTWARE\amdgpu-wddm\Vulkan ("gdi", "dxgi", "dxgi-composition"), read from the 64-bit view, which is
 * where the ICD reads it from as well, also in a 32-bit process. This program reads that value (read-only,
 * never written) and reports it as the route that was asked for, so the arm that sets it and the client that
 * runs under it cannot disagree about what was asked. The environment switch AMDGPU_WDDM_VK_WSI is not used:
 * the release ICD has no environment overrides (BD-167), and an arm built on the registry runs against both
 * flavours unchanged.
 *
 * The route the ICD actually took is measured in this process, not assumed: the DXGI routes bind System32
 * dxgi.dll and d3d12.dll by full path, and the composition route adds DComp.dll, while the GDI route loads
 * none of them (docs/design/vulkan-wsi-dxgi.md, "Application-local runtime modules do not change the
 * route"). The module list of this process before the instance and after the first present therefore says
 * which route ran. An ICD that was asked for dxgi and fell back to GDI is a failure of the arm, not a
 * detail: that fallback is exactly what the route work is about.
 *
 * This program's own folder must hold no dxgi.dll, d3d12.dll or d3d12core.dll. The ICD's module gate refuses
 * the DXGI route to a process that has an application-local copy of one of them, by design, and the check
 * below says so instead of leaving the arm to wonder.
 *
 * Exit: 0 every frame passed, 1 a frame failed or the route was not the one asked for, 2 usage, 3 Vulkan or
 * system error, 4 the deadline passed.
 */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
/* windows.h defines CreateSemaphore as CreateSemaphoreW, which would rename the member of the table below
 * and the string the entry point is asked for. The Vulkan name is the one this file means. */
#undef CreateSemaphore
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BC250_VENDOR_ID 0x1002u
#define BC250_DEVICE_ID 0x13FEu
#define MAX_IMAGES 8u
#define EXIT_PASS 0
#define EXIT_FAIL 1
#define EXIT_USAGE 2
#define EXIT_ERROR 3
#define EXIT_DEADLINE 4

/* The route names of radv_wddm2_wsi_route.h, in its order, and the default it compiles with. */
static const char *const ROUTE_NAMES[] = {"gdi", "dxgi", "dxgi-composition", "unknown"};
#define ROUTE_GDI 0
#define ROUTE_DXGI 1
#define ROUTE_DXGI_COMPOSITION 2
#define ROUTE_UNKNOWN 3                 /* a foreign ICD: the modules say nothing about our WSI route */
#define ROUTE_DEFAULT ROUTE_GDI
#define ROUTE_COUNT 3                   /* the routes a caller may ask for; "unknown" is a result only */

struct options {
   unsigned frames;
   unsigned width;
   unsigned height;
   int device_index;        /* -1: the first device that matches the board, or any with any_device */
   int any_device;
   int list_devices;
   int no_present;          /* enumerate and report, create no window */
   int negative_control;    /* every frame must fail: the comparison itself is under test */
   int borderless;
   unsigned deadline_s;
   const char *json;
   int expect_route;        /* -1: whatever the registry says */
};

struct vk_api {
   HMODULE loader;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
   PFN_vkCreateInstance CreateInstance;
   PFN_vkDestroyInstance DestroyInstance;
   PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
   PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
   PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
   PFN_vkCreateDevice CreateDevice;
   PFN_vkDestroyDevice DestroyDevice;
   PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
   PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
   PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
   PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
   PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
   /* device level */
   PFN_vkGetDeviceQueue GetDeviceQueue;
   PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
   PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
   PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
   PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
   PFN_vkQueuePresentKHR QueuePresentKHR;
   PFN_vkCreateImageView CreateImageView;
   PFN_vkDestroyImageView DestroyImageView;
   PFN_vkCreateRenderPass CreateRenderPass;
   PFN_vkDestroyRenderPass DestroyRenderPass;
   PFN_vkCreateFramebuffer CreateFramebuffer;
   PFN_vkDestroyFramebuffer DestroyFramebuffer;
   PFN_vkCreateCommandPool CreateCommandPool;
   PFN_vkDestroyCommandPool DestroyCommandPool;
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
   PFN_vkBeginCommandBuffer BeginCommandBuffer;
   PFN_vkEndCommandBuffer EndCommandBuffer;
   PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
   PFN_vkCmdEndRenderPass CmdEndRenderPass;
   PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
   PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
   PFN_vkCreateBuffer CreateBuffer;
   PFN_vkDestroyBuffer DestroyBuffer;
   PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkBindBufferMemory BindBufferMemory;
   PFN_vkMapMemory MapMemory;
   PFN_vkUnmapMemory UnmapMemory;
   PFN_vkCreateSemaphore CreateSemaphore;
   PFN_vkDestroySemaphore DestroySemaphore;
   PFN_vkCreateFence CreateFence;
   PFN_vkDestroyFence DestroyFence;
   PFN_vkWaitForFences WaitForFences;
   PFN_vkResetFences ResetFences;
   PFN_vkQueueSubmit QueueSubmit;
   PFN_vkDeviceWaitIdle DeviceWaitIdle;
};

static int g_quit;
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_start;

static double now_s(void)
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)(t.QuadPart - g_start.QuadPart) / (double)g_freq.QuadPart;
}

static void fail_line(const char *what, VkResult r)
{
   printf("vkpresent ERROR %s vk=%d\n", what, (int)r);
   fflush(stdout);
}

/* ---- the route the machine asks for ------------------------------------------------------------------- */

/* The ICD's own order: the environment first, then this value, then its compiled default. This client reads
 * the registry only, and from the 64-bit view, because a 32-bit image must see the same value the ICD sees
 * (the system line reads WsiRoute through KEY_WOW64_64KEY for exactly that reason). */
static int route_from_registry(int *found)
{
   HKEY key;
   char text[64];
   DWORD size = sizeof(text);
   DWORD type = 0;
   unsigned i;

   *found = 0;
   if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\amdgpu-wddm\\Vulkan", 0,
                     KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
      return ROUTE_DEFAULT;
   memset(text, 0, sizeof(text));
   if (RegQueryValueExA(key, "WsiRoute", NULL, &type, (LPBYTE)text, &size) != ERROR_SUCCESS ||
       type != REG_SZ) {
      RegCloseKey(key);
      return ROUTE_DEFAULT;
   }
   RegCloseKey(key);
   text[sizeof(text) - 1] = '\0';
   for (i = 0; i < ROUTE_COUNT; i++) {   /* a stored "unknown" is not a route: the default stands */
      if (_stricmp(text, ROUTE_NAMES[i]) == 0) {
         *found = 1;
         return (int)i;
      }
   }
   /* A value the ICD does not know is its own finding: report it as not found, so the arm sees the default. */
   printf("vkpresent note: WsiRoute is '%s', which is not a route name; the ICD takes its default\n", text);
   return ROUTE_DEFAULT;
}

/* ---- what this process has loaded ---------------------------------------------------------------------- */

struct modules {
   int dxgi;
   int d3d12;
   int dcomp;
   int d3d11;
   char icd_path[MAX_PATH];
   char app_local[256];
};

static int module_dir_is_system32(const char *path, const char *system_dir)
{
   size_t n = strlen(system_dir);
   return n > 0 && _strnicmp(path, system_dir, n) == 0;
}

/* The ICD the loader picked is the module in this process whose file name is one of the names our ICD ships
 * under. Nothing else in the process carries those names. */
static int is_icd_name(const char *name)
{
   return _stricmp(name, "vulkan_radeon.dll") == 0 || _stricmp(name, "amdgpu_wddm_radv.dll") == 0 ||
          _stricmp(name, "amdgpu_wddm_radv_d3d11.dll") == 0 ||
          _stricmp(name, "amdgpu_wddm_radv_d3d1132.dll") == 0;
}

static void read_modules(struct modules *m)
{
   HMODULE mods[512];
   DWORD needed = 0;
   char system_dir[MAX_PATH];
   char exe_dir[MAX_PATH];
   char *cut;
   DWORD i, count;

   memset(m, 0, sizeof(*m));
   if (!GetSystemDirectoryA(system_dir, (UINT)sizeof(system_dir)))
      system_dir[0] = '\0';
   if (!GetModuleFileNameA(NULL, exe_dir, (DWORD)sizeof(exe_dir)))
      exe_dir[0] = '\0';
   cut = strrchr(exe_dir, '\\');
   if (cut)
      *cut = '\0';

   if (!EnumProcessModulesEx(GetCurrentProcess(), mods, (DWORD)sizeof(mods), &needed, LIST_MODULES_ALL))
      return;
   count = needed / (DWORD)sizeof(HMODULE);
   if (count > (DWORD)(sizeof(mods) / sizeof(mods[0])))
      count = (DWORD)(sizeof(mods) / sizeof(mods[0]));
   for (i = 0; i < count; i++) {
      char path[MAX_PATH];
      const char *name;
      if (!GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, (DWORD)sizeof(path)))
         continue;
      name = strrchr(path, '\\');
      name = name ? name + 1 : path;
      if (_stricmp(name, "dxgi.dll") == 0)
         m->dxgi = module_dir_is_system32(path, system_dir) ? 1 : 2;
      else if (_stricmp(name, "d3d12.dll") == 0 || _stricmp(name, "d3d12core.dll") == 0)
         m->d3d12 = module_dir_is_system32(path, system_dir) ? 1 : 2;
      else if (_stricmp(name, "dcomp.dll") == 0)
         m->dcomp = module_dir_is_system32(path, system_dir) ? 1 : 2;
      else if (_stricmp(name, "d3d11.dll") == 0)
         m->d3d11 = module_dir_is_system32(path, system_dir) ? 1 : 2;
      else if (is_icd_name(name) && m->icd_path[0] == '\0')
         strncpy(m->icd_path, path, sizeof(m->icd_path) - 1);
   }

   /* An application-local runtime module next to this program would stop the DXGI route by design. Say so
    * here rather than let the arm read a GDI fallback as a driver defect. */
   if (exe_dir[0]) {
      static const char *const names[3] = {"dxgi.dll", "d3d12.dll", "d3d12core.dll"};
      size_t used = 0;
      unsigned k;
      for (k = 0; k < 3; k++) {
         char probe[MAX_PATH];
         int n = _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s\\%s", exe_dir, names[k]);
         if (n < 0)
            continue;
         if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            int w = _snprintf_s(m->app_local + used, sizeof(m->app_local) - used, _TRUNCATE,
                                used ? ",%s" : "%s", names[k]);
            if (w > 0)
               used += (size_t)w;
         }
      }
   }
}

/* Which route the modules of this process say ran. The DXGI routes take System32 dxgi.dll and d3d12.dll by
 * full path; the composition route adds DComp.dll. The GDI route takes none of them. */
static int route_from_modules(const struct modules *m)
{
   if (m->dcomp && m->dxgi)
      return ROUTE_DXGI_COMPOSITION;
   if (m->dxgi && m->d3d12)
      return ROUTE_DXGI;
   return ROUTE_GDI;
}

/* ---- the window ---------------------------------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
   switch (msg) {
   case WM_CLOSE:
   case WM_DESTROY:
      g_quit = 1;
      return 0;
   case WM_ERASEBKGND:
      return 1;        /* the swapchain owns every pixel; a background erase would flash */
   default:
      break;
   }
   return DefWindowProcA(h, msg, wp, lp);
}

static HWND make_window(const struct options *o)
{
   WNDCLASSEXA cls;
   HWND h;
   RECT r;
   DWORD style = o->borderless ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
   int x, y;

   memset(&cls, 0, sizeof(cls));
   cls.cbSize = sizeof(cls);
   cls.style = CS_OWNDC;
   cls.lpfnWndProc = wnd_proc;
   cls.hInstance = GetModuleHandleA(NULL);
   cls.hCursor = LoadCursorA(NULL, IDC_ARROW);
   cls.lpszClassName = "bc250_vkpresent";
   if (!RegisterClassExA(&cls))
      return NULL;

   r.left = 0;
   r.top = 0;
   r.right = (LONG)o->width;
   r.bottom = (LONG)o->height;
   if (!o->borderless)
      AdjustWindowRect(&r, style, FALSE);
   x = (GetSystemMetrics(SM_CXSCREEN) - (int)(r.right - r.left)) / 2;
   y = (GetSystemMetrics(SM_CYSCREEN) - (int)(r.bottom - r.top)) / 2;
   if (x < 0)
      x = 0;
   if (y < 0)
      y = 0;
   h = CreateWindowExA(0, cls.lpszClassName, "vkpresent", style, x, y,
                       (int)(r.right - r.left), (int)(r.bottom - r.top), NULL, NULL, cls.hInstance, NULL);
   if (h) {
      ShowWindow(h, SW_SHOW);
      UpdateWindow(h);
   }
   return h;
}

static void pump(void)
{
   MSG msg;
   while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
   }
}

/* ---- the colour of a frame, and its bytes in the swapchain's format ------------------------------------ */

struct rgba8 {
   uint8_t r, g, b, a;
};

static struct rgba8 frame_colour(unsigned frame, int negative)
{
   /* Three channels that move at different rates, so no two frames of a run share a colour and a stale
    * image shows up as the wrong frame rather than as a colour that is merely close. */
   struct rgba8 c;
   c.r = (uint8_t)(16u + (frame * 7u) % 224u);
   c.g = (uint8_t)(32u + (frame * 11u) % 192u);
   c.b = (uint8_t)(64u + (frame * 13u) % 160u);
   c.a = 255u;
   if (negative) {
      /* The negative control asks for one colour and expects another: every frame must fail. */
      c.r = (uint8_t)(255u - c.r);
   }
   return c;
}

static int format_is_bgra(VkFormat f)
{
   return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

static int format_is_rgba(VkFormat f)
{
   return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB;
}

static int format_is_srgb(VkFormat f)
{
   return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
}

/* An sRGB attachment stores the colour the clear value names after the linear-to-sRGB transfer, so the bytes
 * that come back are not the bytes that were asked for. The clear value is therefore built from the linear
 * value of each channel, and the comparison stays in the 8-bit space the surface uses. */
static float srgb_to_linear(float c)
{
   if (c <= 0.04045f)
      return c / 12.92f;
   return (float)pow(((double)c + 0.055) / 1.055, 2.4);
}

/* ---- usage --------------------------------------------------------------------------------------------- */

static void usage(void)
{
   printf(
      "vkpresent: present frames through the registered Vulkan ICD and check every one.\n"
      "\n"
      "  --frames N         frames to present (default 60, max 10000)\n"
      "  --width N          window width (default 640)\n"
      "  --height N         window height (default 360)\n"
      "  --borderless       a WS_POPUP window, for the independent-flip shape\n"
      "  --device-index N   use this physical device instead of the BC-250 board\n"
      "  --any-device       accept any Vulkan device, not only 0x1002:0x13FE\n"
      "  --list-devices     print the devices and exit\n"
      "  --no-present       enumerate and report, create no window (safe on any PC)\n"
      "  --expect-route R   require this route: gdi, dxgi or dxgi-composition\n"
      "                     (the default is whatever HKLM\\SOFTWARE\\amdgpu-wddm\\Vulkan WsiRoute says)\n"
      "  --negative-control ask for one colour and expect another: every frame must fail\n"
      "  --deadline S       give up after S seconds (default 60)\n"
      "  --json PATH        write the result as JSON as well\n"
      "\n"
      "The route itself is not set here: it is the REG_SZ value WsiRoute under\n"
      "HKLM\\SOFTWARE\\amdgpu-wddm\\Vulkan, which this program only reads.\n"
      "\n"
      "Exit: 0 pass, 1 a frame or the route failed, 2 usage, 3 Vulkan or system error, 4 deadline.\n");
}

static int parse_route(const char *s)
{
   unsigned i;
   for (i = 0; i < ROUTE_COUNT; i++)     /* --expect-route unknown is a usage error, not a route */
      if (_stricmp(s, ROUTE_NAMES[i]) == 0)
         return (int)i;
   return -1;
}

static int parse_options(int argc, char **argv, struct options *o)
{
   int i;

   memset(o, 0, sizeof(*o));
   o->frames = 60;
   o->width = 640;
   o->height = 360;
   o->device_index = -1;
   o->deadline_s = 60;
   o->expect_route = -1;

   for (i = 1; i < argc; i++) {
      const char *a = argv[i];
      if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
         usage();
         exit(EXIT_PASS);
      } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
         long v = strtol(argv[++i], NULL, 10);
         if (v < 1 || v > 10000)
            return 0;
         o->frames = (unsigned)v;
      } else if (strcmp(a, "--width") == 0 && i + 1 < argc) {
         long v = strtol(argv[++i], NULL, 10);
         if (v < 16 || v > 7680)
            return 0;
         o->width = (unsigned)v;
      } else if (strcmp(a, "--height") == 0 && i + 1 < argc) {
         long v = strtol(argv[++i], NULL, 10);
         if (v < 16 || v > 4320)
            return 0;
         o->height = (unsigned)v;
      } else if (strcmp(a, "--device-index") == 0 && i + 1 < argc) {
         long v = strtol(argv[++i], NULL, 10);
         if (v < 0 || v > 31)
            return 0;
         o->device_index = (int)v;
      } else if (strcmp(a, "--deadline") == 0 && i + 1 < argc) {
         long v = strtol(argv[++i], NULL, 10);
         if (v < 5 || v > 170)
            return 0;
         o->deadline_s = (unsigned)v;
      } else if (strcmp(a, "--expect-route") == 0 && i + 1 < argc) {
         o->expect_route = parse_route(argv[++i]);
         if (o->expect_route < 0)
            return 0;
      } else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
         o->json = argv[++i];
      } else if (strcmp(a, "--any-device") == 0) {
         o->any_device = 1;
      } else if (strcmp(a, "--list-devices") == 0) {
         o->list_devices = 1;
      } else if (strcmp(a, "--no-present") == 0) {
         o->no_present = 1;
      } else if (strcmp(a, "--negative-control") == 0) {
         o->negative_control = 1;
      } else if (strcmp(a, "--borderless") == 0) {
         o->borderless = 1;
      } else {
         printf("vkpresent: unknown or incomplete option '%s'\n", a);
         return 0;
      }
   }
   return 1;
}

/* ---- the loader ---------------------------------------------------------------------------------------- */

#define GET_I(api, inst, name)                                                      \
   do {                                                                             \
      (api)->name = (PFN_vk##name)(api)->GetInstanceProcAddr((inst), "vk" #name);    \
      if (!(api)->name) {                                                            \
         printf("vkpresent ERROR instance entry point vk%s is missing\n", #name);     \
         return 0;                                                                   \
      }                                                                              \
   } while (0)

#define GET_D(api, dev, name)                                                       \
   do {                                                                             \
      (api)->name = (PFN_vk##name)(api)->GetDeviceProcAddr((dev), "vk" #name);       \
      if (!(api)->name) {                                                            \
         printf("vkpresent ERROR device entry point vk%s is missing\n", #name);       \
         return 0;                                                                   \
      }                                                                              \
   } while (0)

static int load_loader(struct vk_api *api)
{
   memset(api, 0, sizeof(*api));
   /* By name, so the Vulkan loader of this architecture is the one that answers, and the ICD comes from the
    * machine's registration. An x86 image gets SysWOW64\vulkan-1.dll and the WOW6432Node registration. */
   api->loader = LoadLibraryA("vulkan-1.dll");
   if (!api->loader) {
      printf("vkpresent ERROR vulkan-1.dll did not load (GetLastError %lu). A 32-bit program needs the "
             "32-bit Vulkan loader of this computer.\n", (unsigned long)GetLastError());
      return 0;
   }
   api->GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(api->loader, "vkGetInstanceProcAddr");
   if (!api->GetInstanceProcAddr) {
      printf("vkpresent ERROR vulkan-1.dll has no vkGetInstanceProcAddr\n");
      return 0;
   }
   GET_I(api, VK_NULL_HANDLE, CreateInstance);
   GET_I(api, VK_NULL_HANDLE, EnumerateInstanceExtensionProperties);
   return 1;
}

static int load_instance_api(struct vk_api *api, VkInstance inst)
{
   GET_I(api, inst, DestroyInstance);
   GET_I(api, inst, EnumeratePhysicalDevices);
   GET_I(api, inst, GetPhysicalDeviceProperties);
   GET_I(api, inst, GetPhysicalDeviceQueueFamilyProperties);
   GET_I(api, inst, GetPhysicalDeviceMemoryProperties);
   GET_I(api, inst, CreateDevice);
   GET_I(api, inst, DestroyDevice);
   GET_I(api, inst, GetDeviceProcAddr);
   return 1;
}

static int load_surface_api(struct vk_api *api, VkInstance inst)
{
   GET_I(api, inst, CreateWin32SurfaceKHR);
   GET_I(api, inst, DestroySurfaceKHR);
   GET_I(api, inst, GetPhysicalDeviceSurfaceSupportKHR);
   GET_I(api, inst, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   GET_I(api, inst, GetPhysicalDeviceSurfaceFormatsKHR);
   GET_I(api, inst, GetPhysicalDeviceSurfacePresentModesKHR);
   return 1;
}

static int load_device_api(struct vk_api *api, VkDevice dev)
{
   GET_D(api, dev, GetDeviceQueue);
   GET_D(api, dev, CreateSwapchainKHR);
   GET_D(api, dev, DestroySwapchainKHR);
   GET_D(api, dev, GetSwapchainImagesKHR);
   GET_D(api, dev, AcquireNextImageKHR);
   GET_D(api, dev, QueuePresentKHR);
   GET_D(api, dev, CreateImageView);
   GET_D(api, dev, DestroyImageView);
   GET_D(api, dev, CreateRenderPass);
   GET_D(api, dev, DestroyRenderPass);
   GET_D(api, dev, CreateFramebuffer);
   GET_D(api, dev, DestroyFramebuffer);
   GET_D(api, dev, CreateCommandPool);
   GET_D(api, dev, DestroyCommandPool);
   GET_D(api, dev, AllocateCommandBuffers);
   GET_D(api, dev, BeginCommandBuffer);
   GET_D(api, dev, EndCommandBuffer);
   GET_D(api, dev, CmdBeginRenderPass);
   GET_D(api, dev, CmdEndRenderPass);
   GET_D(api, dev, CmdPipelineBarrier);
   GET_D(api, dev, CmdCopyImageToBuffer);
   GET_D(api, dev, CreateBuffer);
   GET_D(api, dev, DestroyBuffer);
   GET_D(api, dev, GetBufferMemoryRequirements);
   GET_D(api, dev, AllocateMemory);
   GET_D(api, dev, FreeMemory);
   GET_D(api, dev, BindBufferMemory);
   GET_D(api, dev, MapMemory);
   GET_D(api, dev, UnmapMemory);
   GET_D(api, dev, CreateSemaphore);
   GET_D(api, dev, DestroySemaphore);
   GET_D(api, dev, CreateFence);
   GET_D(api, dev, DestroyFence);
   GET_D(api, dev, WaitForFences);
   GET_D(api, dev, ResetFences);
   GET_D(api, dev, QueueSubmit);
   GET_D(api, dev, DeviceWaitIdle);
   return 1;
}

/* ---- the run ------------------------------------------------------------------------------------------- */

struct run_result {
   unsigned frames;
   unsigned presented;
   unsigned lost;              /* frames whose present or wait failed */
   unsigned mismatched;        /* frames whose readback was not the colour asked for */
   int readback;               /* 1 checked, 0 the surface offers no transfer source */
   uint32_t surface_usage;     /* supportedUsageFlags of the surface: what our WSI exposes */
   double max_ms;
   double total_s;
   int route_asked;
   int route_taken;
   int route_from_registry;
   char device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
   uint32_t api_version;
   uint32_t driver_version;
   uint32_t vendor_id;
   uint32_t device_id;
   char icd_path[MAX_PATH];
   char app_local[256];
   VkFormat format;
   VkPresentModeKHR present_mode;
};

static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *props, uint32_t bits,
                            VkMemoryPropertyFlags want)
{
   uint32_t i;
   for (i = 0; i < props->memoryTypeCount; i++)
      if ((bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

static int present_loop(struct vk_api *api, VkDevice dev, VkQueue queue, VkSwapchainKHR chain,
                        VkImage *images, VkFramebuffer *fbs, VkRenderPass pass, VkCommandBuffer *cmds,
                        uint32_t image_count, const struct options *o, struct run_result *res,
                        VkBuffer readback, void *readback_cpu, size_t readback_bytes)
{
   VkSemaphore acquired[MAX_IMAGES];
   VkSemaphore rendered[MAX_IMAGES];
   VkFence fences[MAX_IMAGES];
   VkSemaphoreCreateInfo sem_info;
   VkFenceCreateInfo fence_info;
   unsigned frame;
   uint32_t i;
   int rc = EXIT_PASS;

   memset(&sem_info, 0, sizeof(sem_info));
   sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
   memset(&fence_info, 0, sizeof(fence_info));
   fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   for (i = 0; i < image_count; i++) {
      acquired[i] = VK_NULL_HANDLE;
      rendered[i] = VK_NULL_HANDLE;
      fences[i] = VK_NULL_HANDLE;
      if (api->CreateSemaphore(dev, &sem_info, NULL, &acquired[i]) != VK_SUCCESS ||
          api->CreateSemaphore(dev, &sem_info, NULL, &rendered[i]) != VK_SUCCESS ||
          api->CreateFence(dev, &fence_info, NULL, &fences[i]) != VK_SUCCESS) {
         printf("vkpresent ERROR could not create the per-image synchronisation objects\n");
         return EXIT_ERROR;
      }
   }

   for (frame = 0; frame < o->frames && !g_quit; frame++) {
      uint32_t index = 0;
      unsigned slot = frame % image_count;
      struct rgba8 asked = frame_colour(frame, 0);
      struct rgba8 expect = frame_colour(frame, o->negative_control);
      VkClearValue clear;
      VkCommandBufferBeginInfo begin;
      VkRenderPassBeginInfo rp;
      VkSubmitInfo submit;
      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkPresentInfoKHR present;
      VkResult r;
      double t0 = now_s();
      double dt;

      pump();
      if (t0 > (double)o->deadline_s) {
         printf("vkpresent DEADLINE after %u frames at %.3f s\n", frame, t0);
         rc = EXIT_DEADLINE;
         break;
      }

      r = api->AcquireNextImageKHR(dev, chain, 2000000000ull, acquired[slot], VK_NULL_HANDLE, &index);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         printf("vkpresent frame %u FAIL acquire vk=%d\n", frame, (int)r);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }
      if (index >= image_count) {
         printf("vkpresent frame %u FAIL acquire gave image %u of %u\n", frame, index, image_count);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }

      memset(&clear, 0, sizeof(clear));
      {
         float c[4];
         c[0] = (float)asked.r / 255.0f;
         c[1] = (float)asked.g / 255.0f;
         c[2] = (float)asked.b / 255.0f;
         c[3] = 1.0f;
         if (format_is_srgb(res->format)) {
            c[0] = srgb_to_linear(c[0]);
            c[1] = srgb_to_linear(c[1]);
            c[2] = srgb_to_linear(c[2]);
         }
         /* A clear value is always given as R, G, B, A, whatever the order of the format's channels: the
          * implementation puts it where that format keeps each channel. Swapping it here for a BGRA format
          * swapped it twice, because the readback below reads the memory of the format, and every frame of the
          * first run on this development PC reported the colour reversed (asked AA327E, got 7E32AA). */
         clear.color.float32[0] = c[0];
         clear.color.float32[1] = c[1];
         clear.color.float32[2] = c[2];
         clear.color.float32[3] = c[3];
      }

      memset(&begin, 0, sizeof(begin));
      begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (api->BeginCommandBuffer(cmds[index], &begin) != VK_SUCCESS) {
         printf("vkpresent frame %u FAIL begin command buffer\n", frame);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }
      memset(&rp, 0, sizeof(rp));
      rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rp.renderPass = pass;
      rp.framebuffer = fbs[index];
      rp.renderArea.extent.width = o->width;
      rp.renderArea.extent.height = o->height;
      rp.clearValueCount = 1;
      rp.pClearValues = &clear;
      api->CmdBeginRenderPass(cmds[index], &rp, VK_SUBPASS_CONTENTS_INLINE);
      api->CmdEndRenderPass(cmds[index]);

      if (res->readback) {
         VkImageMemoryBarrier to_src;
         VkImageMemoryBarrier to_present;
         VkBufferImageCopy copy;

         memset(&to_src, 0, sizeof(to_src));
         to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
         to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
         to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
         to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
         to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
         to_src.image = images[index];
         to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         to_src.subresourceRange.levelCount = 1;
         to_src.subresourceRange.layerCount = 1;
         api->CmdPipelineBarrier(cmds[index], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

         memset(&copy, 0, sizeof(copy));
         copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         copy.imageSubresource.layerCount = 1;
         copy.imageExtent.width = o->width;
         copy.imageExtent.height = 1;        /* one row is enough to name the colour of the frame */
         copy.imageExtent.depth = 1;
         api->CmdCopyImageToBuffer(cmds[index], images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback, 1, &copy);

         memset(&to_present, 0, sizeof(to_present));
         to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
         to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
         to_present.dstAccessMask = 0;
         to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
         to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         to_present.image = images[index];
         to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         to_present.subresourceRange.levelCount = 1;
         to_present.subresourceRange.layerCount = 1;
         api->CmdPipelineBarrier(cmds[index], VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
      }
      if (api->EndCommandBuffer(cmds[index]) != VK_SUCCESS) {
         printf("vkpresent frame %u FAIL end command buffer\n", frame);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }

      memset(&submit, 0, sizeof(submit));
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &acquired[slot];
      submit.pWaitDstStageMask = &wait_stage;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmds[index];
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &rendered[slot];
      if (api->ResetFences(dev, 1, &fences[slot]) != VK_SUCCESS ||
          api->QueueSubmit(queue, 1, &submit, fences[slot]) != VK_SUCCESS) {
         printf("vkpresent frame %u FAIL submit\n", frame);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }

      memset(&present, 0, sizeof(present));
      present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
      present.waitSemaphoreCount = 1;
      present.pWaitSemaphores = &rendered[slot];
      present.swapchainCount = 1;
      present.pSwapchains = &chain;
      present.pImageIndices = &index;
      r = api->QueuePresentKHR(queue, &present);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         printf("vkpresent frame %u FAIL present vk=%d\n", frame, (int)r);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }

      r = api->WaitForFences(dev, 1, &fences[slot], VK_TRUE, 3000000000ull);
      if (r != VK_SUCCESS) {
         printf("vkpresent frame %u FAIL fence wait vk=%d\n", frame, (int)r);
         res->lost++;
         rc = EXIT_FAIL;
         continue;
      }
      res->presented++;
      dt = (now_s() - t0) * 1000.0;
      if (dt > res->max_ms)
         res->max_ms = dt;

      if (res->readback && readback_cpu && readback_bytes >= 4) {
         const uint8_t *p = (const uint8_t *)readback_cpu;
         uint8_t got_r, got_g, got_b;
         if (format_is_bgra(res->format)) {
            got_b = p[0];
            got_g = p[1];
            got_r = p[2];
         } else {
            got_r = p[0];
            got_g = p[1];
            got_b = p[2];
         }
         /* One step of slack per channel: an sRGB surface rounds, and a driver may dither the clear. */
         if ((int)got_r - (int)expect.r > 1 || (int)expect.r - (int)got_r > 1 ||
             (int)got_g - (int)expect.g > 1 || (int)expect.g - (int)got_g > 1 ||
             (int)got_b - (int)expect.b > 1 || (int)expect.b - (int)got_b > 1) {
            printf("vkpresent frame %u FAIL readback asked %02X%02X%02X expected %02X%02X%02X got "
                   "%02X%02X%02X\n", frame, asked.r, asked.g, asked.b, expect.r, expect.g, expect.b,
                   got_r, got_g, got_b);
            res->mismatched++;
            rc = EXIT_FAIL;
         }
      }
   }

   api->DeviceWaitIdle(dev);
   for (i = 0; i < image_count; i++) {
      if (acquired[i])
         api->DestroySemaphore(dev, acquired[i], NULL);
      if (rendered[i])
         api->DestroySemaphore(dev, rendered[i], NULL);
      if (fences[i])
         api->DestroyFence(dev, fences[i], NULL);
   }
   if (!format_is_bgra(res->format) && !format_is_rgba(res->format) && res->readback) {
      printf("vkpresent note: the surface format %d is not an 8-bit BGRA or RGBA one, so the readback was "
             "not compared\n", (int)res->format);
   }
   return rc;
}

static void write_json(const struct options *o, const struct run_result *r, int rc)
{
   FILE *f;
   if (!o->json)
      return;
   f = fopen(o->json, "wb");
   if (!f) {
      printf("vkpresent note: could not write %s\n", o->json);
      return;
   }
   fprintf(f, "{\n");
   fprintf(f, "  \"arch\": \"%s\",\n", sizeof(void *) == 8 ? "x64" : "x86");
   fprintf(f, "  \"result\": \"%s\",\n", rc == EXIT_PASS ? "PASS" : "FAIL");
   fprintf(f, "  \"exit\": %d,\n", rc);
   fprintf(f, "  \"route_asked\": \"%s\",\n", ROUTE_NAMES[r->route_asked]);
   fprintf(f, "  \"route_taken\": \"%s\",\n", ROUTE_NAMES[r->route_taken]);
   fprintf(f, "  \"route_from_registry\": %s,\n", r->route_from_registry ? "true" : "false");
   fprintf(f, "  \"icd\": \"");
   {
      const char *p;
      for (p = r->icd_path; *p; p++) {
         if (*p == '\\')
            fputs("\\\\", f);
         else
            fputc(*p, f);
      }
   }
   fprintf(f, "\",\n");
   fprintf(f, "  \"device\": \"%s\",\n", r->device);
   fprintf(f, "  \"vendor_id\": %u,\n", r->vendor_id);
   fprintf(f, "  \"device_id\": %u,\n", r->device_id);
   fprintf(f, "  \"api_version\": \"%u.%u.%u\",\n", VK_API_VERSION_MAJOR(r->api_version),
           VK_API_VERSION_MINOR(r->api_version), VK_API_VERSION_PATCH(r->api_version));
   fprintf(f, "  \"driver_version\": %u,\n", r->driver_version);
   fprintf(f, "  \"format\": %d,\n", (int)r->format);
   fprintf(f, "  \"present_mode\": %d,\n", (int)r->present_mode);
   fprintf(f, "  \"surface_usage\": %u,\n", r->surface_usage);
   fprintf(f, "  \"frames\": %u,\n", r->frames);
   fprintf(f, "  \"presented\": %u,\n", r->presented);
   fprintf(f, "  \"lost\": %u,\n", r->lost);
   fprintf(f, "  \"mismatched\": %u,\n", r->mismatched);
   fprintf(f, "  \"readback\": \"%s\",\n", r->readback ? (r->mismatched ? "bad" : "ok") : "unsupported");
   fprintf(f, "  \"max_ms\": %.3f,\n", r->max_ms);
   fprintf(f, "  \"total_s\": %.3f,\n", r->total_s);
   fprintf(f, "  \"app_local\": \"%s\"\n", r->app_local[0] ? r->app_local : "none");
   fprintf(f, "}\n");
   fclose(f);
}

int main(int argc, char **argv)
{
   struct options o;
   struct vk_api api;
   struct run_result res;
   struct modules before;
   struct modules after;
   VkInstance inst = VK_NULL_HANDLE;
   VkPhysicalDevice phys = VK_NULL_HANDLE;
   VkPhysicalDevice devices[32];
   uint32_t device_count = (uint32_t)(sizeof(devices) / sizeof(devices[0]));
   VkPhysicalDeviceProperties props;
   VkPhysicalDeviceMemoryProperties mem_props;
   VkInstanceCreateInfo inst_info;
   VkApplicationInfo app;
   const char *inst_ext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
   const char *dev_ext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   VkResult r;
   uint32_t i;
   int rc;
   int found;

   QueryPerformanceFrequency(&g_freq);
   QueryPerformanceCounter(&g_start);
   /* Unbuffered, not line buffered: MSVC's setvbuf refuses a line-buffered stream with no buffer, and a
    * client that may hang in its first present must have every line on disk before it does (BD-159). */
   setvbuf(stdout, NULL, _IONBF, 0);

   if (!parse_options(argc, argv, &o)) {
      usage();
      return EXIT_USAGE;
   }
   /* A window cannot be shown from session 0, where an SSH shell on the lab runs: the present would fail or
    * the frames would go nowhere, and the run would say something about this program instead of about the
    * route. The operator script starts it in the interactive session; this says so when something else does
    * not. --no-present needs no window and no session. */
   if (!o.no_present) {
      DWORD session = 0;
      if (ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == 0) {
         printf("vkpresent ERROR this is session 0 (an SSH shell on the lab), where no window can be shown. "
                "Start it in the interactive desktop session, the way tools\\win\\vkpresent\\README.md "
                "describes, or use --no-present for the enumeration alone.\n");
         return EXIT_ERROR;
      }
   }
   memset(&res, 0, sizeof(res));
   res.frames = o.frames;
   res.route_asked = route_from_registry(&found);
   res.route_from_registry = found;
   if (o.expect_route >= 0)
      res.route_asked = o.expect_route;

   read_modules(&before);
   strncpy(res.app_local, before.app_local, sizeof(res.app_local) - 1);
   if (res.app_local[0])
      printf("vkpresent note: this program's folder holds %s, which stops the DXGI route by design\n",
             res.app_local);

   if (!load_loader(&api))
      return EXIT_ERROR;

   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "vkpresent";
   app.applicationVersion = 1;
   app.apiVersion = VK_API_VERSION_1_1;
   memset(&inst_info, 0, sizeof(inst_info));
   inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   inst_info.pApplicationInfo = &app;
   inst_info.enabledExtensionCount = o.no_present ? 0u : 2u;
   inst_info.ppEnabledExtensionNames = o.no_present ? NULL : inst_ext;
   r = api.CreateInstance(&inst_info, NULL, &inst);
   if (r != VK_SUCCESS) {
      fail_line("vkCreateInstance", r);
      return EXIT_ERROR;
   }
   if (!load_instance_api(&api, inst))
      return EXIT_ERROR;
   if (!o.no_present && !load_surface_api(&api, inst))
      return EXIT_ERROR;

   r = api.EnumeratePhysicalDevices(inst, &device_count, devices);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || device_count == 0) {
      printf("vkpresent ERROR no Vulkan device (vk=%d, count %u). The loader found no ICD for this "
             "architecture.\n", (int)r, device_count);
      return EXIT_ERROR;
   }
   for (i = 0; i < device_count; i++) {
      api.GetPhysicalDeviceProperties(devices[i], &props);
      if (o.list_devices)
         printf("vkpresent device %u: %s vendor 0x%04X device 0x%04X api %u.%u.%u driver %u\n", i,
                props.deviceName, props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
                props.driverVersion);
      if (phys == VK_NULL_HANDLE) {
         if (o.device_index >= 0) {
            if ((uint32_t)o.device_index == i)
               phys = devices[i];
         } else if (o.any_device ||
                    (props.vendorID == BC250_VENDOR_ID && props.deviceID == BC250_DEVICE_ID)) {
            phys = devices[i];
         }
      }
   }
   if (o.list_devices)
      return EXIT_PASS;
   if (phys == VK_NULL_HANDLE) {
      printf("vkpresent ERROR none of the %u devices is the BC-250 board (0x%04X:0x%04X). Use "
             "--any-device or --device-index.\n", device_count, BC250_VENDOR_ID, BC250_DEVICE_ID);
      return EXIT_ERROR;
   }
   api.GetPhysicalDeviceProperties(phys, &props);
   api.GetPhysicalDeviceMemoryProperties(phys, &mem_props);
   strncpy(res.device, props.deviceName, sizeof(res.device) - 1);
   res.api_version = props.apiVersion;
   res.driver_version = props.driverVersion;
   res.vendor_id = props.vendorID;
   res.device_id = props.deviceID;

   read_modules(&after);
   strncpy(res.icd_path, after.icd_path, sizeof(res.icd_path) - 1);
   if (o.no_present) {
      res.route_taken = res.route_asked;
      printf("vkpresent ENUMERATED arch=%s icd=%s device=\"%s\" api=%u.%u.%u driver=%u devices=%u\n",
             sizeof(void *) == 8 ? "x64" : "x86", res.icd_path[0] ? res.icd_path : "unknown",
             res.device, VK_API_VERSION_MAJOR(res.api_version), VK_API_VERSION_MINOR(res.api_version),
             VK_API_VERSION_PATCH(res.api_version), res.driver_version, device_count);
      write_json(&o, &res, EXIT_PASS);
      return EXIT_PASS;
   }

   /* ---- the window, the surface and the swapchain ------------------------------------------------------ */
   {
      HWND window = make_window(&o);
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      VkWin32SurfaceCreateInfoKHR surf_info;
      VkSurfaceCapabilitiesKHR caps;
      VkSurfaceFormatKHR formats[32];
      uint32_t format_count = (uint32_t)(sizeof(formats) / sizeof(formats[0]));
      VkPresentModeKHR modes[8];
      uint32_t mode_count = (uint32_t)(sizeof(modes) / sizeof(modes[0]));
      VkQueueFamilyProperties families[16];
      uint32_t family_count = (uint32_t)(sizeof(families) / sizeof(families[0]));
      uint32_t family = UINT32_MAX;
      VkDeviceQueueCreateInfo queue_info;
      VkDeviceCreateInfo dev_info;
      VkDevice dev = VK_NULL_HANDLE;
      VkQueue queue = VK_NULL_HANDLE;
      VkSwapchainCreateInfoKHR chain_info;
      VkSwapchainKHR chain = VK_NULL_HANDLE;
      VkImage images[MAX_IMAGES];
      VkImageView views[MAX_IMAGES];
      VkFramebuffer fbs[MAX_IMAGES];
      VkCommandBuffer cmds[MAX_IMAGES];
      uint32_t image_count = 0;
      VkAttachmentDescription attach;
      VkAttachmentReference ref;
      VkSubpassDescription subpass;
      VkRenderPassCreateInfo pass_info;
      VkRenderPass pass = VK_NULL_HANDLE;
      VkCommandPoolCreateInfo pool_info;
      VkCommandPool pool = VK_NULL_HANDLE;
      VkCommandBufferAllocateInfo cmd_info;
      VkBuffer readback = VK_NULL_HANDLE;
      VkDeviceMemory readback_mem = VK_NULL_HANDLE;
      void *readback_cpu = NULL;
      size_t readback_bytes = (size_t)o.width * 4u;
      float prio = 1.0f;

      if (!window) {
         printf("vkpresent ERROR the window was not created (GetLastError %lu)\n",
                (unsigned long)GetLastError());
         return EXIT_ERROR;
      }

      memset(&surf_info, 0, sizeof(surf_info));
      surf_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
      surf_info.hinstance = GetModuleHandleA(NULL);
      surf_info.hwnd = window;
      r = api.CreateWin32SurfaceKHR(inst, &surf_info, NULL, &surface);
      if (r != VK_SUCCESS) {
         fail_line("vkCreateWin32SurfaceKHR", r);
         return EXIT_ERROR;
      }

      api.GetPhysicalDeviceQueueFamilyProperties(phys, &family_count, families);
      for (i = 0; i < family_count; i++) {
         VkBool32 supported = VK_FALSE;
         if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
         if (api.GetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &supported) == VK_SUCCESS &&
             supported) {
            family = i;
            break;
         }
      }
      if (family == UINT32_MAX) {
         printf("vkpresent ERROR no graphics queue family presents to this surface\n");
         return EXIT_ERROR;
      }

      memset(&queue_info, 0, sizeof(queue_info));
      queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      queue_info.queueFamilyIndex = family;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &prio;
      memset(&dev_info, 0, sizeof(dev_info));
      dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
      dev_info.queueCreateInfoCount = 1;
      dev_info.pQueueCreateInfos = &queue_info;
      dev_info.enabledExtensionCount = 1;
      dev_info.ppEnabledExtensionNames = dev_ext;
      r = api.CreateDevice(phys, &dev_info, NULL, &dev);
      if (r != VK_SUCCESS) {
         fail_line("vkCreateDevice", r);
         return EXIT_ERROR;
      }
      if (!load_device_api(&api, dev))
         return EXIT_ERROR;
      api.GetDeviceQueue(dev, family, 0, &queue);

      if (api.GetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) {
         printf("vkpresent ERROR the surface has no capabilities\n");
         return EXIT_ERROR;
      }
      if (api.GetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &format_count, formats) != VK_SUCCESS ||
          format_count == 0) {
         printf("vkpresent ERROR the surface offers no format\n");
         return EXIT_ERROR;
      }
      res.format = formats[0].format;
      for (i = 0; i < format_count; i++) {
         if (format_is_bgra(formats[i].format) || format_is_rgba(formats[i].format)) {
            res.format = formats[i].format;
            break;
         }
      }
      res.present_mode = VK_PRESENT_MODE_FIFO_KHR;
      if (api.GetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &mode_count, modes) != VK_SUCCESS ||
          mode_count == 0) {
         printf("vkpresent ERROR the surface offers no present mode\n");
         return EXIT_ERROR;
      }

      /* The readback needs the images as a transfer source, which the surface has to allow. The clear is the
       * render pass's own load operation, so nothing here copies into the image and TRANSFER_DST is not asked
       * for. On a foreign ICD a surface without TRANSFER_SRC is no failure: the frames are still presented and
       * the line says the pixels were not compared. On our own board it is a failure, because then the run
       * proves nothing about the frames it presented, which is the only thing it is for. The usage mask goes
       * in the line either way, so the first lab arm says what our WSI exposes. */
      res.readback = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? 1 : 0;
      if (res.readback && !format_is_bgra(res.format) && !format_is_rgba(res.format))
         res.readback = 0;
      if (!res.readback && res.vendor_id == BC250_VENDOR_ID && res.device_id == BC250_DEVICE_ID) {
         printf("vkpresent FAIL readback=unsupported on 0x%04X:0x%04X: surface usage 0x%X needs 0x%X "
                "(VK_IMAGE_USAGE_TRANSFER_SRC_BIT), format %d. The frames cannot be compared, so the run "
                "proves nothing about them.\n",
                res.vendor_id, res.device_id, caps.supportedUsageFlags,
                (unsigned)VK_IMAGE_USAGE_TRANSFER_SRC_BIT, (int)res.format);
         return EXIT_FAIL;
      }
      res.surface_usage = caps.supportedUsageFlags;

      memset(&chain_info, 0, sizeof(chain_info));
      chain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
      chain_info.surface = surface;
      chain_info.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
      if (caps.maxImageCount && chain_info.minImageCount > caps.maxImageCount)
         chain_info.minImageCount = caps.maxImageCount;
      chain_info.imageFormat = res.format;
      chain_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      chain_info.imageExtent.width = o.width;
      chain_info.imageExtent.height = o.height;
      if (caps.currentExtent.width != 0xFFFFFFFFu) {
         chain_info.imageExtent = caps.currentExtent;
         o.width = caps.currentExtent.width;
         o.height = caps.currentExtent.height;
         readback_bytes = (size_t)o.width * 4u;
      }
      chain_info.imageArrayLayers = 1;
      chain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              (res.readback ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
      chain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      chain_info.preTransform = caps.currentTransform;
      chain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      chain_info.presentMode = res.present_mode;
      chain_info.clipped = VK_TRUE;
      r = api.CreateSwapchainKHR(dev, &chain_info, NULL, &chain);
      if (r != VK_SUCCESS) {
         fail_line("vkCreateSwapchainKHR", r);
         return EXIT_ERROR;
      }

      image_count = MAX_IMAGES;
      if (api.GetSwapchainImagesKHR(dev, chain, &image_count, images) != VK_SUCCESS || image_count == 0) {
         printf("vkpresent ERROR the swapchain has no image\n");
         return EXIT_ERROR;
      }
      if (image_count > MAX_IMAGES)
         image_count = MAX_IMAGES;

      memset(&attach, 0, sizeof(attach));
      attach.format = res.format;
      attach.samples = VK_SAMPLE_COUNT_1_BIT;
      attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      /* With the readback the pass leaves the image as a transfer source and the barriers above take it to
       * PRESENT_SRC; without it the pass leaves it ready to present. */
      attach.finalLayout = res.readback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                        : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      memset(&ref, 0, sizeof(ref));
      ref.attachment = 0;
      ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      memset(&subpass, 0, sizeof(subpass));
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &ref;
      memset(&pass_info, 0, sizeof(pass_info));
      pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      pass_info.attachmentCount = 1;
      pass_info.pAttachments = &attach;
      pass_info.subpassCount = 1;
      pass_info.pSubpasses = &subpass;
      if (api.CreateRenderPass(dev, &pass_info, NULL, &pass) != VK_SUCCESS) {
         printf("vkpresent ERROR the render pass was not created\n");
         return EXIT_ERROR;
      }

      for (i = 0; i < image_count; i++) {
         VkImageViewCreateInfo view_info;
         VkFramebufferCreateInfo fb_info;
         memset(&view_info, 0, sizeof(view_info));
         view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
         view_info.image = images[i];
         view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
         view_info.format = res.format;
         view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         view_info.subresourceRange.levelCount = 1;
         view_info.subresourceRange.layerCount = 1;
         views[i] = VK_NULL_HANDLE;
         fbs[i] = VK_NULL_HANDLE;
         if (api.CreateImageView(dev, &view_info, NULL, &views[i]) != VK_SUCCESS) {
            printf("vkpresent ERROR image view %u was not created\n", i);
            return EXIT_ERROR;
         }
         memset(&fb_info, 0, sizeof(fb_info));
         fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
         fb_info.renderPass = pass;
         fb_info.attachmentCount = 1;
         fb_info.pAttachments = &views[i];
         fb_info.width = o.width;
         fb_info.height = o.height;
         fb_info.layers = 1;
         if (api.CreateFramebuffer(dev, &fb_info, NULL, &fbs[i]) != VK_SUCCESS) {
            printf("vkpresent ERROR framebuffer %u was not created\n", i);
            return EXIT_ERROR;
         }
      }

      memset(&pool_info, 0, sizeof(pool_info));
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = family;
      if (api.CreateCommandPool(dev, &pool_info, NULL, &pool) != VK_SUCCESS) {
         printf("vkpresent ERROR the command pool was not created\n");
         return EXIT_ERROR;
      }
      memset(&cmd_info, 0, sizeof(cmd_info));
      cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cmd_info.commandPool = pool;
      cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cmd_info.commandBufferCount = image_count;
      if (api.AllocateCommandBuffers(dev, &cmd_info, cmds) != VK_SUCCESS) {
         printf("vkpresent ERROR the command buffers were not allocated\n");
         return EXIT_ERROR;
      }

      if (res.readback) {
         VkBufferCreateInfo buf_info;
         VkMemoryRequirements req;
         VkMemoryAllocateInfo alloc;
         uint32_t type;
         memset(&buf_info, 0, sizeof(buf_info));
         buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
         buf_info.size = readback_bytes;
         buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
         buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
         if (api.CreateBuffer(dev, &buf_info, NULL, &readback) != VK_SUCCESS) {
            printf("vkpresent ERROR the readback buffer was not created\n");
            return EXIT_ERROR;
         }
         api.GetBufferMemoryRequirements(dev, readback, &req);
         type = memory_type(&mem_props, req.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
         if (type == UINT32_MAX) {
            printf("vkpresent note: no host visible and coherent memory type, so the pixels are not "
                   "compared\n");
            res.readback = 0;
         } else {
            memset(&alloc, 0, sizeof(alloc));
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = type;
            if (api.AllocateMemory(dev, &alloc, NULL, &readback_mem) != VK_SUCCESS ||
                api.BindBufferMemory(dev, readback, readback_mem, 0) != VK_SUCCESS ||
                api.MapMemory(dev, readback_mem, 0, req.size, 0, &readback_cpu) != VK_SUCCESS) {
               printf("vkpresent ERROR the readback buffer has no host memory\n");
               return EXIT_ERROR;
            }
         }
      }

      rc = present_loop(&api, dev, queue, chain, images, fbs, pass, cmds, image_count, &o, &res,
                        readback, readback_cpu, readback_bytes);
      res.total_s = now_s();

      read_modules(&after);
      /* The witness is the set of modules this process loaded, and it speaks for our ICD alone. A foreign ICD
       * binds dxgi.dll, d3d12.dll and DComp.dll for its own reasons: the first run on this development PC,
       * through the NVIDIA ICD, read "took dxgi-composition" from a process where our WSI was never loaded.
       * So on any device but the board the route is reported as unknown and decides nothing. */
      res.route_taken = (res.vendor_id == BC250_VENDOR_ID && res.device_id == BC250_DEVICE_ID)
                        ? route_from_modules(&after) : ROUTE_UNKNOWN;
      if (res.icd_path[0] == '\0')
         strncpy(res.icd_path, after.icd_path, sizeof(res.icd_path) - 1);

      if (readback_cpu)
         api.UnmapMemory(dev, readback_mem);
      if (readback_mem)
         api.FreeMemory(dev, readback_mem, NULL);
      if (readback)
         api.DestroyBuffer(dev, readback, NULL);
      for (i = 0; i < image_count; i++) {
         if (fbs[i])
            api.DestroyFramebuffer(dev, fbs[i], NULL);
         if (views[i])
            api.DestroyImageView(dev, views[i], NULL);
      }
      if (pool)
         api.DestroyCommandPool(dev, pool, NULL);
      if (pass)
         api.DestroyRenderPass(dev, pass, NULL);
      if (chain)
         api.DestroySwapchainKHR(dev, chain, NULL);
      api.DestroyDevice(dev, NULL);
      api.DestroySurfaceKHR(inst, surface, NULL);
      DestroyWindow(window);
   }
   api.DestroyInstance(inst, NULL);

   if (res.route_taken == ROUTE_UNKNOWN) {
      printf("vkpresent note: the device is 0x%04X:0x%04X, not the board, so the route was not witnessed "
             "(dxgi.dll %s, d3d12 %s, DComp %s say nothing here). This run proves the present and the "
             "readback of this program, and nothing about our WSI.\n", res.vendor_id, res.device_id,
             after.dxgi ? (after.dxgi == 1 ? "System32" : "app-local") : "no",
             after.d3d12 ? (after.d3d12 == 1 ? "System32" : "app-local") : "no",
             after.dcomp ? (after.dcomp == 1 ? "System32" : "app-local") : "no");
   } else if (res.route_taken != res.route_asked) {
      printf("vkpresent ROUTE asked %s, took %s (dxgi.dll %s, d3d12 %s, DComp %s)\n",
             ROUTE_NAMES[res.route_asked], ROUTE_NAMES[res.route_taken],
             after.dxgi ? (after.dxgi == 1 ? "System32" : "app-local") : "no",
             after.d3d12 ? (after.d3d12 == 1 ? "System32" : "app-local") : "no",
             after.dcomp ? (after.dcomp == 1 ? "System32" : "app-local") : "no");
      rc = EXIT_FAIL;
   }
   if (o.negative_control) {
      /* The control is the comparison itself: every frame must have failed it. A control that passes means
       * the readback proves nothing, so it is reported as the failure it is. */
      if (res.mismatched == res.presented && res.presented > 0) {
         printf("vkpresent NEGATIVE-CONTROL ok: %u of %u frames failed the comparison\n", res.mismatched,
                res.presented);
         rc = EXIT_FAIL;
      } else {
         printf("vkpresent NEGATIVE-CONTROL BROKEN: only %u of %u frames failed; the comparison proves "
                "nothing\n", res.mismatched, res.presented);
         rc = EXIT_ERROR;
      }
   }

   printf("vkpresent %s arch=%s route=%s/%s source=%s icd=%s device=\"%s\" api=%u.%u.%u driver=%u "
          "format=%d usage=0x%X frames=%u presented=%u lost=%u readback=%s maxms=%.3f total=%.3fs\n",
          rc == EXIT_PASS ? "PASS" : "FAIL", sizeof(void *) == 8 ? "x64" : "x86",
          ROUTE_NAMES[res.route_asked], ROUTE_NAMES[res.route_taken],
          res.route_from_registry ? "registry" : "default",
          res.icd_path[0] ? res.icd_path : "unknown", res.device,
          VK_API_VERSION_MAJOR(res.api_version), VK_API_VERSION_MINOR(res.api_version),
          VK_API_VERSION_PATCH(res.api_version), res.driver_version, (int)res.format, res.surface_usage,
          res.frames, res.presented, res.lost,
          res.readback ? (res.mismatched ? "bad" : "ok") : "unsupported", res.max_ms, res.total_s);
   write_json(&o, &res, rc);
   return rc;
}
