// SPDX-License-Identifier: MIT
// engine-ddi: asynchronous pipeline-state compilation (set_pso_policy, engine-ddi.h; BD-158). Off, the create slot
// translates the description and calls the engine on the calling thread, as it always did. On, the slot keeps every
// check and every refusal on that thread, puts a deep copy of the description (PsoPlan) into a job, and returns
// S_OK; a pool thread makes the engine's create. Every consumer of the pipeline waits for its job, or runs it on its
// own thread when no worker has started it.
//
// Ownership, the rules the code keeps:
//   P1 A job is owned by exactly one party at a time: the pool queue while it is Queued, the thread that claimed it
//      while it is Running, and the pipeline record once it is Done. Only the owner writes it.
//   P2 The runner (a pool thread, or a consumer that claimed a queued job) touches the job and its DeviceContext
//      only before it publishes Done. A pool thread publishes after its run hook has returned. After the publish
//      it touches pool-global memory alone, because the record
//      that holds the job may be destroyed, and the job freed, the moment Done is visible.
//   P3 A consumer waits on the pool's generation word, which no device frees, and re-reads the job's state; the
//      runner bumps that word and wakes the waiters after the publish. A bounded WaitOnAddress makes a lost wake
//      a latency, never a hang.
//   P4 A record keeps live != 0 in its DeviceContext until its destroy, and a destroy waits for the job. So a
//      DeviceContext is alive for as long as any job of it is not Done, and destroy_device_context cannot run
//      while a compile is in flight.
//   P5 Nothing is joined in DllMain. The pool's threads are started by the first device that turns the policy on
//      and joined by the last device that turns it off, both inside a DDI call.
// Fields, writer -> readers:
//   PsoJob::state: queue claims under the pool lock; Done release -> every consumer's acquire. PsoJob::pso and ::hr:
//     the runner before the Done
//     store (release) -> the consumers after their acquire load of state.
//   PsoJob::retry: Available only for a failed pool create, set before Done. One consumer claims it with CAS,
//     writes retry_pso/retry_hr, and publishes Done (release); other consumers acquire that result. The original
//     pso/hr never change after the first Done. The runtime keeps the record alive for concurrent consumers;
//     its destroy waits for an active retry and releases both results exactly once.
//   PsoAsync counters: every thread, relaxed -> the summary line at the device's destruction.
#pragma once
#include "internal.h"
#include <cstring>
#include <string>
#include <vector>

namespace engine_ddi {

// Pool threads. The default (PsoPolicy::workers 0) is logical processors - 1, at least one: the thread that creates
// the pipelines is busy creating them.
inline constexpr uint32_t kMaxPsoWorkers = 64;

// The programs a plan can carry, one slot each.
enum PsoStage : uint32_t { kPsoStageVS, kPsoStagePS, kPsoStageGS, kPsoStageHS, kPsoStageDS, kPsoStageCS,
                           kPsoStageMS, kPsoStageAS, kPsoStageCount };

// The subobject stream of a rasterizing pipeline, in the layout the engine reads: a packed struct of the type and
// the value, its size rounded up to a multiple of a pointer, laid out end to end (vkd3d-proton state.c,
// VKD3D_HANDLE_SUBOBJECT_EXPLICIT: `stream_ptr += align(sizeof(*subobject), sizeof(void*))`). The stride is a
// multiple of `sizeof(void*)`, not of 8: the 32-bit shell of a WoW64 process builds this same file, and there a
// pointer is 4 bytes, so an MS subobject is 12 bytes and not 16. A stride of 8 there would leave our own padding
// where the engine reads the next type word, which reads as type 0 (ROOT_SIGNATURE, already seen) and fails every
// pipeline creation with "Duplicate pipeline subobject type". The buffer is a vector of 64-bit words, so its first
// byte is aligned for every value a subobject carries, and every stride is a multiple of the alignment the
// static_assert admits.
//
// A subobject the stream leaves out keeps the engine's own default (vkd3d-proton state.c,
// d3d12_init_pipeline_state_desc), which is the D3D12 API default. put_common_state (pipelines.cpp) writes every
// state a D3D12_GRAPHICS_PIPELINE_STATE_DESC used to carry, so no path depends on those defaults.
class SubobjectStream {
public:
    template <class T> void put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& value) {
        struct Subobject {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            T data;
        };
        constexpr size_t kAlign = sizeof(void*);             // the engine's stride, 8 on x64 and 4 on x86
        static_assert(std::is_trivially_copyable_v<T>, "a subobject value is copied into the stream");
        static_assert(alignof(Subobject) <= kAlign, "the engine's pointer-sized stride aligns every subobject");
        Subobject object{};                                  // every padding byte zero: nothing uninitialized ships
        object.type = type;
        object.data = value;
        const size_t padded = (sizeof(Subobject) + kAlign - 1) / kAlign * kAlign;
        const size_t at = size_;
        words_.resize((at + padded + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        std::memcpy(reinterpret_cast<uint8_t*>(words_.data()) + at, &object, sizeof(object));
        size_ = at + padded;
    }
    D3D12_PIPELINE_STATE_STREAM_DESC desc() noexcept {
        return {size_, words_.empty() ? nullptr : words_.data()};
    }

private:
    std::vector<uint64_t> words_;
    size_t size_ = 0;
};

// One pipeline description with every byte the engine create reads inside it. The DDI arrays live only for the
// length of the create call and the rebuilt containers of the ShaderObjects only as long as the shader records,
// while a job outlives both, so nothing the engine reads may stay behind a pointer of theirs.
//
// Plan rule: every array the stream (or the compute description) points at is one of the vectors below, sized once
// and then written in place. A pointer taken into it afterwards stays valid for the plan's life, and a move of the
// plan moves no element: a vector move steals the buffer, so the elements, the std::string objects among them, keep
// their addresses. A plan is therefore moved and never copied, and no vector of it grows after a pointer into it
// has gone into the stream.
//
// The reference on the root signature belongs to the job, not to the plan, so that a plan stays plain data.
struct PsoPlan {
    SubobjectStream stream;                             // a rasterizing pipeline
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};   // a compute pipeline; its CS points into code[kPsoStageCS]
    bool compute = false;
    ID3D12RootSignature* root = nullptr;                // no reference here (see above)
    // The storage the stream and the compute description point at.
    std::vector<uint8_t> code[kPsoStageCount];          // empty: the stage has no program
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> element_names;             // one per element, named by the stream's elements
    std::vector<D3D12_SO_DECLARATION_ENTRY> so;
    std::vector<std::string> so_names;                  // one per entry; empty for a gap (a NULL SemanticName)
    std::vector<UINT> so_strides;
    std::vector<D3D12_VIEW_INSTANCE_LOCATION> locations;

    PsoPlan() = default;
    PsoPlan(PsoPlan&&) = default;
    PsoPlan& operator=(PsoPlan&&) = default;
    PsoPlan(const PsoPlan&) = delete;                   // the plan rule above: a copy would move the elements
    PsoPlan& operator=(const PsoPlan&) = delete;

    // The bytecode of one stage, copied into this plan, as the stream names it. An absent program stays empty.
    D3D12_SHADER_BYTECODE take_code(uint32_t stage, const void* bytes, size_t size) {
        if (!bytes || !size) return {};
        const auto* p = static_cast<const uint8_t*>(bytes);
        code[stage].assign(p, p + size);
        return {code[stage].data(), code[stage].size()};
    }
};

enum class PsoState : uint32_t { Queued = 0, Running = 1, Done = 2 };
enum class PsoRetry : uint32_t { None = 0, Available = 1, Running = 2, Done = 3 };

struct PsoJob {
    DeviceContext* context = nullptr;
    PsoPlan plan;
    ID3D12RootSignature* root_ref = nullptr;            // the plan's root signature, one reference
    std::atomic<uint32_t> state{static_cast<uint32_t>(PsoState::Queued)};
    ID3D12PipelineState* pso = nullptr;                  // the runner's result, before the Done store
    HRESULT hr = S_OK;                                   // the engine create's result, likewise
    bool compiled = false;                              // the run hook called its body; only the runner writes
    uint32_t device_number = 0;                         // kept for diagnostics even after the policy is off
    std::atomic<uint32_t> retry{static_cast<uint32_t>(PsoRetry::None)};
    ID3D12PipelineState* retry_pso = nullptr;            // separate result: the first result stays immutable
    HRESULT retry_hr = S_OK;                            // published by retry == Done
    uint64_t queued_qpc = 0;
    uint64_t id = 0;                                     // diagnostic, from DeviceContext::next_id
    std::atomic<uint32_t> reported{0};                   // a failed create is logged once, reported at every use
    PsoJob* prev = nullptr;                              // the pool queue, under its lock; linked while Queued
    PsoJob* next = nullptr;
};

// The per-device state of the policy, allocated by set_pso_policy and freed by pso_off.
struct PsoAsync {
    PsoPolicy policy{};
    uint64_t qpf = 1;
    uint32_t number = 0;                                // the device's number in this process, for the log lines
    PsoAsync* next_device = nullptr;                    // the pool's list of devices with the policy on, under its lock
    // The periodic summary (policy.summary_period_ms): activity is bumped by every create and every consumer, and
    // the two words below hold what the last line saw, so an idle device writes nothing and a working one writes
    // one line a period whichever thread gets there first.
    std::atomic<uint64_t> activity{0};
    std::atomic<uint64_t> summary_ms{0};
    std::atomic<uint64_t> summary_activity{0};
    std::atomic<uint64_t> periodic{0};                  // periodic lines written, diagnostic
    std::atomic<uint32_t> outstanding{0};               // jobs of this device that are not Done
    std::atomic<uint64_t> queued{0};                    // creates the pool took
    std::atomic<uint64_t> synchronous{0};               // creates made on the calling thread anyway
    std::atomic<uint64_t> by_worker{0};                 // jobs a pool thread ran
    std::atomic<uint64_t> by_consumer{0};               // jobs a consumer ran on its own thread
    std::atomic<uint64_t> waits{0};                     // consumers that waited for a running job
    std::atomic<uint64_t> wait_qpc{0};                  // their waiting time together
    std::atomic<uint64_t> longest_wait_qpc{0};
    std::atomic<uint64_t> failures{0};                  // engine creates that failed after the S_OK
    std::atomic<uint64_t> retried{0};                   // once per failed pool job, on a consumer thread
    std::atomic<uint64_t> retry_failures{0};            // separate from failures, which counts the first creates
    std::atomic<uint64_t> aborted{0};                   // jobs pso_off ended without a create
};

// set_pso_policy's two halves (context.cpp calls them; the validation is there).
HRESULT pso_on(DeviceContext* context, const PsoPolicy* policy) noexcept;
// Ends every job of this context that is still queued, writes the summary line, leaves the pool (the last leave
// stops its threads) and frees the context's PsoAsync. A context without the policy is left alone.
void pso_off(DeviceContext* context) noexcept;

// The engine create of one plan on the engine device, on the calling thread. The synchronous path and every runner
// go through it. A load out of a pipeline library is not here: it stays in pipelines.cpp and stays synchronous.
HRESULT pso_create_now(DeviceContext* context, PsoPlan& plan, ID3D12PipelineState** out) noexcept;

// Hands the plan to the pool. On success the caller's pipeline record owns the returned job and ends it with
// pso_destroy. Null when the policy is off or the job could not be made, and the plan is then untouched: the
// caller creates with pso_create_now on its own thread instead.
PsoJob* pso_submit(DeviceContext* context, PsoPlan&& plan) noexcept;

// The pipeline of a job, on a DDI thread: runs the job here when no worker has started it, else waits for the
// worker. A failed pool create gets one synchronous retry; concurrent consumers share that result. A queued job
// first compiled by its consumer is already synchronous and is not retried. *hr gets the final engine result
// (S_OK with a pipeline, or the failure, with null). Calls the
// policy's drained hook, so a device removal a pool thread saw is reported from this thread.
ID3D12PipelineState* pso_resolve(PsoJob* job, HRESULT* hr) noexcept;

// The record's destroy: ends the job (takes it over and ends it without a create when it is still queued, waits
// for a running one), releases the pipeline it holds and frees the job. The pipeline is released here, inside the
// destroy, exactly as release_engine did it before the policy existed.
void pso_destroy(PsoJob* job) noexcept;

// Whether the engine has not made this job's pipeline yet. A consumer that only needs the pipeline later asks
// this before it waits (pipelines.cpp, the deferred library store).
inline bool pso_pending(const PsoJob* job) noexcept {
    return job && job->state.load(std::memory_order_acquire) != static_cast<uint32_t>(PsoState::Done);
}

// The pool's thread count right now, 0 with no device on the pool. For a log line and for the host tests; a test
// that wants a job to stay queued asks for one worker and keeps that worker inside its run hook.
uint32_t pso_pool_workers() noexcept;

// The clock the periodic summary reads, milliseconds, GetTickCount64 by default. Replaceable so that a host test
// steps time instead of waiting half a minute for it; null restores the default. Written before any device turns
// the policy on, never while one is running, and not part of the boundary (engine-ddi.h).
using PsoClock = uint64_t (*)() noexcept;
void pso_set_clock(PsoClock clock) noexcept;

} // namespace engine_ddi
