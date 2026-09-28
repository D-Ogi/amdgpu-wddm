// SPDX-License-Identifier: MIT
// engine-ddi-harness: plays the D3D12 runtime and the native12 shell against the real engine DLL on the
// development PC. It proves the positive paths the shell integration needs: table composition, heap and resource
// creation, command lists, execution through an engine queue, fence completion, Map and readback, and a compute
// dispatch on the harness-only container path. Built only with AMDGPU_WDDM_ENGINE_DDI_HARNESS.
#pragma once
#include "internal.h"                   // engine-ddi.h and the harness-only entry points
#include <cstdint>
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
    struct Bind { void* list; uint32_t table; };
    std::vector<Bind> binds;
    int table_of(D3D12DDI_HRTCOMMANDLIST list) const;   // -1 if never bound
};

// What D3D12DDI_HDEVICE points at: the shell's device, from which the resolver finds the context.
struct ShellDevice {
    uint32_t magic;
    engine_ddi::DeviceContext* context;
};
inline constexpr uint32_t kShellDeviceMagic = 0x44534844u;

struct Env {
    ID3D12Device* engine = nullptr;     // the harness's own reference
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
// A device context in EnginePrivateTest mode over env.engine, with the recorder hooks.
HRESULT open_device(Env& env, Device& device);

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
void destroy_buffer(Env& env, Device& device, Buffer& buffer);

// One pool, one recorder and one list of the given queue flags, the list reset and open for recording.
struct Recording {
    void* pool = nullptr;
    void* recorder = nullptr;
    void* list = nullptr;
    int rt = 0;
    uint32_t table = 0;
    D3D12DDI_HCOMMANDLIST hlist() const { return D3D12DDI_HCOMMANDLIST{list}; }
    D3D12DDI_HRTCOMMANDLIST rtlist() { return D3D12DDI_HRTCOMMANDLIST{&rt}; }
};
HRESULT open_recording(Env& env, Device& device, D3D12DDI_COMMAND_QUEUE_FLAGS queue_flags, Recording& out);
void destroy_recording(Env& env, Device& device, Recording& recording);

// Signals an engine fence of the harness on the engine queue after everything submitted so far and waits for it
// with a NULL event (the INLINE mode's bounded wait, engine V8).
bool wait_queue_idle(Env& env, engine_ddi::EngineQueue* queue, const char* what);

D3D12DDIARG_RESOURCE_BARRIER_0022 transition(const Buffer& buffer, D3D12DDI_RESOURCE_STATES before,
                                             D3D12DDI_RESOURCE_STATES after);

// ---- Round trips ---------------------------------------------------------------------------------------------------
void test_copy(Env& env, Device& device);
void test_compute(Env& env, Device& device);
void test_retirement(Env& env);
void test_device_queries(Env& env, Device& device);

} // namespace harness
