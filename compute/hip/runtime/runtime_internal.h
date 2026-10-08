// runtime_internal.h - the private state of amdhip64.dll (M16 route B, layer 2).
//
// Layer 2 calls bc250hsa.h and nothing else of the submission path (design
// docs/design/m16-hip-route-b.md, section 1). This header holds the process state: the one
// device, the registered modules, the host-stub map, the allocation table, the kernel argument
// pool, the default stream and the per-thread error state.
//
// Locking: one process lock. Every entry point that touches the state takes it. The design
// gives layer 1 one hardware queue, so a finer lock would buy nothing in build 1.

#ifndef BC250_HIP_RUNTIME_INTERNAL_H
#define BC250_HIP_RUNTIME_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bc250hsa.h"
#include "hip/hip_runtime.h"

// The magic words catch a handle of the wrong kind, which a HIP program can pass by mistake.
#define BC250_HIP_STREAM_MAGIC 0x53524D54u /* 'SRMT' */
#define BC250_HIP_EVENT_MAGIC  0x45564E54u /* 'EVNT' */

struct ihipStream_t {
    uint32_t magic;
    unsigned flags;
    uint64_t last_fence;     // the value of the last submission of this stream
    uint64_t pending_wait;   // a value to wait for before the next submission of this stream
};

struct ihipEvent_t {
    uint32_t magic;
    unsigned flags;
    int      recorded;
    int      pending;        // 1: the value has not retired yet, so host_ms is still open
    uint64_t fence_value;    // the stream's last value at the time of the record
    double   host_ms;        // the host time at which this value retired, in milliseconds
};

namespace bc250hip {

// One device global that clang registered with __hipRegisterVar. Step 2 has no symbol entry
// point, so the record exists to resolve the address once the module loads and to report a
// global that the code object does not hold.
struct Var {
    void*       host_var = nullptr;
    std::string device_name;
    size_t      bytes = 0;
    uint64_t    va = 0;
    bool        resolved = false;
};

// One registered fat binary. The bundle is read at registration, because a wrong target must be
// reported early, and the code object is loaded on the first launch, so a process that starts
// no kernel pays nothing (design section 4.2).
struct Module {
    const void*       wrapper = nullptr;   // the .hipFatBinSegment wrapper that clang passes
    const void*       image = nullptr;     // the code object inside the bundle, not copied
    size_t            image_bytes = 0;
    bc250hsa_module*  loaded = nullptr;
    bc250hsa_status   bundle_status = BC250HSA_OK;
    bc250hsa_status   load_status = BC250HSA_OK;
    bool              load_tried = false;
    // How often this wrapper is registered now. __hipUnregisterFatBinary takes one away and
    // tears the module down at zero, so a wrapper that registered twice survives the first
    // unregister. It is a count of live registrations and not a total.
    int               registrations = 0;
    std::vector<Var>  vars;
};

// One host stub of a __global__ function. hipLaunchKernel receives the stub address and must
// find the kernel from it (design section 4.2).
struct Function {
    Module*                 module = nullptr;
    std::string             device_name;
    const bc250hsa_kernel*  kernel = nullptr;
};

struct Allocation {
    bc250hsa_mem mem{};
};

// A kernel argument buffer of the pool. The buffer stays in the pool after the launch and is
// used again when its fence retires, so a second run of the same work allocates nothing new.
struct KernargBuffer {
    bc250hsa_mem mem{};
    uint64_t     fence = 0;
    bool         busy = false;
};

struct State {
    std::mutex                              lock;
    bc250hsa_device*                        dev = nullptr;
    bool                                    open_tried = false;
    bc250hsa_status                         open_status = BC250HSA_OK;
    bc250hsa_props                          props{};
    bool                                    props_valid = false;
    std::vector<Module*>                    modules;
    std::unordered_map<const void*, Module*> module_by_wrapper;
    std::unordered_map<const void*, Function> functions;   // keyed by the host stub address
    std::map<uint64_t, Allocation>          by_va;         // ordered, for a range lookup
    std::unordered_map<const void*, uint64_t> va_by_host;
    std::vector<KernargBuffer>              kernargs;
    std::vector<ihipEvent_t*>               pending_events; // recorded, value not retired yet
    ihipStream_t                            null_stream{};
    int                                     current_device = 0;
    // The wait policy of the process, which bc250hsa.h rule 6 puts on layer 2. 0 and 0 take
    // the library defaults (1000 ms slices, 120000 ms in all). A short trial sets
    // BC250_HIP_WAIT_TOTAL_MS, so that a dispatch which never retires cannot hold the lab for
    // two minutes per wait (design decision 10).
    uint32_t                                wait_slice_ms = 0;
    uint32_t                                wait_total_ms = 0;
};

State& state();

// The lazy device. It opens the adapter on the first call that needs it. Every later call
// returns the same status, so a machine with no BC-250 adapter answers at once.
hipError_t device(bc250hsa_device** out);

// The status translation of design section 4.5. One table and no judgement.
hipError_t translate(bc250hsa_status status);

// The per-thread error state. hipGetLastError clears it and hipPeekAtLastError does not.
hipError_t last_error_peek();
hipError_t last_error_take();
void       last_error_set(hipError_t err);

// Records an error and returns it, so that an entry point can write `return fail(...)`.
inline hipError_t fail(hipError_t err) {
    if (err != hipSuccess) {
        last_error_set(err);
    }
    return err;
}
inline hipError_t fail_status(bc250hsa_status status) { return fail(translate(status)); }

// The allocation table. find_allocation accepts an interior pointer, because a HIP program
// passes `buffer + offset` to hipMemcpy.
const Allocation* find_allocation(uint64_t va, uint64_t bytes, uint64_t* offset);
const Allocation* find_allocation_ptr(const void* ptr, uint64_t bytes, uint64_t* offset);

// Every wait of layer 2 goes through this one function, so that the process has one wait
// policy and no entry point can wait without the bound that State holds. The caller holds the
// lock.
hipError_t wait_fence(bc250hsa_device* dev, uint64_t value);

// The default stream of the process. A HIP program passes a null stream handle for it.
ihipStream_t* resolve_stream(hipStream_t stream);
bool          stream_valid(const ihipStream_t* stream);
bool          is_null_stream(const ihipStream_t* stream);

// The fence value that a program sees as "everything this stream owes". It covers the work of
// the stream and the event wait that the stream still carries. The legacy null stream owes the
// work of the whole device, which is what HIP says of it.
uint64_t stream_target_value(bc250hsa_device* dev, const ihipStream_t* stream);

// Stamps every recorded event whose value the device has reached. Every wait calls it, so an
// event carries the host time of the moment its value retired and not the time a program asked
// about it.
void events_stamp_retired(bc250hsa_device* dev);
void events_forget(ihipEvent_t* event);

// Waits for everything this stream owes, then clears its pending wait. The caller holds the
// lock.
hipError_t stream_drain_pending(ihipStream_t* stream);

// The kernel argument buffer pool.
hipError_t kernarg_acquire(uint32_t bytes, uint32_t alignment, KernargBuffer** out);
void       kernarg_release(KernargBuffer* buffer, uint64_t fence);

// Loads the code object of a module, once. The caller holds the lock.
bc250hsa_status module_ensure_loaded(Module* module);

// A monotonic host clock in milliseconds, for hipEventElapsedTime.
double host_now_ms();

// The free and total device memory, from the properties and the allocation table.
void memory_info(size_t* free_bytes, size_t* total_bytes);

}  // namespace bc250hip

#endif  // BC250_HIP_RUNTIME_INTERNAL_H
