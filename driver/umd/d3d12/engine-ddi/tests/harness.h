// SPDX-License-Identifier: MIT
// engine-ddi-harness: plays the D3D12 runtime and the native12 shell against the real engine DLL on the
// development PC. It proves the positive paths the shell integration needs: table composition, heap and resource
// creation, command lists, execution through an engine queue, fence completion, Map and readback, a compute dispatch
// and a draw from shaders created through the DDI's native shader slots. Built only with
// AMDGPU_WDDM_ENGINE_DDI_HARNESS.
#pragma once
#include "internal.h"                   // engine-ddi.h and the harness-only entry points
#include <cstdint>
#include <string>
#include <vector>

namespace harness {

void check(bool ok, const char* what);
void checkf(bool ok, const char* format, ...);
int failure_count();

// Runtime-owned private storage: filled with 0xCD like fresh runtime memory, with a canary behind the size the
// driver asked for. Blocks live until the harness ends.
class Storage {
public:
    Storage() = default;
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    ~Storage();
    void* alloc(SIZE_T size);           // null for a size of 0 or above 1 MiB
    bool canaries_intact() const;

private:
    struct Block { uint8_t* p; SIZE_T size; };
    std::vector<Block> blocks_;
};

// Hook recorder: the fake shell behind ShellHooks.
struct Shell {
    uint32_t device_errors = 0;
    uint32_t list_errors = 0;
    HRESULT last_device_error = S_OK;
    HRESULT last_list_error = S_OK;
    void* last_list = nullptr;          // runtime handle of the list the latest list error named
    struct Bind { void* list; uint32_t table; };
    std::vector<Bind> binds;
    int table_of(D3D12DDI_HRTCOMMANDLIST list) const;   // -1 if never bound
    void* memory = nullptr;             // RuntimeBacked: the stub shell's memory state (test-runtime-backed.cpp)
};

// What D3D12DDI_HDEVICE points at: the shell's device, from which the resolver finds the context.
struct ShellDevice {
    uint32_t magic;
    engine_ddi::DeviceContext* context;
};
inline constexpr uint32_t kShellDeviceMagic = 0x44534844u;

struct Env {
    ID3D12Device* engine = nullptr;     // the harness's own reference
    PFN_vkGetInstanceProcAddr gipa = nullptr;   // the Vulkan entry the engine device was created with
    BC250_VKD3D_ENGINE_FUNCS funcs{};
    D3D12DDI_DEVICE_FUNCS_CORE_0088 core{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 lists[2]{};     // [0] compute table, [1] graphics table
    Storage storage;
};

struct Device {
    Shell shell;
    ShellDevice sd{kShellDeviceMagic, nullptr};
    engine_ddi::DeviceContext* context = nullptr;
    D3D12DDI_HDEVICE h() { return D3D12DDI_HDEVICE{&sd}; }
};
// A device context over env.engine (or over engine, when given) with the recorder hooks: EnginePrivateTest mode,
// or RuntimeBacked with the given memory hooks (their shell argument is &device.shell).
HRESULT open_device(Env& env, Device& device,
                    decltype(engine_ddi::ShellHooks::allocate_memory) allocate_memory = nullptr,
                    decltype(engine_ddi::ShellHooks::free_memory) free_memory = nullptr, ID3D12Device* engine = nullptr);

// Engine ABI 1.2 r4 V12: a second engine device from the same PRIVATE create info, with a device context over it
// while first is live; the two report different VkInstances through GetVulkanHandles.
void test_private_instances(Env& env, const BC250_VKD3D_DEVICE_CREATE_INFO& create, Device& first);

// ---- Runtime-side helpers ----------------------------------------------------------------------------------------
enum class HeapKind { Upload, Default, Readback };

// A committed buffer: heap and resource created by one CreateHeapAndResource call, as the runtime does for
// CreateCommittedResource (INFERENCE on the runtime's exact shape).
struct Buffer {
    void* heap = nullptr;               // private storage of the heap record
    void* resource = nullptr;           // private storage of the resource record
    int rt = 0;                         // what the runtime handle points at
    D3D12DDI_HHEAP hheap() const { return D3D12DDI_HHEAP{heap}; }
    D3D12DDI_HRESOURCE hres() const { return D3D12DDI_HRESOURCE{resource}; }
};
HRESULT create_buffer(Env& env, Device& device, HeapKind kind, UINT64 size, bool uav, Buffer& out);
// The same with the heap's ByteSize given by the caller instead of taken from the allocation info.
HRESULT create_buffer_in_heap_of(Env& env, Device& device, HeapKind kind, UINT64 size, UINT64 heap_bytes, Buffer& out);
// A heap alone, for buffers, of heap_bytes.
HRESULT create_heap_alone(Env& env, Device& device, HeapKind kind, UINT64 heap_bytes, Buffer& out);
// A buffer placed in base's heap at offset bytes from base (resource description only, ReuseBufferGPUVA naming
// base: engine-ddi.h, placed shape). out.heap stays null; the placed record is destroyed with its hres alone.
HRESULT create_placed_buffer(Env& env, Device& device, const Buffer& base, UINT64 offset, UINT64 size, Buffer& out);
void destroy_buffer(Env& env, Device& device, Buffer& buffer);

// One pool, one recorder and one list of the given queue flags, the list reset and open for recording. A DIRECT
// list gets the runtime's default state; a bundle gets none, most of that state is not legal in a bundle.
struct Recording {
    void* pool = nullptr;
    void* recorder = nullptr;
    void* list = nullptr;
    int rt = 0;
    uint32_t table = 0;
    D3D12DDI_HCOMMANDLIST hlist() const { return D3D12DDI_HCOMMANDLIST{list}; }
    D3D12DDI_HRTCOMMANDLIST rtlist() { return D3D12DDI_HRTCOMMANDLIST{&rt}; }
};
HRESULT open_recording(Env& env, Device& device, D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags, Recording& out,
                       D3D12DDI_COMMAND_LIST_TYPE type = D3D12DDI_COMMAND_LIST_TYPE_DIRECT);
void destroy_recording(Env& env, Device& device, Recording& recording);

// Signals an engine fence of the harness on the engine queue after everything submitted so far and waits for it
// with a NULL event (the INLINE mode's bounded wait, engine V8).
bool wait_queue_idle(Env& env, engine_ddi::EngineQueue* queue, const char* what);
// Whether a fence's completed value shows target reached. UINT64_MAX is what a removed device's fence answers
// (internal.h, kFenceRemoved): it never proves completion.
bool fence_reached(UINT64 completed, UINT64 target);

D3D12DDIARG_RESOURCE_BARRIER_0022 transition(const Buffer& buffer, D3D12DDI_RESOURCE_STATES before,
                                             D3D12DDI_RESOURCE_STATES after);

// ---- Shaders in the DDI form (test-shaders.cpp) ----------------------------------------------------------------------
// A compiled container reduced to what a create-shader slot receives: the program part (SHEX, SHDR or DXIL) as
// pShaderCode, and signature entries without names. The reduction is the harness's model of the runtime, the one
// shader-container's offline control uses (shader-container/README.md, "The offline control"), not a measurement:
// system value from the element's system value (SV_Target and the like become undefined), register, mask, stream,
// component type and minimum precision copied.
struct DdiShader {
    std::vector<UINT> code;
    std::vector<D3D12DDIARG_SIGNATURE_ENTRY_0012> input, output;
    struct Named {
        std::string name;
        UINT index;
        UINT reg;
    };
    std::vector<Named> input_names;     // the container's input elements, for the harness's own input layouts
    UINT input_register(const char* name, UINT index) const;    // ~0u when the input signature has no such element
};
bool ddi_form(const BYTE* container, size_t bytes, DdiShader& out);
// Creates a shader through a create-shader slot with Standard signatures. Returns its private storage; the create
// reports a failure through report_device_error, which the caller checks.
void* create_shader(Env& env, Device& device, PFND3D12DDI_CREATE_SHADER_0026 slot, const DdiShader& shader,
                    D3D12DDI_HROOTSIGNATURE root);

// ---- The stub shell's RuntimeBacked memory (test-runtime-backed.cpp) --------------------------------------------------
// allocate_memory and free_memory as INTEGRATION.md asks of the shell, on the engine's own VkDevice
// (GetVulkanHandles): one whole VkDeviceMemory per heap with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, the type
// vkd3d-proton would pick for the heap's CPU page property, stand-in allocation handles. Device::shell.memory points
// at the StubMemory; open_device(env, device, stub_allocate, stub_free) makes a RuntimeBacked device over it.
struct StubMemory {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties properties{};
    PFN_vkAllocateMemory allocate = nullptr;
    PFN_vkFreeMemory free = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements requirements = nullptr;
    PFN_vkBindBufferMemory bind = nullptr;
    PFN_vkGetBufferDeviceAddress address = nullptr;
    uint32_t allocations = 0;
    uint32_t dedicated = 0;
    uint64_t last_byte_size = 0;                // byte_size of the latest request
    uint32_t last_flags = 0;                    // and its other fields
    uint32_t last_type_bits = 0;
    uint64_t last_alignment = 0;
    uint32_t last_row_pitch = 0;
    uint64_t last_layout_size = 0;
    uint32_t frees = 0;
    D3DKMT_HANDLE next_allocation = 0x40000000u;
};
bool load_stub(Env& env, StubMemory& m);
HRESULT APIENTRY stub_allocate(void* shell, const engine_ddi::MemoryRequest* request, engine_ddi::ImportedMemory* out);
HRESULT APIENTRY stub_free(void* shell, const engine_ddi::ImportedMemory* memory);

// ---- Round trips ---------------------------------------------------------------------------------------------------
void test_copy(Env& env, Device& device);
void test_compute(Env& env, Device& device);
void test_graphics(Env& env, Device& device);
void test_retirement(Env& env);
void test_retire_handoff(Env& env);
void test_device_queries(Env& env, Device& device);
void test_runtime_backed(Env& env);
void test_tiled(Env& env);
void test_small_placement(Env& env);
void test_linear_primary(Env& env);
void test_raytracing(Env& env);
void test_raytracing_pipeline(Env& env);
// GetCaps 1002 and the shell's memory architecture policy, on query_adapter_caps with the harness's create info.
void test_memory_policy(const Env& env, const BC250_VKD3D_DEVICE_CREATE_INFO& create);
// The engine's disk shader cache in INLINE: concurrent pipeline creation, no engine thread, whole archives.
void test_disk_cache(Env& env, const BC250_VKD3D_DEVICE_CREATE_INFO& create, HMODULE engine_dll);

} // namespace harness
