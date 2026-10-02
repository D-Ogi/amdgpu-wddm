// SPDX-License-Identifier: MIT
// engine-ddi: deferred command-list replay (set_replay_policy, engine-ddi.h). Off (the default), every recording
// slot calls the engine list itself, as before the policy existed. On, a slot still validates, translates and
// reports on the calling thread, and then, instead of the engine call, writes an entry into the ring of the calling
// thread: the engine list, a copy of the call (a lambda that captures values only) and a copy of every array and
// descriptor the engine reads through a pointer. The ring's worker thread makes the engine calls in order.
//
// Topology: one ring per (device, recording thread), each with one worker thread, up to the policy's ring cap; a
// thread beyond the cap records directly. A list's entries are a subsequence of its thread's ring, so the order
// within a list is the thread's program order; a list recorded by another thread later first waits for its entries
// on the old ring (Drain::Switch). A ring is one fixed region: no allocation per call, at most rings x ring_bytes
// per device, the space recycled as the worker advances.
//
// Close is itself an entry, the list's last (close_list). Drains make published entries replay before the caller
// continues: a list's entries (drain_list) before its Reset, ExecuteBundle, ExecuteCommandLists and destroy, a direct
// call, or a move to another ring; a pool's last Close entry on each ring (drain_closes) before the pool's reset and a
// Reset into it, since the engine Close reaches the allocator; every ring's (drain_all) before the destroy of any
// object an entry can name, a command pool's destroy (or its reset while a list of it is open), and
// SetPipelineStackSize, whose value the engine reads at record time. A drain waits as long as it takes (a line every
// second names what it waits for), and gives up only when the worker thread no longer exists.
//
// Priority: a worker runs at its owner thread's level. A drain that waits past its spin lifts the worker one level
// above the waiting thread until the last such wait ends: the waiter is blocked on the worker's calls, which would
// otherwise compete at the waiter's own level (or below) with every other thread of the process.
//
// Ownership, the rules the code keeps:
//   R1 Published entries are immutable; the producer writes only free space.
//   R2 An engine list is touched by one thread at a time; the hand-overs are a publish (to the worker) and a drain
//      (back to the caller). The one exception is QueryInterface for List1 and later at encode, which only
//      increments the list's reference count atomically (vkd3d-proton, d3d12_command_list_QueryInterface).
//   R3 An engine object named by a pending entry is alive: every destroy that can free one drains first.
//   R4 The worker makes no context (HCONTEXT) operation and no error, table or removal callback: the shell's worker
//      hook runs it in a scope that admits only the runtime callbacks recording needs (hosted-dispatch.h).
//   R5 A lambda calls engine methods only: no hook, no engine-ddi lock, no record write. The one exception is the
//      Close entry, which stores a failure into its list's close_hr (atomic; a drain of the list reads it).
//   R6 No engine-ddi lock is held while a drain waits; the worker takes no lock but its ring's (a leaf).
// Fields, writer -> readers:
//   DeviceContext::replay: set_replay_policy, before the device is used on another thread and at teardown -> every
//     slot. Replay::rings and count: the thread lookup under Replay::lock (count stored with release) -> drains.
//   ReplayRing producer fields (write, cached_done, snapshots, tag, owner): the owner thread; a ring passes to
//     another thread only under Replay::lock, after its owner thread has exited.
//   Ring bytes in [done, published): immutable, read by the worker. published: the producer (release). done: the
//     worker (seq_cst, after the entry's engine call returned). sleeping: the worker; wakers clear it by exchange.
//     next_wake: waiters under the ring's lock, the worker reads it lock-free. base_priority, applied_priority,
//     boosters, stuck_longest, stuck_run: under the ring's lock (the owner at creation and take-over, waiters
//     around and during a slow wait).
//   CommandListRecord::replay_tail: the thread recording the list, or a drain of the list; other threads read it
//     after the application's own ordering (a list is not free-threaded). CommandListRecord::close_hr: the worker
//     (a failed Close), cleared by the drain that reports it. CommandPoolRecord::closing: close_list, read by the
//     pool's reset and by Reset after the application's ordering (an allocator is not free-threaded either).
#pragma once
#include "entry.h"
#include "internal.h"
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace engine_ddi {

// CommandListRecord::replay_tail: ((ring index + 1) << 56) | the ring position where the list's last entry ended.
inline constexpr uint64_t kReplayPositionMask = (uint64_t{1} << 56) - 1;

enum class Drain : uint32_t {
    Reset, Bundle, Ecl, Switch, Direct, Destroy, Pool, Closes, Stack, Space, Slots, Teardown, Count
};

// The start of every entry. run null: a skip to the end of the ring. size: the entry's bytes, a multiple of 16.
struct EntryHeader {
    void (*run)(const EntryHeader* entry) noexcept;
    uint32_t size;
    uint32_t reserved;
};
static_assert(sizeof(EntryHeader) == 16, "entry header");

// The argument kinds of record(). Each is copied into the entry at encode and passed to the lambda as a pointer to
// the copy; a null source stays null, a non-null source with count 0 stays non-null.
template <class T> struct In {                  // count elements; the lambda gets const T*
    const T* data;
    size_t count;
};
template <class T> In<T> in(const T* data, size_t count) noexcept { return {data, count}; }
template <class T> struct Gather {              // count pointers to elements (ARRAY_OF_POINTERS); const T* const*
    const T* const* data;
    size_t count;
};
// CPU descriptors the engine reads at record time (OMSetRenderTargets, the clears): copied, on the calling thread,
// into slots of the ring's own non-shader-visible heap of that type; the lambda gets handles of the copies, with the
// call's shape kept (one handle for a range, else one per descriptor). An entry holds at most one Snap of each heap
// type (OMSetRenderTargets: one RTV and one DSV): replay_snapshot_ready makes room for each Snap's count alone.
struct Snap {
    const D3D12_CPU_DESCRIPTOR_HANDLE* handles;
    UINT count;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    BOOL range;
};

// A ring's copies of CPU descriptors of one type: FIFO slots, each free again once the worker has passed the entry
// that named it (free_after). Producer-owned.
struct SnapshotHeap {
    ID3D12DescriptorHeap* heap;                 // engine heap, one reference; null until first use
    D3D12_CPU_DESCRIPTOR_HANDLE base;
    UINT increment;
    uint32_t capacity;
    uint32_t next;
    bool failed;                                // its creation failed: calls that need it are made directly
    uint64_t* free_after;                       // per slot: the ring position the worker must pass first
};

struct Replay;

// C4324: the shared fields sit on cache lines of their own on purpose.
#pragma warning(push)
#pragma warning(disable : 4324)
struct ReplayRing {
    // Fixed at creation.
    Replay* replay;
    uint8_t* bytes;
    uint64_t size;                              // a power of two
    uint64_t mask;
    uint64_t max_entry;                         // size / 8; a larger entry is made directly
    uint64_t tag;                               // (index + 1) << 56
    uint32_t index;
    HANDLE thread;
    WorkerStats* stats;                         // the worker's times (entry.h), null with the statistics off
    // The worker's priority (ring lock): the owner's level, the level set now, and the waits that lifted it.
    int base_priority;
    int applied_priority;
    uint32_t boosters;
    // The longest stretch a slow wait saw without the worker finishing an entry (QPC ticks), and the entry it was
    // running then (ring lock). The waiters measure it between their 2 ms slices, so it is a lower bound, short by up
    // to one slice as the timer rounds it (a 15.6 ms tick unless the process raised the timer resolution); the
    // worker pays nothing for it.
    uint64_t stuck_longest;
    void (*stuck_run)(const EntryHeader*) noexcept;
    // The producer: the owner thread, or under Replay::lock the thread that takes the ring over.
    uint64_t write;                             // the end of the last reserved entry
    uint64_t cached_done;                       // a value of done read earlier: done is at least this
    HANDLE owner;                               // SYNCHRONIZE handle of the owner thread, signalled once it exits
    DWORD owner_id;
    SnapshotHeap snapshots[4];                  // by D3D12_DESCRIPTOR_HEAP_TYPE; none for samplers
    std::atomic<uint64_t> entries{0};           // single writer, read by the summary lines
    std::atomic<uint64_t> entry_bytes{0};
    // The producer publishes, the worker reads.
    alignas(64) std::atomic<uint64_t> published{0};
    // The worker.
    alignas(64) std::atomic<uint64_t> done{0};
    std::atomic<void (*)(const EntryHeader*) noexcept> running{nullptr};   // the last entry started: stall lines
    std::atomic<uint64_t> worker_entries{0};
    std::atomic<uint64_t> sleeps{0};
    std::atomic<uint64_t> notifies{0};
    std::atomic<bool> stop{false};
    // The worker's futex word: 1 while it sleeps or is about to; a waker exchanges it to 0 and wakes it.
    alignas(64) std::atomic<uint32_t> sleeping{0};
    std::atomic<uint64_t> wakes{0};
    // Waiters (drains): the smallest target among them, and where they sleep.
    alignas(64) std::atomic<uint64_t> next_wake{UINT64_MAX};
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
};
#pragma warning(pop)

struct ReplayDrainCounter {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> waits{0};
    std::atomic<uint64_t> qpc{0};               // QueryPerformanceCounter ticks spent waiting
    std::atomic<uint64_t> over_1ms{0};          // waits of 1 ms or more (a drain_all: its rings' waits together)
    std::atomic<uint64_t> over_10ms{0};
    std::atomic<uint64_t> over_50ms{0};
    std::atomic<uint64_t> longest{0};           // QPC ticks
};

struct Replay {
    DeviceContext* context = nullptr;
    ReplayPolicy policy{};
    uint64_t serial = 0;                        // identity for the thread caches, never reused
    uint64_t qpf = 1;                           // QueryPerformanceFrequency
    uint64_t spin_ticks = 0;                    // the worker's spin before it yields
    uint64_t yield_ticks = 0;                   // then its yielding before it sleeps
    uint64_t wake_bytes = 0;                    // a publish wakes a sleeping worker once this much is pending
    uint64_t qpc0 = 0, tsc0 = 0;                // QPC and TSC when the policy went on: thread cycles to time
    SRWLOCK lock = SRWLOCK_INIT;                // the thread lookup: rings, count, ring owners
    ReplayRing* rings[kMaxReplayRings]{};
    std::atomic<uint32_t> count{0};
    ReplayDrainCounter drains[static_cast<size_t>(Drain::Count)];
    std::atomic<uint64_t> oversize{0};          // entries over max_entry, made directly
    std::atomic<uint64_t> fallbacks{0};         // calls made directly because a snapshot heap was missing
    std::atomic<uint64_t> direct_lookups{0};    // thread lookups that found no ring: that thread records directly
    std::atomic<uint64_t> reclaimed{0};         // rings taken over from an exited thread
    std::atomic<uint64_t> stalls{0};            // stall lines
    std::atomic<uint64_t> ring_failures{0};     // rings that could not be created
    std::atomic<uint64_t> asleep_waits{0};      // slow waits that found the worker asleep with work behind it
    std::atomic<uint64_t> boosts{0};            // worker priority lifts by a waiting drain
    std::atomic<uint64_t> long_waits{0};        // ring waits of kLongWaitMilliseconds or more
    std::atomic<uint64_t> next_summary{UINT64_MAX};   // QPC of the next timed summary, after the one at close 64
};

// The calling thread's ring for one Replay: serial names the Replay (0: none), ring is null for a direct thread.
struct ReplayThreadCache {
    uint64_t serial;
    ReplayRing* ring;
};
inline thread_local ReplayThreadCache t_replay{};

// replay.cpp.
ReplayRing* replay_thread_ring(Replay* replay) noexcept;               // the slow path of replay_ring
void replay_switch(Replay* replay, CommandListRecord* list) noexcept;  // Drain::Switch of the list's old ring
void replay_make_room(ReplayRing* ring, uint64_t until) noexcept;      // Drain::Space: waits until done >= until
void replay_wake(ReplayRing* ring) noexcept;
bool replay_fallback(ReplayRing* ring, CommandListRecord* list, bool oversize) noexcept;   // drains; false
bool replay_snapshot_ready(ReplayRing* ring, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count) noexcept;
void replay_snapshot(ReplayRing* ring, const Snap& snap, D3D12_CPU_DESCRIPTOR_HANDLE* out, uint64_t end) noexcept;
void replay_drain_list(Replay* replay, CommandListRecord* list, Drain kind) noexcept;
void replay_drain_closes(Replay* replay, const std::atomic<uint64_t>* closing) noexcept;
void replay_drain_all(Replay* replay, Drain kind) noexcept;
// Teardown: drains every ring, stops and joins every worker, frees everything; the context's replay is null after.
void replay_off(DeviceContext* context) noexcept;

inline ReplayRing* replay_ring(Replay* replay) noexcept {
    const ReplayThreadCache& cache = t_replay;
    return cache.serial == replay->serial ? cache.ring : replay_thread_ring(replay);
}

// The list's entries replay before the caller goes on (a no-op with the policy off).
inline void drain_list(CommandListRecord* list, Drain kind) noexcept {
    if (Replay* replay = list->h.device->replay) replay_drain_list(replay, list, kind);
}
// A pool's last Close entry on each ring (CommandPoolRecord::closing) has replayed.
inline void drain_closes(DeviceContext* context, const std::atomic<uint64_t>* closing) noexcept {
    if (Replay* replay = context->replay) replay_drain_closes(replay, closing);
}
// Every entry published so far, on every ring of the device.
inline void drain_all(DeviceContext* context, Drain kind) noexcept {
    if (Replay* replay = context->replay) replay_drain_all(replay, kind);
}
// Wakes the worker of the ring that holds an entry ending at tail (a list's replay_tail), if it sleeps.
inline void wake_for(Replay* replay, uint64_t tail) noexcept {
    ReplayRing* r = replay->rings[(tail >> 56) - 1];
    if (r->sleeping.load(std::memory_order_relaxed)) replay_wake(r);
}

namespace replay_detail {
constexpr size_t align_up(size_t value, size_t alignment) noexcept { return (value + alignment - 1) & ~(alignment - 1); }

// Entry layout: header, engine list, the lambda, one offset per argument (from the entry's start, 0 for null), then
// the arguments' copies.
template <class F, size_t N> struct Layout {
    static constexpr size_t kList = sizeof(EntryHeader);
    static constexpr size_t kF = align_up(kList + sizeof(void*), alignof(F));
    static constexpr size_t kOffsets = align_up(kF + sizeof(F), alignof(uint32_t));
    static constexpr size_t kFixed = kOffsets + sizeof(uint32_t) * N;
};

template <class A> struct Arg;
template <class T> struct Arg<In<T>> {
    using type = const T*;
    static constexpr size_t kAlign = alignof(T) > 8 ? alignof(T) : 8;
    static bool bytes(const In<T>& a, size_t limit, size_t* out) noexcept {
        if (a.count > limit / sizeof(T)) return false;
        *out = a.count * sizeof(T);
        return true;
    }
    static bool ready(ReplayRing*, const In<T>&) noexcept { return true; }
    static bool present(const In<T>& a) noexcept { return a.data != nullptr; }
    static type source(const In<T>& a) noexcept { return a.data; }
    static void put(ReplayRing*, uint8_t* dst, const In<T>& a, uint64_t) noexcept {
        if (a.count) std::memcpy(dst, a.data, a.count * sizeof(T));
    }
    static type at(const uint8_t* entry, uint32_t offset) noexcept {
        return offset ? reinterpret_cast<const T*>(entry + offset) : nullptr;
    }
};
// The pointers first (into this entry, so valid while it is pending), then the elements they point to.
template <class T> struct Arg<Gather<T>> {
    using type = const T* const*;
    static constexpr size_t kAlign = alignof(T) > 8 ? alignof(T) : 8;
    static size_t items(size_t count) noexcept { return align_up(count * sizeof(const T*), kAlign); }
    static bool bytes(const Gather<T>& a, size_t limit, size_t* out) noexcept {
        if (a.count > limit / (sizeof(const T*) + sizeof(T) + kAlign)) return false;
        *out = items(a.count) + a.count * sizeof(T);
        return true;
    }
    static bool ready(ReplayRing*, const Gather<T>&) noexcept { return true; }
    static bool present(const Gather<T>& a) noexcept { return a.data != nullptr; }
    static type source(const Gather<T>& a) noexcept { return a.data; }
    static void put(ReplayRing*, uint8_t* dst, const Gather<T>& a, uint64_t) noexcept {
        uint8_t* item = dst + items(a.count);
        for (size_t i = 0; i < a.count; ++i, item += sizeof(T)) {
            const T* p = nullptr;
            if (a.data[i]) {
                std::memcpy(item, a.data[i], sizeof(T));
                p = reinterpret_cast<const T*>(item);
            }
            std::memcpy(dst + i * sizeof(p), &p, sizeof(p));
        }
    }
    static type at(const uint8_t* entry, uint32_t offset) noexcept {
        return offset ? reinterpret_cast<const T* const*>(entry + offset) : nullptr;
    }
};
template <> struct Arg<Snap> {
    using type = const D3D12_CPU_DESCRIPTOR_HANDLE*;
    static constexpr size_t kAlign = 8;
    static size_t handles(const Snap& s) noexcept { return s.range ? (s.count ? 1u : 0u) : s.count; }
    static bool bytes(const Snap& s, size_t limit, size_t* out) noexcept {
        if (handles(s) > limit / sizeof(D3D12_CPU_DESCRIPTOR_HANDLE)) return false;
        *out = handles(s) * sizeof(D3D12_CPU_DESCRIPTOR_HANDLE);
        return true;
    }
    static bool ready(ReplayRing* r, const Snap& s) noexcept {
        return !s.handles || !s.count || replay_snapshot_ready(r, s.type, s.count);
    }
    static bool present(const Snap& s) noexcept { return s.handles != nullptr; }
    static type source(const Snap& s) noexcept { return s.handles; }
    static void put(ReplayRing* r, uint8_t* dst, const Snap& s, uint64_t end) noexcept {
        if (s.count) replay_snapshot(r, s, reinterpret_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(dst), end);
    }
    static type at(const uint8_t* entry, uint32_t offset) noexcept {
        return offset ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(entry + offset) : nullptr;
    }
};

template <size_t N> struct Plan {
    size_t total;
    uint32_t offsets[N + 1];
    bool oversize;                              // over max_entry
    bool unready;                               // a snapshot heap is missing
};
template <class A, size_t N> void plan(Plan<N>& p, size_t i, ReplayRing* r, const A& a) noexcept {
    size_t bytes = 0;
    if (p.oversize || p.unready) return;
    if (!Arg<A>::bytes(a, r->max_entry, &bytes)) {
        p.oversize = true;
        return;
    }
    if (!Arg<A>::ready(r, a)) {
        p.unready = true;
        return;
    }
    if (!Arg<A>::present(a)) return;
    p.total = align_up(p.total, Arg<A>::kAlign);
    p.offsets[i] = static_cast<uint32_t>(p.total);
    p.total += bytes;
    if (p.total > r->max_entry) p.oversize = true;
}

template <class F, class... A, size_t... I> void run(const uint8_t* entry, std::index_sequence<I...>) noexcept {
    using L = Layout<F, sizeof...(A)>;
    ID3D12GraphicsCommandList* list = nullptr;
    std::memcpy(&list, entry + L::kList, sizeof(list));
    [[maybe_unused]] uint32_t offsets[sizeof...(A) + 1]{};
    std::memcpy(offsets, entry + L::kOffsets, sizeof(uint32_t) * sizeof...(A));
    const F& f = *std::launder(reinterpret_cast<const F*>(entry + L::kF));
    f(list, Arg<A>::at(entry, offsets[I])...);
}
template <class F, class... A> void run_entry(const EntryHeader* entry) noexcept {
    run<F, A...>(reinterpret_cast<const uint8_t*>(entry), std::index_sequence_for<A...>{});
}

// The calling thread owns r. Room for the entry (and a skip to the ring's end before it, when it would straddle
// the end), waiting for the worker when the ring is full; returns where the entry starts, write is its end.
inline uint8_t* reserve(ReplayRing* r, size_t size) noexcept {
    uint64_t pos = r->write;
    const uint64_t index = pos & r->mask;
    const uint64_t skip = index + size > r->size ? r->size - index : 0;
    const uint64_t end = pos + skip + size;
    if (end - r->cached_done > r->size) {
        r->cached_done = r->done.load(std::memory_order_acquire);
        if (end - r->cached_done > r->size) replay_make_room(r, end - r->size);
    }
    if (skip) {
        const EntryHeader h{nullptr, static_cast<uint32_t>(skip), 0};
        std::memcpy(r->bytes + index, &h, sizeof(h));
        pos += skip;
    }
    r->write = end;
    return r->bytes + (pos & r->mask);
}

inline void publish(ReplayRing* r, CommandListRecord* l, uint64_t end, size_t size) noexcept {
    // seq_cst store and load (an xchg here): the load of sleeping below must not pass the store, or a worker that
    // stores sleeping and then reads published (seq_cst both) between them misses this entry while this publish
    // misses its sleep, and the entry waits for the next publish, a drain or the worker's 100 ms timeout.
    r->published.store(end, std::memory_order_seq_cst);
    l->replay_tail.store(r->tag | end, std::memory_order_relaxed);
    r->entries.store(r->entries.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    r->entry_bytes.store(r->entry_bytes.load(std::memory_order_relaxed) + size, std::memory_order_relaxed);
    // A sleeping worker is woken once wake_bytes are pending, not by the first publish: a game records a few calls,
    // runs its own code longer than the worker's spin, records a few more, and a wake per such burst cost the
    // recording thread a system call each (304: 1.2 ms per frame of the main thread in WakeByAddressSingle).
    // While it sleeps the worker writes nothing, so done is the position it caught up to. Entries below the bound
    // wait at most until the next Close (it wakes the worker), the next drain, the next publish past the bound or the
    // worker's own timeout: latency, never an entry.
    // The load of sleeping is seq_cst against the seq_cst store above. A relaxed load may pass that store. A worker
    // that stores sleeping and then reads the old published between the two misses this entry, while this publish
    // misses the sleep.
    if (r->sleeping.load(std::memory_order_seq_cst) &&
        end - r->done.load(std::memory_order_relaxed) >= r->replay->wake_bytes)
        replay_wake(r);
}

// Direct: the direct entry's encode, which leaves an entry it cannot write to the slot (no fallback drain here; the
// slot's own record makes it).
template <bool Direct = false, class F, class... A, size_t... I>
bool encode(ReplayRing* r, CommandListRecord* l, const F& f, std::index_sequence<I...>, const A&... args) noexcept {
    using L = Layout<F, sizeof...(A)>;
    Plan<sizeof...(A)> p{L::kFixed, {}, false, false};
    (plan(p, I, r, args), ...);
    if (p.oversize || p.unready) return Direct ? false : replay_fallback(r, l, p.oversize);
    const size_t size = align_up(p.total, 16);
    uint8_t* e = reserve(r, size);
    const uint64_t end = r->write;
    const EntryHeader h{&run_entry<F, A...>, static_cast<uint32_t>(size), 0};
    std::memcpy(e, &h, sizeof(h));
    ID3D12GraphicsCommandList* list = l->list();
    std::memcpy(e + L::kList, &list, sizeof(list));
    new (e + L::kF) F(f);
    std::memcpy(e + L::kOffsets, p.offsets, sizeof(uint32_t) * sizeof...(A));
    ((p.offsets[I] ? Arg<A>::put(r, e + p.offsets[I], args, end) : void()), ...);
    publish(r, l, end, size);
    return true;
}
} // namespace replay_detail

// A recording slot's engine call. f(list, args...) receives the engine list and, for each argument, a pointer to
// its data: the caller's own with the policy off or on a thread without a ring, the entry's copy on the worker.
// f captures values only (scalars, engine pointers resolved on the calling thread, translated structures).
template <class F, class... A> void record(CommandListRecord* l, const F& f, const A&... args) noexcept {
    static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F> && alignof(F) <= 16,
                  "a replay lambda captures plain values only");
    if (Replay* replay = l->h.device->replay) {
        ReplayRing* r = replay_ring(replay);
        const uint64_t tail = l->replay_tail.load(std::memory_order_relaxed);
        if (tail && (tail & ~kReplayPositionMask) != (r ? r->tag : 0)) replay_switch(replay, l);
        if (r && replay_detail::encode(r, l, f, std::index_sequence_for<A...>{}, args...)) return;
    }
    f(l->list(), replay_detail::Arg<A>::source(args)...);
}

// The direct entry (engine-ddi.h, "Entry path"): record()'s deferred half for a call that comes straight from the
// runtime's table, without the shell's entry. It writes the same entry record() would write (same f, same arguments),
// and only when that needs nothing but this thread's ring: a live list of a context the shell admits (set_direct_entry,
// published with the shell's recording binding and cleared with it), deferred replay on, this thread's ring already
// looked up, the list's pending entries (if any) on that ring, an entry that fits. It makes no engine call, takes no
// lock, waits only for ring space as record() does (Drain::Space, which calls no hook), and changes nothing when it
// returns false: the caller then goes to the slot, which validates, reports and records as before.
template <class F, class... A>
__forceinline bool record_direct(EntryTimer& timer, D3D12DDI_HCOMMANDLIST h, const F& f, const A&... args) noexcept {
    static_assert(std::is_trivially_copyable_v<F> && std::is_trivially_destructible_v<F> && alignof(F) <= 16,
                  "a replay lambda captures plain values only");
    auto* l = record_of<CommandListRecord>(h.pDrvPrivate, Tag::CommandList);
    if (!l) {
        timer.miss(DirectMiss::Record);
        return false;
    }
    DeviceContext* c = l->h.device;
    if (!c->direct.load(std::memory_order_relaxed)) {
        timer.miss(DirectMiss::Admission);
        return false;
    }
    Replay* replay = c->replay;
    if (!replay) {
        timer.miss(DirectMiss::Replay);
        return false;
    }
    const ReplayThreadCache& cache = t_replay;
    if (cache.serial != replay->serial || !cache.ring) {
        timer.miss(DirectMiss::Ring);
        return false;
    }
    ReplayRing* r = cache.ring;
    const uint64_t tail = l->replay_tail.load(std::memory_order_relaxed);
    if (tail && (tail & ~kReplayPositionMask) != r->tag) {
        timer.miss(DirectMiss::Switch);
        return false;
    }
    if (!replay_detail::encode<true>(r, l, f, std::index_sequence_for<A...>{}, args...)) {
        timer.miss(DirectMiss::Encode);
        return false;
    }
    timer.direct();
    return true;
}

} // namespace engine_ddi
