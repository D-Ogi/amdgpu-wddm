// runtime_internal.h - the private state of amdhip64.dll (M16 route B, layer 2).
//
// Layer 2 calls bc250hsa.h and nothing else of the submission path (design
// docs/design/m16-hip-route-b.md, section 1). This header holds the process state: the one
// device, the registered modules, the host-stub map, the allocation table, the kernel argument
// pool, the default stream and the per-thread error state.
//
// Locking: one process lock, and one rule above it. Every entry point that touches the state
// takes the lock, and no thread holds it while it waits for the device. A wait therefore has
// three parts: read the fence value to wait for with the lock held, wait with the lock released
// (bc250hip::Guard::wait), take the lock again and update the state. Layer 1 has one hardware
// queue, so submissions stay serialised whatever the threads do; the rule is about the waits,
// which are the long part. Everything a thread still needs after a wait is either held with a
// reference count (a stream, an event), pinned (an allocation that a GPU copy or fill names,
// Allocation::pins) or looked up again (a function, an allocation), because another thread
// owns the state while the wait runs.
//
// A GPU copy or fill of bc250hsa.h section 5.1 submits and waits inside one layer-1 call, so
// the runtime cannot read a fence value first and wait for it second. It pins the allocations
// the call names, copies their handles, and makes the whole call with the lock released
// (Guard::unlocked). hipFree waits until no such call pins the allocation and until the last
// fence value one of them submitted has retired (hip_memory.cpp, free_allocation).

#ifndef BC250_HIP_RUNTIME_INTERNAL_H
#define BC250_HIP_RUNTIME_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <condition_variable>
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

// refs is the reference count of the object: the handle holds one, and every entry point that
// has to keep the object across a wait holds one more. hipStreamDestroy and hipEventDestroy take
// the handle's reference away, so an object that another thread still waits on dies when that
// thread lets go of it and never under its feet. The count is read and written with the process
// lock held, which is why it needs no atomic.
struct ihipStream_t {
    uint32_t magic;
    unsigned flags;
    uint64_t last_fence;     // the value of the last submission of this stream
    uint64_t pending_wait;   // a value to wait for before the next submission of this stream
    int      refs;
};

struct ihipEvent_t {
    uint32_t magic;
    unsigned flags;
    int      recorded;
    int      pending;        // 1: the value has not retired yet, so host_ms is still open
    uint64_t fence_value;    // the stream's last value at the time of the record
    double   host_ms;        // the host time at which this value retired, in milliseconds
    int      refs;
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
    // The largest dynamic group memory this kernel may ask for, as hipFuncSetAttribute set it.
    // -1 means that no program set it. The value is advice on this part: there is no 48 KiB
    // default ceiling to lift, and layer 1 checks the real dispatch against the hardware. It is
    // kept and reported, so that a program which sets a value above the hardware limit learns
    // it at the call that sets it and not at the launch.
    int                     dynamic_group_max = -1;
};

// pins and busy_fence are the lifetime of a GPU copy or fill that names this allocation and
// runs with the process lock released: how many such calls have not returned yet, and the
// last fence value one of them left with the device (bc250hsa_op_receipt, also when its wait
// failed). They change under the process lock only. They are mutable because the table hands
// out const pointers, and neither of them is part of what the allocation holds.
struct Allocation {
    bc250hsa_mem     mem{};
    mutable int      pins = 0;
    mutable uint64_t busy_fence = 0;
};

// A kernel argument buffer of the pool. The buffer stays in the pool after the launch and is
// used again when its fence retires, so a second run of the same work allocates nothing new.
//
// The invariant of the two flags: `busy` with `fence` 0 means a thread holds this buffer and
// has not submitted it yet, and no other thread may take it. kernarg_acquire therefore clears
// `fence` on every way out, and kernarg_release writes the fence value of the submission (or 0,
// which frees the buffer again when the launch failed). A buffer whose retired fence value
// stayed while a thread held it would read as free for a second thread, and the two would pack
// their arguments into one buffer.
struct KernargBuffer {
    bc250hsa_mem mem{};
    uint64_t     fence = 0;
    bool         busy = false;
    // One thread waits for this buffer's fence with the process lock released. No other thread
    // may take the buffer in the meantime, so a claim is not a second kind of busy: it says
    // that a waiter already owns the outcome of that wait.
    bool         claimed = false;
};

// The dense kernel argument arena (hip_kernarg_arena.cpp). One host-visible allocation of
// kBlocks blocks; a launch takes the next kGranule-aligned slice of the open block, and the
// arguments of consecutive launches therefore lie side by side, as in the kernel argument ring
// of the ROCm runtime (ManagedBuffer of ROCm/clr rocvirtual.cpp). A block is used again only
// when no launch holds a slice of it and the highest fence value of its slices has retired.
// The arena knows offsets only; hip_launch.cpp adds the base address and the waits.
struct KernargArena {
    static constexpr uint32_t kBlocks = 16;
    static constexpr uint32_t kBlockBytes = 64u * 1024u;
    // One GL2 cache line of GFX10. No two launches share a line, so a line that the GPU read
    // for one launch never holds bytes the host processor writes later for another.
    static constexpr uint32_t kGranule = 128;
    // A request above this goes to the pool, so one large kernel cannot use up a block.
    static constexpr uint32_t kMaxSlice = kBlockBytes / 4u;

    struct Block {
        uint64_t fence = 0;     // the highest fence value of a settled slice of this block
        uint32_t takers = 0;    // slices that a launch took and has not settled yet
        bool     claimed = false;   // one thread waits for `fence` with the lock released
    };
    Block    blocks[kBlocks];
    // The open block and the first free byte in it. The start state is the last block, full,
    // so the first take opens block 0.
    uint32_t current = kBlocks - 1u;
    uint32_t offset = kBlockBytes;
};

enum class ArenaTake {
    kTaken,      // *offset_out is the slice; settle it with arena_settle
    kWait,       // the next block waits for *wait_for, and it is claimed: *offset_out is its
                 // start, for arena_unclaim
    kFallback,   // use the pool for this launch
};

// Takes a slice of `bytes` at `alignment` (0 means 16). `retired` is the device fence value
// that has retired. An offset is counted from the start of the arena, not of a block. The
// caller holds the process lock.
ArenaTake arena_take(KernargArena& arena, uint32_t bytes, uint32_t alignment, uint64_t retired,
                     uint32_t* offset_out, uint64_t* wait_for);
// Ends the claim of a kWait answer, after the wait (or its failure). The caller then takes
// again, because the arena can change while the lock is released.
void arena_unclaim(KernargArena& arena, uint32_t block_offset);
// Settles one slice that started at `offset`: fence is the submission's value, or 0 when the
// launch failed and nothing was submitted.
void arena_settle(KernargArena& arena, uint32_t offset, uint64_t fence);

// The kernel arguments of one launch: a slice of the arena, or a buffer of the pool.
struct KernargLease {
    unsigned char* host = nullptr;
    uint64_t       va = 0;
    KernargBuffer* pooled = nullptr;   // null for a slice of the arena
    uint32_t       offset = 0;         // the offset of the slice in the arena
};

struct State {
    std::mutex                              lock;
    // Woken when a GPU copy or fill lets go of its allocations, for a hipFree that waits for
    // the pins of one of them to reach zero.
    std::condition_variable                 unpinned;
    bc250hsa_device*                        dev = nullptr;
    uint64_t                                journal_dispatch_id = 0;
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
    // The dense arena in front of the pool. BC250_HIP_KERNARG_ARENA=0 (lab flavour only) turns
    // it off, so that a lab arm can measure the pool alone. An arena whose allocation fails is
    // turned off for the process, and every launch then takes the pool.
    KernargArena                            kernarg_arena;
    bc250hsa_mem                            kernarg_arena_mem{};
    bool                                    kernarg_arena_enabled = true;
    // The dispatch journal record of the launch in progress (hip_launch.cpp), kept for its capacity.
    std::vector<unsigned char>              launch_record;
    std::vector<ihipEvent_t*>               pending_events; // recorded, value not retired yet
    ihipStream_t                            null_stream{};
    int                                     current_device = 0;
    // The wait policy of the process, which bc250hsa.h rule 6 puts on layer 2. 0 and 0 take
    // the library defaults (1000 ms slices, 120000 ms in all). A short trial sets
    // BC250_HIP_WAIT_TOTAL_MS, so that a dispatch which never retires cannot hold the lab for
    // two minutes per wait (design decision 10).
    uint32_t                                wait_slice_ms = 0;
    uint32_t                                wait_total_ms = 0;
    // The BC250HSA_DISPATCH_* bits every launch of this process carries. It is 0 for a product
    // build; BC250_HIP_PM4_STATE_CACHE=0 adds BC250HSA_DISPATCH_FULL_STATE, which writes the
    // whole compute state for every dispatch of a batch (section 8.8 of the design). The
    // switch exists so that a lab arm can measure the state cache against the build without
    // it, and so that a wrong assumption about persistent register state can be turned off in
    // one environment variable instead of a new build.
    uint32_t                                dispatch_flags = 0;
    // The GPU copy route (scratch/hip-gpu-copy/CONTRACT.md). A device-to-host copy and a
    // device-to-device copy read device memory, which the host processor does at a
    // MEASURED 0.21 GB/s, so from gpu_copy_min_bytes upwards they go to the command
    // processor instead. The numbers arrive in the environment:
    //
    //   BC250_HIP_GPU_COPY=0|1      the whole route; the default is on
    //   BC250_HIP_GPU_COPY_MIN=<n>  bytes from which the GPU route is taken, default 4096.
    //                               0 is a value and not an absent variable: it puts every
    //                               copy on the GPU route
    //   BC250_HIP_GPU_FILL=0|1      hipMemset of a device allocation that has a host mapping
    //
    // The threshold is derived in CONTRACT.md section 10 from two measured rates: a
    // submission with its fence costs 14.2 us, and the host-processor read it replaces
    // runs at 0.21 GB/s, so the two meet near 4 KB.
    //
    // gpu_fill_mapped is off by default, and that is not an oversight: a host-processor
    // fill of device memory is a write, and a write of this mapping is MEASURED at
    // 15.6 GB/s (evidence/m16/perf-rerun-2026-10-09), so there is nothing to win. An
    // allocation with no host mapping has no host-processor path at all and always takes
    // the GPU fill; on this part hipMalloc produces no such allocation, because it falls
    // back to host-visible memory when the mapping fails (hip_memory.cpp, allocate_device),
    // so the switch is the only way the GPU fill runs here at all.
    //
    // BC250_HIP_GPU_FILL=1 turns the GPU fill on for every device allocation, so that the
    // lab can measure the two against each other. It does not reach a hipHostMalloc
    // pointer: that fill stays a host-processor write of memory the program already holds a
    // pointer to, and sending it to the engine would put the program's next read of it
    // behind the coherence control of CONTRACT.md section 8 for nothing.
    bool                                    gpu_copy_enabled = true;
    bool                                    gpu_fill_mapped = false;
    uint32_t                                gpu_copy_min_bytes = 4096;
    // Streams and events that exist now, the null stream apart. A host test reads them to see
    // that an object destroyed while another thread waited on it is freed one time and not
    // twice, which a reference count that counted wrong would show here as a number that never
    // returns to zero.
    int                                     live_streams = 0;
    int                                     live_events = 0;
};

State& state();

// Does this build hold the process lock while it waits for the device? The product build does
// not, and the answer is therefore no. A build with BC250_HIP_WAIT_UNDER_LOCK=1 restores the
// behaviour of the first build of layer 2 and is the negative control of the multithreaded host
// test: with it, a thread in a long wait stops every other thread. Nothing but that test build
// defines the macro.
#if defined(BC250_HIP_WAIT_UNDER_LOCK) && BC250_HIP_WAIT_UNDER_LOCK
#define BC250_HIP_WAIT_UNDER_LOCK_BUILD 1
#else
#define BC250_HIP_WAIT_UNDER_LOCK_BUILD 0
#endif
bool waits_under_lock();

// The negative control of the dispatch packet (section 7.1 of bc250hsa.h, defect BD-110). A
// build with BC250_HIP_NO_DISPATCH_PACKET=1 passes no packet address, so every kernel that
// reads its own blockDim is refused exactly as the build of 2026-10-09 refused it. It exists
// so that the fix can be shown to be the fix, offline, against the mock backend, and nothing
// but that control build defines the macro.
#if defined(BC250_HIP_NO_DISPATCH_PACKET) && BC250_HIP_NO_DISPATCH_PACKET
#define BC250_HIP_NO_DISPATCH_PACKET_BUILD 1
#else
#define BC250_HIP_NO_DISPATCH_PACKET_BUILD 0
#endif

// The process lock, and the only way to wait for the device.
//
// An entry point writes `Guard guard;` and holds the state. When it has to wait, it reads the
// fence value under the lock and calls guard.wait(), which releases the lock, waits in layer 1
// and takes the lock again. Layer 1's wait is safe to enter from several threads at once: it
// creates its own operating system event per call and its counters are interlocked.
class Guard {
public:
    Guard() : held_(state().lock) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    // Waits until the device reaches value. The lock is released while it waits and held again
    // when this returns, whatever the answer is. On success it stamps every recorded event the
    // device has passed. The caller must assume that the state changed under it.
    hipError_t wait(bc250hsa_device* dev, uint64_t value);

    // Runs f with the lock released and holds the lock again when this returns. It is for a
    // layer-1 call that submits and waits for its own work, a GPU copy or fill of bc250hsa.h
    // section 5.1. f touches nothing of the state: the caller copies what f needs and pins the
    // allocations it names before it calls this. The negative-control build keeps the lock, as
    // it does for every other wait.
    template <typename F>
    void unlocked(F&& f) {
#if BC250_HIP_WAIT_UNDER_LOCK_BUILD
        f();
#else
        held_.unlock();
        f();
        held_.lock();
#endif
    }

    // Waits on cv with the lock released until ready(), which runs with the lock held.
    template <typename P>
    void wait_until(std::condition_variable& cv, P ready) {
        cv.wait(held_, ready);
    }

private:
    std::unique_lock<std::mutex> held_;
};

// The lazy device. It opens the adapter on the first call that needs it. Every later call
// returns the same status, so a machine with no BC-250 adapter answers at once.
hipError_t device(bc250hsa_device** out);

// The status translation of design section 4.5. One table and no judgement.
hipError_t translate(bc250hsa_status status);

// The per-thread error state. hipGetLastError clears it and hipPeekAtLastError does not.
hipError_t last_error_peek();
hipError_t last_error_take();
void       last_error_set(hipError_t err);

// The diagnostic log of this runtime (hip_log.cpp). BC250_HIP_LOG turns it on, and with it on
// every refusal of this runtime and every bc250hsa_log line of layer 1 names itself. It is off
// by default and then costs one read of a local static per call.
void log_start();
bool log_on();
void log_line(uint32_t level, const char* format, ...);

// Records an error and returns it, so that an entry point can write `return fail(...)`.
inline hipError_t fail(hipError_t err) {
    if (err != hipSuccess) {
        last_error_set(err);
    }
    return err;
}
inline hipError_t fail_status(bc250hsa_status status) { return fail(translate(status)); }

// A refusal that says which call refused what and why. It records the error exactly as fail()
// does, and with the log off it costs the same. Every path of this runtime that answers "this
// build cannot do that" goes through one of the two, so that a lab session never again has to
// guess which of them spoke (defect BD-110).
hipError_t refuse(const char* call, const char* kernel, const char* why, hipError_t err);
hipError_t refuse_status(const char* call, const char* kernel, bc250hsa_status status);

// The allocation table. find_allocation accepts an interior pointer, because a HIP program
// passes `buffer + offset` to hipMemcpy.
const Allocation* find_allocation(uint64_t va, uint64_t bytes, uint64_t* offset);
const Allocation* find_allocation_ptr(const void* ptr, uint64_t bytes, uint64_t* offset);

// The default stream of the process. A HIP program passes a null stream handle for it.
ihipStream_t* resolve_stream(hipStream_t stream);
bool          stream_valid(const ihipStream_t* stream);
bool          is_null_stream(const ihipStream_t* stream);

// The reference counts. Every one of these needs the process lock. The null stream is part of
// State and is never counted and never freed.
void stream_retain(ihipStream_t* stream);
void stream_release(ihipStream_t* stream);
void event_retain(ihipEvent_t* event);
void event_release(ihipEvent_t* event);

// Holds a stream across a wait that releases the lock, so that hipStreamDestroy on another
// thread cannot free it under the waiter. Declare it after the Guard: it must let go of the
// object while the lock is still held.
class StreamRef {
public:
    StreamRef() = default;
    StreamRef(const StreamRef&) = delete;
    StreamRef& operator=(const StreamRef&) = delete;
    ~StreamRef() { reset(); }
    void attach(ihipStream_t* stream) {
        reset();
        stream_ = stream;
        stream_retain(stream_);
    }
    void reset() {
        if (stream_ != nullptr) {
            stream_release(stream_);
            stream_ = nullptr;
        }
    }
    ihipStream_t* get() const { return stream_; }

private:
    ihipStream_t* stream_ = nullptr;
};

// The same for an event.
class EventRef {
public:
    EventRef() = default;
    EventRef(const EventRef&) = delete;
    EventRef& operator=(const EventRef&) = delete;
    ~EventRef() { reset(); }
    void attach(ihipEvent_t* event) {
        reset();
        event_ = event;
        event_retain(event_);
    }
    void reset() {
        if (event_ != nullptr) {
            event_release(event_);
            event_ = nullptr;
        }
    }
    ihipEvent_t* get() const { return event_; }

private:
    ihipEvent_t* event_ = nullptr;
};

// The fence value that a program sees as "everything this stream owes". It covers the work of
// the stream and the event wait that the stream still carries. The legacy null stream owes the
// work of the whole device, which is what HIP says of it.
uint64_t stream_target_value(bc250hsa_device* dev, const ihipStream_t* stream);

// Stamps every recorded event whose value the device has reached. Every wait calls it, so an
// event carries the host time of the moment its value retired and not the time a program asked
// about it.
void events_stamp_retired(bc250hsa_device* dev);
void events_forget(ihipEvent_t* event);

// Waits for the event wait that this stream still carries, then clears it. The caller holds the
// lock and the stream (StreamRef), because the wait releases the lock.
hipError_t stream_drain_pending(Guard& guard, ihipStream_t* stream);

// The kernel argument buffer pool. A full pool waits for the oldest buffer, so this takes the
// guard as well.
hipError_t kernarg_acquire(Guard& guard, uint32_t bytes, uint32_t alignment,
                           KernargBuffer** out);
void       kernarg_release(KernargBuffer* buffer, uint64_t fence);
// The kernel arguments of one launch: the arena first, the pool when the arena cannot give a
// slice. Both can wait, so both take the guard. kernarg_settle ends the lease exactly once.
hipError_t kernarg_take(Guard& guard, uint32_t bytes, uint32_t alignment, KernargLease* out);
void       kernarg_settle(KernargLease& lease, uint64_t fence);

// Loads the code object of a module, once. The caller holds the lock.
bc250hsa_status module_ensure_loaded(Module* module);

// Caller holds state lock; output owns every byte through recorded submit.
hipError_t prepare_dispatch_record(const bc250hsa_dispatch& dispatch,
                                  const void* packed, uint32_t packed_bytes,
                                  uint64_t dispatch_id,
                                  const std::map<uint64_t, Allocation>& allocations,
                                  std::vector<unsigned char>& output);

// The occupancy model of hipOccupancyMaxActiveBlocksPerMultiprocessor (hip_occupancy.cpp). It
// is pure, so a host test checks it with no device. One "multiprocessor" is one compute unit of
// two SIMDs, which is what hipGetDeviceProperties counts in multiProcessorCount.
struct OccupancyInput {
    uint32_t block_threads = 0;     // the blockSize argument
    uint32_t wave_size = 0;         // 32 on this part
    uint32_t waves_per_cu = 0;      // bc250hsa_props.waves_per_cu, both SIMDs
    uint32_t vgprs = 0;             // VGPRs of one wave, occupancy_kernel_vgprs
    uint64_t group_bytes = 0;       // the static group segment plus the dynamic request
    uint64_t lds_bytes_per_cu = 0;  // the local memory of one compute unit
};
uint32_t occupancy_kernel_vgprs(const bc250hsa_kernel& kernel);
int      occupancy_blocks_per_cu(const OccupancyInput& in);

// A monotonic host clock in milliseconds, for hipEventElapsedTime.
double host_now_ms();

// The free and total device memory, from the properties and the allocation table.
void memory_info(size_t* free_bytes, size_t* total_bytes);

// The text of BC250_HIP_GPU_COPY_MIN as a number of bytes: decimal digits only, at most
// 4294967295. A sign, a space, a suffix or a value out of range is false and *out is not
// written, so the default stands.
bool parse_copy_min(const char* text, uint32_t* out);

}  // namespace bc250hip

#endif  // BC250_HIP_RUNTIME_INTERNAL_H
