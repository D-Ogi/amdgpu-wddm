// SPDX-License-Identifier: MIT
// Round trip: the engine's disk shader cache in the inline queue mode. The engine keeps vkd3d-proton's stream archive
// in %LOCALAPPDATA%\amdgpu-wddm\vkd3d (the harness points LOCALAPPDATA beside itself, harness.cpp) and, in INLINE,
// runs it without a thread: the archive is parsed during CreateDevice and each new pipeline is appended by the thread
// that created it. Four threads create compute pipelines on the engine device at once, 8 each, every one with its
// own root signature, so every one is a new archive entry on a cold cache (and a hit on a warm one). Then:
//   - no thread of this process starts inside the engine DLL (vkd3d-proton's threads start at its own wrapper, so a
//     disk cache, fence or submission thread would be one);
//   - the archives on disk are whole: every entry's checksum holds and the entries end exactly at the end of the file,
//     which a torn append from two threads would break; together they hold at least the 32 pipelines.
// An engine without the per-user cache (ABI 1.3 r5 and older) creates no directory; the archive checks then SKIP.
#include "harness.h"
#include "fixture-cs.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>
#include <unordered_set>

namespace harness {

namespace {
constexpr int kThreads = 4;
constexpr int kPerThread = 8;

// vkd3d-proton's stream archive (libs/vkd3d/cache.c): a 48-byte header ("VKS" 4), then entries of a 24-byte header
// (hash, checksum, size, type) and size bytes padded to 8. The checksum is FNV-1a over the data, then hash, size, type.
constexpr uint32_t kArchiveMagic = 'V' | ('K' << 8) | ('S' << 16) | (4u << 24);
constexpr size_t kArchiveHeader = 48;
constexpr size_t kEntryHeader = 24;
constexpr uint32_t kEntryPipeline = 2;

uint64_t fnv(uint64_t h, uint32_t v) { return (h * 0x100000001b3ull) ^ v; }

struct Archive {
    bool present = false;
    bool whole = false;
    size_t bytes = 0;
    size_t entries = 0;
};

Archive walk_archive(const std::wstring& path, std::unordered_set<uint64_t>& pipelines) {
    Archive a;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return a;
    a.present = true;
    std::vector<uint8_t> data;
    LARGE_INTEGER size{};
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (INT64{1} << 30)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        if (!data.empty() && !ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
    }
    CloseHandle(f);
    a.bytes = got;
    uint32_t magic = 0;
    if (got != data.size() || got < kArchiveHeader || (std::memcpy(&magic, data.data(), 4), magic != kArchiveMagic))
        return a;
    size_t off = kArchiveHeader;
    while (off + kEntryHeader <= data.size()) {
        uint64_t hash, checksum;
        uint32_t bytes, type;
        std::memcpy(&hash, &data[off], 8);
        std::memcpy(&checksum, &data[off + 8], 8);
        std::memcpy(&bytes, &data[off + 16], 4);
        std::memcpy(&type, &data[off + 20], 4);
        const size_t padded = (size_t{bytes} + 7) & ~size_t{7};
        if (off + kEntryHeader + padded > data.size()) return a;
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < bytes; ++i) h = fnv(h, data[off + kEntryHeader + i]);
        h = fnv(fnv(h, static_cast<uint32_t>(hash)), static_cast<uint32_t>(hash >> 32));
        h = fnv(fnv(h, bytes), type);
        if (h != checksum) return a;
        if (type == kEntryPipeline) pipelines.insert(hash);
        ++a.entries;
        off += kEntryHeader + padded;
    }
    a.whole = off == data.size();
    return a;
}

// Threads of this process whose Win32 start address lies inside module.
int threads_started_in(HMODULE module) {
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    uintptr_t base = 0, end = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return -1;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (me.hModule == module) {
            base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
            end = base + me.modBaseSize;
        }
    }
    CloseHandle(snap);
    using QueryThread = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    auto query = reinterpret_cast<QueryThread>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    if (!base || !query) return -1;
    constexpr int kThreadQuerySetWin32StartAddress = 9;
    int found = 0, unread = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        // The start address needs THREAD_QUERY_INFORMATION; the limited right reads nothing.
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        uintptr_t start = 0;
        if (!t || query(t, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) < 0)
            ++unread;
        else if (start >= base && start < end)
            ++found;
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
    return unread ? -1 : found;
}

// Compute pipelines of the fixture shader, each over "DescriptorTable(UAV(u0)), RootConstants(n, b0)" with its own n.
HRESULT create_pipelines(ID3D12Device* engine, int first_constants) {
    const D3D12DDI_DESCRIPTOR_RANGE_0013 range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
                                               D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE,
                                               D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    for (int i = 0; i < kPerThread; ++i) {
        D3D12DDI_ROOT_PARAMETER_0013 params[2]{};
        params[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range};
        params[0].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, static_cast<UINT>(first_constants + i)};
        params[1].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
        const D3D12DDI_ROOT_SIGNATURE_0013 rs{2, params, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
        std::vector<uint8_t> blob;
        HRESULT hr = engine_ddi::serialize_root_signature(&rs, blob);
        ID3D12RootSignature* root = nullptr;
        if (SUCCEEDED(hr))
            hr = engine->CreateRootSignature(0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature),
                                             reinterpret_cast<void**>(&root));
        if (FAILED(hr)) return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.CS = {g_engine_test_cs, sizeof(g_engine_test_cs)};
        ID3D12PipelineState* pso = nullptr;
        hr = engine->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
        if (pso) pso->Release();
        root->Release();
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}
} // namespace

void test_disk_cache(Env& env, const BC250_VKD3D_DEVICE_CREATE_INFO& create, HMODULE engine_dll) {
    HRESULT results[kThreads];
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&env, &results, t] { results[t] = create_pipelines(env.engine, 1 + t * kPerThread); });
    for (std::thread& t : threads) t.join();
    bool created = true;
    for (HRESULT hr : results) created = created && hr == S_OK;
    checkf(created, "disk cache: %d threads create %d compute pipelines each, with distinct root signatures, at once "
                    "(hr %08lx %08lx %08lx %08lx)",
           kThreads, kPerThread, static_cast<unsigned long>(results[0]), static_cast<unsigned long>(results[1]),
           static_cast<unsigned long>(results[2]), static_cast<unsigned long>(results[3]));

    const int engine_threads = threads_started_in(engine_dll);
    checkf(engine_threads == 0, "disk cache: no thread of this process starts in the engine DLL (%d)", engine_threads);

    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    const DWORD m = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring cache_dir = std::wstring(n && n < MAX_PATH ? dir : L"") + L"\\amdgpu-wddm\\vkd3d";
    if (!n || n >= MAX_PATH || !m || GetFileAttributesW(cache_dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("SKIP  disk cache: no %ls; an engine without the per-user disk cache\n", cache_dir.c_str());
    } else {
        const wchar_t* name = std::wcsrchr(exe, L'\\');
        const std::wstring read = cache_dir + L"\\vkd3d-proton." + (name ? name + 1 : exe) + L".cache";
        std::unordered_set<uint64_t> pipelines;
        const Archive r = walk_archive(read, pipelines);
        const Archive w = walk_archive(read + L".write", pipelines);
        checkf((r.present || w.present) && (!r.present || r.whole) && (!w.present || w.whole) &&
                   pipelines.size() >= size_t{kThreads} * kPerThread,
               "disk cache: the archives are whole, every entry's checksum holds, and they hold the pipelines (read "
               "%s, %zu bytes, %zu entries; write %s, %zu bytes, %zu entries; %zu pipelines)",
               r.present ? (r.whole ? "whole" : "TORN") : "absent", r.bytes, r.entries,
               w.present ? (w.whole ? "whole" : "TORN") : "absent", w.bytes, w.entries, pipelines.size());
    }

    // Positive control of the thread count: a THREADED device of the same engine starts its fence workers and
    // submission threads, which the count must see. Last, because its disk cache opens the same archives.
    BC250_VKD3D_DEVICE_CREATE_INFO threaded = create;
    threaded.QueueMode = BC250_VKD3D_QUEUE_MODE_THREADED;
    threaded.Services = nullptr;
    ID3D12Device* device = nullptr;
    const HRESULT hr = env.funcs.CreateDevice(&threaded, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    const int threaded_threads = SUCCEEDED(hr) ? threads_started_in(engine_dll) : -1;
    if (device) device->Release();
    checkf(SUCCEEDED(hr) && threaded_threads > 0,
           "disk cache: positive control, a THREADED device of the same engine starts threads the count sees "
           "(hr %08lx, %d threads)", static_cast<unsigned long>(hr), threaded_threads);
}

} // namespace harness
