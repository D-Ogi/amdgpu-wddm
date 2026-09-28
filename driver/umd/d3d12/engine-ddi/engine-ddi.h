// SPDX-License-Identifier: MIT
// engine-ddi: D3D12 DDI 0092 slots translated onto the vkd3d-proton engine (amdgpu_wddm_vkd3d.dll).
//
// Boundary draft r1, for review before any slot is implemented. The engine side owns this directory.
// The shell (namespace native12) owns everything else:
//   - the adapter, device state and FillDDITable composition;
//   - queues and their WDDM contexts, fences and queue Signal/Wait;
//   - allocation and residency callbacks, the DXGI table, present and registration.
// engine-ddi never reads native12::Device. The shell hands over what it needs through ShellHooks and finds the
// DeviceContext of a D3D12DDI_HDEVICE through the ResolveDevice hook.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3d12umddi.h>
#include <vulkan/vulkan_core.h>
#include "bc250_vkd3d_engine.h"
#include <cstdint>

namespace engine_ddi {

inline constexpr uint32_t kBoundaryRevision = 1;

// ---- Memory policy -----------------------------------------------------------------------------------------
// RuntimeBacked is the only mode of the native driver. Every heap, and the memory of every committed or placed
// resource created through CreateHeapAndResource, comes from ShellHooks::allocate_memory: a runtime allocation
// (pfnAllocateCb_0022) that hosted RADV has imported into the engine's VkDevice. If allocation fails, the create
// fails. There is no fallback to engine allocations.
// EnginePrivateTest lets the engine allocate through its own Vulkan device. It exists for the offline harness
// only: create_device_context refuses it unless the translation unit that defines the implementation is built
// with AMDGPU_WDDM_ENGINE_DDI_HARNESS, and the shell's DLL build never defines that macro.
// Memory that is not an API heap or resource memory stays engine-internal: descriptor heap backing, upload
// rings, scratch. Hosted RADV allocates it through the same runtime callbacks, but with no hRTResource.
enum class MemoryMode : uint32_t { RuntimeBacked = 1, EnginePrivateTest = 2 };

struct MemoryRequest {
    uint32_t size;                              // sizeof(MemoryRequest)
    uint32_t reserved;                          // 0
    D3D12DDI_HRTRESOURCE rt_resource;           // runtime resource this memory backs; null for heap-only memory
    const D3D12DDIARG_CREATEHEAP_0001* heap;    // the runtime's heap description, valid only during the call
    uint64_t byte_size;                         // from the engine's allocation info for the resource or heap
    uint64_t alignment;
    uint32_t memory_type_bits;                  // Vulkan memory types the engine accepts for this memory
};

// One runtime allocation that the shell has made and imported. The shell owns it; engine-ddi hands it back
// through free_memory exactly once, after the engine has released every object that uses it.
struct ImportedMemory {
    uint32_t size;                              // sizeof(ImportedMemory)
    uint32_t memory_type_index;                 // index on the engine's physical device
    VkDeviceMemory memory;                      // valid on the engine's VkDevice until free_memory
    uint64_t byte_size;
    D3DKMT_HANDLE allocation;                   // the kernel allocation from pfnAllocateCb_0022
    D3DGPU_VIRTUAL_ADDRESS gpu_va;              // as the allocate callback reported it
    void* cookie;                               // shell-private, passed back unchanged
};

// ---- Hooks the shell provides --------------------------------------------------------------------------------
// Each hook is called only on the thread of the DDI call that caused it. Hooks never re-enter engine-ddi.
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
    // RuntimeBacked memory. Both must be set in RuntimeBacked mode and both must be null in EnginePrivateTest.
    HRESULT (APIENTRY* allocate_memory)(void* shell, const MemoryRequest* request, ImportedMemory* memory);
    void (APIENTRY* free_memory)(void* shell, const ImportedMemory* memory);
};

// ---- Device context ------------------------------------------------------------------------------------------
// One per D3D12 device. The shell creates it in CreateDevice after the engine device exists, and keeps the
// pointer in its own state. It destroys it in DestroyDevice after the last engine-ddi object of that device.
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
// Releases the context's engine reference and frees it. It calls no hook. If engine-ddi objects of this device
// are still alive, it returns S_FALSE and their count in *live_objects. They are not freed, because the runtime
// owns their storage.
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
// report_device_error. It never touches RUNTIME slots or shell-owned MIXED slots. It fails with E_INVALIDARG if
// table_size is not sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088).
HRESULT fill_device_core(D3D12DDI_DEVICE_FUNCS_CORE_0088* table, SIZE_T table_size, const FillInfo* info) noexcept;
// Fills all 70 command-list slots for one uTableNum. Graphics-only slots in a compute table report
// E_INVALIDARG through report_list_error. pfnPresent gets a fail-safe entry; the shell overrides it with its
// own Present, which calls resource_allocation.
HRESULT fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, SIZE_T table_size, uint32_t table_index,
                          const FillInfo* info) noexcept;

// ---- Engine parts of shell-owned MIXED slots ---------------------------------------------------------------------
// Queue (CreateCommandQueue/DestroyCommandQueue are the shell's). The shell creates the WDDM context first. The
// engine queue then binds its VkQueue to that context through the engine's BindQueue service; the cookie is
// the value the shell's BindQueue receives.
struct EngineQueue;
HRESULT create_engine_queue(DeviceContext* context, const BC250_VKD3D_COMMAND_QUEUE_DESC* desc, void* queue_cookie,
                            EngineQueue** out) noexcept;
void destroy_engine_queue(EngineQueue* queue) noexcept;
// ExecuteCommandLists (the queue table is the shell's). Everything is submitted to the queue's bound context
// before this returns (engine INLINE mode). A failure is reported through report_device_error and returned.
HRESULT execute_command_lists(EngineQueue* queue, UINT count, const D3D12DDI_HCOMMANDLIST* lists) noexcept;
// Present (the DXGI table is the shell's): the runtime allocation behind a resource. The resource must have
// been created in RuntimeBacked mode, or the call fails. Present-capable resources get their own allocation,
// never a sub-allocation.
HRESULT resource_allocation(DeviceContext* context, D3D12DDI_HRESOURCE resource, D3DKMT_HANDLE* allocation,
                            uint64_t* offset) noexcept;

// ---- Capabilities --------------------------------------------------------------------------------------------------
// Adapter-scoped answers read from one engine device of the selected adapter, never from another GPU. How the
// shell obtains that device before the first CreateDevice is its choice, and an open question of this revision.
struct CapsSnapshot;
HRESULT collect_caps(ID3D12Device* engine_device, CapsSnapshot** out) noexcept;
void free_caps(CapsSnapshot* caps) noexcept;
// Answers one GetCaps call. It writes only when DataSize equals the exact size of a validated layout for that
// type at or below the negotiated version, and never copies a prefix. It returns E_NOTIMPL for types this
// revision does not answer, and E_INVALIDARG for a size it does not know.
HRESULT build_caps(const CapsSnapshot* caps, uint32_t ddi_version, const D3D12DDIARG_GETCAPS* request) noexcept;

// ---- Private storage records ------------------------------------------------------------------------------------
// Every engine-ddi object starts with this header, constructed in the runtime-owned storage. Destroy releases
// `engine`, frees heap copies owned by the record and sets `tag` to kPoisoned. The storage itself is never
// freed, because the runtime owns it.
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
    IUnknown* engine;                           // one reference, or null for shell-only records
    DeviceContext* device;                      // creating device; use through another device is refused
};
static_assert(sizeof(RecordHeader) == 24, "RecordHeader layout");

} // namespace engine_ddi
