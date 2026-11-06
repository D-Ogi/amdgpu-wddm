// SPDX-License-Identifier: MIT
// nv12layout: the layout that the Vulkan driver gives a LINEAR two-plane 4:2:0 image (BD-223 V7). An NV12 runtime
// surface (a YUV swap-chain buffer) is one linear allocation, and the KMD and the UMD must agree on where the CbCr
// plane starts and on the pitch of each plane. This probe reads those numbers from the driver instead of computing them:
// the format features (linear and optimal), the usages a linear image admits, and for each size the offset, size and
// row pitch of plane 0 and plane 1 (vkGetImageSubresourceLayout) and the memory size and alignment.
//
//   nv12layout.exe --out <file.json> [--icd <path of a Vulkan ICD DLL>]
//
// Without --icd the Vulkan loader (vulkan-1.dll) picks the driver. With --icd the probe loads that ICD itself through
// vk_icdGetInstanceProcAddr, so the numbers are those of exactly that file (the hosted RADV of the D3D11 shell).
// Exit 0: every size was measured. Exit 1: a size could not be measured. Exit 3: no Vulkan driver or device. Exit 2:
// usage. Last line: NV12LAYOUT <OK|PARTIAL|SETUP> bits=<32|64> driver=<name> sizes=<n> measured=<m> first=<why|->
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

PFN_vkGetInstanceProcAddr gipa = nullptr;
#define FN(name) PFN_##name name = nullptr
FN(vkCreateInstance); FN(vkDestroyInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceProperties2);
FN(vkGetPhysicalDeviceFormatProperties); FN(vkGetPhysicalDeviceImageFormatProperties2); FN(vkGetPhysicalDeviceQueueFamilyProperties);
FN(vkCreateDevice); FN(vkGetDeviceProcAddr); FN(vkDestroyDevice); FN(vkCreateImage); FN(vkDestroyImage);
FN(vkGetImageSubresourceLayout); FN(vkGetImageMemoryRequirements);
#undef FN

std::string hex(uint64_t v) { char b[24]; std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v)); return b; }

struct Size { uint32_t w, h; };
// 1080 and 720 lines (the common video heights), a width that is no multiple of 64, and a height whose chroma plane
// has an odd number of rows (362 / 2 = 181).
const Size kSizes[] = {{1920, 1080}, {1280, 720}, {854, 480}, {640, 362}};

struct Fmt { const char *name; VkFormat vk; };
const Fmt kFormats[] = {
    {"NV12", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM},
    {"P010", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16},
};

struct Usage { const char *name; VkImageUsageFlags bits; };
const Usage kUsages[] = {
    {"transfer", VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
    {"transfer+sampled", VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
    {"transfer+sampled+color", VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
};

}  // namespace

int main(int argc, char **argv) {
    std::string out, icd;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { std::printf("nv12layout.exe --out <file.json> [--icd <ICD DLL>]\n"); return 0; }
        if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
        if (a == "--icd" && i + 1 < argc) { icd = argv[++i]; continue; }
        std::fprintf(stderr, "nv12layout: unknown argument %s\n", a.c_str());
        return 2;
    }
    if (out.empty()) { std::fprintf(stderr, "nv12layout: --out is required\n"); return 2; }
    const unsigned bits = unsigned(sizeof(void *) * 8);
    std::string setup, driver = "-", json;

    HMODULE lib = LoadLibraryA(icd.empty() ? "vulkan-1.dll" : icd.c_str());
    if (!lib) setup = std::string("LoadLibrary ") + (icd.empty() ? "vulkan-1.dll" : icd) + " error " + std::to_string(GetLastError());
    if (setup.empty()) {
        if (icd.empty()) {
            gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
        } else {
            // The loader-ICD interface: negotiate first, then vk_icdGetInstanceProcAddr is the instance entry.
            using Negotiate = VkResult(VKAPI_PTR *)(uint32_t *);
            auto negotiate = reinterpret_cast<Negotiate>(reinterpret_cast<void *>(GetProcAddress(lib, "vk_icdNegotiateLoaderICDInterfaceVersion")));
            if (negotiate) {
                uint32_t version = 5;
                const VkResult nr = negotiate(&version);
                json += "  \"icd_interface\": {\"result\": " + std::to_string(int(nr)) + ", \"version\": " + std::to_string(version) + "},\n";
            }
            gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void *>(GetProcAddress(lib, "vk_icdGetInstanceProcAddr")));
        }
        if (!gipa) setup = "no vkGetInstanceProcAddr / vk_icdGetInstanceProcAddr export";
    }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (setup.empty()) {
        vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "nv12layout";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        const VkResult r = vkCreateInstance ? vkCreateInstance(&ici, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
        if (r != VK_SUCCESS) setup = "vkCreateInstance " + std::to_string(int(r));
    }
    if (setup.empty()) {
#define LOAD(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
        LOAD(vkDestroyInstance); LOAD(vkEnumeratePhysicalDevices); LOAD(vkGetPhysicalDeviceProperties2);
        LOAD(vkGetPhysicalDeviceFormatProperties); LOAD(vkGetPhysicalDeviceImageFormatProperties2);
        LOAD(vkGetPhysicalDeviceQueueFamilyProperties); LOAD(vkCreateDevice); LOAD(vkGetDeviceProcAddr);
#undef LOAD
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> list(n);
        if (n) vkEnumeratePhysicalDevices(instance, &n, list.data());
        if (!n) setup = "no physical device";
        else phys = list[0];
        json += "  \"physical_devices\": " + std::to_string(n) + ",\n";
    }
    VkDevice device = VK_NULL_HANDLE;
    if (setup.empty()) {
        VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
        vkGetPhysicalDeviceProperties2(phys, &p2);
        driver = dp.driverName;
        char v[160];
        std::snprintf(v, sizeof(v), "  \"device\": \"%04X:%04X %s\",\n  \"driver_info\": \"%s\",\n", p2.properties.vendorID,
                      p2.properties.deviceID, p2.properties.deviceName, dp.driverInfo);
        json += v;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = 0; q.queueCount = 1; q.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = qn ? 1 : 0; dci.pQueueCreateInfos = &q;
        const VkResult r = vkCreateDevice(phys, &dci, nullptr, &device);
        if (r != VK_SUCCESS) setup = "vkCreateDevice " + std::to_string(int(r));
    }
    unsigned sizes = 0, measured = 0;
    std::string first = "-";
    if (setup.empty()) {
#define LOADD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
        LOADD(vkDestroyDevice); LOADD(vkCreateImage); LOADD(vkDestroyImage); LOADD(vkGetImageSubresourceLayout);
        LOADD(vkGetImageMemoryRequirements);
#undef LOADD
        json += "  \"formats\": [\n";
        for (size_t fi = 0; fi < sizeof(kFormats) / sizeof(kFormats[0]); ++fi) {
            const Fmt &fmt = kFormats[fi];
            VkFormatProperties fp = {};
            vkGetPhysicalDeviceFormatProperties(phys, fmt.vk, &fp);
            json += std::string("    {\"format\": \"") + fmt.name + "\", \"linear_features\": \"" + hex(fp.linearTilingFeatures) +
                    "\", \"optimal_features\": \"" + hex(fp.optimalTilingFeatures) + "\", \"linear_usages\": {";
            VkImageUsageFlags best = 0;
            for (size_t ui = 0; ui < sizeof(kUsages) / sizeof(kUsages[0]); ++ui) {
                VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
                info.format = fmt.vk; info.type = VK_IMAGE_TYPE_2D; info.tiling = VK_IMAGE_TILING_LINEAR; info.usage = kUsages[ui].bits;
                VkImageFormatProperties2 prop = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
                const VkResult r = vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &prop);
                if (r == VK_SUCCESS) best = kUsages[ui].bits;
                json += std::string(ui ? ", " : "") + "\"" + kUsages[ui].name + "\": " + std::to_string(int(r));
            }
            json += "}, \"sizes\": [";
            for (size_t si = 0; si < sizeof(kSizes) / sizeof(kSizes[0]); ++si) {
                const Size s = kSizes[si];
                ++sizes;
                char why[96] = {};
                VkImage image = VK_NULL_HANDLE;
                VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt.vk; ci.extent = {s.w, s.h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
                ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_LINEAR; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                ci.usage = best ? best : kUsages[0].bits; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                const VkResult r = vkCreateImage(device, &ci, nullptr, &image);
                char row[512];
                if (r != VK_SUCCESS) {
                    std::snprintf(why, sizeof(why), "%s %ux%u vkCreateImage %d", fmt.name, s.w, s.h, int(r));
                    std::snprintf(row, sizeof(row), "%s{\"w\": %u, \"h\": %u, \"error\": \"vkCreateImage %d\"}", si ? ", " : "", s.w, s.h, int(r));
                } else {
                    VkSubresourceLayout l0 = {}, l1 = {};
                    VkImageSubresource sr = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0};
                    vkGetImageSubresourceLayout(device, image, &sr, &l0);
                    sr.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
                    vkGetImageSubresourceLayout(device, image, &sr, &l1);
                    VkMemoryRequirements mr = {};
                    vkGetImageMemoryRequirements(device, image, &mr);
                    vkDestroyImage(device, image, nullptr);
                    ++measured;
                    std::snprintf(row, sizeof(row),
                                  "%s{\"w\": %u, \"h\": %u, \"usage\": \"%s\", \"plane0\": {\"offset\": %llu, \"size\": %llu, \"row_pitch\": %llu}, "
                                  "\"plane1\": {\"offset\": %llu, \"size\": %llu, \"row_pitch\": %llu}, \"memory_size\": %llu, \"alignment\": %llu}",
                                  si ? ", " : "", s.w, s.h, hex(ci.usage).c_str(), (unsigned long long)l0.offset, (unsigned long long)l0.size,
                                  (unsigned long long)l0.rowPitch, (unsigned long long)l1.offset, (unsigned long long)l1.size,
                                  (unsigned long long)l1.rowPitch, (unsigned long long)mr.size, (unsigned long long)mr.alignment);
                    std::printf("LAYOUT %s %ux%u usage=%s y.offset=%llu y.pitch=%llu uv.offset=%llu uv.pitch=%llu uv.size=%llu mem=%llu align=%llu\n",
                                fmt.name, s.w, s.h, hex(ci.usage).c_str(), (unsigned long long)l0.offset, (unsigned long long)l0.rowPitch,
                                (unsigned long long)l1.offset, (unsigned long long)l1.rowPitch, (unsigned long long)l1.size,
                                (unsigned long long)mr.size, (unsigned long long)mr.alignment);
                }
                if (why[0] && first == "-") first = why;
                json += row;
            }
            json += std::string("]}") + (fi + 1 < sizeof(kFormats) / sizeof(kFormats[0]) ? "," : "") + "\n";
        }
        json += "  ],\n";
        vkDestroyDevice(device, nullptr);
    }
    if (instance && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
    const char *verdict = !setup.empty() ? "SETUP" : (measured == sizes ? "OK" : "PARTIAL");
    FILE *f = nullptr;
    if (fopen_s(&f, out.c_str(), "wb") == 0 && f) {
        std::string icdText;
        for (char ch : icd) { if (ch == '\\' || ch == '"') icdText += '\\'; icdText += ch; }
        std::fprintf(f, "{\n  \"tool\": \"nv12layout\",\n  \"bits\": %u,\n  \"icd\": \"%s\",\n  \"setup\": \"%s\",\n  \"driver\": \"%s\",\n%s  \"verdict\": \"NV12LAYOUT-%s\"\n}\n",
                     bits, icdText.c_str(), setup.c_str(), driver.c_str(), json.c_str(), verdict);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "nv12layout: cannot write %s\n", out.c_str());
    }
    if (!setup.empty()) first = setup;
    std::printf("NV12LAYOUT %s bits=%u driver=%s sizes=%u measured=%u first=%s\n", verdict, bits, driver.c_str(), sizes, measured, first.c_str());
    return !setup.empty() ? 3 : (measured == sizes ? 0 : 1);
}
