// SPDX-License-Identifier: MIT
// mesh-demo: a windowed D3D12 mesh shader demo, something to watch rather than a witness. A field of rotating torus
// knots is drawn by an amplification program, a mesh program and a pixel program (shaders\mesh-demo.hlsl): no
// vertex buffer, no index buffer, no input layout. The amplification program culls each object against the frustum
// and picks its level of detail; the mesh program makes the meshlets; M colours each meshlet on its own.
//
// The window title shows the adapter, MeshShaderTier, the frame rate, the GPU time of a frame (timestamps around its
// work, so a frame rate cap outside the program does not hide it) and the culling result that the amplification
// program counts on the GPU, about twice a second. The Microsoft runtime is loaded from System32, never an
// application-local copy.
//
//   mesh-demo [--seconds N] [--adapter TEXT] [--debug] [--vsync] [--grid N] [--no-stats] [--no-activate]
//             [--simulate-tier0]
//   exit 0 ended by Esc, by closing the window or by --seconds; 1 a failed call, a lost device or a debug layer
//   error; 2 usage; 3 no device; 4 mesh shaders not supported (shown for 5 seconds first)
//
// Keys: Esc quits, Space pauses the animation, M toggles the meshlet colours.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "gen/mesh_demo_as.h"
#include "gen/mesh_demo_ms.h"
#include "gen/mesh_demo_ps.h"

namespace {

const char* const usage =
    "mesh-demo [--seconds N] [--adapter TEXT] [--debug] [--vsync] [--grid N] [--no-stats] [--no-activate]\n"
    "          [--simulate-tier0]\n"
    "  --seconds N        end by itself after N seconds (1..3600); without it the demo runs until Esc\n"
    "  --adapter TEXT     the first adapter whose name contains TEXT (any case); default the first hardware adapter\n"
    "  --debug            the D3D12 debug layer; its warnings and errors go to stderr, an error makes exit code 1\n"
    "  --vsync            present with sync interval 1 (default 0: the frame rate is not capped)\n"
    "  --grid N           N x N objects (4..128, default 48)\n"
    "  --no-stats         the amplification program writes no counters (the title then shows no culling result)\n"
    "  --no-activate      show the window without taking the keyboard focus\n"
    "  --simulate-tier0   behave as on an adapter without mesh shaders (test of that path)\n"
    "Keys: Esc quits, Space pauses the animation, M toggles the meshlet colours.\n"
    "Exit: 0 done, 1 failure, 2 usage, 3 no device, 4 mesh shaders not supported.";

template <class T> class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (p_) p_->Release(); }
    T* Get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T** operator&() noexcept { if (p_) { p_->Release(); p_ = nullptr; } return &p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    template <class U> HRESULT As(ComPtr<U>& other) const { return p_ ? p_->QueryInterface(IID_PPV_ARGS(&other)) : E_POINTER; }
private:
    T* p_{};
};

// Every failed call is one stderr line with the name of the call; the caller then returns a non-zero exit code.
bool failed(HRESULT hr, const char* call) {
    if (SUCCEEDED(hr)) return false;
    std::fprintf(stderr, "mesh-demo: %s failed hr=0x%08lx\n", call, static_cast<unsigned long>(hr));
    return true;
}

constexpr UINT kFrames = 3;                 // swap chain buffers, and frames in flight
constexpr UINT kWidth = 1280, kHeight = 720;
constexpr UINT kAsGroup = 32;               // AS_GROUP in the shader
constexpr UINT kTrianglesPerMeshlet = 128;  // TILE_TRIS in the shader
constexpr UINT kFlagMeshletColours = 1, kFlagStats = 2;
constexpr DWORD kFenceTimeoutMs = 5000;
constexpr double kUnsupportedSeconds = 5.0;

// Mirrors cbuffer Frame of shaders\mesh-demo.hlsl, register by register.
struct FrameConstants {
    float view_proj[16];
    float planes[6][4];
    float eye[3];      float time;
    float sun[3];      UINT object_count;
    float fog[3];      UINT grid_side;
    float lod[4];
    float spacing;     float fog_end;    UINT flags;    float pad;
};
static_assert(sizeof(FrameConstants) == 240, "FrameConstants must match cbuffer Frame");
constexpr UINT kConstantsStride = 256;      // a root CBV address is 256-byte aligned

// Counters of g_stats in the shader: objects, meshlets, objects per level of detail. Each frame's read-back buffer
// holds them, then the two GPU timestamps around the frame's work.
struct Stats {
    UINT objects, meshlets, lod[4];
};
constexpr UINT kStatsBytes = 32;
constexpr UINT kTimestampOffset = kStatsBytes;
constexpr UINT kReadbackBytes = kStatsBytes + 2 * sizeof(UINT64);
static_assert(sizeof(Stats) <= kStatsBytes, "Stats must fit the counter buffer");

// D3D12_MESH_SHADER_TIER_1 is the value 10; the title shows the tier number.
int tier_number(int tier) { return tier >= 10 ? tier / 10 : tier; }

struct Options {
    unsigned seconds{};
    std::wstring adapter;
    unsigned grid{48};
    bool debug{}, vsync{}, stats{true}, activate{true}, simulate_tier0{};
};

// What the window procedure changes and the frame loop reads; both run on the one thread.
struct Input {
    bool quit{}, paused{}, meshlet_colours{true};
} g_input;

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_KEYDOWN:
        if (lparam & (1 << 30)) return 0;   // auto-repeat
        if (wparam == VK_ESCAPE) g_input.quit = true;
        else if (wparam == VK_SPACE) g_input.paused = !g_input.paused;
        else if (wparam == 'M') g_input.meshlet_colours = !g_input.meshlet_colours;
        return 0;
    case WM_CLOSE:
        g_input.quit = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

unsigned number(const char* text, unsigned low, unsigned high) {
    unsigned value = 0;
    if (!*text) return 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > high) return 0;
        value = value * 10 + static_cast<unsigned>(*p - '0');
    }
    return value >= low && value <= high ? value : 0;
}

std::string narrow(const wchar_t* text) {
    char out[256]{};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

const char* model_text(D3D_SHADER_MODEL model) {
    switch (model) {
    case D3D_SHADER_MODEL_6_0: return "6_0";
    case D3D_SHADER_MODEL_6_1: return "6_1";
    case D3D_SHADER_MODEL_6_2: return "6_2";
    case D3D_SHADER_MODEL_6_3: return "6_3";
    case D3D_SHADER_MODEL_6_4: return "6_4";
    case D3D_SHADER_MODEL_6_5: return "6_5";
    case D3D_SHADER_MODEL_6_6: return "6_6";
    case D3D_SHADER_MODEL_6_7: return "6_7";
    case D3D_SHADER_MODEL_6_8: return "6_8";
    default: return "below 6_0";
    }
}

// The first hardware adapter that makes a feature level 11_0 device, or with --adapter the first adapter whose
// description contains the text.
HRESULT pick_adapter(IDXGIFactory4* factory, const std::wstring& which, decltype(&D3D12CreateDevice) create,
                     ComPtr<IDXGIAdapter1>& chosen) {
    const std::wstring wanted = lower(which);
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (failed(hr, "IDXGIFactory1::EnumAdapters1")) return hr;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(candidate->GetDesc1(&d))) continue;
        const bool take = wanted.empty()
            ? !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
              SUCCEEDED(create(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))
            : lower(d.Description).find(wanted) != std::wstring::npos;
        if (take) return candidate.As(chosen);
    }
    return DXGI_ERROR_NOT_FOUND;
}

// The subobject stream of the mesh pipeline. Each subobject is its type then its value, aligned to a pointer, as the
// runtime reads D3D12_PIPELINE_STATE_STREAM_DESC (d3d12.h, CD3DX12_PIPELINE_STATE_STREAM).
#pragma warning(push)
#pragma warning(disable : 4324)   // padded by the alignment specifier: that padding is what the runtime reads
template <class T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type> struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    T value{};
};
struct MeshStream {
    Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> as;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    Sub<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    Sub<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    Sub<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
    Sub<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
    Sub<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> formats;
    Sub<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> depth_format;
    Sub<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
};
#pragma warning(pop)

D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// Prints the stored debug layer messages of warning severity and worse to stderr and counts them.
void drain(ID3D12InfoQueue* queue, unsigned& errors, unsigned& warnings) {
    if (!queue) return;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)) || !size) continue;
        std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (FAILED(queue->GetMessage(i, message, &size))) continue;
        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        const bool error = message->Severity != D3D12_MESSAGE_SEVERITY_WARNING;
        (error ? errors : warnings) += 1;
        std::fprintf(stderr, "D3D12 %s #%d: %.*s\n", error ? "ERROR" : "WARNING", static_cast<int>(message->ID),
                     static_cast<int>(message->DescriptionByteLength), message->pDescription);
    }
    const UINT64 lost = queue->GetNumMessagesDiscardedByMessageCountLimit();
    if (lost) {
        std::fprintf(stderr, "D3D12 ERROR: %llu debug layer messages discarded\n", static_cast<unsigned long long>(lost));
        errors += 1;
    }
    queue->ClearStoredMessages();
}

double now() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(frequency.QuadPart);
}

bool wait_fence(ID3D12Fence* fence, UINT64 value, HANDLE event) {
    if (fence->GetCompletedValue() >= value) return true;
    if (failed(fence->SetEventOnCompletion(value, event), "ID3D12Fence::SetEventOnCompletion")) return false;
    if (WaitForSingleObject(event, kFenceTimeoutMs) != WAIT_OBJECT_0) {
        std::fprintf(stderr, "mesh-demo: GPU fence value %llu not reached within %lu ms\n",
                     static_cast<unsigned long long>(value), static_cast<unsigned long>(kFenceTimeoutMs));
        return false;
    }
    return true;
}

// The camera, the frustum planes and the rest of the frame constants for animation time t.
void frame_constants(FrameConstants& f, float t, float aspect, const Options& o) {
    using namespace DirectX;
    const float a = 0.08f * t;
    const XMVECTOR eye = XMVectorSet(120.0f * std::cos(a), 30.0f + 8.0f * std::sin(0.23f * t), 120.0f * std::sin(a), 1.0f);
    const XMVECTOR target = XMVectorSet(40.0f * std::cos(a + 1.1f), 0.0f, 40.0f * std::sin(a + 1.1f), 1.0f);
    const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.5f, 700.0f);
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixMultiply(view, proj));
    std::memcpy(f.view_proj, &m, sizeof(f.view_proj));
    // Row vectors: clip = v * M, so plane k comes from the columns of M (Gribb and Hartmann). Depth runs 0..1.
    const float col[4][4] = {{m._11, m._21, m._31, m._41}, {m._12, m._22, m._32, m._42},
                             {m._13, m._23, m._33, m._43}, {m._14, m._24, m._34, m._44}};
    for (int k = 0; k < 6; ++k) {
        float p[4];
        for (int i = 0; i < 4; ++i) {
            switch (k) {
            case 0: p[i] = col[3][i] + col[0][i]; break;   // left
            case 1: p[i] = col[3][i] - col[0][i]; break;   // right
            case 2: p[i] = col[3][i] + col[1][i]; break;   // bottom
            case 3: p[i] = col[3][i] - col[1][i]; break;   // top
            case 4: p[i] = col[2][i]; break;               // near
            default: p[i] = col[3][i] - col[2][i]; break;  // far
            }
        }
        const float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (int i = 0; i < 4; ++i) f.planes[k][i] = p[i] / length;
    }
    f.eye[0] = XMVectorGetX(eye);
    f.eye[1] = XMVectorGetY(eye);
    f.eye[2] = XMVectorGetZ(eye);
    f.time = t;
    const float sun_length = std::sqrt(0.4f * 0.4f + 0.8f * 0.8f + 0.3f * 0.3f);
    f.sun[0] = 0.4f / sun_length;
    f.sun[1] = 0.8f / sun_length;
    f.sun[2] = 0.3f / sun_length;
    f.object_count = o.grid * o.grid;
    f.fog[0] = 0.030f;
    f.fog[1] = 0.040f;
    f.fog[2] = 0.075f;
    f.grid_side = o.grid;
    f.lod[0] = 50.0f;     // level 1 starts here
    f.lod[1] = 110.0f;    // level 2
    f.lod[2] = 220.0f;    // level 3
    f.lod[3] = 150.0f;    // fog start
    f.spacing = 9.0f;
    f.fog_end = 520.0f;
    f.flags = (g_input.meshlet_colours ? kFlagMeshletColours : 0u) | (o.stats ? kFlagStats : 0u);
    f.pad = 0.0f;
}

int run(const Options& o) {
    // The runtime from System32 and nowhere else.
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\d3d12.dll")) return 3;
    const HMODULE runtime = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) {
        std::fprintf(stderr, "mesh-demo: LoadLibraryExW(System32\\d3d12.dll) failed error=%lu\n", GetLastError());
        return 3;
    }
    auto proc = GetProcAddress(runtime, "D3D12CreateDevice");
    decltype(&D3D12CreateDevice) create = nullptr;
    static_assert(sizeof(create) == sizeof(proc), "function pointer size");
    std::memcpy(&create, &proc, sizeof(create));
    proc = GetProcAddress(runtime, "D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize = nullptr;
    std::memcpy(&serialize, &proc, sizeof(serialize));
    if (!create || !serialize) {
        std::fprintf(stderr, "mesh-demo: d3d12.dll exports D3D12CreateDevice or D3D12SerializeRootSignature missing\n");
        return 3;
    }
    if (o.debug) {
        proc = GetProcAddress(runtime, "D3D12GetDebugInterface");
        decltype(&D3D12GetDebugInterface) get_debug = nullptr;
        std::memcpy(&get_debug, &proc, sizeof(get_debug));
        ComPtr<ID3D12Debug> debug;
        if (!get_debug || failed(get_debug(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface")) return 3;
        debug->EnableDebugLayer();
    }

    ComPtr<IDXGIFactory4> factory;
    if (failed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = pick_adapter(factory.Get(), o.adapter, create, adapter);
    if (FAILED(hr)) {
        std::fprintf(stderr, "mesh-demo: no adapter%s%s hr=0x%08lx\n", o.adapter.empty() ? "" : " matching ",
                     narrow(o.adapter.c_str()).c_str(), static_cast<unsigned long>(hr));
        return 3;
    }
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    ComPtr<ID3D12Device> device;
    if (failed(create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) return 3;
    ComPtr<ID3D12InfoQueue> info;
    if (o.debug && failed(device.As(info), "ID3D12Device::QueryInterface(ID3D12InfoQueue)")) return 1;
    unsigned debug_errors = 0, debug_warnings = 0;

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (failed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)),
               "CheckFeatureSupport(D3D12_OPTIONS7)"))
        return 1;
    if (o.simulate_tier0) options7.MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    D3D_SHADER_MODEL model = static_cast<D3D_SHADER_MODEL>(0);
    for (const D3D_SHADER_MODEL m : {D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                                     D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_0}) {
        D3D12_FEATURE_DATA_SHADER_MODEL ask{m};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &ask, sizeof(ask)))) {
            model = ask.HighestShaderModel;
            break;
        }
    }
    const int tier = static_cast<int>(options7.MeshShaderTier);
    const bool supported = tier >= D3D12_MESH_SHADER_TIER_1 && model >= D3D_SHADER_MODEL_6_5;
    const std::string adapter_name = narrow(adapter_desc.Description);
    std::printf("MESH-DEMO adapter=\"%s\" VEN_%04X DEV_%04X MeshShaderTier=%d HighestShaderModel=%s grid=%u%s\n",
                adapter_name.c_str(), adapter_desc.VendorId, adapter_desc.DeviceId, tier, model_text(model), o.grid,
                o.simulate_tier0 ? " simulate-tier0" : "");
    std::fflush(stdout);

    // The window: a fixed 1280x720 client area in physical pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"bc250-mesh-demo";
    if (!RegisterClassExW(&window_class)) {
        std::fprintf(stderr, "mesh-demo: RegisterClassExW failed error=%lu\n", GetLastError());
        return 1;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    const HWND hwnd = CreateWindowExW(0, window_class.lpszClassName, L"mesh-demo", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "mesh-demo: CreateWindowExW failed error=%lu\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, o.activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT width = client.right > 0 ? static_cast<UINT>(client.right) : kWidth;
    const UINT height = client.bottom > 0 ? static_cast<UINT>(client.bottom) : kHeight;

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (failed(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "ID3D12Device::CreateCommandQueue"))
        return 1;

    // Flip model, 8-bit UNORM back buffers; the pixel program encodes the gamma itself.
    DXGI_SWAP_CHAIN_DESC1 chain_desc{};
    chain_desc.Width = width;
    chain_desc.Height = height;
    chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    chain_desc.SampleDesc = {1, 0};
    chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain_desc.BufferCount = kFrames;
    chain_desc.Scaling = DXGI_SCALING_STRETCH;
    chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    chain_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    ComPtr<IDXGISwapChain1> chain1;
    if (failed(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &chain_desc, nullptr, nullptr, &chain1),
               "IDXGIFactory2::CreateSwapChainForHwnd"))
        return 1;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain3> chain;
    if (failed(chain1.As(chain), "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)")) return 1;

    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrames, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (failed(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)), "ID3D12Device::CreateDescriptorHeap(RTV)"))
        return 1;
    heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (failed(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap)), "ID3D12Device::CreateDescriptorHeap(DSV)"))
        return 1;
    const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> back[kFrames];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[kFrames]{};
    for (UINT i = 0; i < kFrames; ++i) {
        if (failed(chain->GetBuffer(i, IID_PPV_ARGS(&back[i])), "IDXGISwapChain::GetBuffer")) return 1;
        rtv[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += static_cast<SIZE_T>(i) * rtv_size;
        device->CreateRenderTargetView(back[i].Get(), nullptr, rtv[i]);
    }

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                             D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                            D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                              D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    ComPtr<ID3D12Resource> depth;
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc = {1, 0};
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        if (failed(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &d,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)),
                   "ID3D12Device::CreateCommittedResource(depth)"))
            return 1;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

    // Frame constants, one 256-byte slot per frame in flight, mapped for the life of the program. The counters: a
    // UAV buffer zeroed from a zero upload buffer before every frame and copied to that frame's read-back buffer.
    ComPtr<ID3D12Resource> constants, zero, stats;
    ComPtr<ID3D12Resource> readback[kFrames];
    D3D12_RESOURCE_DESC desc = buffer_desc(kConstantsStride * kFrames);
    if (failed(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constants)),
               "ID3D12Device::CreateCommittedResource(constants)"))
        return 1;
    unsigned char* constants_cpu = nullptr;
    D3D12_RANGE none{0, 0};
    if (failed(constants->Map(0, &none, reinterpret_cast<void**>(&constants_cpu)), "ID3D12Resource::Map(constants)"))
        return 1;
    desc = buffer_desc(kStatsBytes);
    if (failed(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&zero)),
               "ID3D12Device::CreateCommittedResource(zero)"))
        return 1;
    {
        void* mapped = nullptr;
        if (failed(zero->Map(0, &none, &mapped), "ID3D12Resource::Map(zero)")) return 1;
        std::memset(mapped, 0, kStatsBytes);
        zero->Unmap(0, nullptr);
    }
    desc = buffer_desc(kStatsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (failed(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&stats)),
               "ID3D12Device::CreateCommittedResource(stats)"))
        return 1;
    desc = buffer_desc(kReadbackBytes);
    for (UINT i = 0; i < kFrames; ++i)
        if (failed(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback[i])),
                   "ID3D12Device::CreateCommittedResource(readback)"))
            return 1;

    ComPtr<ID3D12CommandAllocator> allocator[kFrames];
    for (UINT i = 0; i < kFrames; ++i)
        if (failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator[i])),
                   "ID3D12Device::CreateCommandAllocator"))
            return 1;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator[0].Get(), nullptr,
                                         IID_PPV_ARGS(&list)),
               "ID3D12Device::CreateCommandList"))
        return 1;
    list->Close();
    ComPtr<ID3D12Fence> fence;
    if (failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "ID3D12Device::CreateFence"))
        return 1;
    const HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) return 1;
    UINT64 fence_value = 0;
    UINT64 slot_fence[kFrames]{};

    // The mesh pipeline and the frame timestamps: only where mesh shaders are supported.
    ComPtr<ID3D12GraphicsCommandList6> list6;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12QueryHeap> timestamps;
    UINT64 timestamp_frequency = 0;
    if (supported) {
        if (failed(list.As(list6), "ID3D12GraphicsCommandList::QueryInterface(ID3D12GraphicsCommandList6)")) return 1;
        const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kFrames, 0};
        if (failed(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&timestamps)), "ID3D12Device::CreateQueryHeap"))
            return 1;
        if (failed(queue->GetTimestampFrequency(&timestamp_frequency), "ID3D12CommandQueue::GetTimestampFrequency"))
            return 1;
        ComPtr<ID3D12Device2> device2;
        if (failed(device.As(device2), "ID3D12Device::QueryInterface(ID3D12Device2)")) return 1;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b0: cbuffer Frame
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;   // u0: the counters
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 2;
        root_desc.pParameters = params;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   // no input assembler: a mesh pipeline has none
        ComPtr<ID3DBlob> blob, error;
        hr = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr) && error)
            std::fprintf(stderr, "mesh-demo: %.*s\n", static_cast<int>(error->GetBufferSize()),
                         static_cast<const char*>(error->GetBufferPointer()));
        if (failed(hr, "D3D12SerializeRootSignature")) return 1;
        if (failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
                   "ID3D12Device::CreateRootSignature"))
            return 1;
        MeshStream stream{};
        stream.root.value = root.Get();
        stream.as.value = {g_mesh_demo_as, sizeof(g_mesh_demo_as)};
        stream.ms.value = {g_mesh_demo_ms, sizeof(g_mesh_demo_ms)};
        stream.ps.value = {g_mesh_demo_ps, sizeof(g_mesh_demo_ps)};
        stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.sample_mask.value = UINT_MAX;
        // The mesh program makes every triangle outward-facing and clockwise on screen when seen from outside.
        stream.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
        stream.raster.value.CullMode = D3D12_CULL_MODE_BACK;
        stream.raster.value.FrontCounterClockwise = FALSE;
        stream.raster.value.DepthClipEnable = TRUE;
        stream.depth.value.DepthEnable = TRUE;
        stream.depth.value.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        stream.depth.value.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        stream.formats.value.NumRenderTargets = 1;
        stream.formats.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.depth_format.value = DXGI_FORMAT_D32_FLOAT;
        stream.sample.value = {1, 0};
        const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
        if (failed(device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline)),
                   "ID3D12Device2::CreatePipelineState(mesh)"))
            return 1;
    } else {
        wchar_t title[160]{};
        if (tier < D3D12_MESH_SHADER_TIER_1)
            swprintf_s(title, L"Mesh shaders not supported (MeshShaderTier %d)", tier_number(tier));
        else
            swprintf_s(title, L"Mesh shaders not supported (MeshShaderTier %d, shader model %hs)", tier_number(tier),
                       model_text(model));
        SetWindowTextW(hwnd, title);
        std::printf("MESH-DEMO not supported: MeshShaderTier=%d HighestShaderModel=%s; exit code 4 after %.0f s\n",
                    tier, model_text(model), kUnsupportedSeconds);
        std::fflush(stdout);
    }

    // A plain dark red where nothing can be drawn; the fog colour, gamma encoded, behind the knots.
    const float unsupported_clear[4] = {0.30f, 0.05f, 0.05f, 1.0f};
    FrameConstants f{};
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    frame_constants(f, 0.0f, aspect, o);
    const float clear[4] = {std::pow(f.fog[0], 1.0f / 2.2f), std::pow(f.fog[1], 1.0f / 2.2f),
                            std::pow(f.fog[2], 1.0f / 2.2f), 1.0f};
    const UINT groups = (o.grid * o.grid + kAsGroup - 1) / kAsGroup;

    const double start = now();
    double previous = start, title_time = start, anim = 0.0;
    unsigned long long frames = 0, title_frames = 0, samples = 0, title_samples = 0, gpu_samples = 0, title_gpu_samples = 0;
    double sum_objects = 0, sum_meshlets = 0, sum_gpu_ms = 0;
    double title_objects = 0, title_meshlets = 0, title_lod[4]{}, title_gpu_ms = 0;
    bool recorded[kFrames]{};   // the slot's read-back buffer holds a finished frame
    int exit_code = 0;
    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_input.quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const double t = now();
        if (g_input.quit) break;
        if (o.seconds && t - start >= o.seconds) break;
        if (!supported && t - start >= (o.seconds && o.seconds < kUnsupportedSeconds ? o.seconds : kUnsupportedSeconds))
            break;
        if (!g_input.paused) anim += t - previous;
        previous = t;

        const UINT slot = chain->GetCurrentBackBufferIndex();
        if (!wait_fence(fence.Get(), slot_fence[slot], fence_event)) { exit_code = 1; break; }
        if (recorded[slot]) {
            const D3D12_RANGE read{0, kReadbackBytes};
            unsigned char* mapped = nullptr;
            if (SUCCEEDED(readback[slot]->Map(0, &read, reinterpret_cast<void**>(&mapped)))) {
                Stats s{};
                UINT64 stamp[2]{};
                std::memcpy(&s, mapped, sizeof(s));
                std::memcpy(stamp, mapped + kTimestampOffset, sizeof(stamp));
                readback[slot]->Unmap(0, &none);
                if (o.stats) {
                    ++samples;
                    ++title_samples;
                    sum_objects += s.objects;
                    sum_meshlets += s.meshlets;
                    title_objects += s.objects;
                    title_meshlets += s.meshlets;
                    for (int i = 0; i < 4; ++i) title_lod[i] += s.lod[i];
                }
                if (stamp[1] > stamp[0] && timestamp_frequency) {
                    const double ms = static_cast<double>(stamp[1] - stamp[0]) * 1000.0 / timestamp_frequency;
                    ++gpu_samples;
                    ++title_gpu_samples;
                    sum_gpu_ms += ms;
                    title_gpu_ms += ms;
                }
            }
        }

        if (failed(allocator[slot]->Reset(), "ID3D12CommandAllocator::Reset")) { exit_code = 1; break; }
        if (failed(list->Reset(allocator[slot].Get(), nullptr), "ID3D12GraphicsCommandList::Reset")) { exit_code = 1; break; }
        D3D12_RESOURCE_BARRIER barriers[2] = {
            transition(back[slot].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET), {}};
        list->ResourceBarrier(1, barriers);
        if (supported) {
            frame_constants(f, static_cast<float>(anim), aspect, o);
            std::memcpy(constants_cpu + slot * kConstantsStride, &f, sizeof(f));
            // Buffers decay to COMMON at the end of every ExecuteCommandLists, so each frame starts from there.
            barriers[0] = transition(stats.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, barriers);
            list->CopyBufferRegion(stats.Get(), 0, zero.Get(), 0, kStatsBytes);
            barriers[0] = transition(stats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, barriers);
            list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot);
            list->ClearRenderTargetView(rtv[slot], clear, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->OMSetRenderTargets(1, &rtv[slot], FALSE, &dsv);
            const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(root.Get());
            list->SetPipelineState(pipeline.Get());
            list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress() + slot * kConstantsStride);
            list->SetGraphicsRootUnorderedAccessView(1, stats->GetGPUVirtualAddress());
            list6->DispatchMesh(groups, 1, 1);
            list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot + 1);
            barriers[0] = transition(stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, barriers);
            list->CopyBufferRegion(readback[slot].Get(), 0, stats.Get(), 0, kStatsBytes);
            list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot, 2, readback[slot].Get(),
                                   kTimestampOffset);
        } else {
            list->ClearRenderTargetView(rtv[slot], unsupported_clear, 0, nullptr);
        }
        barriers[0] = transition(back[slot].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, barriers);
        if (failed(list->Close(), "ID3D12GraphicsCommandList::Close")) { exit_code = 1; break; }
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        hr = chain->Present(o.vsync ? 1 : 0, 0);
        if (FAILED(hr)) {
            failed(hr, "IDXGISwapChain::Present");
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                std::fprintf(stderr, "mesh-demo: device removed reason hr=0x%08lx\n",
                             static_cast<unsigned long>(device->GetDeviceRemovedReason()));
            exit_code = 1;
            break;
        }
        if (failed(queue->Signal(fence.Get(), ++fence_value), "ID3D12CommandQueue::Signal")) { exit_code = 1; break; }
        slot_fence[slot] = fence_value;
        recorded[slot] = supported;
        ++frames;
        ++title_frames;

        if (t - title_time >= 0.5) {
            drain(info.Get(), debug_errors, debug_warnings);
            if (supported) {
                const double fps = title_frames / (t - title_time);
                const double gpu_ms = title_gpu_samples ? title_gpu_ms / static_cast<double>(title_gpu_samples) : 0.0;
                wchar_t culling[160] = L"stats off";
                if (title_samples) {
                    const double n = static_cast<double>(title_samples);
                    swprintf_s(culling, L"objects %.0f/%u | meshlets %.0f | %.2f M triangles | LOD %.0f/%.0f/%.0f/%.0f",
                               title_objects / n, o.grid * o.grid, title_meshlets / n,
                               title_meshlets / n * kTrianglesPerMeshlet / 1e6, title_lod[0] / n, title_lod[1] / n,
                               title_lod[2] / n, title_lod[3] / n);
                }
                wchar_t title[400]{};
                swprintf_s(title, L"mesh-demo | %s | MeshShaderTier %d | %.0f fps | GPU %.2f ms | %s%s%s",
                           adapter_desc.Description, tier_number(tier), fps, gpu_ms, culling,
                           g_input.meshlet_colours ? L" | meshlet colours" : L"", g_input.paused ? L" | paused" : L"");
                SetWindowTextW(hwnd, title);
            }
            title_time = t;
            title_frames = title_samples = title_gpu_samples = 0;
            title_objects = title_meshlets = title_gpu_ms = 0;
            for (double& v : title_lod) v = 0;
        }
    }
    const double elapsed = now() - start;

    // Idle before anything is released.
    if (fence_value && !wait_fence(fence.Get(), fence_value, fence_event)) exit_code = 1;
    drain(info.Get(), debug_errors, debug_warnings);
    if (debug_errors) exit_code = 1;
    if (!supported && !exit_code) exit_code = 4;
    if (supported) {
        const double n = samples ? static_cast<double>(samples) : 1.0;
        std::printf("MESH-DEMO frames=%llu seconds=%.2f fps=%.1f gpu_ms_avg=%.3f objects_avg=%.0f/%u meshlets_avg=%.0f "
                    "triangles_avg=%.0f stats=%s\n",
                    frames, elapsed, elapsed > 0 ? frames / elapsed : 0.0,
                    gpu_samples ? sum_gpu_ms / static_cast<double>(gpu_samples) : 0.0, sum_objects / n, o.grid * o.grid,
                    sum_meshlets / n, sum_meshlets / n * kTrianglesPerMeshlet, samples ? "on" : "off");
    }
    if (o.debug) std::printf("DEBUG-LAYER errors=%u warnings=%u\n", debug_errors, debug_warnings);
    std::printf("MESH-DEMO exit=%d\n", exit_code);
    std::fflush(stdout);
    constants->Unmap(0, nullptr);
    CloseHandle(fence_event);
    DestroyWindow(hwnd);
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h" || a == "/?") {
            std::puts(usage);
            return 0;
        } else if (a == "--seconds" && i + 1 < argc) {
            o.seconds = number(argv[++i], 1, 3600);
            if (!o.seconds) { std::fprintf(stderr, "%s\n", usage); return 2; }
        } else if (a == "--adapter" && i + 1 < argc) {
            const int size = MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, nullptr, 0);
            if (size <= 1) { std::fprintf(stderr, "%s\n", usage); return 2; }
            o.adapter.resize(static_cast<size_t>(size));
            MultiByteToWideChar(CP_ACP, 0, argv[i], -1, o.adapter.data(), size);
            o.adapter.resize(static_cast<size_t>(size - 1));
        } else if (a == "--grid" && i + 1 < argc) {
            o.grid = number(argv[++i], 4, 128);
            if (!o.grid) { std::fprintf(stderr, "%s\n", usage); return 2; }
        } else if (a == "--debug") {
            o.debug = true;
        } else if (a == "--vsync") {
            o.vsync = true;
        } else if (a == "--no-stats") {
            o.stats = false;
        } else if (a == "--no-activate") {
            o.activate = false;
        } else if (a == "--simulate-tier0") {
            o.simulate_tier0 = true;
        } else {
            std::fprintf(stderr, "%s\n", usage);
            return 2;
        }
    }
    return run(o);
}
