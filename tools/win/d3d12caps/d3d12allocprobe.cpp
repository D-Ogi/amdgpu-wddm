// SPDX-License-Identifier: MIT
// Whether a texture the driver cannot size costs the application its device. For R8G8B8A8_UNORM (the control), YUY2
// and R8G8_B8G8_UNORM, a 64 x 64 texture goes through GetResourceAllocationInfo, CreateCommittedResource, CreateHeap
// and CreatePlacedResource, each step followed by GetDeviceRemovedReason; then the format's support answer is read.
// The control runs first and again last. Before each format, a removed device is released and a new one created at
// the same level. d3d12.dll and dxgi.dll are loaded from System32 only, so the system runtime and the registered
// driver answer whatever lies next to the executable. One JSON document with the sorted keys of d3d12caps; every
// result is data in it. A deadline thread writes what is there and ends the process with 3 when a call does not
// return.
//
// Usage: amdgpu_wddm_d3d12allocprobe.exe [adapter-index] [output-path] [--progress]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <psapi.h>
#include <fcntl.h>
#include <io.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
using Microsoft::WRL::ComPtr;

// Built with -AgilitySdkVersion <n>: d3d12.dll reads these exports and loads .\D3D12_0\D3D12Core.dll.
#ifdef CAPS_AGILITY_SDK_VERSION
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = CAPS_AGILITY_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12_0\\"; }
#endif

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
Json hex(unsigned long long v) { char b[32]; snprintf(b, sizeof b, "0x%llX", v); return str(b); }
// Sizes: a UINT64_MAX returned by the runtime is its error marker, recorded as such.
Json size(UINT64 v) { return v == UINT64_MAX ? str("UINT64_MAX") : num(static_cast<long long>(v)); }

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string key(unsigned index, const char* name) {
    char buf[96];
    snprintf(buf, sizeof buf, "%u_%s", index, name);
    return buf;
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
        {DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE"},
        {D3D12_ERROR_ADAPTER_NOT_FOUND, "D3D12_ERROR_ADAPTER_NOT_FOUND"},
        {D3D12_ERROR_DRIVER_VERSION_MISMATCH, "D3D12_ERROR_DRIVER_VERSION_MISMATCH"},
        {D3D12_ERROR_INVALID_REDIST, "D3D12_ERROR_INVALID_REDIST"},
        {HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), "ERROR_MOD_NOT_FOUND"},
        {HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "ERROR_PROC_NOT_FOUND"},
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

// "A|B|C", unknown bits as a trailing hex value, "" for none (the tables of d3d12caps.cpp).
struct FlagName { UINT bit; const char* name; };
const FlagName kSupport1[] = {
    {0x1, "BUFFER"}, {0x2, "IA_VERTEX_BUFFER"}, {0x4, "IA_INDEX_BUFFER"}, {0x8, "SO_BUFFER"}, {0x10, "TEXTURE1D"},
    {0x20, "TEXTURE2D"}, {0x40, "TEXTURE3D"}, {0x80, "TEXTURECUBE"}, {0x100, "SHADER_LOAD"}, {0x200, "SHADER_SAMPLE"},
    {0x400, "SHADER_SAMPLE_COMPARISON"}, {0x800, "SHADER_SAMPLE_MONO_TEXT"}, {0x1000, "MIP"},
    {0x4000, "RENDER_TARGET"}, {0x8000, "BLENDABLE"}, {0x10000, "DEPTH_STENCIL"}, {0x40000, "MULTISAMPLE_RESOLVE"},
    {0x80000, "DISPLAY"}, {0x100000, "CAST_WITHIN_BIT_LAYOUT"}, {0x200000, "MULTISAMPLE_RENDERTARGET"},
    {0x400000, "MULTISAMPLE_LOAD"}, {0x800000, "SHADER_GATHER"}, {0x1000000, "BACK_BUFFER_CAST"},
    {0x2000000, "TYPED_UNORDERED_ACCESS_VIEW"}, {0x4000000, "SHADER_GATHER_COMPARISON"},
    {0x8000000, "DECODER_OUTPUT"}, {0x10000000, "VIDEO_PROCESSOR_OUTPUT"}, {0x20000000, "VIDEO_PROCESSOR_INPUT"},
    {0x40000000, "VIDEO_ENCODER"},
};
const FlagName kSupport2[] = {
    {0x1, "UAV_ATOMIC_ADD"}, {0x2, "UAV_ATOMIC_BITWISE_OPS"}, {0x4, "UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE"},
    {0x8, "UAV_ATOMIC_EXCHANGE"}, {0x10, "UAV_ATOMIC_SIGNED_MIN_OR_MAX"}, {0x20, "UAV_ATOMIC_UNSIGNED_MIN_OR_MAX"},
    {0x40, "UAV_TYPED_LOAD"}, {0x80, "UAV_TYPED_STORE"}, {0x100, "OUTPUT_MERGER_LOGIC_OP"}, {0x200, "TILED"},
    {0x4000, "MULTIPLANE_OVERLAY"}, {0x8000, "SAMPLER_FEEDBACK"}, {0x10000, "DISPLAYABLE"},
};
template <size_t N> Json flags(UINT v, const FlagName (&table)[N]) {
    std::string s;
    for (const auto& f : table) if (v & f.bit) { if (!s.empty()) s += '|'; s += f.name; v &= ~f.bit; }
    if (v) { char b[24]; snprintf(b, sizeof b, "%s0x%X", s.empty() ? "" : "|", v); s += b; }
    return str(s);
}

const char* feature_level_name(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_1_0_GENERIC: return "1_0_GENERIC";
    case D3D_FEATURE_LEVEL_1_0_CORE: return "1_0_CORE";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    default: return nullptr;
    }
}
Json feature_level(D3D_FEATURE_LEVEL fl) {
    const char* n = feature_level_name(fl);
    return n ? str(n) : hex(static_cast<UINT>(fl));
}

// ---------------------------------------------------------------------------------------------------------------
// Modules loaded since start, as d3d12caps records them: where each comes from and its file version.
std::wstring lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
std::wstring dir_of(const std::wstring& path) {
    auto p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

std::set<HMODULE> module_set() {
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                               &needed))
        return {};
    mods.resize(needed / sizeof(HMODULE) < mods.size() ? needed / sizeof(HMODULE) : mods.size());
    return {mods.begin(), mods.end()};
}

void modules(const std::set<HMODULE>& before, Json& out) {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    const std::wstring exe_dir = lower(dir_of(buf));
    GetSystemDirectoryW(buf, ARRAYSIZE(buf));
    const std::wstring sys_dir = lower(buf);
    for (HMODULE m : module_set()) {
        if (before.count(m)) continue;
        if (!GetModuleFileNameW(m, buf, ARRAYSIZE(buf))) continue;
        const std::wstring path = buf;
        const std::wstring lpath = lower(path);
        const std::wstring dir = dir_of(lpath);
        const std::wstring base = lpath.substr(dir.size() + 1);
        std::string location;
        const size_t ds = lpath.find(L"\\driverstore\\filerepository\\");
        if (dir == exe_dir) location = "exe_dir";
        else if (dir.rfind(exe_dir + L"\\", 0) == 0) location = "exe_dir" + utf8(dir.substr(exe_dir.size()).c_str());
        else if (dir == sys_dir) location = "system32";
        else if (ds != std::wstring::npos) {
            std::wstring rest = lpath.substr(ds + wcslen(L"\\driverstore\\filerepository\\"));
            location = "driver_store\\" + utf8(rest.substr(0, rest.find(L'\\')).c_str());
        } else location = "other";
        Json& j = out[utf8(base.c_str())];
        j["location"] = str(location);
        DWORD handle = 0;
        DWORD len = GetFileVersionInfoSizeW(path.c_str(), &handle);
        std::vector<unsigned char> info(len ? len : 1);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT flen = 0;
        if (len && GetFileVersionInfoW(path.c_str(), 0, len, info.data()) &&
            VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &flen) && fixed) {
            char b[48];
            snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                     HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
            j["file_version"] = str(b);
        } else j["file_version"] = null_value();
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
// The document is shared with the deadline thread. The main thread changes it only through put() and never holds
// the lock across a driver call; the deadline thread takes the lock, writes and ends the process.
constexpr DWORD kDeadlineMs = 12000;            // the caps profile of a lab trial gives both its runs 35 s together
CRITICAL_SECTION g_lock;
Json g_doc;
std::string g_step = "start";
bool g_written = false;                         // the main thread writes the document: the deadline thread does not
const wchar_t* g_out_path = nullptr;
bool g_progress = false;

void put(std::initializer_list<std::string> path, Json value) {
    EnterCriticalSection(&g_lock);
    Json* j = &g_doc;
    for (const std::string& k : path) j = &(*j)[k];
    *j = std::move(value);
    LeaveCriticalSection(&g_lock);
}

// The step about to start, for the deadline document and, with --progress, on stderr. D3D12ALLOCPROBE_TEST_STALL
// names a step at which the main thread stops for good: the build's test of the deadline (build.ps1).
void step(const std::string& what) {
    EnterCriticalSection(&g_lock);
    g_step = what;
    LeaveCriticalSection(&g_lock);
    if (g_progress) { fprintf(stderr, "d3d12allocprobe: %s\n", what.c_str()); fflush(stderr); }
    char stall[64];
    const DWORD n = GetEnvironmentVariableA("D3D12ALLOCPROBE_TEST_STALL", stall, sizeof stall);
    if (n && n < sizeof stall && what == stall) Sleep(INFINITE);
}

// The document, durably to the output path or to stdout. The caller holds the lock.
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

// When the main thread has not finished in time: the document as it is, with the step that did not return, and exit
// code 3. TerminateProcess, so that no DLL of a stuck driver runs its detach.
DWORD WINAPI deadline(void* done) {
    if (WaitForSingleObject(done, kDeadlineMs) != WAIT_TIMEOUT) return 0;
    EnterCriticalSection(&g_lock);
    if (g_written) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    g_doc["deadline"]["hit"] = jbool(true);
    g_doc["deadline"]["step"] = str(g_step);
    std::string why;
    if (!emit(why)) fprintf(stderr, "d3d12allocprobe: %s\n", why.c_str());
    fprintf(stderr, "d3d12allocprobe: deadline of %lu ms in step %s\n", kDeadlineMs, g_step.c_str());
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return 3;
}

// ---------------------------------------------------------------------------------------------------------------
constexpr UINT kTexels = 64;                    // 64 x 64: even, as YUY2 and R8G8_B8G8_UNORM need
constexpr UINT64 kHeapBytes = 4ull << 20;       // the placed step's heap, room for any 64 x 64 texture

// One format on device d: four steps, each recorded with GetDeviceRemovedReason right after it, then the format's
// support answer. The resources and the heap are released before the next format.
void probe_format(ID3D12Device* d, DXGI_FORMAT format, const std::string& fk) {
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kTexels, kTexels, 1, 1, format, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                      D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    const auto removed = [&](const char* name) {
        const HRESULT reason = d->GetDeviceRemovedReason();
        put({"probe", fk, name, "GetDeviceRemovedReason"}, hresult(reason));
    };

    step(fk + " GetResourceAllocationInfo");
    const D3D12_RESOURCE_ALLOCATION_INFO info = d->GetResourceAllocationInfo(0, 1, &desc);
    removed("1_GetResourceAllocationInfo");
    put({"probe", fk, "1_GetResourceAllocationInfo", "SizeInBytes"}, size(info.SizeInBytes));
    put({"probe", fk, "1_GetResourceAllocationInfo", "Alignment"}, size(info.Alignment));

    step(fk + " CreateCommittedResource");
    ComPtr<ID3D12Resource> committed;
    HRESULT hr = d->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&committed));
    removed("2_CreateCommittedResource");
    put({"probe", fk, "2_CreateCommittedResource", "hr"}, hresult(hr));
    committed.Reset();

    step(fk + " CreateHeap");
    const D3D12_HEAP_DESC heap_desc{kHeapBytes, props, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
    ComPtr<ID3D12Heap> heap;
    hr = d->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    removed("3_CreateHeap");
    put({"probe", fk, "3_CreateHeap", "hr"}, hresult(hr));
    put({"probe", fk, "3_CreateHeap", "SizeInBytes"}, size(kHeapBytes));

    step(fk + " CreatePlacedResource");
    if (heap) {
        ComPtr<ID3D12Resource> placed;
        hr = d->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&placed));
        removed("4_CreatePlacedResource");
        put({"probe", fk, "4_CreatePlacedResource", "hr"}, hresult(hr));
        put({"probe", fk, "4_CreatePlacedResource", "HeapOffset"}, num(0));
    } else {
        put({"probe", fk, "4_CreatePlacedResource", "skipped"}, str("no heap"));
    }
    heap.Reset();

    step(fk + " CheckFeatureSupport");
    D3D12_FEATURE_DATA_FORMAT_SUPPORT s{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    hr = d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof s);
    removed("5_FORMAT_SUPPORT");
    put({"probe", fk, "5_FORMAT_SUPPORT", "hr"}, hresult(hr));
    if (SUCCEEDED(hr)) {
        put({"probe", fk, "5_FORMAT_SUPPORT", "Support1"}, hex(static_cast<UINT>(s.Support1)));
        put({"probe", fk, "5_FORMAT_SUPPORT", "Support1Names"}, flags(static_cast<UINT>(s.Support1), kSupport1));
        put({"probe", fk, "5_FORMAT_SUPPORT", "Support2"}, hex(static_cast<UINT>(s.Support2)));
        put({"probe", fk, "5_FORMAT_SUPPORT", "Support2Names"}, flags(static_cast<UINT>(s.Support2), kSupport2));
    }
}

// What the deployment checks read from a caps document (Get-CapsWitness): the device's level list and tiled tier.
void witness_features(ID3D12Device* d) {
    const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_1_0_GENERIC, D3D_FEATURE_LEVEL_1_0_CORE, D3D_FEATURE_LEVEL_9_1,
        D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{static_cast<UINT>(ARRAYSIZE(all)), all, static_cast<D3D_FEATURE_LEVEL>(0)};
    HRESULT hr = d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    put({"device", "features", "FEATURE_LEVELS", "list_all", "hr"}, hresult(hr));
    if (SUCCEEDED(hr))
        put({"device", "features", "FEATURE_LEVELS", "list_all", "MaxSupportedFeatureLevel"},
            feature_level(fl.MaxSupportedFeatureLevel));
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    hr = d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    put({"device", "features", "D3D12_OPTIONS", "hr"}, hresult(hr));
    if (SUCCEEDED(hr)) put({"device", "features", "D3D12_OPTIONS", "TiledResourcesTier"}, num(o.TiledResourcesTier));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    UINT index = 0;
    const wchar_t* out_path = nullptr;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            puts("amdgpu_wddm_d3d12allocprobe [adapter-index] [output-path] [--progress]\n"
                 "Asks the system D3D12 runtime of the adapter (default 0) to size and create a 64 x 64 texture of\n"
                 "R8G8B8A8_UNORM, YUY2, R8G8_B8G8_UNORM and R8G8B8A8_UNORM again, and records each result with\n"
                 "GetDeviceRemovedReason as one JSON document, to stdout or durably to output-path. --progress names\n"
                 "each step on stderr.");
            return 0;
        }
        if (!wcscmp(argv[i], L"--progress")) { g_progress = true; continue; }
        if (positional == 0) {
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(argv[i], &end, 10);
            if (!*argv[i] || *end || v > 64) { fprintf(stderr, "d3d12allocprobe: invalid adapter index\n"); return 2; }
            index = static_cast<UINT>(v);
        } else if (positional == 1) out_path = argv[i];
        else { fprintf(stderr, "d3d12allocprobe: too many arguments\n"); return 2; }
        positional++;
    }

    InitializeCriticalSection(&g_lock);
    g_out_path = out_path;
    const std::set<HMODULE> before = module_set();
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = done ? CreateThread(nullptr, 0, deadline, done, 0, nullptr) : nullptr;
    put({"schema"}, num(1));
    put({"tool", "name"}, str("d3d12allocprobe"));
    put({"tool", "headers"}, str("Windows SDK 10.0.26100"));
#ifdef CAPS_AGILITY_SDK_VERSION
    put({"tool", "agility_sdk_version"}, num(CAPS_AGILITY_SDK_VERSION));
    put({"tool", "agility_sdk_path"}, str(".\\D3D12_0\\"));
#else
    put({"tool", "agility_sdk_version"}, null_value());
#endif
    put({"tool", "adapter_index"}, num(index));
    put({"tool", "texture"}, str("TEXTURE2D 64 x 64, 1 mip, 1 sample, layout UNKNOWN, flags NONE, state COMMON; "
                                 "heap DEFAULT, placed in a 4 MiB ALLOW_ONLY_NON_RT_DS_TEXTURES heap at offset 0"));
    put({"deadline", "seconds"}, num(kDeadlineMs / 1000));
    put({"deadline", "hit"}, jbool(false));
    if (!thread) put({"deadline", "thread"}, hresult(HRESULT_FROM_WIN32(GetLastError())));

    // System32 only: the system runtime, never an application-local d3d12.dll or dxgi.dll.
    step("dxgi");
    ComPtr<IDXGIFactory1> factory;
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    put({"dxgi", "LoadLibrary"}, hresult(dxgi ? S_OK : HRESULT_FROM_WIN32(GetLastError())));
    if (dxgi) {
        using Create2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
        auto c2 = reinterpret_cast<Create2>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
        const HRESULT hr = c2 ? c2(0, IID_PPV_ARGS(&factory)) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        put({"dxgi", "CreateDXGIFactory2"}, hresult(hr));
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (factory) {
        for (UINT i = 0;; i++) {
            ComPtr<IDXGIAdapter1> a;
            const HRESULT hr = factory->EnumAdapters1(i, &a);
            if (FAILED(hr)) {
                put({"dxgi", "adapter_count"}, num(i));
                if (hr != DXGI_ERROR_NOT_FOUND) put({"dxgi", "EnumAdapters1_error"}, hresult(hr));
                break;
            }
            char k[8];
            snprintf(k, sizeof k, "%02u", i);
            DXGI_ADAPTER_DESC1 desc{};
            const HRESULT hd = a->GetDesc1(&desc);
            put({"dxgi", "adapters", k, "GetDesc1", "hr"}, hresult(hd));
            if (SUCCEEDED(hd)) {
                put({"dxgi", "adapters", k, "GetDesc1", "Description"}, str(utf8(desc.Description)));
                put({"dxgi", "adapters", k, "GetDesc1", "VendorId"}, hex(desc.VendorId));
                put({"dxgi", "adapters", k, "GetDesc1", "DeviceId"}, hex(desc.DeviceId));
            }
            if (i == index) adapter = a;
        }
    }
    if (!adapter) put({"adapter_error"}, str("adapter index not present"));

    step("device");
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    put({"device", "LoadLibrary"}, hresult(d3d12 ? S_OK : HRESULT_FROM_WIN32(GetLastError())));
    auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                              reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                        : nullptr;
    if (d3d12 && !create) put({"device", "D3D12CreateDevice_export"}, str("missing"));
    ComPtr<ID3D12Device> device;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (create && adapter) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        for (auto fl : levels) {
            const HRESULT hr = create(adapter.Get(), fl, IID_PPV_ARGS(&device));
            put({"device", "D3D12CreateDevice", feature_level_name(fl)}, hresult(hr));
            if (SUCCEEDED(hr)) { level = fl; break; }
        }
    }
    put({"device", "created_at"}, device ? str(feature_level_name(level)) : null_value());
    unsigned devices = device ? 1 : 0;
    if (device) {
        witness_features(device.Get());
        Json loaded;                                    // already here, for a document the deadline writes
        modules(before, loaded);
        put({"modules"}, std::move(loaded));
    }

    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_YUY2, DXGI_FORMAT_R8G8_B8G8_UNORM,
                                   DXGI_FORMAT_R8G8B8A8_UNORM};
    const char* names[] = {"R8G8B8A8_UNORM", "YUY2", "R8G8_B8G8_UNORM", "R8G8B8A8_UNORM"};
    for (unsigned i = 0; i < ARRAYSIZE(formats) && devices; i++) {
        const std::string fk = key(i, names[i]);
        if (device && device->GetDeviceRemovedReason() != S_OK) {
            // A removed device answers nothing more: the next format gets a new one at the same level.
            device.Reset();
            step(fk + " new device");
            const HRESULT hr = create(adapter.Get(), level, IID_PPV_ARGS(&device));
            put({"probe", fk, "0_new_device", "D3D12CreateDevice"}, hresult(hr));
            if (SUCCEEDED(hr)) {
                devices++;
                put({"probe", fk, "0_new_device", "GetDeviceRemovedReason"}, hresult(device->GetDeviceRemovedReason()));
            }
        }
        if (!device) {
            put({"probe", fk, "skipped"}, str("no device"));
            continue;
        }
        probe_format(device.Get(), formats[i], fk);
    }

    step("end");
    if (device) put({"device", "GetDeviceRemovedReason"}, hresult(device->GetDeviceRemovedReason()));
    put({"device", "devices_created"}, num(devices));
    Json loaded;
    modules(before, loaded);
    put({"modules"}, std::move(loaded));
    step("release");
    device.Reset();
    adapter.Reset();
    factory.Reset();

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
    if (!ok) {
        fprintf(stderr, "d3d12allocprobe: %s\n", why.c_str());
        return 1;
    }
    return 0;
}
