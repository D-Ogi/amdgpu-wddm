// SPDX-License-Identifier: MIT
// engine-ddi: device context, resolver, table filling with fail-safes and the release sequence.
#include "internal.h"
#include "replay.h"
#include <cstdarg>
#include <cstdio>
#include <tuple>
#include <type_traits>

namespace engine_ddi {

namespace {
// AMDGPU_WDDM_DDI_TRACE=2 is the shell's failures-only debugger mode (ddi-trace.h, same parse). The lab's game runs
// capture the debugger's log and not stderr (game-runtime.ps1), so without this copy the lines below (a release
// that cannot prove retirement, a release past its bound, a queue destroyed unretired, a fence anomaly) never reach
// any record of a game run. Bounded like the shell's failure notes; the stderr line is unchanged.
bool debugger_lines() noexcept {
    static const bool on = []() noexcept {
        char value[2]{};
        return GetEnvironmentVariableA("AMDGPU_WDDM_DDI_TRACE", value, sizeof(value)) == 1 && value[0] == '2';
    }();
    return on;
}
std::atomic<int> debugger_budget{1024};
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
std::atomic<LogObserver> log_observer{nullptr};
std::atomic<void*> log_observer_user{nullptr};
#endif
} // namespace

void log_line(const char* format, ...) noexcept {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "engine-ddi: %s\n", text);
    if (!debugger_lines() || debugger_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    char line[600];
    std::snprintf(line, sizeof(line), "amdgpu_wddm_d3d12 engine-ddi: %s qpc=%lld thread=%lu\n", text,
                  static_cast<long long>(now.QuadPart), static_cast<unsigned long>(GetCurrentThreadId()));
    OutputDebugStringA(line);
}

void log_refusal(const char* format, ...) noexcept {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "engine-ddi: %s\n", text);
    char line[540];
    std::snprintf(line, sizeof(line), "amdgpu_wddm_d3d12 engine-ddi: %s\n", text);
    OutputDebugStringA(line);
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (const LogObserver observer = log_observer.load()) observer(text, log_observer_user.load());
#endif
}

// ---- Release sequence ------------------------------------------------------------------------------------------
void ReleaseQueue::add(PendingRelease* node) noexcept {
    node->next = head_;
    head_ = node;
    ++count_;
}

void ReleaseQueue::queue_destroyed(uint32_t slot, uint64_t final_completed) noexcept {
    const uint64_t bit = uint64_t{1} << slot;
    for (PendingRelease* n = head_; n; n = n->next) {
        if (!(n->mask & bit)) continue;
        if (final_completed == kFenceRemoved || final_completed < n->marks[slot]) n->stuck = true;
        n->mask &= ~bit;
        n->marks[slot] = 0;
    }
}

size_t ReleaseQueue::stuck() const noexcept {
    size_t count = 0;
    for (const PendingRelease* n = head_; n; n = n->next) count += n->stuck ? 1 : 0;
    return count;
}

uint64_t completed_value(EngineQueue* q) noexcept {
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (const uint64_t forced = q->harness_completed.load()) return forced;
#endif
    return q->fence->GetCompletedValue();
}

// completed_value() with the two invariants every release decision rests on checked on the way (fence-lifetime
// review): a retirement fence never goes back, and it never passes what its queue signalled. submit_locked sets bit 0
// of state before the Signal of value v+1 is issued, so a state word read after the fence covers any value the fence
// can hold: (state >> 1) + (state & 1). A violation is logged (and reaches the debugger in trace mode 2), never acted
// on: the release logic stays as it is, so the check changes no decision and no timing apart from its loads.
uint64_t observed_value(EngineQueue* q) noexcept {
    const uint64_t done = completed_value(q);
    if (done == kFenceRemoved) return done;
    const uint64_t state = q->state.load();
    const uint64_t signalled = (state >> 1) + (state & 1);
    uint64_t seen = q->last_done.load(std::memory_order_relaxed);
    if (done < seen)
        log_line("invariant: retirement fence of queue %llu went back from %llu to %llu",
                 static_cast<unsigned long long>(q->id), static_cast<unsigned long long>(seen),
                 static_cast<unsigned long long>(done));
    while (done > seen && !q->last_done.compare_exchange_weak(seen, done, std::memory_order_relaxed)) {
    }
    if (done > signalled)
        log_line("invariant: retirement fence of queue %llu reads %llu beyond its last signal %llu (state %llx)",
                 static_cast<unsigned long long>(q->id), static_cast<unsigned long long>(done),
                 static_cast<unsigned long long>(signalled), static_cast<unsigned long long>(state));
    return done;
}

HRESULT run_release(const ShellHooks& hooks, const ReleasePayload& payload) noexcept {
    for (IUnknown* object : payload.objects)
        if (object) object->Release();
    if (!payload.has_memory) return S_OK;
    // Exactly once, never retried: the shell keeps its record on failure.
    HRESULT hr = hooks.free_memory ? hooks.free_memory(hooks.shell, &payload.memory) : E_UNEXPECTED;
    if (FAILED(hr) && hooks.report_device_error) hooks.report_device_error(hooks.shell, hr);
    return hr;
}

void DeviceContext::notify(const ReleasePayload& payload, bool deferred, HRESULT free_result) const noexcept {
    if (!observer) return;
    ReleaseEvent event{payload.id, GetCurrentThreadId(), payload.has_memory, deferred, free_result};
    observer(observer_user, &event);
}

// The snapshot. It covers every ExecuteCommandLists call that returned before this call began (the application
// orders a destroy after the submissions that use the memory): each such call either stored its signal value in
// the queue's state word, or left bit 0 set because its signal failed, in which case only the queue's next
// successful signal covers it. Queues are added and removed only under the lock held here, and a removed queue's
// marks are resolved under the same lock (destroy_engine_queue), so no queue is missed or counted twice. Engine
// calls (GetCompletedValue) happen under the lock; hooks never do.
// The caller holds lock. It writes mask, marks and stuck, and leaves the node's phase alone.
uint32_t DeviceContext::snapshot_marks(PendingRelease* node) noexcept {
    node->mask = 0;
    for (uint64_t& mark : node->marks) mark = 0;
    node->stuck = retirement_lost;
    uint32_t marked = 0;
    for (uint32_t s = 0; s < kMaxEngineQueues; ++s) {
        if (!((queue_mask >> s) & 1)) continue;
        EngineQueue* q = queues[s];
        const uint64_t state = q->state.load();
        const uint64_t mark = (state >> 1) + (state & 1);   // unsignalled work: the next signal must retire it
        if (!mark) continue;                                // nothing was ever submitted
        const uint64_t done = observed_value(q);
        if (done == kFenceRemoved) {
            node->stuck = true;                             // a removed device proves nothing
            continue;
        }
        if (done >= mark) continue;
        node->marks[s] = mark;
        node->mask |= uint64_t{1} << s;
        ++marked;
    }
    return marked;
}

namespace {
bool power_of_two(uint64_t n) noexcept { return n && !(n & (n - 1)); }
uint64_t now_qpc() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}
} // namespace

void DeviceContext::release(PendingRelease* node) noexcept {
    node->next = nullptr;
    bool recorded = false;
    bool late = false;
    const ULONGLONG began = GetTickCount64();
    // The two-phase gate (set_release_policy) takes the destroy's snapshot as phase 1 and records the node
    // even with no mark, so that the second phase covers a submission that races this destroy. The memory
    // of a linear primary keeps its single bounded phase inside this DDI.
    const bool two_phase = release_two_phase && node->payload.has_memory && !node->payload.in_ddi;
again:
    AcquireSRWLockExclusive(&lock);
    const uint32_t marked = snapshot_marks(node);
    const bool stuck = node->stuck;
    const uint64_t id = node->payload.id;
    if (node->payload.in_ddi && node->payload.has_memory && !stuck && node->mask) {
        if (GetTickCount64() - began < in_ddi_bound_ms) {
            ReleaseSRWLockExclusive(&lock);             // never wait with the lock held
            Sleep(1);
            goto again;
        }
        late = true;
    }
    uint32_t phase = 0;
    uint64_t destroy_qpc = 0;
    if (stuck || node->mask || (two_phase && !stuck)) {
        if (two_phase && !stuck) {
            node->phase = 1;
            node->queues[0] = marked;
            node->snapshot_qpc[0] = now_qpc();
        }
        phase = node->phase;
        destroy_qpc = node->snapshot_qpc[0];
        releases.add(node);                                 // from here on another thread may retire the node
        live.fetch_add(1);
        pending.fetch_add(1);
        recorded = true;
    }
    ReleaseSRWLockExclusive(&lock);
    if (stuck)
        log_line("release %llu: retirement cannot be proven (removed device or lost queue); memory stays owned",
                 static_cast<unsigned long long>(id));
    if (late) {
        log_line("release %llu: the work before this destroy did not retire in %u ms; the memory's release "
                 "leaves its DDI",
                 static_cast<unsigned long long>(id), in_ddi_bound_ms);
        report(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    if (recorded) {
        // Instrumentation item 5 of the trial 245 report: the release's identity and the first phase's
        // marks, at powers of two of the recorded count, so a game adds a line per doubling and no more.
        const uint64_t count = releases_recorded.fetch_add(1) + 1;
        if (power_of_two(count))
            log_line("release %llu recorded (%llu so far): phase %u, %u queues marked, destroy qpc %llu",
                     static_cast<unsigned long long>(id), static_cast<unsigned long long>(count), phase, marked,
                     static_cast<unsigned long long>(destroy_qpc));
        return;
    }
    const ReleasePayload payload = node->payload;
    delete node;
    HRESULT hr = run_release(hooks, payload);
    releases_run.fetch_add(1);
    notify(payload, false, hr);
}

void DeviceContext::process_retired() noexcept {
    if (!pending.load()) return;
    AcquireSRWLockExclusive(&lock);
    PendingRelease* ready = releases.take_retired([this](uint32_t slot) {
        return ((queue_mask >> slot) & 1) ? observed_value(queues[slot]) : kFenceRemoved;
    });
    // Second phase: a node whose first-phase marks are reached records every queue's current mark once more
    // and goes back into the queue; only a node past its second phase runs now. A node the policy never
    // took (phase 0) is unchanged.
    PendingRelease* run = nullptr;
    // What the last held node would log, copied out: once the lock is released another thread may retire
    // and delete it.
    bool any_held = false;
    uint64_t held_id = 0, held_first_qpc = 0, held_second_qpc = 0;
    uint32_t held_marks = 0;
    while (PendingRelease* n = ready) {
        ready = n->next;
        if (n->phase == 1 && !n->stuck) {
            n->phase = 2;
            n->queues[1] = snapshot_marks(n);
            n->snapshot_qpc[1] = now_qpc();
            if (n->mask || n->stuck) {
                any_held = true;
                held_id = n->payload.id;
                held_marks = n->queues[1];
                held_first_qpc = n->snapshot_qpc[0];
                held_second_qpc = n->snapshot_qpc[1];
                releases.add(n);                            // still waiting: the second phase holds it
                continue;
            }
        }
        n->next = run;
        run = n;
    }
    ReleaseSRWLockExclusive(&lock);
    if (any_held) {
        const uint64_t count = releases_second.fetch_add(1) + 1;
        if (power_of_two(count))
            log_line("release %llu second phase (%llu so far): %u queues marked, phase 1 qpc %llu, phase 2 qpc %llu",
                     static_cast<unsigned long long>(held_id), static_cast<unsigned long long>(count), held_marks,
                     static_cast<unsigned long long>(held_first_qpc),
                     static_cast<unsigned long long>(held_second_qpc));
    }
    while (PendingRelease* n = run) {
        run = n->next;
        const ReleasePayload payload = n->payload;
        const uint32_t phase = n->phase;
        const uint32_t first = n->queues[0];
        const uint32_t second = n->queues[1];
        const uint64_t destroy_qpc = n->snapshot_qpc[0];
        delete n;
        HRESULT hr = run_release(hooks, payload);
        pending.fetch_sub(1);
        live.fetch_sub(1);
        const uint64_t count = releases_run.fetch_add(1) + 1;
        if (power_of_two(count))
            log_line("release %llu run (%llu so far): phase %u, marks %u/%u, destroy qpc %llu, release qpc %llu, "
                     "status %08lx",
                     static_cast<unsigned long long>(payload.id), static_cast<unsigned long long>(count), phase,
                     first, second, static_cast<unsigned long long>(destroy_qpc),
                     static_cast<unsigned long long>(now_qpc()), static_cast<unsigned long>(hr));
        notify(payload, true, hr);
    }
}

void DeviceContext::retire_after_submit() noexcept {
    const uint32_t waiting = pending.load();
    if (!waiting) return;
    if (retire_handoff && retire_defers(true, waiting, retire_backlog_bound, GetTickCount64(),
                                        retire_resource_tick.load(std::memory_order_relaxed), retire_age_bound_ms)) {
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
        retire_deferred.fetch_add(1);
#endif
        return;
    }
    process_retired();
}

void DeviceContext::retire_at_resource() noexcept {
    if (retire_handoff) {
        const uint64_t now = GetTickCount64();
        if (retire_resource_tick.load(std::memory_order_relaxed) != now)
            retire_resource_tick.store(now, std::memory_order_relaxed);
    }
    process_retired();
}

HRESULT set_retire_policy(DeviceContext* context, const RetirePolicy* policy) noexcept {
    if (!context || !policy || policy->size != sizeof(RetirePolicy) || policy->handoff > 1 ||
        (policy->handoff &&
         (!policy->backlog_bound || !policy->age_bound_ms || policy->age_bound_ms > 10000))) {
        log_refusal("set_retire_policy: refused (%s)", !context ? "no context" : !policy ? "no policy" :
                    policy->size != sizeof(RetirePolicy) ? "size" : policy->handoff > 1 ? "handoff" : "bounds");
        return E_INVALIDARG;
    }
    context->retire_handoff = policy->handoff != 0;
    context->retire_backlog_bound = policy->handoff ? policy->backlog_bound : 0;
    context->retire_age_bound_ms = policy->handoff ? policy->age_bound_ms : 0;
    log_line("retire policy: handoff %u, backlog bound %u, age bound %u ms", policy->handoff,
             context->retire_backlog_bound, context->retire_age_bound_ms);
    return S_OK;
}

HRESULT set_release_policy(DeviceContext* context, const ReleasePolicy* policy) noexcept {
    if (!context || !policy || policy->size != sizeof(ReleasePolicy) || policy->two_phase > 1) {
        log_refusal("set_release_policy: refused (%s)", !context ? "no context" : !policy ? "no policy" :
                    policy->size != sizeof(ReleasePolicy) ? "size" : "two_phase");
        return E_INVALIDARG;
    }
    context->release_two_phase = policy->two_phase != 0;
    log_line("release policy: two-phase retirement %u", policy->two_phase);
    return S_OK;
}

// ---- Context --------------------------------------------------------------------------------------------------
namespace {
template <class T> void release_ref(T*& p) noexcept {
    if (p) p->Release();
    p = nullptr;
}

void destroy(DeviceContext* c) noexcept {
    replay_off(c);                              // the workers first: their pending calls name the engine device
    release_initialization(c);
    release_ref(c->empty_local);
    release_ref(c->device10);
    release_ref(c->device8);
    release_ref(c->device7);
    release_ref(c->device5);
    release_ref(c->device4);
    release_ref(c->device);
    delete c;
}

bool hooks_valid(const ShellHooks& h, MemoryMode mode) noexcept {
    if (h.size != sizeof(ShellHooks) || !h.report_device_error || !h.report_list_error || !h.is_device_lost ||
        !h.bind_list_table)
        return false;
    if (mode == MemoryMode::RuntimeBacked) return h.allocate_memory && h.free_memory;
    return !h.allocate_memory && !h.free_memory;
}
} // namespace

HRESULT create_device_context(const ContextCreateInfo* info, DeviceContext** out) noexcept {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!info || info->size != sizeof(ContextCreateInfo) || info->boundary_revision != kBoundaryRevision ||
        !info->engine_device || !info->engine_funcs || info->engine_funcs->Size < sizeof(BC250_VKD3D_ENGINE_FUNCS) ||
        info->engine_funcs->AbiVersion < 0x00010001u || !info->engine_funcs->CreateCommandQueue)
        return E_INVALIDARG;
    if (info->memory_mode != MemoryMode::RuntimeBacked && info->memory_mode != MemoryMode::EnginePrivateTest)
        return E_INVALIDARG;
#ifndef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (info->memory_mode == MemoryMode::EnginePrivateTest) return E_INVALIDARG;
#endif
    // Engine ABI 1.2 V10: MapHeap and UnmapHeap in both modes, CreateHeapFromMemory for runtime memory.
    // 1.3 V13: the linear image entries, which only runtime memory uses.
    const BC250_VKD3D_ENGINE_FUNCS& f = *info->engine_funcs;
    if (f.AbiVersion < BC250_VKD3D_ENGINE_ABI_VERSION || !f.MapHeap || !f.UnmapHeap ||
        (info->memory_mode == MemoryMode::RuntimeBacked &&
         (!f.CreateHeapFromMemory || !f.QueryLinearImage || !f.CreateLinearPlacedResource)))
        return E_INVALIDARG;
    if (!hooks_valid(info->hooks, info->memory_mode)) return E_INVALIDARG;

    auto* c = make_new<DeviceContext>();
    if (!c) return E_OUTOFMEMORY;
    c->device = info->engine_device;
    c->device->AddRef();
    HRESULT hr = c->device->QueryInterface(__uuidof(ID3D12Device4), reinterpret_cast<void**>(&c->device4));
    if (SUCCEEDED(hr)) hr = c->device->QueryInterface(__uuidof(ID3D12Device5), reinterpret_cast<void**>(&c->device5));
    if (SUCCEEDED(hr)) hr = c->device->QueryInterface(__uuidof(ID3D12Device8), reinterpret_cast<void**>(&c->device8));
    if (SUCCEEDED(hr)) hr = c->device->QueryInterface(__uuidof(ID3D12Device10), reinterpret_cast<void**>(&c->device10));
    if (FAILED(hr)) {
        destroy(c);
        return E_NOINTERFACE;
    }
    // Optional: only AddToStateObject needs it.
    if (FAILED(c->device->QueryInterface(__uuidof(ID3D12Device7), reinterpret_cast<void**>(&c->device7))))
        c->device7 = nullptr;
    c->mode = info->memory_mode;
    c->hooks = info->hooks;
    c->funcs = *info->engine_funcs;
    c->ddi_interface = info->ddi_interface;
    c->ddi_version = info->ddi_version;
    for (UINT t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++t)
        c->increments[t] = c->device->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(t));
    *out = c;
    return S_OK;
}

HRESULT destroy_device_context(DeviceContext* context, uint32_t* live_objects) noexcept {
    if (live_objects) *live_objects = 0;
    if (!context) return E_INVALIDARG;
    context->process_retired();
    uint32_t live = context->live.load();
    AcquireSRWLockShared(&context->lock);
    for (uint32_t s = 0; s < kMaxEngineQueues; ++s) live += (context->queue_mask >> s) & 1;
    ReleaseSRWLockShared(&context->lock);
    if (live) {
        if (live_objects) *live_objects = live;
        return S_FALSE;
    }
    destroy(context);
    return S_OK;
}

// ---- Resolver ----------------------------------------------------------------------------------------------------
namespace {
std::atomic<ResolveDevice> g_resolve{nullptr};

HRESULT install_resolver(const FillInfo* info) noexcept {
    if (!info || info->size != sizeof(FillInfo) || !info->resolve) return E_INVALIDARG;
    ResolveDevice expected = nullptr;
    if (g_resolve.compare_exchange_strong(expected, info->resolve) || expected == info->resolve) return S_OK;
    return E_INVALIDARG;
}
} // namespace

DeviceContext* resolve(D3D12DDI_HDEVICE device) noexcept {
    ResolveDevice r = g_resolve.load();
    return (r && device.pDrvPrivate) ? r(device) : nullptr;
}

CommandListRecord* list_of(D3D12DDI_HCOMMANDLIST list, const char* slot) noexcept {
    auto* r = record_of<CommandListRecord>(list.pDrvPrivate, Tag::CommandList);
    if (!r && slot) log_line("%s: not a live command list record", slot);
    return r;
}

void* command_list_shell(D3D12DDI_HCOMMANDLIST list) noexcept {
    const auto* r = record_of<CommandListRecord>(list.pDrvPrivate, Tag::CommandList);
    return (r && r->h.device) ? r->h.device->hooks.shell : nullptr;
}

bool reject_in_compute_table(const CommandListRecord* list) noexcept {
    if (list->table != 0) return false;
    list->h.device->report_list(list->rt, E_INVALIDARG);
    return true;
}

// ---- Fail-safes ----------------------------------------------------------------------------------------------------
namespace {
template <class... A> struct FirstOf { using type = void; };
template <class T, class... A> struct FirstOf<T, A...> { using type = T; };

template <class F, char Table, size_t Offset> struct FailSafe;
template <class R, class... A, char Table, size_t Offset> struct FailSafe<R(APIENTRY*)(A...), Table, Offset> {
    using First = typename FirstOf<A...>::type;
    static void note() noexcept {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) log_line("fail-safe slot %c+0x%03zx called", Table, Offset);
    }
    static void report(HRESULT hr, A... args) noexcept {
        if constexpr (std::is_same_v<First, D3D12DDI_HDEVICE>) {
            if (DeviceContext* c = resolve(std::get<0>(std::tuple<A...>(args...)))) c->report(hr);
        } else if constexpr (std::is_same_v<First, D3D12DDI_HCOMMANDLIST>) {
            if (CommandListRecord* l = list_of(std::get<0>(std::tuple<A...>(args...)), nullptr))
                l->h.device->report_list(l->rt, hr);
        } else {
            ((void)args, ...);
            (void)hr;
        }
    }
    // HRESULT slots return E_NOTIMPL; every other slot reports it and returns a zero value.
    static R APIENTRY slot(A... args) noexcept {
        note();
        if constexpr (std::is_same_v<R, HRESULT>) {
            ((void)args, ...);
            return E_NOTIMPL;
        } else {
            report(E_NOTIMPL, args...);
            if constexpr (!std::is_void_v<R>) return R{};
        }
    }
    // A void query slot whose non-const pointer arguments are all _Out_ (checked per slot against the WDK header):
    // each is zeroed, so the caller never reads what was in its memory before, then E_NOTIMPL is reported.
    template <class P> static void zero_out(P p) noexcept {
        if constexpr (std::is_pointer_v<P>) {
            using T = std::remove_pointer_t<P>;
            if constexpr (!std::is_const_v<T> && !std::is_void_v<T>) {
                if (p) *p = T{};
            }
        }
    }
    static R APIENTRY zeroing(A... args) noexcept {
        static_assert(std::is_void_v<R>, "zeroing fail-safes are void query slots");
        note();
        (zero_out(args), ...);
        report(E_NOTIMPL, args...);
    }
    // Graphics-only command-list slot in the compute table.
    static R APIENTRY graphics_only(A... args) noexcept {
        static_assert(std::is_void_v<R>, "command-list slots return void");
        report(E_INVALIDARG, args...);
    }
    // Private-storage size of a fail-safe create: a header's worth, never used.
    static R APIENTRY calc(A... args) noexcept {
        ((void)args, ...);
        note();
        if constexpr (std::is_same_v<R, D3D12DDI_HEAP_AND_RESOURCE_SIZES>) return R{64, 64};
        else return R{64};
    }
};

D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY unrecognized_identifier(
    D3D12DDI_HDEVICE, D3D12DDI_SERIALIZED_DATA_TYPE, const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054*) {
    return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
}

// No meta commands: an empty enumeration is the exact answer, not a failure.
HRESULT APIENTRY enumerate_no_meta_commands(D3D12DDI_HDEVICE, UINT* count, D3D12DDIARG_META_COMMAND_DESC*) {
    if (!count) return E_INVALIDARG;
    *count = 0;
    return S_OK;
}

// engine-ddi reports no driver-managed shader cache (1006 D3D12_OPTIONS DriverManagedShaderCachePresent FALSE,
// d3d12umddi.h D3D12DDI_D3D12_OPTIONS_DATA_0089; INTEGRATION.md), so
// there is no cache of engine-ddi's to disable, enable or clear: the control is accepted as a no-op, never an error.
void APIENTRY no_implicit_shader_cache(D3D12DDI_HDEVICE, D3D12DDI_IMPLICIT_SHADER_CACHE_CONTROL_FLAGS_0080) {}
} // namespace

#define ENGINE_DDI_FS(t, T, m) (t)->m = FailSafe<decltype((t)->m), 'D', offsetof(T, m)>::slot
#define ENGINE_DDI_CALC(t, T, m) (t)->m = FailSafe<decltype((t)->m), 'D', offsetof(T, m)>::calc
#define ENGINE_DDI_ZERO(t, T, m) (t)->m = FailSafe<decltype((t)->m), 'D', offsetof(T, m)>::zeroing

void fill_core_failsafe(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    using T = D3D12DDI_DEVICE_FUNCS_CORE_0088;
#define FS(m) ENGINE_DDI_FS(t, T, m)
#define CALC(m) ENGINE_DDI_CALC(t, T, m)
#define ZERO(m) ENGINE_DDI_ZERO(t, T, m)
    FS(pfnCheckFormatSupport); ZERO(pfnCheckMultisampleQualityLevels); ZERO(pfnGetMipPacking);
    CALC(pfnCalcPrivateElementLayoutSize); FS(pfnCreateElementLayout); FS(pfnDestroyElementLayout);
    CALC(pfnCalcPrivateBlendStateSize); FS(pfnCreateBlendState); FS(pfnDestroyBlendState);
    CALC(pfnCalcPrivateDepthStencilStateSize); FS(pfnCreateDepthStencilState); FS(pfnDestroyDepthStencilState);
    CALC(pfnCalcPrivateRasterizerStateSize); FS(pfnCreateRasterizerState); FS(pfnDestroyRasterizerState);
    CALC(pfnCalcPrivateShaderSize); FS(pfnCreateVertexShader); FS(pfnCreatePixelShader);
    FS(pfnCreateGeometryShader); FS(pfnCreateComputeShader);
    CALC(pfnCalcPrivateGeometryShaderWithStreamOutput); FS(pfnCreateGeometryShaderWithStreamOutput);
    CALC(pfnCalcPrivateTessellationShaderSize); FS(pfnCreateHullShader); FS(pfnCreateDomainShader);
    FS(pfnDestroyShader);
    CALC(pfnCalcPrivateCommandPoolSize); FS(pfnCreateCommandPool); FS(pfnDestroyCommandPool); FS(pfnResetCommandPool);
    CALC(pfnCalcPrivatePipelineStateSize); FS(pfnCreatePipelineState); FS(pfnDestroyPipelineState);
    CALC(pfnCalcPrivateCommandListSize); FS(pfnCreateCommandList); FS(pfnDestroyCommandList);
    CALC(pfnCalcPrivateDescriptorHeapSize); FS(pfnCreateDescriptorHeap); FS(pfnDestroyDescriptorHeap);
    FS(pfnGetDescriptorSizeInBytes); FS(pfnGetCPUDescriptorHandleForHeapStart);
    FS(pfnGetGPUDescriptorHandleForHeapStart);
    FS(pfnCreateShaderResourceView); FS(pfnCreateConstantBufferView); FS(pfnCreateSampler);
    FS(pfnCreateUnorderedAccessView); FS(pfnCreateRenderTargetView); FS(pfnCreateDepthStencilView);
    CALC(pfnCalcPrivateRootSignatureSize); FS(pfnCreateRootSignature); FS(pfnDestroyRootSignature);
    FS(pfnMapHeap); FS(pfnUnmapHeap);
    CALC(pfnCalcPrivateHeapAndResourceSizes); FS(pfnCreateHeapAndResource); FS(pfnDestroyHeapAndResource);
    CALC(pfnCalcPrivateOpenedHeapAndResourceSizes); FS(pfnOpenHeapAndResource);
    FS(pfnCopyDescriptors); FS(pfnCopyDescriptorsSimple);
    CALC(pfnCalcPrivateQueryHeapSize); FS(pfnCreateQueryHeap); FS(pfnDestroyQueryHeap);
    CALC(pfnCalcPrivateCommandSignatureSize); FS(pfnCreateCommandSignature); FS(pfnDestroyCommandSignature);
    FS(pfnCheckResourceVirtualAddress); ZERO(pfnCheckResourceAllocationInfo); ZERO(pfnCheckSubresourceInfo);
    ZERO(pfnCheckExistingResourceAllocationInfo);
    FS(pfnRetrieveShaderComment); FS(pfnCheckResourceAllocationHandle);
    CALC(pfnCalcPrivatePipelineLibrarySize); FS(pfnCreatePipelineLibrary); FS(pfnDestroyPipelineLibrary);
    FS(pfnAddPipelineStateToLibrary); FS(pfnCalcSerializedLibrarySize); FS(pfnSerializeLibrary);
    CALC(pfnCalcPrivateCommandRecorderSize); FS(pfnCreateCommandRecorder); FS(pfnDestroyCommandRecorder);
    FS(pfnCommandRecorderSetCommandPoolAsTarget);
    t->pfnEnumerateMetaCommands = enumerate_no_meta_commands;
    FS(pfnEnumerateMetaCommandParameters); CALC(pfnCalcPrivateMetaCommandSize); FS(pfnCreateMetaCommand);
    FS(pfnDestroyMetaCommand); ZERO(pfnGetMetaCommandRequiredParameterInfo);
    CALC(pfnCalcPrivateStateObjectSize); FS(pfnCreateStateObject); FS(pfnDestroyStateObject);
    ZERO(pfnGetRaytracingAccelerationStructurePrebuildInfo);
    t->pfnCheckDriverMatchingIdentifier = unrecognized_identifier;
    FS(pfnGetShaderIdentifier); FS(pfnGetShaderStackSize); FS(pfnGetPipelineStackSize); FS(pfnSetPipelineStackSize);
    CALC(pfnCalcPrivateAddToStateObjectSize); FS(pfnAddToStateObject);
    FS(pfnCreateSamplerFeedbackUnorderedAccessView); FS(pfnCreateAmplificationShader); FS(pfnCreateMeshShader);
    CALC(pfnCalcPrivateMeshShaderSize);
    t->pfnImplicitShaderCacheControl = no_implicit_shader_cache;
#undef FS
#undef CALC
#undef ZERO
}

void fill_list_failsafe(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t table_index) noexcept {
    using T = D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
    const bool compute = table_index == 0;
#define FS(m) (t)->m = FailSafe<decltype((t)->m), 'L', offsetof(T, m)>::slot
#define GFX(m) (t)->m = compute ? FailSafe<decltype((t)->m), 'L', offsetof(T, m)>::graphics_only \
                                : FailSafe<decltype((t)->m), 'L', offsetof(T, m)>::slot
    FS(pfnCloseCommandList); FS(pfnResetCommandList); GFX(pfnDrawInstanced); GFX(pfnDrawIndexedInstanced);
    FS(pfnDispatch); FS(pfnClearUnorderedAccessViewUint); FS(pfnClearUnorderedAccessViewFloat);
    GFX(pfnClearRenderTargetView); GFX(pfnClearDepthStencilView); FS(pfnDiscardResource); FS(pfnCopyTextureRegion);
    FS(pfnResourceCopy); FS(pfnCopyTiles); FS(pfnCopyBufferRegion); GFX(pfnResourceResolveSubresource);
    GFX(pfnExecuteBundle); FS(pfnExecuteIndirect); FS(pfnResourceBarrier); FS(pfnBlt); FS(pfnPresent);
    FS(pfnBeginQuery); FS(pfnEndQuery); FS(pfnResolveQueryData); FS(pfnSetPredication);
    GFX(pfnIaSetTopology); GFX(pfnRsSetViewports); GFX(pfnRsSetScissorRects); GFX(pfnOmSetBlendFactor);
    GFX(pfnOmSetStencilRef); FS(pfnSetPipelineState); FS(pfnSetDescriptorHeaps);
    FS(pfnSetComputeRootSignature); GFX(pfnSetGraphicsRootSignature);
    FS(pfnSetComputeRootDescriptorTable); GFX(pfnSetGraphicsRootDescriptorTable);
    FS(pfnSetComputeRoot32BitConstant); GFX(pfnSetGraphicsRoot32BitConstant);
    FS(pfnSetComputeRoot32BitConstants); GFX(pfnSetGraphicsRoot32BitConstants);
    FS(pfnSetComputeRootConstantBufferView); GFX(pfnSetGraphicsRootConstantBufferView);
    FS(pfnSetComputeRootShaderResourceView); GFX(pfnSetGraphicsRootShaderResourceView);
    FS(pfnSetComputeRootUnorderedAccessView); GFX(pfnSetGraphicsRootUnorderedAccessView);
    GFX(pfnIASetIndexBuffer); GFX(pfnIASetVertexBuffers); GFX(pfnSOSetTargets); GFX(pfnOMSetRenderTargets);
    FS(pfnSetMarker); FS(pfnClearRootArguments); FS(pfnAtomicCopyBufferRegion); GFX(pfnOMSetDepthBounds);
    GFX(pfnSetSamplePositions); GFX(pfnResourceResolveSubresourceRegion); FS(pfnSetProtectedResourceSession);
    FS(pfnWriteBufferImmediate); GFX(pfnSetViewInstanceMask); FS(pfnInitializeMetaCommand);
    FS(pfnExecuteMetaCommand); FS(pfnBuildRaytracingAccelerationStructure);
    FS(pfnEmitRaytracingAccelerationStructurePostbuildInfo); FS(pfnCopyRaytracingAccelerationStructure);
    FS(pfnSetPipelineState1); FS(pfnDispatchRays); GFX(pfnRSSetShadingRate); GFX(pfnRSSetShadingRateImage);
    GFX(pfnDispatchMesh); FS(pfnBarrier); GFX(pfnOmSetAlphaBlendFactor);
#undef FS
#undef GFX
}

// ---- Table filling -------------------------------------------------------------------------------------------
HRESULT fill_device_core(D3D12DDI_DEVICE_FUNCS_CORE_0088* table, SIZE_T table_size, const FillInfo* info) noexcept {
    if (!table || table_size != sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088)) return E_INVALIDARG;
    HRESULT hr = install_resolver(info);
    if (FAILED(hr)) return hr;
    fill_core_failsafe(table);
    fill_core_resources(table);
    fill_core_descriptors(table);
    fill_core_root_signatures(table);
    fill_core_pipelines(table);
    fill_core_graphics(table);
    fill_core_commands(table);
    fill_core_queries(table);
    fill_core_tiles(table);
    fill_core_state_objects(table);
    return S_OK;
}

HRESULT fill_command_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, SIZE_T table_size, uint32_t table_index,
                          const FillInfo* info) noexcept {
    if (!table || table_size != sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092) || table_index > 1) return E_INVALIDARG;
    HRESULT hr = install_resolver(info);
    if (FAILED(hr)) return hr;
    fill_list_failsafe(table, table_index);
    fill_list_resources(table, table_index);
    fill_list_descriptors(table, table_index);
    fill_list_pipelines(table, table_index);
    fill_list_graphics(table, table_index);
    fill_list_commands(table, table_index);
    fill_list_queries(table, table_index);
    fill_list_tiles(table, table_index);
    return S_OK;
}

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
void harness_set_release_observer(DeviceContext* c, ReleaseObserver observer, void* user) noexcept {
    c->observer = observer;
    c->observer_user = user;
}
uint32_t harness_pending_releases(DeviceContext* c) noexcept { return c->pending.load(); }
void harness_set_in_ddi_bound(DeviceContext* c, uint32_t milliseconds) noexcept { c->in_ddi_bound_ms = milliseconds; }
uint32_t harness_stuck_releases(DeviceContext* c) noexcept {
    AcquireSRWLockShared(&c->lock);
    const auto n = static_cast<uint32_t>(c->releases.stuck());
    ReleaseSRWLockShared(&c->lock);
    return n;
}
bool harness_retirement_lost(DeviceContext* c) noexcept {
    AcquireSRWLockShared(&c->lock);
    const bool lost = c->retirement_lost;
    ReleaseSRWLockShared(&c->lock);
    return lost;
}
uint32_t harness_live_objects(DeviceContext* c) noexcept { return c->live.load(); }
uint64_t harness_deferred_retire_points(DeviceContext* c) noexcept { return c->retire_deferred.load(); }
IUnknown* harness_engine_object(const void* storage) noexcept {
    return storage ? static_cast<const RecordHeader*>(storage)->engine : nullptr;
}
void harness_set_log_observer(LogObserver observer, void* user) noexcept {
    log_observer_user.store(user);
    log_observer.store(observer);
}
#endif

} // namespace engine_ddi
