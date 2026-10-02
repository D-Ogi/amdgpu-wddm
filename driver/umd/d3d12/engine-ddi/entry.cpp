// SPDX-License-Identifier: MIT
// engine-ddi: the entry path experiment's knobs, counting blocks and rows (entry.h; engine-ddi.h, "Entry path").
//
// Rows. The first thread that calls entry_frame (the shell's Present) is the presenter; it alone keeps the frame
// clock and the phase. A phase lasts phase_ms; at the first Present past its end the presenter closes it: a row with
// the arm, the phase's Present-to-Present intervals (the first two after an arm change are left out, the arm's effect
// still settling), and the deltas since the last row of every thread block and worker block. Then the next arm takes
// over. Rows are buffered and written every 5 s (and when the buffer fills), from the presenter, in one WriteFile.
//
// Row: "entry row=N arm=X utc=hh:mm:ss.mmm qpc=Q tsc=T dq=dQ dt=dT frames=F skipped=S ft=SUM,SQUARES,MAX" then, per
// thread that made at least 5 % of the phase's timed calls, " | T<tid>[p]" (p: the presenter) followed by
// " <class>=calls/samples/ticks" for each class it called and " dir=direct/record,admission,replay,ring,switch,encode",
// the other threads folded into " | rest", and " | W" with " r<ring>.<serial>=busy/spin/yield/sleep/entries" per
// worker that ran. ft is in QPC ticks, every time in TSC ticks (dt/dq gives TSC per QPC tick); calls count every call,
// samples and ticks the timed ones (about one in sixteen): ticks/samples is the mean time of a call.
#include "entry.h"
#include "internal.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace engine_ddi {
namespace {
constexpr uint32_t kMaxThreads = 128;           // counting blocks reported; a thread beyond them counts unreported
constexpr uint32_t kMaxWorkers = 64;
constexpr uint32_t kMaxArms = 8;
constexpr uint32_t kSkippedPerPhase = 2;        // intervals left out after an arm change
constexpr uint64_t kFlushMilliseconds = 5000;
constexpr size_t kRowBytes = 16384;             // one row at most
constexpr size_t kBufferBytes = 128 * 1024;

const char* const kClassNames[kEntryClasses] = {
#define ENGINE_DDI_ENTRY_NAME(name) #name,
    ENGINE_DDI_ENTRY_CLASSES(ENGINE_DDI_ENTRY_NAME)
#undef ENGINE_DDI_ENTRY_NAME
};

struct Knobs {
    char path[kMaxArms + 1]{};                  // the arm letters as given (valid ones only)
    EntryArm order[kMaxArms]{};
    uint32_t arms = 0;
    bool stats = false;
    bool active = false;                        // path or stats
    uint32_t phase_ms = 2000;
    uint32_t pad_us = 300;
    uint32_t poll_us = 1;
    uint64_t qpf = 1;
    double tsc_per_qpc = 0;
    const char* source = "none";
};
Knobs g_knobs;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

uint64_t qpc() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

// The knob file as the ICD reads it: BC250_DEFERRED_CFG names it, else C:\BC250\tmp\amdgpu_wddm_radv.cfg.
char* read_knob_file() noexcept {
    char path[MAX_PATH] = "C:\\BC250\\tmp\\amdgpu_wddm_radv.cfg";
    char named[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("BC250_DEFERRED_CFG", named, sizeof(named));
    if (n && n < sizeof(named)) std::memcpy(path, named, n + 1);
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") || !file) return nullptr;
    auto* text = static_cast<char*>(std::calloc(1, 65537));
    if (text) text[std::fread(text, 1, 65536, file)] = 0;
    std::fclose(file);
    return text;
}

// key's value: the environment, else the file's last "key=value" line (a '#' line is a comment). Empty if none.
bool knob(const char* file, const char* key, char* out, size_t size, bool* from_file) noexcept {
    out[0] = 0;
    const DWORD n = GetEnvironmentVariableA(key, out, static_cast<DWORD>(size));
    if (n && n < size) return true;
    out[0] = 0;
    if (!file) return false;
    const size_t key_length = std::strlen(key);
    bool found = false;
    for (const char* line = file; *line;) {
        const char* end = line;
        while (*end && *end != '\n' && *end != '\r') ++end;
        if (static_cast<size_t>(end - line) > key_length && !std::strncmp(line, key, key_length) &&
            line[key_length] == '=') {
            const size_t length = static_cast<size_t>(end - line) - key_length - 1;
            if (length < size) {
                std::memcpy(out, line + key_length + 1, length);
                out[length] = 0;
                found = true;
                *from_file = true;
            }
        }
        line = end;
        while (*line == '\n' || *line == '\r') ++line;
    }
    return found;
}

uint32_t knob_number(const char* file, const char* key, uint32_t fallback, uint32_t low, uint32_t high,
                     bool* from_file) noexcept {
    char value[32];
    if (!knob(file, key, value, sizeof(value), from_file) || !value[0]) return fallback;
    char* end = nullptr;
    const unsigned long n = std::strtoul(value, &end, 10);
    return (end && !*end && n >= low && n <= high) ? static_cast<uint32_t>(n) : fallback;
}

uint64_t poll_tsc() noexcept {
    return static_cast<uint64_t>(g_knobs.poll_us * g_knobs.tsc_per_qpc * static_cast<double>(g_knobs.qpf) / 1e6);
}

BOOL CALLBACK load_knobs(PINIT_ONCE, PVOID, PVOID*) {
    char* file = read_knob_file();
    bool from_file = false;
    char value[64];
    if (knob(file, "BC250_ENTRY_PATH", value, sizeof(value), &from_file)) {
        for (const char* p = value; *p && g_knobs.arms < kMaxArms; ++p) {
            if (*p < 'a' || *p > 'd') continue;
            g_knobs.path[g_knobs.arms] = *p;
            g_knobs.order[g_knobs.arms++] = static_cast<EntryArm>(*p - 'a');
        }
    }
    g_knobs.stats = knob(file, "BC250_ENTRY_STATS", value, sizeof(value), &from_file) && !std::strcmp(value, "1");
    g_knobs.phase_ms = knob_number(file, "BC250_ENTRY_PHASE_MS", 2000, 100, 600000, &from_file);
    g_knobs.pad_us = knob_number(file, "BC250_ENTRY_PAD_US", 300, 0, 100000, &from_file);
    g_knobs.poll_us = knob_number(file, "BC250_ENTRY_POLL_US", 1, 1, 1000, &from_file);
    std::free(file);
    if (!g_knobs.arms && g_knobs.stats) {       // statistics alone: arm a throughout
        g_knobs.path[0] = 'a';
        g_knobs.order[0] = EntryArm::Thunk;
        g_knobs.arms = 1;
    }
    g_knobs.active = g_knobs.arms != 0;
    g_knobs.source = from_file ? "file" : g_knobs.active ? "environment" : "none";
    if (!g_knobs.active) return TRUE;
    // TSC ticks per QPC tick, over 2 ms: the workers' poll interval of arm c is in TSC ticks.
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_knobs.qpf = static_cast<uint64_t>(frequency.QuadPart);
    const uint64_t q0 = qpc(), t0 = __rdtsc();
    uint64_t q1 = q0;
    while ((q1 = qpc()) - q0 < g_knobs.qpf / 500) _mm_pause();
    const uint64_t t1 = __rdtsc();
    g_knobs.tsc_per_qpc = static_cast<double>(t1 - t0) / static_cast<double>(q1 - q0);
    const EntryArm first = g_knobs.order[0];
    g_entry.arm.store(static_cast<uint32_t>(first), std::memory_order_relaxed);
    g_entry.poll_tsc.store(first == EntryArm::DirectPoll ? poll_tsc() : 0, std::memory_order_relaxed);
    g_entry.stats.store(g_knobs.stats, std::memory_order_release);
    log_line("entry path: arms %s every %u ms, statistics %s, pad %u us, poll %u us (%s); %.3f TSC ticks per QPC tick",
             g_knobs.path, g_knobs.phase_ms, g_knobs.stats ? "on" : "off", g_knobs.pad_us, g_knobs.poll_us,
             g_knobs.source, g_knobs.tsc_per_qpc);
    return TRUE;
}

// ---- Blocks -------------------------------------------------------------------------------------------------------
SRWLOCK g_blocks_lock = SRWLOCK_INIT;
EntryThread* g_threads[kMaxThreads]{};
std::atomic<uint32_t> g_thread_count{0};
WorkerStats* g_workers[kMaxWorkers]{};
std::atomic<uint32_t> g_worker_count{0};

// ---- Presenter state (the presenter thread alone) -----------------------------------------------------------------
struct ThreadCounts {
    uint64_t calls[kEntryClasses];
    uint64_t samples[kEntryClasses];
    uint64_t ticks[kEntryClasses];
    uint64_t direct;
    uint64_t misses[kDirectMisses];
};
struct WorkerCounts {
    uint64_t busy, spin, yield, sleep, entries;
};
struct Presenter {
    bool started = false;
    uint32_t phase = 0;                         // index into the arm order
    uint64_t row = 0;
    uint64_t phase_start = 0, last = 0, row_qpc = 0, row_tsc = 0, flushed = 0;
    uint32_t intervals = 0;                     // since the arm changed
    uint64_t frames = 0, skipped = 0, sum = 0, squares = 0, longest = 0;
    ThreadCounts threads[kMaxThreads];          // at the last row
    ThreadCounts now_counts[kMaxThreads];       // close_row's reading
    WorkerCounts workers[kMaxWorkers];
    char row_text[kRowBytes];
    char buffer[kBufferBytes];
    size_t used = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool opened = false;
};
std::atomic<DWORD> g_presenter{0};
Presenter* g_state = nullptr;

void open_log(Presenter& s) noexcept {
    s.opened = true;
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "C:\\BC250\\tmp\\amdgpu_wddm_radv-deferred-%lu-shell.log", GetCurrentProcessId());
    s.file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        char temp[MAX_PATH];
        const DWORD n = GetTempPathA(sizeof(temp), temp);
        if (n && n < sizeof(temp)) {
            std::snprintf(path, sizeof(path), "%samdgpu_wddm_radv-deferred-%lu-shell.log", temp, GetCurrentProcessId());
            s.file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    log_line("entry path: rows in %s", s.file == INVALID_HANDLE_VALUE ? "(none: the log could not be opened)" : path);
}

void flush(Presenter& s) noexcept {
    if (!s.used) return;
    if (!s.opened) open_log(s);
    if (s.file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(s.file, s.buffer, static_cast<DWORD>(s.used), &written, nullptr);
    }
    s.used = 0;
}

void put(Presenter& s, const char* text, size_t length) noexcept {
    if (s.used + length > sizeof(s.buffer)) flush(s);
    if (length > sizeof(s.buffer)) return;
    std::memcpy(s.buffer + s.used, text, length);
    s.used += length;
}

// Appends to the row; false once it is full (the row then ends there).
struct Row {
    char* text;
    size_t size, used = 0;
    bool full = false;
    void add(const char* format, ...) noexcept {
        if (full) return;
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(text + used, size - used, format, args);
        va_end(args);
        if (n < 0 || static_cast<size_t>(n) >= size - used - 2) {
            full = true;
            return;
        }
        used += static_cast<size_t>(n);
    }
};

void read_thread(const EntryThread& t, ThreadCounts& out) noexcept {
    for (uint32_t k = 0; k < kEntryClasses; ++k) {
        out.calls[k] = t.calls[k].load(std::memory_order_relaxed);
        out.samples[k] = t.samples[k].load(std::memory_order_relaxed);
        out.ticks[k] = t.ticks[k].load(std::memory_order_relaxed);
    }
    out.direct = t.direct.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < kDirectMisses; ++k) out.misses[k] = t.misses[k].load(std::memory_order_relaxed);
}

void add_counts(ThreadCounts& sum, const ThreadCounts& now, const ThreadCounts& before) noexcept {
    for (uint32_t k = 0; k < kEntryClasses; ++k) {
        sum.calls[k] += now.calls[k] - before.calls[k];
        sum.samples[k] += now.samples[k] - before.samples[k];
        sum.ticks[k] += now.ticks[k] - before.ticks[k];
    }
    sum.direct += now.direct - before.direct;
    for (uint32_t k = 0; k < kDirectMisses; ++k) sum.misses[k] += now.misses[k] - before.misses[k];
}

void add_group(Row& row, const char* head, const ThreadCounts& d) noexcept {
    row.add(" | %s", head);
    for (uint32_t k = 0; k < kEntryClasses; ++k)
        if (d.calls[k] || d.samples[k])
            row.add(" %s=%llu/%llu/%llu", kClassNames[k], static_cast<unsigned long long>(d.calls[k]),
                    static_cast<unsigned long long>(d.samples[k]), static_cast<unsigned long long>(d.ticks[k]));
    row.add(" dir=%llu/%llu,%llu,%llu,%llu,%llu,%llu", static_cast<unsigned long long>(d.direct),
            static_cast<unsigned long long>(d.misses[0]), static_cast<unsigned long long>(d.misses[1]),
            static_cast<unsigned long long>(d.misses[2]), static_cast<unsigned long long>(d.misses[3]),
            static_cast<unsigned long long>(d.misses[4]), static_cast<unsigned long long>(d.misses[5]));
}

void close_row(Presenter& s, uint64_t now, EntryArm arm) noexcept {
    const uint64_t tsc = __rdtsc();
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    Row row{s.row_text, sizeof(s.row_text)};
    row.add("entry row=%llu arm=%c utc=%02u:%02u:%02u.%03u qpc=%llu tsc=%llu dq=%llu dt=%llu frames=%llu skipped=%llu "
            "ft=%llu,%llu,%llu",
            static_cast<unsigned long long>(s.row), 'a' + static_cast<int>(arm), utc.wHour, utc.wMinute, utc.wSecond,
            utc.wMilliseconds, static_cast<unsigned long long>(now), static_cast<unsigned long long>(tsc),
            static_cast<unsigned long long>(now - s.row_qpc), static_cast<unsigned long long>(tsc - s.row_tsc),
            static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.skipped),
            static_cast<unsigned long long>(s.sum), static_cast<unsigned long long>(s.squares),
            static_cast<unsigned long long>(s.longest));
    // Thread deltas: those with at least 5 % of the timed calls on their own, the rest folded.
    const uint32_t n = g_thread_count.load(std::memory_order_acquire);
    const DWORD presenter = g_presenter.load(std::memory_order_relaxed);
    ThreadCounts* const now_counts = s.now_counts;
    uint64_t totals[kMaxThreads]{}, all = 0;
    for (uint32_t i = 0; i < n; ++i) {
        read_thread(*g_threads[i], now_counts[i]);
        for (uint32_t k = 0; k < kEntryClasses; ++k) totals[i] += now_counts[i].calls[k] - s.threads[i].calls[k];
        all += totals[i];
    }
    ThreadCounts rest{};
    bool any_rest = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (!totals[i]) continue;
        ThreadCounts d{};
        add_counts(d, now_counts[i], s.threads[i]);
        if (totals[i] * 20 >= all || g_threads[i]->tid == presenter) {
            char head[32];
            std::snprintf(head, sizeof(head), "T%lu%s", g_threads[i]->tid, g_threads[i]->tid == presenter ? "p" : "");
            add_group(row, head, d);
        } else {
            add_counts(rest, now_counts[i], s.threads[i]);
            any_rest = true;
        }
    }
    if (any_rest) add_group(row, "rest", rest);
    for (uint32_t i = 0; i < n; ++i) s.threads[i] = now_counts[i];
    // Workers.
    const uint32_t w = g_worker_count.load(std::memory_order_acquire);
    row.add(" | W");
    for (uint32_t i = 0; i < w; ++i) {
        const WorkerStats& ws = *g_workers[i];
        const WorkerCounts c{ws.busy.load(std::memory_order_relaxed), ws.spin.load(std::memory_order_relaxed),
                             ws.yield.load(std::memory_order_relaxed), ws.sleep.load(std::memory_order_relaxed),
                             ws.entries.load(std::memory_order_relaxed)};
        WorkerCounts& p = s.workers[i];
        if (c.busy != p.busy || c.entries != p.entries)
            row.add(" r%u.%u=%llu/%llu/%llu/%llu/%llu", ws.ring, ws.serial, static_cast<unsigned long long>(c.busy - p.busy),
                    static_cast<unsigned long long>(c.spin - p.spin), static_cast<unsigned long long>(c.yield - p.yield),
                    static_cast<unsigned long long>(c.sleep - p.sleep),
                    static_cast<unsigned long long>(c.entries - p.entries));
        p = c;
    }
    row.text[row.used++] = '\n';
    put(s, row.text, row.used);
    ++s.row;
    s.row_qpc = now;
    s.row_tsc = tsc;
    s.frames = s.skipped = s.sum = s.squares = s.longest = 0;
}
} // namespace

void entry_knobs() noexcept { InitOnceExecuteOnce(&g_once, load_knobs, nullptr, nullptr); }

bool entry_direct_wanted() noexcept {
    entry_knobs();
    return g_knobs.active;
}

bool entry_stats_on() noexcept {
    entry_knobs();
    return g_knobs.stats;
}

EntryThread* entry_thread_slow() noexcept {
    auto* t = make_new<EntryThread>();
    if (!t) return nullptr;
    t->tid = GetCurrentThreadId();
    t->rng = (t->tid * 2654435761u) | 1;      // xorshift needs a nonzero state
    AcquireSRWLockExclusive(&g_blocks_lock);
    const uint32_t n = g_thread_count.load(std::memory_order_relaxed);
    if (n < kMaxThreads) {
        g_threads[n] = t;
        g_thread_count.store(n + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_blocks_lock);
    t_entry = t;
    return t;
}

WorkerStats* entry_worker_stats(uint32_t ring, uint64_t serial) noexcept {
    if (!entry_stats_on()) return nullptr;
    auto* w = make_new<WorkerStats>();
    if (!w) return nullptr;
    w->ring = ring;
    w->serial = static_cast<uint32_t>(serial);
    AcquireSRWLockExclusive(&g_blocks_lock);
    const uint32_t n = g_worker_count.load(std::memory_order_relaxed);
    if (n < kMaxWorkers) {
        g_workers[n] = w;
        g_worker_count.store(n + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_blocks_lock);
    return w;
}

void entry_frame() noexcept {
    entry_knobs();
    if (!g_knobs.active) return;
    const DWORD self = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_presenter.compare_exchange_strong(expected, self, std::memory_order_relaxed) && expected != self) return;
    if (!g_state && !(g_state = make_new<Presenter>())) return;
    Presenter& s = *g_state;
    const uint64_t now = qpc();
    const EntryArm arm = g_knobs.order[s.phase];
    if (!s.started) {
        s.started = true;
        s.phase_start = s.last = s.row_qpc = s.flushed = now;
        s.row_tsc = __rdtsc();
        if (g_knobs.stats) {
            char head[384];
            const int n = std::snprintf(head, sizeof(head),
                "entry knobs path=%s stats=1 phase_ms=%u pad_us=%u poll_us=%u qpf=%llu tsc_per_qpc=%.4f source=%s "
                "pid=%lu presenter=%lu classes=%u\n",
                g_knobs.path, g_knobs.phase_ms, g_knobs.pad_us, g_knobs.poll_us,
                static_cast<unsigned long long>(g_knobs.qpf), g_knobs.tsc_per_qpc, g_knobs.source,
                GetCurrentProcessId(), self, kEntryClasses);
            if (n > 0) put(s, head, static_cast<size_t>(n));
        }
    } else {
        const uint64_t interval = now - s.last;
        s.last = now;
        if (s.intervals++ < kSkippedPerPhase) {
            ++s.skipped;
        } else {
            ++s.frames;
            s.sum += interval;
            s.squares += interval * interval;
            if (interval > s.longest) s.longest = interval;
        }
    }
    if (now - s.phase_start >= g_knobs.qpf * g_knobs.phase_ms / 1000) {
        if (g_knobs.stats) close_row(s, now, arm);
        s.phase = (s.phase + 1) % g_knobs.arms;
        s.phase_start = now;
        const EntryArm next = g_knobs.order[s.phase];
        if (next != arm) s.intervals = 0;
        g_entry.arm.store(static_cast<uint32_t>(next), std::memory_order_relaxed);
        g_entry.poll_tsc.store(next == EntryArm::DirectPoll ? poll_tsc() : 0, std::memory_order_relaxed);
    }
    if (g_knobs.stats && now - s.flushed >= g_knobs.qpf * kFlushMilliseconds / 1000) {
        flush(s);
        s.flushed = now;
    }
    // Arm d: the presenting thread's frame gets longer by the pad, as if its own work had grown by that much.
    if (static_cast<EntryArm>(g_entry.arm.load(std::memory_order_relaxed)) == EntryArm::ThunkPad && g_knobs.pad_us) {
        const uint64_t until = qpc() + g_knobs.qpf * g_knobs.pad_us / 1000000;
        while (qpc() < until) _mm_pause();
    }
}

} // namespace engine_ddi
