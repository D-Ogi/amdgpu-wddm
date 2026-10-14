// SPDX-License-Identifier: MIT
// engine-ddi: the pipeline-state compile pool (pso-async.h, set_pso_policy; BD-158).
//
// "Co masz zrobic jutro, zrob dzisiaj - a najlepiej na wszystkich rdzeniach." What you can do tomorrow, do today,
// and best of all on every core.
#include "pso-async.h"
#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "synchronization.lib")     // WaitOnAddress, WakeByAddressAll

namespace engine_ddi {
namespace {

uint64_t qpc() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

uint64_t tick_ms() noexcept { return GetTickCount64(); }
// The clock of the periodic summary. A plain pointer: a host test sets it before any device turns the policy on
// (pso-async.h), and nothing changes it afterwards.
PsoClock g_clock = tick_ms;

void pso_log(const PsoAsync* a, _Printf_format_string_ const char* format, ...) noexcept {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log_line("%s", text);
    if (a && a->policy.log) a->policy.log(a->policy.shell, text);
}

// ---- The counters ------------------------------------------------------------------------------------------------
// One writer for the three places that print them: the device's destruction, the periodic line while the device
// works, and the shell's DLL_PROCESS_DETACH. The reason word says which one it was. Formats and writes, nothing
// else: no lock, no wait, no allocation, so that the detach site can use it as it is.
void write_summary(PsoAsync* a, const char* reason, uint32_t stranded) noexcept {
    const uint64_t waits = a->waits.load(std::memory_order_relaxed);
    const uint64_t wait_us = a->wait_qpc.load(std::memory_order_relaxed) * 1000000 / a->qpf;
    const uint64_t longest_us = a->longest_wait_qpc.load(std::memory_order_relaxed) * 1000000 / a->qpf;
    pso_log(a, "pso summary (%s, device %u): %llu queued, %llu synchronous, %llu by a pool thread, "
               "%llu by the consumer, %llu waits (%llu us, longest %llu us), %llu failures, "
               "%llu ended without a create, %u stranded, %u outstanding",
            reason, a->number,
            static_cast<unsigned long long>(a->queued.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(a->synchronous.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(a->by_worker.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(a->by_consumer.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(waits), static_cast<unsigned long long>(wait_us),
            static_cast<unsigned long long>(longest_us),
            static_cast<unsigned long long>(a->failures.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(a->aborted.load(std::memory_order_relaxed)), stranded,
            a->outstanding.load(std::memory_order_relaxed));
}

// Work that moves a counter: a create queued, a compile finished, a consumer that waited, a job ended without a
// create. Not a bind of a pipeline whose compile is long done - a game in its steady state binds pipelines
// thousands of times a second, and none of that is pipeline compilation, so none of it may keep a line coming.
void bump(PsoAsync* a) noexcept { a->activity.fetch_add(1, std::memory_order_relaxed); }

// The periodic line, called on a DDI thread that passes anyway: a create, a consumer or a destroy. Two relaxed
// loads when there is nothing to do. A device that is idle writes nothing, because the activity word is the same
// as at the last line; a device that works writes one line a period, whichever thread moves the stamp first.
void pso_tick(PsoAsync* a) noexcept {
    const uint32_t period = a->policy.summary_period_ms;
    if (!period) return;
    const uint64_t activity = a->activity.load(std::memory_order_relaxed);
    if (activity == a->summary_activity.load(std::memory_order_relaxed)) return;
    uint64_t last = a->summary_ms.load(std::memory_order_relaxed);
    const uint64_t now = g_clock();
    if (now - last < period) return;
    if (!a->summary_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    a->summary_activity.store(activity, std::memory_order_relaxed);
    a->periodic.fetch_add(1, std::memory_order_relaxed);
    write_summary(a, "periodic", 0);
}

// ---- The pool ----------------------------------------------------------------------------------------------------
// One per process (engine-ddi.h: a game has one device and the cores are the process's). Constant-initialized, so
// it needs no dynamic initialization and no guard. The queue is a FIFO: a job is linked while it is Queued, and a
// consumer that needs a queued job unlinks it under this lock and runs it itself, so nothing waits behind a long
// queue. generation and its wake are the only memory a runner touches after it has published Done (P2, P3).
#pragma warning(push)
#pragma warning(disable : 4324)                 // "structure was padded": the point of the alignas below
struct Pool {
    SRWLOCK lock;
    CONDITION_VARIABLE work;
    PsoJob* head;                               // oldest
    PsoJob* tail;
    HANDLE threads[kMaxPsoWorkers];
    uint32_t count;                             // threads started
    uint32_t devices;                           // contexts with the policy on
    // Those same contexts as a list, so that the detach summary can read every device's counters without a lock it
    // could wait on. Linked at pso_on, unlinked at pso_off, both under the lock below.
    PsoAsync* devices_head;
    uint32_t device_seq;                        // numbers for the log lines, never reused
    bool stop;
    uint64_t queued;                            // jobs linked since the pool started (diagnostic)
    // On a line of its own: every consumer of every device waits on this word, and the pool's lock and queue are
    // written by the thread that wakes them.
    alignas(64) std::atomic<uint32_t> generation;
};
#pragma warning(pop)
Pool g_pool{SRWLOCK_INIT, CONDITION_VARIABLE_INIT};

// The wake of every consumer: one word of the pool, never freed. Bumped after a Done store.
void pool_wake() noexcept {
    g_pool.generation.fetch_add(1, std::memory_order_release);
    WakeByAddressAll(&g_pool.generation);
}

void link_locked(PsoJob* job) noexcept {
    job->prev = g_pool.tail;
    job->next = nullptr;
    if (g_pool.tail) g_pool.tail->next = job;
    else g_pool.head = job;
    g_pool.tail = job;
    ++g_pool.queued;
}

void unlink_locked(PsoJob* job) noexcept {
    if (job->prev) job->prev->next = job->next;
    else g_pool.head = job->next;
    if (job->next) job->next->prev = job->prev;
    else g_pool.tail = job->prev;
    job->prev = job->next = nullptr;
}

// A job that is linked is Queued and nothing else owns it, so the state store needs no compare: linked and claimed
// are the same decision, made under the pool's lock (P1).
bool claim_locked(PsoJob* job) noexcept {
    const bool linked = g_pool.head == job || job->prev || job->next;
    if (!linked) return false;
    unlink_locked(job);
    job->state.store(static_cast<uint32_t>(PsoState::Running), std::memory_order_relaxed);
    return true;
}

bool claim(PsoJob* job) noexcept {
    AcquireSRWLockExclusive(&g_pool.lock);
    const bool got = claim_locked(job);
    ReleaseSRWLockExclusive(&g_pool.lock);
    return got;
}

// The runner's last act: the result, then Done, then the wake. Nothing of the job or the context after this (P2).
void publish(PsoJob* job, ID3D12PipelineState* pso, HRESULT hr) noexcept {
    job->pso = pso;
    job->hr = hr;
    job->state.store(static_cast<uint32_t>(PsoState::Done), std::memory_order_release);
    pool_wake();
}

// Runs one claimed job. The counters and the context are touched here, before the publish, which P4 makes safe.
void run_claimed(PsoJob* job) noexcept {
    DeviceContext* c = job->context;
    PsoAsync* a = c->pso;
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = pso_create_now(c, job->plan, &pso);
    if (FAILED(hr)) a->failures.fetch_add(1, std::memory_order_relaxed);
    bump(a);                                    // a compile finished: the next DDI thread may write the line
    a->outstanding.fetch_sub(1, std::memory_order_relaxed);
    publish(job, pso, hr);
}

void APIENTRY worker_body(void* job) noexcept {
    run_claimed(static_cast<PsoJob*>(job));
}

// A claimed job on a pool thread: the shell enters the scope a pipeline create needs and calls back into
// worker_body. The counters and the policy are read before the publish, which P4 makes safe.
void run_with_hook(PsoJob* job) noexcept {
    PsoAsync* a = job->context->pso;
    a->by_worker.fetch_add(1, std::memory_order_relaxed);
    const PsoPolicy policy = a->policy;         // a copy: nothing of the context after the publish (P2)
    policy.run(policy.shell, worker_body, job);
    // A run hook that returns without calling the body would leave the job Running for ever, and every consumer of
    // it waiting. It cannot happen with our shell; it must not hang a game if it ever does.
    if (job->state.load(std::memory_order_acquire) != static_cast<uint32_t>(PsoState::Done)) {
        log_refusal("pso: the run hook returned without compiling job %llu",
                    static_cast<unsigned long long>(job->id));
        publish(job, nullptr, E_UNEXPECTED);
    }
}

// The oldest queued job, claimed, or null when the pool stops. Under the pool's lock, which the sleep releases.
PsoJob* take_job() noexcept {
    AcquireSRWLockExclusive(&g_pool.lock);
    for (;;) {
        if (PsoJob* job = g_pool.head) {
            unlink_locked(job);
            job->state.store(static_cast<uint32_t>(PsoState::Running), std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&g_pool.lock);
            return job;
        }
        if (g_pool.stop) break;
        SleepConditionVariableSRW(&g_pool.work, &g_pool.lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&g_pool.lock);
    return nullptr;
}

DWORD WINAPI pool_thread(void*) noexcept {
    while (PsoJob* job = take_job()) run_with_hook(job);
    return 0;
}

uint32_t default_workers() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uint32_t processors = info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
    const uint32_t workers = processors > 1 ? processors - 1 : 1;
    return workers > kMaxPsoWorkers ? kMaxPsoWorkers : workers;
}

// The pool's threads, started by the first device that joins. Under the pool's lock. A pool that could not start a
// single thread is no pool: the caller then keeps the synchronous path.
bool pool_join_locked(uint32_t workers) noexcept {
    if (g_pool.devices) {
        ++g_pool.devices;
        return g_pool.count != 0;
    }
    g_pool.stop = false;
    for (uint32_t i = 0; i < workers; ++i) {
        HANDLE thread = CreateThread(nullptr, 0, pool_thread, nullptr, 0, nullptr);
        if (!thread) break;
        SetThreadDescription(thread, L"amdgpu_wddm pso compile");
        g_pool.threads[g_pool.count++] = thread;
    }
    if (!g_pool.count) return false;
    ++g_pool.devices;
    return true;
}

// The last device to leave stops the threads and joins them. Never called from DllMain (P5).
void pool_leave() noexcept {
    HANDLE threads[kMaxPsoWorkers];
    uint32_t count = 0;
    AcquireSRWLockExclusive(&g_pool.lock);
    if (g_pool.devices && --g_pool.devices == 0) {
        g_pool.stop = true;
        count = g_pool.count;
        for (uint32_t i = 0; i < count; ++i) threads[i] = g_pool.threads[i];
        g_pool.count = 0;
        WakeAllConditionVariable(&g_pool.work);
    }
    ReleaseSRWLockExclusive(&g_pool.lock);
    for (uint32_t i = 0; i < count; ++i) {
        // A thread that does not come back in a second gets a line, then another second, as the replay teardown
        // does: a worker inside a compile may legitimately take that long.
        for (uint32_t s = 1; WaitForSingleObject(threads[i], 1000) == WAIT_TIMEOUT; ++s)
            log_line("pso: waiting for pool thread %u to stop (%u s)", i, s);
        CloseHandle(threads[i]);
    }
}

} // namespace

// ---- The create ---------------------------------------------------------------------------------------------------
HRESULT pso_create_now(DeviceContext* context, PsoPlan& plan, ID3D12PipelineState** out) noexcept {
    if (plan.compute)
        return context->device->CreateComputePipelineState(&plan.compute_desc, __uuidof(ID3D12PipelineState),
                                                           reinterpret_cast<void**>(out));
    if (!context->device4) return E_NOTIMPL;                // refused by the plan already; never reached
    const D3D12_PIPELINE_STATE_STREAM_DESC desc = plan.stream.desc();
    return context->device4->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                 reinterpret_cast<void**>(out));
}

// ---- Jobs --------------------------------------------------------------------------------------------------------
PsoJob* pso_submit(DeviceContext* context, PsoPlan&& plan) noexcept {
    PsoAsync* a = context->pso;
    if (!a) return nullptr;
    PsoJob* job = make_new<PsoJob>();
    if (!job) return nullptr;
    job->context = context;
    job->plan = std::move(plan);                // vector moves steal, so this allocates nothing and cannot throw
    job->id = context->next_id.fetch_add(1, std::memory_order_relaxed);
    job->queued_qpc = qpc();
    if (job->plan.root) {
        job->root_ref = job->plan.root;
        job->root_ref->AddRef();
    }
    a->outstanding.fetch_add(1, std::memory_order_relaxed);
    a->queued.fetch_add(1, std::memory_order_relaxed);
    bump(a);
    AcquireSRWLockExclusive(&g_pool.lock);
    link_locked(job);
    ReleaseSRWLockExclusive(&g_pool.lock);
    WakeConditionVariable(&g_pool.work);
    pso_tick(a);                                // the periodic counter line, after the job is on its way
    return job;
}

namespace {
// Waits for a job that another thread is running. On the pool's generation word, with a bound, so that a wake lost
// between the Done store and the bump is a latency and never a hang (P3).
void wait_for_done(PsoJob* job, PsoAsync* a) noexcept {
    const uint64_t start = qpc();
    a->waits.fetch_add(1, std::memory_order_relaxed);
    bump(a);                                    // a wait moves the counters: the next line has something to say
    uint32_t seen = g_pool.generation.load(std::memory_order_acquire);
    while (job->state.load(std::memory_order_acquire) != static_cast<uint32_t>(PsoState::Done)) {
        WaitOnAddress(&g_pool.generation, &seen, sizeof(seen), 20);
        seen = g_pool.generation.load(std::memory_order_acquire);
    }
    const uint64_t ticks = qpc() - start;
    a->wait_qpc.fetch_add(ticks, std::memory_order_relaxed);
    uint64_t longest = a->longest_wait_qpc.load(std::memory_order_relaxed);
    while (ticks > longest && !a->longest_wait_qpc.compare_exchange_weak(longest, ticks, std::memory_order_relaxed)) {
    }
}
} // namespace

ID3D12PipelineState* pso_resolve(PsoJob* job, HRESULT* hr) noexcept {
    // The counters are read inside the branch: a job that is not Done means its device's policy is still on,
    // because pso_off ends every job of its device before it clears this (P4). A null here is a corrupted record
    // and not a state the code can reach; it ends as "no pipeline", which the result below turns into a failure.
    if (job->state.load(std::memory_order_acquire) != static_cast<uint32_t>(PsoState::Done)) {
        PsoAsync* a = job->context->pso;
        if (!a) {
            log_refusal("pso: job %llu has no policy to finish it", static_cast<unsigned long long>(job->id));
        } else if (claim(job)) {
            a->by_consumer.fetch_add(1, std::memory_order_relaxed);
            run_claimed(job);
        } else {
            wait_for_done(job, a);
        }
    }
    // A pool thread cannot call the runtime's error callback. This is a DDI thread with no lock held, which is
    // where the shell reports what the thread that compiled could not (the replay drain hook's contract). Outside
    // the branch above: a compile that failed while no consumer was waiting is reported by the first consumer that
    // arrives, even though it finds the job already done.
    if (PsoAsync* a = job->context->pso; a && a->policy.drained) a->policy.drained(a->policy.shell);
    // The periodic line is written by whichever DDI thread arrives first after the period, and only when a counter
    // moved since the last one (pso_tick). A bind of a pipeline that is already compiled moves nothing and prints
    // nothing, which is what keeps an idle device quiet.
    if (PsoAsync* a = job->context->pso; a) pso_tick(a);
    // A job that ends with no pipeline and no failure is one that pso_off ended without a create, after its device
    // was already going away. A consumer must never read that as "bind no pipeline": it is a failure here.
    if (hr) *hr = SUCCEEDED(job->hr) && !job->pso ? E_UNEXPECTED : job->hr;
    return job->pso;
}

void pso_destroy(PsoJob* job) noexcept {
    if (job->state.load(std::memory_order_acquire) != static_cast<uint32_t>(PsoState::Done)) {
        PsoAsync* a = job->context->pso;         // still on, for the reason pso_resolve states
        if (!a) {
            log_refusal("pso: job %llu is destroyed with no policy to end it", static_cast<unsigned long long>(job->id));
            publish(job, nullptr, E_UNEXPECTED);
        } else if (claim(job)) {
            // Nothing will ever use this pipeline: the create is not worth making. The counter and the publish
            // keep the job's bookkeeping identical to a compiled one.
            a->aborted.fetch_add(1, std::memory_order_relaxed);
            bump(a);
            a->outstanding.fetch_sub(1, std::memory_order_relaxed);
            publish(job, nullptr, S_OK);
        } else {
            wait_for_done(job, a);
        }
    }
    // The destroy is a consumer like any other as far as a removal a pool thread saw is concerned (pso_resolve).
    if (PsoAsync* a = job->context->pso; a && a->policy.drained) a->policy.drained(a->policy.shell);
    if (PsoAsync* a = job->context->pso; a) pso_tick(a);
    // Past this point the job is Done and nobody else owns it (P1).
    if (job->pso) job->pso->Release();
    if (job->root_ref) job->root_ref->Release();
    delete job;
}

// ---- The policy --------------------------------------------------------------------------------------------------
HRESULT pso_on(DeviceContext* context, const PsoPolicy* policy) noexcept {
    auto* a = make_new<PsoAsync>();
    if (!a) return E_OUTOFMEMORY;
    a->policy = *policy;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    a->qpf = frequency.QuadPart ? static_cast<uint64_t>(frequency.QuadPart) : 1;
    const uint32_t workers = policy->workers ? policy->workers : default_workers();
    a->summary_ms.store(g_clock(), std::memory_order_relaxed);  // the first periodic line one period from here
    AcquireSRWLockExclusive(&g_pool.lock);
    const bool joined = pool_join_locked(workers);
    const uint32_t count = g_pool.count;
    if (joined) {
        a->number = ++g_pool.device_seq;
        a->next_device = g_pool.devices_head;
        g_pool.devices_head = a;
    }
    ReleaseSRWLockExclusive(&g_pool.lock);
    if (!joined) {
        delete a;
        log_refusal("set_pso_policy: no compile thread could be started; pipeline creation stays synchronous");
        return E_FAIL;
    }
    context->pso = a;
    pso_log(a, "pso policy: asynchronous pipeline compilation on, %u pool thread%s, device %u, summary every %u ms",
            count, count == 1 ? "" : "s", a->number, a->policy.summary_period_ms);
    return S_OK;
}

void pso_off(DeviceContext* context) noexcept {
    PsoAsync* a = context->pso;
    if (!a) return;
    // Every pipeline record of this device is destroyed before this point (destroy_device_context counts them), so
    // no job of it can be left. If one is, it is ended here without a create rather than left for a pool thread
    // that would read a freed context, and the line says so.
    uint32_t stranded = 0;
    for (;;) {
        PsoJob* job = nullptr;
        AcquireSRWLockExclusive(&g_pool.lock);
        for (PsoJob* j = g_pool.head; j; j = j->next) {
            if (j->context != context) continue;
            claim_locked(j);
            job = j;
            break;
        }
        ReleaseSRWLockExclusive(&g_pool.lock);
        if (!job) break;
        ++stranded;
        a->aborted.fetch_add(1, std::memory_order_relaxed);
        a->outstanding.fetch_sub(1, std::memory_order_relaxed);
        publish(job, nullptr, S_OK);            // its record, if any, reports no pipeline rather than reading free memory
    }
    // A compile of this device still in flight: a job a worker claimed before the loop above could strand it, or
    // one whose record the runtime left alive. Its runner writes the counters and reads the policy out of the
    // PsoAsync this function frees, and it does so before its publish (P2), so outstanding reaching zero is the
    // point where freeing is safe. Every runner finishes: an engine create returns, and no runner waits on a DDI
    // thread. The summary below is therefore the final count as well.
    for (uint32_t spins = 0; uint32_t left = a->outstanding.load(std::memory_order_acquire); ++spins) {
        if (spins && spins % 50 == 0) log_line("pso: waiting for %u compile(s) of this device to finish", left);
        uint32_t seen = g_pool.generation.load(std::memory_order_acquire);
        WaitOnAddress(&g_pool.generation, &seen, sizeof(seen), 20);
    }
    write_summary(a, "the device's destruction", stranded);
    // Out of the pool's device list before the free, so that a detach summary can never read it afterwards.
    AcquireSRWLockExclusive(&g_pool.lock);
    for (PsoAsync** at = &g_pool.devices_head; *at; at = &(*at)->next_device)
        if (*at == a) {
            *at = a->next_device;
            break;
        }
    ReleaseSRWLockExclusive(&g_pool.lock);
    context->pso = nullptr;
    delete a;
    pool_leave();
}

uint32_t pso_detach_summary() noexcept {
    // Best effort from the shell's DllMain (engine-ddi.h): a shared lock that is not free right now means another
    // thread is inside the pool, and at detach that thread may never come out, so the lines are dropped instead.
    if (!TryAcquireSRWLockShared(&g_pool.lock)) return 0;
    uint32_t written = 0;
    for (PsoAsync* a = g_pool.devices_head; a; a = a->next_device) {
        write_summary(a, "process detach", 0);
        ++written;
    }
    ReleaseSRWLockShared(&g_pool.lock);
    return written;
}

uint32_t pso_pool_workers() noexcept {
    AcquireSRWLockShared(&g_pool.lock);
    const uint32_t count = g_pool.count;
    ReleaseSRWLockShared(&g_pool.lock);
    return count;
}

void pso_set_clock(PsoClock clock) noexcept { g_clock = clock ? clock : tick_ms; }

} // namespace engine_ddi
