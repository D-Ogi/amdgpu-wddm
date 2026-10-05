// SPDX-License-Identifier: MIT
// amdgpu_wddm_frameloop: a native D3D12 client that runs a game-shaped frame loop with a calibrated amount of
// GPU work per frame and reports, for every frame, where the time went on the CPU and on the GPU.
//
// Why it exists: on the lab unit a D3D12 game shows the graphics pipe idle for a fixed ~5.9 ms per frame both
// at 54.8 and at 31.3 fps, which does not look like GPU work and does look like latency in our frame loop
// (submission to GPU start, GPU completion to CPU wake, Present). This client reproduces the loop with known
// work so that gap can be measured and attributed without a long game session.
//
// What the loop does per frame, in order: wait until frame N-L finished on the GPU (an ID3D12Fence per frame
// slot, SetEventOnCompletion + WaitForSingleObject, as a game does), optional CPU busy work, then for each of
// K command lists record it and submit it in its own ExecuteCommandLists call; the last list also clears the
// back buffer and draws one fullscreen triangle that reads the buffer the dispatches wrote; then Signal, then
// Present. Every list brackets itself with two timestamp queries, resolved in the last list of the frame and
// read back once the frame's fence has signalled.
//
// The Microsoft runtime comes from System32 only: d3d12.dll and dxgi.dll are loaded by full path with
// LOAD_LIBRARY_SEARCH_SYSTEM32 and nothing links against their import libraries, so an application-local copy
// next to the exe cannot be picked up.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include "measure.h"
#include "gen/spin_cs.h"
#include "gen/present_vs.h"
#include "gen/present_ps.h"

using Microsoft::WRL::ComPtr;
using measure::Qpc;

namespace {

// --------------------------------------------------------------------------------------------------- format
// The back buffer format: the one place in this client where it is named. The owner requires the Present path
// to stay ready for 10-bit and HDR back buffers, so nothing below may assume 8 bits per channel: there is no
// byte-level readback of a back buffer here, the clear takes a float4 and the pixel shader writes a float4, so
// changing this constant (and, for HDR, adding the colour-space call) is the whole change.
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr const char* kBackBufferFormatName = "B8G8R8A8_UNORM";

// Bounds on what the client will allocate: the raw per-frame records are reserved up front so the frame loop
// itself performs no growth allocation, and a run that reaches the frame bound ends early and says so.
constexpr size_t kMaxListRecords = 2000000;
constexpr size_t kMaxFrames = 131072;
constexpr unsigned kThreadsPerGroup = 64;

const char* const kUsage =
    "amdgpu_wddm_frameloop [options]\n"
    "  --seconds S              run length, 1..60 (default 10)\n"
    "  --gpu-ms T               GPU work per frame in ms, 0..100 (default 10; 0 = clear and draw only)\n"
    "  --lists K                command lists (and ExecuteCommandLists calls) per frame, 1..64 (default 7)\n"
    "  --latency L              frames in flight: wait for frame N-L, 1..8 (default 2)\n"
    "  --buffers N              swap chain buffers, 2..4 (default 2)\n"
    "  --present-interval N     Present sync interval, 0..4 (default 0)\n"
    "  --tearing                request ALLOW_TEARING (only taken if the factory supports it)\n"
    "  --frame-latency-waitable N   waitable swap chain, SetMaximumFrameLatency(N), 1..16\n"
    "  --cpu-ms X               CPU busy work per frame in ms, 0..100 (default 0)\n"
    "  --groups G               thread groups per dispatch, 1..65536 (default 2048)\n"
    "  --record-threads N       threads recording the K lists in parallel, 1..16 (default 1; needs --submit\n"
    "                           sequential or batch, and is capped at --lists)\n"
    "  --submit MODE            interleaved (default: record a list, submit it, one call each), sequential\n"
    "                           (record all, then one call each), batch (record all, all K in one call)\n"
    "  --size WxH               borderless window of this size instead of the whole output\n"
    "  --warp | --adapter-luid HI:LO | --adapter-index N   adapter choice (default: first hardware adapter)\n"
    "  --calibrate-every N      GetClockCalibration every N frames, 1..4096 (default 32)\n"
    "  --ts-hz HZ               the frequency the GPU timestamp counter really runs at, 1e6..1e10, used for\n"
    "                           every conversion and for the --gpu-ms calibration instead of what\n"
    "                           GetTimestampFrequency reports, and the clock origin is then fitted from\n"
    "                           bracketed dispatches (for a driver whose reported clock is wrong)\n"
    "  --warmup N               frames excluded from the distributions, 0..1000 (default 10)\n"
    "  --raw-frames N           per-frame records written to the JSON, 0..131072 (default 20000)\n"
    "  --frame-statistics       also call GetFrameStatistics after each Present\n"
    "  --no-timestamps          run without GPU timestamp queries (CPU numbers only)\n"
    "  --debug-layer            enable the D3D12 debug layer\n"
    "  --out PATH               JSON output file (default: no file, stdout summary only)\n"
    "  --selftest               CPU-only checks, no device\n"
    "  --help\n"
    "Exit: 0 ran and self-consistent, 1 a failure (device removed, timeout, invalid timestamps),\n"
    "      2 usage, 3 no adapter or device, 5 watchdog terminated the process.";

// --------------------------------------------------------------------------------------------------- options
struct Options {
    unsigned seconds = 10;
    double gpu_ms = 10.0;
    unsigned lists = 7;
    unsigned latency = 2;
    unsigned buffers = 2;
    unsigned present_interval = 0;
    bool tearing = false;
    unsigned waitable = 0;          // 0 = not a waitable swap chain
    double cpu_ms = 0.0;
    unsigned groups = 2048;
    unsigned width = 0;             // 0 = the whole output
    unsigned height = 0;
    bool warp = false;
    bool have_luid = false;
    LUID luid{};
    int adapter_index = -1;
    unsigned record_threads = 1;
    // How the frame's K command lists reach the queue. "interleaved" (the default) records a list and submits
    // it at once, which is what every measurement before this option was taken with; "sequential" records them
    // all and then submits each in its own call; "batch" records them all and submits them in one call.
    enum class Submit { Interleaved, Sequential, Batch } submit = Submit::Interleaved;
    unsigned calibrate_every = 32;
    // The timestamp frequency to believe instead of the one the driver reports, 0 = believe the driver. An
    // interim lever for BD-056: the BC-250 KMD answers CalibrateGpuClock with the CPU's performance counter
    // (10 MHz) while the command processor writes a 100 MHz counter, so without this every GPU-side duration
    // on that stack is ten times too large and the --gpu-ms calibration produces a tenth of the work asked for.
    UINT64 ts_hz = 0;
    unsigned warmup = 10;
    unsigned raw_frames = 20000;
    // The phase window of the vblank classification: a GPU gap whose end falls this close after a vblank counts
    // as vblank-aligned. 300 us at 60 Hz is 1.8 % of the refresh period, which is also the share a uniform
    // distribution would give, so the measured share is always read against that computed baseline.
    unsigned vblank_phase_us = 300;
    // The ICD log to read the winsys knobs and counters back from, so an arm of a runtime switch carries its own
    // proof. Empty: BC250_DEFERRED_LOG when the environment sets it, else the ICD's default path for this pid.
    std::string icd_log;
    bool frame_statistics = false;
    bool no_timestamps = false;
    bool debug_layer = false;
    bool selftest = false;
    bool help = false;
    std::string out;
    std::string command_line;
};

bool parse_unsigned(const char* text, unsigned low, unsigned high, unsigned& value) {
    if (!text || !*text) { return false; }
    unsigned long long accumulated = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '9') { return false; }
        accumulated = accumulated * 10 + static_cast<unsigned long long>(*c - '0');
        if (accumulated > 0xffffffffull) { return false; }
    }
    if (accumulated < low || accumulated > high) { return false; }
    value = static_cast<unsigned>(accumulated);
    return true;
}

bool parse_double(const char* text, double low, double high, double& value) {
    if (!text || !*text) { return false; }
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (!end || *end || parsed < low || parsed > high) { return false; }
    value = parsed;
    return true;
}

// "1280x720"
bool parse_size(const char* text, unsigned& width, unsigned& height) {
    if (!text) { return false; }
    const char* separator = std::strchr(text, 'x');
    if (!separator || separator == text) { return false; }
    const std::string left(text, static_cast<size_t>(separator - text));
    return parse_unsigned(left.c_str(), 16, 16384, width) && parse_unsigned(separator + 1, 16, 16384, height);
}

// "HI:LO", both decimal, as printed by our other clients for an adapter LUID.
bool parse_luid(const char* text, LUID& luid) {
    if (!text) { return false; }
    const char* separator = std::strchr(text, ':');
    if (!separator || separator == text || !separator[1]) { return false; }
    char* end = nullptr;
    const long long high = std::strtoll(text, &end, 10);
    if (end != separator) { return false; }
    end = nullptr;
    const unsigned long long low = std::strtoull(separator + 1, &end, 10);
    if (!end || *end) { return false; }
    luid.HighPart = static_cast<LONG>(high);
    luid.LowPart = static_cast<DWORD>(low);
    return true;
}

// Returns false on a usage error. Every option is validated here; nothing below re-checks a range.
bool parse_options(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto next = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (!std::strcmp(a, "--help")) { o.help = true; }
        else if (!std::strcmp(a, "--selftest")) { o.selftest = true; }
        else if (!std::strcmp(a, "--tearing")) { o.tearing = true; }
        else if (!std::strcmp(a, "--warp")) { o.warp = true; }
        else if (!std::strcmp(a, "--frame-statistics")) { o.frame_statistics = true; }
        else if (!std::strcmp(a, "--no-timestamps")) { o.no_timestamps = true; }
        else if (!std::strcmp(a, "--debug-layer")) { o.debug_layer = true; }
        else if (!std::strcmp(a, "--seconds")) { if (!parse_unsigned(next(), 1, 60, o.seconds)) { return false; } }
        else if (!std::strcmp(a, "--lists")) { if (!parse_unsigned(next(), 1, 64, o.lists)) { return false; } }
        else if (!std::strcmp(a, "--latency")) { if (!parse_unsigned(next(), 1, 8, o.latency)) { return false; } }
        else if (!std::strcmp(a, "--buffers")) { if (!parse_unsigned(next(), 2, 4, o.buffers)) { return false; } }
        else if (!std::strcmp(a, "--present-interval")) { if (!parse_unsigned(next(), 0, 4, o.present_interval)) { return false; } }
        else if (!std::strcmp(a, "--frame-latency-waitable")) { if (!parse_unsigned(next(), 1, 16, o.waitable)) { return false; } }
        else if (!std::strcmp(a, "--groups")) { if (!parse_unsigned(next(), 1, 65536, o.groups)) { return false; } }
        else if (!std::strcmp(a, "--record-threads")) { if (!parse_unsigned(next(), 1, 16, o.record_threads)) { return false; } }
        else if (!std::strcmp(a, "--submit")) {
            const char* mode = next();
            if (!mode) { return false; }
            if (!std::strcmp(mode, "interleaved")) { o.submit = Options::Submit::Interleaved; }
            else if (!std::strcmp(mode, "sequential")) { o.submit = Options::Submit::Sequential; }
            else if (!std::strcmp(mode, "batch")) { o.submit = Options::Submit::Batch; }
            else { return false; }
        }
        else if (!std::strcmp(a, "--calibrate-every")) { if (!parse_unsigned(next(), 1, 4096, o.calibrate_every)) { return false; } }
        else if (!std::strcmp(a, "--ts-hz")) {
            // A frequency does not fit the unsigned parser's range, so it is parsed as a double and kept exact:
            // 100000000 and 1e8 are both accepted, a fraction of a hertz is not.
            double hz = 0.0;
            if (!parse_double(next(), 1000000.0, 10000000000.0, hz) || hz != std::floor(hz)) { return false; }
            o.ts_hz = static_cast<UINT64>(hz);
        }
        else if (!std::strcmp(a, "--warmup")) { if (!parse_unsigned(next(), 0, 1000, o.warmup)) { return false; } }
        else if (!std::strcmp(a, "--raw-frames")) { if (!parse_unsigned(next(), 0, static_cast<unsigned>(kMaxFrames), o.raw_frames)) { return false; } }
        else if (!std::strcmp(a, "--gpu-ms")) { if (!parse_double(next(), 0.0, 100.0, o.gpu_ms)) { return false; } }
        else if (!std::strcmp(a, "--cpu-ms")) { if (!parse_double(next(), 0.0, 100.0, o.cpu_ms)) { return false; } }
        else if (!std::strcmp(a, "--size")) { if (!parse_size(next(), o.width, o.height)) { return false; } }
        else if (!std::strcmp(a, "--adapter-luid")) { if (!parse_luid(next(), o.luid)) { return false; } o.have_luid = true; }
        else if (!std::strcmp(a, "--adapter-index")) {
            unsigned index = 0;
            if (!parse_unsigned(next(), 0, 15, index)) { return false; }
            o.adapter_index = static_cast<int>(index);
        }
        else if (!std::strcmp(a, "--out")) { const char* p = next(); if (!p || !*p) { return false; } o.out = p; }
        else { return false; }
    }
    if (o.warp && (o.have_luid || o.adapter_index >= 0)) { return false; }
    if (o.have_luid && o.adapter_index >= 0) { return false; }
    if (o.tearing && o.present_interval != 0) { return false; }  // tearing is only legal at interval 0
    if ((o.width == 0) != (o.height == 0)) { return false; }
    // Parallel recording cannot interleave with in-order submission: the whole point of interleaving is that
    // list k is submitted before list k+1 is recorded, which serialises the recording again.
    if (o.record_threads > 1 && o.submit == Options::Submit::Interleaved) { return false; }
    for (int i = 1; i < argc; ++i) { o.command_line += (i > 1 ? " " : ""); o.command_line += argv[i]; }
    return true;
}

const char* submit_name(Options::Submit submit) {
    switch (submit) {
    case Options::Submit::Sequential: return "sequential";
    case Options::Submit::Batch: return "batch";
    default: return "interleaved";
    }
}

// ------------------------------------------------------------------------------------------------- records
struct ListRecord {
    Qpc exec_qpc = 0;        // QPC taken immediately before ExecuteCommandLists
    Qpc exec_done_qpc = 0;   // QPC immediately after it returned
    UINT64 gpu_begin = 0;    // timestamp query at the top of the list
    UINT64 gpu_end = 0;      // timestamp query at the bottom of the list
};

struct FrameRecord {
    unsigned index = 0;
    UINT64 fence_value = 0;                // from one monotonic counter shared by all slot fences
    Qpc begin_qpc = 0;
    Qpc waitable_begin = 0, waitable_end = 0;
    Qpc wait_begin = 0, wait_end = 0;      // the fence wait this frame performed, for frame index - latency
    Qpc drain_begin = 0, drain_end = 0;    // the wait that retired THIS frame after the loop ended
    Qpc cpu_work_end = 0;
    // What recording cost the frame: wall time of the record phase (in interleaved mode, where there is no
    // contiguous phase, the summed per-list spans) and the CPU time summed over the recording threads.
    Qpc record_ticks = 0;
    Qpc record_cpu_ticks = 0;
    Qpc execute_ticks = 0;                 // summed ExecuteCommandLists time
    Qpc signal_begin = 0, signal_end = 0;
    Qpc present_begin = 0, present_end = 0;
    Qpc end_qpc = 0;
    UINT64 present_count = 0;              // GetFrameStatistics, only with --frame-statistics
    Qpc present_sync_qpc = 0;
    HRESULT present_hr = S_OK;
    unsigned retired = 0;                  // the frame whose fence this frame waited on
    bool retired_valid = false;
    bool drained = false;                  // retired outside the steady loop, after the last frame
    bool timestamps_valid = false;
    size_t list_offset = 0;
    unsigned clock_point = 0;              // index into the calibration point list used for this frame
};

struct ClockPoint {
    measure::Clock clock{};
    Qpc taken_qpc = 0;
    Qpc cost_ticks = 0;
};

// ------------------------------------------------------------------------------------------------- watchdog
// The client must never be left running on the lab. The main loop ends at --seconds; this thread is the hard
// stop: it asks for a stop at seconds + 15, and if the process is still alive at seconds + 20 it terminates it
// with exit code 5 after leaving a one-line marker file where the JSON would have gone. The lab harness's own
// limits sit above those two (task limit seconds + 25, poll deadline seconds + 30), so a run that goes wrong
// still ends with the client's evidence; lab\host-checks.ps1 enforces that ordering across the two files.
std::atomic<bool> g_stop{false};
std::atomic<bool> g_writing{false};
std::string g_watchdog_out;
unsigned g_watchdog_seconds = 0;
// QPC at the top of main, so the timeline can charge loader and runtime work to the client rather than to the
// harness that started it.
Qpc g_process_qpc = 0;

DWORD WINAPI watchdog(LPVOID) {
    // The budget covers the measured run plus device creation and the calibration (a few isolated dispatches
    // and up to three 0.5 s sustained passes), so a healthy run never reaches it.
    const unsigned limit = g_watchdog_seconds + 15;
    const DWORD soft = limit * 1000;
    const DWORD hard = 5000;
    for (DWORD waited = 0; waited < soft && !g_stop.load(); waited += 100) { Sleep(100); }
    if (!g_stop.exchange(true)) {
        std::fprintf(stderr, "frameloop: watchdog asked the loop to stop at %u s\n", limit);
    }
    for (DWORD waited = 0; waited < hard; waited += 100) {
        if (g_writing.load()) { return 0; }   // the main thread is already producing output; let it finish
        Sleep(100);
    }
    if (!g_watchdog_out.empty()) {
        std::FILE* file = nullptr;
        if (!fopen_s(&file, g_watchdog_out.c_str(), "wb") && file) {
            std::fprintf(file, "{\"client\": \"amdgpu_wddm_frameloop\", \"status\": \"WATCHDOG\", \"seconds\": %u}\n", g_watchdog_seconds);
            std::fclose(file);
        }
    }
    std::fprintf(stderr, "frameloop: watchdog terminating the process\n");
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 5);
    return 0;
}

// --------------------------------------------------------------------------------------------------- window
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { g_stop.store(true); }
        return 0;
    case WM_CLOSE:
        g_stop.store(true);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w, l);
}

// Per-monitor DPI awareness, loaded dynamically so the client runs on a host without the V2 context: without
// it a borderless window over a scaled output gets a smaller client area than the output's pixels and DXGI
// stretches, which would quietly change what Present costs.
void request_dpi_awareness(const char*& reported) {
    reported = "none";
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) { return; }
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto proc = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    SetContext set = nullptr;
    std::memcpy(&set, &proc, sizeof(set));
    if (set && set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) { reported = "per-monitor-v2"; return; }
    if (set && set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) { reported = "per-monitor"; return; }
}

// ------------------------------------------------------------------------------------------------- selftest
bool expect(bool condition, const char* what, unsigned& failures) {
    std::printf("SELFTEST %s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) { ++failures; }
    return condition;
}

int selftest() {
    unsigned failures = 0;
    // Nearest-rank percentiles on a known series: every reported value occurred, p50 of 1..100 is 50.
    {
        std::vector<double> values;
        for (int i = 1; i <= 100; ++i) { values.push_back(static_cast<double>(i)); }
        const measure::Summary s = measure::summarize(values);
        char text[160]{};
        sprintf_s(text, "summary of 1..100: count %zu min %.1f p50 %.1f p90 %.1f p99 %.1f max %.1f mean %.1f",
                  s.count, s.min, s.p50, s.p90, s.p99, s.max, s.mean);
        expect(s.count == 100 && s.min == 1.0 && s.p50 == 50.0 && s.p90 == 90.0 && s.p99 == 99.0 &&
               s.max == 100.0 && s.mean == 50.5, text, failures);
        const measure::Summary empty = measure::summarize({});
        expect(empty.count == 0 && empty.max == 0.0, "summary of an empty series is zeros with count 0", failures);
        const measure::Summary one = measure::summarize({7.0});
        expect(one.count == 1 && one.p50 == 7.0 && one.p99 == 7.0 && one.max == 7.0, "summary of one sample", failures);
    }
    // The timestamp-scale check: busy plus idle is one frame, so a sound run sits at ratio one, a GPU clock
    // reported ten times too fast reads ten, and a run with no GPU timestamps reports nothing instead of a
    // mismatch. The 3 % case stands for the frame-set difference a sound run has and must still pass.
    {
        const double sound = measure::timestamp_scale_ratio(11.99, 0.12, 12.07);
        const double tenfold = measure::timestamp_scale_ratio(119.9, 1.2, 12.07);
        const double none = measure::timestamp_scale_ratio(0.0, 0.0, 12.07);
        char text[160]{};
        sprintf_s(text, "timestamp scale: sound %.4f tenfold %.4f absent %.4f", sound, tenfold, none);
        expect(sound > 0.99 && sound < 1.01 && tenfold > 9.9 && none == 0.0, text, failures);
        expect(measure::timestamp_scale_ok(sound) && !measure::timestamp_scale_ok(tenfold) &&
               measure::timestamp_scale_ok(none) && measure::timestamp_scale_ok(1.03) &&
               !measure::timestamp_scale_ok(0.4),
               "timestamp scale gate: 1.00 and 1.03 pass, 0.40 and 10.0 fail, absent passes", failures);
    }
    // The GPU-to-QPC mapping: a timestamp at the calibration point maps to the calibration QPC, one GPU second
    // later maps one QPC second later, and a timestamp before the point maps before it (signed, not wrapped).
    {
        measure::Clock c{};
        c.gpu_ticks = 1000000;
        c.cpu_ticks = 5000000;
        c.gpu_frequency = 100000000;      // 100 MHz
        c.qpc_frequency = 10000000;       // 10 MHz
        const Qpc at = c.to_qpc(c.gpu_ticks);
        const Qpc later = c.to_qpc(c.gpu_ticks + 100000000);
        const Qpc earlier = c.to_qpc(c.gpu_ticks - 50000000);
        char text[160]{};
        sprintf_s(text, "clock mapping: at %lld later %lld earlier %lld", at, later, earlier);
        expect(at == 5000000 && later == 5000000 + 10000000 && earlier == 5000000 - 5000000, text, failures);
        expect(c.gpu_ticks_to_ms(100000000) == 1000.0, "100 MHz: 1e8 GPU ticks is 1000 ms", failures);
        expect(measure::qpc_to_ms(10000000, 10000000) == 1000.0, "10 MHz QPC: 1e7 ticks is 1000 ms", failures);
    }
    // Option parsing: the defaults, each range boundary, and the combinations that must be refused.
    {
        char exe[] = "frameloop";
        // The target is reset first: parse_options only writes the options it is given, so a reused Options
        // would carry values from the previous case and a test could pass on stale state.
        const auto run = [&](std::vector<const char*> args, Options& o) -> bool {
            o = Options{};
            std::vector<char*> argv;
            argv.push_back(exe);
            for (const char* a : args) { argv.push_back(const_cast<char*>(a)); }
            return parse_options(static_cast<int>(argv.size()), argv.data(), o);
        };
        Options d{};
        expect(run({}, d) && d.seconds == 10 && d.gpu_ms == 10.0 && d.lists == 7 && d.latency == 2 &&
               d.buffers == 2 && d.present_interval == 0, "defaults: 10 s, 10 ms, 7 lists, latency 2, 2 buffers, interval 0", failures);
        Options a{};
        expect(run({"--seconds", "30", "--gpu-ms", "25.5", "--lists", "1", "--latency", "3",
                    "--present-interval", "1", "--size", "1280x720", "--adapter-luid", "0:65539"}, a) &&
               a.seconds == 30 && a.gpu_ms == 25.5 && a.lists == 1 && a.latency == 3 && a.present_interval == 1 &&
               a.width == 1280 && a.height == 720 && a.have_luid && a.luid.LowPart == 65539,
               "a full command line parses", failures);
        Options bad{};
        expect(!run({"--seconds", "0"}, bad), "--seconds 0 refused", failures);
        expect(!run({"--seconds", "61"}, bad), "--seconds 61 refused", failures);
        expect(!run({"--lists", "65"}, bad), "--lists 65 refused", failures);
        expect(!run({"--latency", "9"}, bad), "--latency 9 refused", failures);
        expect(!run({"--buffers", "1"}, bad), "--buffers 1 refused", failures);
        expect(!run({"--gpu-ms", "101"}, bad), "--gpu-ms 101 refused", failures);
        expect(!run({"--gpu-ms", "4x"}, bad), "--gpu-ms 4x refused", failures);
        expect(!run({"--seconds"}, bad), "--seconds without a value refused", failures);
        expect(!run({"--tearing", "--present-interval", "1"}, bad), "--tearing with interval 1 refused", failures);
        expect(!run({"--warp", "--adapter-index", "0"}, bad), "--warp with --adapter-index refused", failures);
        expect(!run({"--size", "1280"}, bad), "--size without a height refused", failures);
        expect(!run({"--nonsense"}, bad), "an unknown option refused", failures);
        Options hz{};
        expect(run({"--ts-hz", "100000000"}, hz) && hz.ts_hz == 100000000ull, "--ts-hz 100000000 parses", failures);
        expect(run({"--ts-hz", "1e8"}, hz) && hz.ts_hz == 100000000ull, "--ts-hz 1e8 is the same", failures);
        expect(run({}, hz) && hz.ts_hz == 0, "without --ts-hz the driver's frequency is used", failures);
        expect(!run({"--ts-hz", "999999"}, bad) && !run({"--ts-hz", "2e10"}, bad) &&
               !run({"--ts-hz", "100000000.5"}, bad) && !run({"--ts-hz", "fast"}, bad),
               "--ts-hz refuses out of range, fractional and non-numeric values", failures);
        Options zero{};
        expect(run({"--gpu-ms", "0", "--lists", "64", "--latency", "8", "--buffers", "4"}, zero) &&
               zero.gpu_ms == 0.0 && zero.lists == 64 && zero.latency == 8 && zero.buffers == 4,
               "the upper boundaries and --gpu-ms 0 accepted", failures);
        // Recording threads and submission mode.
        Options mt{};
        expect(run({}, mt) && mt.record_threads == 1 && mt.submit == Options::Submit::Interleaved,
               "defaults: 1 recording thread, interleaved submission", failures);
        expect(run({"--record-threads", "4", "--submit", "sequential"}, mt) && mt.record_threads == 4 &&
               mt.submit == Options::Submit::Sequential, "--record-threads 4 --submit sequential", failures);
        expect(run({"--record-threads", "16", "--submit", "batch"}, mt) && mt.record_threads == 16 &&
               mt.submit == Options::Submit::Batch, "--record-threads 16 --submit batch", failures);
        expect(run({"--submit", "batch"}, mt) && mt.record_threads == 1 && mt.submit == Options::Submit::Batch,
               "--submit batch alone keeps one recording thread", failures);
        expect(!run({"--record-threads", "2"}, bad), "--record-threads 2 without a submit mode refused", failures);
        expect(!run({"--record-threads", "2", "--submit", "interleaved"}, bad),
               "--record-threads 2 with interleaved submission refused", failures);
        expect(!run({"--record-threads", "17", "--submit", "batch"}, bad), "--record-threads 17 refused", failures);
        expect(!run({"--record-threads", "0", "--submit", "batch"}, bad), "--record-threads 0 refused", failures);
        expect(!run({"--submit", "parallel"}, bad), "--submit parallel refused", failures);
        expect(!run({"--submit"}, bad), "--submit without a value refused", failures);
        expect(!std::strcmp(submit_name(Options::Submit::Interleaved), "interleaved") &&
               !std::strcmp(submit_name(Options::Submit::Sequential), "sequential") &&
               !std::strcmp(submit_name(Options::Submit::Batch), "batch"),
               "submit mode names round trip into the JSON", failures);
    }
    // JSON escaping and number formatting: a Windows path survives, a non-finite value becomes null.
    {
        expect(measure::escape("P:\\a\"b\n") == "P:\\\\a\\\"b\\n", "escape handles backslash, quote and newline", failures);
        expect(measure::number(1.23456) == "1.235" && measure::number(-0.0005, 3) == "-0.001",
               "number rounds to three decimals", failures);
        expect(measure::number(std::nan("")) == "null" && measure::number(HUGE_VAL) == "null",
               "a non-finite number becomes null", failures);
    }
    std::printf("SELFTEST %s %u failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

// --------------------------------------------------------------------------------------------------- client
struct Slot {
    // One allocator per recording thread: a command allocator can have only one list recording into it at a
    // time, so parallel recording needs one each. List k belongs to thread k % effective_record_threads and is
    // always reset from that thread's allocator.
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
};

class Client;

// One recording worker. Thread 0 is the main thread, which records its own share and then joins these.
struct Recorder {
    Client* client = nullptr;
    unsigned index = 0;
    HANDLE start = nullptr;    // auto-reset: the main thread releases this worker for one frame
    HANDLE done = nullptr;     // auto-reset: the worker has finished its share of the frame
    HANDLE thread = nullptr;
    Qpc ticks = 0;             // this worker's own recording time for the frame
    HRESULT hr = S_OK;
};

struct CalibrationRound {
    unsigned iterations = 0;
    double measured_ms = 0.0;        // per dispatch
    const char* kind = "isolated";   // "isolated" (one dispatch on an idle queue) or "sustained" (a short real pass)
};

const char* hr_name(HRESULT hr) {
    switch (hr) {
    case S_OK: return "S_OK";
    case DXGI_STATUS_OCCLUDED: return "DXGI_STATUS_OCCLUDED";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    default: return "";
    }
}

std::string hr_text(HRESULT hr) {
    char text[64]{};
    const char* name = hr_name(hr);
    if (*name) { sprintf_s(text, "%08lx %s", static_cast<unsigned long>(hr), name); }
    else { sprintf_s(text, "%08lx", static_cast<unsigned long>(hr)); }
    return text;
}

class Client {
public:
    explicit Client(const Options& options) : o(options) {}
    ~Client() { teardown(); }

    int run();

private:
    HRESULT load_runtime();
    HRESULT pick_adapter();
    HRESULT create_device();
    HRESULT create_window();
    HRESULT create_swap_chain();
    HRESULT create_pipeline();
    HRESULT create_frames();
    HRESULT calibrate();
    HRESULT calibrate_sustained();
    HRESULT loop(double seconds);
    double mean_gpu_busy_tail() const;
    void analyse();
    bool write_json() const;
    void teardown();

    // One spin dispatch alone on the queue, measured with the frame-0 timestamp pair. Used only by calibrate().
    HRESULT measure_dispatch(unsigned iterations, unsigned samples, double& median_ms);
    void record_compute(ID3D12GraphicsCommandList* list, unsigned iterations);
    HRESULT record_frame_list(unsigned frame, unsigned slot, unsigned list_index, unsigned back_buffer);
    HRESULT record_share(unsigned thread, Qpc& ticks);
    HRESULT record_all(unsigned frame, unsigned slot, unsigned back_buffer, Qpc& wall, Qpc& cpu_total);
    HRESULT start_recorders();
    void stop_recorders();
    static DWORD WINAPI recorder_main(LPVOID parameter);
    HRESULT wait_slot(unsigned slot, UINT64 value, Qpc& wait_begin, Qpc& wait_end, bool& timed_out);
    HRESULT read_timestamps(unsigned frame_position, unsigned slot);
    void take_clock_point();
    unsigned slots() const { return o.latency + 1; }
    unsigned queries_per_frame() const { return o.no_timestamps ? 0u : o.lists * 2u; }

    Options o;
    // The runtime, by full System32 path.
    HMODULE d3d12 = nullptr;
    HMODULE dxgi = nullptr;
    decltype(&D3D12CreateDevice) create_device_proc = nullptr;
    decltype(&D3D12SerializeRootSignature) serialize_root_signature = nullptr;
    decltype(&CreateDXGIFactory2) create_factory = nullptr;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    ComPtr<ID3D12Device> device;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> chain;
    HANDLE waitable_handle = nullptr;
    HWND window = nullptr;
    bool window_class_registered = false;
    UINT width = 0, height = 0;
    UINT chain_flags = 0;
    UINT present_flags = 0;
    bool tearing_supported = false;

    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_increment = 0;
    std::vector<ComPtr<ID3D12Resource>> back_buffers;
    ComPtr<ID3D12DescriptorHeap> view_heap;          // shader visible: UAV at 0, SRV at 1
    ComPtr<ID3D12Resource> sink;                     // the buffer the dispatches write and the draw reads
    unsigned sink_slots = 0;
    ComPtr<ID3D12RootSignature> compute_root;
    ComPtr<ID3D12RootSignature> graphics_root;
    ComPtr<ID3D12PipelineState> compute_pso;
    ComPtr<ID3D12PipelineState> graphics_pso;
    ComPtr<ID3D12QueryHeap> query_heap;
    ComPtr<ID3D12Resource> query_readback;
    const UINT64* query_data = nullptr;

    std::vector<Slot> frame_slots;
    unsigned effective_record_threads = 1;
    std::vector<Recorder> recorders;        // effective_record_threads - 1 entries; thread 0 is the main thread
    std::atomic<bool> recorders_quit{false};
    // The frame the recorders are working on. Written by the main thread before it releases them and read by
    // them after; the start event is the barrier that orders the two.
    unsigned record_frame = 0, record_slot = 0, record_back_buffer = 0;
    unsigned iterations = 0;
    std::vector<CalibrationRound> calibration_rounds;
    double calibration_target_ms = 0.0;
    double calibration_measured_ms = 0.0;
    double calibration_slope_ms = 0.0;
    double calibration_intercept_ms = 0.0;
    // The cost of one dispatch that does almost nothing, measured at startup. K lists cannot produce less than
    // K times this, so --gpu-ms below that floor cannot be met however the iteration count is chosen.
    double dispatch_floor_ms = 0.0;

    measure::Clock clock{};
    std::vector<ClockPoint> clock_points;
    Qpc qpc_frequency = 1;
    UINT64 gpu_frequency = 1;
    Qpc first_qpc = 0;
    Qpc loop_begin_qpc = 0;
    Qpc loop_end_qpc = 0;

    std::vector<FrameRecord> frames;
    std::vector<ListRecord> list_records;
    unsigned signalled_frames = 0;
    // One monotonic counter for every slot fence. A per-slot "frame + 1" would be lower than the values the
    // calibration already signalled on that fence, and SetEventOnCompletion on an already-higher fence returns
    // at once: the first frames would then be read before their resolve had landed.
    UINT64 next_fence_value = 1;
    unsigned calibration_passes = 0;
    size_t frame_budget = 0;
    bool budget_reached = false;
    bool escape_pressed = false;
    bool wait_timed_out = false;
    unsigned invalid_timestamp_frames = 0;
    HRESULT removed_reason = S_OK;
    HRESULT failure = S_OK;
    const char* failure_where = "";
    const char* dpi_awareness = "none";

    // What GetTimestampFrequency answered, kept next to the frequency actually used (--ts-hz may replace it).
    UINT64 reported_gpu_frequency = 0;
    // Where each phase of the run ended, so that a run's wall time can be accounted for: a trial has to size
    // its own bounds, and "27 s for 5 s of measurement" is a question the client should answer itself.
    struct Mark { const char* phase; Qpc at; };
    std::vector<Mark> timeline;
    Qpc process_qpc = 0;
    void mark(const char* phase) { timeline.push_back({phase, measure::now()}); }
    // The bracket on the GPU clock's origin, collected from every dispatch measured on its own. A dispatch
    // cannot start on the GPU before it was submitted and cannot end after the CPU saw its fence, so each one
    // bounds the QPC that GPU tick zero corresponds to from below and from above. Used when --ts-hz replaces a
    // driver's reported clock, because then the driver's own calibration point is worthless as well; reported
    // always, because the width of the bracket says how much of the submit and wake latency is irreducible.
    struct ClockFit {
        bool applied = false;
        unsigned samples = 0;
        Qpc lower = 0, upper = 0, origin = 0;
        double bracket_ms = 0.0;
        double kmd_point_offset_ms = 0.0;   // where the driver's own point sits against the fit
        size_t applied_from_point = 0;
        UINT64 kmd_gpu_ticks = 0, kmd_cpu_ticks = 0;
        bool all_time_crossed = false;      // bounds from the whole run disagree: the clock drifted
    } clock_fit;
    Qpc fit_lower = LLONG_MIN;
    Qpc fit_upper = LLONG_MAX;
    unsigned fit_samples = 0;
    // The bracket has to be re-fitted as the run goes on, not once: a GPU clock drifts against QPC (46 ppm
    // measured on the host, which is 0.3 ms over 7 s), and a single origin would make the wake latency
    // negative - the CPU waking before the GPU finished - a mistake that reads as a result. Each sample is
    // kept with the time it was taken so that a clock point can be fitted from a recent window only.
    struct Bracket { Qpc lower, upper, at; };
    static constexpr size_t kBrackets = 1024;
    Bracket brackets[kBrackets]{};
    size_t bracket_count = 0;
    Qpc last_point_qpc = 0;
    unsigned fit_windows = 0;          // clock points that moved the origin from a recent window
    unsigned fit_clamped = 0;          // refits held back by the drift allowance
    Qpc fit_origin0 = 0;               // the two-sided fit made before the loop
    Qpc fit_origin0_at = 0;
    // How far the GPU clock is allowed to have drifted against QPC, parts per million. The host measures 46;
    // 200 leaves room for a worse part while still refusing a refit that a loose window would drag away.
    static constexpr double kDriftPpm = 200.0;
    void add_clock_bracket(Qpc submitted_qpc, Qpc woke_qpc, UINT64 gpu_begin, UINT64 gpu_end);
    void reset_clock_bracket() { fit_lower = LLONG_MIN; fit_upper = LLONG_MAX; fit_samples = 0; bracket_count = 0; }
    bool refit_origin(Qpc since, Qpc at);
    bool compute_clock_bracket();
    void apply_clock_fit();

    // Derived series, filled by analyse().
    struct Derived {
        measure::Summary interval, fence_wait, waitable_wait, cpu_work, record, record_cpu, execute, signal, present, frame_cpu;
        measure::Summary gpu_busy, intra_gap_total, intra_gap_each, inter_gap, submit_latency, wake_latency, gpu_span;
        measure::Summary idle_total, idle_awaiting_submission, idle_after_submission, wake_blocking;
        double fps = 0.0;
        double seconds = 0.0;
        size_t measured = 0;
        double gpu_busy_error_pct = 0.0;
        double clock_drift_ppm = 0.0;
        double gpu_accounted_ms = 0.0;
        double gpu_over_interval = 0.0;
        bool timestamp_scale_ok = true;
        // Time runs forwards: a list cannot start before its submission and the CPU cannot wake before the GPU
        // finished. Either count above a handful of samples means the GPU timestamps are not aligned with QPC,
        // which invalidates the latencies and the awaiting/after split - and nothing else.
        double negative_tolerance_ms = 0.0;
        size_t negative_submit = 0;
        size_t negative_wake = 0;
    } d;
};

HRESULT Client::load_runtime() {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) { return HRESULT_FROM_WIN32(GetLastError()); }
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, system);
    if (wcscat_s(path, L"\\dxgi.dll")) { return E_FAIL; }
    dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi) { return HRESULT_FROM_WIN32(GetLastError()); }
    wcscpy_s(path, system);
    if (wcscat_s(path, L"\\d3d12.dll")) { return E_FAIL; }
    d3d12 = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d12) { return HRESULT_FROM_WIN32(GetLastError()); }
    const auto bind = [](HMODULE module, const char* name, auto& target) -> bool {
        auto proc = GetProcAddress(module, name);
        if (!proc) { return false; }
        std::memcpy(&target, &proc, sizeof(target));
        return true;
    };
    if (!bind(dxgi, "CreateDXGIFactory2", create_factory)) { return E_NOINTERFACE; }
    if (!bind(d3d12, "D3D12CreateDevice", create_device_proc)) { return E_NOINTERFACE; }
    if (!bind(d3d12, "D3D12SerializeRootSignature", serialize_root_signature)) { return E_NOINTERFACE; }
    if (o.debug_layer) {
        decltype(&D3D12GetDebugInterface) get_debug = nullptr;
        if (bind(d3d12, "D3D12GetDebugInterface", get_debug)) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(get_debug(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
        }
    }
    return S_OK;
}

HRESULT Client::pick_adapter() {
    ComPtr<IDXGIFactory4> factory4;
    HRESULT hr = create_factory(o.debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory4));
    if (FAILED(hr)) { return hr; }
    hr = factory4.As(&factory);
    if (FAILED(hr)) { return hr; }
    if (o.warp) {
        ComPtr<IDXGIAdapter> warp;
        hr = factory4->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        if (FAILED(hr)) { return hr; }
        hr = warp.As(&adapter);
        if (FAILED(hr)) { return hr; }
        return adapter->GetDesc1(&adapter_desc);
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) { break; }
        if (FAILED(hr)) { return hr; }
        DXGI_ADAPTER_DESC1 desc{};
        hr = candidate->GetDesc1(&desc);
        if (FAILED(hr)) { return hr; }
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (o.have_luid) {
            if (desc.AdapterLuid.LowPart == o.luid.LowPart && desc.AdapterLuid.HighPart == o.luid.HighPart) {
                adapter = candidate; adapter_desc = desc; return S_OK;
            }
        } else if (o.adapter_index >= 0) {
            if (index == static_cast<UINT>(o.adapter_index)) { adapter = candidate; adapter_desc = desc; return S_OK; }
        } else if (!software) {
            adapter = candidate; adapter_desc = desc; return S_OK;
        }
    }
    return DXGI_ERROR_NOT_FOUND;
}

HRESULT Client::create_device() {
    const D3D_FEATURE_LEVEL wanted[]{D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = E_FAIL;
    for (const D3D_FEATURE_LEVEL level : wanted) {
        hr = create_device_proc(adapter.Get(), level, IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) { feature_level = level; break; }
    }
    if (FAILED(hr)) { return hr; }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) { return hr; }
    if (!o.no_timestamps) {
        hr = queue->GetTimestampFrequency(&gpu_frequency);
        if (FAILED(hr) || !gpu_frequency) {
            std::fprintf(stderr, "frameloop: GetTimestampFrequency failed (%s), continuing without GPU timestamps\n", hr_text(hr).c_str());
            o.no_timestamps = true;
            gpu_frequency = 1;
        }
        reported_gpu_frequency = gpu_frequency;
        if (o.ts_hz && !o.no_timestamps) {
            gpu_frequency = o.ts_hz;
            std::printf("frameloop: timestamp frequency %llu Hz by --ts-hz, not the %llu Hz reported;"
                        " conversions and the --gpu-ms calibration use it and the origin is fitted\n",
                        static_cast<unsigned long long>(gpu_frequency),
                        static_cast<unsigned long long>(reported_gpu_frequency));
            std::fflush(stdout);
        }
    }
    return S_OK;
}

HRESULT Client::create_window() {
    request_dpi_awareness(dpi_awareness);
    RECT rect{0, 0, 1280, 720};
    bool have_rect = false;
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc))) { rect = desc.DesktopCoordinates; have_rect = true; }
    }
    if (!have_rect) {
        const POINT origin{0, 0};
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor && GetMonitorInfoW(monitor, &info)) { rect = info.rcMonitor; }
    }
    if (o.width) {
        rect.right = rect.left + static_cast<LONG>(o.width);
        rect.bottom = rect.top + static_cast<LONG>(o.height);
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"amdgpu_wddm_frameloop";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class)) { return HRESULT_FROM_WIN32(GetLastError()); }
    window_class_registered = true;
    // Borderless: WS_POPUP has no frame, so the client area is the rectangle asked for and the swap chain
    // matches the output pixel for pixel.
    window = CreateWindowExW(0, window_class.lpszClassName, L"amdgpu-wddm frame loop", WS_POPUP,
                             rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                             nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) { return HRESULT_FROM_WIN32(GetLastError()); }
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    RECT client{};
    if (!GetClientRect(window, &client)) { return HRESULT_FROM_WIN32(GetLastError()); }
    width = static_cast<UINT>(client.right - client.left);
    height = static_cast<UINT>(client.bottom - client.top);
    if (!width || !height) { return E_UNEXPECTED;}
    return S_OK;
}

HRESULT Client::create_swap_chain() {
    BOOL allow_tearing = FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
            allow_tearing = FALSE;
        }
    }
    tearing_supported = allow_tearing != FALSE;
    chain_flags = 0;
    if (o.tearing && tearing_supported) { chain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; }
    if (o.waitable) { chain_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT; }
    present_flags = (chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0u;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = kBackBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = o.buffers;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = chain_flags;
    ComPtr<IDXGISwapChain1> chain1;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain1);
    if (FAILED(hr)) { return hr; }
    hr = factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) { return hr; }
    hr = chain1.As(&chain);
    if (FAILED(hr)) { return hr; }
    if (o.waitable) {
        hr = chain->SetMaximumFrameLatency(o.waitable);
        if (FAILED(hr)) { return hr; }
        waitable_handle = chain->GetFrameLatencyWaitableObject();
        if (!waitable_handle) { return E_UNEXPECTED; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvs{};
    rtvs.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvs.NumDescriptors = o.buffers;
    hr = device->CreateDescriptorHeap(&rtvs, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) { return hr; }
    rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    back_buffers.resize(o.buffers);
    for (UINT i = 0; i < o.buffers; ++i) {
        hr = chain->GetBuffer(i, IID_PPV_ARGS(&back_buffers[i]));
        if (FAILED(hr)) { return hr; }
        D3D12_CPU_DESCRIPTOR_HANDLE view = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        view.ptr += SIZE_T{i} * rtv_increment;
        device->CreateRenderTargetView(back_buffers[i].Get(), nullptr, view);
    }
    return S_OK;
}

HRESULT Client::create_pipeline() {
    // The buffer the dispatches write and the draw reads, one 32-bit slot per dispatched thread.
    sink_slots = o.groups * kThreadsPerGroup;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64{sink_slots} * 4;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sink));
    if (FAILED(hr)) { return hr; }

    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    views.NumDescriptors = 2;
    views.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&view_heap));
    if (FAILED(hr)) { return hr; }
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = sink_slots;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(sink.Get(), nullptr, &uav, cpu);
    cpu.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = sink_slots;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(sink.Get(), &srv, cpu);

    // Root signatures: four root constants plus one single-descriptor table. Root descriptors would be one
    // call fewer, but a descriptor table is the path a game takes and the path already exercised on the lab.
    const auto make_root = [&](D3D12_DESCRIPTOR_RANGE_TYPE range_type, D3D12_SHADER_VISIBILITY visibility,
                               ComPtr<ID3D12RootSignature>& out) -> HRESULT {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = range_type;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = visibility;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = visibility;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 2;
        root.pParameters = params;
        ComPtr<ID3DBlob> blob, error;
        HRESULT serialized = serialize_root_signature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(serialized)) {
            if (error) { std::fprintf(stderr, "frameloop: root signature: %.*s\n", static_cast<int>(error->GetBufferSize()), static_cast<const char*>(error->GetBufferPointer())); }
            return serialized;
        }
        return device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out));
    };
    hr = make_root(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_SHADER_VISIBILITY_ALL, compute_root);
    if (FAILED(hr)) { return hr; }
    hr = make_root(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_SHADER_VISIBILITY_ALL, graphics_root);
    if (FAILED(hr)) { return hr; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = compute_root.Get();
    compute.CS.pShaderBytecode = g_spin_cs;
    compute.CS.BytecodeLength = sizeof(g_spin_cs);
    hr = device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&compute_pso));
    if (FAILED(hr)) { return hr; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.pRootSignature = graphics_root.Get();
    graphics.VS.pShaderBytecode = g_present_vs;
    graphics.VS.BytecodeLength = sizeof(g_present_vs);
    graphics.PS.pShaderBytecode = g_present_ps;
    graphics.PS.BytecodeLength = sizeof(g_present_ps);
    graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.SampleMask = UINT_MAX;
    graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable = TRUE;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets = 1;
    graphics.RTVFormats[0] = kBackBufferFormat;
    graphics.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&graphics_pso));
    if (FAILED(hr)) { return hr; }

    if (!o.no_timestamps) {
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queries.Count = slots() * queries_per_frame();
        hr = device->CreateQueryHeap(&queries, IID_PPV_ARGS(&query_heap));
        if (FAILED(hr)) { return hr; }
        D3D12_HEAP_PROPERTIES readback{};
        readback.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC results{};
        results.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        results.Width = UINT64{queries.Count} * sizeof(UINT64);
        results.Height = 1;
        results.DepthOrArraySize = 1;
        results.MipLevels = 1;
        results.SampleDesc.Count = 1;
        results.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &results,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&query_readback));
        if (FAILED(hr)) { return hr; }
        void* mapped = nullptr;
        hr = query_readback->Map(0, nullptr, &mapped);
        if (FAILED(hr)) { return hr; }
        query_data = static_cast<const UINT64*>(mapped);
    }
    return S_OK;
}

HRESULT Client::create_frames() {
    effective_record_threads = o.record_threads < o.lists ? o.record_threads : o.lists;
    frame_slots.resize(slots());
    for (unsigned s = 0; s < slots(); ++s) {
        frame_slots[s].allocators.resize(effective_record_threads);
        for (unsigned t = 0; t < effective_record_threads; ++t) {
            const HRESULT allocated = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                     IID_PPV_ARGS(&frame_slots[s].allocators[t]));
            if (FAILED(allocated)) { return allocated; }
        }
        HRESULT hr = S_OK;
        frame_slots[s].lists.resize(o.lists);
        for (unsigned k = 0; k < o.lists; ++k) {
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           frame_slots[s].allocators[k % effective_record_threads].Get(),
                                           nullptr, IID_PPV_ARGS(&frame_slots[s].lists[k]));
            if (FAILED(hr)) { return hr; }
            hr = frame_slots[s].lists[k]->Close();
            if (FAILED(hr)) { return hr; }
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_slots[s].fence));
        if (FAILED(hr)) { return hr; }
        frame_slots[s].event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frame_slots[s].event) { return HRESULT_FROM_WIN32(GetLastError()); }
    }
    frame_budget = kMaxListRecords / o.lists;
    if (frame_budget > kMaxFrames) { frame_budget = kMaxFrames; }
    if (frame_budget < 256) { frame_budget = 256; }
    frames.reserve(frame_budget);
    list_records.reserve(frame_budget * o.lists);
    return S_OK;
}

void Client::record_compute(ID3D12GraphicsCommandList* list, unsigned spin) {
    ID3D12DescriptorHeap* heaps[]{view_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(compute_root.Get());
    list->SetPipelineState(compute_pso.Get());
    const UINT constants[4]{spin, 0x9e3779b9u, sink_slots, 0u};
    list->SetComputeRoot32BitConstants(0, 4, constants, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE table = view_heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(1, table);
    list->Dispatch(o.groups, 1, 1);
    // The UAV barrier keeps consecutive dispatches from overlapping, so each list's bottom timestamp really
    // marks the end of that list's work and the gaps between lists mean what they say.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = sink.Get();
    list->ResourceBarrier(1, &barrier);
}

HRESULT Client::record_frame_list(unsigned frame, unsigned slot, unsigned list_index, unsigned back_buffer) {
    ID3D12GraphicsCommandList* list = frame_slots[slot].lists[list_index].Get();
    HRESULT hr = list->Reset(frame_slots[slot].allocators[list_index % effective_record_threads].Get(), nullptr);
    if (FAILED(hr)) { return hr; }
    const UINT base = slot * queries_per_frame();
    if (!o.no_timestamps) { list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + list_index * 2); }
    if (iterations) { record_compute(list, iterations); }
    const bool last = list_index + 1 == o.lists;
    if (last) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = sink.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = back_buffers[back_buffer].Get();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(2, barriers);

        D3D12_CPU_DESCRIPTOR_HANDLE view = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        view.ptr += SIZE_T{back_buffer} * rtv_increment;
        list->OMSetRenderTargets(1, &view, FALSE, nullptr);
        // A clear colour that moves with the frame, in float: correct for an 8-bit, a 10-bit or a float target.
        const float phase = static_cast<float>(frame % 256) / 255.0f;
        const FLOAT clear[4]{0.05f, phase * 0.25f, 0.12f, 1.0f};
        list->ClearRenderTargetView(view, clear, 0, nullptr);
        D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[]{view_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(graphics_root.Get());
        list->SetPipelineState(graphics_pso.Get());
        const UINT constants[4]{frame, sink_slots, 0u, 0u};
        list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE table = view_heap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetGraphicsRootDescriptorTable(1, table);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(2, barriers);
    }
    if (!o.no_timestamps) {
        list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + list_index * 2 + 1);
        if (last) {
            list->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, queries_per_frame(),
                                   query_readback.Get(), UINT64{base} * sizeof(UINT64));
        }
    }
    return list->Close();
}

// This thread's share of the frame's lists: thread t takes t, t + T, t + 2T, ... Round robin rather than
// contiguous blocks, because the last list carries the clear, the draw and the query resolve and is the most
// expensive one to record; round robin keeps it from always landing on the same thread as its neighbours.
// Everything read here is immutable for the duration of the loop, and each list and each allocator is touched
// by exactly one thread, which is what D3D12 requires of parallel recording.
HRESULT Client::record_share(unsigned thread, Qpc& ticks) {
    const Qpc begin = measure::now();
    HRESULT hr = S_OK;
    for (unsigned k = thread; k < o.lists; k += effective_record_threads) {
        hr = record_frame_list(record_frame, record_slot, k, record_back_buffer);
        if (FAILED(hr)) { break; }
    }
    ticks = measure::now() - begin;
    return hr;
}

DWORD WINAPI Client::recorder_main(LPVOID parameter) {
    Recorder* recorder = static_cast<Recorder*>(parameter);
    for (;;) {
        if (WaitForSingleObject(recorder->start, INFINITE) != WAIT_OBJECT_0) { return 1; }
        if (recorder->client->recorders_quit.load()) { return 0; }
        recorder->hr = recorder->client->record_share(recorder->index, recorder->ticks);
        SetEvent(recorder->done);
    }
}

HRESULT Client::start_recorders() {
    if (effective_record_threads < 2) { return S_OK; }
    recorders.resize(effective_record_threads - 1);
    for (unsigned i = 0; i < recorders.size(); ++i) {
        Recorder& recorder = recorders[i];
        recorder.client = this;
        recorder.index = i + 1;              // thread 0 is the main thread
        recorder.start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        recorder.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!recorder.start || !recorder.done) { return HRESULT_FROM_WIN32(GetLastError()); }
        recorder.thread = CreateThread(nullptr, 0, recorder_main, &recorder, 0, nullptr);
        if (!recorder.thread) { return HRESULT_FROM_WIN32(GetLastError()); }
    }
    return S_OK;
}

void Client::stop_recorders() {
    recorders_quit.store(true);
    for (Recorder& recorder : recorders) { if (recorder.start) { SetEvent(recorder.start); } }
    for (Recorder& recorder : recorders) {
        if (recorder.thread) { WaitForSingleObject(recorder.thread, 2000); CloseHandle(recorder.thread); recorder.thread = nullptr; }
        if (recorder.start) { CloseHandle(recorder.start); recorder.start = nullptr; }
        if (recorder.done) { CloseHandle(recorder.done); recorder.done = nullptr; }
    }
    recorders.clear();
}

// Record all K lists, in parallel over effective_record_threads threads, and report both what the frame paid
// (wall) and what the machine paid (cpu_total). Used by the sequential and batch submit modes; interleaved
// records inside its submit loop instead.
HRESULT Client::record_all(unsigned frame, unsigned slot, unsigned back_buffer, Qpc& wall, Qpc& cpu_total) {
    record_frame = frame;
    record_slot = slot;
    record_back_buffer = back_buffer;
    const Qpc begin = measure::now();
    for (Recorder& recorder : recorders) { recorder.ticks = 0; recorder.hr = S_OK; SetEvent(recorder.start); }
    Qpc own = 0;
    HRESULT hr = record_share(0, own);
    cpu_total = own;
    if (!recorders.empty()) {
        HANDLE done[16]{};
        for (size_t i = 0; i < recorders.size(); ++i) { done[i] = recorders[i].done; }
        const DWORD joined = WaitForMultipleObjects(static_cast<DWORD>(recorders.size()), done, TRUE, 5000);
        if (joined == WAIT_TIMEOUT || joined == WAIT_FAILED) { wall = measure::now() - begin; return HRESULT_FROM_WIN32(WAIT_TIMEOUT); }
        for (const Recorder& recorder : recorders) {
            cpu_total += recorder.ticks;
            if (SUCCEEDED(hr) && FAILED(recorder.hr)) { hr = recorder.hr; }
        }
    }
    wall = measure::now() - begin;
    return hr;
}

HRESULT Client::wait_slot(unsigned slot, UINT64 value, Qpc& wait_begin, Qpc& wait_end, bool& timed_out) {
    Slot& s = frame_slots[slot];
    timed_out = false;
    wait_begin = measure::now();
    HRESULT hr = s.fence->SetEventOnCompletion(value, s.event);
    if (FAILED(hr)) { wait_end = measure::now(); return hr; }
    DWORD result = WAIT_TIMEOUT;
    for (unsigned attempt = 0; attempt < 5 && result == WAIT_TIMEOUT; ++attempt) {
        result = WaitForSingleObject(s.event, 1000);
        if (result == WAIT_TIMEOUT && g_stop.load() && s.fence->GetCompletedValue() >= value) { break; }
    }
    wait_end = measure::now();
    if (result != WAIT_OBJECT_0 && s.fence->GetCompletedValue() < value) {
        timed_out = true;
        return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    return S_OK;
}

// The timestamps of the frame at `frame_position` in `frames`, whose work used `slot`. Called once that
// frame's fence has signalled, so the resolve has landed.
HRESULT Client::read_timestamps(unsigned frame_position, unsigned slot) {
    FrameRecord& record = frames[frame_position];
    if (o.no_timestamps || !query_data) { return S_OK; }
    const UINT base = slot * queries_per_frame();
    const D3D12_RANGE range{base * sizeof(UINT64), (base + queries_per_frame()) * sizeof(UINT64)};
    // A no-op on a coherent mapping, a cache invalidation hint where the driver wants one; the persistent
    // pointer stays valid either way.
    void* ignored = nullptr;
    HRESULT hr = query_readback->Map(0, &range, &ignored);
    if (FAILED(hr)) { return hr; }
    bool valid = true;
    for (unsigned k = 0; k < o.lists; ++k) {
        ListRecord& l = list_records[record.list_offset + k];
        l.gpu_begin = query_data[base + k * 2];
        l.gpu_end = query_data[base + k * 2 + 1];
        if (!l.gpu_begin || l.gpu_end < l.gpu_begin) { valid = false; }
        if (k && l.gpu_begin < list_records[record.list_offset + k - 1].gpu_begin) { valid = false; }
    }
    const D3D12_RANGE written{0, 0};
    query_readback->Unmap(0, &written);
    record.timestamps_valid = valid;
    if (!valid) { ++invalid_timestamp_frames; }
    return S_OK;
}

// One measured dispatch, one bound on each side of the clock's origin. Both bounds are in QPC ticks and assume
// only that time runs forwards: the GPU started the list after ExecuteCommandLists returned the submission's
// timestamp, and finished it before the CPU's wait on its fence came back.
void Client::add_clock_bracket(Qpc submitted_qpc, Qpc woke_qpc, UINT64 gpu_begin, UINT64 gpu_end) {
    if (!gpu_frequency || !gpu_begin || gpu_end < gpu_begin || woke_qpc < submitted_qpc) { return; }
    const long double scale = static_cast<long double>(qpc_frequency) / static_cast<long double>(gpu_frequency);
    const Qpc lower = submitted_qpc - static_cast<Qpc>(llroundl(static_cast<long double>(gpu_begin) * scale));
    const Qpc upper = woke_qpc - static_cast<Qpc>(llroundl(static_cast<long double>(gpu_end) * scale));
    if (lower > fit_lower) { fit_lower = lower; }
    if (upper < fit_upper) { fit_upper = upper; }
    ++fit_samples;
    brackets[bracket_count % kBrackets] = {lower, upper, woke_qpc};
    ++bracket_count;
}

// Move the origin to follow the clock's drift, from the samples taken since `since`.
//
// Not the middle of the window's bounds: inside the loop the queue is deliberately backlogged, so the
// submission-side bound is loose by a whole frame and a midpoint sits half a frame early (5.4 ms, measured
// against the host driver's own calibration). The wake-side bound is the tight one whenever the wait blocked,
// so it is the estimate - and because it is loose in its turn when the loop is Present-bound and the fence
// completes long before the wait, the move is allowed only as far as a drift of kDriftPpm could have taken the
// clock since the two-sided fit. A refit therefore tracks drift and can never be dragged away by a loose window.
bool Client::refit_origin(Qpc since, Qpc at) {
    Qpc lower = LLONG_MIN, upper = LLONG_MAX;
    unsigned used = 0;
    const size_t have = bracket_count < kBrackets ? bracket_count : kBrackets;
    for (size_t i = 0; i < have; ++i) {
        const Bracket& b = brackets[i];
        if (b.at < since) { continue; }
        if (b.lower > lower) { lower = b.lower; }
        if (b.upper < upper) { upper = b.upper; }
        ++used;
    }
    if (used < 2 || lower > upper) { return false; }
    const double elapsed_s = measure::qpc_to_ms(at - fit_origin0_at, qpc_frequency) / 1000.0;
    const Qpc allowance = static_cast<Qpc>(kDriftPpm * 1e-6 * elapsed_s * static_cast<double>(qpc_frequency)) + 1;
    Qpc candidate = upper;
    if (candidate > fit_origin0 + allowance) { candidate = fit_origin0 + allowance; ++fit_clamped; }
    else if (candidate < fit_origin0 - allowance) { candidate = fit_origin0 - allowance; ++fit_clamped; }
    if (candidate < lower) { candidate = lower; }   // a sound lower bound still wins
    clock_fit.origin = candidate;
    ++fit_windows;
    return true;
}

// Reduce the collected bounds to an origin and a width, without deciding to use them. Reported on every run:
// the width is a floor on this stack's submission plus wake latency, and on a sound driver the fitted origin
// has to agree with the driver's own calibration point, which is a check worth having.
bool Client::compute_clock_bracket() {
    clock_fit.samples = fit_samples;
    clock_fit.all_time_crossed = fit_samples && fit_lower > fit_upper;
    if (!fit_samples || fit_lower > fit_upper) { return false; }
    clock_fit.lower = fit_lower;
    clock_fit.upper = fit_upper;
    clock_fit.origin = fit_lower + (fit_upper - fit_lower) / 2;
    clock_fit.bracket_ms = measure::qpc_to_ms(fit_upper - fit_lower, qpc_frequency);
    if (!clock_points.empty()) {
        const measure::Clock& driver = clock_points.front().clock;
        clock_fit.kmd_gpu_ticks = driver.gpu_ticks;
        clock_fit.kmd_cpu_ticks = driver.cpu_ticks;
        // Where the driver's point would put the instant the fit puts at its own origin. Inside the bracket on
        // a sound stack; days out on the BC-250, whose KMD reports the CPU's counter as the GPU's.
        measure::Clock fitted{};
        fitted.gpu_frequency = gpu_frequency;
        fitted.qpc_frequency = qpc_frequency;
        fitted.cpu_ticks = static_cast<UINT64>(clock_fit.origin);
        clock_fit.kmd_point_offset_ms = measure::qpc_to_ms(
            static_cast<Qpc>(driver.cpu_ticks) - fitted.to_qpc(driver.gpu_ticks), qpc_frequency);
    }
    return true;
}

// Make the fitted clock the one the rest of the run uses. Called only when --ts-hz says the driver's clock is
// not to be trusted, because then its calibration point is worthless as well. The bracket's width is the
// uncertainty every cross-clock number (submit_to_gpu_start, gpu_end_to_wake, the awaiting/after split) carries.
void Client::apply_clock_fit() {
    if (!compute_clock_bracket()) {
        std::fprintf(stderr, "frameloop: no usable clock bracket (%u samples%s); the awaiting/after split and"
                             " the latencies stay on the driver's own calibration point\n",
                     fit_samples, fit_samples && fit_lower > fit_upper ? ", bounds crossed" : "");
        return;
    }
    clock_fit.applied = true;
    clock_fit.applied_from_point = clock_points.size();
    fit_origin0 = clock_fit.origin;
    fit_origin0_at = measure::now();
    std::printf("frameloop: clock origin fitted from %u bracketed dispatches, bracket %s ms"
                " (the uncertainty of every cross-clock number), driver point off by %s ms\n",
                clock_fit.samples, measure::number(clock_fit.bracket_ms, 4).c_str(),
                measure::number(clock_fit.kmd_point_offset_ms, 1).c_str());
    std::fflush(stdout);
}

void Client::take_clock_point() {
    ClockPoint point{};
    point.clock.gpu_frequency = gpu_frequency;
    point.clock.qpc_frequency = qpc_frequency;
    const Qpc before = measure::now();
    UINT64 gpu = 0, cpu = 0;
    if (clock_fit.applied) {
        // The fitted clock: GPU tick zero sits at the fitted origin, and the frequency is the one --ts-hz gave.
        // The driver's calibration is still called, so its cost and its answer stay in the record.
        if (!o.no_timestamps) { queue->GetClockCalibration(&gpu, &cpu); }
        // Re-fit from the samples since the previous point, which is what keeps the origin on a drifting clock.
        refit_origin(last_point_qpc, before);
        point.clock.gpu_ticks = 0;
        point.clock.cpu_ticks = static_cast<UINT64>(clock_fit.origin);
    } else if (!o.no_timestamps && SUCCEEDED(queue->GetClockCalibration(&gpu, &cpu))) {
        point.clock.gpu_ticks = gpu;
        point.clock.cpu_ticks = cpu;
    } else if (!clock_points.empty()) {
        point.clock = clock_points.back().clock;
    }
    point.cost_ticks = measure::now() - before;
    point.taken_qpc = before;
    last_point_qpc = before;
    clock_points.push_back(point);
    clock = point.clock;
}

HRESULT Client::measure_dispatch(unsigned spin, unsigned samples, double& median_ms) {
    // Slot 0, list 0 and the slot-0 query pair, used before the frame loop starts.
    std::vector<double> results;
    const unsigned total = samples + 2;  // two warm-up submissions whose results are discarded
    for (unsigned attempt = 0; attempt < total; ++attempt) {
        HRESULT hr = frame_slots[0].allocators[0]->Reset();
        if (FAILED(hr)) { return hr; }
        ID3D12GraphicsCommandList* list = frame_slots[0].lists[0].Get();
        hr = list->Reset(frame_slots[0].allocators[0].Get(), nullptr);
        if (FAILED(hr)) { return hr; }
        if (!o.no_timestamps) { list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0); }
        if (spin) { record_compute(list, spin); }
        if (!o.no_timestamps) {
            list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, query_readback.Get(), 0);
        }
        hr = list->Close();
        if (FAILED(hr)) { return hr; }
        ID3D12CommandList* submit[]{list};
        const Qpc submitted_qpc = measure::now();
        queue->ExecuteCommandLists(1, submit);
        const UINT64 value = next_fence_value++;
        hr = queue->Signal(frame_slots[0].fence.Get(), value);
        if (FAILED(hr)) { return hr; }
        Qpc begin = 0, end = 0;
        bool timed_out = false;
        hr = wait_slot(0, value, begin, end, timed_out);
        if (FAILED(hr)) { return hr; }
        if (attempt < 2) { continue; }
        if (o.no_timestamps) {
            // Without timestamp queries the only duration available is wall time from the submission to the
            // fence wake. It includes submission and wake latency, so the calibration then targets that whole
            // wall time per dispatch rather than GPU busy time; the JSON records which mode was used.
            results.push_back(measure::qpc_to_ms(end - submitted_qpc, qpc_frequency));
            continue;
        }
        const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
        void* ignored = nullptr;
        hr = query_readback->Map(0, &range, &ignored);
        if (FAILED(hr)) { return hr; }
        const UINT64 gpu_begin = query_data[0], gpu_end = query_data[1];
        const D3D12_RANGE written{0, 0};
        query_readback->Unmap(0, &written);
        if (!gpu_begin || gpu_end < gpu_begin) { return E_UNEXPECTED; }
        // This dispatch had the queue to itself and its submission and wake are both timed, so it is also one
        // sample of the clock-origin bracket (see add_clock_bracket).
        add_clock_bracket(submitted_qpc, end, gpu_begin, gpu_end);
        results.push_back(clock.gpu_ticks_to_ms(gpu_end - gpu_begin));
    }
    if (results.empty()) { median_ms = 0.0; return E_UNEXPECTED; }
    std::sort(results.begin(), results.end());
    median_ms = results[results.size() / 2];
    return S_OK;
}

HRESULT Client::calibrate() {
    // A priming dispatch first: it writes every slot of the buffer, so the draw reads defined data even when
    // --gpu-ms is 0 and no dispatch runs inside the loop.
    iterations = 0;
    double primed = 0.0;
    HRESULT hr = measure_dispatch(1, 1, primed);
    if (FAILED(hr)) { return hr; }
    dispatch_floor_ms = primed;
    if (o.gpu_ms > 0.0 && o.gpu_ms < 1.5 * primed * static_cast<double>(o.lists)) {
        std::fprintf(stderr, "frameloop: --gpu-ms %s is close to the floor of %u dispatches (%s ms each,"
                             " %s ms per frame); the calibration will not reach it\n",
                     measure::number(o.gpu_ms).c_str(), o.lists, measure::number(primed, 4).c_str(),
                     measure::number(primed * static_cast<double>(o.lists), 4).c_str());
    }
    if (o.gpu_ms <= 0.0) {
        calibration_target_ms = 0.0;
        calibration_measured_ms = primed;
        iterations = 0;
        return S_OK;
    }
    calibration_target_ms = o.gpu_ms / static_cast<double>(o.lists);
    double low_ms = 0.0, high_ms = 0.0;
    hr = measure_dispatch(64, 3, low_ms);
    if (FAILED(hr)) { return hr; }
    hr = measure_dispatch(1024, 3, high_ms);
    if (FAILED(hr)) { return hr; }
    calibration_slope_ms = (high_ms - low_ms) / (1024.0 - 64.0);
    calibration_intercept_ms = low_ms - calibration_slope_ms * 64.0;
    if (!(calibration_slope_ms > 0.0)) { return E_UNEXPECTED; }
    double wanted = (calibration_target_ms - calibration_intercept_ms) / calibration_slope_ms;
    const auto clamp_iterations = [](double value) -> unsigned {
        if (!(value >= 1.0)) { return 1u; }
        if (value > 16777216.0) { return 16777216u; }
        return static_cast<unsigned>(value + 0.5);
    };
    iterations = clamp_iterations(wanted);
    for (unsigned round = 0; round < 6; ++round) {
        double measured = 0.0;
        hr = measure_dispatch(iterations, 3, measured);
        if (FAILED(hr)) { return hr; }
        calibration_rounds.push_back({iterations, measured, "isolated"});
        calibration_measured_ms = measured;
        if (measured <= 0.0) { return E_UNEXPECTED; }
        const double ratio = measured / calibration_target_ms;
        if (ratio > 0.98 && ratio < 1.02) { break; }
        const unsigned next = clamp_iterations(static_cast<double>(iterations) / ratio);
        if (next == iterations) { break; }
        iterations = next;
    }
    return S_OK;
}

// One pass of the frame loop, for `seconds`. Called once for the measured run and, before it, for the short
// sustained calibration passes; each call starts from an empty record set, so only the last pass is reported.
// The mean GPU busy time of the last half of the frames of the pass just run, over frames with valid
// timestamps. The tail is used because a GPU that boosts or drops its clock under sustained load needs some
// frames to settle, and it is the settled value the frame loop will actually see.
double Client::mean_gpu_busy_tail() const {
    if (frames.size() < 4 || clock_points.empty()) { return 0.0; }
    const size_t begin = frames.size() / 2;
    double total = 0.0;
    size_t counted = 0;
    for (size_t i = begin; i < frames.size(); ++i) {
        const FrameRecord& f = frames[i];
        if (!f.timestamps_valid) { continue; }
        const measure::Clock& c = clock_points[f.clock_point < clock_points.size() ? f.clock_point : 0].clock;
        UINT64 busy = 0;
        for (unsigned k = 0; k < o.lists; ++k) {
            const ListRecord& l = list_records[f.list_offset + k];
            busy += l.gpu_end - l.gpu_begin;
        }
        total += c.gpu_ticks_to_ms(busy);
        ++counted;
    }
    return counted ? total / static_cast<double>(counted) : 0.0;
}

// The isolated calibration measures one dispatch on an otherwise idle queue. Under the sustained load of the
// real loop the same dispatch can take materially longer: a GPU that boosts drops its clock, caches behave
// differently, and the last list also carries the clear and the draw. So the calibration finishes in situ:
// short real passes of the whole frame loop, each one correcting `iterations` against the GPU busy time the
// loop actually produced. On the lab, where the clocks are pinned, these rounds should barely move it.
HRESULT Client::calibrate_sustained() {
    if (o.gpu_ms <= 0.0 || o.no_timestamps || !iterations) { return S_OK; }
    for (unsigned round = 0; round < 3; ++round) {
        const HRESULT hr = loop(0.5);
        ++calibration_passes;
        if (FAILED(hr)) { return hr; }
        if (g_stop.load()) { return S_OK; }
        const double busy = mean_gpu_busy_tail();
        if (!(busy > 0.0)) { return S_OK; }   // no valid timestamps: keep what the isolated fit produced
        calibration_rounds.push_back({iterations, busy / static_cast<double>(o.lists), "sustained"});
        calibration_measured_ms = busy / static_cast<double>(o.lists);
        const double ratio = busy / o.gpu_ms;
        if (ratio > 0.97 && ratio < 1.03) { break; }
        double next = static_cast<double>(iterations) / ratio;
        if (next < 1.0) { next = 1.0; }
        if (next > 16777216.0) { next = 16777216.0; }
        const unsigned adjusted = static_cast<unsigned>(next + 0.5);
        if (adjusted == iterations) { break; }
        iterations = adjusted;
    }
    return S_OK;
}

HRESULT Client::loop(double seconds) {
    frames.clear();
    list_records.clear();
    signalled_frames = 0;
    invalid_timestamp_frames = 0;
    budget_reached = false;
    qpc_frequency = measure::frequency();
    first_qpc = measure::now();
    clock_points.reserve(clock_points.size() + frame_budget / o.calibrate_every + 8);
    take_clock_point();
    const Qpc budget_ticks = static_cast<Qpc>(static_cast<double>(qpc_frequency) * seconds);
    const Qpc cpu_work_ticks = static_cast<Qpc>(static_cast<double>(qpc_frequency) * o.cpu_ms / 1000.0);
    loop_begin_qpc = measure::now();
    HRESULT hr = S_OK;
    unsigned frame = 0;
    for (;; ++frame) {
        if (g_stop.load()) { break; }
        if (measure::now() - loop_begin_qpc >= budget_ticks) { break; }
        if (frames.size() >= frame_budget) { budget_reached = true; break; }

        frames.push_back(FrameRecord{});
        FrameRecord& record = frames.back();
        record.index = frame;
        record.list_offset = list_records.size();
        list_records.resize(list_records.size() + o.lists);
        record.clock_point = clock_points.empty() ? 0u : static_cast<unsigned>(clock_points.size() - 1);
        record.begin_qpc = measure::now();

        if (waitable_handle) {
            record.waitable_begin = measure::now();
            WaitForSingleObjectEx(waitable_handle, 1000, TRUE);
            record.waitable_end = measure::now();
        }
        // The fence wait a game does: the frame started L frames ago must have finished before this one is
        // recorded into the same slot ring.
        if (frame >= o.latency) {
            const unsigned retired = frame - o.latency;
            const unsigned retired_slot = retired % slots();
            bool timed_out = false;
            hr = wait_slot(retired_slot, frames[retired].fence_value, record.wait_begin, record.wait_end, timed_out);
            record.retired = retired;
            record.retired_valid = true;
            if (timed_out) { wait_timed_out = true; failure_where = "fence wait"; failure = hr; break; }
            if (FAILED(hr)) { failure_where = "fence wait"; failure = hr; break; }
            hr = read_timestamps(retired, retired_slot);
            if (FAILED(hr)) { failure_where = "timestamp readback"; failure = hr; break; }
            // The retired frame is one more bracket on the clock's origin, from data already in hand: its
            // first list cannot have started before it was submitted, and its last cannot have ended after
            // the wait that just returned. Needed only for a fitted clock, free to collect either way.
            if (frames[retired].timestamps_valid) {
                const ListRecord* retired_lists = &list_records[frames[retired].list_offset];
                add_clock_bracket(retired_lists[0].exec_qpc, record.wait_end, retired_lists[0].gpu_begin,
                                  retired_lists[o.lists - 1].gpu_end);
            }
        }
        if (cpu_work_ticks) {
            const Qpc until = measure::now() + cpu_work_ticks;
            volatile unsigned sink_value = 0;
            while (measure::now() < until) { for (unsigned i = 0; i < 64; ++i) { sink_value = sink_value * 1664525u + 1013904223u; } }
        }
        record.cpu_work_end = measure::now();

        const unsigned slot = frame % slots();
        for (auto& allocator : frame_slots[slot].allocators) {
            hr = allocator->Reset();
            if (FAILED(hr)) { break; }
        }
        if (FAILED(hr)) { failure_where = "allocator reset"; failure = hr; break; }
        const unsigned back_buffer = chain->GetCurrentBackBufferIndex();
        if (o.submit == Options::Submit::Interleaved) {
            for (unsigned k = 0; k < o.lists; ++k) {
                const Qpc record_begin = measure::now();
                hr = record_frame_list(frame, slot, k, back_buffer);
                const Qpc record_end = measure::now();
                record.record_ticks += record_end - record_begin;
                if (FAILED(hr)) { failure_where = "command list recording"; failure = hr; break; }
                ListRecord& l = list_records[record.list_offset + k];
                ID3D12CommandList* submit[]{frame_slots[slot].lists[k].Get()};
                l.exec_qpc = measure::now();
                queue->ExecuteCommandLists(1, submit);
                l.exec_done_qpc = measure::now();
                record.execute_ticks += l.exec_done_qpc - l.exec_qpc;
            }
            record.record_cpu_ticks = record.record_ticks;
            if (FAILED(hr)) { failure = hr; break; }
        } else {
            hr = record_all(frame, slot, back_buffer, record.record_ticks, record.record_cpu_ticks);
            if (FAILED(hr)) { failure_where = "command list recording"; failure = hr; break; }
            if (o.submit == Options::Submit::Batch) {
                // All K lists in one call. Every list gets the same submission timestamps, because that is the
                // truth: one call put all of them on the queue.
                ID3D12CommandList* batch[64]{};
                for (unsigned k = 0; k < o.lists; ++k) { batch[k] = frame_slots[slot].lists[k].Get(); }
                const Qpc exec_begin = measure::now();
                queue->ExecuteCommandLists(o.lists, batch);
                const Qpc exec_end = measure::now();
                record.execute_ticks = exec_end - exec_begin;
                for (unsigned k = 0; k < o.lists; ++k) {
                    ListRecord& l = list_records[record.list_offset + k];
                    l.exec_qpc = exec_begin;
                    l.exec_done_qpc = exec_end;
                }
            } else {
                for (unsigned k = 0; k < o.lists; ++k) {
                    ListRecord& l = list_records[record.list_offset + k];
                    ID3D12CommandList* submit[]{frame_slots[slot].lists[k].Get()};
                    l.exec_qpc = measure::now();
                    queue->ExecuteCommandLists(1, submit);
                    l.exec_done_qpc = measure::now();
                    record.execute_ticks += l.exec_done_qpc - l.exec_qpc;
                }
            }
        }

        record.fence_value = next_fence_value++;
        record.signal_begin = measure::now();
        hr = queue->Signal(frame_slots[slot].fence.Get(), record.fence_value);
        record.signal_end = measure::now();
        if (FAILED(hr)) { failure_where = "queue signal"; failure = hr; break; }
        signalled_frames = frame + 1;

        record.present_begin = measure::now();
        record.present_hr = chain->Present(o.present_interval, present_flags);
        record.present_end = measure::now();
        if (FAILED(record.present_hr)) {
            failure_where = "Present";
            failure = record.present_hr;
            removed_reason = device->GetDeviceRemovedReason();
            break;
        }
        if (o.frame_statistics) {
            DXGI_FRAME_STATISTICS stats{};
            if (SUCCEEDED(chain->GetFrameStatistics(&stats))) {
                record.present_count = stats.PresentCount;
                record.present_sync_qpc = stats.SyncQPCTime.QuadPart;
            }
        }
        record.end_qpc = measure::now();

        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { escape_pressed = true; g_stop.store(true); }
        if ((frame + 1) % o.calibrate_every == 0) { take_clock_point(); }
    }
    loop_end_qpc = measure::now();
    // Drain: every frame whose Signal was issued and whose fence the loop never waited on is retired here, so
    // the GPU is idle before the swap chain goes away and the last frames' timestamps are read. The drain wait
    // is kept in its own fields, because wait_begin/wait_end hold the wait that frame performed for an earlier
    // frame and the wake latency of the last measured frames is read from exactly those.
    const unsigned submitted = static_cast<unsigned>(frames.size());
    const unsigned read_through = submitted > o.latency ? submitted - o.latency : 0;
    for (unsigned retired = read_through; retired < signalled_frames; ++retired) {
        if (retired >= frames.size()) { break; }
        const unsigned retired_slot = retired % slots();
        Qpc begin = 0, end = 0;
        bool timed_out = false;
        const HRESULT drained = wait_slot(retired_slot, frames[retired].fence_value, begin, end, timed_out);
        frames[retired].drained = true;
        frames[retired].drain_begin = begin;
        frames[retired].drain_end = end;
        if (SUCCEEDED(drained)) {
            read_timestamps(retired, retired_slot);
        } else if (SUCCEEDED(failure)) {
            wait_timed_out = wait_timed_out || timed_out;
            failure_where = "drain";
            failure = drained;
        }
    }
    take_clock_point();
    if (FAILED(failure) && SUCCEEDED(removed_reason)) { removed_reason = device->GetDeviceRemovedReason(); }
    return failure;
}

void Client::analyse() {
    d.seconds = measure::qpc_to_ms(loop_end_qpc - loop_begin_qpc, qpc_frequency) / 1000.0;
    if (frames.empty() || clock_points.empty()) { return; }
    // Steady state: the warm-up frames at the front and the frames retired by the drain at the back are left
    // out, so the distributions describe the loop and not its ends.
    const size_t total = frames.size();
    size_t begin = o.warmup < total ? o.warmup : total - 1;
    size_t end = total > o.latency ? total - o.latency : total;
    if (end <= begin) { begin = 0; end = total; }
    d.measured = end - begin;

    std::vector<double> interval, fence_wait, waitable_wait, cpu_work, record, record_cpu, execute, signal, present, frame_cpu;
    std::vector<double> gpu_busy, intra_total, intra_each, inter_gap, submit, wake, wake_blocking, span;
    std::vector<double> idle_total, idle_awaiting, idle_after;
    const auto ms = [&](Qpc ticks) { return measure::qpc_to_ms(ticks, qpc_frequency); };
    double gpu_busy_total = 0.0;
    size_t gpu_busy_frames = 0;
    for (size_t i = begin; i < end; ++i) {
        const FrameRecord& f = frames[i];
        if (i > begin) { interval.push_back(ms(f.begin_qpc - frames[i - 1].begin_qpc)); }
        if (f.retired_valid) { fence_wait.push_back(ms(f.wait_end - f.wait_begin)); }
        if (f.waitable_end) { waitable_wait.push_back(ms(f.waitable_end - f.waitable_begin)); }
        cpu_work.push_back(ms(f.cpu_work_end - (f.retired_valid ? f.wait_end : f.begin_qpc)));
        record.push_back(ms(f.record_ticks));
        record_cpu.push_back(ms(f.record_cpu_ticks));
        execute.push_back(ms(f.execute_ticks));
        signal.push_back(ms(f.signal_end - f.signal_begin));
        present.push_back(ms(f.present_end - f.present_begin));
        frame_cpu.push_back(ms(f.end_qpc - f.begin_qpc));
        if (!f.timestamps_valid) { continue; }
        const measure::Clock& c = clock_points[f.clock_point < clock_points.size() ? f.clock_point : 0].clock;
        const ListRecord* lists = &list_records[f.list_offset];
        UINT64 busy = 0, gaps = 0;
        for (unsigned k = 0; k < o.lists; ++k) {
            busy += lists[k].gpu_end - lists[k].gpu_begin;
            if (k) {
                const UINT64 begin_tick = lists[k].gpu_begin;
                const UINT64 previous_end = lists[k - 1].gpu_end;
                const double gap = begin_tick >= previous_end ? c.gpu_ticks_to_ms(begin_tick - previous_end)
                                                              : -c.gpu_ticks_to_ms(previous_end - begin_tick);
                intra_each.push_back(gap);
                if (begin_tick >= previous_end) { gaps += begin_tick - previous_end; }
            }
            submit.push_back(ms(c.to_qpc(lists[k].gpu_begin) - lists[k].exec_qpc));
        }
        gpu_busy.push_back(c.gpu_ticks_to_ms(busy));
        gpu_busy_total += c.gpu_ticks_to_ms(busy);
        ++gpu_busy_frames;
        intra_total.push_back(c.gpu_ticks_to_ms(gaps));
        // The attribution the whole client exists for. Every window in which the pipe had nothing running is
        // split in two: the part before the next piece of work had even been submitted (a CPU-side cost: the
        // fence wait, the recording, the Present) and the part after it was submitted and the GPU still had
        // not started it (a driver, KMD or scheduler cost). The transition into a frame uses the previous
        // frame's last list, so the frame boundary is included. Both ends are mapped with this frame's clock
        // point; consecutive calibration points differ by far less than these gaps.
        {
            const bool have_previous = i > begin && frames[i - 1].timestamps_valid;
            const UINT64 previous_frame_end = have_previous
                ? list_records[frames[i - 1].list_offset + o.lists - 1].gpu_end : 0;
            double awaiting_sum = 0.0, after_sum = 0.0, idle_sum = 0.0;
            for (unsigned k = 0; k < o.lists; ++k) {
                if (!k && !have_previous) { continue; }
                const UINT64 previous_end = k ? lists[k - 1].gpu_end : previous_frame_end;
                const Qpc previous_end_qpc = c.to_qpc(previous_end);
                const Qpc start_qpc = c.to_qpc(lists[k].gpu_begin);
                if (start_qpc <= previous_end_qpc) { continue; }
                const Qpc submitted_qpc = lists[k].exec_done_qpc < previous_end_qpc ? previous_end_qpc
                    : (lists[k].exec_done_qpc > start_qpc ? start_qpc : lists[k].exec_done_qpc);
                idle_sum += ms(start_qpc - previous_end_qpc);
                awaiting_sum += ms(submitted_qpc - previous_end_qpc);
                after_sum += ms(start_qpc - submitted_qpc);
            }
            if (have_previous) {
                idle_total.push_back(idle_sum);
                idle_awaiting.push_back(awaiting_sum);
                idle_after.push_back(after_sum);
            }
        }
        span.push_back(c.gpu_ticks_to_ms(lists[o.lists - 1].gpu_end - lists[0].gpu_begin));
        // Wake latency: the frame's last GPU timestamp against the QPC at which the CPU's wait for that same
        // frame returned. The wait happens L frames later, so the value is taken from that frame's record.
        const size_t waiter = i + o.latency;
        if (waiter < frames.size() && frames[waiter].retired_valid && frames[waiter].retired == f.index) {
            const double value = ms(frames[waiter].wait_end - c.to_qpc(lists[o.lists - 1].gpu_end));
            wake.push_back(value);
            // Only a wait that actually blocked measures a wake: when the loop is bound by Present or by the
            // CPU, the fence has long completed before the wait is reached and the number is the CPU's
            // lateness, not the driver's. gpu_end_to_wake_blocking keeps the waits that blocked.
            if (ms(frames[waiter].wait_end - frames[waiter].wait_begin) > 0.05) { wake_blocking.push_back(value); }
        } else if (f.drained && f.drain_end) {
            wake.push_back(ms(f.drain_end - c.to_qpc(lists[o.lists - 1].gpu_end)));
        }
        // The gap the investigation is about: the GPU's idle time between the last list of this frame and the
        // first list of the next.
        if (i + 1 < end && frames[i + 1].timestamps_valid) {
            const ListRecord* next = &list_records[frames[i + 1].list_offset];
            const UINT64 this_end = lists[o.lists - 1].gpu_end;
            inter_gap.push_back(next[0].gpu_begin >= this_end ? c.gpu_ticks_to_ms(next[0].gpu_begin - this_end)
                                                              : -c.gpu_ticks_to_ms(this_end - next[0].gpu_begin));
        }
    }
    d.interval = measure::summarize(interval);
    d.fence_wait = measure::summarize(fence_wait);
    d.waitable_wait = measure::summarize(waitable_wait);
    d.cpu_work = measure::summarize(cpu_work);
    d.record = measure::summarize(record);
    d.record_cpu = measure::summarize(record_cpu);
    d.execute = measure::summarize(execute);
    d.signal = measure::summarize(signal);
    d.present = measure::summarize(present);
    d.frame_cpu = measure::summarize(frame_cpu);
    d.gpu_busy = measure::summarize(gpu_busy);
    d.intra_gap_total = measure::summarize(intra_total);
    d.intra_gap_each = measure::summarize(intra_each);
    d.inter_gap = measure::summarize(inter_gap);
    d.submit_latency = measure::summarize(submit);
    d.wake_latency = measure::summarize(wake);
    d.wake_blocking = measure::summarize(wake_blocking);
    d.gpu_span = measure::summarize(span);
    d.idle_total = measure::summarize(idle_total);
    d.idle_awaiting_submission = measure::summarize(idle_awaiting);
    d.idle_after_submission = measure::summarize(idle_after);
    if (d.interval.mean > 0.0) { d.fps = 1000.0 / d.interval.mean; }
    // Is the GPU timeline on the same scale as the clock everything else is measured with? The means are used
    // because the accounting identity is per frame and survives averaging, while a p50 of a sum would not.
    d.gpu_accounted_ms = d.gpu_busy.mean + d.idle_total.mean;
    d.gpu_over_interval = measure::timestamp_scale_ratio(d.gpu_busy.mean, d.idle_total.mean, d.interval.mean);
    d.timestamp_scale_ok = measure::timestamp_scale_ok(d.gpu_over_interval);
    // The bracket is reported whether or not it was used; when it was not, this is the only place that fills it.
    if (!clock_fit.applied) { compute_clock_bracket(); }
    // A negative latency only means something when it is larger than the clock mapping's own uncertainty: a
    // fitted origin sits at the tightest wake-side bound of its window, so a later frame whose wake was tighter
    // still reads a few tens of microseconds below zero on a sound run. Milliseconds of it are the real signal.
    d.negative_tolerance_ms = -(0.01 + (clock_fit.applied ? clock_fit.bracket_ms : 0.0));
    for (double value : submit) { if (value < d.negative_tolerance_ms) { ++d.negative_submit; } }
    for (double value : wake_blocking) { if (value < d.negative_tolerance_ms) { ++d.negative_wake; } }
    if (gpu_busy_frames && o.gpu_ms > 0.0) {
        d.gpu_busy_error_pct = 100.0 * (gpu_busy_total / static_cast<double>(gpu_busy_frames) - o.gpu_ms) / o.gpu_ms;
    }
    // Drift between the first and the last calibration point: how far the GPU clock moved against QPC over
    // the run, in parts per million. A large value would mean the per-frame mapping needs more points.
    // Under a fitted clock every point carries the same origin by construction, so there is nothing to measure
    // and the field says so (null) instead of reporting a drift of zero that was never observed.
    if (clock_fit.applied) {
        d.clock_drift_ppm = std::numeric_limits<double>::quiet_NaN();
    } else if (clock_points.size() >= 2) {
        const ClockPoint& a = clock_points.front();
        const ClockPoint& b = clock_points.back();
        const double cpu_span = static_cast<double>(static_cast<long long>(b.clock.cpu_ticks - a.clock.cpu_ticks)) / static_cast<double>(qpc_frequency);
        const double gpu_span = static_cast<double>(static_cast<long long>(b.clock.gpu_ticks - a.clock.gpu_ticks)) / static_cast<double>(gpu_frequency);
        if (cpu_span > 0.001) { d.clock_drift_ppm = 1e6 * (gpu_span - cpu_span) / cpu_span; }
    }
}

bool Client::write_json() const {
    if (o.out.empty()) { return true; }
    std::FILE* file = nullptr;
    if (fopen_s(&file, o.out.c_str(), "wb") || !file) {
        std::fprintf(stderr, "frameloop: cannot write %s\n", o.out.c_str());
        return false;
    }
    const auto ms = [&](Qpc ticks) { return measure::number(measure::qpc_to_ms(ticks, qpc_frequency)); };
    char description[256]{};
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, description, sizeof(description) - 1, nullptr, nullptr);
    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"client\": \"amdgpu_wddm_frameloop\",\n  \"schema\": 2,\n");
    std::fprintf(file, "  \"command_line\": \"%s\",\n", measure::escape(o.command_line).c_str());
    std::fprintf(file, "  \"config\": {\"seconds\": %u, \"gpu_ms\": %s, \"lists\": %u, \"latency\": %u, \"buffers\": %u,\n"
                       "              \"present_interval\": %u, \"tearing_requested\": %s, \"frame_latency_waitable\": %u,\n"
                       "              \"cpu_ms\": %s, \"groups\": %u, \"threads_per_group\": %u, \"calibrate_every\": %u,\n"
                       "              \"warmup\": %u, \"frame_statistics\": %s, \"timestamps\": %s, \"debug_layer\": %s,\n"
                       "              \"record_threads\": %u, \"effective_record_threads\": %u, \"submit\": \"%s\",\n"
                       "              \"ts_hz\": %llu},\n",
                 o.seconds, measure::number(o.gpu_ms).c_str(), o.lists, o.latency, o.buffers, o.present_interval,
                 o.tearing ? "true" : "false", o.waitable, measure::number(o.cpu_ms).c_str(), o.groups,
                 kThreadsPerGroup, o.calibrate_every, o.warmup, o.frame_statistics ? "true" : "false",
                 o.no_timestamps ? "false" : "true", o.debug_layer ? "true" : "false",
                 o.record_threads, effective_record_threads, submit_name(o.submit),
                 static_cast<unsigned long long>(o.ts_hz));
    std::fprintf(file, "  \"host\": {\"qpc_frequency\": %lld, \"first_qpc\": %lld, \"gpu_timestamp_frequency\": %llu,\n"
                       "            \"gpu_timestamp_frequency_reported\": %llu, \"timestamp_frequency_overridden\": %s,\n"
                       "            \"dpi_awareness\": \"%s\", \"clock_points\": %zu, \"clock_drift_ppm\": %s},\n",
                 qpc_frequency, first_qpc, static_cast<unsigned long long>(gpu_frequency),
                 static_cast<unsigned long long>(reported_gpu_frequency),
                 (o.ts_hz && !o.no_timestamps) ? "true" : "false", dpi_awareness,
                 clock_points.size(), measure::number(d.clock_drift_ppm, 2).c_str());
    // The clock the GPU-side numbers were read with: what the driver said, what was used, and - when the
    // origin had to be fitted - how wide the bracket was, which is the uncertainty of every number that
    // compares a GPU timestamp with a CPU one.
    std::fprintf(file, "  \"clock_fit\": {\"applied\": %s, \"samples\": %u, \"bracket_ms\": %s,\n"
                       "                 \"origin_qpc\": %lld, \"lower_qpc\": %lld, \"upper_qpc\": %lld,\n"
                       "                 \"applied_from_clock_point\": %zu, \"driver_point_offset_ms\": %s,\n"
                       "                 \"refits\": %u, \"refits_clamped\": %u, \"drift_allowance_ppm\": %s,\n"
                       "                 \"all_time_bounds_crossed\": %s,\n"
                       "                 \"driver_gpu_ticks\": %llu, \"driver_cpu_ticks\": %llu},\n",
                 clock_fit.applied ? "true" : "false", clock_fit.samples,
                 measure::number(clock_fit.bracket_ms, 4).c_str(), clock_fit.origin, clock_fit.lower,
                 clock_fit.upper, clock_fit.applied_from_point,
                 measure::number(clock_fit.kmd_point_offset_ms, 1).c_str(),
                 fit_windows, fit_clamped, measure::number(kDriftPpm, 0).c_str(),
                 clock_fit.all_time_crossed ? "true" : "false",
                 static_cast<unsigned long long>(clock_fit.kmd_gpu_ticks),
                 static_cast<unsigned long long>(clock_fit.kmd_cpu_ticks));
    std::fprintf(file, "  \"adapter\": {\"description\": \"%s\", \"vendor_id\": %u, \"device_id\": %u,\n"
                       "               \"luid\": \"%ld:%lu\", \"dedicated_video_mb\": %llu, \"shared_system_mb\": %llu,\n"
                       "               \"software\": %s, \"feature_level\": \"%x\"},\n",
                 measure::escape(description).c_str(), adapter_desc.VendorId, adapter_desc.DeviceId,
                 adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart,
                 static_cast<unsigned long long>(adapter_desc.DedicatedVideoMemory >> 20),
                 static_cast<unsigned long long>(adapter_desc.SharedSystemMemory >> 20),
                 (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false",
                 static_cast<unsigned>(feature_level));
    std::fprintf(file, "  \"swapchain\": {\"format\": \"%s\", \"buffers\": %u, \"width\": %u, \"height\": %u,\n"
                       "                 \"swap_effect\": \"FLIP_DISCARD\", \"flags\": %u, \"present_flags\": %u,\n"
                       "                 \"tearing_supported\": %s},\n",
                 kBackBufferFormatName, o.buffers, width, height, chain_flags, present_flags,
                 tearing_supported ? "true" : "false");
    std::fprintf(file, "  \"calibration\": {\"groups\": %u, \"iterations\": %u, \"target_ms_per_dispatch\": %s,\n"
                       "                   \"measured_ms_per_dispatch\": %s, \"slope_ms_per_iteration\": %s,\n"
                       "                   \"intercept_ms\": %s, \"rounds\": [",
                 o.groups, iterations, measure::number(calibration_target_ms, 4).c_str(),
                 measure::number(calibration_measured_ms, 4).c_str(), measure::number(calibration_slope_ms, 9).c_str(),
                 measure::number(calibration_intercept_ms, 4).c_str());
    for (size_t i = 0; i < calibration_rounds.size(); ++i) {
        std::fprintf(file, "%s{\"kind\": \"%s\", \"iterations\": %u, \"measured_ms\": %s}", i ? ", " : "",
                     calibration_rounds[i].kind, calibration_rounds[i].iterations,
                     measure::number(calibration_rounds[i].measured_ms, 4).c_str());
    }
    std::fprintf(file, "],\n                   \"sustained_passes\": %u, \"dispatch_floor_ms\": %s,"
                       " \"frame_floor_ms\": %s, \"gpu_busy_error_pct\": %s},\n",
                 calibration_passes, measure::number(dispatch_floor_ms, 4).c_str(),
                 measure::number(dispatch_floor_ms * static_cast<double>(o.lists), 4).c_str(),
                 measure::number(d.gpu_busy_error_pct, 2).c_str());

    std::fprintf(file, "  \"summary\": {\n");
    std::fprintf(file, "    \"frames_submitted\": %zu, \"frames_measured\": %zu, \"seconds\": %s, \"fps\": %s,\n",
                 frames.size(), d.measured, measure::number(d.seconds).c_str(), measure::number(d.fps, 2).c_str());
    std::fprintf(file, "    \"invalid_timestamp_frames\": %u, \"frame_budget\": %zu, \"budget_reached\": %s,\n",
                 invalid_timestamp_frames, frame_budget, budget_reached ? "true" : "false");
    std::fprintf(file, "    \"consistency\": {\"gpu_accounted_ms\": %s, \"interval_ms\": %s,\n"
                       "                    \"gpu_over_interval\": %s, \"timestamp_scale_ok\": %s,\n"
                       "                    \"negative_submit_latency\": %zu, \"negative_wake_latency\": %zu,\n"
                       "                    \"negative_tolerance_ms\": %s, \"cross_clock_usable\": %s},\n",
                 measure::number(d.gpu_accounted_ms).c_str(), measure::number(d.interval.mean).c_str(),
                 measure::number(d.gpu_over_interval, 4).c_str(), d.timestamp_scale_ok ? "true" : "false",
                 d.negative_submit, d.negative_wake, measure::number(d.negative_tolerance_ms, 4).c_str(),
                 // Also false when every refit was held back by the drift allowance: the origin then never had
                 // a tight bound to follow, which is what a Present-bound loop looks like (the fence completes
                 // long before the wait, so the wake-side bound is loose too).
                 (d.timestamp_scale_ok && d.negative_submit < 2 && d.negative_wake < 2 &&
                  !(fit_windows && fit_clamped == fit_windows)) ? "true" : "false");
    std::fprintf(file, "    \"cpu_ms\": {\n");
    measure::write_summary(file, "frame_interval", d.interval, ",");
    measure::write_summary(file, "fence_wait", d.fence_wait, ",");
    measure::write_summary(file, "waitable_wait", d.waitable_wait, ",");
    measure::write_summary(file, "cpu_work", d.cpu_work, ",");
    measure::write_summary(file, "record", d.record, ",");
    measure::write_summary(file, "record_cpu_total", d.record_cpu, ",");
    measure::write_summary(file, "execute_total", d.execute, ",");
    measure::write_summary(file, "signal", d.signal, ",");
    measure::write_summary(file, "present", d.present, ",");
    measure::write_summary(file, "frame_cpu_total", d.frame_cpu, "");
    std::fprintf(file, "    },\n    \"gpu_ms\": {\n");
    measure::write_summary(file, "busy", d.gpu_busy, ",");
    measure::write_summary(file, "span_first_to_last_list", d.gpu_span, ",");
    measure::write_summary(file, "intra_frame_gap_total", d.intra_gap_total, ",");
    measure::write_summary(file, "intra_frame_gap_each", d.intra_gap_each, ",");
    measure::write_summary(file, "inter_frame_gap", d.inter_gap, ",");
    measure::write_summary(file, "idle_per_frame", d.idle_total, ",");
    measure::write_summary(file, "idle_awaiting_submission_per_frame", d.idle_awaiting_submission, ",");
    measure::write_summary(file, "idle_after_submission_per_frame", d.idle_after_submission, "");
    std::fprintf(file, "    },\n    \"latency_ms\": {\n");
    measure::write_summary(file, "submit_to_gpu_start", d.submit_latency, ",");
    measure::write_summary(file, "gpu_end_to_wake", d.wake_latency, ",");
    measure::write_summary(file, "gpu_end_to_wake_blocking", d.wake_blocking, "");
    std::fprintf(file, "    }\n  },\n");

    // Where the wall time went, as the phases ended. `ms` is the phase's own cost, `at_ms` its end since the
    // top of main, so a harness can size a task limit from a run instead of guessing at one.
    std::fprintf(file, "  \"timeline\": [");
    {
        Qpc previous = process_qpc;
        for (size_t i = 0; i < timeline.size(); ++i) {
            std::fprintf(file, "%s{\"phase\": \"%s\", \"ms\": %s, \"at_ms\": %s}", i ? ",\n                " : "",
                         timeline[i].phase, ms(timeline[i].at - previous).c_str(),
                         ms(timeline[i].at - process_qpc).c_str());
            previous = timeline[i].at;
        }
    }
    std::fprintf(file, "],\n");
    std::fprintf(file, "  \"clock_calibration\": [");
    for (size_t i = 0; i < clock_points.size(); ++i) {
        std::fprintf(file, "%s{\"qpc\": %lld, \"gpu_ticks\": %llu, \"cpu_ticks\": %llu, \"cost_ms\": %s}",
                     i ? ",\n                        " : "", clock_points[i].taken_qpc,
                     static_cast<unsigned long long>(clock_points[i].clock.gpu_ticks),
                     static_cast<unsigned long long>(clock_points[i].clock.cpu_ticks),
                     ms(clock_points[i].cost_ticks).c_str());
    }
    std::fprintf(file, "],\n");

    // Raw records, one frame per line, so a run can be aligned with ETW or a register timeline later. All
    // times are raw QPC ticks and raw GPU timestamp ticks; nothing here is pre-reduced.
    const size_t raw = frames.size() < o.raw_frames ? frames.size() : o.raw_frames;
    std::fprintf(file, "  \"frames_note\": \"qpc ticks and GPU timestamp ticks as measured; lists[] is [exec_qpc, exec_done_qpc, gpu_begin, gpu_end] per command list\",\n");
    std::fprintf(file, "  \"frames\": [\n");
    for (size_t i = 0; i < raw; ++i) {
        const FrameRecord& f = frames[i];
        std::fprintf(file, "    {\"i\": %u, \"begin\": %lld, \"wait\": [%lld, %lld], \"cpu_work_end\": %lld,"
                           " \"record_ticks\": %lld, \"record_cpu_ticks\": %lld, \"execute_ticks\": %lld, \"signal\": [%lld, %lld],"
                           " \"present\": [%lld, %lld], \"end\": %lld, \"present_hr\": \"%08lx\","
                           " \"retired\": %d, \"drained\": %s, \"ts_valid\": %s, \"clock_point\": %u",
                     f.index, f.begin_qpc, f.wait_begin, f.wait_end, f.cpu_work_end, f.record_ticks,
                     f.record_cpu_ticks, f.execute_ticks, f.signal_begin, f.signal_end, f.present_begin, f.present_end, f.end_qpc,
                     static_cast<unsigned long>(f.present_hr), f.retired_valid ? static_cast<int>(f.retired) : -1,
                     f.drained ? "true" : "false", f.timestamps_valid ? "true" : "false", f.clock_point);
        if (f.waitable_end) { std::fprintf(file, ", \"waitable\": [%lld, %lld]", f.waitable_begin, f.waitable_end); }
        if (o.frame_statistics) { std::fprintf(file, ", \"present_count\": %llu, \"sync_qpc\": %lld",
                                              static_cast<unsigned long long>(f.present_count), f.present_sync_qpc); }
        std::fprintf(file, ", \"lists\": [");
        for (unsigned k = 0; k < o.lists; ++k) {
            const ListRecord& l = list_records[f.list_offset + k];
            std::fprintf(file, "%s[%lld, %lld, %llu, %llu]", k ? ", " : "", l.exec_qpc, l.exec_done_qpc,
                         static_cast<unsigned long long>(l.gpu_begin), static_cast<unsigned long long>(l.gpu_end));
        }
        std::fprintf(file, "]}%s\n", i + 1 < raw ? "," : "");
    }
    std::fprintf(file, "  ],\n");
    std::fprintf(file, "  \"result\": {\"status\": \"%s\", \"failure\": \"%s\", \"failure_where\": \"%s\",\n"
                       "              \"device_removed_reason\": \"%s\", \"escape\": %s, \"wait_timed_out\": %s,\n"
                       "              \"stop_requested\": %s}\n",
                 SUCCEEDED(failure) && !invalid_timestamp_frames ? "PASS" : "FAIL",
                 measure::escape(hr_text(failure)).c_str(), failure_where,
                 measure::escape(hr_text(removed_reason)).c_str(), escape_pressed ? "true" : "false",
                 wait_timed_out ? "true" : "false", g_stop.load() ? "true" : "false");
    std::fprintf(file, "}\n");
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

void Client::teardown() {
    stop_recorders();
    // The GPU must be idle before the swap chain and its buffers go away; loop() already drained, this is the
    // belt for the paths that failed before the drain.
    if (queue && !frame_slots.empty()) {
        for (unsigned s = 0; s < frame_slots.size(); ++s) {
            if (!frame_slots[s].fence) { continue; }
            const UINT64 value = next_fence_value++;
            if (SUCCEEDED(queue->Signal(frame_slots[s].fence.Get(), value)) &&
                SUCCEEDED(frame_slots[s].fence->SetEventOnCompletion(value, frame_slots[s].event))) {
                WaitForSingleObject(frame_slots[s].event, 2000);
            }
        }
    }
    if (query_readback && query_data) { const D3D12_RANGE none{0, 0}; query_readback->Unmap(0, &none); query_data = nullptr; }
    for (Slot& s : frame_slots) { if (s.event) { CloseHandle(s.event); s.event = nullptr; } }
    frame_slots.clear();
    back_buffers.clear();
    chain.Reset();
    if (window) { DestroyWindow(window); window = nullptr; }
    if (window_class_registered) { UnregisterClassW(L"amdgpu_wddm_frameloop", GetModuleHandleW(nullptr)); window_class_registered = false; }
}

int Client::run() {
    const struct { const char* where; HRESULT (Client::*step)(); } steps[]{
        {"runtime", &Client::load_runtime}, {"adapter", &Client::pick_adapter}, {"device", &Client::create_device},
        {"window", &Client::create_window}, {"swap chain", &Client::create_swap_chain},
        {"pipeline", &Client::create_pipeline}, {"frames", &Client::create_frames},
        {"recorders", &Client::start_recorders},
    };
    qpc_frequency = measure::frequency();
    process_qpc = g_process_qpc ? g_process_qpc : measure::now();
    mark("start");
    for (const auto& step : steps) {
        const HRESULT hr = (this->*step.step)();
        mark(step.where);
        if (FAILED(hr)) {
            std::fprintf(stderr, "frameloop: %s failed: %s\n", step.where, hr_text(hr).c_str());
            failure = hr;
            failure_where = step.where;
            g_writing.store(true);
            write_json();
            return (!std::strcmp(step.where, "adapter") || !std::strcmp(step.where, "device")) ? 3 : 1;
        }
    }
    std::printf("frameloop: runtime=System32 adapter=\"%ls\" vendor=%04x device=%04x fl=%x %ux%u %s %u buffers,"
                " submit %s, %u recording thread%s\n",
                adapter_desc.Description, adapter_desc.VendorId, adapter_desc.DeviceId,
                static_cast<unsigned>(feature_level), width, height, kBackBufferFormatName, o.buffers,
                submit_name(o.submit), effective_record_threads, effective_record_threads == 1 ? "" : "s");
    std::fflush(stdout);
    take_clock_point();
    HRESULT hr = calibrate();
    mark("calibration");
    if (FAILED(hr)) {
        std::fprintf(stderr, "frameloop: calibration failed: %s\n", hr_text(hr).c_str());
        failure = hr;
        failure_where = "calibration";
        g_writing.store(true);
        write_json();
        return 1;
    }
    hr = calibrate_sustained();
    mark("sustained calibration");
    if (FAILED(hr)) {
        std::fprintf(stderr, "frameloop: sustained calibration failed: %s\n", hr_text(hr).c_str());
        failure = hr;
        failure_where = "sustained calibration";
        teardown();
        g_writing.store(true);
        write_json();
        return 1;
    }
    std::printf("frameloop: %u groups x %u threads, %u iterations, %s ms per dispatch (target %s), %u lists,"
                " %u sustained calibration passes\n",
                o.groups, kThreadsPerGroup, iterations, measure::number(calibration_measured_ms, 4).c_str(),
                measure::number(calibration_target_ms, 4).c_str(), o.lists, calibration_passes);
    std::fflush(stdout);
    // The two-sided fit, immediately before the measured loop and from its own short pass of dispatches that
    // have the queue to themselves. Both bounds are tight only when nothing else is in flight, and they have to
    // come from one short window: over a whole run a drifting clock makes an early lower bound and a late upper
    // bound cross, which is what the first version of this did (136 samples, bounds crossed).
    if (o.ts_hz && !o.no_timestamps) {
        reset_clock_bracket();
        double ignored = 0.0;
        const HRESULT fit_hr = measure_dispatch(1, 16, ignored);
        if (FAILED(fit_hr)) {
            std::fprintf(stderr, "frameloop: the clock fit pass failed (%s)\n", hr_text(fit_hr).c_str());
        }
        apply_clock_fit();
    }
    mark("clock fit");
    loop(static_cast<double>(o.seconds));
    mark("loop");
    analyse();
    teardown();
    mark("teardown");
    g_writing.store(true);
    const bool written = write_json();
    const auto line = [](const char* name, const measure::Summary& s) {
        std::printf("  %-26s p50 %8.3f  p90 %8.3f  p99 %8.3f  max %9.3f  mean %8.3f  n %zu\n",
                    name, s.p50, s.p90, s.p99, s.max, s.mean, s.count);
    };
    // Where the wall time went, phase by phase. A trial sizes its own task limits from this, so it is printed
    // on every run: "27 s for 5 s of measurement" is a question the client should be able to answer itself.
    {
        const Qpc last = timeline.empty() ? process_qpc : timeline.back().at;
        std::printf("frameloop: wall %.3f s:", measure::qpc_to_ms(last - process_qpc, qpc_frequency) / 1000.0);
        Qpc previous = process_qpc;
        for (const auto& m : timeline) {
            if (std::strcmp(m.phase, "start")) {
                std::printf(" %s %.0f,", m.phase, measure::qpc_to_ms(m.at - previous, qpc_frequency));
            }
            previous = m.at;
        }
        std::printf(" all in ms\n");
    }
    std::printf("frameloop: %zu frames in %.3f s, %.2f fps, %zu measured, GPU busy error %+.2f %%%s\n",
                frames.size(), d.seconds, d.fps, d.measured, d.gpu_busy_error_pct,
                (o.gpu_ms > 0.0 && (d.gpu_busy_error_pct > 10.0 || d.gpu_busy_error_pct < -10.0))
                    ? " (OUTSIDE 10 %: see calibration.frame_floor_ms)" : "");
    line("frame interval ms", d.interval);
    line("cpu fence wait ms", d.fence_wait);
    line("cpu record wall ms", d.record);
    line("cpu record all threads ms", d.record_cpu);
    line("cpu execute total ms", d.execute);
    line("cpu signal ms", d.signal);
    line("cpu present ms", d.present);
    line("gpu busy ms", d.gpu_busy);
    line("gpu intra-frame gaps ms", d.intra_gap_total);
    line("gpu inter-frame gap ms", d.inter_gap);
    line("gpu idle per frame ms", d.idle_total);
    line("  of it: awaiting submit", d.idle_awaiting_submission);
    line("  of it: after submit", d.idle_after_submission);
    line("submit to gpu start ms", d.submit_latency);
    line("gpu end to cpu wake ms", d.wake_latency);
    line("  wake, blocking waits", d.wake_blocking);
    if (!o.no_timestamps && d.gpu_over_interval > 0.0) {
        std::printf("frameloop: GPU timeline accounts %.3f ms per frame against a %.3f ms interval, ratio %.4f%s\n",
                    d.gpu_accounted_ms, d.interval.mean, d.gpu_over_interval,
                    d.timestamp_scale_ok ? "" : "  TIMESTAMP SCALE MISMATCH");
        if (d.negative_submit > 1 || d.negative_wake > 1) {
            std::printf("frameloop: %zu negative submit latencies and %zu negative wake latencies: the GPU"
                        " timestamps are not aligned with QPC, so the latencies and the awaiting/after split"
                        " are not readable (the per-frame busy, gaps and idle still are)\n",
                        d.negative_submit, d.negative_wake);
        }
        if (!d.timestamp_scale_ok) {
            std::fprintf(stderr, "frameloop: the reported timestamp frequency (%llu Hz) does not match the counter"
                                 " the GPU writes: every gpu_ms and latency_ms value is scaled by %.4f."
                                 " Read the cpu_ms numbers only, or divide by that ratio.\n",
                         static_cast<unsigned long long>(gpu_frequency), d.gpu_over_interval);
        }
    }
    const bool pass = SUCCEEDED(failure) && !invalid_timestamp_frames && written;
    std::printf("FRAMELOOP %s failure=%s where=%s removed=%s invalid_ts=%u ts_scale=%.4f scale_ok=%d\n",
                pass ? "PASS" : "FAIL", hr_text(failure).c_str(), *failure_where ? failure_where : "-",
                hr_text(removed_reason).c_str(), invalid_timestamp_frames, d.gpu_over_interval,
                d.timestamp_scale_ok ? 1 : 0);
    std::fflush(stdout);
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    g_process_qpc = measure::now();
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr, "%s\n", kUsage);
        return 2;
    }
    if (options.help) { std::printf("%s\n", kUsage); return 0; }
    if (options.selftest) { return selftest(); }
    g_watchdog_out = options.out;
    g_watchdog_seconds = options.seconds;
    HANDLE guard = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    int code = 1;
    {
        Client client(options);
        code = client.run();
    }
    g_stop.store(true);
    if (guard) { WaitForSingleObject(guard, 2000); CloseHandle(guard); }
    return code;
}
