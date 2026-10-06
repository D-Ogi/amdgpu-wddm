// SPDX-License-Identifier: MIT
// engine-ddi: D3D12 DDI 0092 slots translated onto the vkd3d-proton engine (amdgpu_wddm_vkd3d.dll).
//
// Boundary r3 (2026-09-28): r2 plus the adapter caps path (query_adapter_caps, build_caps answering GetCaps) on
// engine ABI 1.2 QueryAdapterCaps, after the lab run M768 showed the runtime's first two GetCaps calls. Added within
// r3, additively: set_memory_architecture_policy, the shell's policy for GetCaps 1002. The engine
// side owns this directory; INTEGRATION.md lists what the shell calls and when.
// Boundary r4 (2026-09-29): the linear primary. MemoryRequest grows by the surface fields and
// kMemoryLinearSurface, and memory_type_bits is filled for such a request; engine ABI 1.3. Added within r4,
// additively: set_retire_policy, the retire hand-off of submissions to resource DDIs (off unless set),
// set_release_policy, the two-phase retirement of heap memory, and set_replay_policy, deferred command-list replay
// on worker threads (both also off unless set).
// Boundary r5 (2026-10-05): shared resources (BD-075). MemoryRequestFlags grows by kMemoryShareable, allocate_memory
// may answer the shell-internal kShareRequired for a request that carried it, and ShellHooks grows by the optional
// adopt_memory, which takes an allocation the runtime opened instead of allocating one. The engine ABI does not
// change: a shared surface is the same linear image the linear primary already is (1.3 V13).
// The shell (namespace native12) owns everything else:
//   - the adapter, device state and FillDDITable composition;
//   - queues and their WDDM contexts, fences and queue Signal/Wait;
//   - allocation and residency callbacks, the DXGI table, present and registration.
// engine-ddi never reads native12::Device. The shell hands over what it needs through ShellHooks and finds the
// DeviceContext of a D3D12DDI_HDEVICE through the ResolveDevice hook.
//
// Engine ABI: bc250_vkd3d_engine.h r5-draft (ABI 1.3), included by path from the vkd3d-proton fork checkout
// pinned in engine-abi.json. engine-ddi uses:
//   - 1.1: CreateDevice in the INLINE queue mode and CreateCommandQueue (queue.cpp);
//   - 1.2 V10 CreateHeapFromMemory, MapHeap and UnmapHeap: RuntimeBacked heaps over the shell's VkDeviceMemory,
//     and MapHeap/UnmapHeap in both memory modes (resources.cpp);
//   - 1.2 V11 QueryAdapterCaps: the adapter caps of GetCaps (caps.cpp);
//   - 1.2 r4 V12 InstanceMode: the create info of CreateDevice and QueryAdapterCaps says PRIVATE, so that every
//     engine device has a VkInstance of its own (INTEGRATION.md, "Adapter: GetCaps");
//   - 1.3 V13 QueryLinearImage and CreateLinearPlacedResource: the linear primary (resources.cpp).
// So the engine must be asked for 1.3; create_device_context refuses a function table without these entries.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <d3d12umddi.h>
#include <vulkan/vulkan_core.h>
#include "bc250_vkd3d_engine.h"
#include <cstdint>

namespace engine_ddi {

inline constexpr uint32_t kBoundaryRevision = 5;

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
//       * Vulkan (RuntimeBacked): byte_size covers the request, and gpu_va is non-zero and a multiple of
//         MemoryRequest::alignment. A mismatch hands the memory straight back through free_memory and fails the
//         create. The memory type is checked by the engine's CreateHeapFromMemory (V10), which refuses a type it
//         would not pick for the heap; the memory is then handed back the same way and the create fails with the
//         engine's E_INVALIDARG.
//   - Aliasing follows D3D12: overlapping placed resources are allowed, only one is active at a time, and
//     activation is the application's aliasing barrier, discard or clear. engine-ddi passes aliasing barriers to
//     the engine unchanged and tracks nothing itself.
//   - RuntimeBacked heaps are engine heaps made by CreateHeapFromMemory over the ImportedMemory, for all three
//     shapes. Whether hosted RADV's import of a runtime allocation is the same storage at the same GPU virtual
//     address as the runtime's view is a lab question (not yet measured); the development PC harness allocates the
//     VkDeviceMemory itself.
//   - Reserved (tiled; resource description only, with neither a heap nor a base resource in ReuseBufferGPUVA): the
//     engine's CreateReservedResource1 (a LEGACY_* initial layout) or CreateReservedResource2. No allocate_memory
//     call and no heap reference: the resource has no memory until update_tile_mappings binds tiles of heaps into
//     it, and those heaps keep their own release sequence. That this is the runtime's shape for
//     CreateReservedResource is an INFERENCE from the DDI (no runtime call logged). The engine creates one only at
//     tiled resources tier 1 or above.
//
// Release sequence of heap memory, run once when the last user of a heap is destroyed (the heap record and
// every resource placed in it):
//   1. Retirement. A final COM Release does not prove that the GPU has finished. engine-ddi keeps ownership in a
//      deferred-release record until every engine queue of the device has completed all work submitted before
//      the destroy. It learns that from its own fence on each engine queue, signalled after every
//      execute_command_lists. A queue destroyed before retirement is observed resolves its part only if the
//      queue's fence reached the recorded value by the queue's final Release; otherwise the record stays
//      pending for the device's lifetime and counts as live in destroy_device_context.
//   2. The engine's final Release of its heap (V10 step 2; the resources placed in it were released at their own
//      destroy, except a committed resource that an initialization batch still named at its destroy, released
//      here just before the heap: INTEGRATION.md, "Committed render targets"). The engine never frees borrowed
//      memory and makes no Vulkan call on it after this Release.
//   3. free_memory: the shell releases its Vulkan import. engine-ddi calls it exactly once per ImportedMemory and
//      never retries, whatever it returns. A failure goes to report_device_error, and the shell keeps its record
//      (with the allocation handle and cookie).
//   4. The runtime deallocation, by the shell, on the same thread, within free_memory or after it.
//   Steps 2 and 3 run only on the thread of a DDI call into the device that owns the memory, while that runtime
//   device exists: the DDI call that makes the last destroy if the work has already retired, otherwise the first
//   later DDI call that observes retirement (execute_command_lists, update_tile_mappings, copy_tile_mappings,
//   pfnCreateHeapAndResource, pfnDestroyHeapAndResource, destroy_engine_queue, destroy_device_context). Never from
//   an engine thread or an engine callback: the engine has no threads in INLINE mode, and the only threads
//   engine-ddi creates, the workers of set_replay_policy, make engine command-list calls and nothing else.
//   With a retire policy (set_retire_policy) the three submission calls observe retirement only past its bounds,
//   and the resource DDIs and the queue and device calls take the rest; which thread runs steps 2 and 3 changes,
//   not what proves retirement, and not the order of the steps.
//   A tile mapping is queue work like a submission: its call signals the queue's retirement fence after the bind, so
//   heap memory destroyed after the mapping waits for it.
//   The memory of a linear primary (kMemoryLinearSurface) is the exception to "the first later DDI call": the
//   shell may release it to the runtime only inside the pfnDestroyHeapAndResource that ends it. That destroy
//   therefore waits for retirement, at most 2000 ms and without engine-ddi's lock held. When the bound passes,
//   ERROR_TIMEOUT (as an HRESULT) goes to report_device_error and the release is recorded like any other: its
//   free_memory then comes from a later DDI call, where the shell frees its import and keeps the allocation.
enum class MemoryMode : uint32_t { RuntimeBacked = 1, EnginePrivateTest = 2 };

enum MemoryRequestFlags : uint32_t {
    kMemoryDedicated = 0x1,                     // committed resource: the memory backs exactly this resource
    kMemoryPrimary = 0x2,                       // D3D12DDI_HEAP_FLAG_PRIMARY was set
    // The memory backs one linear image at offset 0 (engine ABI 1.3 V13), which a consumer outside the engine
    // reads by row pitch: the surface fields are filled, memory_type_bits is the image's and alignment is the
    // image's alone. Set only together with kMemoryDedicated and kMemoryPrimary. A primary without it is a
    // description engine-ddi cannot make linear; the shell decides what becomes of it.
    kMemoryLinearSurface = 0x4,
    // BD-075: the runtime may be creating this resource as a shared one, and the shell may retry with the
    // shareable argument shape. It changes nothing about the request that carries it; it is permission alone.
    // The D3D12 DDI has no sharing field and no sharing flag anywhere (d3d12umddi.h: D3D12DDIARG_CREATEHEAP_0001,
    // ..._CREATERESOURCE_0088, D3D12DDICB_ALLOCATE_0022, D3D12DDI_ALLOCATION_INFO_0022), and the heap flags a
    // shared committed texture arrives with are an ordinary committed texture's (0x26, all three categories), so
    // no description tells the two apart. What does tell them apart is the runtime's own refusal of the ordinary
    // allocation shape: it arrives for a shared resource and for nothing else that reaches this path. engine-ddi
    // therefore sets this flag on the first attempt for a description inside the shareable envelope, and retries
    // with kMemoryDedicated|kMemoryShareable|kMemoryLinearSurface when the shell answers kShareRequired.
    // Set only together with kMemoryDedicated, and never with kMemoryPrimary: a primary is not shared by handle.
    kMemoryShareable = 0x8,
};

// The shell's answer to an allocate_memory that carried kMemoryShareable and whose runtime allocation callback
// refused the ordinary argument shape with E_INVALIDARG: nothing was allocated, and the shell is willing to try
// the shareable shape. It is internal to this driver and never reaches the runtime; admitted_create_failure
// clamps it like any other refusal, so a path that forgets to handle it reports E_OUTOFMEMORY and not a code
// that would cost the application its device. The customer bit is set, so it can never collide with an HRESULT
// of the platform's.
inline constexpr HRESULT kShareRequired = static_cast<HRESULT>(0xA0BC2075);

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
    // Vulkan memory types the engine accepts for this heap, 0 for "not narrowed": CreateHeapFromMemory checks
    // the type then (INTEGRATION.md says how the shell picks it). With kMemoryLinearSurface it is the
    // memoryTypeBits of the image, and the shell picks one of them.
    uint32_t memory_type_bits;
    uint32_t reserved;                          // 0
    // kMemoryLinearSurface only, otherwise 0. The image's width, height and format are the resource's;
    // byte_size is at least surface_row_pitch * (height rounded up to 4) and at least the image's memory size,
    // rounded up to 4 KiB.
    uint32_t surface_row_pitch;                 // bytes, VkSubresourceLayout::rowPitch; a multiple of 16
    uint32_t reserved2;                         // 0
    uint64_t surface_layout_size;               // VkSubresourceLayout::size
};

// BD-075, the open half: an allocation the runtime opened for us (D3D12DDIARG_OPENHEAP_0003), which the shell
// must map, make resident and import into the engine's VkDevice without allocating anything. The record it
// builds is borrowed: the shell never deallocates it, because it never allocated it, and the runtime destroys
// it as soon as pfnDestroyHeapAndResource returns. flags is kMemoryDedicated|kMemoryShareable|
// kMemoryLinearSurface, as the shareable create's second attempt is, so the shell applies the same rules.
struct AdoptRequest {
    uint32_t size;                              // sizeof(AdoptRequest)
    uint32_t flags;                             // MemoryRequestFlags
    // The runtime owner as the open DDI supplied it: the fourth argument of pfnOpenHeapAndResource. It names the
    // resource, not an allocation to release, so the shell deliberately keeps no owner for it: the record's
    // authority stays 0 and RuntimeAllocation::adopt names no resource, which is what makes the owner release form
    // impossible for a borrowed allocation. The field is here because the request carries the whole shape of the
    // open for the shell's refusal lines and traces, not because anything is recorded from it. The sentence this
    // replaces said "the shell records it", which it never did (BD-075 review, 2026-10-06).
    D3D12DDI_HRTRESOURCE rt_owner;
    D3DKMT_HANDLE allocation;                   // pOpenAllocationInfo[0].hAllocation; never ours to destroy
    uint32_t memory_type_bits;                  // the engine's memoryTypeBits for the image, as above
    uint64_t byte_size;                         // the backing the image needs, page rounded
    uint64_t alignment;                         // the image's memory alignment
};

// One runtime allocation that the shell has made and imported. The shell owns it (see the release sequence).
// The VkDeviceMemory is what engine ABI 1.2 V10 takes: a whole allocation on the engine's VkDevice, allocated with
// VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT (always, whatever the heap allows), not a dedicated allocation
// (no VkMemoryDedicatedAllocateInfo, even for a committed resource), and not mapped by the shell while engine-ddi
// holds it (the engine maps a CPU-visible heap's memory itself, once).
struct ImportedMemory {
    uint32_t size;                              // sizeof(ImportedMemory)
    uint32_t memory_type_index;                 // index on the engine's physical device
    VkDeviceMemory memory;                      // valid on the engine's VkDevice until free_memory
    uint64_t byte_size;                         // its VkMemoryAllocateInfo::allocationSize
    D3DKMT_HANDLE allocation;                   // the kernel allocation from pfnAllocateCb_0022
    uint32_t reserved;                          // 0
    // The GPU virtual address of a completed, validated mapping: filled only after MapGpuVirtualAddress has
    // completed. The address AllocateCb reports may be 0 until then. Residency is not required, and the shell must
    // not make the allocation resident here: a driver with the MakeResident and Evict DDIs creates no allocation
    // resident (DirectX-Specs d3d/ResourceHeaps.md, "must no longer create allocations ... as resident during
    // creation"); the runtime makes it resident through pfnMakeResident, which the shell resolves with
    // object_allocation. 0 means "no VA": every use that needs a VA fails with E_INVALIDARG. Every heap needs
    // one, so allocate_memory output with gpu_va 0 is handed straight back through free_memory and the create
    // fails with E_INVALIDARG.
    D3DGPU_VIRTUAL_ADDRESS gpu_va;
    void* cookie;                               // shell-private, passed back unchanged
};

// ---- Hooks the shell provides --------------------------------------------------------------------------------
// Hooks are called only on the thread of a DDI call into the device, during that call, and never re-enter
// engine-ddi. Two DDI threads of one device may call hooks at the same time (the runtime calls heap and resource
// creation and destruction concurrently). engine-ddi never holds a lock of its own while it calls a hook. A replay
// worker (set_replay_policy) calls none of them.
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
    // BD-075: the borrowed memory of an opened shared resource. Optional, and null means no shared open is
    // served: pfnOpenHeapAndResource then refuses, as it did before r5. It fills *memory only when it
    // succeeds, and the memory it fills comes back through free_memory like any other, where the shell's own
    // record knows that it must not deallocate what it never allocated.
    HRESULT (APIENTRY* adopt_memory)(void* shell, const AdoptRequest* request, ImportedMemory* memory);
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

// Retire hand-off. Without a policy (and with handoff 0) every retirement point runs the release sequence for what
// has retired, the submission calls included: execute_command_lists, update_tile_mappings and copy_tile_mappings
// then pay steps 2 to 4 (the runtime's DestroyAllocation2 and FreeGpuVirtualAddress among them) on the submitting
// thread, which a game keeps on its critical path (trial 217: 0.40 ms/frame on-CPU and 0.31 ms/frame waiting in
// the kernel on the main thread, the only submitter). With handoff 1 a submission call runs the sequence only when
// at least backlog_bound releases are pending, or when no pfnCreateHeapAndResource or pfnDestroyHeapAndResource
// of the device has run it for age_bound_ms (by GetTickCount64, whose step is about 16 ms); otherwise it leaves
// the releases to the next resource DDI, on whichever thread the application makes it. Retirement is proven as
// before (the fences, the marks, stuck releases), each release still runs once and on a DDI thread of its device,
// and destroy_engine_queue and destroy_device_context still run the sequence unconditionally. A
// pfnCreateHeapAndResource still runs it before it allocates. Bounds: backlog_bound 1 or more; age_bound_ms 1 to
// 10000. Call it after create_device_context and before the context is used on another thread or by any DDI; it
// writes the context. E_INVALIDARG, with the policy held before kept, for a null argument, a size other than
// sizeof(RetirePolicy), handoff other than 0 or 1, or handoff 1 with a bound out of range.
struct RetirePolicy {
    uint32_t size;                              // sizeof(RetirePolicy)
    uint32_t handoff;                           // 0: every retirement point runs the sequence; 1: hand-off
    uint32_t backlog_bound;                     // handoff 1: a submission runs it at this many pending releases
    uint32_t age_bound_ms;                      // handoff 1: ... or after this long without a resource DDI's pass
};
HRESULT set_retire_policy(DeviceContext* context, const RetirePolicy* policy) noexcept;

// Two-phase retirement of heap memory (M15.8, fix F1 of the trial 245 report). Without a policy (and with
// two_phase 0) a release waits for the marks of one snapshot: the work every engine queue had submitted when
// the destroy reached engine-ddi. That is what the application's own ordering promises, and nothing more: a
// submission the engine makes after the destroy, on any queue, still naming the memory (a batch recorded
// before it, or an engine-internal path ordered on a queue's timeline) is outside those marks. Trial 245
// faulted on exactly that window: the GFX job was in the ring 2.1 ms before the unmap of the memory it read.
// With two_phase 1, when a release's first-phase marks are reached, every engine queue's current mark is
// recorded once more and the release waits for those too; a release that had no mark at all waits for one
// such second phase as well, so a destroy that races a submission on another thread is covered by it. An
// idle queue's mark is already retired, so the second phase adds nothing to wait for and the hold ends at
// the next retirement point; a busy queue holds the memory about one more frame. Nothing waits on the CPU:
// both phases are reads of the queues' state words and retirement fences, taken at the retirement points
// engine-ddi already has. The memory of a linear primary (in_ddi) keeps its single bounded phase inside the
// destroy that ends it. Call it like set_retire_policy: after create_device_context, before the context is
// used on another thread. E_INVALIDARG, with the policy held before kept, for a null argument, a size other
// than sizeof(ReleasePolicy), or two_phase other than 0 or 1.
struct ReleasePolicy {
    uint32_t size;                              // sizeof(ReleasePolicy)
    uint32_t two_phase;                         // 0: one snapshot per release; 1: the second phase above
};
HRESULT set_release_policy(DeviceContext* context, const ReleasePolicy* policy) noexcept;

// Deferred command-list replay. Off (enabled 0, the default) every recording slot calls the engine list on the
// calling thread, as before this policy existed: no ring, no thread. On, a recording slot still validates,
// translates and reports on the calling thread, then writes the engine call, with a copy of every array and
// descriptor the engine reads through a pointer, into the calling thread's ring; each ring's worker thread makes
// the engine calls in order (replay.h: topology, drains, ownership rules). Close is the list's last entry, made by
// the worker; Reset, ExecuteBundle, ExecuteCommandLists and the destroy of a list wait for that list's pending calls,
// and the first of them reports a failed engine Close to the runtime (the destroy drops it). A command pool's reset
// and a Reset into the pool wait for the pool's pending Closes; the destroy of any object a pending call can name, a
// command pool's destroy, and SetPipelineStackSize wait for every ring. Up to rings recording threads (1 to 16)
// get a ring of ring_bytes each (a power of two, 64 KiB to 64 MiB); a thread beyond them records directly.
//   worker(shell, body, ring): the start of each worker thread, called once on it; it must call body(ring), which
//     returns at teardown. The worker makes engine command-list calls only, and the engine's recording may call
//     runtime callbacks through the hosted Vulkan driver (descriptor and memory work), so the shell sets up what
//     those need on the thread and keeps out what a recording call must not do there.
//   drained(shell): on a DDI thread, after each drain that a DDI call makes, with no engine-ddi lock held; the shell
//     reports there what a worker could not (a device removal it saw).
//   log(shell, line): optional (null: none). Every replay line (the policy, rings, the summaries, long waits and
//     stalls), one call per line without its newline, on whichever thread writes it, with no engine-ddi lock held;
//     the same lines also go where engine-ddi's other lines go. For a process whose stderr nobody reads.
// A worker starts at the priority of the thread whose ring it serves (and of a thread that takes the ring over); while
// a drain waits for it past its spin, the worker runs one level above the waiting thread (at most HIGHEST, unless
// the waiter is above that), and returns to its own level when the last such wait ends.
// enabled 0 on a context with the policy on drains every ring, stops and joins the workers and frees the rings;
// destroy_device_context does the same before it frees the context. Call it with enabled 1 like set_retire_policy:
// after create_device_context, before the context is used on another thread. E_INVALIDARG, with the policy held
// before kept, for a null argument, a size other than sizeof(ReplayPolicy), enabled other than 0 or 1, enabled 1
// while on, rings or ring_bytes out of range, or a null hook.
using ReplayBody = void (APIENTRY*)(void* ring);
struct ReplayPolicy {
    uint32_t size;                              // sizeof(ReplayPolicy)
    uint32_t enabled;                           // 0: direct recording (turns it off if on); 1: deferred replay
    uint32_t rings;                             // enabled 1: recording threads with a ring, 1 to 16
    uint32_t ring_bytes;                        // enabled 1: bytes per ring
    void* shell;                                // passed back unchanged
    void (APIENTRY* worker)(void* shell, ReplayBody body, void* ring);
    void (APIENTRY* drained)(void* shell);
    void (APIENTRY* log)(void* shell, const char* line);
};
HRESULT set_replay_policy(DeviceContext* context, const ReplayPolicy* policy) noexcept;

// ---- Direct entry and the entry path experiment ------------------------------------------------------------------
// The direct recording entry is the default wherever the shell installs it (native-tables.cpp: unless the experiment
// direct-entry-off, recording-bind-off or deferred-replay-off, or the full trace); trial 327 measured it against the
// shell's entry at Witcher 3 LOW in one process: main thread 0.51 ms/frame less, frames 0.50 ms shorter (+3.8 %).
// Knobs, for measurements only, read once per process from the environment, else from the file BC250_ENTRY_CFG names
// (KEY=VALUE lines; a '#' line is a comment). With that variable unset no file is read: there is no machine-wide knob
// file, for the reasons the ICD removed its own (entry.cpp, mesa 0bb2d1ea). A knob therefore reaches a game that Steam
// starts through BC250_ENTRY_CFG in the environment of the process that starts it:
//   BC250_ENTRY_PATH   one letter per arm, rotated every BC250_ENTRY_PHASE_MS (default 2000) at the shell's Present;
//                      a single letter fixes the arm; without it, b. a: the shell's entry; b: the direct entry;
//                      c: the direct entry, with spinning replay workers looking for work every BC250_ENTRY_POLL_US
//                      (default 1) instead of after every pause; d: the shell's entry and a busy wait of
//                      BC250_ENTRY_PAD_US (default 300) in each Present.
//   BC250_ENTRY_STATS  1: a row per phase in the file BC250_ENTRY_LOG names ("%p" becomes the process id), else in
//                      amdgpu_wddm_radv-deferred-<pid>-shell.log in %TEMP%: frame times, calls and sampled times per
//                      timed entry and thread, the direct entry's misses, the replay workers' busy, spin, yield and
//                      sleep times.
// The direct entry is the graphics table's slot for the value-only recording calls (draws, input assembler,
// viewports and scissors, blend factor and stencil reference, graphics root arguments): with deferred replay on, it
// writes the slot's own ring entry from the table entry itself, without the shell's entry thunk; anything else, and
// every call of an arm without it, goes to the slot as filled before.
// install_direct_list: writes the direct entries over the graphics table's (table_index 1) slots, keeping those as
// the fallback. A later call for another device must find the same slots (else false: that table keeps them).
bool install_direct_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
// The direct entry records only for a context the shell admits as it admits its own fast entry (the recording
// binding published): on after that is published, off before it is cleared.
void set_direct_entry(DeviceContext* context, bool on) noexcept;
// The shell's Present, first thing: the frame clock, the arm rotation, the rows and arm d's wait. Only the first
// thread that calls it counts frames; for the others it returns at once.
void entry_frame() noexcept;
// Whether the statistics are on (the shell then times its own entries).
bool entry_stats_on() noexcept;

// ---- Failures a create or open DDI may report -------------------------------------------------------------------
// windows-driver-docs display handling-errors.md: a creation function of a user-mode display driver is in the
// AllowOutOfMemory category. The runtime admits E_OUTOFMEMORY and D3DDDIERR_DEVICEREMOVED from it and treats
// every other failure as critical: it logs the driver, removes the device and sets the removed reason to
// DXGI_ERROR_DRIVER_INTERNAL_ERROR. Measured on this stack for E_NOTIMPL out of pfnCreateCommandSignature
// (INTEGRATION.md) and for pfnCreateHeapAndResource and pfnOpenHeapAndResource of a shared resource
// (BD-075: hr 0x887A0005, removed reason 0x887A0020, in all 13 shared-resource cells of capture-share).
// A refusal must therefore leave the create and open slots as E_OUTOFMEMORY, however the refusal was decided
// inside the driver: the application then sees a creation that failed, not a device it has to recreate. The real
// HRESULT is in the log_refusal line of the same call, so the diagnosis is not lost.
// That the D3D12 runtime applies the same rule as D3D10/11 is an INFERENCE from that document plus the two
// measurements above; it is what the lab script of BD-075 checks.
// Two codes are admitted and no third: E_OUTOFMEMORY and D3DDDIERR_DEVICEREMOVED. A lost device is therefore
// reported under the name the runtime admits, not under the name the API shows the application:
// DXGI_ERROR_DEVICE_REMOVED (0x887A0005) is not in the AllowOutOfMemory list, so a create slot that reports it
// loses the device a second time, with DRIVER_INTERNAL_ERROR over the real reason - the BD-075 signature itself.
// Both are live paths: create_heap_and_resource answers DXGI_ERROR_DEVICE_REMOVED for a context already lost, and
// heap-import maps VK_ERROR_DEVICE_LOST to it. The clamp translates the three DXGI device codes instead.
// native12::ddi_admitted_create_failure (ddi-entry.h) is the same rule for the refusals the DDI thunk itself
// decides, above this module; native-tables.cpp static_asserts that the two agree.
inline constexpr HRESULT kDriverDeviceRemoved = static_cast<HRESULT>(0x88760870);  // D3DDDIERR_DEVICEREMOVED
constexpr HRESULT admitted_create_failure(HRESULT hr) noexcept {
    if (SUCCEEDED(hr) || hr == E_OUTOFMEMORY || hr == kDriverDeviceRemoved) return hr;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return kDriverDeviceRemoved;
    return E_OUTOFMEMORY;
}
// The shell-internal retry sentinel is a refusal like any other as far as the runtime is concerned: a create path
// that let it escape would still report a code the AllowOutOfMemory category admits (BD-075).
static_assert(admitted_create_failure(kShareRequired) == E_OUTOFMEMORY);

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

// The owner of a command list, for the shell's own command-list slots (whose first argument is the list, not the
// device): ShellHooks::shell of the device context that created the list. Null for storage that holds no live
// engine-ddi command list: not yet constructed by CreateCommandList, or already destroyed by DestroyCommandList
// (CloseCommandList and ResetCommandList do not end a list). Read-only: it reads the list's record, takes no lock
// and calls nothing, and the shell needs no knowledge of the record's layout. Lifetime: the runtime serializes the
// calls of one command list, so a call made from a slot of that list, or while the shell otherwise knows the list
// is not being created or destroyed, reads a stable record.
void* command_list_shell(D3D12DDI_HCOMMANDLIST list) noexcept;

// The owner of a ray tracing state object, for the slots whose only handle is the state object (GetShaderIdentifier,
// GetShaderStackSize, GetPipelineStackSize, SetPipelineStackSize): ShellHooks::shell of the device context that
// created it. Null for storage that holds no engine-ddi state object record: not yet constructed by
// CreateStateObject, or already destroyed by DestroyStateObject. A record left inert by a failed create still names
// its device; the slots themselves refuse it. Read-only, as command_list_shell. Lifetime: the caller of those slots
// holds the state object, so its record is neither being created nor destroyed during the call.
void* state_object_shell(D3D12DDI_HSTATEOBJECT_0054 state_object) noexcept;

// ---- Shaders ---------------------------------------------------------------------------------------------------
// Native intake (every create-shader slot): the payload is the bare program with its length in DWORD 1, and
// register-only signature entries. That the buffer holds exactly pShaderCode[1] DWORDs is an INFERENCE from the
// SAL annotation _In_reads_(pShaderCode[1]) on D3D12DDIARG_CREATE_SHADER_0026; no runtime payload has been
// measured. engine-ddi rebuilds the container the engine consumes with shader-container's BuildContainer
// (shader-container/README.md), which reads no further than that length; hull and domain programs take the
// Tessellation signatures, the others Standard. The container lives in engine-ddi's own allocation until
// DestroyShader; the record holds no engine object. A failure is logged and reported through
// report_device_error (E_NOTIMPL for an unsupported program, E_INVALIDARG for a malformed one, E_OUTOFMEMORY);
// mesh and amplification programs are E_NOTIMPL. CreatePipelineState passes the container bytes to the engine,
// names input elements from the vertex program's input signature and stream-output entries from the last stage
// before rasterization. A gap in a stream-output declaration goes to the engine as an entry with a NULL
// SemanticName, which the pinned r4 engine takes (INTEGRATION.md, "Shaders and pipelines").

// ---- Engine parts of shell-owned MIXED slots ---------------------------------------------------------------------
// Queue (CreateCommandQueue/DestroyCommandQueue are the shell's). The shell creates the WDDM context first. The
// engine queue then binds its VkQueue to that context through the engine's BindQueue service; the cookie is
// the value the shell's BindQueue receives. engine-ddi also creates one engine fence per queue (internal memory)
// for the retirement of step 1 of the release sequence.
struct EngineQueue;
HRESULT create_engine_queue(DeviceContext* context, const BC250_VKD3D_COMMAND_QUEUE_DESC* desc, void* queue_cookie,
                            EngineQueue** out) noexcept;
// Releases the engine queue (its final Release waits for the queue's last submission, engine rule V7), records
// how far the queue's fence got, and then runs the release sequence for work that has retired. The result says
// whether every engine use of the queue has retired:
//   - Retired: the device was not removed, every submission is covered by a successful signal of the queue's
//     retirement fence, and the fence had reached the last signal when the final Release returned.
//   - NotRetired: anything else. engine-ddi then records retirement_lost for the device: heap memory released
//     after this point stays owned for the device's life (the release sequence, step 1). NotRetired depends only
//     on the queue's own fence and state, never on the engine's GetDeviceRemovedReason, which can read healthy.
// Ownership, the same for both results: the EngineQueue is freed before the call returns, and the pointer must
// not be used again. engine-ddi keeps no queue record, only the device's retirement bookkeeping (the queue slot's
// marks and retirement_lost); the engine queue and its fence have been released. Everything the shell owns (its
// WDDM context, its tokens) stays the shell's: after NotRetired the GPU may still be using that context. A null
// queue returns Retired and does nothing.
enum class QueueClose : uint32_t { Retired = 1, NotRetired = 2 };
QueueClose destroy_engine_queue(EngineQueue* queue) noexcept;
// ExecuteCommandLists (the queue table is the shell's). Everything is submitted to the queue's bound context
// before this returns (engine INLINE mode), followed by the signal of the queue's retirement fence. A failure is
// reported through report_device_error and returned. First, when committed render targets or depth-stencil
// resources were created since the last call, it submits their initialization on this queue if it is DIRECT,
// otherwise on another live DIRECT engine queue of the device, and this queue waits for it on the GPU
// (INTEGRATION.md, "Committed render targets").
HRESULT execute_command_lists(EngineQueue* queue, UINT count, const D3D12DDI_HCOMMANDLIST* lists) noexcept;
// Tile mappings. pfnUpdateTileMappings and pfnCopyTileMappings (Q3, Q4) are slots of the shell's queue table, whose
// first argument is the shell's D3D12DDI_HCOMMANDQUEUE: the shell resolves it to the queue's EngineQueue, as for
// ExecuteCommandLists, and passes every other argument of the slot unchanged (INTEGRATION.md, "Tiled resources").
// Each call takes the queue's submission lock, calls the engine queue's UpdateTileMappings or CopyTileMappings (in
// INLINE mode the engine submits the sparse bind on the queue before it returns), then signals the queue's
// retirement fence like execute_command_lists does, and is a retirement point. The resources must be reserved
// resources of the queue's device (resources.cpp); heap is a heap record of that device, and may be null only when
// every range is NULL or SKIP. The heap's memory is what CreateHeapFromMemory imported (RuntimeBacked), so the tiles
// are bound to the runtime allocation. What engine-ddi can check it checks first: a malformed call binds nothing,
// is reported through report_device_error and returns E_INVALIDARG. A failed retirement signal is reported and
// returned. The engine reports nothing: a tile it cannot map (out of the resource's bounds) is logged and dropped,
// as in vkd3d-proton.
HRESULT update_tile_mappings(EngineQueue* queue, D3D12DDI_HRESOURCE resource, UINT region_count,
                             const D3D12DDI_TILED_RESOURCE_COORDINATE* region_starts,
                             const D3D12DDI_TILE_REGION_SIZE* region_sizes, D3D12DDI_HHEAP heap, UINT range_count,
                             const D3D12DDI_TILE_RANGE_FLAGS* range_flags, const UINT* heap_range_starts,
                             const UINT* range_tile_counts, D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept;
HRESULT copy_tile_mappings(EngineQueue* queue, D3D12DDI_HRESOURCE dst, const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_start,
                           D3D12DDI_HRESOURCE src, const D3D12DDI_TILED_RESOURCE_COORDINATE* src_start,
                           const D3D12DDI_TILE_REGION_SIZE* size, D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept;
// Present (the DXGI table is the shell's): the runtime allocation behind a resource. The resource must have
// been created in RuntimeBacked mode as a committed resource (a dedicated allocation), or the call fails.
HRESULT resource_allocation(DeviceContext* context, D3D12DDI_HRESOURCE resource, D3DKMT_HANDLE* allocation,
                            uint64_t* offset) noexcept;
// Present, and the destroy's owner scope: the runtime allocation of a surface this device created as a linear
// surface of its own - a linear primary or a shared create - one image from the first byte of its allocation.
// E_INVALIDARG for any other resource, and *allocation is 0 unless S_OK. An opened shared surface is refused
// here on purpose: it is nobody's primary and only its creator's own destroy may release that allocation, which
// is what the destroy slot uses this answer for.
HRESULT present_allocation(DeviceContext* context, D3D12DDI_HRESOURCE resource,
                           D3DKMT_HANDLE* allocation) noexcept;
// The same answer for the destination of a blt-model Present, which may also be a surface this device opened
// (ResourceKind::Opened). The runtime named it and owns it for the call; nothing of its lifetime is ours, so this
// form must never be used for the destroy's owner scope. Before BD-075 no opened resource could exist and the two
// questions had one answer; giving the destination the stricter answer turned a legal Present into a removed
// device at stage 4 (BD-075 review, 2026-10-06).
HRESULT present_destination_allocation(DeviceContext* context, D3D12DDI_HRESOURCE resource,
                                       D3DKMT_HANDLE* allocation) noexcept;
// BD-075: how many creates this process has turned into a shared linear surface after the runtime refused the
// ordinary allocation shape. The named risk of that retry is that it fires on an ordinary committed texture, so
// the count is kept whatever the trace switches say and every retry also names itself through log_refusal.
uint64_t shared_surface_retries() noexcept;
// MakeResident and Evict (the slots are the shell's): the kernel allocation behind one object of the
// D3D12DDI_HANDLE_AND_TYPE list, whose Handle is the object's pDrvPrivate (INFERENCE: no runtime list has been
// logged). *allocation is 0 unless S_OK.
//   - D3D12DDI_HT_HEAP, D3D12DDI_HT_0012_RESOURCE (committed or placed) of this device: S_OK and the allocation of
//     the heap memory the object lives in; a placed resource gives its heap's allocation, so a placed resource and
//     its heap give the same handle, which the shell drops as a duplicate. S_FALSE for such an object without a
//     runtime allocation (EnginePrivateTest memory, harness only), and for a reserved resource: it has no memory of
//     its own, and the heaps its tiles are mapped from are made resident as heaps.
//   - D3D12DDI_HT_DESCRIPTOR_HEAP, D3D12DDI_HT_QUERY_HEAP of this device: S_FALSE. Their memory is engine-internal
//     and always resident; there is nothing to make resident or evict.
//   - Any other type, a record of another type or of another device, a destroyed object, a resource without heap
//     memory: E_INVALIDARG. Nothing is reported through report_device_error; the shell decides.
// Lifetime: it takes no lock and reads only what is fixed at creation (the record's tag and device, the heap
// memory it holds a reference to). The caller must keep the object alive for the call, as the runtime does for
// the objects of a MakeResident or Evict call. The handle stays valid while the object lives; the allocation
// itself is freed only through free_memory, after the last user of the heap memory has gone and retired.
HRESULT object_allocation(DeviceContext* context, D3D12DDI_HANDLE_AND_TYPE object, D3DKMT_HANDLE* allocation) noexcept;

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

// Memory architecture policy of GetCaps 1002 (INTEGRATION.md, "Memory architecture policy"). Whether the GPU's
// accesses to system memory are I/O coherent is host and kernel-driver policy the engine cannot see; so may be UMA,
// CacheCoherent and the serialization tiers. The shell states that policy per adapter, field by field: Default
// keeps the answer build_caps gives without a policy, anything else is the explicit answer. A policy with `size`
// set and every other byte zero is all Default.
enum class PolicyBool : uint32_t { Default = 0, False = 1, True = 2 };
struct PolicyTier {
    uint32_t set;                               // 0: Default, and value must be 0; 1: value is the answer
    uint32_t value;                             // a value of the field's DDI enum at 0092 (H:6793-6804)
};
struct MemoryArchitecturePolicy {
    uint32_t size;                              // sizeof(MemoryArchitecturePolicy)
    PolicyBool uma;                             // D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041::UMA
    PolicyBool cache_coherent;                  // ::CacheCoherent
    PolicyBool io_coherent;                     // ::IOCoherent
    PolicyTier heap_serialization_tier;         // ::HeapSerializationTier, D3D12DDI_HEAP_SERIALIZATION_TIER_0041
    PolicyTier resource_serialization_tier;     // ::ResourceSerializationTier, D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041
};
// Replaces caps's memory architecture policy; build_caps applies it to type 1002 and to nothing else. Call it after
// query_adapter_caps and before the first GetCaps: it writes caps, so it must not run concurrently with build_caps.
// E_INVALIDARG, with the policy held before kept, for a null argument, a size other than
// sizeof(MemoryArchitecturePolicy), a PolicyBool or PolicyTier::set out of range, a Default tier with a non-zero
// value, a tier the 0092 header does not define, or a resulting 1002 answer (every Default resolved against the
// engine's answers) that the specification calls contradictory: CacheCoherent without UMA, heap serialization
// tier 1 without resource serialization tier 2. Logged like build_caps's refusals.
HRESULT set_memory_architecture_policy(AdapterCaps* caps, const MemoryArchitecturePolicy* policy) noexcept;

// Reporting is ON by default: type 1006 answers RaytracingTier 1_1 when the engine's own answer is 1_1 or
// higher, and NOT_SUPPORTED otherwise; the engine's answer is never raised. Call this with `report` false to
// take the whole answer back to NOT_SUPPORTED, which the shell does for the experiment raytracing-tier-off.
// Two known gaps the tier promises and engine-ddi does not yet keep: an existing collection imported with an
// export list is refused (E_NOTIMPL, until an engine with the fix of its deferred import loop is pinned;
// importing a whole collection works), and the runtime's own state object description is unmeasured. Indirect
// ray dispatch traces every record up to the count only on an engine with the fork's fix, which is the pinned
// one (vkd3d-proton upstream traces the first record alone and nothing with a count buffer).
// Same calling rule as set_memory_architecture_policy. E_INVALIDARG for a null caps.
HRESULT set_raytracing_tier_reporting(AdapterCaps* caps, bool report) noexcept;

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
