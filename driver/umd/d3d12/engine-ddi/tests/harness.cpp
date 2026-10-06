// SPDX-License-Identifier: MIT
// engine-ddi-harness main: loads the engine DLL like the shell does, creates an INLINE engine device on this PC's
// GPU, composes the DDI tables, runs the round trips and tears everything down. Development PC only.
//
//   engine-ddi-harness.exe --engine <amdgpu_wddm_vkd3d.dll> [--adapter <substring of the DXGI description>]
//                          [--deferred-replay]
//
// --deferred-replay turns the replay policy on for every device context the run opens (open_device), as the
// shell's deferred-replay experiment does: the recording slots' engine calls run on replay workers.
//
// The direct entry (engine-ddi.h, "Direct entry") is installed over the graphics table's slots as in the driver, and
// every device context is admitted (as the shell admits one with its recording binding): with --deferred-replay the
// round trips' value-only calls go through it (without, they all go to the slots). The run has no Present, so the
// first arm of BC250_ENTRY_PATH holds throughout (a: every call to the slots); BC250_ENTRY_STATS=1 checks the count.
#include "harness.h"
#include "entry.h"
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace harness {

namespace {
int g_failures = 0;
bool g_replay = false;                  // --deferred-replay
constexpr SIZE_T kCanaryBytes = 32;
constexpr uint8_t kFill = 0xCD;
constexpr uint8_t kCanary = 0xA5;
} // namespace

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

void checkf(bool ok, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    check(ok, text);
}

int failure_count() { return g_failures; }

namespace {
SRWLOCK g_lines_lock = SRWLOCK_INIT;
std::vector<std::string> g_lines;       // every log_refusal line, from any thread

void record_line(const char* text, void*) {
    AcquireSRWLockExclusive(&g_lines_lock);
    g_lines.emplace_back(text);
    ReleaseSRWLockExclusive(&g_lines_lock);
}
} // namespace

std::vector<std::string> refusal_lines(const char* prefix) {
    std::vector<std::string> out;
    const size_t n = std::strlen(prefix);
    AcquireSRWLockShared(&g_lines_lock);
    for (const std::string& line : g_lines)
        if (!line.compare(0, n, prefix)) out.push_back(line);
    ReleaseSRWLockShared(&g_lines_lock);
    return out;
}

// ---- Storage ---------------------------------------------------------------------------------------------------------
Storage::~Storage() {
    for (const Block& b : blocks_) delete[] b.p;
}

void* Storage::alloc(SIZE_T size) {
    if (!size || size > (SIZE_T{1} << 20)) return nullptr;
    auto* p = new (std::nothrow) uint8_t[size + kCanaryBytes];
    if (!p) return nullptr;
    std::memset(p, kFill, size);
    std::memset(p + size, kCanary, kCanaryBytes);
    blocks_.push_back({p, size});
    return p;
}

bool Storage::canaries_intact() const {
    for (const Block& b : blocks_)
        for (SIZE_T i = 0; i < kCanaryBytes; ++i)
            if (b.p[b.size + i] != kCanary) return false;
    return true;
}

// ---- Shell hooks -------------------------------------------------------------------------------------------------
int Shell::table_of(D3D12DDI_HRTCOMMANDLIST list) const {
    for (const Bind& b : binds)
        if (b.list == list.handle) return static_cast<int>(b.table);
    return -1;
}

namespace {
void APIENTRY report_device_error(void* shell, HRESULT hr) {
    auto* s = static_cast<Shell*>(shell);
    ++s->device_errors;
    s->last_device_error = hr;
    std::printf("     shell: device error %08lx\n", static_cast<unsigned long>(hr));
}

void APIENTRY report_list_error(void* shell, D3D12DDI_HRTCOMMANDLIST list, HRESULT hr) {
    auto* s = static_cast<Shell*>(shell);
    ++s->list_errors;
    s->last_list_error = hr;
    s->last_list = list.handle;
    std::printf("     shell: command list error %08lx\n", static_cast<unsigned long>(hr));
}

BOOL APIENTRY is_device_lost(void*) { return FALSE; }

// The shell's replay hooks without its scopes: the harness has no runtime domain or hosted dispatch to enter.
void APIENTRY replay_worker(void*, engine_ddi::ReplayBody body, void* ring) { body(ring); }
void APIENTRY replay_drained(void*) {}

HRESULT APIENTRY bind_list_table(void* shell, D3D12DDI_HRTCOMMANDLIST list, uint32_t table) {
    static_cast<Shell*>(shell)->binds.push_back({list.handle, table});
    return S_OK;
}

engine_ddi::DeviceContext* APIENTRY resolve_device(D3D12DDI_HDEVICE device) {
    auto* sd = static_cast<ShellDevice*>(device.pDrvPrivate);
    return (sd && sd->magic == kShellDeviceMagic) ? sd->context : nullptr;
}

// Engine services of the INLINE queue mode. The shell binds each VkQueue to its WDDM context here; the harness
// has none and only counts.
LONG g_binds = 0;
LONG g_unbinds = 0;
HRESULT APIENTRY bind_queue(void*, void*, VkQueue queue) {
    InterlockedIncrement(&g_binds);
    return queue ? S_OK : E_INVALIDARG;
}
void APIENTRY unbind_queue(void*, void*, VkQueue) { InterlockedIncrement(&g_unbinds); }

bool find_adapter(const wchar_t* filter, LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!filter || std::wcsstr(desc.Description, filter))) {
            std::printf("adapter: %ls, LUID %08lx:%08lx\n", desc.Description,
                        static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                        static_cast<unsigned long>(desc.AdapterLuid.LowPart));
            luid = desc.AdapterLuid;
            found = true;
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}
} // namespace

HRESULT open_device(Env& env, Device& device, decltype(engine_ddi::ShellHooks::allocate_memory) allocate_memory,
                    decltype(engine_ddi::ShellHooks::free_memory) free_memory, ID3D12Device* engine) {
    engine_ddi::ContextCreateInfo info{};
    info.size = sizeof(info);
    info.boundary_revision = engine_ddi::kBoundaryRevision;
    info.memory_mode = allocate_memory ? engine_ddi::MemoryMode::RuntimeBacked : engine_ddi::MemoryMode::EnginePrivateTest;
    info.hooks.allocate_memory = allocate_memory;
    info.hooks.free_memory = free_memory;
    info.ddi_interface = D3D12DDI_INTERFACE_VERSION_R8;
    info.ddi_version = D3D12DDI_BUILD_VERSION_0092;
    info.engine_device = engine ? engine : env.engine;
    info.engine_funcs = &env.funcs;
    info.hooks.size = sizeof(info.hooks);
    info.hooks.shell = &device.shell;
    info.hooks.report_device_error = report_device_error;
    info.hooks.report_list_error = report_list_error;
    info.hooks.is_device_lost = is_device_lost;
    info.hooks.bind_list_table = bind_list_table;
    HRESULT hr = engine_ddi::create_device_context(&info, &device.context);
    device.sd.context = device.context;
    if (hr == S_OK) engine_ddi::set_direct_entry(device.context, true);
    if (hr == S_OK && g_replay) {
        const engine_ddi::ReplayPolicy policy{sizeof(policy), 1, 8, 4u << 20, &device.shell, replay_worker,
                                              replay_drained};
        hr = engine_ddi::set_replay_policy(device.context, &policy);
        if (hr != S_OK)
            checkf(false, "deferred replay: policy on for a device context (hr %08lx)", static_cast<unsigned long>(hr));
    }
    return hr;
}

// ---- Runtime-side helpers ----------------------------------------------------------------------------------------
namespace {
HRESULT create_buffer_sized(Env& env, Device& device, HeapKind kind, UINT64 size, bool uav, UINT64 heap_bytes,
                            Buffer& out);
} // namespace

HRESULT create_buffer(Env& env, Device& device, HeapKind kind, UINT64 size, bool uav, Buffer& out) {
    return create_buffer_sized(env, device, kind, size, uav, 0, out);
}

HRESULT create_buffer_in_heap_of(Env& env, Device& device, HeapKind kind, UINT64 size, UINT64 heap_bytes, Buffer& out) {
    return create_buffer_sized(env, device, kind, size, false, heap_bytes, out);
}

HRESULT create_heap_alone(Env& env, Device& device, HeapKind kind, UINT64 heap_bytes, Buffer& out) {
    out = Buffer{};
    static const D3D12_HEAP_TYPE types[] = {D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_READBACK};
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, types[static_cast<int>(kind)]);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = heap_bytes;
    heap.Alignment = 64 * 1024;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &heap, nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.heap = env.storage.alloc(sizes.Heap);
    if (!out.heap) return E_OUTOFMEMORY;
    const HRESULT hr = env.core.pfnCreateHeapAndResource(device.h(), &heap, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt},
                                                         nullptr, nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{},
                                                         D3D12DDI_HRESOURCE{});
    if (FAILED(hr)) out.heap = nullptr;                 // nothing was constructed: nothing to destroy
    return hr;
}

namespace {
HRESULT create_buffer_sized(Env& env, Device& device, HeapKind kind, UINT64 size, bool uav, UINT64 heap_bytes,
                            Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_BUFFER;
    res.Width = size;
    res.Height = 1;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_UNKNOWN;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_ROW_MAJOR;
    res.Flags = uav ? D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS : D3D12DDI_RESOURCE_FLAG_0003_NONE;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;

    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;

    // The runtime turns the API heap type into CPU page property and memory pool for this adapter; the engine's
    // GetCustomHeapProperties gives the same answer (API values are the DDI values plus one).
    static const D3D12_HEAP_TYPE types[] = {D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_READBACK};
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, types[static_cast<int>(kind)]);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = heap_bytes ? heap_bytes : info.ResourceDataSize;
    heap.Alignment = info.ResourceDataAlignment;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &heap, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    const HRESULT hr = env.core.pfnCreateHeapAndResource(device.h(), &heap, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt},
                                                         &res, nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{},
                                                         out.hres());
    if (FAILED(hr) && heap_bytes) out.heap = out.resource = nullptr;    // refused by size: nothing was constructed
    return hr;
}
} // namespace

HRESULT create_placed_buffer(Env& env, Device& device, const Buffer& base, UINT64 offset, UINT64 size, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ReuseBufferGPUVA.BaseAddress.UMD = {base.hres(), offset};
    res.ResourceType = D3D12DDI_RT_BUFFER;
    res.Width = size;
    res.Height = 1;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_UNKNOWN;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_ROW_MAJOR;
    res.Flags = D3D12DDI_RESOURCE_FLAG_0003_NONE;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), nullptr, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.resource) return E_OUTOFMEMORY;
    return env.core.pfnCreateHeapAndResource(device.h(), nullptr, D3D12DDI_HHEAP{}, D3D12DDI_HRTRESOURCE{&out.rt}, &res,
                                             nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
}

void destroy_buffer(Env& env, Device& device, Buffer& buffer) {
    if (buffer.heap || buffer.resource) env.core.pfnDestroyHeapAndResource(device.h(), buffer.hheap(), buffer.hres());
    buffer.heap = buffer.resource = nullptr;
}

void write_default_state(Env& env, Device& device, Recording& r);

HRESULT open_recording(Env& env, Device& device, D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags, Recording& out,
                       D3D12DDI_COMMAND_LIST_TYPE type) {
    out = Recording{};
    D3D12DDIARG_CREATE_COMMAND_POOL_0040 pool{D3D12DDI_COMMAND_POOL_FLAG_NONE};
    out.pool = env.storage.alloc(env.core.pfnCalcPrivateCommandPoolSize(device.h(), &pool));
    HRESULT hr = out.pool ? env.core.pfnCreateCommandPool(device.h(), &pool, D3D12DDI_HCOMMANDPOOL_0040{out.pool})
                          : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        out.pool = nullptr;
        return hr;
    }
    D3D12DDIARG_CREATE_COMMAND_RECORDER_0040 recorder{queue_flags, D3D12DDI_COMMAND_RECORDER_FLAG_NONE};
    out.recorder = env.storage.alloc(env.core.pfnCalcPrivateCommandRecorderSize(device.h(), &recorder));
    hr = out.recorder ? env.core.pfnCreateCommandRecorder(device.h(), &recorder,
                                                          D3D12DDI_HCOMMANDRECORDER_0040{out.recorder})
                      : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        out.recorder = nullptr;
        return hr;
    }
    env.core.pfnCommandRecorderSetCommandPoolAsTarget(device.h(), D3D12DDI_HCOMMANDRECORDER_0040{out.recorder},
                                                      D3D12DDI_HCOMMANDPOOL_0040{out.pool});
    D3D12DDIARG_CREATE_COMMAND_LIST_0040 list{};
    list.Type = type;
    list.QueueFlags = queue_flags;
    list.ID = 1;
    list.CommandListFlags = D3D12DDI_COMMAND_LIST_FLAG_NONE;
    list.NodeMask = 0;
    out.list = env.storage.alloc(env.core.pfnCalcPrivateCommandListSize(device.h(), &list));
    hr = out.list ? env.core.pfnCreateCommandList(device.h(), &list, out.hlist(), out.rtlist()) : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        out.list = nullptr;
        return hr;
    }
    const int table = device.shell.table_of(out.rtlist());
    if (table < 0) return E_FAIL;                       // the shell never bound the list to a table
    out.table = static_cast<uint32_t>(table);
    // The list starts closed; ResetCommandList opens it on the recorder's pool, as the runtime's does after
    // creating a list.
    D3D12DDIARG_RESETCOMMANDLIST_0040 reset{D3D12DDI_HCOMMANDRECORDER_0040{out.recorder}, 1,
                                           D3D12DDI_COMMAND_LIST_FLAG_NONE};
    env.lists[out.table].pfnResetCommandList(out.hlist(), &reset);
    if (type != D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) write_default_state(env, device, out);
    return S_OK;
}

// The state the runtime writes into a list it has reset. The first thirteen calls and their order were
// observed on a direct list under the system runtime; the arguments were not. Scalar values are the API's
// documented defaults; null pointers and zero counts are this harness's choice of an unbound state. The
// calls after the thirteenth are the remaining state slots, in no observed order, and the alpha blend
// factor is an unused-slot exercise rather than reset state. A list of
// the compute table gets the calls that are legal there. Any error they report fails the recording.
void write_default_state(Env& env, Device& device, Recording& r) {
    const auto& t = env.lists[r.table];
    const bool graphics = r.table == 1;
    const uint32_t device_before = device.shell.device_errors;
    const uint32_t list_before = device.shell.list_errors;
    const FLOAT blend[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    t.pfnSetPipelineState(r.hlist(), D3D12DDI_HPIPELINESTATE{nullptr});
    if (graphics) t.pfnIaSetTopology(r.hlist(), D3D12DDI_PRIMITIVE_TOPOLOGY_UNDEFINED);
    t.pfnSetDescriptorHeaps(r.hlist(), 0, nullptr);
    if (graphics) {
        t.pfnIASetVertexBuffers(r.hlist(), 0, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, nullptr);
        t.pfnIASetIndexBuffer(r.hlist(), nullptr);
        t.pfnSOSetTargets(r.hlist(), 0, D3D12_SO_BUFFER_SLOT_COUNT, nullptr);
        t.pfnOMSetRenderTargets(r.hlist(), 0, nullptr, FALSE, nullptr);
        t.pfnRsSetViewports(r.hlist(), 0, nullptr);
        t.pfnRsSetScissorRects(r.hlist(), 0, nullptr);
        t.pfnOmSetBlendFactor(r.hlist(), blend);
        t.pfnOmSetStencilRef(r.hlist(), 0);
    }
    t.pfnSetPredication(r.hlist(), D3D12DDI_HRESOURCE{nullptr}, 0, D3D12DDI_PREDICATION_OP_EQUAL_ZERO);
    t.pfnClearRootArguments(r.hlist());
    t.pfnSetProtectedResourceSession(r.hlist(), D3D12DDI_HPROTECTEDRESOURCESESSION_0030{nullptr});
    if (graphics) {
        t.pfnOMSetDepthBounds(r.hlist(), 0.0f, 1.0f);
        t.pfnSetSamplePositions(r.hlist(), 0, 0, nullptr);
        t.pfnSetViewInstanceMask(r.hlist(), 0);
        t.pfnRSSetShadingRate(r.hlist(), D3D12DDI_SHADING_RATE_0062_1X1, nullptr);
        t.pfnRSSetShadingRateImage(r.hlist(), D3D12DDI_HRESOURCE{nullptr});
        t.pfnOmSetAlphaBlendFactor(r.hlist(), 1.0f);
    }
    checkf(device.shell.device_errors == device_before && device.shell.list_errors == list_before,
           "default state of a reset list, table %u: no device or list error (%u device, %u list)", r.table,
           device.shell.device_errors - device_before, device.shell.list_errors - list_before);
}

void destroy_recording(Env& env, Device& device, Recording& r) {
    if (r.list) env.core.pfnDestroyCommandList(device.h(), r.hlist());
    if (r.recorder) env.core.pfnDestroyCommandRecorder(device.h(), D3D12DDI_HCOMMANDRECORDER_0040{r.recorder});
    if (r.pool) env.core.pfnDestroyCommandPool(device.h(), D3D12DDI_HCOMMANDPOOL_0040{r.pool});
    r.list = r.recorder = r.pool = nullptr;
}

bool wait_queue_idle(Env& env, engine_ddi::EngineQueue* queue, const char* what) {
    ID3D12CommandQueue* q = engine_ddi::harness_engine_queue(queue);
    ID3D12Fence* fence = nullptr;
    HRESULT hr = env.engine->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (SUCCEEDED(hr)) hr = q->Signal(fence, 1);
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, nullptr);
    const UINT64 completed = SUCCEEDED(hr) ? fence->GetCompletedValue() : 0;
    const bool done = SUCCEEDED(hr) && fence_reached(completed, 1);
    if (fence) fence->Release();
    checkf(done,
           "%s: engine fence signalled on the engine queue completes (SetEventOnCompletion(1, NULL) hr %08lx, "
           "completed value %llu)",
           what, static_cast<unsigned long>(hr), static_cast<unsigned long long>(completed));
    return done;
}

bool fence_reached(UINT64 completed, UINT64 target) {
    return completed != UINT64_MAX && completed >= target;
}

D3D12DDIARG_RESOURCE_BARRIER_0022 transition(const Buffer& buffer, D3D12DDI_RESOURCE_STATES before,
                                             D3D12DDI_RESOURCE_STATES after) {
    D3D12DDIARG_RESOURCE_BARRIER_0022 b{};
    b.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12DDI_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.hResource = buffer.hres();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

void test_private_instances(Env& env, const BC250_VKD3D_DEVICE_CREATE_INFO& create, Device& first) {
    ID3D12Device* engine = nullptr;
    HRESULT hr = env.funcs.CreateDevice(&create, __uuidof(ID3D12Device), reinterpret_cast<void**>(&engine));
    checkf(SUCCEEDED(hr) && engine, "private instances: a second engine device from the same create info (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (FAILED(hr) || !engine) return;
    Device second;
    hr = open_device(env, second, nullptr, nullptr, engine);
    VkInstance a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    VkPhysicalDevice pa = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkDevice da = VK_NULL_HANDLE, db = VK_NULL_HANDLE;
    uint32_t fa = 0, fb = 0;
    const HRESULT ha = env.funcs.GetVulkanHandles(env.engine, &a, &pa, &da, &fa);
    const HRESULT hb = env.funcs.GetVulkanHandles(engine, &b, &pb, &db, &fb);
    checkf(hr == S_OK && first.context && second.context && ha == S_OK && hb == S_OK && a && b && a != b && da != db,
           "private instances: two live engine-ddi devices report two VkInstances through GetVulkanHandles (%p, %p)",
           static_cast<void*>(a), static_cast<void*>(b));
    if (second.context) {
        uint32_t live = UINT32_MAX;
        hr = engine_ddi::destroy_device_context(second.context, &live);
        checkf(hr == S_OK && live == 0, "private instances: the second device context goes (hr %08lx, %u live)",
               static_cast<unsigned long>(hr), live);
    }
    engine->Release();
}

} // namespace harness

using namespace harness;

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const wchar_t* engine_path = nullptr;
    const wchar_t* adapter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--engine") && i + 1 < argc) {
            engine_path = argv[++i];
        } else if (!std::wcscmp(argv[i], L"--adapter") && i + 1 < argc) {
            adapter = argv[++i];
        } else if (!std::wcscmp(argv[i], L"--deferred-replay")) {
            g_replay = true;
        } else {
            std::printf("FAIL  unknown or incomplete option %ls\n", argv[i]);
            return 2;
        }
    }
    if (!engine_path) {
        std::printf("usage: engine-ddi-harness --engine <amdgpu_wddm_vkd3d.dll> [--adapter <substring>] "
                    "[--deferred-replay]\n");
        return 2;
    }
    std::printf("deferred replay: %s\n", g_replay ? "on for every device context" : "off");
    engine_ddi::harness_set_log_observer(record_line, nullptr);

    // The engine keeps its disk shader cache in %LOCALAPPDATA%\amdgpu-wddm\vkd3d. The harness points LOCALAPPDATA
    // at localappdata beside itself, before the engine's first device reads it, so no run writes to the profile;
    // without that directory the cache is off (VKD3D_SHADER_CACHE_PATH=0).
    wchar_t full[MAX_PATH];
    const DWORD self = GetModuleFileNameW(nullptr, full, MAX_PATH);
    wchar_t* slash = self && self < MAX_PATH ? std::wcsrchr(full, L'\\') : nullptr;
    if (slash && wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - full), L"localappdata") == 0 &&
        (CreateDirectoryW(full, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS))
        SetEnvironmentVariableW(L"LOCALAPPDATA", full);
    else
        SetEnvironmentVariableW(L"VKD3D_SHADER_CACHE_PATH", L"0");

    // The shell's loads: an absolute path, dependencies from the DLL's own directory and System32 only.
    HMODULE engine_dll = nullptr;
    if (GetFullPathNameW(engine_path, MAX_PATH, full, nullptr))
        engine_dll = LoadLibraryExW(full, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE vulkan = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto gipa = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                             reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")))
                       : nullptr;
    auto get_funcs = engine_dll ? reinterpret_cast<PFN_BC250_VKD3D_ENGINE_GET_FUNCS>(reinterpret_cast<void*>(
                                      GetProcAddress(engine_dll, BC250_VKD3D_ENGINE_GET_FUNCS_NAME)))
                                : nullptr;
    LUID luid{};
    if (!engine_dll || !get_funcs || !gipa || !find_adapter(adapter, luid)) {
        std::printf("FAIL  setup: engine %s, export %s, vkGetInstanceProcAddr %s, adapter %s\n",
                    engine_dll ? "loaded" : "not loaded", get_funcs ? "found" : "missing", gipa ? "found" : "missing",
                    adapter ? "not matched" : "none");
        return 1;
    }

    Env env;
    env.gipa = gipa;
    env.funcs.Size = sizeof(env.funcs);
    HRESULT hr = get_funcs(BC250_VKD3D_ENGINE_ABI_VERSION, &env.funcs);
    checkf(hr == S_OK && env.funcs.CreateDevice && env.funcs.CreateCommandQueue,
           "engine ABI %u.%u: Bc250Vkd3dEngineGetFuncs fills CreateDevice and CreateCommandQueue",
           BC250_VKD3D_ENGINE_ABI_MAJOR, BC250_VKD3D_ENGINE_ABI_MINOR);
    if (hr != S_OK) return 1;

    BC250_VKD3D_SHELL_SERVICES services{};
    services.Size = sizeof(services);
    services.BindQueue = bind_queue;
    services.UnbindQueue = unbind_queue;
    BC250_VKD3D_DEVICE_CREATE_INFO create{};
    create.Size = sizeof(create);
    create.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    create.GetInstanceProcAddr = gipa;
    create.AdapterLuid = luid;
    create.MinimumFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    create.QueueMode = BC250_VKD3D_QUEUE_MODE_INLINE;
    create.Services = &services;
    create.InstanceMode = BC250_VKD3D_INSTANCE_MODE_PRIVATE;
    hr = env.funcs.CreateDevice(&create, __uuidof(ID3D12Device), reinterpret_cast<void**>(&env.engine));
    checkf(SUCCEEDED(hr) && env.engine, "engine CreateDevice in the INLINE queue mode, PRIVATE instance (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (FAILED(hr) || !env.engine) return 1;

    // Table composition, as the shell's FillDDITable will do it: engine-ddi first, the shell's slots after.
    const engine_ddi::FillInfo fill{sizeof(engine_ddi::FillInfo), resolve_device};
    check(engine_ddi::fill_device_core(&env.core, sizeof(env.core), &fill) == S_OK &&
              engine_ddi::fill_command_list(&env.lists[0], sizeof(env.lists[0]), 0, &fill) == S_OK &&
              engine_ddi::fill_command_list(&env.lists[1], sizeof(env.lists[1]), 1, &fill) == S_OK,
          "core table and both command-list tables filled");
    const bool direct = engine_ddi::install_direct_list(&env.lists[1], 1);
    std::printf("info  entry path: direct entries %s\n", direct ? "installed over the graphics table's slots" : "off");
    // wait_queue_idle once took a removed device's UINT64_MAX for completion.
    check(fence_reached(1, 1) && fence_reached(7, 1) && !fence_reached(0, 1) && !fence_reached(UINT64_MAX, 1),
          "wait_queue_idle: a completed value reaches its target only when it is at least the target and not "
          "UINT64_MAX (a removed device)");

    Device device;
    hr = open_device(env, device);
    checkf(hr == S_OK && device.context, "device context in EnginePrivateTest mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr == S_OK) {
        test_private_instances(env, create, device);
        test_copy(env, device);
        test_copy_slices(env, device);
        test_copy_bc_volume(env, device);
        test_stored_formats(env, device);
        test_compute(env, device);
        test_graphics(env, device);
        test_draw_path(env, device);
        test_device_queries(env, device);
        uint32_t live = UINT32_MAX;
        hr = engine_ddi::destroy_device_context(device.context, &live);
        checkf(hr == S_OK && live == 0, "destroy_device_context: S_OK with no live object (hr %08lx, live %u)",
               static_cast<unsigned long>(hr), live);
        checkf(!device.shell.device_errors && !device.shell.list_errors,
               "positive paths: no device or command-list error reported (%u device, %u list)",
               device.shell.device_errors, device.shell.list_errors);
    }
    test_log_lines(env);
    test_retirement(env);
    test_retire_handoff(env);
    test_runtime_backed(env);
    test_tiled(env);
    test_small_placement(env);
    test_linear_primary(env);
    test_raytracing(env);
    test_raytracing_pipeline(env);
    test_memory_policy(env, create);
    test_disk_cache(env, create, engine_dll);
    // With the direct arm, replay and the statistics on, the round trips' value-only calls went through the direct
    // entries (this thread's block counts them); the misses are those record_direct left to the slots.
    if (direct && g_replay && engine_ddi::entry_direct_arm() && engine_ddi::entry_stats_on()) {
        const engine_ddi::EntryThread* t = engine_ddi::t_entry;
        const auto n = [&](engine_ddi::DirectMiss m) {
            return t ? static_cast<unsigned long long>(t->misses[static_cast<uint32_t>(m)].load()) : 0ull;
        };
        checkf(t && t->direct.load() > 1000,
               "entry path: the direct entries recorded %llu calls on the main thread (misses: record %llu, admission "
               "%llu, replay %llu, ring %llu, switch %llu, encode %llu)",
               t ? static_cast<unsigned long long>(t->direct.load()) : 0ull, n(engine_ddi::DirectMiss::Record),
               n(engine_ddi::DirectMiss::Admission), n(engine_ddi::DirectMiss::Replay), n(engine_ddi::DirectMiss::Ring),
               n(engine_ddi::DirectMiss::Switch), n(engine_ddi::DirectMiss::Encode));
    }
    check(env.storage.canaries_intact(), "private storage: every canary behind the driver's size intact");
    checkf(g_binds >= 1 && g_binds >= g_unbinds, "engine services: %ld BindQueue, %ld UnbindQueue", g_binds, g_unbinds);
    env.engine->Release();
    env.engine = nullptr;
    std::printf("%s\n", failure_count() ? "FAILED" : "PASSED");
    return failure_count() ? 1 : 0;
}
