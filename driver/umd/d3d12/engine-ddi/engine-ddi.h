// SPDX-License-Identifier: MIT
// engine-ddi: D3D12 DDI 0092 slots translated onto the vkd3d-proton engine (amdgpu_wddm_vkd3d.dll).
//
// Boundary r3 (2026-09-28): r2 plus the adapter caps path (query_adapter_caps, build_caps answering GetCaps) on
// engine ABI 1.2 QueryAdapterCaps, after the lab run M768 showed the runtime's first two GetCaps calls. The engine
// side owns this directory; INTEGRATION.md lists what the shell calls and when.
// The shell (namespace native12) owns everything else:
//   - the adapter, device state and FillDDITable composition;
//   - queues and their WDDM contexts, fences and queue Signal/Wait;
//   - allocation and residency callbacks, the DXGI table, present and registration.
// engine-ddi never reads native12::Device. The shell hands over what it needs through ShellHooks and finds the
// DeviceContext of a D3D12DDI_HDEVICE through the ResolveDevice hook.
//
// Engine ABI: bc250_vkd3d_engine.h r3-draft (ABI 1.2), included by path from the vkd3d-proton fork checkout
// pinned in engine-abi.json. engine-ddi uses:
//   - 1.1: CreateDevice in the INLINE queue mode and CreateCommandQueue (queue.cpp);
//   - 1.2 V11 QueryAdapterCaps: the adapter caps of GetCaps (caps.cpp), so the engine must be asked for 1.2.
// Not wired yet: 1.2 V10 CreateHeapFromMemory. RuntimeBacked heap creation still stops after the shell's
// allocation has been validated and handed back (E_NOTIMPL); V10 takes a VkDeviceMemory the shell allocates on
// the engine's VkDevice (GetVulkanHandles), and ImportedMemory below has to follow it.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3d12umddi.h>
#include <vulkan/vulkan_core.h>
#include "bc250_vkd3d_engine.h"
#include <cstdint>

namespace engine_ddi {

inline constexpr uint32_t kBoundaryRevision = 3;

// ---- Memory policy -----------------------------------------------------------------------------------------
// RuntimeBacked is the only mode of the native driver. The memory of every heap, and of every committed
// resource, comes from ShellHooks::allocate_memory: a runtime allocation (pfnAllocateCb_0022) that hosted RADV
// has imported into the engine's VkDevice. If allocation fails, the create fails. There is no fallback to engine
// allocations.
// EnginePrivateTest lets the engine allocate heaps through its own Vulkan device. It exists for the offline
// harness only: create_device_context refuses it unless the translation unit that defines the implementation is
// built with AMDGPU_WDDM_ENGINE_DDI_HARNESS, and the shell's DLL build never defines that macro.
//
// Two memory domains, never mixed:
//   1. Heap memory: one ImportedMemory record per heap (a committed resource has a dedicated heap). It is owned
//      by the shell from the moment allocate_memory succeeds, and engine-ddi hands it back through free_memory.
//   2. Internal memory: what the engine's VkDevice allocates for itself, with no heap or resource behind it
//      (descriptor heap backing, upload rings, scratch, query pools, the fences engine-ddi creates). Its lifetime
//      and residency belong to the device and go through the device's callbacks on the hosted RADV side. No
//      standalone KMT allocation escapes it, and it never becomes an ImportedMemory record. engine-ddi never
//      allocates internal memory itself; it only states this rule.
//
// Heaps, placed and committed resources (pfnCreateHeapAndResource, PFND3D12DDI_CREATEHEAPANDRESOURCE_0088,
// d3d12umddi.h:10894-10897; its fourth argument is the D3D12DDI_HRTRESOURCE the driver uses when it calls back
// into the runtime):
//   - Heap only (heap description, no resource description): one allocate_memory call for the heap.
//   - Committed (both descriptions): a dedicated allocation, a heap made from that memory and a resource placed
//     at offset 0. Exactly one allocate_memory call.
//   - Placed (resource description only, ReuseBufferGPUVA.BaseAddress.UMD.hResource names a resource of the
//     same device): the resource lives in that resource's heap at the base resource's offset plus
//     ReuseBufferGPUVA.BaseAddress.UMD.Offset. It holds a reference on the heap's memory and never calls
//     allocate_memory. Which of these shapes the runtime uses for which API call is unlogged (INFERENCE).
//   - Placement rules, all E_INVALIDARG on violation, checked before the engine is called:
//       * D3D12: the offset is a multiple of the resource's placement alignment from the engine's
//         GetResourceAllocationInfo, and offset + size fits in the heap; the heap's allowed resource categories
//         (D3D12DDI_HEAP_FLAG_BUFFERS, _NON_RT_DS_TEXTURES, _RT_DS_TEXTURES) include the resource's.
//       * Vulkan (RuntimeBacked): ImportedMemory::memory_type_index is one of MemoryRequest::memory_type_bits,
//         which the engine derives from every resource category the heap allows; byte_size covers the request;
//         the allocation's alignment covers MemoryRequest::alignment. A mismatch hands the memory straight back
//         through free_memory and fails the create.
//   - Aliasing follows D3D12: overlapping placed resources are allowed, only one is active at a time, and
//     activation is the application's aliasing barrier, discard or clear. engine-ddi passes aliasing barriers to
//     the engine unchanged and tracks nothing itself.
//   - First RuntimeBacked slice: dedicated allocations only (committed resources, which include Present back
//     buffers). Heap-only heaps and placed resources on runtime memory follow the rules above, but return
//     E_NOTIMPL until the hosted same-storage/VA import is validated.
//
// Release sequence of heap memory, run once when the last user of a heap is destroyed (the heap record and
// every resource placed in it):
//   1. Retirement. A final COM Release does not prove that the GPU has finished. engine-ddi keeps ownership in a
//      deferred-release record until every engine queue of the device has completed all work submitted before
//      the destroy. It learns that from its own fence on each engine queue, signalled after every
//      execute_command_lists. A queue destroyed before retirement is observed resolves its part only if the
//      queue's fence reached the recorded value by the queue's final Release; otherwise the record stays
//      pending for the device's lifetime and counts as live in destroy_device_context.
//   2. The engine's final Release of its heap (and of the heap-wide buffer MapHeap may have created). The engine
//      never frees borrowed memory.
//   3. free_memory: the shell releases its Vulkan import. engine-ddi calls it exactly once per ImportedMemory and
//      never retries, whatever it returns. A failure goes to report_device_error, and the shell keeps its record
//      (with the allocation handle and cookie).
//   4. The runtime deallocation, by the shell, on the same thread, within free_memory or after it.
//   Steps 2 and 3 run only on the thread of a DDI call into the device that owns the memory, while that runtime
//   device exists: the DDI call that makes the last destroy if the work has already retired, otherwise the first
//   later DDI call that observes retirement (execute_command_lists, pfnCreateHeapAndResource,
//   pfnDestroyHeapAndResource, destroy_engine_queue, destroy_device_context). Never from an engine thread or an
//   engine callback: the engine has no threads in INLINE mode, and engine-ddi creates none.
enum class MemoryMode : uint32_t { RuntimeBacked = 1, EnginePrivateTest = 2 };

enum MemoryRequestFlags : uint32_t {
    kMemoryDedicated = 0x1,                     // committed resource: the memory backs exactly this resource
    kMemoryPrimary = 0x2,                       // D3D12DDI_HEAP_FLAG_PRIMARY was set
};

struct MemoryRequest {
    uint32_t size;                              // sizeof(MemoryRequest)
    uint32_t flags;                             // MemoryRequestFlags
    // The runtime owner as the DDI supplied it: the fourth argument of pfnCreateHeapAndResource, the heap's
    // handle for a heap-only call and the pair's for a committed call. engine-ddi passes it on unchanged and
    // never invents one.
    D3D12DDI_HRTRESOURCE rt_owner;
    const D3D12DDIARG_CREATEHEAP_0001* heap;            // never null; valid only during the call
    const D3D12DDIARG_CREATERESOURCE_0088* resource;    // committed: the resource; heap only: null
    uint64_t byte_size;                         // from the engine's allocation info for the resource or heap
    uint64_t alignment;
    uint32_t memory_type_bits;                  // Vulkan memory types the engine accepts for this heap
    uint32_t reserved;                          // 0
};

// One runtime allocation that the shell has made and imported. The shell owns it (see the release sequence).
struct ImportedMemory {
    uint32_t size;                              // sizeof(ImportedMemory)
    uint32_t memory_type_index;                 // index on the engine's physical device
    VkDeviceMemory memory;                      // valid on the engine's VkDevice until free_memory
    uint64_t byte_size;
    D3DKMT_HANDLE allocation;                   // the kernel allocation from pfnAllocateCb_0022
    uint32_t reserved;                          // 0
    // The GPU virtual address of a completed, validated mapping: filled only after MapGpuVirtualAddress and
    // residency have completed. The address AllocateCb reports may be 0 until then. 0 means "no VA": every use
    // that needs a VA fails with E_INVALIDARG. In r2 every heap needs one, so allocate_memory output with
    // gpu_va 0 is handed straight back through free_memory and the create fails with E_INVALIDARG.
    D3DGPU_VIRTUAL_ADDRESS gpu_va;
    void* cookie;                               // shell-private, passed back unchanged
};

// ---- Hooks the shell provides --------------------------------------------------------------------------------
// Hooks are called only on the thread of a DDI call into the device, during that call, and never re-enter
// engine-ddi. Two DDI threads of one device may call hooks at the same time (the runtime calls heap and resource
// creation and destruction concurrently). engine-ddi never holds a lock of its own while it calls a hook.
struct ShellHooks {
    uint32_t size;                              // sizeof(ShellHooks)
    void* shell;                                // passed back unchanged
    // Error reporting. A void slot reports through these. An HRESULT slot returns its code and reports only what
    // the shell's policy asks for (report_device_error decides about sticky device loss).
    void (APIENTRY* report_device_error)(void* shell, HRESULT hr);
    void (APIENTRY* report_list_error)(void* shell, D3D12DDI_HRTCOMMANDLIST list, HRESULT hr);
    BOOL (APIENTRY* is_device_lost)(void* shell);
    // Binds a new runtime command list to the table the shell filled for table_index. The shell resolves the
    // matching hRTTable and calls pfnSetCommandListDDITableCb itself.
    HRESULT (APIENTRY* bind_list_table)(void* shell, D3D12DDI_HRTCOMMANDLIST list, uint32_t table_index);
    // RuntimeBacked heap memory. Both must be set in RuntimeBacked mode and both must be null in
    // EnginePrivateTest. allocate_memory fills *memory only when it succeeds; on failure engine-ddi owns nothing.
    HRESULT (APIENTRY* allocate_memory)(void* shell, const MemoryRequest* request, ImportedMemory* memory);
    HRESULT (APIENTRY* free_memory)(void* shell, const ImportedMemory* memory);
};

// ---- Device context ------------------------------------------------------------------------------------------
// One per D3D12 device. The shell creates it in CreateDevice after the engine device exists, and keeps the
// pointer in its own state. It destroys it in DestroyDevice.
class DeviceContext;

struct ContextCreateInfo {
    uint32_t size;                              // sizeof(ContextCreateInfo)
    uint32_t boundary_revision;                 // kBoundaryRevision the shell was built against
    MemoryMode memory_mode;
    uint32_t ddi_interface;                     // negotiated D3D12DDI interface and version, used to bound
    uint32_t ddi_version;                       //   struct versions and caps payloads
    ID3D12Device* engine_device;                // the context takes its own reference; the caller keeps its own
    const BC250_VKD3D_ENGINE_FUNCS* engine_funcs; // copied
    ShellHooks hooks;                           // copied
};

// Validates everything before it touches the engine. On failure *out is null and nothing is retained.
HRESULT create_device_context(const ContextCreateInfo* info, DeviceContext** out) noexcept;
// First runs the release sequence for heap memory whose work has retired (this may call free_memory and
// report_device_error). Then, if no live object remains, it releases the context's engine reference, frees the
// context and returns S_OK. A live object is a record that holds an engine reference or a heap reference and has
// not been destroyed, or a deferred release still pending. With live objects it returns S_FALSE and their count
// in *live_objects, and changes nothing else: the context and its engine reference stay intact, because those
// records still point to it. The shell may call again later. There is no abandon operation in r2; if terminal
// device loss needs one, it will be a separate reviewed addition that states how every record stops using the
// context.
HRESULT destroy_device_context(DeviceContext* context, uint32_t* live_objects) noexcept;

// ---- Table filling ---------------------------------------------------------------------------------------------
using ResolveDevice = DeviceContext* (APIENTRY*)(D3D12DDI_HDEVICE device);

struct FillInfo {
    uint32_t size;                              // sizeof(FillInfo)
    ResolveDevice resolve;                      // one per process; a second, different resolver is refused
};

// Fills the ENGINE slots and the engine-owned MIXED slots of the core table: heap and resource, command list
// creation, CheckResourceAllocationHandle. The slot list is engine-ddi/SLOTS.md. Every slot this revision does
// not implement gets a fail-safe entry: HRESULT slots return E_NOTIMPL, and void slots report E_NOTIMPL through
// report_device_error; void query slots zero their _Out_ arguments first (INTEGRATION.md, "Query slots around
// CreateDevice"). It never touches RUNTIME slots or shell-owned MIXED slots. It fails with E_INVALIDARG if
// table_size is not sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088).
HRESULT fill_device_core(D3D12DDI_DEVICE_FUNCS_CORE_0088* table, SIZE_T table_size, const FillInfo* info) noexcept;
// Fills all 70 command-list slots for one uTableNum: 0 is the compute table, 1 the graphics table. Graphics-only
// slots in the compute table report E_INVALIDARG through report_list_error. pfnPresent gets a fail-safe entry;
// the shell overrides it with its own Present, which calls resource_allocation.
HRESULT fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, SIZE_T table_size, uint32_t table_index,
                          const FillInfo* info) noexcept;

// ---- Shaders ---------------------------------------------------------------------------------------------------
// Native intake (every create-shader slot): the payload is the bare program with its length in DWORD 1, and
// register-only signature entries. That the buffer holds exactly pShaderCode[1] DWORDs is an INFERENCE from the
// SAL annotation _In_reads_(pShaderCode[1]) on D3D12DDIARG_CREATE_SHADER_0026; no runtime payload has been
// measured. Reads are bounded by it: pShaderCode is checked for null before DWORD 1 is read, a length below 2 is
// refused, and nothing past the declared length is read. engine-ddi builds no container. It copies the declared
// length into the shader's private storage (CalcPrivateShaderSize sizes it from the same DWORD 1), logs at most
// the first four DWORDs that lie within that length, the length and the signature entry counts, and reports
// E_NOTIMPL through report_device_error. The record holds no engine object. Native pipelines that name such a
// shader fail with E_NOTIMPL.
// Harness path, compiled only with AMDGPU_WDDM_ENGINE_DDI_HARNESS: a payload that starts with the "DXBC" magic
// is taken as a complete DXBC or DXIL container (total size in DWORD 6) and handed to the engine unchanged when
// a pipeline uses it. That path exists so that pipelines and dispatches can be tested offline; it proves nothing
// about what the runtime passes.

// ---- Engine parts of shell-owned MIXED slots ---------------------------------------------------------------------
// Queue (CreateCommandQueue/DestroyCommandQueue are the shell's). The shell creates the WDDM context first. The
// engine queue then binds its VkQueue to that context through the engine's BindQueue service; the cookie is
// the value the shell's BindQueue receives. engine-ddi also creates one engine fence per queue (internal memory)
// for the retirement of step 1 of the release sequence.
struct EngineQueue;
HRESULT create_engine_queue(DeviceContext* context, const BC250_VKD3D_COMMAND_QUEUE_DESC* desc, void* queue_cookie,
                            EngineQueue** out) noexcept;
// Releases the engine queue (its final Release waits for the queue's last submission, engine rule V7), records
// how far the queue's fence got, and then runs the release sequence for work that has retired.
void destroy_engine_queue(EngineQueue* queue) noexcept;
// ExecuteCommandLists (the queue table is the shell's). Everything is submitted to the queue's bound context
// before this returns (engine INLINE mode), followed by the signal of the queue's retirement fence. A failure is
// reported through report_device_error and returned.
HRESULT execute_command_lists(EngineQueue* queue, UINT count, const D3D12DDI_HCOMMANDLIST* lists) noexcept;
// Present (the DXGI table is the shell's): the runtime allocation behind a resource. The resource must have
// been created in RuntimeBacked mode as a committed resource (a dedicated allocation), or the call fails.
HRESULT resource_allocation(DeviceContext* context, D3D12DDI_HRESOURCE resource, D3DKMT_HANDLE* allocation,
                            uint64_t* offset) noexcept;

// ---- Capabilities --------------------------------------------------------------------------------------------------
// GetCaps arrives before any device (M768: OpenAdapter12, GetCaps 1074, GetCaps 1007, GetSupportedVersions), so
// its answers come from engine ABI 1.2 QueryAdapterCaps (V11): the engine's CheckFeatureSupport answers for the
// device CreateDevice(info) would make, under the same policy, with no VkDevice. There is no temporary engine
// device and no caps structure of engine-ddi's own: AdapterCaps holds the engine's answers of one batch.
class AdapterCaps;
// Asks the engine once, one QueryAdapterCaps batch (one VkInstance), for the D3D12_FEATURE_* answers build_caps
// maps. info must be the create info the shell will pass to CreateDevice (INLINE queue mode, its Services and
// MinimumFeatureLevel), because admission and policy depend on it. Needs funcs from GetFuncs(1.2) with
// QueryAdapterCaps set, otherwise E_INVALIDARG. Returns the engine's failure when the engine refuses info or a
// required feature is unanswered (INTEGRATION.md lists which); *out is null then.
HRESULT query_adapter_caps(const BC250_VKD3D_ENGINE_FUNCS* funcs, const BC250_VKD3D_DEVICE_CREATE_INFO* info,
                           AdapterCaps** out) noexcept;
void free_adapter_caps(AdapterCaps* caps) noexcept;
// Answers one GetCaps call from the engine's answers or a documented constant (INTEGRATION.md, "GetCaps").
// ddi_version is the build version whose payload layouts the caller expects; this revision knows 92
// (D3D12DDI_BUILD_VERSION_0092) only. It writes only when DataSize is the exact payload size of the type,
// never a prefix; a wrong size or a bad pInfo is E_INVALIDARG with nothing written, and a type it does not
// answer is E_NOTIMPL. Both are logged with the type and the size. Thread-safe: it only reads caps.
HRESULT build_caps(const AdapterCaps* caps, uint32_t ddi_version, const D3D12DDIARG_GETCAPS* request) noexcept;

// ---- Private storage records ------------------------------------------------------------------------------------
// Every engine-ddi object starts with this header, constructed in the runtime-owned storage. Destroy releases
// what the record holds (or hands heap memory to the release sequence) and sets `tag` to Poisoned. The storage
// itself is never freed, because the runtime owns it.
enum class Tag : uint32_t {
    None = 0, Poisoned = 0xDEADDEADu,
    Heap = 0x31504845u, Resource = 0x31534552u, DescriptorHeap = 0x31484344u, RootSignature = 0x31475352u,
    Shader = 0x31524853u, ElementLayout = 0x31594C45u, StateBlend = 0x31444C42u, StateDepth = 0x31505444u,
    StateRaster = 0x31535352u, PipelineState = 0x314F5350u, CommandPool = 0x314C4F50u,
    CommandRecorder = 0x31434552u, CommandList = 0x3154534Cu, QueryHeap = 0x31485951u,
    CommandSignature = 0x31474953u, StateObject = 0x314A4253u,
};
struct RecordHeader {
    Tag tag;
    uint32_t flags;
    IUnknown* engine;                           // one reference, or null for records without an engine object
    DeviceContext* device;                      // creating device; use through another device is refused
};
static_assert(sizeof(RecordHeader) == 24, "RecordHeader layout");

} // namespace engine_ddi
