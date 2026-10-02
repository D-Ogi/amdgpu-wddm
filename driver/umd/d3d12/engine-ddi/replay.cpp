// SPDX-License-Identifier: MIT
// engine-ddi: deferred command-list replay (replay.h), everything off the encode path: the thread's ring, the
// worker, waits and drains, descriptor snapshots, set_replay_policy and teardown.
#include "replay.h"
#include <cstdarg>
#include <cstdio>
#include <intrin.h>
#include <iterator>

#pragma comment(lib, "synchronization.lib")     // WaitOnAddress, WakeByAddressSingle

namespace engine_ddi {

namespace {
constexpr const char* kDrainNames[] = {"reset", "bundle", "ecl", "switch", "direct", "destroy",
                                       "pool", "closes", "stack", "space", "slots", "teardown"};
static_assert(std::size(kDrainNames) == static_cast<size_t>(Drain::Count), "drain names");
// Snapshot slots by descriptor heap type (CBV_SRV_UAV, SAMPLER, RTV, DSV). OMSetRenderTargets names up to eight
// RTVs and a DSV per call, a clear one view; a slot is free again as soon as the worker has passed its entry.
constexpr uint32_t kSnapshotSlots[4] = {1024, 0, 4096, 1024};
// The ring's control block and its snapshot bookkeeping come first in the ring's allocation.
constexpr uint64_t kRingHeader = 64 * 1024;
static_assert(replay_detail::align_up(sizeof(ReplayRing), 64) + sizeof(uint64_t) * (1024 + 4096 + 1024) <= kRingHeader,
              "ring header");
constexpr uint32_t kSpinMicroseconds = 20;      // the worker spins this long for more work before it yields
// Then it yields its processor to ready threads, looking for work between yields, this long before it sleeps. A
// sleeping worker costs the recording thread a wake (a system call) per burst: 310 measured 0.57 ms per frame of the
// main thread in replay_wake, publishes and Closes together. Yielding costs the worker's processor only when no other
// thread is ready to run on it.
constexpr uint32_t kYieldMicroseconds = 1000;
constexpr DWORD kSleepMilliseconds = 100;       // a sleeping worker looks again at least this often
constexpr uint32_t kWakeKilobytes = 8;          // pending entries that wake a sleeping worker (about 80 calls)
constexpr uint32_t kLongWaitMilliseconds = 20;  // a ring wait this long gets a line of its own
constexpr uint64_t kLongWaitLines = 4096;       // that many lines, then one at every power of two
constexpr uint32_t kSummarySeconds = 10;        // after the summary at ecl drain 64, one at the first drain this much later

std::atomic<uint64_t> g_serial{0};

uint64_t qpc() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}
bool power_of_two(uint64_t n) noexcept { return n && !(n & (n - 1)); }
// A counter with one writer: a plain load and store, no locked instruction.
void bump(std::atomic<uint64_t>& counter, uint64_t by = 1) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}
// The drains of DDI calls: the shell's drained hook runs after them (a removal the worker found is reported there).
bool ddi_drain(Drain kind) noexcept {
    switch (kind) {
    case Drain::Reset: case Drain::Bundle: case Drain::Ecl: case Drain::Destroy: case Drain::Pool: case Drain::Closes:
    case Drain::Stack: return true;
    default: return false;
    }
}
uint64_t microseconds(const Replay* rp, uint64_t ticks) noexcept { return ticks * 1000000 / rp->qpf; }
double milliseconds(const Replay* rp, uint64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(rp->qpf);
}
// Thread cycles (QueryThreadCycleTime counts at the TSC's rate) as milliseconds, by the TSC rate since the policy
// went on: an estimate for the long-wait lines.
double cycle_milliseconds(const Replay* rp, uint64_t cycles) noexcept {
    const uint64_t ticks = qpc() - rp->qpc0, tsc = __rdtsc() - rp->tsc0;
    return ticks && tsc ? static_cast<double>(cycles) * milliseconds(rp, ticks) / static_cast<double>(tsc) : 0.0;
}
// An entry's run as an offset in its module: the linker map names the slot's lambda.
unsigned long long rva_of(void (*run)(const EntryHeader*) noexcept) noexcept {
    HMODULE module = nullptr;
    if (run && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(run)), &module) && module)
        return reinterpret_cast<uintptr_t>(run) - reinterpret_cast<uintptr_t>(module);
    return 0;
}

// Every replay line: where engine-ddi's lines go, and to the policy's log hook.
void replay_log(const Replay* rp, _Printf_format_string_ const char* format, ...) noexcept {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log_line("%s", text);
    if (rp->policy.log) rp->policy.log(rp->policy.shell, text);
}

// ---- Worker priority --------------------------------------------------------------------------------------------
// The level a waiting thread lifts the worker to: one above its own, within the levels SetThreadPriority takes (a
// waiter at HIGHEST or above gives its own level), never below the ring's base.
int boost_level(int waiter, int base) noexcept {
    int want = base;
    if (waiter == THREAD_PRIORITY_ERROR_RETURN) return base;
    if (waiter >= THREAD_PRIORITY_HIGHEST) want = waiter;
    else if (waiter < THREAD_PRIORITY_LOWEST) want = THREAD_PRIORITY_LOWEST;
    else want = waiter + 1;
    return want > base ? want : base;
}
// Both under the ring's lock.
void boost_begin(ReplayRing* r, int waiter) noexcept {
    ++r->boosters;
    const int want = boost_level(waiter, r->base_priority);
    if (want <= r->applied_priority || !SetThreadPriority(r->thread, want)) return;
    r->applied_priority = want;
    r->replay->boosts.fetch_add(1, std::memory_order_relaxed);
}
void boost_end(ReplayRing* r) noexcept {
    if (--r->boosters || r->applied_priority == r->base_priority) return;
    if (SetThreadPriority(r->thread, r->base_priority)) r->applied_priority = r->base_priority;
}
// The ring's base: the calling thread's level (normal when it cannot be read). Under the ring's lock, or before the
// worker is visible to any waiter.
void set_base_priority(ReplayRing* r) noexcept {
    const int level = GetThreadPriority(GetCurrentThread());
    r->base_priority = level == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : level;
    if (r->boosters || r->applied_priority == r->base_priority) return;
    if (SetThreadPriority(r->thread, r->base_priority)) r->applied_priority = r->base_priority;
}

// ---- Worker -----------------------------------------------------------------------------------------------------
// Waiters lower next_wake to their target under the ring's lock; the worker, having stored done, wakes them all once
// done reaches it. Both sides use seq_cst (the worker: store done, load next_wake; a waiter: store next_wake, load
// done), so either the worker sees the target or the waiter sees done; the waiter's 2 ms slices are a safety net.
void notify(ReplayRing* r) noexcept {
    AcquireSRWLockExclusive(&r->lock);
    r->next_wake.store(UINT64_MAX, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&r->lock);
    WakeAllConditionVariable(&r->cv);
    bump(r->notifies);
}

// After catching up: true when work (or stop) arrived within the spin. Each look reads published, a line the
// producer writes on every publish; the entry path's arm c (entry.h, g_entry.poll_tsc) looks only every poll TSC ticks,
// pausing in between, so that a producer in a burst of calls does not find the line taken back after each one.
bool spin_for_work(ReplayRing* r, uint64_t pos, uint64_t ticks) noexcept {
    const uint64_t start = qpc();
    const uint64_t poll = g_entry.poll_tsc.load(std::memory_order_relaxed);
    if (poll) {
        for (uint64_t next = __rdtsc() + poll;;) {
            _mm_pause();
            const uint64_t now = __rdtsc();
            if (now < next) continue;
            next = now + poll;
            if (r->published.load(std::memory_order_relaxed) != pos || r->stop.load(std::memory_order_relaxed)) return true;
            if (qpc() - start > ticks) return false;
        }
    }
    for (uint32_t n = 1;; ++n) {
        _mm_pause();
        if (r->published.load(std::memory_order_relaxed) != pos || r->stop.load(std::memory_order_relaxed)) return true;
        if (!(n & 15) && qpc() - start > ticks) return false;
    }
}

// After the spin: the same, yielding between looks (a short pause instead when nothing else was ready to run).
bool yield_for_work(ReplayRing* r, uint64_t pos, uint64_t ticks) noexcept {
    const uint64_t start = qpc();
    for (;;) {
        if (r->published.load(std::memory_order_relaxed) != pos || r->stop.load(std::memory_order_relaxed)) return true;
        if (qpc() - start > ticks) return false;
        if (!SwitchToThread())
            for (uint32_t k = 0; k < 32; ++k) _mm_pause();
    }
}

// The worker's time by what it does, with the entry statistics on (entry.h): TSC ticks since mark go to counter.
void account(WorkerStats* stats, std::atomic<uint64_t> WorkerStats::*counter, uint64_t& mark) noexcept {
    if (!stats) return;
    const uint64_t now = __rdtsc();
    entry_bump(stats->*counter, now - mark);
    mark = now;
}

void APIENTRY worker_body(void* ring) {
    auto* r = static_cast<ReplayRing*>(ring);
    const uint64_t spin = r->replay->spin_ticks;
    const uint64_t yield = r->replay->yield_ticks;
    WorkerStats* const stats = r->stats;
    uint64_t mark = stats ? __rdtsc() : 0;
    uint64_t pos = r->done.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t end = r->published.load(std::memory_order_acquire);
        if (pos != end) {
            uint64_t ran = 0;
            while (pos != end) {
                const auto* h = reinterpret_cast<const EntryHeader*>(r->bytes + (pos & r->mask));
                const uint32_t size = h->size;
                if (const auto run = h->run) {
                    r->running.store(run, std::memory_order_relaxed);
                    run(h);
                    bump(r->worker_entries);
                    ++ran;
                }
                pos += size;
                r->done.store(pos, std::memory_order_seq_cst);
                if (pos >= r->next_wake.load(std::memory_order_seq_cst)) notify(r);
            }
            account(stats, &WorkerStats::busy, mark);
            if (stats) entry_bump(stats->entries, ran);
        }
        if (r->stop.load(std::memory_order_acquire)) {
            if (r->published.load(std::memory_order_acquire) == pos) return;
            continue;
        }
        const bool spun = spin_for_work(r, pos, spin);
        account(stats, &WorkerStats::spin, mark);
        if (spun) continue;
        const bool yielded = yield_for_work(r, pos, yield);
        account(stats, &WorkerStats::yield, mark);
        if (yielded) continue;
        // Sleep on the futex word. A waker exchanges it to 0 first, so a wake that comes before WaitOnAddress makes
        // it return at once; the read-modify-write of published orders this check after the store of sleeping.
        r->sleeping.store(1, std::memory_order_seq_cst);
        if (r->published.fetch_add(0, std::memory_order_seq_cst) == pos && !r->stop.load(std::memory_order_seq_cst)) {
            uint32_t asleep = 1;
            bump(r->sleeps);
            WaitOnAddress(&r->sleeping, &asleep, sizeof(asleep), kSleepMilliseconds);
        }
        r->sleeping.store(0, std::memory_order_relaxed);
        account(stats, &WorkerStats::sleep, mark);
    }
}

DWORD WINAPI worker_thread(void* ring) {
    const ReplayPolicy& policy = static_cast<ReplayRing*>(ring)->replay->policy;
    policy.worker(policy.shell, worker_body, ring);
    return 0;
}

// ---- Waits ------------------------------------------------------------------------------------------------------
void stall_line(ReplayRing* r, uint64_t target, Drain kind, uint64_t ticks, bool gone) noexcept {
    Replay* rp = r->replay;
    rp->stalls.fetch_add(1, std::memory_order_relaxed);
    replay_log(rp, "replay: %s drain has waited %llu ms for ring %u (thread %lu): target %llu, done %llu, published %llu, "
               "last entry started at rva 0x%llx%s",
               kDrainNames[static_cast<size_t>(kind)], static_cast<unsigned long long>(microseconds(rp, ticks) / 1000),
               r->index, GetCurrentThreadId(), static_cast<unsigned long long>(target),
               static_cast<unsigned long long>(r->done.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(r->published.load(std::memory_order_relaxed)),
               rva_of(r->running.load(std::memory_order_relaxed)),
               gone ? "; the worker thread has exited, the drain ends" : "");
}

// What a wait saw when it stopped spinning, for its long-wait line.
struct WaitStart {
    uint64_t qpc;                               // when the wait began
    uint64_t done, entries, wakes;
    ULONG64 cycles;                             // the worker's
    int waiter;                                 // the waiting thread's level
    bool asleep;                                // the worker slept with this work behind it
};

// What a wait saw while it slept: the longest stretch without a finished entry, and the entry running then.
struct WaitStuck {
    uint64_t ticks;
    void (*run)(const EntryHeader*) noexcept;
};

// One line per long ring wait, as key=value pairs: what waited, the worker's levels (base, then during the wait), how
// far behind it was, how much it ran (bytes, entries, CPU time) and its longest stretch without finishing an entry.
// stuck_ms near ms with worker_cpu_ms near 0: one entry blocked (or the worker descheduled) inside the engine; with
// worker_cpu_ms near ms: one long engine call; worker_cpu_ms well below ms over many entries: the worker waited for
// a CPU.
void long_wait_line(ReplayRing* r, uint64_t target, Drain kind, const WaitStart& w, uint64_t ticks, int base,
                    int lifted, const WaitStuck& stuck) noexcept {
    Replay* rp = r->replay;
    const uint64_t n = rp->long_waits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > kLongWaitLines && !power_of_two(n)) return;
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(r->thread, &cycles)) cycles = w.cycles;
    const uint64_t done = r->done.load(std::memory_order_relaxed);
    replay_log(rp, "replay long-wait: n=%llu kind=%s ms=%.1f ring=%u thread=%lu qpc=%llu waiter_priority=%d "
               "worker_priority=%d/%d asleep=%u behind_bytes=%llu ran_bytes=%llu entries=%llu worker_cpu_ms=%.1f "
               "wakes=%llu stuck_ms=%.1f stuck_rva=0x%llx",
               static_cast<unsigned long long>(n), kDrainNames[static_cast<size_t>(kind)], milliseconds(rp, ticks),
               r->index, GetCurrentThreadId(), static_cast<unsigned long long>(w.qpc), w.waiter, base, lifted,
               w.asleep ? 1u : 0u, static_cast<unsigned long long>(target - w.done),
               static_cast<unsigned long long>(done - w.done),
               static_cast<unsigned long long>(r->worker_entries.load(std::memory_order_relaxed) - w.entries),
               cycle_milliseconds(rp, cycles - w.cycles),
               static_cast<unsigned long long>(r->wakes.load(std::memory_order_relaxed) - w.wakes),
               milliseconds(rp, stuck.ticks), rva_of(stuck.run));
}

// Waits until the ring's worker has passed target. Returns whether it had to wait, and the ticks in *ticks. Gives up
// only when the worker thread no longer exists (a process exit ends threads before it tears devices down).
bool wait_until(ReplayRing* r, uint64_t target, Drain kind, uint64_t* ticks) noexcept {
    *ticks = 0;
    if (r->done.load(std::memory_order_acquire) >= target) return false;
    Replay* rp = r->replay;
    const uint64_t start = qpc();
    // Asleep with work behind it: a wake was lost, or the worker is between the publish's wake and running again.
    const bool asleep = r->sleeping.load(std::memory_order_seq_cst) != 0;
    replay_wake(r);
    // Most waits end within microseconds (a Close right after its list's last calls): spin before the lock.
    for (uint32_t n = 1;; ++n) {
        _mm_pause();
        if (r->done.load(std::memory_order_acquire) >= target) {
            *ticks = qpc() - start;
            return true;
        }
        if (!(n & 15) && qpc() - start > 2 * rp->spin_ticks) break;
    }
    // Past the spin: the waiter is blocked on the worker, which runs above it until the wait ends (boost_begin).
    WaitStart w{start, r->done.load(std::memory_order_relaxed), r->worker_entries.load(std::memory_order_relaxed),
                r->wakes.load(std::memory_order_relaxed), 0, GetThreadPriority(GetCurrentThread()), asleep};
    QueryThreadCycleTime(r->thread, &w.cycles);
    if (asleep && w.done < target) rp->asleep_waits.fetch_add(1, std::memory_order_relaxed);
    uint64_t line = start + rp->qpf;
    AcquireSRWLockExclusive(&r->lock);
    boost_begin(r, w.waiter);
    const int base = r->base_priority, lifted = r->applied_priority;
    WaitStuck stuck{0, nullptr};
    uint64_t seen = w.entries, since = start;   // the worker's finished entries, and since when that count stands
    for (;;) {
        if (target < r->next_wake.load(std::memory_order_relaxed)) r->next_wake.store(target, std::memory_order_seq_cst);
        if (r->done.load(std::memory_order_seq_cst) >= target) break;
        replay_wake(r);
        SleepConditionVariableSRW(&r->cv, &r->lock, 2, 0);
        const uint64_t now = qpc(), finished = r->worker_entries.load(std::memory_order_relaxed);
        if (finished != seen) {
            seen = finished;
            since = now;
        } else if (now - since > stuck.ticks) {
            stuck = {now - since, r->running.load(std::memory_order_relaxed)};
        }
        if (now < line) continue;
        ReleaseSRWLockExclusive(&r->lock);          // never log with the lock held
        const bool gone = WaitForSingleObject(r->thread, 0) == WAIT_OBJECT_0;
        stall_line(r, target, kind, now - start, gone);
        line = now + rp->qpf;
        AcquireSRWLockExclusive(&r->lock);
        if (gone) break;
    }
    boost_end(r);
    if (stuck.ticks > r->stuck_longest) {
        r->stuck_longest = stuck.ticks;
        r->stuck_run = stuck.run;
    }
    ReleaseSRWLockExclusive(&r->lock);
    *ticks = qpc() - start;
    if (*ticks >= rp->qpf * kLongWaitMilliseconds / 1000) long_wait_line(r, target, kind, w, *ticks, base, lifted, stuck);
    return true;
}

void count_drain(Replay* rp, Drain kind, bool waited, uint64_t ticks) noexcept {
    ReplayDrainCounter& c = rp->drains[static_cast<size_t>(kind)];
    c.calls.fetch_add(1, std::memory_order_relaxed);
    if (!waited) return;
    c.waits.fetch_add(1, std::memory_order_relaxed);
    c.qpc.fetch_add(ticks, std::memory_order_relaxed);
    const uint64_t ms = rp->qpf / 1000;
    if (ticks < ms) return;
    c.over_1ms.fetch_add(1, std::memory_order_relaxed);
    if (ticks >= 10 * ms) c.over_10ms.fetch_add(1, std::memory_order_relaxed);
    if (ticks >= 50 * ms) c.over_50ms.fetch_add(1, std::memory_order_relaxed);
    for (uint64_t seen = c.longest.load(std::memory_order_relaxed);
         ticks > seen && !c.longest.compare_exchange_weak(seen, ticks, std::memory_order_relaxed);) {
    }
}

// Appends " <kind> <fields>" for every drain kind that has run, as long as text has room.
template <class F> void per_kind(const Replay* rp, char (&text)[400], F&& fields) noexcept {
    size_t used = 0;
    for (size_t k = 0; k < static_cast<size_t>(Drain::Count) && used < sizeof(text); ++k) {
        const ReplayDrainCounter& c = rp->drains[k];
        if (!c.calls.load(std::memory_order_relaxed)) continue;
        const int written = fields(text + used, sizeof(text) - used, kDrainNames[k], c);
        if (written < 0) break;
        used += static_cast<size_t>(written);
    }
}

// Four lines: what the rings carried; each drain kind as calls/waits/microseconds waited; each kind's waits of 1, 10
// and 50 ms or more and its longest; the workers' longest stuck stretch seen by a wait, levels and wakes. Every
// figure counts from the policy's start: a window is the difference of two summaries.
void summary(Replay* rp, const char* why) noexcept {
    const uint32_t n = rp->count.load(std::memory_order_acquire);
    uint64_t entries = 0, bytes = 0, run = 0, wakes = 0, sleeps = 0, notifies = 0, stuck = 0;
    void (*stuck_run)(const EntryHeader*) noexcept = nullptr;
    char levels[64] = {};
    size_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        ReplayRing* r = rp->rings[i];
        entries += r->entries.load(std::memory_order_relaxed);
        bytes += r->entry_bytes.load(std::memory_order_relaxed);
        run += r->worker_entries.load(std::memory_order_relaxed);
        wakes += r->wakes.load(std::memory_order_relaxed);
        sleeps += r->sleeps.load(std::memory_order_relaxed);
        notifies += r->notifies.load(std::memory_order_relaxed);
        AcquireSRWLockExclusive(&r->lock);
        const int base = r->base_priority;
        if (r->stuck_longest > stuck) {
            stuck = r->stuck_longest;
            stuck_run = r->stuck_run;
        }
        ReleaseSRWLockExclusive(&r->lock);
        if (used >= sizeof(levels)) continue;
        const int written = std::snprintf(levels + used, sizeof(levels) - used, "%s%d", i ? "," : "", base);
        if (written > 0) used += static_cast<size_t>(written);
    }
    const uint64_t now = qpc();
    replay_log(rp, "replay %s: %u rings, %llu entries (%llu KiB, %llu run), oversize %llu, fallbacks %llu, direct "
               "lookups %llu, reclaimed %llu, ring failures %llu, stalls %llu, wakes %llu, sleeps %llu, notifies %llu "
               "(qpc %llu, %.1f s on)",
               why, n, static_cast<unsigned long long>(entries), static_cast<unsigned long long>(bytes >> 10),
               static_cast<unsigned long long>(run), static_cast<unsigned long long>(rp->oversize.load()),
               static_cast<unsigned long long>(rp->fallbacks.load()), static_cast<unsigned long long>(rp->direct_lookups.load()),
               static_cast<unsigned long long>(rp->reclaimed.load()), static_cast<unsigned long long>(rp->ring_failures.load()),
               static_cast<unsigned long long>(rp->stalls.load()), static_cast<unsigned long long>(wakes),
               static_cast<unsigned long long>(sleeps), static_cast<unsigned long long>(notifies),
               static_cast<unsigned long long>(now), milliseconds(rp, now - rp->qpc0) / 1000.0);
    char text[400] = {};
    per_kind(rp, text, [&](char* out, size_t room, const char* name, const ReplayDrainCounter& c) {
        return std::snprintf(out, room, " %s %llu/%llu/%llu", name,
                             static_cast<unsigned long long>(c.calls.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(c.waits.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(microseconds(rp, c.qpc.load(std::memory_order_relaxed))));
    });
    replay_log(rp, "replay %s drains (calls/waits/us):%s", why, text);
    text[0] = 0;
    per_kind(rp, text, [&](char* out, size_t room, const char* name, const ReplayDrainCounter& c) {
        if (!c.over_1ms.load(std::memory_order_relaxed)) return 0;
        return std::snprintf(out, room, " %s %llu/%llu/%llu/%llu", name,
                             static_cast<unsigned long long>(c.over_1ms.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(c.over_10ms.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(c.over_50ms.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(microseconds(rp, c.longest.load(std::memory_order_relaxed))));
    });
    replay_log(rp, "replay %s long drains (waits of 1/10/50 ms or more, longest us):%s", why, text[0] ? text : " none");
    replay_log(rp, "replay %s workers: longest stretch without a finished entry seen by a wait %llu us at rva 0x%llx; "
               "base levels %s; waits that found a worker asleep %llu, priority lifts %llu, long waits %llu",
               why, static_cast<unsigned long long>(microseconds(rp, stuck)), rva_of(stuck_run), levels,
               static_cast<unsigned long long>(rp->asleep_waits.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(rp->boosts.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(rp->long_waits.load(std::memory_order_relaxed)));
}

// The list's entries, wherever they are, have replayed when this returns; its tail is cleared.
void drain_list_impl(Replay* rp, CommandListRecord* l, Drain kind) noexcept {
    const uint64_t tail = l->replay_tail.load(std::memory_order_relaxed);
    bool waited = false;
    uint64_t ticks = 0;
    if (tail) {
        waited = wait_until(rp->rings[(tail >> 56) - 1], tail & kReplayPositionMask, kind, &ticks);
        l->replay_tail.store(0, std::memory_order_relaxed);
    }
    count_drain(rp, kind, waited, ticks);
}

// ---- Rings ------------------------------------------------------------------------------------------------------
ReplayRing* create_ring(Replay* rp, uint32_t index) noexcept {
    const uint64_t bytes = rp->policy.ring_bytes;
    HANDLE owner = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &owner, SYNCHRONIZE, FALSE, 0))
        return nullptr;
    void* memory = VirtualAlloc(nullptr, kRingHeader + bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) {
        CloseHandle(owner);
        return nullptr;
    }
    auto* r = new (memory) ReplayRing{};
    auto* slots = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(memory) + replay_detail::align_up(sizeof(ReplayRing), 64));
    for (uint32_t t = 0; t < 4; ++t) {
        r->snapshots[t].capacity = kSnapshotSlots[t];
        r->snapshots[t].free_after = slots;       // zero: VirtualAlloc's pages are
        slots += kSnapshotSlots[t];
    }
    r->replay = rp;
    r->bytes = static_cast<uint8_t*>(memory) + kRingHeader;
    r->size = bytes;
    r->mask = bytes - 1;
    r->max_entry = bytes / 8;
    r->tag = uint64_t{index + 1} << 56;
    r->index = index;
    r->stats = entry_worker_stats(index, rp->serial);
    r->owner = owner;
    r->owner_id = GetCurrentThreadId();
    r->thread = CreateThread(nullptr, 0, worker_thread, r, CREATE_SUSPENDED, nullptr);
    if (!r->thread) {
        r->~ReplayRing();
        VirtualFree(memory, 0, MEM_RELEASE);
        CloseHandle(owner);
        return nullptr;
    }
    SetThreadDescription(r->thread, L"amdgpu_wddm replay");
    set_base_priority(r);                       // no waiter yet: the ring is not in Replay::rings
    ResumeThread(r->thread);
    return r;
}

// After its worker has been joined.
void free_ring(ReplayRing* r) noexcept {
    for (SnapshotHeap& s : r->snapshots)
        if (s.heap) s.heap->Release();
    if (r->owner) CloseHandle(r->owner);
    if (r->thread) CloseHandle(r->thread);
    r->~ReplayRing();
    VirtualFree(r, 0, MEM_RELEASE);
}

// A ring passes to this thread. Under Replay::lock, its old owner thread has exited: nothing writes its producer
// fields any more, and the wait on the old owner's handle orders that thread's last writes before ours.
bool adopt(ReplayRing* r) noexcept {
    HANDLE owner = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &owner, SYNCHRONIZE, FALSE, 0))
        return false;
    CloseHandle(r->owner);
    r->owner = owner;
    r->owner_id = GetCurrentThreadId();
    AcquireSRWLockExclusive(&r->lock);          // a leaf under Replay::lock
    set_base_priority(r);
    ReleaseSRWLockExclusive(&r->lock);
    return true;
}
} // namespace

void replay_wake(ReplayRing* r) noexcept {
    if (!r->sleeping.exchange(0, std::memory_order_seq_cst)) return;
    WakeByAddressSingle(&r->sleeping);
    r->wakes.fetch_add(1, std::memory_order_relaxed);
}

ReplayRing* replay_thread_ring(Replay* rp) noexcept {
    const DWORD self = GetCurrentThreadId();
    ReplayRing* found = nullptr;
    bool created = false, reclaimed = false;
    AcquireSRWLockExclusive(&rp->lock);
    const uint32_t n = rp->count.load(std::memory_order_relaxed);
    // This thread's ring: its id, and its handle not signalled (an exited thread's id can be reused).
    for (uint32_t i = 0; i < n && !found; ++i) {
        ReplayRing* r = rp->rings[i];
        if (r && r->owner_id == self && WaitForSingleObject(r->owner, 0) == WAIT_TIMEOUT) found = r;
    }
    // A ring whose owner thread has exited.
    for (uint32_t i = 0; i < n && !found; ++i) {
        ReplayRing* r = rp->rings[i];
        if (r && WaitForSingleObject(r->owner, 0) == WAIT_OBJECT_0 && adopt(r)) {
            found = r;
            reclaimed = true;
        }
    }
    if (!found && n < rp->policy.rings) {
        found = create_ring(rp, n);
        if (found) {
            rp->rings[n] = found;
            rp->count.store(n + 1, std::memory_order_release);
            created = true;
        } else {
            rp->ring_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockExclusive(&rp->lock);
    const int level = GetThreadPriority(GetCurrentThread());
    if (created) replay_log(rp, "replay: ring %u of %u for thread %lu, worker level %d", n, rp->policy.rings, self, level);
    if (reclaimed && found) {
        rp->reclaimed.fetch_add(1, std::memory_order_relaxed);
        replay_log(rp, "replay: ring %u taken over by thread %lu, worker level %d", found->index, self, level);
    }
    if (!found) {
        const uint64_t lookups = rp->direct_lookups.fetch_add(1, std::memory_order_relaxed) + 1;
        if (power_of_two(lookups)) replay_log(rp, "replay: thread %lu records directly (%llu such lookups)", self,
                                              static_cast<unsigned long long>(lookups));
    }
    t_replay = {rp->serial, found};
    return found;
}

void replay_switch(Replay* rp, CommandListRecord* l) noexcept { drain_list_impl(rp, l, Drain::Switch); }

void replay_make_room(ReplayRing* r, uint64_t until) noexcept {
    uint64_t ticks = 0;
    const bool waited = wait_until(r, until, Drain::Space, &ticks);
    count_drain(r->replay, Drain::Space, waited, ticks);
    r->cached_done = r->done.load(std::memory_order_acquire);
}

bool replay_fallback(ReplayRing* r, CommandListRecord* l, bool oversize) noexcept {
    Replay* rp = r->replay;
    const uint64_t n = (oversize ? rp->oversize : rp->fallbacks).fetch_add(1, std::memory_order_relaxed) + 1;
    if (power_of_two(n))
        replay_log(rp, "replay: a call made directly (%s, %llu so far)",
                   oversize ? "entry over the size limit" : "no snapshot heap", static_cast<unsigned long long>(n));
    drain_list_impl(rp, l, Drain::Direct);
    return false;
}

bool replay_snapshot_ready(ReplayRing* r, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count) noexcept {
    if (type < 0 || type >= 4) return false;
    SnapshotHeap& s = r->snapshots[type];
    if (count > s.capacity) return false;
    if (s.heap) return true;
    if (s.failed) return false;
    DeviceContext* c = r->replay->context;
    const D3D12_DESCRIPTOR_HEAP_DESC desc{type, s.capacity, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap* heap = nullptr;
    const HRESULT hr = c->device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
    if (FAILED(hr) || !heap) {
        s.failed = true;
        replay_log(r->replay, "replay: ring %u has no snapshot heap of type %d (%08lx); calls that need one are made "
                   "directly", r->index, static_cast<int>(type), static_cast<unsigned long>(hr));
        return false;
    }
    s.heap = heap;
    s.base = heap->GetCPUDescriptorHandleForHeapStart();
    s.increment = c->increments[type];
    return true;
}

// Takes contiguous slots (FIFO, from the start again when the rest is too short), waits until the worker has passed
// every entry that named them, copies the descriptors with the engine's own CopyDescriptorsSimple and writes the
// copies' handles to out. A null source handle stays null: the engine treats it as unbound.
void replay_snapshot(ReplayRing* r, const Snap& snap, D3D12_CPU_DESCRIPTOR_HANDLE* out, uint64_t end) noexcept {
    SnapshotHeap& s = r->snapshots[snap.type];
    uint32_t first = s.next;
    if (first + snap.count > s.capacity) first = 0;
    uint64_t need = 0;
    for (uint32_t k = 0; k < snap.count; ++k) need = s.free_after[first + k] > need ? s.free_after[first + k] : need;
    if (need > r->cached_done) {
        r->cached_done = r->done.load(std::memory_order_acquire);
        if (need > r->cached_done) {
            uint64_t ticks = 0;
            const bool waited = wait_until(r, need, Drain::Slots, &ticks);
            count_drain(r->replay, Drain::Slots, waited, ticks);
            r->cached_done = r->done.load(std::memory_order_acquire);
        }
    }
    for (uint32_t k = 0; k < snap.count; ++k) s.free_after[first + k] = end;
    s.next = first + snap.count;
    ID3D12Device* device = r->replay->context->device;
    const D3D12_CPU_DESCRIPTOR_HANDLE base{s.base.ptr + static_cast<SIZE_T>(first) * s.increment};
    if (snap.range) {
        out[0] = snap.handles[0].ptr ? base : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (snap.handles[0].ptr) device->CopyDescriptorsSimple(snap.count, base, snap.handles[0], snap.type);
        return;
    }
    for (uint32_t k = 0; k < snap.count; ++k) {
        const D3D12_CPU_DESCRIPTOR_HANDLE copy{base.ptr + static_cast<SIZE_T>(k) * s.increment};
        out[k] = snap.handles[k].ptr ? copy : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (snap.handles[k].ptr) device->CopyDescriptorsSimple(1, copy, snap.handles[k], snap.type);
    }
}

void replay_drain_list(Replay* rp, CommandListRecord* l, Drain kind) noexcept {
    drain_list_impl(rp, l, kind);
    if (ddi_drain(kind)) rp->policy.drained(rp->policy.shell);
    // A failed engine Close the worker made (close_list) reaches the runtime from this drain, on its API thread. The
    // destroy of the list drops it: the runtime's list is going away.
    if (FAILED(l->close_hr.load(std::memory_order_relaxed))) {
        const HRESULT hr = l->close_hr.exchange(S_OK, std::memory_order_relaxed);
        log_line("CloseCommandList: the engine Close failed on the replay worker (%08lx), %s at the list's %s",
                 static_cast<unsigned long>(hr), kind == Drain::Destroy ? "dropped" : "reported",
                 kDrainNames[static_cast<size_t>(kind)]);
        if (kind != Drain::Destroy) l->h.device->report_list(l->rt, hr);
    }
    // The ecl drain is the one a frame makes: the list's Close is the list's last replay entry and drains nothing.
    if (kind != Drain::Ecl) return;
    const uint64_t executes = rp->drains[static_cast<size_t>(Drain::Ecl)].calls.load(std::memory_order_relaxed);
    const uint64_t now = qpc(), period = rp->qpf * kSummarySeconds;
    char why[64];
    // A summary at the powers of two from ecl drain 64 on. The powers of two thin out, so from 64 on there is also
    // one at the first ecl drain kSummarySeconds after the last summary: a game that never tears its device down (a
    // process exit does not) still leaves figures every few seconds.
    if (executes >= 64 && power_of_two(executes)) {
        if (executes == 64) rp->next_summary.store(now + period, std::memory_order_relaxed);
    } else {
        uint64_t due = rp->next_summary.load(std::memory_order_relaxed);
        if (now < due || !rp->next_summary.compare_exchange_strong(due, now + period, std::memory_order_relaxed)) return;
    }
    std::snprintf(why, sizeof(why), "at ecl drain %llu", static_cast<unsigned long long>(executes));
    summary(rp, why);
}

// Positions above a ring's published end are from a replay turned off since (set_replay_policy): nothing to wait for.
void replay_drain_closes(Replay* rp, const std::atomic<uint64_t>* closing) noexcept {
    const uint32_t n = rp->count.load(std::memory_order_acquire);
    bool pending = false, waited = false;
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t target = closing[i].load(std::memory_order_relaxed);
        ReplayRing* r = rp->rings[i];
        if (!target || r->done.load(std::memory_order_acquire) >= target ||
            target > r->published.load(std::memory_order_acquire))
            continue;
        pending = true;
        uint64_t t = 0;
        if (wait_until(r, target, Drain::Closes, &t)) {
            waited = true;
            ticks += t;
        }
    }
    if (!pending) return;                       // counted and hooked only when a Close was still pending
    count_drain(rp, Drain::Closes, waited, ticks);
    rp->policy.drained(rp->policy.shell);
}

void replay_drain_all(Replay* rp, Drain kind) noexcept {
    const uint32_t n = rp->count.load(std::memory_order_acquire);
    bool waited = false;
    uint64_t ticks = 0;
    for (uint32_t i = 0; i < n; ++i) {
        ReplayRing* r = rp->rings[i];
        uint64_t t = 0;
        if (wait_until(r, r->published.load(std::memory_order_seq_cst), kind, &t)) {
            waited = true;
            ticks += t;
        }
    }
    count_drain(rp, kind, waited, ticks);
    if (ddi_drain(kind)) rp->policy.drained(rp->policy.shell);
}

void replay_off(DeviceContext* c) noexcept {
    Replay* rp = c->replay;
    if (!rp) return;
    const uint32_t n = rp->count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) {
        ReplayRing* r = rp->rings[i];
        uint64_t ticks = 0;
        const bool waited = wait_until(r, r->published.load(std::memory_order_seq_cst), Drain::Teardown, &ticks);
        count_drain(rp, Drain::Teardown, waited, ticks);
        // The worker is caught up: it sees stop within its spin, or at once when woken from its sleep.
        r->stop.store(true, std::memory_order_seq_cst);
        replay_wake(r);
        for (uint32_t s = 1; WaitForSingleObject(r->thread, 1000) == WAIT_TIMEOUT; ++s)
            replay_log(rp, "replay: the worker of ring %u has not exited after %u s", i, s);
    }
    summary(rp, "teardown");
    replay_log(rp, "replay policy: off");
    c->replay = nullptr;
    for (uint32_t i = 0; i < n; ++i) free_ring(rp->rings[i]);
    delete rp;
}

HRESULT set_replay_policy(DeviceContext* c, const ReplayPolicy* policy) noexcept {
    const char* refused = !c ? "no context" : !policy ? "no policy" : policy->size != sizeof(ReplayPolicy) ? "size" :
                          policy->enabled > 1 ? "enabled" : nullptr;
    if (!refused && policy->enabled) {
        const uint32_t bytes = policy->ring_bytes;
        refused = c->replay ? "already on" :
                  (!policy->rings || policy->rings > kMaxReplayRings) ? "rings" :
                  (bytes < (64u << 10) || bytes > (64u << 20) || (bytes & (bytes - 1))) ? "ring bytes" :
                  (!policy->worker || !policy->drained) ? "hooks" : nullptr;
    }
    if (refused) {
        log_refusal("set_replay_policy: refused (%s)", refused);
        return E_INVALIDARG;
    }
    if (!policy->enabled) {
        replay_off(c);
        return S_OK;
    }
    auto* rp = make_new<Replay>();
    if (!rp) return E_OUTOFMEMORY;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    rp->context = c;
    rp->policy = *policy;
    rp->serial = g_serial.fetch_add(1) + 1;
    rp->qpf = static_cast<uint64_t>(frequency.QuadPart);
    rp->spin_ticks = rp->qpf * kSpinMicroseconds / 1000000;
    rp->yield_ticks = rp->qpf * kYieldMicroseconds / 1000000;
    rp->wake_bytes = uint64_t{kWakeKilobytes} << 10;
    rp->qpc0 = qpc();
    rp->tsc0 = __rdtsc();
    c->replay = rp;
    replay_log(rp, "replay policy: on, up to %u rings of %u KiB, worker spin %u us, yield %u us, wake at %u KiB pending, "
               "workers at their owner's level and lifted above a waiting drain, long waits from %u ms, summaries every "
               "%u s (qpc %llu, qpf %llu)", policy->rings, policy->ring_bytes >> 10, kSpinMicroseconds,
               kYieldMicroseconds, kWakeKilobytes, kLongWaitMilliseconds, kSummarySeconds,
               static_cast<unsigned long long>(rp->qpc0), static_cast<unsigned long long>(rp->qpf));
    return S_OK;
}

} // namespace engine_ddi
