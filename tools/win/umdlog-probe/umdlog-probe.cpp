// SPDX-License-Identifier: MIT
// UMD logging probe (BD-379): records the events of the WDDM 1.2 UMD logging provider (Microsoft-Windows-
// D3DUmdLogging) that the installed D3D11 driver writes for renamed buffers, in their order, and checks them against
// the model of the HLK test UMDLoggingRenamedSurface. No window. The program does what one case of that test does:
//   1. creates two DYNAMIC buffers A and B (512 KiB by default) and a DEFAULT buffer that the GPU copies into;
//   2. starts a real-time ETW session, enables the provider, and requests a capture state (the rundown);
//   3. N times (4 by default): copies A to the DEFAULT buffer, maps A with WRITE_DISCARD and unmaps it; the same
//      for B. The copy keeps the GPU on the current storage, so each discard renames the buffer;
//   4. submits, waits one second, stops the session and prints each event of this process.
// The model checks, per buffer: MAP of a new instance comes before UNMAP of the old one; every event has Renamed
// usage; each instance is a kernel allocation of its own (not Packed, offset 0, never one of the other buffer);
// each new instance is mapped while an earlier instance is still mapped; at the end 2 to N+2 instances are mapped.
// Exit 0: every check passes; 1: a check fails; 2: usage; 3: setup (no adapter, no device, no ETW session).
// The program needs the right to start a trace session (an administrator, or a member of Performance Log Users).
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;

namespace {
// Microsoft-Windows-D3DUmdLogging, as umdprovider.h of the WDK names it (UMDEtwProviderGuid)
constexpr GUID Provider = { 0xA688EE40, 0xD8D9, 0x4736, { 0xB6, 0xF9, 0x6B, 0x74, 0x93, 0x5B, 0xA3, 0xB1 } };
constexpr UCHAR OpMap = 1u, OpUnmap = 2u, OpRundown = 3u;
constexpr UINT32 UsagePacked = 1u, UsageRenamed = 2u;

struct Event {
    LONGLONG time = 0; UCHAR opcode = 0;
    UINT64 d3d = 0, dxg = 0, offset = 0, size = 0; UINT32 usage = 0, semantic = 0;
};
// A point of the program's own sequence, in the clock of the session (see now())
struct Marker { LONGLONG time = 0; std::string what; int buffer = -1; };

// The clock of the event time stamps: the session asks for QPC (Wnode.ClientContext 1), and the consumer reads the
// time stamps raw (PROCESS_TRACE_MODE_RAW_TIMESTAMP). Without that mode ETW gives system time (FILETIME) to the
// consumer, which does not compare with QPC markers. OpenTrace reports the clock type in
// LogfileHeader.ReservedFlags: 1 QPC, 2 system time, 3 CPU cycle counter.
constexpr ULONG ClockQpc = 1u, ClockSystemTime = 2u;
ULONG g_clock = ClockQpc;

DWORD g_pid = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Event> g_events;
ULONG g_foreign = 0, g_short = 0;

void WINAPI on_event(PEVENT_RECORD r) {
    if (!IsEqualGUID(r->EventHeader.ProviderId, Provider)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (r->EventHeader.ProcessId != g_pid) { g_foreign++; ReleaseSRWLockExclusive(&g_lock); return; }
    // The payload of LogMapAllocation: four 8-byte fields, then Usage and Semantic, 4 bytes each
    if (r->UserDataLength < 40u) { g_short++; ReleaseSRWLockExclusive(&g_lock); return; }
    Event e; e.time = r->EventHeader.TimeStamp.QuadPart; e.opcode = r->EventHeader.EventDescriptor.Opcode;
    const auto *p = static_cast<const unsigned char *>(r->UserData);
    std::memcpy(&e.d3d, p, 8); std::memcpy(&e.dxg, p + 8, 8); std::memcpy(&e.offset, p + 16, 8);
    std::memcpy(&e.size, p + 24, 8); std::memcpy(&e.usage, p + 32, 4); std::memcpy(&e.semantic, p + 36, 4);
    g_events.push_back(e);
    ReleaseSRWLockExclusive(&g_lock);
}

// The time now in the clock of the event time stamps
LONGLONG now() {
    if (g_clock == ClockSystemTime) {
        FILETIME f; GetSystemTimePreciseAsFileTime(&f);
        return LONGLONG((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime);
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart;
}

// Ticks of that clock per second
double ticks_per_second() {
    if (g_clock == ClockSystemTime) return 1.0e7;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart);
}

const char *op_name(UCHAR op) {
    return op == OpMap ? "MAP" : op == OpUnmap ? "UNMAP" : op == OpRundown ? "RUNDOWN" : "OTHER";
}

// One real-time session with the provider enabled; stopped by the destructor on every exit path
class Session {
public:
    explicit Session(const std::wstring &name) : name_(name) {
        buffer_.resize(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t));
    }
    ~Session() { stop(); }
    ULONG start() {
        // A session of the same name that a killed run left goes first
        control(EVENT_TRACE_CONTROL_STOP);
        auto *p = props();
        p->Wnode.ClientContext = 1u; // QPC time stamps, the clock of the markers
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p->FlushTimer = 1u;
        ULONG status = StartTraceW(&handle_, name_.c_str(), p);
        if (status != ERROR_SUCCESS) return status;
        started_ = true;
        EVENT_TRACE_LOGFILEW log{};
        log.LoggerName = const_cast<wchar_t *>(name_.c_str());
        log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD
                             | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        log.EventRecordCallback = on_event;
        trace_ = OpenTraceW(&log);
        if (trace_ == INVALID_PROCESSTRACE_HANDLE) return GetLastError();
        // The markers use the clock that the session has; a cycle counter has no user-mode equivalent here
        // 0: the header does not say; the session has the clock that it asked for
        clock_ = log.LogfileHeader.ReservedFlags ? log.LogfileHeader.ReservedFlags : ClockQpc;
        if (clock_ != ClockQpc && clock_ != ClockSystemTime) return ERROR_NOT_SUPPORTED;
        g_clock = clock_;
        consumer_ = std::thread([this] { ProcessTrace(&trace_, 1u, nullptr, nullptr); });
        return EnableTraceEx2(handle_, &Provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                              0u, 0u, 0u, nullptr);
    }
    ULONG capture_state() {
        return EnableTraceEx2(handle_, &Provider, EVENT_CONTROL_CODE_CAPTURE_STATE, TRACE_LEVEL_VERBOSE,
                              0u, 0u, 0u, nullptr);
    }
    // Lost events make the order incomplete; the result says so
    ULONG lost() const { return lost_; }
    ULONG clock() const { return clock_; }
    void stop() {
        if (!started_) return;
        started_ = false;
        EnableTraceEx2(handle_, &Provider, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0u, 0u, 0u, 0u, nullptr);
        auto *p = props();
        if (ControlTraceW(0u, name_.c_str(), p, EVENT_TRACE_CONTROL_STOP) == ERROR_SUCCESS) lost_ = p->EventsLost;
        if (consumer_.joinable()) consumer_.join();
        if (trace_ != INVALID_PROCESSTRACE_HANDLE) CloseTrace(trace_);
        trace_ = INVALID_PROCESSTRACE_HANDLE;
    }
private:
    EVENT_TRACE_PROPERTIES *props() {
        std::fill(buffer_.begin(), buffer_.end(), 0u);
        auto *p = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer_.data());
        p->Wnode.BufferSize = ULONG(buffer_.size());
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        return p;
    }
    ULONG control(ULONG code) { return ControlTraceW(0u, name_.c_str(), props(), code); }
    std::wstring name_;
    std::vector<unsigned char> buffer_;
    TRACEHANDLE handle_ = 0, trace_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread consumer_;
    bool started_ = false;
    ULONG lost_ = 0, clock_ = 0;
};

int usage() {
    std::fprintf(stderr, "usage: umdlog-probe [--rounds N] [--size BYTES] [--any-adapter]\n");
    return 2;
}
} // namespace

int main(int argc, char **argv) {
    unsigned rounds = 4u, size = 512u << 10; bool anyAdapter = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) size = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--any-adapter")) anyAdapter = true;
        else return usage();
    }
    if (rounds < 1u || rounds > 64u || size < 256u || size > (128u << 20)) return usage();
    g_pid = GetCurrentProcessId();

    ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 ad{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { std::printf("SETUP no DXGI factory\n"); return 3; }
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> a; if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (anyAdapter || (d.VendorId == 0x1002 && d.DeviceId == 0x13FE)) { adapter = a; ad = d; break; }
    }
    if (!adapter) { std::printf("SETUP no matching adapter\n"); return 3; }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0u, levels, 2u,
                                 D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        std::printf("SETUP D3D11CreateDevice failed\n"); return 3;
    }

    // A and B before the session, as the HLK test creates them before it starts its session
    D3D11_BUFFER_DESC dyn{}; dyn.ByteWidth = size; dyn.Usage = D3D11_USAGE_DYNAMIC;
    dyn.BindFlags = D3D11_BIND_VERTEX_BUFFER; dyn.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BUFFER_DESC def{}; def.ByteWidth = size; def.Usage = D3D11_USAGE_DEFAULT; def.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> buffers[2], target;
    if (FAILED(device->CreateBuffer(&dyn, nullptr, &buffers[0])) || FAILED(device->CreateBuffer(&dyn, nullptr, &buffers[1]))
     || FAILED(device->CreateBuffer(&def, nullptr, &target))) {
        std::printf("SETUP CreateBuffer failed\n"); return 3;
    }

    std::vector<Marker> markers;
    wchar_t name[64]; std::swprintf(name, 64, L"bc250-umdlog-probe-%lu", g_pid);
    Session session(name);
    if (ULONG status = session.start(); status != ERROR_SUCCESS) {
        std::printf("SETUP ETW session: error %lu (5 = access denied: run as an administrator)\n", status); return 3;
    }
    Sleep(500u);
    markers.push_back({ now(), "capture state", -1 });
    session.capture_state();
    Sleep(500u);

    for (unsigned r = 0; r < rounds; r++) {
        for (int b = 0; b < 2; b++) {
            ctx->CopyResource(target.Get(), buffers[b].Get());
            markers.push_back({ now(), std::string("discard ") + std::to_string(r + 1u) + (b ? " B" : " A"), b });
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(buffers[b].Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &m))) {
                std::memset(m.pData, int(r * 2u + unsigned(b) + 1u), 16u);
                ctx->Unmap(buffers[b].Get(), 0u);
            }
        }
    }
    markers.push_back({ now(), "flush", -1 });
    ctx->Flush();
    Sleep(1000u);
    markers.push_back({ now(), "stop", -1 });
    session.stop();

    AcquireSRWLockExclusive(&g_lock);
    std::vector<Event> events = g_events;
    ReleaseSRWLockExclusive(&g_lock);
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.time < b.time; });

    // The buffer of each event: the MAP and UNMAP events inside a discard window are of the buffer that it maps.
    // The engine writes them inside the Map call (E2), so the window holds them.
    std::map<UINT64, int> owner;
    for (const Event &e : events) {
        // hD3DAllocation 0 is an allocation of the driver itself, not of a resource
        if (e.opcode == OpRundown || !e.d3d) continue;
        for (size_t m = 0; m + 1 < markers.size(); m++) {
            if (markers[m].buffer >= 0 && e.time >= markers[m].time && e.time < markers[m + 1].time)
                owner.emplace(e.d3d, markers[m].buffer);
        }
    }

    std::wprintf(L"adapter %ls (%04X:%04X), %u rounds, %u-byte DYNAMIC buffers, event clock %ls\n", ad.Description,
                 ad.VendorId, ad.DeviceId, rounds, size, g_clock == ClockSystemTime ? L"system time" : L"QPC");
    size_t next = 0;
    LONGLONG origin = markers.front().time;
    const double ticks = ticks_per_second();
    auto ms = [&](LONGLONG t) { return double(t - origin) * 1000.0 / ticks; };
    for (const Event &e : events) {
        while (next < markers.size() && markers[next].time <= e.time) {
            std::printf("%9.3f ms  -- %s\n", ms(markers[next].time), markers[next].what.c_str()); next++;
        }
        auto o = owner.find(e.d3d);
        std::printf("%9.3f ms  %-7s %s hD3D %016llX hDxg %016llX offset %llu size %llu usage %u%s%s semantic %u\n",
                    ms(e.time), op_name(e.opcode), o == owner.end() ? "?" : o->second ? "B" : "A",
                    (unsigned long long)e.d3d, (unsigned long long)e.dxg, (unsigned long long)e.offset,
                    (unsigned long long)e.size, e.usage, (e.usage & UsagePacked) ? " Packed" : "",
                    (e.usage & UsageRenamed) ? " Renamed" : "", e.semantic);
    }
    for (; next < markers.size(); next++) std::printf("%9.3f ms  -- %s\n", ms(markers[next].time), markers[next].what.c_str());

    // The model of the test, per buffer
    bool pass = true;
    auto check = [&](bool ok, const char *buffer, const std::string &what) {
        std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", buffer, what.c_str()); pass = pass && ok;
    };
    // Self-check of the time base: each event comes between the first and the last marker, and at least one MAP or
    // UNMAP in a discard window. Else the markers and the events do not have one clock, and the checks below cannot
    // name the buffers. A driver can write some events later (at a flush), so not every event must be in a window.
    size_t outside = 0, inWindow = 0, mapsAndUnmaps = 0;
    for (const Event &e : events) {
        if (e.time < markers.front().time || e.time > markers.back().time) outside++;
        if (e.opcode == OpRundown) continue;
        mapsAndUnmaps++;
        for (size_t m = 0; m + 1 < markers.size(); m++) {
            if (markers[m].buffer >= 0 && e.time >= markers[m].time && e.time < markers[m + 1].time) { inWindow++; break; }
        }
    }
    check(!events.empty() && outside == 0u && inWindow > 0u, "clock",
          std::to_string(events.size()) + " events, " + std::to_string(outside) + " outside the run, "
          + std::to_string(inWindow) + " of " + std::to_string(mapsAndUnmaps) + " MAP and UNMAP events in a discard window");
    std::map<UINT64, int> dxgOwner; bool shared = false;
    for (int b = 0; b < 2; b++) {
        const char *label = b ? "B" : "A";
        UINT64 d3d = 0;
        for (const auto &[key, value] : owner) if (value == b) d3d = key;
        if (!d3d) { check(false, label, "no MAP or UNMAP event of a discard"); continue; }
        std::vector<UINT64> mapped; bool order = true, renamed = true, own = true, linked = true;
        for (size_t i = 0; i < events.size(); i++) {
            const Event &e = events[i];
            if (e.d3d != d3d) continue;
            renamed = renamed && (e.usage & UsageRenamed);
            if (e.opcode == OpUnmap) {
                // An UNMAP followed in the same discard window by a MAP of this buffer breaks the link
                for (size_t j = i + 1; j < events.size(); j++) {
                    if (events[j].d3d != d3d) continue;
                    if (events[j].opcode == OpMap) {
                        bool sameWindow = true;
                        for (const Marker &m : markers) if (m.time > e.time && m.time <= events[j].time) sameWindow = false;
                        if (sameWindow) order = false;
                    }
                    break;
                }
                mapped.erase(std::remove(mapped.begin(), mapped.end(), e.dxg), mapped.end());
            } else {
                if (e.opcode == OpMap && mapped.empty()) linked = false;
                own = own && !(e.usage & UsagePacked) && e.offset == 0u;
                auto [at, added] = dxgOwner.emplace(e.dxg, b);
                if (!added && at->second != b) shared = true;
                if (std::find(mapped.begin(), mapped.end(), e.dxg) == mapped.end()) mapped.push_back(e.dxg);
                else own = false; // two mappings of one allocation by one buffer
            }
        }
        check(order, label, "MAP of a new instance comes before UNMAP of the old one");
        check(renamed, label, "every event has Renamed usage");
        check(own, label, "each instance is an allocation of its own (not Packed, offset 0)");
        check(linked, label, "each MAP comes while an earlier instance is mapped");
        check(mapped.size() >= 2u && mapped.size() <= rounds + 2u, label,
              "mapped instances at the end: " + std::to_string(mapped.size()) + " (2 to " + std::to_string(rounds + 2u) + ")");
    }
    check(!shared, "A+B", "no allocation is an instance of both buffers");
    check(session.lost() == 0u && g_short == 0u, "session",
          "events lost " + std::to_string(session.lost()) + ", short payloads " + std::to_string(g_short)
          + " (events of other processes, not counted: " + std::to_string(g_foreign) + ")");
    std::printf("%s\n", pass ? "RESULT PASS" : "RESULT FAIL");
    return pass ? 0 : 1;
}
