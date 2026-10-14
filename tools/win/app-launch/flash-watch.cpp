// SPDX-License-Identifier: MIT
// flash-watch.exe - what DWM composes over the start window, frame by frame (owner, 2026-10-14: the small blue
// window after logon flashes for one or two frames every 6-8 s). It starts the window itself, with the arguments of
// the logon task "amdgpu-wddm start window" (common.ps1 Get-LogonTaskPlan), finds that process's window, and reads
// the window's rectangle from every frame that Desktop Duplication delivers. The lab's overlay (bc250mon), when it
// runs, is a second target: a window of ours that also changes its text every second, drawn double-buffered and
// layered. When duplication cannot start, the rectangles are read from the screen through GDI every 2 ms instead,
// which DWM answers with its last composed frame.
//
//   flash-watch.exe <out-prefix> <deadline-ms> <powershell.exe> <window script> <seconds> [surface]
//   flash-watch.exe --self-test
//
// With "surface", a third target reads the start window's own surface through its window DC about every millisecond
// (SurfaceLoop), so that a flash in DWM's frames can be told from a state that the window itself passed through.
//
// measure-flash.ps1 runs it in the console session through a scheduled task. All times are QueryPerformanceCounter
// ticks, the clock that poll-repro.ps1 ([Diagnostics.Stopwatch]::GetTimestamp) and DXGI's LastPresentTime use too.
//
// Output, next to <out-prefix>:
//   -frames.csv  one row per frame that DWM presented and per target it touched:
//                frame,target,qpc,present_qpc,accumulated,mode,dirty,dx0,dy0,dx1,dy1,navy,cyan,black,other,hash.
//                dirty is 1 when a dirty rectangle of the frame met the target (dx0..dy1, target coordinates), 2 when
//                the frame carried no dirty rectangles at all (then every frame is read), 0 for a frame elsewhere on
//                the screen (target "-", its times only). navy/cyan/black/other count the target's pixels by colour.
//                In the GDI fallback a row is a change of a rectangle (mode gdi, dirty 2).
//   -log.txt     key=value lines: start, adapter, output, mode, child, target, flash, end.
//   -ref.bmp     the start window's rectangle of its first complete frame, and -<target>-NN.bmp of each flash (40 at
//                most per target). Nothing outside a target's rectangle is ever written.
// A flash is a content X of a target's rectangle that DWM showed for less than 60 ms between two other contents.
// The countdown changes the text once a second, so every regular content stays about 1000 ms.
//
// Exit codes: 0 the run ended, 1 no window appeared or no frame could be read, 2 bad arguments, 4 the watchdog ended
// the process. The child is started with no startup information, as the task starts it, and is ended only when it is
// still running at the deadline: this tool stops no process that it did not start.
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace {

const int64_t kFlashMs = 60;        // a content shown for less than this, between two others, is a flash
const int kMaxBmps = 40;

// ------------------------------------------------------------------------------------------------ pure part
struct Counts { uint32_t navy = 0, cyan = 0, black = 0, other = 0; };

// Navy (0,0,128) and cyan (0,255,255) are the start window's two colours; antialiased text gives colours between
// them. The bounds are wide on purpose: a colour-managed output may move a channel by a few dozen steps.
Counts Classify(const uint8_t *bgra, int width, int height, int pitch, uint64_t *hash)
{
    Counts c;
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < height; ++y) {
        const uint8_t *p = bgra + (size_t)y * pitch;
        for (int x = 0; x < width; ++x, p += 4) {
            const int b = p[0], g = p[1], r = p[2];
            if (r < 40 && g < 40 && b >= 96 && b <= 200) ++c.navy;
            else if (r < 80 && g > 180 && b > 180) ++c.cyan;
            else if (r < 24 && g < 24 && b < 24) ++c.black;
            else ++c.other;
            for (int k = 0; k < 3; ++k) { h ^= p[k]; h *= 1099511628211ull; }
        }
    }
    *hash = h;
    return c;
}

// What a short-lived content of the start window looked like, in one word.
const char *Kind(const Counts &c, bool earlier)
{
    const uint32_t all = c.navy + c.cyan + c.black + c.other;
    if (!all) return "empty";
    if (c.black * 2 > all) return "black";
    if (c.navy * 2 < all) return "not-the-window";
    if (c.cyan == 0 && c.other * 100 < all) return "navy-only";
    if (earlier) return "earlier-content";
    return "other-content";
}

struct Content {
    uint64_t hash = 0;
    int64_t qpc = 0;
    Counts c;
    // The composed frame that carried this content: its number in the run, when it was read and when DWM presented
    // it (Desktop Duplication's LastPresentTime, 0 for a GDI read), how many presents it held, and the reading mode.
    uint64_t frame = 0;
    int64_t captureQpc = 0, presentQpc = 0;
    UINT accumulated = 0;
    const char *mode = "none";
    std::vector<uint8_t> px;        // the rectangle's pixels, kept while this is the current content
    int pitch = 0;
};

// Feed a target's contents in the order DWM showed them. A content that stayed less than the flash time between
// two others is a flash, reported once, when the content after it arrives.
class Detector {
public:
    explicit Detector(int64_t flashTicks) : flash_(flashTicks) {}

    // True when the content before this one was a flash: *out is that content (pixels included), *ticks how long it
    // stayed, *earlier whether the same pixels were on the screen before the content that preceded it.
    bool Feed(Content &&now, Content *out, int64_t *ticks, bool *earlier)
    {
        if (have_ && now.hash == cur_.hash) return false;
        bool flashed = false;
        if (have_ && havePrev_ && now.qpc - cur_.qpc < flash_) {
            *ticks = now.qpc - cur_.qpc;
            *earlier = Seen(cur_.hash, prevQpc_);
            flashed = true;
        }
        if (have_) {
            Remember(cur_.hash, cur_.qpc);
            prevQpc_ = cur_.qpc;
            havePrev_ = true;
            if (flashed) *out = std::move(cur_);
        }
        cur_ = std::move(now);
        have_ = true;
        ++changes_;
        return flashed;
    }
    uint64_t Changes() const { return changes_; }

private:
    bool Seen(uint64_t hash, int64_t before) const
    {
        for (const auto &h : history_)
            if (h.first == hash && h.second < before) return true;
        return false;
    }
    void Remember(uint64_t hash, int64_t qpc)
    {
        history_.emplace_back(hash, qpc);
        if (history_.size() > 512) history_.erase(history_.begin());
    }
    int64_t flash_;
    bool have_ = false, havePrev_ = false;
    Content cur_;
    int64_t prevQpc_ = 0;
    std::vector<std::pair<uint64_t, int64_t>> history_;
    uint64_t changes_ = 0;
};

int SelfTest()
{
    int fail = 0;
    auto check = [&](bool ok, const char *what) {
        std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++fail;
    };
    const int64_t ms = 1000;    // ticks per millisecond in this test
    auto at = [](uint64_t hash, int64_t qpc) {
        Content c;
        c.hash = hash;
        c.qpc = qpc;
        return c;
    };
    Content out;
    int64_t t = 0;
    bool earlier = false;
    {
        Detector d(kFlashMs * ms);
        bool any = false;
        for (int i = 0; i < 5; ++i) any |= d.Feed(at(100 + i, i * 1000 * ms), &out, &t, &earlier);
        check(!any && d.Changes() == 5, "a countdown that changes once a second has no flash");
    }
    {
        Detector d(kFlashMs * ms);
        d.Feed(at(1, 0), &out, &t, &earlier);
        d.Feed(at(2, 1000 * ms), &out, &t, &earlier);
        const bool f = d.Feed(at(3, 1013 * ms), &out, &t, &earlier);
        check(f && out.hash == 2 && t == 13 * ms && !earlier, "a content shown for one frame between two others is a flash");
    }
    {
        Detector d(kFlashMs * ms);
        d.Feed(at(1, 0), &out, &t, &earlier);
        d.Feed(at(2, 1000 * ms), &out, &t, &earlier);
        check(!d.Feed(at(2, 1005 * ms), &out, &t, &earlier) && d.Changes() == 2,
              "a frame with the same pixels again is no change");
    }
    {
        Detector d(kFlashMs * ms);
        d.Feed(at(1, 0), &out, &t, &earlier);
        d.Feed(at(2, 1000 * ms), &out, &t, &earlier);
        d.Feed(at(1, 2000 * ms), &out, &t, &earlier);     // the content of two seconds ago, for two frames
        const bool f = d.Feed(at(3, 2027 * ms), &out, &t, &earlier);
        check(f && earlier && t == 27 * ms, "an old content shown again for two frames is a flash of earlier content");
    }
    {
        Detector d(kFlashMs * ms);
        d.Feed(at(1, 0), &out, &t, &earlier);
        const bool f = d.Feed(at(2, 10 * ms), &out, &t, &earlier);
        check(!f, "the first content of the window is no flash, however short");
    }
    {
        std::vector<uint8_t> px(4 * 10 * 2, 0);
        for (int i = 0; i < 20; ++i) px[i * 4] = 128;                         // navy
        uint64_t h1 = 0, h2 = 0, h3 = 0;
        const Counts c1 = Classify(px.data(), 10, 2, 40, &h1);
        check(c1.navy == 20 && std::strcmp(Kind(c1, false), "navy-only") == 0, "a rectangle of navy alone is navy-only");
        for (int i = 0; i < 4; ++i) { px[i * 4] = 255; px[i * 4 + 1] = 255; }  // cyan text
        const Counts c2 = Classify(px.data(), 10, 2, 40, &h2);
        check(c2.cyan == 4 && h1 != h2 && std::strcmp(Kind(c2, false), "other-content") == 0,
              "cyan text changes the class and the hash");
        const std::vector<uint8_t> dark(4 * 10 * 2, 0);
        const Counts c3 = Classify(dark.data(), 10, 2, 40, &h3);
        check(std::strcmp(Kind(c3, false), "black") == 0, "a black rectangle is black");
    }
    std::printf("flash-watch self-test: %s\n", fail ? "FAILED" : "all checks passed");
    return fail ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------ the run
FILE *g_log = nullptr, *g_csv = nullptr;
int64_t g_freq = 1;
int64_t g_deadline = 0;
std::wstring g_prefix;
std::string g_mode = "none";
uint64_t g_frames = 0;
CRITICAL_SECTION g_logLock;             // the log is written by the frame loop and by the surface reader
volatile LONG g_stop = 0;               // set when the frame loop ends; the surface reader stops then

int64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void Log(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    EnterCriticalSection(&g_logLock);
    std::vfprintf(g_log, fmt, a);
    std::fputc('\n', g_log);
    std::fflush(g_log);
    LeaveCriticalSection(&g_logLock);
    va_end(a);
}

std::string Narrow(const wchar_t *w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// A rectangle only, as a top-down 32-bit BMP.
bool WriteBmp(const std::wstring &path, const uint8_t *bgra, int width, int height, int pitch)
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = -height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)(width * 4 * height);
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    for (int y = 0; y < height; ++y) std::fwrite(bgra + (size_t)y * pitch, 4, (size_t)width, f);
    std::fclose(f);
    return true;
}

// The largest visible WinForms top-level window of a process.
struct Found { DWORD pid; HWND hwnd; RECT rect; LONG area; };

BOOL CALLBACK FindWindowOf(HWND h, LPARAM l)
{
    Found *f = reinterpret_cast<Found *>(l);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcsncmp(cls, L"WindowsForms10.Window", 21) != 0) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 64 || r.bottom - r.top < 16) return TRUE;
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > f->area) {
        f->hwnd = h;
        f->rect = r;
        f->area = area;
    }
    return TRUE;
}

DWORD ProcessByName(const wchar_t *exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {};
    e.dwSize = sizeof(e);
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (!_wcsicmp(e.szExeFile, exe)) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

bool Intersect(const RECT &a, const RECT &b, RECT *out)
{
    RECT r = {max(a.left, b.left), max(a.top, b.top), min(a.right, b.right), min(a.bottom, b.bottom)};
    if (r.left >= r.right || r.top >= r.bottom) return false;
    *out = r;
    return true;
}

// One watched window: the start window (the process this tool started) or the overlay (bc250mon.exe) in DWM's
// frames, or the start window's own surface (mode "surface", read by SurfaceLoop).
struct Target {
    const char *name;
    const wchar_t *exe;             // found by image name when not null
    const char *mode = nullptr;     // the reading mode of this target's rows, when it is not the frame loop's
    DWORD pid = 0;
    HWND volatile hwnd = nullptr;   // read by the surface reader too
    bool have = false;
    RECT rect = {};                 // desktop coordinates, physical pixels
    int64_t firstQpc = 0, lastLocate = 0;
    Detector detector;
    uint64_t analysed = 0, flashes = 0, accumulatedMisses = 0;
    uint32_t mostNavy = 0;
    int bmps = 0;
    bool haveRef = false;
    ComPtr<ID3D11Texture2D> staging;
    int stagingW = 0, stagingH = 0;

    Target(const char *n, const wchar_t *e) : name(n), exe(e), detector(kFlashMs * g_freq / 1000) {}
    int Width() const { return rect.right - rect.left; }
    int Height() const { return rect.bottom - rect.top; }

    void Locate()
    {
        const int64_t now = Qpc();
        if (now - lastLocate < g_freq / 4) return;
        lastLocate = now;
        if (exe && !pid) pid = ProcessByName(exe);
        if (!pid) return;
        Found f = {pid, nullptr, {}, 0};
        EnumWindows(FindWindowOf, reinterpret_cast<LPARAM>(&f));
        if (!f.hwnd) return;
        if (!have || std::memcmp(&f.rect, &rect, sizeof(RECT)) != 0) {
            Log("target %s pid=%lu hwnd=0x%p rect=%ld,%ld,%ld,%ld qpc=%lld", name, pid, (void *)f.hwnd, f.rect.left,
                f.rect.top, f.rect.right - f.rect.left, f.rect.bottom - f.rect.top, (long long)now);
            if (!have) firstQpc = now;
        }
        have = true;
        rect = f.rect;
        hwnd = f.hwnd;
    }

    // One frame's rectangle: a CSV row, and the detector.
    void Analyse(const uint8_t *bgra, int pitch, int64_t qpc, int64_t presentQpc, UINT accumulated, int dirty,
                 const RECT &d)
    {
        const int w = Width(), h = Height();
        const bool window = !exe;
        Content now;
        now.c = Classify(bgra, w, h, pitch, &now.hash);
        now.qpc = presentQpc ? presentQpc : qpc;
        now.frame = g_frames;
        now.captureQpc = qpc;
        now.presentQpc = presentQpc;
        now.accumulated = accumulated;
        now.mode = mode ? mode : g_mode == "dda" ? "dda" : g_mode == "gdi" ? "gdi" : "none";
        ++analysed;
        mostNavy = max(mostNavy, now.c.navy);
        std::fprintf(g_csv, "%llu,%s,%lld,%lld,%u,%s,%d,%ld,%ld,%ld,%ld,%u,%u,%u,%u,%016llx\n",
                     (unsigned long long)g_frames, name, (long long)qpc, (long long)presentQpc, accumulated,
                     now.mode, dirty, d.left, d.top, d.right, d.bottom, now.c.navy, now.c.cyan, now.c.black,
                     now.c.other, (unsigned long long)now.hash);
        if (window && !mode && !haveRef && now.c.cyan > 0 && now.c.navy * 2 > (uint32_t)(w * h))
            haveRef = WriteBmp(g_prefix + L"-ref.bmp", bgra, w, h, pitch);
        const uint64_t hash = now.hash;
        now.px.assign(bgra, bgra + (size_t)pitch * (h - 1) + (size_t)w * 4);
        now.pitch = pitch;
        Content out;
        int64_t ticks = 0;
        bool earlier = false;
        if (!detector.Feed(std::move(now), &out, &ticks, &earlier)) return;
        ++flashes;
        std::string bmp = "-";
        if (bmps < kMaxBmps && !out.px.empty()) {
            const std::wstring file = g_prefix + L"-" + std::wstring(name, name + std::strlen(name)) + L"-" +
                                      std::to_wstring(bmps) + L".bmp";
            if (WriteBmp(file, out.px.data(), w, h, out.pitch)) {
                bmp = Narrow(file.c_str());
                const size_t slash = bmp.find_last_of("\\/");
                if (slash != std::string::npos) bmp = bmp.substr(slash + 1);
                ++bmps;
            }
        }
        const char *kind = window ? Kind(out.c, earlier) : (earlier ? "earlier-content" : "other-content");
        Log("flash target=%s qpc=%lld ms=%.1f kind=%s navy=%u cyan=%u black=%u other=%u hash=%016llx next=%016llx "
            "since_target_ms=%lld bmp=%s frame=%llu present=%lld capture=%lld accumulated=%u mode=%s",
            name, (long long)out.qpc, ticks * 1000.0 / g_freq, kind, out.c.navy, out.c.cyan, out.c.black, out.c.other,
            (unsigned long long)out.hash, (unsigned long long)hash, (long long)((out.qpc - firstQpc) * 1000 / g_freq),
            bmp.c_str(), (unsigned long long)out.frame, (long long)out.presentQpc, (long long)out.captureQpc,
            out.accumulated, out.mode);
    }
};

struct Run {
    PROCESS_INFORMATION child = {};
    int64_t childExitQpc = 0;
    std::vector<Target> targets;

    bool ChildRunning()
    {
        if (childExitQpc) return false;
        if (WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) return true;
        childExitQpc = Qpc();
        Log("child exited qpc=%lld", (long long)childExitQpc);
        return false;
    }

    void Locate()
    {
        for (auto &t : targets) t.Locate();
    }

    bool Done()
    {
        const int64_t now = Qpc();
        if (now >= g_deadline) return true;
        // The window process ends after its -Seconds; half a second more catches its last frames.
        return !ChildRunning() && now - childExitQpc > g_freq / 2;
    }
};

// ------------------------------------------------------------------------------------------------ Desktop Duplication
struct Dda {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutputDuplication> dup;
    DXGI_OUTPUT_DESC od = {};
    std::vector<uint8_t> meta;
    std::vector<RECT> changed;

    // The adapter and output that show the desktop's top-left corner, where the start window sits.
    bool Open()
    {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) { Log("dda factory hr=0x%08lx", (unsigned long)hr); return false; }
        ComPtr<IDXGIAdapter1> chosen;
        for (UINT a = 0;; ++a) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 ad = {};
            adapter->GetDesc1(&ad);
            Log("adapter index=%u vendor=0x%04x device=0x%04x name=%s", a, ad.VendorId, ad.DeviceId,
                Narrow(ad.Description).c_str());
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIOutput> o;
                if (adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC d = {};
                o->GetDesc(&d);
                const RECT r = d.DesktopCoordinates;
                Log("output adapter=%u index=%u attached=%u desktop=%ld,%ld,%ld,%ld", a, i, d.AttachedToDesktop ? 1u : 0u,
                    r.left, r.top, r.right, r.bottom);
                if (!output && d.AttachedToDesktop && r.left <= 0 && r.top <= 0 && r.right > 0 && r.bottom > 0) {
                    output = o;
                    od = d;
                    chosen = adapter;
                }
            }
        }
        if (!output) { Log("dda no output holds the desktop's top-left corner"); return false; }
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr)) { Log("dda device hr=0x%08lx", (unsigned long)hr); return false; }
        return Duplicate();
    }

    bool Duplicate()
    {
        dup.Reset();
        ComPtr<IDXGIOutput1> o1;
        HRESULT hr = output.As(&o1);
        if (SUCCEEDED(hr)) hr = o1->DuplicateOutput(dev.Get(), &dup);
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            ComPtr<IDXGIOutput5> o5;
            if (SUCCEEDED(output.As(&o5))) {
                const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
                hr = o5->DuplicateOutput1(dev.Get(), 0, 1, formats, &dup);
                Log("dda DuplicateOutput unsupported, DuplicateOutput1(B8G8R8A8) hr=0x%08lx", (unsigned long)hr);
            }
        }
        if (FAILED(hr)) { Log("dda duplicate hr=0x%08lx", (unsigned long)hr); return false; }
        DXGI_OUTDUPL_DESC dd = {};
        dup->GetDesc(&dd);
        Log("dda mode=%ux%u format=%d rotation=%d system_memory=%u", dd.ModeDesc.Width, dd.ModeDesc.Height,
            (int)dd.ModeDesc.Format, (int)dd.Rotation, dd.DesktopImageInSystemMemory ? 1u : 0u);
        return true;
    }

    // The frame's changed rectangles (move destinations and dirty rectangles), in output coordinates. False when the
    // frame carried none at all.
    bool Changed(const DXGI_OUTDUPL_FRAME_INFO &fi)
    {
        changed.clear();
        if (!fi.TotalMetadataBufferSize) return false;
        if (meta.size() < fi.TotalMetadataBufferSize) meta.resize(fi.TotalMetadataBufferSize);
        UINT used = 0;
        if (SUCCEEDED(dup->GetFrameMoveRects((UINT)meta.size(), reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT *>(meta.data()), &used)))
            for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i)
                changed.push_back(reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT *>(meta.data())[i].DestinationRect);
        used = 0;
        if (SUCCEEDED(dup->GetFrameDirtyRects((UINT)meta.size(), reinterpret_cast<RECT *>(meta.data()), &used)))
            for (UINT i = 0; i < used / sizeof(RECT); ++i) changed.push_back(reinterpret_cast<RECT *>(meta.data())[i]);
        return true;
    }

    RECT Local(const RECT &r) const
    {
        return {r.left - od.DesktopCoordinates.left, r.top - od.DesktopCoordinates.top,
                r.right - od.DesktopCoordinates.left, r.bottom - od.DesktopCoordinates.top};
    }

    // 1 a changed rectangle met the target (*hit their union, target coordinates), 0 none did.
    int Dirty(const Target &t, RECT *hit) const
    {
        const RECT local = Local(t.rect);
        RECT u = {LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        bool any = false;
        for (const RECT &r : changed) {
            RECT x;
            if (!Intersect(r, local, &x)) continue;
            any = true;
            u.left = min(u.left, x.left - local.left);
            u.top = min(u.top, x.top - local.top);
            u.right = max(u.right, x.right - local.left);
            u.bottom = max(u.bottom, x.bottom - local.top);
        }
        if (any) *hit = u;
        return any ? 1 : 0;
    }

    bool EnsureStaging(Target &t)
    {
        const int w = t.Width(), h = t.Height();
        if (t.staging && w == t.stagingW && h == t.stagingH) return true;
        t.staging.Reset();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)w;
        d.Height = (UINT)h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &t.staging);
        if (FAILED(hr)) { Log("dda staging target=%s hr=0x%08lx", t.name, (unsigned long)hr); return false; }
        t.stagingW = w;
        t.stagingH = h;
        return true;
    }

    // Frames until the run is done. False when no frame could be read at all.
    bool Loop(Run &run)
    {
        unsigned lost = 0, timeouts = 0;
        uint64_t got = 0;
        while (!run.Done()) {
            run.Locate();
            DXGI_OUTDUPL_FRAME_INFO fi = {};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup->AcquireNextFrame(20, &fi, &res);
            const int64_t qpc = Qpc();
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) { ++timeouts; continue; }
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                Log("dda access-lost n=%u qpc=%lld", ++lost, (long long)qpc);
                if (lost > 20 || !Duplicate()) return got > 0;
                continue;
            }
            if (FAILED(hr)) { Log("dda acquire hr=0x%08lx", (unsigned long)hr); return got > 0; }
            ++got;
            ++g_frames;
            bool touched = false;
            if (fi.LastPresentTime.QuadPart) {
                const bool listed = Changed(fi);
                ComPtr<ID3D11Texture2D> tex;
                res.As(&tex);
                for (auto &t : run.targets) {
                    if (!t.have) continue;
                    RECT hit = {0, 0, t.Width(), t.Height()};
                    const int dirty = listed ? Dirty(t, &hit) : 2;
                    if (!dirty) continue;
                    touched = true;
                    if (fi.AccumulatedFrames > 1) ++t.accumulatedMisses;
                    if (!tex || !EnsureStaging(t)) continue;
                    // Copied and read before ReleaseFrame: after it, DWM may write the next frame into this texture.
                    const RECT l = Local(t.rect);
                    D3D11_BOX box = {(UINT)max(0L, l.left), (UINT)max(0L, l.top), 0, (UINT)l.right, (UINT)l.bottom, 1};
                    ctx->CopySubresourceRegion(t.staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
                    D3D11_MAPPED_SUBRESOURCE m = {};
                    hr = ctx->Map(t.staging.Get(), 0, D3D11_MAP_READ, 0, &m);
                    if (FAILED(hr)) { Log("dda map target=%s hr=0x%08lx", t.name, (unsigned long)hr); continue; }
                    t.Analyse(static_cast<const uint8_t *>(m.pData), (int)m.RowPitch, qpc, fi.LastPresentTime.QuadPart,
                              fi.AccumulatedFrames, dirty, hit);
                    ctx->Unmap(t.staging.Get(), 0);
                }
                if (!touched) {
                    // A frame elsewhere on the screen: its times only, the timeline of DWM's presents.
                    std::fprintf(g_csv, "%llu,-,%lld,%lld,%u,%s,0,,,,,,,,,\n", (unsigned long long)g_frames,
                                 (long long)qpc, (long long)fi.LastPresentTime.QuadPart, fi.AccumulatedFrames,
                                 g_mode.c_str());
                }
            }
            res.Reset();
            dup->ReleaseFrame();
            // Frames that never show the window's navy, 3 s after the window appeared, are not the screen: the GDI
            // reads take over (a duplication that delivers a texture it cannot copy reads as zeros).
            const Target &w = run.targets[0];
            if (w.have && w.analysed && !w.mostNavy && qpc - w.firstQpc > 3 * g_freq) {
                Log("dda frames hold no navy over the window after 3 s: not the screen");
                return false;
            }
        }
        Log("dda frames=%llu timeouts=%u access_lost=%u", (unsigned long long)got, timeouts, lost);
        return got > 0;
    }
};

// ------------------------------------------------------------------------------------------------ GDI fallback
// The screen DC under DWM answers with the last composed frame. Every 2 ms, one row per change of a rectangle.
bool GdiLoop(Run &run)
{
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    uint64_t samples = 0;
    double slowest = 0;
    struct Dib { HBITMAP bmp = nullptr; void *bits = nullptr; int w = 0, h = 0; uint64_t last = 0; };
    std::vector<Dib> dibs(run.targets.size());
    while (!run.Done()) {
        run.Locate();
        for (size_t i = 0; i < run.targets.size(); ++i) {
            Target &t = run.targets[i];
            Dib &d = dibs[i];
            if (!t.have) continue;
            const int w = t.Width(), h = t.Height();
            if (!d.bmp || w != d.w || h != d.h) {
                if (d.bmp) DeleteObject(d.bmp);
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                d.bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &d.bits, nullptr, 0);
                if (!d.bmp) { Log("gdi dib failed target=%s", t.name); continue; }
                d.w = w;
                d.h = h;
            }
            SelectObject(mem, d.bmp);
            const int64_t t0 = Qpc();
            BitBlt(mem, 0, 0, w, h, screen, t.rect.left, t.rect.top, SRCCOPY);
            GdiFlush();
            const int64_t t1 = Qpc();
            slowest = max(slowest, (t1 - t0) * 1000.0 / g_freq);
            ++samples;
            ++g_frames;
            uint64_t hash = 0;
            Classify(static_cast<const uint8_t *>(d.bits), w, h, w * 4, &hash);
            if (hash != d.last) {
                const RECT whole = {0, 0, w, h};
                t.Analyse(static_cast<const uint8_t *>(d.bits), w * 4, (t0 + t1) / 2, 0, 0, 2, whole);
                d.last = hash;
            }
        }
        Sleep(1);
    }
    Log("gdi samples=%llu slowest_ms=%.1f", (unsigned long long)samples, slowest);
    for (auto &d : dibs)
        if (d.bmp) DeleteObject(d.bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return samples > 0;
}

// ------------------------------------------------------------------------------------------------ the window's surface
// With "surface": the start window's own pixels, read through its window DC about every millisecond, next to the
// frames that DWM composes. A read of a redirected window's DC is answered from the window's redirection surface,
// between two GDI calls on it. So a flash here is a state of the window's own surface; a flash in DWM's frames that
// never shows here is a state of DWM's copy of it.
struct Surface {
    Target *window = nullptr;       // the frame loop's start window: its HWND
    Target target{"surface", nullptr};
    uint64_t samples = 0;
    double slowest = 0;
};

DWORD WINAPI SurfaceLoop(void *p)
{
    Surface &s = *static_cast<Surface *>(p);
    s.target.mode = "surface";
    // Every read, with its own QPC before and after the BitBlt: the reads line up with DWM's presents, and the gaps
    // between reads say how much of the time around a flash this target covered.
    FILE *reads = nullptr;
    if (_wfopen_s(&reads, (g_prefix + L"-surface.csv").c_str(), L"w") || !reads) { Log("surface csv failed"); return 0; }
    setvbuf(reads, nullptr, _IOFBF, 1 << 20);
    std::fprintf(reads, "read,qpc0,qpc1,hash\n");
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP bmp = nullptr;
    void *bits = nullptr;
    int bw = 0, bh = 0;
    uint64_t last = 0;
    while (!g_stop && Qpc() < g_deadline) {
        const HWND h = s.window->hwnd;
        RECT rc = {};
        if (!h || !GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0) { Sleep(1); continue; }
        if (!bmp || rc.right != bw || rc.bottom != bh) {
            if (bmp) DeleteObject(bmp);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = rc.right;
            bi.bmiHeader.biHeight = -rc.bottom;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp) { Log("surface dib failed"); break; }
            SelectObject(mem, bmp);
            bw = rc.right;
            bh = rc.bottom;
            s.target.rect = {0, 0, bw, bh};
            s.target.have = true;
            if (!s.target.firstQpc) s.target.firstQpc = Qpc();
            Log("target surface hwnd=0x%p rect=0,0,%d,%d qpc=%lld", (void *)h, bw, bh, (long long)s.target.firstQpc);
        }
        HDC dc = GetDC(h);
        if (!dc) { Sleep(1); continue; }
        const int64_t t0 = Qpc();
        BitBlt(mem, 0, 0, bw, bh, dc, 0, 0, SRCCOPY);
        GdiFlush();
        const int64_t t1 = Qpc();
        ReleaseDC(h, dc);
        s.slowest = max(s.slowest, (t1 - t0) * 1000.0 / g_freq);
        ++s.samples;
        uint64_t hash = 0;
        Classify(static_cast<const uint8_t *>(bits), bw, bh, bw * 4, &hash);
        std::fprintf(reads, "%llu,%lld,%lld,%016llx\n", (unsigned long long)s.samples, (long long)t0, (long long)t1,
                     (unsigned long long)hash);
        if (hash != last) {
            const RECT whole = {0, 0, bw, bh};
            s.target.Analyse(static_cast<const uint8_t *>(bits), bw * 4, (t0 + t1) / 2, 0, 0, 2, whole);
            last = hash;
        }
        Sleep(1);
    }
    Log("surface samples=%llu slowest_ms=%.2f", (unsigned long long)s.samples, s.slowest);
    std::fclose(reads);
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    return 0;
}

DWORD WINAPI Watchdog(void *)
{
    for (;;) {
        Sleep(200);
        if (Qpc() > g_deadline + 5 * g_freq) {
            if (g_log) Log("watchdog fired");
            ExitProcess(4);
        }
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    InitializeCriticalSection(&g_logLock);
    if (argc == 2 && !wcscmp(argv[1], L"--self-test")) {
        // A windows-subsystem program has no console. Standard output is used when the caller gave one (a file);
        // otherwise the lines go into the parent's console, when there is one.
        if (_fileno(stdout) < 0 && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
        return SelfTest();
    }
    if (argc != 6 && !(argc == 7 && !wcscmp(argv[6], L"surface"))) return 2;
    const bool readSurface = argc == 7;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    const long long deadlineMs = _wtoll(argv[2]);
    const int seconds = _wtoi(argv[5]);
    if (deadlineMs < 1000 || deadlineMs > 175000 || seconds < 1) return 2;
    g_deadline = Qpc() + deadlineMs * g_freq / 1000;

    g_prefix = argv[1];
    if (_wfopen_s(&g_log, (g_prefix + L"-log.txt").c_str(), L"w") || !g_log) return 2;
    if (_wfopen_s(&g_csv, (g_prefix + L"-frames.csv").c_str(), L"w") || !g_csv) return 2;
    std::fprintf(g_csv, "frame,target,qpc,present_qpc,accumulated,mode,dirty,dx0,dy0,dx1,dy1,navy,cyan,black,other,hash\n");
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    // Physical pixels everywhere: the windows' rectangles and the duplicated image must agree at any scale.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Log("start qpc=%lld freq=%lld filetime=%llu session=%lu console=%lu", (long long)Qpc(), (long long)g_freq,
        ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime, session, WTSGetActiveConsoleSessionId());

    Dda dda;
    const bool haveDda = dda.Open();
    g_mode = haveDda ? "dda" : "gdi";
    Log("mode=%s", g_mode.c_str());

    Run run;
    run.targets.reserve(2);
    run.targets.emplace_back("window", nullptr);
    run.targets.emplace_back("overlay", L"bc250mon.exe");
    // The arguments of the logon task, with this script and bound (common.ps1 Get-LogonTaskPlan).
    std::wstring cmd = L"\"" + std::wstring(argv[3]) + L"\" -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" +
                       argv[4] + L"\" -Seconds " + std::to_wstring(seconds);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    const int64_t before = Qpc();
    if (!CreateProcessW(argv[3], line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &run.child)) {
        Log("child create-failed error=%lu", GetLastError());
        Log("end frames=0 child_exit=- window=0");
        return 1;
    }
    run.targets[0].pid = run.child.dwProcessId;
    Log("child pid=%lu qpc=%lld", run.child.dwProcessId, (long long)before);
    Surface surface;
    surface.window = &run.targets[0];
    HANDLE surfaceThread = readSurface ? CreateThread(nullptr, 0, SurfaceLoop, &surface, 0, nullptr) : nullptr;

    bool read = haveDda && dda.Loop(run);
    if (!read && !run.Done()) {
        // Duplication delivered nothing usable: the rest of the run reads the screen through GDI.
        g_mode = "gdi";
        Log("mode=gdi qpc=%lld", (long long)Qpc());
        for (auto &t : run.targets) t.staging.Reset();
        read = GdiLoop(run);
    }
    InterlockedExchange(&g_stop, 1);
    if (surfaceThread) {
        WaitForSingleObject(surfaceThread, 3000);
        CloseHandle(surfaceThread);
    }
    if (run.ChildRunning()) {
        // Only the process this tool started, and only because it outlived the deadline.
        TerminateProcess(run.child.hProcess, 1);
        WaitForSingleObject(run.child.hProcess, 5000);
        Log("child ended-at-deadline");
    }
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(run.child.hProcess, &code);
    timeEndPeriod(1);
    std::vector<const Target *> all;
    for (const auto &t : run.targets) all.push_back(&t);
    if (readSurface) all.push_back(&surface.target);
    for (const Target *t : all)
        Log("summary target=%s found=%d analysed=%llu changes=%llu flashes=%llu accumulated_misses=%llu", t->name,
            t->have ? 1 : 0, (unsigned long long)t->analysed, (unsigned long long)t->detector.Changes(),
            (unsigned long long)t->flashes, (unsigned long long)t->accumulatedMisses);
    Log("end frames=%llu child_exit=%lu window=%d", (unsigned long long)g_frames, code, run.targets[0].have ? 1 : 0);
    std::fclose(g_csv);
    std::fclose(g_log);
    return (run.targets[0].have && read) ? 0 : 1;
}
