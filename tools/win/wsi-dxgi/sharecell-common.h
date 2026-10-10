// SPDX-License-Identifier: MIT
//
// sharecell-common.h - what the two cross-stack share cells have in common.
//
// Why these clients exist. The b27 Vulkan WSI DXGI route shares two kinds of object between RADV and
// our D3D12 shell: a D3D12 committed texture that RADV imports as
// VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, and a RADV timeline semaphore exported as
// VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT and opened by ID3D12Device::OpenSharedHandle as
// an ID3D12Fence. The independent audit of 2026-10-10 found that nothing had measured either of
// them: M802 measured D3D12 against D3D12 and D3D12 against D3D11, which is prerequisite coverage of
// the kernel driver's sharing and not of this pair. These two clients are the smallest thing that
// measures the pair, in both directions, over exactly those handle types.
//
// What they are not: they are not a swapchain, not a window and not a present. They copy a known
// pattern across the boundary and read it back, and they walk a fence across it in both directions.
// A failure here would mean the route cannot work whatever its present code does; a pass here does
// not mean the route presents.
//
// Nothing resident, no window, no GPU of the development PC is required to run --selftest: the host
// mode drives the pure parts (pattern, the fence schedule, the comparison, argument parsing) and
// says what it skipped.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sharecell {

// ---------------------------------------------------------------------------------------------
// The pattern, and what reading it back proves.
//
// Every texel is a function of its own coordinates and of the round, so a readback says more than
// "some bytes arrived": a row stride misread shows as a diagonal, a swapped channel order shows in
// one byte of four, and a stale image from the round before shows as the wrong round constant. The
// WSI route's own import check (LB7A) compares pitches and descriptions; this compares contents.
// ---------------------------------------------------------------------------------------------
inline uint32_t pattern_texel(uint32_t x, uint32_t y, uint32_t round) noexcept
{
    // BGRA, one byte per channel, all four distinct functions.
    const uint32_t b = (x * 7u + round * 29u) & 0xffu;
    const uint32_t g = (y * 11u + round * 53u) & 0xffu;
    const uint32_t r = ((x ^ y) * 13u + round) & 0xffu;
    const uint32_t a = 0xffu;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

inline void pattern_fill(void* rows, uint32_t width, uint32_t height, uint32_t row_pitch,
                         uint32_t round) noexcept
{
    for (uint32_t y = 0; y < height; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rows) + size_t(y) * row_pitch);
        for (uint32_t x = 0; x < width; x++)
            row[x] = pattern_texel(x, y, round);
    }
}

// The first texel that does not match, as a count of mismatches and the coordinates of the first.
struct pattern_diff {
    uint64_t mismatches{};
    uint32_t first_x{}, first_y{};
    uint32_t want{}, have{};
    bool ok() const noexcept { return mismatches == 0; }
};

inline pattern_diff pattern_compare(const void* rows, uint32_t width, uint32_t height,
                                    uint32_t row_pitch, uint32_t round) noexcept
{
    pattern_diff diff{};
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t* row =
            reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(rows) + size_t(y) * row_pitch);
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t want = pattern_texel(x, y, round);
            if (row[x] == want)
                continue;
            if (!diff.mismatches) {
                diff.first_x = x;
                diff.first_y = y;
                diff.want = want;
                diff.have = row[x];
            }
            diff.mismatches++;
        }
    }
    return diff;
}

// ---------------------------------------------------------------------------------------------
// The fence schedule. Odd values belong to one side and even values to the other, so a value that
// arrives on the wrong side is a value that cannot be explained away: the producer can never produce
// the consumer's value and the other way round. Round n is the pair (2n+1, 2n+2).
// ---------------------------------------------------------------------------------------------
inline uint64_t producer_value(uint32_t round) noexcept { return uint64_t(round) * 2u + 1u; }
inline uint64_t consumer_value(uint32_t round) noexcept { return uint64_t(round) * 2u + 2u; }

inline bool schedule_is_producer(uint64_t value) noexcept { return (value & 1u) == 1u; }
inline bool schedule_is_consumer(uint64_t value) noexcept { return value != 0 && (value & 1u) == 0u; }

// A timeline never goes backwards, and each round raises it by exactly two.
inline bool schedule_monotonic(uint32_t rounds) noexcept
{
    uint64_t previous = 0;
    for (uint32_t r = 0; r < rounds; r++) {
        if (producer_value(r) <= previous || !schedule_is_producer(producer_value(r)))
            return false;
        previous = producer_value(r);
        if (consumer_value(r) != previous + 1 || !schedule_is_consumer(consumer_value(r)))
            return false;
        previous = consumer_value(r);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Reporting. One line per step, so a lab record reads as a sequence and a refusal names the call
// that refused. No handle, no pointer and no address is printed.
// ---------------------------------------------------------------------------------------------
inline int failures = 0;
inline int checks = 0;
inline bool negative_control = false;

inline void step(const char* text) { std::printf("STEP %s\n", text); std::fflush(stdout); }

inline bool check(bool condition, const char* what)
{
    checks++;
    const bool pass = condition != negative_control;
    if (!pass) {
        failures++;
        std::printf("FAIL %s\n", what);
    } else {
        std::printf("PASS %s\n", what);
    }
    std::fflush(stdout);
    return pass;
}

inline void report_unsafe_hr(HRESULT hr)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DEVICE_RESET)
        std::printf("UNSAFE D3D12 device failure hr=0x%08lx\n", static_cast<unsigned long>(hr));
}

inline bool check_hr(HRESULT hr, const char* what)
{
    report_unsafe_hr(hr);
    checks++;
    const bool pass = SUCCEEDED(hr) != negative_control;
    if (!pass) {
        failures++;
        std::printf("FAIL %s hr=0x%08lx\n", what, static_cast<unsigned long>(hr));
    } else {
        std::printf("PASS %s hr=0x%08lx\n", what, static_cast<unsigned long>(hr));
    }
    std::fflush(stdout);
    return pass;
}

// Observations do not turn an ordinary enumeration terminator into a failed check.
inline HRESULT observe_hr(HRESULT hr, const char* what)
{
    report_unsafe_hr(hr);
    std::printf("INFO %s hr=0x%08lx\n", what, static_cast<unsigned long>(hr));
    std::fflush(stdout);
    return hr;
}

inline HRESULT observe_device_reason(ID3D12Device* device, const char* what)
{
    const HRESULT reason = device->GetDeviceRemovedReason();
    // Every failed removal reason means loss, including DRIVER_INTERNAL_ERROR and INVALID_CALL.
    // An ordinary API's E_INVALIDARG is different: it is not itself a removal report.
    if (FAILED(reason))
        std::printf("UNSAFE D3D12 removal reason hr=0x%08lx\n", static_cast<unsigned long>(reason));
    return observe_hr(reason, what);
}

inline UINT64 observe_completed(ID3D12Fence* fence, const char* what)
{
    const UINT64 value = fence->GetCompletedValue();
    std::printf("INFO %s completed=%llu hex=0x%016llx removed_sentinel=%u\n", what,
                static_cast<unsigned long long>(value), static_cast<unsigned long long>(value),
                value == UINT64_MAX ? 1u : 0u);
    std::fflush(stdout);
    return value;
}

// Only graphics modules of this client, never other processes. The operator compares these
// paths with the installed-file hashes; a registry entry alone does not prove which DLL loaded.
inline void observe_graphics_modules()
{
    HMODULE modules[512];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes) ||
        bytes > sizeof(modules)) {
        std::printf("INFO graphics module witness unavailable or truncated\n");
        return;
    }
    for (size_t i = 0; i < bytes / sizeof(HMODULE); ++i) {
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(modules[i], path, DWORD(std::size(path)));
        if (!length || length >= std::size(path)) continue;
        const wchar_t* slash = std::wcsrchr(path, L'\\');
        const wchar_t* name = slash ? slash + 1 : path;
        if (_wcsicmp(name, L"vulkan_radeon.dll") && _wcsicmp(name, L"amdgpu_wddm_d3d12.dll") &&
            _wcsicmp(name, L"vulkan-1.dll") && _wcsicmp(name, L"d3d12.dll") &&
            _wcsicmp(name, L"d3d12core.dll") && _wcsicmp(name, L"dxgi.dll")) continue;
        std::printf("INFO graphics module %ls\n", path);
    }
    std::fflush(stdout);
}

inline bool check_vk(VkResult result, const char* what)
{
    if (result == VK_TIMEOUT || result == VK_ERROR_DEVICE_LOST)
        std::printf("UNSAFE Vulkan wait or device failure vk=%d\n", int(result));
    checks++;
    const bool pass = (result == VK_SUCCESS) != negative_control;
    if (!pass) {
        failures++;
        std::printf("FAIL %s vk=%d\n", what, int(result));
    } else {
        std::printf("PASS %s vk=%d\n", what, int(result));
    }
    std::fflush(stdout);
    return pass;
}

// ---------------------------------------------------------------------------------------------
// The two runtimes, loaded by name. Neither client links d3d12.lib or vulkan-1.lib: a development PC
// or a lab machine without one of them must say so instead of failing to start.
// ---------------------------------------------------------------------------------------------
struct d3d12_api {
    HMODULE dxgi{}, d3d12{};
    HRESULT (WINAPI* CreateDXGIFactory2)(UINT, REFIID, void**) {};
    HRESULT (WINAPI* D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**) {};
    HRESULT (WINAPI* D3D12GetDebugInterface)(REFIID, void**) {};

    bool load(std::string* why)
    {
        dxgi = LoadLibraryW(L"dxgi.dll");
        d3d12 = LoadLibraryW(L"d3d12.dll");
        if (!dxgi || !d3d12) {
            *why = "dxgi.dll or d3d12.dll could not be loaded";
            return false;
        }
        CreateDXGIFactory2 = reinterpret_cast<decltype(CreateDXGIFactory2)>(
            reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
        D3D12CreateDevice = reinterpret_cast<decltype(D3D12CreateDevice)>(
            reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
        D3D12GetDebugInterface = reinterpret_cast<decltype(D3D12GetDebugInterface)>(
            reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12GetDebugInterface")));
        if (!CreateDXGIFactory2 || !D3D12CreateDevice) {
            *why = "dxgi.dll or d3d12.dll has no entry point this client needs";
            return false;
        }
        return true;
    }
};

// Only the entry points these clients call. A table, so a missing one is named and not a crash.
struct vk_api {
    HMODULE module{};
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
    PFN_vkCreateInstance CreateInstance{};
    PFN_vkDestroyInstance DestroyInstance{};
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices{};
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2{};
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties{};
    PFN_vkCreateDevice CreateDevice{};
    PFN_vkDestroyDevice DestroyDevice{};
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr{};
    PFN_vkGetDeviceQueue GetDeviceQueue{};
    PFN_vkCreateImage CreateImage{};
    PFN_vkDestroyImage DestroyImage{};
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements{};
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout{};
    PFN_vkBindImageMemory BindImageMemory{};
    PFN_vkCreateBuffer CreateBuffer{};
    PFN_vkDestroyBuffer DestroyBuffer{};
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements{};
    PFN_vkBindBufferMemory BindBufferMemory{};
    PFN_vkAllocateMemory AllocateMemory{};
    PFN_vkFreeMemory FreeMemory{};
    PFN_vkMapMemory MapMemory{};
    PFN_vkUnmapMemory UnmapMemory{};
    PFN_vkCreateCommandPool CreateCommandPool{};
    PFN_vkDestroyCommandPool DestroyCommandPool{};
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers{};
    PFN_vkBeginCommandBuffer BeginCommandBuffer{};
    PFN_vkEndCommandBuffer EndCommandBuffer{};
    PFN_vkResetCommandBuffer ResetCommandBuffer{};
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier{};
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer{};
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage{};
    PFN_vkQueueSubmit QueueSubmit{};
    PFN_vkQueueWaitIdle QueueWaitIdle{};
    PFN_vkDeviceWaitIdle DeviceWaitIdle{};
    PFN_vkCreateSemaphore CreateSemaphore{};
    PFN_vkDestroySemaphore DestroySemaphore{};
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue{};
    PFN_vkWaitSemaphores WaitSemaphores{};
    PFN_vkSignalSemaphore SignalSemaphore{};
    PFN_vkGetSemaphoreWin32HandleKHR GetSemaphoreWin32HandleKHR{};
    PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR{};

    bool load(std::string* why)
    {
        module = LoadLibraryW(L"vulkan-1.dll");
        if (!module) {
            *why = "vulkan-1.dll could not be loaded";
            return false;
        }
        GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            reinterpret_cast<void*>(GetProcAddress(module, "vkGetInstanceProcAddr")));
        if (!GetInstanceProcAddr) {
            *why = "vulkan-1.dll has no vkGetInstanceProcAddr";
            return false;
        }
        CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
            GetInstanceProcAddr(nullptr, "vkCreateInstance"));
        if (!CreateInstance) {
            *why = "the Vulkan loader offers no vkCreateInstance";
            return false;
        }
        return true;
    }

#define SHARECELL_INSTANCE_FN(name)                                                                \
    name = reinterpret_cast<PFN_vk##name>(GetInstanceProcAddr(instance, "vk" #name));              \
    if (!name) { *why = "the Vulkan instance offers no vk" #name; return false; }

    bool load_instance(VkInstance instance, std::string* why)
    {
        SHARECELL_INSTANCE_FN(DestroyInstance)
        SHARECELL_INSTANCE_FN(EnumeratePhysicalDevices)
        SHARECELL_INSTANCE_FN(GetPhysicalDeviceProperties2)
        SHARECELL_INSTANCE_FN(GetPhysicalDeviceMemoryProperties)
        SHARECELL_INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties)
        SHARECELL_INSTANCE_FN(CreateDevice)
        SHARECELL_INSTANCE_FN(GetDeviceProcAddr)
        return true;
    }
#undef SHARECELL_INSTANCE_FN

#define SHARECELL_DEVICE_FN(name)                                                                  \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));                  \
    if (!name) { *why = "the Vulkan device offers no vk" #name; return false; }

    bool load_device(VkDevice device, std::string* why)
    {
        SHARECELL_DEVICE_FN(DestroyDevice)
        SHARECELL_DEVICE_FN(GetDeviceQueue)
        SHARECELL_DEVICE_FN(CreateImage)
        SHARECELL_DEVICE_FN(DestroyImage)
        SHARECELL_DEVICE_FN(GetImageMemoryRequirements)
        SHARECELL_DEVICE_FN(GetImageSubresourceLayout)
        SHARECELL_DEVICE_FN(BindImageMemory)
        SHARECELL_DEVICE_FN(CreateBuffer)
        SHARECELL_DEVICE_FN(DestroyBuffer)
        SHARECELL_DEVICE_FN(GetBufferMemoryRequirements)
        SHARECELL_DEVICE_FN(BindBufferMemory)
        SHARECELL_DEVICE_FN(AllocateMemory)
        SHARECELL_DEVICE_FN(FreeMemory)
        SHARECELL_DEVICE_FN(MapMemory)
        SHARECELL_DEVICE_FN(UnmapMemory)
        SHARECELL_DEVICE_FN(CreateCommandPool)
        SHARECELL_DEVICE_FN(DestroyCommandPool)
        SHARECELL_DEVICE_FN(AllocateCommandBuffers)
        SHARECELL_DEVICE_FN(BeginCommandBuffer)
        SHARECELL_DEVICE_FN(EndCommandBuffer)
        SHARECELL_DEVICE_FN(ResetCommandBuffer)
        SHARECELL_DEVICE_FN(CmdPipelineBarrier)
        SHARECELL_DEVICE_FN(CmdCopyImageToBuffer)
        SHARECELL_DEVICE_FN(CmdCopyBufferToImage)
        SHARECELL_DEVICE_FN(QueueSubmit)
        SHARECELL_DEVICE_FN(QueueWaitIdle)
        SHARECELL_DEVICE_FN(DeviceWaitIdle)
        SHARECELL_DEVICE_FN(CreateSemaphore)
        SHARECELL_DEVICE_FN(DestroySemaphore)
        SHARECELL_DEVICE_FN(GetSemaphoreCounterValue)
        SHARECELL_DEVICE_FN(WaitSemaphores)
        SHARECELL_DEVICE_FN(SignalSemaphore)
        SHARECELL_DEVICE_FN(GetSemaphoreWin32HandleKHR)
        ImportSemaphoreWin32HandleKHR = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            GetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        return true;
    }
#undef SHARECELL_DEVICE_FN
};

// ---------------------------------------------------------------------------------------------
// Options, shared by both clients.
// ---------------------------------------------------------------------------------------------
enum class fence_origin { radv, d3d12 };
struct options {
    fence_origin origin = fence_origin::radv;
    uint32_t width = 64;
    uint32_t height = 64;
    uint32_t rounds = 3;
    uint32_t adapter = UINT32_MAX;   // UINT32_MAX: the adapter whose LUID matches the Vulkan device
    bool selftest = false;           // the host mode: pure rules only, no device
    bool negative_control = false;   // every expectation inverted; every case must fail
    bool help = false;
    bool bad_option = false;
    std::string bad;
};

inline bool parse_u32(const char* text, uint32_t* out)
{
    if (!text || !*text)
        return false;
    uint64_t value = 0;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10u + uint64_t(*p - '0');
        if (value > 0xffffffffull)
            return false;
    }
    *out = uint32_t(value);
    return true;
}

inline options parse_options(int argc, char** argv)
{
    options o{};
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (arg == "--help" || arg == "-h") {
            o.help = true;
        } else if (arg == "--selftest") {
            o.selftest = true;
        } else if (arg == "--negative-control") {
            o.negative_control = true;
        } else if (arg == "--fence-origin" && next &&
                   (std::strcmp(next, "radv") == 0 || std::strcmp(next, "d3d12") == 0)) {
            o.origin = std::strcmp(next, "d3d12") == 0 ? fence_origin::d3d12 : fence_origin::radv;
            i++;
        } else if (arg == "--width" && parse_u32(next, &o.width)) {
            i++;
        } else if (arg == "--height" && parse_u32(next, &o.height)) {
            i++;
        } else if (arg == "--rounds" && parse_u32(next, &o.rounds)) {
            i++;
        } else if (arg == "--adapter" && parse_u32(next, &o.adapter)) {
            i++;
        } else {
            o.bad_option = true;
            o.bad = arg;
            break;
        }
    }
    if (!o.bad_option && (o.width == 0 || o.height == 0 || o.rounds == 0)) {
        o.bad_option = true;
        o.bad = "a width, a height and a round count must all be above zero";
    }
    return o;
}

// Vulkan imports a reference, not ownership of the application's NT handle. Close it on either
// result. Callbacks let the host control exercise this exact path without opening any real handle.
template<typename Import, typename Close>
inline VkResult import_application_fence(VkSemaphore semaphore, HANDLE handle,
                                         Import import, Close close)
{
    VkImportSemaphoreWin32HandleInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    info.semaphore = semaphore;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    info.handle = handle;
    // A timeline uses a permanent import (flags == 0); a handle import has no name.
    const VkResult result = import(&info);
    close(handle);
    return result;
}

inline bool observe_fence_flags(ID3D12Fence* fence, const char* label)
{
    ID3D12Fence1* fence1 = nullptr;
    const HRESULT hr = fence->QueryInterface(IID_PPV_ARGS(&fence1));
    check_hr(hr, label);
    if (SUCCEEDED(hr)) {
        std::printf("INFO %s creation_flags=0x%08x\n", label,
                    static_cast<unsigned>(fence1->GetCreationFlags()));
        const bool expected = fence1->GetCreationFlags() == D3D12_FENCE_FLAG_SHARED;
        check(expected, "the D3D12-origin fence reports SHARED without NON_MONITORED");
        fence1->Release();
        return expected;
    }
    return false;
}

inline bool create_d3d12_origin(vk_api& api, VkDevice vk_device, ID3D12Device* device,
                                VkSemaphore* timeline, ID3D12Fence** shared)
{
    check(api.ImportSemaphoreWin32HandleKHR != nullptr,
          "vkImportSemaphoreWin32HandleKHR is available");
    if (!api.ImportSemaphoreWin32HandleKHR)
        return false;
    ID3D12Fence* created = nullptr;
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&created));
    check_hr(hr, "D3D12 CreateFence(initial=0, SHARED)");
    if (FAILED(hr))
        return false;
    const bool created_flags = observe_fence_flags(created, "D3D12 created fence");
    const bool created_zero = observe_completed(created, "D3D12 created initial fence") == 0;
    check(created_zero, "the created D3D12 fence reads zero");
    if (!created_flags || !created_zero) {
        created->Release();
        return false;
    }
    HANDLE handle = nullptr;
    hr = device->CreateSharedHandle(created, nullptr, GENERIC_ALL, nullptr, &handle);
    check_hr(hr, "D3D12 CreateSharedHandle(fence, GENERIC_ALL)");
    if (FAILED(hr)) {
        created->Release();
        return false;
    }
    // Independent D3D12 -> D3D12 control, before any Vulkan import, on the same device.
    if (FAILED(observe_device_reason(device, "device before D3D12-origin reopen"))) {
        CloseHandle(handle);
        created->Release();
        return false;
    }
    hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(shared));
    check_hr(hr, "D3D12 reopened its own exported fence on the same device");
    const HRESULT reason = observe_device_reason(device, "device after D3D12-origin reopen");
    if (FAILED(hr) || FAILED(reason)) {
        CloseHandle(handle);
        created->Release();
        return false;
    }
    const bool opened_flags = observe_fence_flags(*shared, "D3D12 reopened fence before Vulkan import");
    const bool opened_zero = observe_completed(*shared, "D3D12 reopened initial fence before Vulkan import") == 0;
    check(opened_zero, "the reopened D3D12 fence reads zero before Vulkan import");
    if (!opened_flags || !opened_zero) {
        CloseHandle(handle);
        created->Release();
        return false;
    }

    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    VkResult result = api.CreateSemaphore(vk_device, &info, nullptr, timeline);
    check_vk(result, "RADV created a timeline for permanent D3D12-fence import");
    if (result != VK_SUCCESS) {
        CloseHandle(handle);
        created->Release();
        return false;
    }
    result = import_application_fence(*timeline, handle,
        [&](const VkImportSemaphoreWin32HandleInfoKHR* import_info) {
            return api.ImportSemaphoreWin32HandleKHR(vk_device, import_info);
        }, [](HANDLE owned) { CloseHandle(owned); });
    check_vk(result, "RADV permanently imported the D3D12-created fence");
    created->Release(); // The reopened fence and imported semaphore retain their own references.
    if (result != VK_SUCCESS)
        return false;
    uint64_t value = UINT64_MAX;
    result = api.GetSemaphoreCounterValue(vk_device, *timeline, &value);
    check_vk(result, "RADV reads the imported D3D12-origin timeline");
    std::printf("INFO imported D3D12-origin timeline value=%llu\n",
                static_cast<unsigned long long>(value));
    check(value == 0, "the imported D3D12-origin timeline reads zero");
    return result == VK_SUCCESS && value == 0 &&
           SUCCEEDED(observe_device_reason(device, "device after D3D12-origin import"));
}

// The pure rules, driven with no device at all. Both clients run this in --selftest and before every
// GPU run, so a machine with no Vulkan driver still gates the parts that do not need one.
inline int run_selftest(const options& o)
{
    negative_control = o.negative_control;
    step("selftest: the pattern, the fence schedule and the comparison, with no device");

    std::vector<uint32_t> rows(size_t(o.width) * o.height);
    const uint32_t pitch = o.width * 4u;
    pattern_fill(rows.data(), o.width, o.height, pitch, 0);
    check(pattern_compare(rows.data(), o.width, o.height, pitch, 0).ok(),
          "the pattern compares equal to itself");

    // A round constant that does not match is a stale image, which is what image reuse must not do.
    check(!pattern_compare(rows.data(), o.width, o.height, pitch, 1).ok(),
          "the pattern of round 0 does not compare equal to round 1");

    // One texel displaced by one byte: the channel order is part of what is being measured.
    if (rows.size() > 1) {
        const uint32_t keep = rows[1];
        rows[1] = keep ^ 0x00000001u;
        const pattern_diff diff = pattern_compare(rows.data(), o.width, o.height, pitch, 0);
        check(diff.mismatches == 1 && diff.first_x == 1 && diff.first_y == 0,
              "one wrong texel is found, and its coordinates are reported");
        rows[1] = keep;
    }

    // A row-pitch misread shows up: the same bytes read with a pitch one texel too long do not
    // compare equal, which is the failure a shared linear surface with the wrong stride produces.
    if (o.height > 1 && o.width > 1) {
        std::vector<uint32_t> padded(size_t(o.width + 1) * o.height, 0u);
        pattern_fill(padded.data(), o.width, o.height, (o.width + 1) * 4u, 0);
        check(pattern_compare(padded.data(), o.width, o.height, (o.width + 1) * 4u, 0).ok(),
              "the pattern compares equal when written and read with the same padded pitch");
        check(!pattern_compare(padded.data(), o.width, o.height, pitch, 0).ok(),
              "the padded pattern read with the unpadded pitch does not compare equal");
    }

    check(schedule_monotonic(o.rounds), "the fence schedule rises by two per round and never repeats");
    check(producer_value(0) == 1 && consumer_value(0) == 2, "round 0 is the pair (1, 2)");
    check(schedule_is_producer(producer_value(o.rounds - 1)),
          "the producer's value of the last round is odd");
    check(schedule_is_consumer(consumer_value(o.rounds - 1)),
          "the consumer's value of the last round is even");
    check(!schedule_is_producer(consumer_value(0)) && !schedule_is_consumer(producer_value(0)),
          "neither side can produce the other side's value");
    check(!schedule_is_consumer(0), "a timeline that never moved belongs to neither side");

    {
        char executable[] = "selftest";
        char option[] = "--fence-origin";
        char radv[] = "radv";
        char d3d12[] = "d3d12";
        char invalid[] = "invalid";
        char* args[] = {executable, option, d3d12};
        check(parse_options(1, args).origin == fence_origin::radv,
              "the default fence origin preserves the RADV baseline");
        check(parse_options(3, args).origin == fence_origin::d3d12 &&
              !parse_options(3, args).bad_option, "the D3D12 origin is selectable");
        args[2] = radv;
        check(parse_options(3, args).origin == fence_origin::radv &&
              !parse_options(3, args).bad_option, "the RADV origin is selectable");
        args[2] = invalid;
        check(parse_options(3, args).bad_option && parse_options(2, args).bad_option,
              "unknown and missing origins are refused");
        for (VkResult expected : {VK_SUCCESS, VK_ERROR_INVALID_EXTERNAL_HANDLE}) {
            int imports = 0, closes = 0;
            bool descriptor_ok = false;
            const HANDLE token = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234));
            const VkSemaphore semaphore = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x5678));
            const VkResult actual = import_application_fence(semaphore, token,
                [&](const VkImportSemaphoreWin32HandleInfoKHR* info) {
                    imports++;
                    descriptor_ok = info->sType == VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR &&
                        info->pNext == nullptr && info->semaphore == semaphore && info->flags == 0 &&
                        info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT &&
                        info->handle == token && info->name == nullptr && closes == 0;
                    return expected;
                }, [&](HANDLE handle) { if (handle == token && imports == 1) closes++; });
            check(descriptor_ok && imports == 1, "permanent D3D12 import uses the exact caller handle and semaphore");
            check(actual == expected && closes == 1,
                  "import result is preserved and caller handle closes once after success or failure");
        }
    }
    std::printf("sharecell selftest: %d checks, %d failed%s\n", checks, failures,
                o.negative_control ? " (negative control)" : "");
    return failures ? 1 : 0;
}

// The Vulkan physical device whose LUID the DXGI adapter must match, so that the two stacks are on
// the same GPU. Returns false when the device does not report a LUID, which is a refusal and not a
// reason to share across adapters.
inline bool physical_device_luid(vk_api& vk, VkPhysicalDevice physical, LUID* out_luid,
                                 std::string* name)
{
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    vk.GetPhysicalDeviceProperties2(physical, &properties);
    if (name)
        *name = properties.properties.deviceName;
    if (!id.deviceLUIDValid)
        return false;
    std::memcpy(out_luid, id.deviceLUID, sizeof(*out_luid));
    return true;
}

inline uint32_t memory_type_index(const VkPhysicalDeviceMemoryProperties& props, uint32_t bits,
                                  VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        if ((props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

// A queue family that can copy. Every family that supports graphics or compute supports transfer
// implicitly, and the explicit transfer bit is also accepted.
inline uint32_t transfer_queue_family(vk_api& vk, VkPhysicalDevice physical)
{
    uint32_t count = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk.GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count; i++) {
        const VkQueueFlags flags = families[i].queueFlags;
        if (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
            return i;
    }
    return UINT32_MAX;
}

inline const char* const kUsage =
    "  --width N --height N     the shared texture's size (default 64x64)\n"
    "  --rounds N               fence round trips and image reuses (default 3)\n"
    "  --fence-origin radv|d3d12 the shared fence creator (default radv)\n"
    "  --adapter N              the DXGI adapter index; the default matches the Vulkan device's LUID\n"
    "  --selftest               the pure rules only: no Vulkan device, no D3D12 device\n"
    "  --negative-control       every expectation inverted; every case must then fail\n"
    "  --help                   this text\n"
    "\n"
    "Exit: 0 every case passed, 1 a case failed, 2 the arguments were wrong, 3 this machine offers\n"
    "no device of one of the two stacks (which is a skip and not a failure).\n";

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNoDevice = 3;

}  // namespace sharecell
