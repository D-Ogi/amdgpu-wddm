// SPDX-License-Identifier: MIT
// Measurement helpers for amdgpu_wddm_frameloop: QPC conversion, a GPU-tick to QPC mapping built from
// ID3D12CommandQueue::GetClockCalibration, and the distribution summary (p50/p90/p99/max/mean) used for every
// reported series. Nothing here touches D3D12, so it can be exercised by the client's --selftest without a
// device.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace measure {

// Ticks of QueryPerformanceCounter. Kept signed so a difference that comes out negative (a GPU timestamp
// mapped before the CPU event that is supposed to precede it) is reported as negative instead of wrapping.
using Qpc = long long;

inline Qpc now() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
inline Qpc frequency() noexcept { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }

// A single clock calibration point: the GPU tick and the QPC tick that the queue reported as simultaneous,
// together with the two frequencies. GPU ticks are converted to the QPC timeline with this, so a GPU interval
// and a CPU interval can be subtracted from each other.
struct Clock {
    UINT64 gpu_ticks{};        // GetClockCalibration's GPU timestamp
    UINT64 cpu_ticks{};        // GetClockCalibration's QPC value at the same instant
    UINT64 gpu_frequency{1};   // GetTimestampFrequency
    Qpc qpc_frequency{1};

    // A GPU timestamp on the QPC timeline. The difference is taken in GPU ticks first and signed, so a
    // timestamp from before the calibration point maps correctly.
    Qpc to_qpc(UINT64 gpu_timestamp) const noexcept {
        const long double delta = static_cast<long double>(static_cast<long long>(gpu_timestamp - gpu_ticks));
        const long double scaled = delta * static_cast<long double>(qpc_frequency) / static_cast<long double>(gpu_frequency ? gpu_frequency : 1);
        return static_cast<Qpc>(cpu_ticks) + static_cast<Qpc>(llroundl(scaled));
    }
    // A GPU tick count as a duration in milliseconds; needs no calibration point, only the GPU frequency.
    double gpu_ticks_to_ms(UINT64 ticks) const noexcept {
        return 1000.0 * static_cast<double>(ticks) / static_cast<double>(gpu_frequency ? gpu_frequency : 1);
    }
};

inline double qpc_to_ms(Qpc ticks, Qpc qpc_frequency) noexcept {
    return 1000.0 * static_cast<double>(ticks) / static_cast<double>(qpc_frequency ? qpc_frequency : 1);
}

// The distribution of one series, as reported in the JSON. An empty series reports zeros with count 0.
struct Summary {
    size_t count{};
    double min{};
    double p50{};
    double p90{};
    double p99{};
    double max{};
    double mean{};
};

// Nearest-rank percentile on the sorted copy: index = ceil(q * n) - 1, clamped. No interpolation, so every
// reported value is a value that actually occurred.
inline double percentile(const std::vector<double>& sorted, double q) noexcept {
    if (sorted.empty()) { return 0.0; }
    const double rank = std::ceil(q * static_cast<double>(sorted.size()));
    size_t index = rank <= 1.0 ? 0 : static_cast<size_t>(rank) - 1;
    if (index >= sorted.size()) { index = sorted.size() - 1; }
    return sorted[index];
}

inline Summary summarize(std::vector<double> values) {
    Summary s{};
    s.count = values.size();
    if (values.empty()) { return s; }
    double total = 0.0;
    for (const double v : values) { total += v; }
    s.mean = total / static_cast<double>(values.size());
    std::sort(values.begin(), values.end());
    s.min = values.front();
    s.max = values.back();
    s.p50 = percentile(values, 0.50);
    s.p90 = percentile(values, 0.90);
    s.p99 = percentile(values, 0.99);
    return s;
}

// ----------------------------------------------------------------------------------------------- vblank
// The display's vertical blanks, as a line fitted to DXGI_FRAME_STATISTICS. SyncRefreshCount counts vblanks and
// SyncQPCTime is the QPC of the vblank that was counted, so the pairs lie on a line whose slope is the refresh
// period. Least squares over the pairs gives the period and an origin, and those turn any QPC into a phase
// inside the refresh interval. The point of it: an event that waits for a vblank lands at a phase close to
// zero, while uniform phases mean nothing waited for one.
struct VblankSample {
    UINT64 refresh{};
    Qpc qpc{};
};

struct VblankFit {
    bool ok{};
    unsigned samples{};        // distinct refresh counts the fit used
    double period_qpc{};       // QPC ticks per refresh
    double origin_qpc{};       // the QPC of refresh 0, extrapolated
    double hz{};
    double residual_us{};      // the largest distance of a sample from the fitted line
    unsigned outliers{};       // samples dropped as a whole number of refreshes off their count (see below)
};

// A fit needs this many distinct refresh counts. Below it a line through frame statistics says nothing.
inline constexpr unsigned kVblankSamplesMin = 8;
// A fitted grid is believed only inside this band and this worst-case residual: outside them the samples are
// not one display's refresh grid (a mode change in the middle of a run, or a statistics source that lies).
inline constexpr double kVblankHzMin = 20.0;
inline constexpr double kVblankHzMax = 400.0;
inline constexpr double kVblankResidualMaxUs = 1000.0;

// Least squares of QPC over refresh count. The counts are centred before the sums, so the squares stay small
// however long the machine has been up. False when the samples do not make a rising line.
inline bool fit_vblank_line(const std::vector<VblankSample>& samples, long double& period, long double& origin) {
    long double sum_n = 0.0L, sum_t = 0.0L;
    for (const VblankSample& s : samples) {
        sum_n += static_cast<long double>(s.refresh);
        sum_t += static_cast<long double>(s.qpc);
    }
    const long double count = static_cast<long double>(samples.size());
    const long double mean_n = sum_n / count, mean_t = sum_t / count;
    long double covariance = 0.0L, variance = 0.0L;
    for (const VblankSample& s : samples) {
        const long double dn = static_cast<long double>(s.refresh) - mean_n;
        covariance += dn * (static_cast<long double>(s.qpc) - mean_t);
        variance += dn * dn;
    }
    if (!(variance > 0.0L) || !(covariance > 0.0L)) { return false; }
    period = covariance / variance;
    origin = mean_t - period * mean_n;
    return true;
}

inline long double vblank_off(const VblankSample& s, long double period, long double origin) {
    const long double fitted = origin + period * static_cast<long double>(s.refresh);
    const long double at = static_cast<long double>(s.qpc);
    return fitted > at ? fitted - at : at - fitted;
}

// GetFrameStatistics now and then pairs a SyncQPCTime with the refresh count of the vblank next to it: the sample
// then sits a whole period off the line (16.6 ms at 60 Hz), which refused the grid of two arms of the C45 set
// 184120Z although 1180 other samples sat within 120 us. Such a sample is dropped and the line fitted again, but
// only when it is within the residual band of a whole number of periods off, and only up to 1 % of the samples:
// a sample off by anything else, or more of them, still means these are not one display's grid.
inline VblankFit fit_vblank(std::vector<VblankSample> samples, Qpc qpc_frequency) {
    VblankFit fit{};
    if (qpc_frequency <= 0) { return fit; }
    std::sort(samples.begin(), samples.end(),
              [](const VblankSample& a, const VblankSample& b) { return a.refresh < b.refresh; });
    samples.erase(std::unique(samples.begin(), samples.end(),
                              [](const VblankSample& a, const VblankSample& b) { return a.refresh == b.refresh; }),
                  samples.end());
    fit.samples = static_cast<unsigned>(samples.size());
    if (fit.samples < kVblankSamplesMin) { return fit; }
    long double period = 0.0L, origin = 0.0L;
    if (!fit_vblank_line(samples, period, origin)) { return fit; }
    const long double band = static_cast<long double>(kVblankResidualMaxUs) * 1e-6L *
                             static_cast<long double>(qpc_frequency);
    std::vector<VblankSample> kept;
    kept.reserve(samples.size());
    unsigned whole_period_off = 0;
    bool other_off = false;
    for (const VblankSample& s : samples) {
        const long double off = vblank_off(s, period, origin);
        if (off <= band) { kept.push_back(s); continue; }
        const long double periods = std::floor(off / period + 0.5L);
        if (periods >= 1.0L && std::fabs(off - periods * period) <= band) { ++whole_period_off; }
        else { other_off = true; }
    }
    const unsigned allowed = fit.samples / 100 > 1 ? fit.samples / 100 : 1;
    if (whole_period_off && !other_off && whole_period_off <= allowed && kept.size() >= kVblankSamplesMin &&
        fit_vblank_line(kept, period, origin)) {
        fit.outliers = whole_period_off;
        samples.swap(kept);
    }
    fit.period_qpc = static_cast<double>(period);
    fit.origin_qpc = static_cast<double>(origin);
    fit.hz = static_cast<double>(static_cast<long double>(qpc_frequency) / period);
    long double worst = 0.0L;
    for (const VblankSample& s : samples) {
        const long double off = vblank_off(s, period, origin);
        if (off > worst) { worst = off; }
    }
    fit.residual_us = 1e6 * static_cast<double>(worst) / static_cast<double>(qpc_frequency);
    fit.ok = fit.hz >= kVblankHzMin && fit.hz <= kVblankHzMax && fit.residual_us <= kVblankResidualMaxUs;
    return fit;
}

// Milliseconds from the vblank before `at` to `at`. A negative value means there is no usable grid, so a caller
// can never read "no grid" as "exactly on the vblank".
inline double vblank_phase_ms(const VblankFit& fit, Qpc at, Qpc qpc_frequency) {
    if (!fit.ok || !(fit.period_qpc > 0.0) || qpc_frequency <= 0) { return -1.0; }
    const double refreshes = (static_cast<double>(at) - fit.origin_qpc) / fit.period_qpc;
    const double phase_ticks = (refreshes - std::floor(refreshes)) * fit.period_qpc;
    return 1000.0 * phase_ticks / static_cast<double>(qpc_frequency);
}

// What share of events would land inside the phase window if nothing waited for a vblank: the window over the
// refresh period. Every measured share is read against this, never against a remembered per cent.
inline double vblank_uniform_share(const VblankFit& fit, double window_ms) {
    if (!fit.ok || !(fit.hz > 0.0)) { return std::numeric_limits<double>::quiet_NaN(); }
    const double period_ms = 1000.0 / fit.hz;
    return period_ms > 0.0 ? window_ms / period_ms : std::numeric_limits<double>::quiet_NaN();
}

// ------------------------------------------------------------------------------------------------- JSON
// A minimal writer: only what this client emits. Keys and values are produced by the client, never by a
// remote party, but strings are still escaped so a path with a backslash cannot break the file.
inline std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[8]{};
                sprintf_s(hex, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += hex;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

// A double for JSON: fixed notation with three decimals is enough for milliseconds and keeps the raw frame
// records readable. Non-finite values become null rather than producing invalid JSON.
inline std::string number(double value, int decimals = 3) {
    if (!std::isfinite(value)) { return "null"; }
    char text[64]{};
    sprintf_s(text, "%.*f", decimals, value);
    return text;
}

// How much of one frame the GPU timeline accounts for, against the frame interval measured on QPC.
//
// Per frame the lists' durations plus the gaps between them (the first gap reaching back into the previous
// frame) span exactly one frame period on the GPU timeline, so the sum must equal the CPU-measured interval,
// which no GPU tick touches. It does not when GetTimestampFrequency and the counter the GPU actually writes
// disagree, and then the ratio is the scale factor: every millisecond in gpu_ms and latency_ms is off by it.
// Worth checking on every new driver stack - our own KMD answers CalibrateGpuClock with the CPU's performance
// counter (driver/kmd/wddm.c Bc250WddmCalibrateGpuClock), which is not what the command processor writes into
// a timestamp query. Returns 0 when there is nothing to compare.
inline double timestamp_scale_ratio(double gpu_busy_ms, double gpu_idle_ms, double interval_ms) noexcept {
    if (!(interval_ms > 0.0)) { return 0.0; }
    const double accounted = gpu_busy_ms + gpu_idle_ms;
    if (!(accounted > 0.0)) { return 0.0; }
    return accounted / interval_ms;
}

// A ratio far from one means the GPU-side millisecond values cannot be read as time. The band is wide on
// purpose: a frame interval and a GPU timeline measured over slightly different frame sets, plus one dropped
// calibration point, move the ratio by a per cent or two, and nothing smaller than that changes a verdict.
inline bool timestamp_scale_ok(double ratio) noexcept {
    return ratio == 0.0 || (ratio > 0.85 && ratio < 1.15);
}

inline void write_summary(std::FILE* file, const char* name, const Summary& s, const char* trailer) {
    std::fprintf(file, "      \"%s\": {\"count\": %zu, \"min\": %s, \"p50\": %s, \"p90\": %s, \"p99\": %s, \"max\": %s, \"mean\": %s}%s\n",
                 name, s.count, number(s.min).c_str(), number(s.p50).c_str(), number(s.p90).c_str(),
                 number(s.p99).c_str(), number(s.max).c_str(), number(s.mean).c_str(), trailer);
}

} // namespace measure
