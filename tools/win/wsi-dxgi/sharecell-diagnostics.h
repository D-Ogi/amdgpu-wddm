// SPDX-License-Identifier: MIT
#pragma once
#include <winternl.h>
#include <d3dkmthk.h>
#include <bcrypt.h>
#include <cstddef>

namespace sharecell {
static_assert(sizeof(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2) == 0x58);
static_assert(offsetof(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2, Flags) == 0xc);
static_assert(offsetof(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2, MonitoredFence.FenceValueCPUVirtualAddress) == 0x18);
static_assert(offsetof(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2, MonitoredFence.FenceValueGPUVirtualAddress) == 0x20);

// Diagnostic only: force every flag value, irrespective of status. This is NOT the runtime's
// conditional retry policy. The NT handle is borrowed; successful KMT handles are owned here.
template<typename Open, typename Destroy>
inline bool replay_fence_patterns(HANDLE nt, D3DKMT_HANDLE device, Open open, Destroy destroy, bool quiet = false)
{
    D3DKMT_HANDLE owned[6]{};
    unsigned count = 0;
    bool complete = true;
    // SDK d3dukmdt.h: 0x80 NoGPUAccess; 0x400 UnwaitCpuWaitersOnlyOnDestroy.
    const UINT flags[] = {0x483, 0x83, 0x3};
    if (!quiet) std::printf("INFO KMT forced diagnostic sequences (not conditional runtime retries)\n");
    for (unsigned pattern = 0; pattern != 2; ++pattern) {
        D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 args{};
        args.hNtHandle = nt;
        args.hDevice = device;
        for (unsigned i = 0; i != 3; ++i) {
            if (pattern == 0) {
                args = {};
                args.hNtHandle = nt;
                args.hDevice = device;
            }
            args.Flags.Value = flags[i];
            auto fields = [&](const char* phase, NTSTATUS status) {
                if (!quiet) std::printf("INFO KMT pattern=%s attempt=%u phase=%s requested_flags=0x%08x flags=0x%08x "
                            "nt=%p device=0x%08x affinity=0x%08x status=0x%08lx sync=0x%08x cpu=%p gpu=0x%016llx\n",
                            pattern ? "reused" : "fresh", i, phase, flags[i], args.Flags.Value,
                            args.hNtHandle, args.hDevice, args.MonitoredFence.EngineAffinity,
                            static_cast<unsigned long>(status), args.hSyncObject,
                            args.MonitoredFence.FenceValueCPUVirtualAddress,
                            static_cast<unsigned long long>(args.MonitoredFence.FenceValueGPUVirtualAddress));
            };
            fields("before-status-not-yet-returned", 0);
            const NTSTATUS status = open(&args);
            fields("after", status);
            if (status >= 0) {
                bool duplicate = false;
                for (unsigned n = 0; n != count; ++n) duplicate |= owned[n] == args.hSyncObject;
                if (!args.hSyncObject || duplicate) {
                    if (!quiet) std::printf("UNSAFE KMT successful open returned zero or already-owned handle; ownership ambiguous, not proof of driver defect\n");
                    complete = false;
                } else {
                    owned[count++] = args.hSyncObject;
                }
            }
        }
    }
    // Keep every success alive through both patterns: failure output can still contain that handle.
    for (unsigned i = 0; i != count; ++i) {
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT args{};
        args.hSyncObject = owned[i];
        const NTSTATUS status = destroy(&args);
        if (!quiet) std::printf("INFO KMT destroy sync=0x%08x status=0x%08lx\n", owned[i], static_cast<unsigned long>(status));
        if (status < 0) {
            if (!quiet) std::printf("UNSAFE KMT synchronization object cleanup unconfirmed\n");
            complete = false;
        }
    }
    return complete;
}

static_assert(sizeof(D3DKMT_ISFEATUREENABLED) == 12);
static_assert(offsetof(D3DKMT_ISFEATUREENABLED, Result) == 8);
static_assert(DXGK_FEATURE_NATIVE_FENCE == 37);
struct native_feature_observation {
    bool available = false, valid = false;
    NTSTATUS status = 0;
    DXGK_ISFEATUREENABLED_RESULT result{};
};

inline native_feature_observation observe_native_feature(D3DKMT_HANDLE adapter,
    PFND3DKMT_ISFEATUREENABLED query, bool quiet = false)
{
    native_feature_observation observed{};
    if (!query) {
        if (!quiet) std::printf("INFO KMT native-fence feature=37 query=unavailable result_valid=0 (not disabled)\n");
        return observed;
    }
    D3DKMT_ISFEATUREENABLED args{};
    args.hAdapter = adapter;
    args.FeatureId = DXGK_FEATURE_NATIVE_FENCE;
    observed.available = true;
    observed.status = query(&args);
    observed.valid = observed.status >= 0;
    observed.result = args.Result; // Retain dirty failure output for diagnosis, but never mark it valid.
    if (!quiet) std::printf("INFO KMT native-fence adapter=0x%08x feature=37 status=0x%08lx result_valid=%u "
                            "Version=%u Value=0x%04x Enabled=%u KnownFeature=%u SupportedByDriver=%u "
                            "SupportedOnCurrentConfig=%u Reserved=0x%03x\n",
                            adapter, static_cast<unsigned long>(observed.status), unsigned(observed.valid),
                            unsigned(args.Result.Version), unsigned(args.Result.Value), unsigned(args.Result.Enabled),
                            unsigned(args.Result.KnownFeature), unsigned(args.Result.SupportedByDriver),
                            unsigned(args.Result.SupportedOnCurrentConfig), unsigned(args.Result.Reserved));
    return observed;
}
template<typename Api>
inline bool native_feature_on_adapter(LUID luid, Api& api, bool quiet = false)
{
    if (!api.query) { (void)observe_native_feature(0, nullptr, quiet); return true; }
    D3DKMT_OPENADAPTERFROMLUID adapter{};
    adapter.AdapterLuid = luid;
    const NTSTATUS opened = api.open(&adapter);
    if (!quiet) std::printf("INFO KMT native-feature adapter-open same_d3d12_luid=%08lx:%08lx status=0x%08lx adapter=0x%08x\n",
        static_cast<unsigned long>(luid.HighPart), luid.LowPart, static_cast<unsigned long>(opened), adapter.hAdapter);
    if (opened < 0) {
        if (!quiet) std::printf("INFO KMT native-fence feature=37 query=adapter-unavailable result_valid=0\n");
        return true;
    }
    if (!adapter.hAdapter) {
        if (!quiet) std::printf("UNSAFE KMT feature adapter success without handle; cleanup unconfirmed\n");
        return false;
    }
    (void)observe_native_feature(adapter.hAdapter, api.query, quiet);
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = adapter.hAdapter;
    const NTSTATUS closed = api.close(&close);
    if (!quiet) std::printf("INFO KMT native-feature adapter-close status=0x%08lx\n", static_cast<unsigned long>(closed));
    if (closed < 0 && !quiet) std::printf("UNSAFE KMT feature adapter cleanup unconfirmed\n");
    return closed >= 0;
}

inline bool probe_native_feature(LUID luid)
{
    HMODULE module = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        std::printf("INFO KMT native-fence feature=37 query=module-unavailable result_valid=0 error=%lu\n", GetLastError());
        return true;
    }
    struct {
        PFND3DKMT_OPENADAPTERFROMLUID open;
        PFND3DKMT_CLOSEADAPTER close;
        PFND3DKMT_ISFEATUREENABLED query;
    } api{reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(module, "D3DKMTOpenAdapterFromLuid")),
          reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(module, "D3DKMTCloseAdapter")),
          reinterpret_cast<PFND3DKMT_ISFEATUREENABLED>(GetProcAddress(module, "D3DKMTIsFeatureEnabled"))};
    bool ok = true;
    if (!api.open || !api.close)
        std::printf("INFO KMT native-fence feature=37 query=adapter-api-unavailable result_valid=0\n");
    else
        ok = native_feature_on_adapter(luid, api);
    FreeLibrary(module);
    return ok;
}
template<typename Api>
inline bool replay_on_adapter(HANDLE nt, LUID luid, Api& api, bool quiet = false)
{
    D3DKMT_OPENADAPTERFROMLUID adapter{};
    adapter.AdapterLuid = luid;
    NTSTATUS status = api.OpenAdapterFromLuid(&adapter);
    if (!quiet) std::printf("INFO KMT OpenAdapterFromLuid same_d3d12_luid=%08lx:%08lx status=0x%08lx adapter=0x%08x\n",
                static_cast<unsigned long>(luid.HighPart), luid.LowPart, static_cast<unsigned long>(status), adapter.hAdapter);
    if (status < 0) { return false; }
    if (!adapter.hAdapter) {
        if (!quiet) std::printf("UNSAFE KMT adapter success without handle; cleanup unconfirmed\n");
        return false;
    }
    D3DKMT_CREATEDEVICE device{};
    device.hAdapter = adapter.hAdapter;
    status = api.CreateDevice(&device);
    if (!quiet) std::printf("INFO KMT CreateDevice status=0x%08lx device=0x%08x (separate diagnostic device)\n",
                static_cast<unsigned long>(status), device.hDevice);
    bool ok = status >= 0 && device.hDevice != 0;
    if (status >= 0 && !device.hDevice)
        if (!quiet) std::printf("UNSAFE KMT device success without handle; cleanup unconfirmed\n");
    if (ok) {
        ok = replay_fence_patterns(nt, device.hDevice,
            [&](auto* args) { return api.OpenSyncObjectFromNtHandle2(args); },
            [&](auto* args) { return api.DestroySynchronizationObject(args); }, quiet);
        D3DKMT_DESTROYDEVICE destroy{};
        destroy.hDevice = device.hDevice;
        status = api.DestroyDevice(&destroy);
        if (!quiet) std::printf("INFO KMT DestroyDevice status=0x%08lx\n", static_cast<unsigned long>(status));
        if (status < 0) if (!quiet) std::printf("UNSAFE KMT device cleanup unconfirmed\n");
        ok &= status >= 0;
    }
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = adapter.hAdapter;
    status = api.CloseAdapter(&close);
    if (!quiet) std::printf("INFO KMT CloseAdapter status=0x%08lx\n", static_cast<unsigned long>(status));
    if (status < 0) if (!quiet) std::printf("UNSAFE KMT adapter cleanup unconfirmed\n");
    ok &= status >= 0;
    return ok;
}

inline bool replay_exported_fence(HANDLE nt, LUID luid)
{
    HMODULE module = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { std::printf("FAIL KMT load gdi32 error=%lu\n", GetLastError()); return false; }
#define KMT_PROC(name, type) const auto name = reinterpret_cast<type>(GetProcAddress(module, "D3DKMT" #name))
    KMT_PROC(OpenAdapterFromLuid, PFND3DKMT_OPENADAPTERFROMLUID);
    KMT_PROC(CreateDevice, PFND3DKMT_CREATEDEVICE);
    KMT_PROC(OpenSyncObjectFromNtHandle2, PFND3DKMT_OPENSYNCOBJECTFROMNTHANDLE2);
    KMT_PROC(DestroySynchronizationObject, PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT);
    KMT_PROC(DestroyDevice, PFND3DKMT_DESTROYDEVICE);
    KMT_PROC(CloseAdapter, PFND3DKMT_CLOSEADAPTER);
#undef KMT_PROC
    if (!OpenAdapterFromLuid || !CreateDevice || !OpenSyncObjectFromNtHandle2 ||
        !DestroySynchronizationObject || !DestroyDevice || !CloseAdapter) {
        std::printf("FAIL KMT required export absent\n"); FreeLibrary(module); return false;
    }
    struct api_table {
        PFND3DKMT_OPENADAPTERFROMLUID OpenAdapterFromLuid;
        PFND3DKMT_CREATEDEVICE CreateDevice;
        PFND3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 OpenSyncObjectFromNtHandle2;
        PFND3DKMT_DESTROYSYNCHRONIZATIONOBJECT DestroySynchronizationObject;
        PFND3DKMT_DESTROYDEVICE DestroyDevice;
        PFND3DKMT_CLOSEADAPTER CloseAdapter;
    } api{OpenAdapterFromLuid, CreateDevice, OpenSyncObjectFromNtHandle2,
          DestroySynchronizationObject, DestroyDevice, CloseAdapter};
    const bool ok = replay_on_adapter(nt, luid, api);
    FreeLibrary(module);
    return ok;
}

struct sha256_state {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    ~sha256_state() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    bool start() {
        return BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
               BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    bool add(const void* bytes, ULONG size) { return BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(bytes)), size, 0) >= 0; }
    bool finish(UCHAR (&digest)[32]) { return BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0; }
};

inline bool path_to_utf8(const wchar_t* path, std::string* out)
{
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > 4 * 32768) return false;
    out->resize(size);
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, out->data(), size, nullptr, nullptr) != size) return false;
    out->pop_back();
    return true;
}
inline void observe_loaded_core(const char* stage)
{
    const HMODULE module = GetModuleHandleW(L"d3d12core.dll");
    if (!module) { std::printf("INFO loaded-core stage=%s identity=absent (not loaded for this probe)\n", stage); return; }
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) {
        std::printf("INFO loaded-core stage=%s identity=unavailable path unavailable error=%lu\n", stage, GetLastError()); return;
    }
    std::string utf8;
    if (!path_to_utf8(path, &utf8)) {
        std::printf("INFO loaded-core stage=%s identity=unavailable UTF8 conversion failed\n", stage); return;
    }
    std::printf("INFO loaded-core stage=%s path_utf8=%s\n", stage, utf8.c_str());
    // Deny concurrent file writers while hashing the backing file. This is not a loaded-memory hash.
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::printf("INFO loaded-core identity=unavailable SHA256 unavailable error=%lu\n", GetLastError()); return; }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 128 * 1024 * 1024;
    sha256_state hash;
    if (ok) ok = hash.start();
    unsigned char buffer[65536];
    LONGLONG remaining = size.QuadPart;
    while (ok && remaining > 0) {
        const DWORD want = static_cast<DWORD>(remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        DWORD got = 0;
        ok = ReadFile(file, buffer, want, &got, nullptr) && got == want && hash.add(buffer, got);
        remaining -= got;
    }
    UCHAR digest[32]{};
    if (ok) ok = hash.finish(digest);
    CloseHandle(file);
    if (!ok) { std::printf("INFO loaded-core identity=unavailable SHA256 unavailable (read/hash failed or size outside bound)\n"); return; }
    std::printf("INFO loaded-core stage=%s identity=ok backing-file-bytes=%llu SHA256=", stage, static_cast<unsigned long long>(size.QuadPart));
    for (UCHAR byte : digest) std::printf("%02X", byte);
    std::printf("\n");
}
} // namespace sharecell
