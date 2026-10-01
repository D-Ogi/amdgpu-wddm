// SPDX-License-Identifier: MIT
// engine-ddi: deferred command-list replay (replay.h), everything off the encode path: the thread's ring, the
// worker, waits and drains, descriptor snapshots, set_replay_policy and teardown.
#include "replay.h"
#include <cstdio>
#include <intrin.h>
#include <iterator>

#pragma comment(lib, "synchronization.lib")     // WaitOnAddress, WakeByAddressSingle

namespace engine_ddi {

namespace {
constexpr const char* kDrainNames[] = {"close", "reset", "bundle", "ecl", "switch", "direct",
                                       "destroy", "pool", "stack", "space", "slots", "teardown"};
static_assert(std::size(kDrainNames) == static_cast<size_t>(Drain::Count), "drain names");
// Snapshot slots by descriptor heap type (CBV_SRV_UAV, SAMPLER, RTV, DSV). OMSetRenderTargets names up to eight
// RTVs and a DSV per call, a clear one view; a slot is free again as soon as the worker has passed its entry.
constexpr uint32_t kSnapshotSlots[4] = {1024, 0, 4096, 1024};
// The ring's control block and its snapshot bookkeeping come first in the ring's allocation.
constexpr uint64_t kRingHeader = 64 * 1024;
static_assert(replay_detail::align_up(sizeof(ReplayRing), 64) + sizeof(uint64_t) * (1024 + 4096 + 1024) <= kRingHeader,
              "ring header");
constexpr uint32_t kSpinMicroseconds = 20;      // the worker spins this long for more work before it sleeps
constexpr DWORD kSleepMilliseconds = 100;       // a sleeping worker looks again at least this often

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
    case Drain::Close: case Drain::Reset: case Drain::Bundle: case Drain::Ecl: case Drain::Destroy: case Drain::Pool:
    case Drain::Stack: return true;
    default: return false;
    }
}
uint64_t microseconds(const Replay* rp, uint64_t ticks) noexcept { return ticks * 1000000 / rp->qpf; }

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

// After catching up: true when work (or stop) arrived within the spin.
bool spin_for_work(ReplayRing* r, uint64_t pos, uint64_t ticks) noexcept {
    const uint64_t start = qpc();
    for (uint32_t n = 1;; ++n) {
        _mm_pause();
        if (r->published.load(std::memory_order_relaxed) != pos || r->stop.load(std::memory_order_relaxed)) return true;
        if (!(n & 15) && qpc() - start > ticks) return false;
    }
}

void APIENTRY worker_body(void* ring) {
    auto* r = static_cast<ReplayRing*>(ring);
    const uint64_t spin = r->replay->spin_ticks;
    uint64_t pos = r->done.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t end = r->published.load(std::memory_order_acquire);
        while (pos != end) {
            const auto* h = reinterpret_cast<const EntryHeader*>(r->bytes + (pos & r->mask));
            const uint32_t size = h->size;
            if (const auto run = h->run) {
                r->running.store(run, std::memory_order_relaxed);
                run(h);
                bump(r->worker_entries);
            }
            pos += size;
            r->done.store(pos, std::memory_order_seq_cst);
            if (pos >= r->next_wake.load(std::memory_order_seq_cst)) notify(r);
        }
        if (r->stop.load(std::memory_order_acquire)) {
            if (r->published.load(std::memory_order_acquire) == pos) return;
            continue;
        }
        if (spin_for_work(r, pos, spin)) continue;
        // Sleep on the futex word. A waker exchanges it to 0 first, so a wake that comes before WaitOnAddress makes
        // it return at once; the read-modify-write of published orders this check after the store of sleeping.
        r->sleep_pos.store(pos, std::memory_order_relaxed);
        r->sleeping.store(1, std::memory_order_seq_cst);
        if (r->published.fetch_add(0, std::memory_order_seq_cst) == pos && !r->stop.load(std::memory_order_seq_cst)) {
            uint32_t asleep = 1;
            bump(r->sleeps);
            WaitOnAddress(&r->sleeping, &asleep, sizeof(asleep), kSleepMilliseconds);
        }
        r->sleeping.store(0, std::memory_order_relaxed);
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
    // The entry the worker started last, as an offset in its module: the linker map names the slot's lambda.
    const auto run = r->running.load(std::memory_order_relaxed);
    HMODULE module = nullptr;
    unsigned long long rva = 0;
    if (run && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(run)), &module) && module)
        rva = reinterpret_cast<uintptr_t>(run) - reinterpret_cast<uintptr_t>(module);
    log_line("replay: %s drain has waited %llu ms for ring %u (thread %lu): target %llu, done %llu, published %llu, "
             "last entry started at rva 0x%llx%s",
             kDrainNames[static_cast<size_t>(kind)], static_cast<unsigned long long>(microseconds(rp, ticks) / 1000),
             r->index, GetCurrentThreadId(), static_cast<unsigned long long>(target),
             static_cast<unsigned long long>(r->done.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(r->published.load(std::memory_order_relaxed)), rva,
             gone ? "; the worker thread has exited, the drain ends" : "");
}

// Waits until the ring's worker has passed target. Returns whether it had to wait, and the ticks in *ticks. Gives up
// only when the worker thread no longer exists (a process exit ends threads before it tears devices down).
bool wait_until(ReplayRing* r, uint64_t target, Drain kind, uint64_t* ticks) noexcept {
    *ticks = 0;
    if (r->done.load(std::memory_order_acquire) >= target) return false;
    const Replay* rp = r->replay;
    const uint64_t start = qpc();
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
    uint64_t line = start + rp->qpf;
    AcquireSRWLockExclusive(&r->lock);
    for (;;) {
        if (target < r->next_wake.load(std::memory_order_relaxed)) r->next_wake.store(target, std::memory_order_seq_cst);
        if (r->done.load(std::memory_order_seq_cst) >= target) break;
        replay_wake(r);
        SleepConditionVariableSRW(&r->cv, &r->lock, 2, 0);
        const uint64_t now = qpc();
        if (now < line) continue;
        ReleaseSRWLockExclusive(&r->lock);          // never log with the lock held
        const bool gone = WaitForSingleObject(r->thread, 0) == WAIT_OBJECT_0;
        stall_line(r, target, kind, now - start, gone);
        if (gone) {
            *ticks = now - start;
            return true;
        }
        line = now + rp->qpf;
        AcquireSRWLockExclusive(&r->lock);
    }
    ReleaseSRWLockExclusive(&r->lock);
    *ticks = qpc() - start;
    return true;
}

void count_drain(Replay* rp, Drain kind, bool waited, uint64_t ticks) noexcept {
    ReplayDrainCounter& c = rp->drains[static_cast<size_t>(kind)];
    c.calls.fetch_add(1, std::memory_order_relaxed);
    if (!waited) return;
    c.waits.fetch_add(1, std::memory_order_relaxed);
    c.qpc.fetch_add(ticks, std::memory_order_relaxed);
}

// Two lines: what the rings carried, then each drain kind as calls/waits/microseconds waited.
void summary(Replay* rp, const char* why) noexcept {
    const uint32_t n = rp->count.load(std::memory_order_acquire);
    uint64_t entries = 0, bytes = 0, run = 0, wakes = 0, sleeps = 0, notifies = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const ReplayRing* r = rp->rings[i];
        entries += r->entries.load(std::memory_order_relaxed);
        bytes += r->entry_bytes.load(std::memory_order_relaxed);
        run += r->worker_entries.load(std::memory_order_relaxed);
        wakes += r->wakes.load(std::memory_order_relaxed);
        sleeps += r->sleeps.load(std::memory_order_relaxed);
        notifies += r->notifies.load(std::memory_order_relaxed);
    }
    log_line("replay %s: %u rings, %llu entries (%llu KiB, %llu run), oversize %llu, fallbacks %llu, direct lookups %llu, "
             "reclaimed %llu, ring failures %llu, stalls %llu, wakes %llu, sleeps %llu, notifies %llu",
             why, n, static_cast<unsigned long long>(entries), static_cast<unsigned long long>(bytes >> 10),
             static_cast<unsigned long long>(run), static_cast<unsigned long long>(rp->oversize.load()),
             static_cast<unsigned long long>(rp->fallbacks.load()), static_cast<unsigned long long>(rp->direct_lookups.load()),
             static_cast<unsigned long long>(rp->reclaimed.load()), static_cast<unsigned long long>(rp->ring_failures.load()),
             static_cast<unsigned long long>(rp->stalls.load()), static_cast<unsigned long long>(wakes),
             static_cast<unsigned long long>(sleeps), static_cast<unsigned long long>(notifies));
    char text[400] = {};
    size_t used = 0;
    for (size_t k = 0; k < static_cast<size_t>(Drain::Count) && used < sizeof(text); ++k) {
        const ReplayDrainCounter& c = rp->drains[k];
        const uint64_t calls = c.calls.load(std::memory_order_relaxed);
        if (!calls) continue;
        const int written = std::snprintf(text + used, sizeof(text) - used, " %s %llu/%llu/%llu", kDrainNames[k],
                                          static_cast<unsigned long long>(calls),
                                          static_cast<unsigned long long>(c.waits.load(std::memory_order_relaxed)),
                                          static_cast<unsigned long long>(microseconds(rp, c.qpc.load(std::memory_order_relaxed))));
        if (written < 0) break;
        used += static_cast<size_t>(written);
    }
    log_line("replay %s drains (calls/waits/us):%s", why, text);
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
    r->owner = owner;
    r->owner_id = GetCurrentThreadId();
    r->thread = CreateThread(nullptr, 0, worker_thread, r, 0, nullptr);
    if (!r->thread) {
        r->~ReplayRing();
        VirtualFree(memory, 0, MEM_RELEASE);
        CloseHandle(owner);
        return nullptr;
    }
    SetThreadDescription(r->thread, L"amdgpu_wddm replay");
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
    if (created) log_line("replay: ring %u of %u for thread %lu", n, rp->policy.rings, self);
    if (reclaimed && found) {
        rp->reclaimed.fetch_add(1, std::memory_order_relaxed);
        log_line("replay: ring %u taken over by thread %lu", found->index, self);
    }
    if (!found) {
        const uint64_t lookups = rp->direct_lookups.fetch_add(1, std::memory_order_relaxed) + 1;
        if (power_of_two(lookups)) log_line("replay: thread %lu records directly (%llu such lookups)", self,
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
        log_line("replay: a call made directly (%s, %llu so far)", oversize ? "entry over the size limit" : "no snapshot heap",
                 static_cast<unsigned long long>(n));
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
        log_line("replay: ring %u has no snapshot heap of type %d (%08lx); calls that need one are made directly",
                 r->index, static_cast<int>(type), static_cast<unsigned long>(hr));
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
    if (kind != Drain::Close) return;
    const uint64_t closes = rp->drains[static_cast<size_t>(Drain::Close)].calls.load(std::memory_order_relaxed);
    if (closes >= 64 && power_of_two(closes)) {
        char why[32];
        std::snprintf(why, sizeof(why), "at close %llu", static_cast<unsigned long long>(closes));
        summary(rp, why);
    }
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
            log_line("replay: the worker of ring %u has not exited after %u s", i, s);
    }
    summary(rp, "teardown");
    c->replay = nullptr;
    for (uint32_t i = 0; i < n; ++i) free_ring(rp->rings[i]);
    delete rp;
    log_line("replay policy: off");
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
    c->replay = rp;
    log_line("replay policy: on, up to %u rings of %u KiB, worker spin %u us, wake batch %llu bytes", policy->rings,
             policy->ring_bytes >> 10, kSpinMicroseconds, static_cast<unsigned long long>(kReplayWakeBytes));
    return S_OK;
}

} // namespace engine_ddi
