// SPDX-License-Identifier: MIT
// The over-commit probe of the memory manager design (memory manager stage 1, test (a)): what the driver and VidMm do
// when one process holds more DEFAULT memory than the LOCAL budget, and UPLOAD memory beside it.
//
//   Phase L  DEFAULT committed buffers of --chunk-mb each until --default-mb is held, all alive.
//   Phase N  with L held, UPLOAD committed buffers until --upload-mb is held; the CPU writes a pattern into each.
//   Phase T  --passes passes over the whole set in creation order: the GPU copies a 256 KiB pattern into every DEFAULT
//            buffer, then copies one 256-byte sample of each DEFAULT and each UPLOAD buffer to a READBACK buffer,
//            which the CPU compares. Each pass makes VidMm bring the whole set back in, so a set larger than the
//            budget is cycled instead of left where the first placement put it.
//
// Budgets are recorded, never used to size a step: QueryVideoMemoryInfo for both groups and dxgkrnl's own view of this
// process (D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP: Budget, Requested, Usage, Demoted[] by priority class) at
// start, after L, after N and after each pass. GetDeviceRemovedReason after each phase. d3d12.dll, dxgi.dll and
// gdi32.dll are loaded from System32 only. One JSON document (sorted keys, the writer of d3d12caps) durably to the
// output path, and one "result" line on stdout. A deadline thread writes what is there and ends the process with 3.
//
// Exit: 0 every buffer created, no removal, every sample matched and every submission fenced; 1 otherwise; 2 usage;
// 3 deadline.
//
// Usage: amdgpu_wddm_d3d12oversub.exe [adapter-index] [output-path] [--default-mb N] [--upload-mb N] [--chunk-mb N]
//                                     [--passes N] [--seconds N] [--progress]
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// The JSON tree and writer of d3d12caps.cpp: objects are std::map, so keys come out sorted, one leaf per line.
struct Json {
    enum class Kind { Obj, Null, Bool, Int, Str };
    Kind kind = Kind::Obj;
    bool b = false;
    long long i = 0;
    std::string s;
    std::map<std::string, Json> obj;
    Json& operator[](const std::string& key) { kind = Kind::Obj; return obj[key]; }
};

Json null_value() { Json j; j.kind = Json::Kind::Null; return j; }
Json jbool(bool v) { Json j; j.kind = Json::Kind::Bool; j.b = v; return j; }
Json num(long long v) { Json j; j.kind = Json::Kind::Int; j.i = v; return j; }
Json str(const std::string& v) { Json j; j.kind = Json::Kind::Str; j.s = v; return j; }

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

Json hresult(HRESULT hr) {
    static const struct { HRESULT hr; const char* name; } names[] = {
        {S_OK, "S_OK"}, {S_FALSE, "S_FALSE"}, {E_FAIL, "E_FAIL"}, {E_INVALIDARG, "E_INVALIDARG"},
        {E_NOTIMPL, "E_NOTIMPL"}, {E_NOINTERFACE, "E_NOINTERFACE"}, {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
        {E_POINTER, "E_POINTER"}, {E_UNEXPECTED, "E_UNEXPECTED"}, {E_ACCESSDENIED, "E_ACCESSDENIED"},
        {DXGI_ERROR_UNSUPPORTED, "DXGI_ERROR_UNSUPPORTED"}, {DXGI_ERROR_NOT_FOUND, "DXGI_ERROR_NOT_FOUND"},
        {DXGI_ERROR_INVALID_CALL, "DXGI_ERROR_INVALID_CALL"}, {DXGI_ERROR_DEVICE_REMOVED, "DXGI_ERROR_DEVICE_REMOVED"},
        {DXGI_ERROR_DEVICE_HUNG, "DXGI_ERROR_DEVICE_HUNG"}, {DXGI_ERROR_DEVICE_RESET, "DXGI_ERROR_DEVICE_RESET"},
        {DXGI_ERROR_DRIVER_INTERNAL_ERROR, "DXGI_ERROR_DRIVER_INTERNAL_ERROR"},
        {HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), "ERROR_MOD_NOT_FOUND"},
        {HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "ERROR_PROC_NOT_FOUND"},
        {HRESULT_FROM_WIN32(WAIT_TIMEOUT), "WAIT_TIMEOUT"},
    };
    char buf[80];
    const char* name = "";
    for (const auto& n : names) if (n.hr == hr) name = n.name;
    snprintf(buf, sizeof buf, "0x%08lX%s%s", static_cast<unsigned long>(hr), *name ? " " : "", name);
    return str(buf);
}

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04X", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
    out += '"';
}

void serialize(std::string& out, const Json& j, int depth) {
    switch (j.kind) {
    case Json::Kind::Null: out += "null"; return;
    case Json::Kind::Bool: out += j.b ? "true" : "false"; return;
    case Json::Kind::Int: out += std::to_string(j.i); return;
    case Json::Kind::Str: escape(out, j.s); return;
    case Json::Kind::Obj:
        if (j.obj.empty()) { out += "{}"; return; }
        out += "{\n";
        size_t n = 0;
        for (const auto& [k, v] : j.obj) {
            out.append(static_cast<size_t>(depth + 1), ' ');
            escape(out, k);
            out += ": ";
            serialize(out, v, depth + 1);
            out += ++n < j.obj.size() ? ",\n" : "\n";
        }
        out.append(static_cast<size_t>(depth), ' ');
        out += '}';
    }
}

bool write_durably(const wchar_t* path, const std::string& text, std::string& why) {
    std::wstring tmp = std::wstring(path) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = "cannot create " + utf8(tmp.c_str()) + " (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(h);
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) { why = "write failed (" + std::to_string(err) + ")"; DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        why = "rename failed (" + std::to_string(GetLastError()) + ")"; return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The document is shared with the deadline thread, as in d3d12allocprobe: the main thread changes it only through
// put() and never holds the lock across a driver call.
CRITICAL_SECTION g_lock;
Json g_doc;
std::string g_step = "start";
bool g_written = false;
const wchar_t* g_out_path = nullptr;
bool g_progress = false;
DWORD g_deadline_ms = 150000;                   // a 150 s client inside a 180 s lab slot
ULONGLONG g_start = 0;

void put(std::initializer_list<std::string> path, Json value) {
    EnterCriticalSection(&g_lock);
    Json* j = &g_doc;
    for (const std::string& k : path) j = &(*j)[k];
    *j = std::move(value);
    LeaveCriticalSection(&g_lock);
}

void step(const std::string& what) {
    EnterCriticalSection(&g_lock);
    g_step = what;
    LeaveCriticalSection(&g_lock);
    if (g_progress) {
        fprintf(stderr, "d3d12oversub: %6.1f s %s\n", (GetTickCount64() - g_start) / 1000.0, what.c_str());
        fflush(stderr);
    }
    char stall[64];
    const DWORD n = GetEnvironmentVariableA("D3D12OVERSUB_TEST_STALL", stall, sizeof stall);
    if (n && n < sizeof stall && what == stall) Sleep(INFINITE);
}

bool emit(std::string& why) {
    std::string text;
    serialize(text, g_doc, 0);
    text += '\n';
    if (g_out_path) return write_durably(g_out_path, text, why);
    _setmode(_fileno(stdout), _O_BINARY);
    if (fwrite(text.data(), 1, text.size(), stdout) != text.size() || fflush(stdout)) {
        why = "stdout write failed";
        return false;
    }
    return true;
}

DWORD WINAPI deadline(void* done) {
    if (WaitForSingleObject(done, g_deadline_ms) != WAIT_TIMEOUT) return 0;
    EnterCriticalSection(&g_lock);
    if (g_written) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    g_doc["deadline"]["hit"] = jbool(true);
    g_doc["deadline"]["step"] = str(g_step);
    std::string why;
    if (!emit(why)) fprintf(stderr, "d3d12oversub: %s\n", why.c_str());
    fprintf(stderr, "d3d12oversub: deadline of %lu ms in step %s\n", g_deadline_ms, g_step.c_str());
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return 3;
}

// ---------------------------------------------------------------------------------------------------------------
constexpr UINT64 kMiB = 1ull << 20;
constexpr UINT64 kPatternBytes = 256ull << 10;  // the touch: one 256 KiB copy into every DEFAULT buffer per pass
constexpr UINT64 kSampleBytes = 256;            // one sample of every buffer per pass, compared on the CPU
constexpr UINT kBatch = 16;                     // buffers per submission (16 x 256 KiB copies)

// The pattern word k of pass p: different in every pass and at every offset, never 0.
uint32_t pattern(uint32_t pass, uint32_t k) { return (0x9E3779B9u * (k + 1)) ^ (pass << 28) ^ 0x5A5A0000u; }
// Where pass p writes into DEFAULT buffer i, and where in that write it samples: both 256-byte aligned.
UINT64 write_offset(UINT64 chunk, UINT i, UINT p) {
    const UINT64 slots = (chunk - kPatternBytes) / (64ull << 10) + 1;
    return ((static_cast<UINT64>(i) * 37 + p * 11) % slots) * (64ull << 10);
}
UINT64 sample_offset(UINT i, UINT p) { return ((static_cast<UINT64>(i) * 13 + p) % (kPatternBytes / kSampleBytes)) * kSampleBytes; }
// The word k of the CPU-written header of UPLOAD buffer j.
uint32_t upload_word(UINT j, uint32_t k) { return 0xC0DE0000u ^ (j << 8) ^ k; }

using QueryStatistics = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
QueryStatistics g_stats = nullptr;
HANDLE g_self = nullptr;
LUID g_luid{};

// Both groups as DXGI reports them, and dxgkrnl's own view of this process, under readings/<point>.
void budgets(IDXGIAdapter3* a, const std::string& point) {
    static const char* const groups[2] = {"local", "non_local"};
    for (int g = 0; g < 2; g++) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        const HRESULT hr = a->QueryVideoMemoryInfo(0, g ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL
                                                        : DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        put({"readings", point, groups[g], "dxgi", "hr"}, hresult(hr));
        if (SUCCEEDED(hr)) {
            put({"readings", point, groups[g], "dxgi", "Budget"}, num(static_cast<long long>(info.Budget)));
            put({"readings", point, groups[g], "dxgi", "CurrentUsage"}, num(static_cast<long long>(info.CurrentUsage)));
        }
        if (!g_stats || !g_self) continue;
        D3DKMT_QUERYSTATISTICS q;
        memset(&q, 0, sizeof q);
        q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP;
        q.AdapterLuid = g_luid;
        q.hProcess = g_self;
        q.QueryProcessSegmentGroup = g ? D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL : D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        const NTSTATUS st = g_stats(&q);
        char b[16];
        snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(st));
        put({"readings", point, groups[g], "process", "status"}, str(b));
        if (st < 0) continue;
        const auto& r = q.QueryResult.ProcessSegmentGroupInformation;
        put({"readings", point, groups[g], "process", "Budget"}, num(static_cast<long long>(r.Budget)));
        put({"readings", point, groups[g], "process", "Requested"}, num(static_cast<long long>(r.Requested)));
        put({"readings", point, groups[g], "process", "Usage"}, num(static_cast<long long>(r.Usage)));
        UINT64 demoted = 0;
        for (size_t c = 0; c < sizeof r.Demoted / sizeof r.Demoted[0]; c++) {
            put({"readings", point, groups[g], "process", "Demoted", std::to_string(c)},
                num(static_cast<long long>(r.Demoted[c])));
            demoted += r.Demoted[c];
        }
        put({"readings", point, groups[g], "process", "DemotedTotal"}, num(static_cast<long long>(demoted)));
    }
    put({"readings", point, "ms"}, num(static_cast<long long>(GetTickCount64() - g_start)));
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    return {D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
}

HRESULT committed(ID3D12Device* d, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state,
                  ComPtr<ID3D12Resource>& out) {
    const D3D12_HEAP_PROPERTIES props{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    const D3D12_RESOURCE_DESC desc = buffer_desc(bytes);
    return d->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out));
}

bool parse_count(const wchar_t* text, unsigned long lo, unsigned long hi, unsigned long& out) {
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(text, &end, 10);
    if (!*text || *end || v < lo || v > hi) return false;
    out = v;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    UINT index = 0;
    const wchar_t* out_path = nullptr;
    unsigned long default_mb = 5632, upload_mb = 1024, chunk_mb = 64, passes = 3, seconds = 150;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        unsigned long* value = nullptr;
        unsigned long lo = 0, hi = 0;
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            puts("amdgpu_wddm_d3d12oversub [adapter-index] [output-path] [--default-mb N] [--upload-mb N] [--chunk-mb N]\n"
                 "                         [--passes N] [--seconds N] [--progress]\n"
                 "Holds --default-mb (5632) of DEFAULT and --upload-mb (1024) of UPLOAD committed buffers of --chunk-mb\n"
                 "(64) each on the adapter (default 0), then makes the GPU write and the CPU check every one of them\n"
                 "--passes (3) times. Budgets are recorded at every phase, never used to size one. One JSON document to\n"
                 "stdout or durably to output-path, a 'result' line on stdout; --seconds (150) is the deadline (exit 3).");
            return 0;
        }
        if (!wcscmp(argv[i], L"--progress")) { g_progress = true; continue; }
        if (!wcscmp(argv[i], L"--default-mb")) { value = &default_mb; lo = 0; hi = 65536; }
        else if (!wcscmp(argv[i], L"--upload-mb")) { value = &upload_mb; lo = 0; hi = 65536; }
        else if (!wcscmp(argv[i], L"--chunk-mb")) { value = &chunk_mb; lo = 1; hi = 1024; }
        else if (!wcscmp(argv[i], L"--passes")) { value = &passes; lo = 0; hi = 16; }
        else if (!wcscmp(argv[i], L"--seconds")) { value = &seconds; lo = 5; hi = 170; }
        if (value) {
            if (i + 1 >= argc || !parse_count(argv[i + 1], lo, hi, *value)) {
                fprintf(stderr, "d3d12oversub: %ls needs a number in [%lu, %lu]\n", argv[i], lo, hi);
                return 2;
            }
            i++;
            continue;
        }
        if (positional == 0) {
            unsigned long v = 0;
            if (!parse_count(argv[i], 0, 64, v)) { fprintf(stderr, "d3d12oversub: invalid adapter index\n"); return 2; }
            index = static_cast<UINT>(v);
        } else if (positional == 1) out_path = argv[i];
        else { fprintf(stderr, "d3d12oversub: too many arguments\n"); return 2; }
        positional++;
    }
    const UINT64 chunk = chunk_mb * kMiB;
    if (chunk < kPatternBytes) { fprintf(stderr, "d3d12oversub: --chunk-mb below the 256 KiB pattern\n"); return 2; }
    const UINT want_default = static_cast<UINT>(default_mb / chunk_mb);
    const UINT want_upload = static_cast<UINT>(upload_mb / chunk_mb);

    g_start = GetTickCount64();
    g_deadline_ms = seconds * 1000;
    InitializeCriticalSection(&g_lock);
    g_out_path = out_path;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = done ? CreateThread(nullptr, 0, deadline, done, 0, nullptr) : nullptr;
    put({"schema"}, num(1));
    put({"tool", "name"}, str("d3d12oversub"));
    put({"tool", "headers"}, str("Windows SDK 10.0.26100"));
    put({"tool", "adapter_index"}, num(index));
    put({"tool", "pid"}, num(GetCurrentProcessId()));
    put({"plan", "chunk_bytes"}, num(static_cast<long long>(chunk)));
    put({"plan", "default_buffers"}, num(want_default));
    put({"plan", "upload_buffers"}, num(want_upload));
    put({"plan", "passes"}, num(passes));
    put({"deadline", "seconds"}, num(seconds));
    put({"deadline", "hit"}, jbool(false));
    if (!thread) put({"deadline", "thread"}, hresult(HRESULT_FROM_WIN32(GetLastError())));

    step("dxgi");
    ComPtr<IDXGIFactory1> factory;
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dxgi) {
        using Create2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
        auto c2 = reinterpret_cast<Create2>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
        const HRESULT hr = c2 ? c2(0, IID_PPV_ARGS(&factory)) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        put({"dxgi", "CreateDXGIFactory2"}, hresult(hr));
    } else put({"dxgi", "LoadLibrary"}, hresult(HRESULT_FROM_WIN32(GetLastError())));
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    if (factory && SUCCEEDED(factory->EnumAdapters1(index, &adapter))) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            put({"dxgi", "Description"}, str(utf8(desc.Description)));
            put({"dxgi", "DedicatedVideoMemory"}, num(static_cast<long long>(desc.DedicatedVideoMemory)));
            put({"dxgi", "SharedSystemMemory"}, num(static_cast<long long>(desc.SharedSystemMemory)));
            g_luid = desc.AdapterLuid;
        }
        adapter.As(&adapter3);
    }
    if (!adapter3) put({"adapter_error"}, str("adapter index not present or no IDXGIAdapter3"));
    HMODULE gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (gdi) g_stats = reinterpret_cast<QueryStatistics>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTQueryStatistics")));
    // A real handle: dxgkrnl refused the process queries on a PROCESS_QUERY_LIMITED_INFORMATION handle (bc250kmd_cli).
    g_self = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    put({"tool", "process_statistics"}, jbool(g_stats && g_self));

    step("device");
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                              reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                        : nullptr;
    ComPtr<ID3D12Device> device;
    if (create && adapter3) {
        const HRESULT hr = create(adapter3.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        put({"device", "D3D12CreateDevice"}, hresult(hr));
    }
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (device) {
        const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                          IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        put({"device", "submission_objects"}, hresult(hr));
        if (FAILED(hr) || !fence_event) device.Reset();
    }
    const auto removed = [&](const char* phase) {
        const HRESULT r = device ? device->GetDeviceRemovedReason() : E_POINTER;
        put({"removed", phase}, hresult(r));
        return r != S_OK;
    };
    if (adapter3) budgets(adapter3.Get(), "0_start");

    // ---- phase L --------------------------------------------------------------------------------------------------
    std::vector<ComPtr<ID3D12Resource>> local, upload;
    UINT failed_creates = 0;
    if (device) {
        step("phase L");
        const ULONGLONG t = GetTickCount64();
        for (UINT i = 0; i < want_default; i++) {
            ComPtr<ID3D12Resource> r;
            const HRESULT hr = committed(device.Get(), D3D12_HEAP_TYPE_DEFAULT, chunk, D3D12_RESOURCE_STATE_COMMON, r);
            if (FAILED(hr)) {
                put({"phase_l", "failure", "index"}, num(i));
                put({"phase_l", "failure", "hr"}, hresult(hr));
                failed_creates++;
                break;
            }
            local.push_back(r);
        }
        put({"phase_l", "created"}, num(static_cast<long long>(local.size())));
        put({"phase_l", "bytes"}, num(static_cast<long long>(local.size() * chunk)));
        put({"phase_l", "ms"}, num(static_cast<long long>(GetTickCount64() - t)));
        removed("1_after_l");
        budgets(adapter3.Get(), "1_after_l");
    }

    // ---- phase N --------------------------------------------------------------------------------------------------
    if (device) {
        step("phase N");
        const ULONGLONG t = GetTickCount64();
        for (UINT j = 0; j < want_upload; j++) {
            ComPtr<ID3D12Resource> r;
            HRESULT hr = committed(device.Get(), D3D12_HEAP_TYPE_UPLOAD, chunk, D3D12_RESOURCE_STATE_GENERIC_READ, r);
            void* p = nullptr;
            const D3D12_RANGE none{0, 0};
            if (SUCCEEDED(hr)) hr = r->Map(0, &none, &p);
            if (FAILED(hr)) {
                put({"phase_n", "failure", "index"}, num(j));
                put({"phase_n", "failure", "hr"}, hresult(hr));
                failed_creates++;
                break;
            }
            uint32_t* w = static_cast<uint32_t*>(p);
            for (uint32_t k = 0; k < kSampleBytes / 4; k++) w[k] = upload_word(j, k);
            const D3D12_RANGE wrote{0, kSampleBytes};
            r->Unmap(0, &wrote);
            upload.push_back(r);
        }
        put({"phase_n", "created"}, num(static_cast<long long>(upload.size())));
        put({"phase_n", "bytes"}, num(static_cast<long long>(upload.size() * chunk)));
        put({"phase_n", "ms"}, num(static_cast<long long>(GetTickCount64() - t)));
        removed("2_after_n");
        budgets(adapter3.Get(), "2_after_n");
    }

    // ---- phase T --------------------------------------------------------------------------------------------------
    long long sampled = 0, verified = 0, mismatched = 0, submissions = 0, fenced = 0;
    ComPtr<ID3D12Resource> staging, readback;
    uint32_t* staging_words = nullptr;
    const UINT total = static_cast<UINT>(local.size() + upload.size());
    unsigned long run = passes;                 // the passes that can run; the verdict counts the asked ones
    if (device && run && total) {
        step("phase T setup");
        HRESULT hr = committed(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kPatternBytes, D3D12_RESOURCE_STATE_GENERIC_READ,
                               staging);
        if (SUCCEEDED(hr)) hr = committed(device.Get(), D3D12_HEAP_TYPE_READBACK, total * kSampleBytes,
                                          D3D12_RESOURCE_STATE_COPY_DEST, readback);
        const D3D12_RANGE none{0, 0};
        if (SUCCEEDED(hr)) hr = staging->Map(0, &none, reinterpret_cast<void**>(&staging_words));
        put({"phase_t", "setup"}, hresult(hr));
        if (FAILED(hr)) run = 0;
    } else run = 0;
    UINT64 fence_value = 0;
    bool stalled = false;
    for (UINT p = 0; p < run && !stalled; p++) {
        step("phase T pass " + std::to_string(p));
        const ULONGLONG t = GetTickCount64();
        for (uint32_t k = 0; k < kPatternBytes / 4; k++) staging_words[k] = pattern(p, k);
        for (UINT first = 0; first < total && !stalled; first += kBatch) {
            const UINT last = first + kBatch < total ? first + kBatch : total;
            HRESULT hr = allocator->Reset();
            if (SUCCEEDED(hr)) hr = list->Reset(allocator.Get(), nullptr);
            for (UINT b = first; b < last && SUCCEEDED(hr); b++) {
                if (b < local.size()) {
                    ID3D12Resource* r = local[b].Get();
                    const UINT64 at = write_offset(chunk, b, p);
                    // COMMON promotes to COPY_DEST on the copy; the explicit barrier makes it a copy source.
                    list->CopyBufferRegion(r, at, staging.Get(), 0, kPatternBytes);
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE};
                    list->ResourceBarrier(1, &barrier);
                    list->CopyBufferRegion(readback.Get(), b * kSampleBytes, r, at + sample_offset(b, p), kSampleBytes);
                } else {
                    list->CopyBufferRegion(readback.Get(), b * kSampleBytes, upload[b - local.size()].Get(), 0,
                                           kSampleBytes);
                }
            }
            if (SUCCEEDED(hr)) hr = list->Close();
            if (FAILED(hr)) { put({"phase_t", "record_error"}, hresult(hr)); stalled = true; break; }
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            submissions++;
            hr = queue->Signal(fence.Get(), ++fence_value);
            if (SUCCEEDED(hr) && fence->GetCompletedValue() < fence_value) {
                hr = fence->SetEventOnCompletion(fence_value, fence_event);
                if (SUCCEEDED(hr) && WaitForSingleObject(fence_event, 30000) != WAIT_OBJECT_0)
                    hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            }
            if (FAILED(hr)) { put({"phase_t", "fence_error"}, hresult(hr)); stalled = true; break; }
            fenced++;
            const D3D12_RANGE read{first * kSampleBytes, last * kSampleBytes};
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, &read, &mapped))) {
                put({"phase_t", "map_error"}, jbool(true));
                stalled = true;
                break;
            }
            const uint8_t* base = static_cast<const uint8_t*>(mapped);
            for (UINT b = first; b < last; b++) {
                const uint32_t* got = reinterpret_cast<const uint32_t*>(base + b * kSampleBytes);
                bool same = true;
                for (uint32_t k = 0; k < kSampleBytes / 4 && same; k++) {
                    const uint32_t want = b < local.size()
                        ? pattern(p, static_cast<uint32_t>(sample_offset(b, p) / 4) + k)
                        : upload_word(static_cast<UINT>(b - local.size()), k);
                    same = got[k] == want;
                }
                sampled++;
                if (same) verified++;
                else if (mismatched++ < 8) {
                    char k[16];
                    snprintf(k, sizeof k, "%u_%u", p, b);
                    put({"phase_t", "mismatch", k}, num(got[0]));
                }
            }
            const D3D12_RANGE wrote{0, 0};
            readback->Unmap(0, &wrote);
        }
        put({"phase_t", "pass_ms", std::to_string(p)}, num(static_cast<long long>(GetTickCount64() - t)));
        char point[24];
        snprintf(point, sizeof point, "%u_after_pass%u", 3 + p, p);
        removed(point);
        budgets(adapter3.Get(), point);
    }
    put({"phase_t", "sampled"}, num(sampled));
    put({"phase_t", "verified"}, num(verified));
    put({"phase_t", "mismatched"}, num(mismatched));
    put({"phase_t", "submissions"}, num(submissions));
    put({"phase_t", "fenced"}, num(fenced));

    step("end");
    const bool gone = removed("9_end");
    const bool pass = device && !gone && !failed_creates && local.size() == want_default &&
        upload.size() == want_upload && mismatched == 0 && verified == sampled && fenced == submissions &&
        sampled == static_cast<long long>(passes) * total;
    put({"result", "pass"}, jbool(pass));
    char line[320];
    snprintf(line, sizeof line, "result %s created_default %zu/%u created_upload %zu/%u bytes_default %llu "
             "bytes_upload %llu sampled %lld verified %lld mismatched %lld fenced %lld/%lld removed %d",
             pass ? "PASS" : "FAIL", local.size(), want_default, upload.size(), want_upload,
             static_cast<unsigned long long>(local.size() * chunk), static_cast<unsigned long long>(upload.size() * chunk),
             sampled, verified, mismatched, fenced, submissions, gone ? 1 : 0);
    put({"result", "line"}, str(line));

    step("release");
    if (staging_words) staging->Unmap(0, nullptr);
    local.clear();
    upload.clear();
    staging.Reset();
    readback.Reset();
    list.Reset();
    allocator.Reset();
    queue.Reset();
    fence.Reset();
    device.Reset();
    if (fence_event) CloseHandle(fence_event);
    if (adapter3) budgets(adapter3.Get(), "9_released");
    if (g_self) CloseHandle(g_self);

    step("write");
    EnterCriticalSection(&g_lock);
    g_written = true;
    std::string why;
    const bool ok = emit(why);
    LeaveCriticalSection(&g_lock);
    if (done) SetEvent(done);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    if (done) CloseHandle(done);
    if (g_out_path) { puts(line); fflush(stdout); }
    if (!ok) {
        fprintf(stderr, "d3d12oversub: %s\n", why.c_str());
        return 1;
    }
    return pass ? 0 : 1;
}
