// SPDX-License-Identifier: MIT
// common.cpp - the run frame of capshare: log file shared by both processes, the debug-string capture, the stage
// watchdog, the pattern oracle, the verdict line and result.json, the peer process with its pipe, adapter choice,
// the loaded-module route tag and the optional DBWIN listener (the protocol dcompwit.exe uses).
#include "capshare.h"
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

Options g_opt;
Ipc g_ipc;
Peer g_peerProcess;

// ------------------------------------------------------------------------------------------------ time and text
ULONGLONG Now() { return GetTickCount64() - g_opt.t0; }

DWORD Remaining(ULONGLONG deadlineTick)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= deadlineTick) return 0;
    const ULONGLONG left = deadlineTick - now;
    return left > 0x7FFFFFFFull ? 0x7FFFFFFFu : (DWORD)left;
}

ULONGLONG WaitDeadline() { return g_opt.deadline > 800 ? g_opt.deadline - 800 : g_opt.deadline; }

std::string HrText(HRESULT hr)
{
    char t[16];
    _snprintf_s(t, _TRUNCATE, "0x%08lX", (unsigned long)hr);
    return t;
}

std::string Narrow(const std::wstring &w)
{
    std::string s;
    for (wchar_t c : w) s += (c >= 32 && c < 127) ? (char)c : '?';
    return s;
}

std::wstring Widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }

std::string FormatText(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "fmt" + std::to_string((int)f);
    }
}

const char *FlText(D3D_FEATURE_LEVEL fl)
{
    switch (fl) {
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    default: return "?";
    }
}

// Values in the verdict line and in pipe messages are single tokens.
static std::string Token(std::string s)
{
    if (s.empty()) return "-";
    for (char &c : s)
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') c = '_';
        else if (c == '"') c = '\'';
    return s;
}

// ------------------------------------------------------------------------------------------------ log
static SRWLOCK g_logLock = SRWLOCK_INIT;
static HANDLE g_logFile = INVALID_HANDLE_VALUE, g_logMirror = nullptr;

bool LogOpen(const std::wstring &path, bool truncate, bool mirrorStdout)
{
    if (truncate) {
        HANDLE t = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (t == INVALID_HANDLE_VALUE) return false;
        CloseHandle(t);
    }
    // Append-only access: the peer process writes into the same file and every line is one atomic append.
    g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (mirrorStdout) {
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
        if (so && so != INVALID_HANDLE_VALUE) {
            const DWORD type = GetFileType(so);
            if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR) g_logMirror = so;
        }
    }
    return g_logFile != INVALID_HANDLE_VALUE;
}

void Log(const char *format, ...)
{
    char body[3500];
    va_list args;
    va_start(args, format);
    int n = _vsnprintf_s(body, _TRUNCATE, format, args);
    va_end(args);
    if (n < 0) n = (int)strlen(body);
    char line[3600];
    int m = _snprintf_s(line, _TRUNCATE, "%c %6llu %s\n", g_opt.peer ? 'B' : 'A', Now(), body);
    if (m < 0) {
        m = (int)strlen(line);
        if (m > 0) line[m - 1] = '\n';
    }
    AcquireSRWLockExclusive(&g_logLock);
    DWORD written = 0;
    if (g_logFile != INVALID_HANDLE_VALUE) WriteFile(g_logFile, line, (DWORD)m, &written, nullptr);
    if (g_logMirror) WriteFile(g_logMirror, line, (DWORD)m, &written, nullptr);
    ReleaseSRWLockExclusive(&g_logLock);
}

// ------------------------------------------------------------------------------------------------ debug strings
// OutputDebugStringA raises DBG_PRINTEXCEPTION_C, OutputDebugStringW DBG_PRINTEXCEPTION_WIDE_C and then the narrow one
// with the same text. With no debugger attached a vectored handler sees both first; it only copies into preallocated
// slots, FlushOds writes them out. This is where the D3D11 shell's "M14 ..." lines, the D3D12 shell's
// "amdgpu_wddm_d3d12 failure ..." lines and the runtimes' "Removing Device" lines of this process arrive.
static const LONG OdsSlots = 4096;
static char g_ods[OdsSlots][480];
static volatile LONG g_odsReady[OdsSlots];
static volatile LONG g_odsNext = 0;
static LONG g_odsWritten = 0;
static SRWLOCK g_odsLock = SRWLOCK_INIT;
static thread_local bool t_skipNarrow = false;

static void CopyDebugText(char *out, size_t capacity, const void *text, bool wide, ULONG_PTR length) noexcept
{
    __try {
        size_t i = 0;
        if (wide) {
            const wchar_t *w = (const wchar_t *)text;
            for (; i + 1 < capacity && i < length && w[i]; ++i)
                out[i] = (w[i] == L'\n' || w[i] == L'\r') ? ' ' : (w[i] >= 32 && w[i] < 127 ? (char)w[i] : '?');
        } else {
            const char *a = (const char *)text;
            for (; i + 1 < capacity && i < length && a[i]; ++i) out[i] = (a[i] == '\n' || a[i] == '\r') ? ' ' : a[i];
        }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(out, capacity, "<unreadable debug string>");
    }
}

static LONG CALLBACK DebugStringHandler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD &r = *e->ExceptionRecord;
    const bool narrow = r.ExceptionCode == 0x40010006L, wide = r.ExceptionCode == 0x4001000AL;
    if ((!narrow && !wide) || r.NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    if (narrow && t_skipNarrow) {
        t_skipNarrow = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (wide) t_skipNarrow = true;
    const LONG slot = InterlockedIncrement(&g_odsNext) - 1;
    if (slot < OdsSlots) {
        CopyDebugText(g_ods[slot], sizeof(g_ods[slot]), (const void *)r.ExceptionInformation[1], wide,
                      r.ExceptionInformation[0]);
        InterlockedExchange(&g_odsReady[slot], 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallDebugStringCapture() { AddVectoredExceptionHandler(1, DebugStringHandler); }

void FlushOds()
{
    AcquireSRWLockExclusive(&g_odsLock);
    const LONG next = g_odsNext;
    const LONG claimed = std::min<LONG>(next, OdsSlots);
    while (g_odsWritten < claimed && g_odsReady[g_odsWritten]) {
        Log("ODS %s", g_ods[g_odsWritten]);
        ++g_odsWritten;
    }
    ReleaseSRWLockExclusive(&g_odsLock);
}

// ------------------------------------------------------------------------------------------------ stage and watchdog
static std::atomic<const char *> g_stage{"start"};
static void (*g_onTimeout)() = nullptr;

void SetStage(const char *stage)
{
    FlushOds();
    g_stage = stage;
    Log("STAGE %s", stage);
}

const char *Stage() { return g_stage.load(); }

static DWORD WINAPI WatchdogThread(LPVOID)
{
    while (Remaining(g_opt.deadline)) Sleep(std::min<DWORD>(Remaining(g_opt.deadline), 250));
    FlushOds();
    Log("WATCHDOG bound reached in stage %s", Stage());
    if (g_onTimeout) g_onTimeout();
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

void StartWatchdog(void (*onTimeout)())
{
    g_onTimeout = onTimeout;
    HANDLE t = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// ------------------------------------------------------------------------------------------------ pattern oracle
static inline uint32_t Mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint32_t PatternPixel(Pattern p, int x, int y, bool opaque)
{
    if (p == Pattern::Poison) return PoisonRgba;
    const uint32_t seed = p == Pattern::A ? SeedA : SeedB;
    const uint32_t v = Mix((uint32_t)x * 0x9E3779B1u ^ Mix((uint32_t)y * 0x85EBCA77u ^ seed));
    return opaque ? (v | 0xFF000000u) : v;
}

uint32_t SwapRB(uint32_t v) { return (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu); }

bool IsBgra(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM;
}

bool IsRgba(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

std::vector<uint32_t> MakeImageMemory(Pattern p, UINT w, UINT h, DXGI_FORMAT f, bool opaque)
{
    std::vector<uint32_t> m((size_t)w * h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            const uint32_t v = PatternPixel(p, (int)x, (int)y, opaque);
            m[(size_t)y * w + x] = IsBgra(f) ? SwapRB(v) : v;
        }
    return m;
}

bool ImageFromRows(Image &img, const void *data, UINT rowPitch, int w, int h, DXGI_FORMAT f)
{
    if (!IsBgra(f) && !IsRgba(f)) return false;
    img.w = w;
    img.h = h;
    img.px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const uint32_t *row = (const uint32_t *)((const unsigned char *)data + (size_t)y * rowPitch);
        for (int x = 0; x < w; ++x) {
            uint32_t v = IsBgra(f) ? SwapRB(row[x]) : row[x];
            if (f == DXGI_FORMAT_B8G8R8X8_UNORM) v |= 0xFF000000u;
            img.px[(size_t)y * w + x] = v;
        }
    }
    return true;
}

Image Crop(const Image &img, int x0, int y0, int w, int h)
{
    Image out;
    out.w = w;
    out.h = h;
    out.px.assign((size_t)w * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx >= 0 && sy >= 0 && sx < img.w && sy < img.h) out.px[(size_t)y * w + x] = img.px[(size_t)sy * img.w + sx];
        }
    return out;
}

std::string RgbaText(uint32_t v)
{
    char t[24];
    _snprintf_s(t, _TRUNCATE, "rgba:%02x%02x%02x%02x", v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24);
    return t;
}

// What the image holds instead: poison (the creator's initial content: no write arrived), mostly the expected pattern
// (partial), the other pattern (a stale or early read), the expected pattern with R and B exchanged, zero, one
// constant colour, or a mix.
static std::string Classify(const Image &img, Pattern expected, bool opaque, bool rgbOnly)
{
    const uint32_t mask = rgbOnly ? 0x00FFFFFFu : 0xFFFFFFFFu;
    const size_t n = img.px.size();
    if (!n) return "empty";
    size_t poison = 0, a = 0, b = 0, zero = 0, swapped = 0;
    bool constant = true;
    const uint32_t first = img.px[0] & mask;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const uint32_t v = img.px[(size_t)y * img.w + x] & mask;
            if (v == (PoisonRgba & mask)) ++poison;
            if (v == (PatternPixel(Pattern::A, x, y, opaque) & mask)) ++a;
            if (v == (PatternPixel(Pattern::B, x, y, opaque) & mask)) ++b;
            if ((v & 0x00FFFFFFu) == 0) ++zero;
            if (v == (SwapRB(PatternPixel(expected, x, y, opaque)) & mask)) ++swapped;
            if (v != first) constant = false;
        }
    auto most = [n](size_t c) { return c * 2 > n; };
    if (most(poison)) return "poison";
    if (most(expected == Pattern::A ? a : b)) return "partial"; // mostly right: diff counts the wrong pixels
    if (most(a)) return "pattern-a";
    if (most(b)) return "pattern-b";
    if (most(swapped)) return "swapped-rb";
    if (most(zero)) return "zero";
    if (constant) return "constant-" + RgbaText(first);
    char t[128];
    _snprintf_s(t, _TRUNCATE, "mixed(a=%zu,b=%zu,poison=%zu,zero=%zu,of=%zu)", a, b, poison, zero, n);
    return t;
}

Check CompareImage(const char *what, const Image &img, Pattern p, bool opaque, bool rgbOnly, int tolerance)
{
    Check c;
    c.what = what;
    c.total = (size_t)img.w * img.h;
    const uint32_t mask = rgbOnly ? 0x00FFFFFFu : 0xFFFFFFFFu;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const uint32_t raw = img.px[(size_t)y * img.w + x], want = PatternPixel(p, x, y, opaque);
            const uint32_t got = raw & mask, expect = want & mask;
            if (got == expect) continue;
            int delta = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                if (!((mask >> shift) & 0xFFu)) continue;
                const int d = abs((int)((got >> shift) & 0xFFu) - (int)((expect >> shift) & 0xFFu));
                delta = std::max(delta, d);
            }
            c.maxDelta = std::max(c.maxDelta, delta);
            if (delta <= tolerance) continue;
            if (!c.diff) {
                c.x = x;
                c.y = y;
                c.got = raw;
                c.want = want;
            }
            ++c.diff;
        }
    c.pass = c.diff == 0 && c.total > 0;
    if (!c.pass) c.content = c.total ? Classify(img, p, opaque, rgbOnly) : "empty";
    return c;
}

// Searches the central block of the expected window image (at most 64x64) inside `big`, where the window's origin
// should be at (bx, by), at offsets up to `margin` in rings of growing distance. A hit means the content is there but
// displaced (borders, DPI, a wrong rectangle); (0, 0) means the centre matches and the difference is elsewhere.
bool FindShift(const Image &big, int bx, int by, int w, int h, Pattern p, int margin, int tolerance, int &dx, int &dy)
{
    const int bw = std::min(64, w), bh = std::min(64, h), lx = (w - bw) / 2, ly = (h - bh) / 2;
    for (int r = 0; r <= margin; ++r)
        for (int sy = -r; sy <= r; ++sy)
            for (int sx = -r; sx <= r; ++sx) {
                if (std::max(abs(sx), abs(sy)) != r) continue;
                bool ok = true;
                for (int y = 0; ok && y < bh; ++y)
                    for (int x = 0; ok && x < bw; ++x) {
                        const int X = bx + lx + x + sx, Y = by + ly + y + sy;
                        if (X < 0 || Y < 0 || X >= big.w || Y >= big.h) {
                            ok = false;
                            break;
                        }
                        const uint32_t got = big.px[(size_t)Y * big.w + X], want = PatternPixel(p, lx + x, ly + y, true);
                        for (int shift = 0; shift < 24; shift += 8)
                            if (abs((int)((got >> shift) & 0xFFu) - (int)((want >> shift) & 0xFFu)) > tolerance) ok = false;
                    }
                if (ok) {
                    dx = sx;
                    dy = sy;
                    return true;
                }
            }
    return false;
}

std::string CheckText(const Check &c)
{
    char t[512];
    _snprintf_s(t, _TRUNCATE, "what=%s pass=%u at=%d,%d got=%s want=%s diff=%zu total=%zu maxd=%d content=%s note=%s",
                Token(c.what).c_str(), c.pass ? 1u : 0u, c.x, c.y, RgbaText(c.got).c_str(), RgbaText(c.want).c_str(),
                c.diff, c.total, c.maxDelta, Token(c.content).c_str(), Token(c.note).c_str());
    return t;
}

std::string Field(const std::string &msg, const char *key)
{
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < msg.size()) {
        size_t end = msg.find(' ', pos);
        if (end == std::string::npos) end = msg.size();
        if (msg.compare(pos, k.size(), k) == 0) return msg.substr(pos + k.size(), end - pos - k.size());
        pos = end + 1;
    }
    return "";
}

static uint32_t ParseRgba(const std::string &t)
{
    if (t.size() != 13 || t.compare(0, 5, "rgba:") != 0) return 0;
    const uint32_t v = (uint32_t)strtoul(t.c_str() + 5, nullptr, 16); // rrggbbaa
    return ((v >> 24) & 0xFFu) | (((v >> 16) & 0xFFu) << 8) | (((v >> 8) & 0xFFu) << 16) | ((v & 0xFFu) << 24);
}

Check CheckFromText(const std::string &msg)
{
    Check c;
    c.what = Field(msg, "what");
    c.pass = Field(msg, "pass") == "1";
    const std::string at = Field(msg, "at");
    if (sscanf_s(at.c_str(), "%d,%d", &c.x, &c.y) != 2) c.x = c.y = -1;
    c.got = ParseRgba(Field(msg, "got"));
    c.want = ParseRgba(Field(msg, "want"));
    c.diff = (size_t)_strtoui64(Field(msg, "diff").c_str(), nullptr, 10);
    c.total = (size_t)_strtoui64(Field(msg, "total").c_str(), nullptr, 10);
    c.maxDelta = atoi(Field(msg, "maxd").c_str());
    c.content = Field(msg, "content");
    c.note = Field(msg, "note");
    if (c.what.empty()) c.what = "?";
    return c;
}

// ------------------------------------------------------------------------------------------------ verdict
struct VerdictState {
    std::string cell = "-", result = "pass", side = "-", stage = "-", call = "-", hr = "-", at = "-", got = "-",
                want = "-", content = "-", note = "-", gate = "-";
    size_t diff = 0, total = 0;
    int maxDelta = 0;
    std::string routeA = "-", routeB = "-", flA = "-", flB = "-";
    std::vector<std::string> checks;
    std::vector<std::pair<std::string, std::string>> extra;
    bool decided = false, emitted = false;
    int exitCode = 0;
};
static VerdictState g_v;
static SRWLOCK g_vLock = SRWLOCK_INIT;

void VerdictInit(const std::string &cell) { g_v.cell = cell; }

void VerdictFail(char side, const std::string &stage, const std::string &call, HRESULT hr, const std::string &note)
{
    Log("FAIL side=%c stage=%s call=%s hr=%s note=%s", side, stage.c_str(), call.c_str(), HrText(hr).c_str(),
        Token(note).c_str());
    AcquireSRWLockExclusive(&g_vLock);
    if (!g_v.decided) {
        g_v.decided = true;
        g_v.result = "fail";
        g_v.side = std::string(1, side);
        g_v.stage = Token(stage);
        g_v.call = Token(call);
        g_v.hr = HrText(hr);
        g_v.note = Token(note);
    }
    ReleaseSRWLockExclusive(&g_vLock);
}

void VerdictTimeout(char side, const std::string &stage, const std::string &note)
{
    Log("TIMEOUT side=%c stage=%s note=%s", side, stage.c_str(), Token(note).c_str());
    AcquireSRWLockExclusive(&g_vLock);
    if (!g_v.decided) {
        g_v.decided = true;
        g_v.result = "timeout";
        g_v.side = std::string(1, side);
        g_v.stage = Token(stage);
        g_v.note = Token(note);
    }
    ReleaseSRWLockExclusive(&g_vLock);
}

void VerdictCheck(char side, const Check &c)
{
    Log("CHECK side=%c %s", side, CheckText(c).c_str());
    AcquireSRWLockExclusive(&g_vLock);
    g_v.checks.push_back(std::string(1, side) + ":" + Token(c.what) + "=" + (c.pass ? "pass" : "mismatch"));
    if (!c.pass && !g_v.decided) {
        g_v.decided = true;
        g_v.result = "mismatch";
        g_v.side = std::string(1, side);
        g_v.stage = Token(c.what);
        char at[32];
        _snprintf_s(at, _TRUNCATE, "%d,%d", c.x, c.y);
        g_v.at = at;
        g_v.got = RgbaText(c.got);
        g_v.want = RgbaText(c.want);
        g_v.diff = c.diff;
        g_v.total = c.total;
        g_v.maxDelta = c.maxDelta;
        g_v.content = Token(c.content);
        g_v.note = Token(c.note);
    }
    ReleaseSRWLockExclusive(&g_vLock);
}

void VerdictGate(const std::string &what, bool held, const std::string &detail)
{
    Log("GATE what=%s held=%u %s", what.c_str(), held ? 1u : 0u, detail.c_str());
    AcquireSRWLockExclusive(&g_vLock);
    if (g_v.gate == "-" || (g_v.gate == "held" && !held)) g_v.gate = held ? "held" : "violated:" + Token(what);
    if (!held && !g_v.decided) {
        g_v.decided = true;
        g_v.result = "mismatch";
        g_v.stage = Token(what);
        g_v.note = Token(detail);
    }
    ReleaseSRWLockExclusive(&g_vLock);
}

void VerdictRoute(char side, const std::string &route, const std::string &fl)
{
    AcquireSRWLockExclusive(&g_vLock);
    (side == 'A' ? g_v.routeA : g_v.routeB) = Token(route);
    (side == 'A' ? g_v.flA : g_v.flB) = Token(fl);
    ReleaseSRWLockExclusive(&g_vLock);
}

void VerdictNote(const std::string &key, const std::string &value)
{
    AcquireSRWLockExclusive(&g_vLock);
    g_v.extra.push_back({key, Token(value)});
    ReleaseSRWLockExclusive(&g_vLock);
}

void VerdictRemoved(char side, HRESULT removed)
{
    if (removed != S_OK) VerdictNote(std::string("removed_") + side, HrText(removed));
}

bool VerdictFailed()
{
    AcquireSRWLockShared(&g_vLock);
    const bool failed = g_v.decided && g_v.result != "pass";
    ReleaseSRWLockShared(&g_vLock);
    return failed;
}

static std::string JsonString(const std::string &s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c < 32) continue;
        o += c;
    }
    return o + "\"";
}

int VerdictEmit()
{
    FlushOds();
    AcquireSRWLockExclusive(&g_vLock);
    if (g_v.emitted) {
        const int code = g_v.exitCode;
        ReleaseSRWLockExclusive(&g_vLock);
        return code;
    }
    g_v.emitted = true;
    std::string checks;
    for (const std::string &c : g_v.checks) checks += (checks.empty() ? "" : ",") + c;
    if (checks.empty()) checks = "-";
    char line[3000];
    _snprintf_s(line, _TRUNCATE,
                "VERDICT cell=%s result=%s side=%s stage=%s call=%s hr=%s at=%s got=%s want=%s diff=%zu/%zu "
                "max_delta=%d content=%s gate=%s route=A:%s,B:%s fl=A:%s,B:%s checks=%s elapsed_ms=%llu note=%s",
                g_v.cell.c_str(), g_v.result.c_str(), g_v.side.c_str(), g_v.stage.c_str(), g_v.call.c_str(),
                g_v.hr.c_str(), g_v.at.c_str(), g_v.got.c_str(), g_v.want.c_str(), g_v.diff, g_v.total, g_v.maxDelta,
                g_v.content.c_str(), g_v.gate.c_str(), g_v.routeA.c_str(), g_v.routeB.c_str(), g_v.flA.c_str(),
                g_v.flB.c_str(), checks.c_str(), Now(), g_v.note.c_str());
    std::string full = line;
    for (const auto &e : g_v.extra) full += " " + e.first + "=" + e.second;
    g_v.exitCode = g_v.result == "pass" ? 0 : g_v.result == "mismatch" ? 1 : g_v.result == "fail" ? 2 : 3;

    std::string j = "{\n";
    auto add = [&j](const char *k, const std::string &v, bool quote = true) {
        j += std::string("  ") + JsonString(k) + ": " + (quote ? JsonString(v) : v) + ",\n";
    };
    add("tool", "capshare");
    add("cell", g_v.cell);
    add("result", g_v.result);
    add("side", g_v.side);
    add("stage", g_v.stage);
    add("call", g_v.call);
    add("hr", g_v.hr);
    add("at", g_v.at);
    add("got", g_v.got);
    add("want", g_v.want);
    add("diff", std::to_string(g_v.diff), false);
    add("total", std::to_string(g_v.total), false);
    add("max_delta", std::to_string(g_v.maxDelta), false);
    add("content", g_v.content);
    add("gate", g_v.gate);
    add("route_a", g_v.routeA);
    add("route_b", g_v.routeB);
    add("fl_a", g_v.flA);
    add("fl_b", g_v.flB);
    std::string arr = "[";
    for (size_t i = 0; i < g_v.checks.size(); ++i) arr += (i ? ", " : "") + JsonString(g_v.checks[i]);
    add("checks", arr + "]", false);
    add("elapsed_ms", std::to_string(Now()), false);
    add("note", g_v.note);
    for (const auto &e : g_v.extra) add(e.first.c_str(), e.second);
    add("exit_code", std::to_string(g_v.exitCode), false);
    j += "  " + JsonString("verdict") + ": " + JsonString(full) + "\n}\n";
    const int code = g_v.exitCode;
    ReleaseSRWLockExclusive(&g_vLock);

    Log("%s", full.c_str());
    HANDLE f = CreateFileW(g_opt.json.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, j.data(), (DWORD)j.size(), &written, nullptr);
        CloseHandle(f);
    } else {
        Log("JSON write failed error=%lu", GetLastError());
    }
    return code;
}

// ------------------------------------------------------------------------------------------------ pipe
static SRWLOCK g_ipcLock = SRWLOCK_INIT, g_sendLock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_ipcCv = CONDITION_VARIABLE_INIT;
static std::deque<std::string> g_ipcQueue;
static bool g_ipcEof = false;

static bool VerbIs(const std::string &msg, const char *verb)
{
    const size_t n = strlen(verb);
    return msg.compare(0, n, verb) == 0 && (msg.size() == n || msg[n] == ' ');
}

static DWORD WINAPI IpcReader(LPVOID)
{
    std::string partial;
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(g_opt.ipcIn, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        partial.append(buf, n);
        size_t pos;
        while ((pos = partial.find('\n')) != std::string::npos) {
            std::string line = partial.substr(0, pos);
            partial.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            AcquireSRWLockExclusive(&g_ipcLock);
            g_ipcQueue.push_back(line);
            ReleaseSRWLockExclusive(&g_ipcLock);
            WakeAllConditionVariable(&g_ipcCv);
        }
    }
    AcquireSRWLockExclusive(&g_ipcLock);
    g_ipcEof = true;
    ReleaseSRWLockExclusive(&g_ipcLock);
    WakeAllConditionVariable(&g_ipcCv);
    return 0;
}

bool Ipc::Start()
{
    HANDLE t = CreateThread(nullptr, 0, IpcReader, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

bool Ipc::Send(const char *format, ...)
{
    char body[1500];
    va_list args;
    va_start(args, format);
    int n = _vsnprintf_s(body, _TRUNCATE, format, args);
    va_end(args);
    if (n < 0) n = (int)strlen(body);
    Log("-> %s", body);
    std::string line(body, (size_t)n);
    line += '\n';
    AcquireSRWLockExclusive(&g_sendLock);
    DWORD written = 0;
    const BOOL ok = g_opt.ipcOut && WriteFile(g_opt.ipcOut, line.data(), (DWORD)line.size(), &written, nullptr);
    ReleaseSRWLockExclusive(&g_sendLock);
    return ok && written == line.size();
}

bool Ipc::Expect(const char *verb, std::string &msg, ULONGLONG deadlineTick)
{
    AcquireSRWLockExclusive(&g_ipcLock);
    for (;;) {
        for (auto it = g_ipcQueue.begin(); it != g_ipcQueue.end(); ++it)
            if (VerbIs(*it, verb) || VerbIs(*it, "FAIL")) {
                msg = *it;
                g_ipcQueue.erase(it);
                ReleaseSRWLockExclusive(&g_ipcLock);
                return VerbIs(msg, verb);
            }
        if (g_ipcEof) {
            msg = "EOF";
            break;
        }
        const DWORD left = Remaining(deadlineTick);
        if (!left) {
            msg = "TIMEOUT";
            break;
        }
        SleepConditionVariableSRW(&g_ipcCv, &g_ipcLock, left, 0);
    }
    ReleaseSRWLockExclusive(&g_ipcLock);
    return false;
}

bool Ipc::Has(const char *verb)
{
    AcquireSRWLockShared(&g_ipcLock);
    bool found = false;
    for (const std::string &m : g_ipcQueue)
        if (VerbIs(m, verb)) found = true;
    ReleaseSRWLockShared(&g_ipcLock);
    return found;
}

bool Ipc::TryNext(std::string &msg)
{
    AcquireSRWLockExclusive(&g_ipcLock);
    const bool any = !g_ipcQueue.empty();
    if (any) {
        msg = g_ipcQueue.front();
        g_ipcQueue.pop_front();
    } else if (g_ipcEof) {
        msg = "EOF";
    }
    ReleaseSRWLockExclusive(&g_ipcLock);
    return any || msg == "EOF";
}

// ------------------------------------------------------------------------------------------------ peer process
static HANDLE DupInheritable(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, TRUE, DUPLICATE_SAME_ACCESS)) return nullptr;
    return out;
}

static std::wstring HexW(ULONG_PTR v)
{
    wchar_t t[32];
    _snwprintf_s(t, _TRUNCATE, L"%llx", (unsigned long long)v);
    return t;
}

static std::wstring PeerExePath()
{
    std::wstring p = g_opt.peerExe.empty() ? g_opt.self : g_opt.peerExe;
    if (p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'))) return p;
    const size_t slash = g_opt.self.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : g_opt.self.substr(0, slash + 1)) + p;
}

static std::wstring PeerArgs()
{
    std::wstring a = L"--role peer --cell " + Widen(g_opt.cell);
    a += L" --size " + std::to_wstring(g_opt.w) + L"x" + std::to_wstring(g_opt.h);
    a += g_opt.format == DXGI_FORMAT_R8G8B8A8_UNORM ? L" --format rgba8" : L" --format bgra8";
    a += g_opt.syncFence ? L" --sync fence" : L" --sync cpu";
    a += g_opt.kmt ? L" --handle kmt" : L" --handle nt";
    a += L" --delay-copies " + std::to_wstring(g_opt.delayCopies) + L" --delay-size " + std::to_wstring(g_opt.delaySize);
    a += L" --gate-ms " + std::to_wstring(g_opt.gateMs) + L" --tolerance " + std::to_wstring(g_opt.tolerance);
    a += g_opt.producerGdi ? L" --producer gdi" : L" --producer d3d12";
    if (g_opt.x != INT_MIN) a += L" --x " + std::to_wstring(g_opt.x) + L" --y " + std::to_wstring(g_opt.y);
    if (g_opt.simultaneous) a += L" --simultaneous";
    if (g_opt.skipWait) a += L" --inject skip-wait";
    if (g_opt.interactiveOk) a += L" --interactive-ok";
    a += L" --t0 " + std::to_wstring(g_opt.t0);
    // The peer stops 1.5 s ahead of the parent, so that its own timeout report still reaches the parent.
    a += L" --deadline " + std::to_wstring(g_opt.deadline > 2500 ? g_opt.deadline - 1500 : g_opt.deadline);
    a += L" --out \"" + g_opt.out + L"\"";
    if (g_opt.haveLuid)
        a += L" --luid " + HexW((ULONG_PTR)(ULONG)g_opt.luid.HighPart) + L":" + HexW((ULONG_PTR)g_opt.luid.LowPart);
    return a;
}

bool SpawnPeer(std::string &error)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE toPeerRead = nullptr, toPeerWrite = nullptr, fromPeerRead = nullptr, fromPeerWrite = nullptr;
    if (!CreatePipe(&toPeerRead, &toPeerWrite, &sa, 0) || !CreatePipe(&fromPeerRead, &fromPeerWrite, &sa, 0)) {
        error = "CreatePipe error=" + std::to_string(GetLastError());
        return false;
    }
    SetHandleInformation(toPeerWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromPeerRead, HANDLE_FLAG_INHERIT, 0);
    // The peer gets our standard handles, so the UMD diagnostics it writes to stderr land in the same file as ours.
    HANDLE stdOut = DupInheritable(GetStdHandle(STD_OUTPUT_HANDLE)), stdErr = DupInheritable(GetStdHandle(STD_ERROR_HANDLE));
    HANDLE inherit[4];
    DWORD count = 0;
    inherit[count++] = toPeerRead;
    inherit[count++] = fromPeerWrite;
    if (stdOut) inherit[count++] = stdOut;
    if (stdErr) inherit[count++] = stdErr;

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = stdOut;
    si.StartupInfo.hStdError = stdErr;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attr(size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size) ||
        !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   count * sizeof(HANDLE), nullptr, nullptr)) {
        error = "ProcThreadAttribute error=" + std::to_string(GetLastError());
        return false;
    }
    const std::wstring exe = PeerExePath();
    std::wstring cmd = L"\"" + exe + L"\" " + PeerArgs() + L" --ipc " + HexW((ULONG_PTR)toPeerRead) + L"," +
                       HexW((ULONG_PTR)fromPeerWrite);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    PROCESS_INFORMATION pi = {};
    const BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    const DWORD createError = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(toPeerRead);
    CloseHandle(fromPeerWrite);
    if (stdOut) CloseHandle(stdOut);
    if (stdErr) CloseHandle(stdErr);
    if (!ok) {
        error = "CreateProcess(" + Narrow(exe) + ") error=" + std::to_string(createError);
        CloseHandle(toPeerWrite);
        CloseHandle(fromPeerRead);
        return false;
    }
    // The peer dies with this process (kill on job close), whatever ends it.
    g_peerProcess.job = CreateJobObjectW(nullptr, nullptr);
    if (g_peerProcess.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_peerProcess.job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        if (!AssignProcessToJobObject(g_peerProcess.job, pi.hProcess))
            Log("PEER job assignment failed error=%lu (the peer still stops at its own bound)", GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_peerProcess.process = pi.hProcess;
    g_peerProcess.pid = pi.dwProcessId;
    g_opt.ipcIn = fromPeerRead;
    g_opt.ipcOut = toPeerWrite;
    Log("PEER started pid=%lu exe=%s", pi.dwProcessId, Narrow(exe).c_str());
    return g_ipc.Start();
}

HANDLE DupToPeer(HANDLE h)
{
    HANDLE out = nullptr;
    if (!h || !g_peerProcess.process) return nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, g_peerProcess.process, &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Log("DuplicateHandle into the peer failed error=%lu", GetLastError());
        return nullptr;
    }
    return out;
}

DWORD PeerExit(DWORD waitMs)
{
    if (!g_peerProcess.process) return 0;
    WaitForSingleObject(g_peerProcess.process, waitMs);
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(g_peerProcess.process, &code);
    return code;
}

void KillPeer()
{
    if (g_peerProcess.job) TerminateJobObject(g_peerProcess.job, 9);
    else if (g_peerProcess.process) TerminateProcess(g_peerProcess.process, 9);
}

void VerdictFromPeer(const std::string &msg, const char *stage)
{
    if (VerbIs(msg, "FAIL")) {
        const std::string kind = Field(msg, "kind"), pstage = Field(msg, "stage"), note = Field(msg, "note");
        if (kind == "timeout") VerdictTimeout('B', pstage, note.empty() ? "-" : note);
        else
            VerdictFail('B', pstage, Field(msg, "call"), (HRESULT)strtoul(Field(msg, "hr").c_str(), nullptr, 16),
                        note.empty() ? "-" : note);
        VerdictRemoved('B', (HRESULT)strtoul(Field(msg, "removed").c_str(), nullptr, 16));
    } else if (msg == "EOF") {
        const DWORD code = PeerExit(2000);
        VerdictFail('B', stage, "peer-exited", (HRESULT)code, "the peer ended without reporting");
    } else {
        VerdictTimeout('A', std::string("waiting-for-peer:") + stage);
    }
}

bool PeerStep(const char *verb, std::string &msg, const char *stage)
{
    SetStage(stage);
    if (g_ipc.Expect(verb, msg, WaitDeadline())) return true;
    VerdictFromPeer(msg, stage);
    return false;
}

// ------------------------------------------------------------------------------------------------ adapters, modules
bool PickAdapter(ComPtr<IDXGIAdapter1> &adapter, LUID &luid, std::string &error)
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = "CreateDXGIFactory2 hr=" + HrText(hr);
        return false;
    }
    int chosen = -1;
    const int wanted = g_opt.adapter == L"auto" ? -1 : _wtoi(g_opt.adapter.c_str());
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        UINT flags3 = 0;
        ComPtr<IDXGIAdapter4> a4;
        if (SUCCEEDED(a.As(&a4))) {
            DXGI_ADAPTER_DESC3 d3 = {};
            if (SUCCEEDED(a4->GetDesc3(&d3))) flags3 = (UINT)d3.Flags;
        }
        Log("ADAPTER index=%u luid=%lx:%08lx vendor=0x%04x device=0x%04x flags=0x%x monitored_fences=%u "
            "non_monitored_fences=%u keyed_mutex_conformance=%u vram_mib=%llu desc=\"%s\"",
            i, (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, d.VendorId, d.DeviceId, d.Flags,
            (flags3 & DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES) ? 1u : 0u,
            (flags3 & DXGI_ADAPTER_FLAG3_SUPPORT_NON_MONITORED_FENCES) ? 1u : 0u,
            (flags3 & DXGI_ADAPTER_FLAG3_KEYED_MUTEX_CONFORMANCE) ? 1u : 0u,
            (unsigned long long)(d.DedicatedVideoMemory >> 20), Narrow(d.Description).c_str());
        const bool hardware = !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414;
        if (chosen < 0 && !g_opt.haveLuid && ((wanted < 0 && hardware) || (int)i == wanted)) {
            chosen = (int)i;
            adapter = a;
            luid = d.AdapterLuid;
        }
    }
    if (g_opt.haveLuid) {
        hr = factory->EnumAdapterByLuid(g_opt.luid, IID_PPV_ARGS(&adapter));
        if (FAILED(hr)) {
            error = "EnumAdapterByLuid hr=" + HrText(hr);
            return false;
        }
        luid = g_opt.luid;
        Log("ADAPTER chosen by luid %lx:%08lx", (unsigned long)luid.HighPart, luid.LowPart);
        return true;
    }
    if (chosen < 0) {
        error = "no adapter matches --adapter " + Narrow(g_opt.adapter);
        return false;
    }
    Log("ADAPTER chosen index=%d", chosen);
    return true;
}

static std::string Lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string RouteTag(std::string *umds)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return "?";
    MODULEENTRY32W m = {sizeof(m)};
    std::vector<std::string> tags;
    std::string list;
    auto tag = [&tags](const char *t) {
        if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
    };
    static const char *umdPrefixes[] = {"amdgpu_wddm", "bc250", "nvwgf2um", "nvldumd", "d3d10warp", "d3d11on12", "vulkan-1"};
    for (BOOL ok = Module32FirstW(snapshot, &m); ok; ok = Module32NextW(snapshot, &m)) {
        const std::string name = Lower(Narrow(m.szModule));
        if (name == "amdgpu_wddm_d3d11.dll") tag("gpu11");
        if (name == "bc250d3d.dll") tag("cpu11");
        if (name == "amdgpu_wddm_d3d12.dll") tag("d3d12");
        if (name == "nvwgf2umx.dll" || name == "nvldumdx.dll") tag("nvidia");
        if (name == "d3d10warp.dll") tag("warp");
        if (name == "d3d11on12.dll") tag("11on12");
        for (const char *p : umdPrefixes)
            if (name.compare(0, strlen(p), p) == 0) {
                list += (list.empty() ? "" : "+") + name;
                break;
            }
    }
    CloseHandle(snapshot);
    if (umds) *umds = list.empty() ? "-" : list;
    std::string t;
    for (const std::string &s : tags) t += (t.empty() ? "" : "+") + s;
    return t.empty() ? "none" : t;
}

void LogModules(const char *when)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log("MODULES when=%s error=%lu", when, GetLastError());
        return;
    }
    MODULEENTRY32W m = {sizeof(m)};
    unsigned total = 0, listed = 0;
    static const char *keys[] = {"amdgpu", "bc250", "d3d1", "dxgi", "d3d10warp", "vulkan", "dcomp", "graphicscapture",
                                 "nvwgf", "nvldumd", "nvd3dum", "dwmapi", "windows.graphics"};
    for (BOOL ok = Module32FirstW(snapshot, &m); ok; ok = Module32NextW(snapshot, &m)) {
        ++total;
        const std::string path = Narrow(m.szExePath), lower = Lower(Narrow(m.szModule));
        for (const char *k : keys)
            if (lower.find(k) != std::string::npos) {
                Log("MODULE when=%s %s", when, path.c_str());
                ++listed;
                break;
            }
    }
    CloseHandle(snapshot);
    std::string umds;
    const std::string route = RouteTag(&umds);
    Log("MODULES when=%s total=%u graphics=%u route=%s umds=%s", when, total, listed, route.c_str(), umds.c_str());
}

void LogEnvironment()
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = {sizeof(v)};
    auto rtlGetVersion = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtlGetVersion) rtlGetVersion(&v);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const size_t slash = g_opt.self.find_last_of(L"\\/");
    Log("ENV role=%s pid=%lu exe=%s os=%lu.%lu.%lu session=%lu cell=%s size=%ux%u format=%s bound_s=%u relaunched=%u",
        g_opt.peer ? "peer" : "parent", GetCurrentProcessId(),
        Narrow(slash == std::wstring::npos ? g_opt.self : g_opt.self.substr(slash + 1)).c_str(), v.dwMajorVersion,
        v.dwMinorVersion, v.dwBuildNumber, session, g_opt.cell.c_str(), g_opt.w, g_opt.h, FormatText(g_opt.format).c_str(),
        g_opt.boundS, g_opt.relaunched ? 1u : 0u);
    Log("ENV cmdline=%s", Narrow(GetCommandLineW()).c_str());
    wchar_t *block = GetEnvironmentStringsW();
    static const wchar_t *prefixes[] = {L"AMDGPU_", L"BC250_", L"DXVK_", L"VKD3D", L"RADV_", L"MESA_", L"ZINK", L"VK_",
                                        L"GALLIUM_", L"LP_"};
    for (const wchar_t *e = block; e && *e; e += wcslen(e) + 1)
        for (const wchar_t *p : prefixes)
            if (_wcsnicmp(e, p, wcslen(p)) == 0) {
                Log("ENV var %s", Narrow(e).c_str());
                break;
            }
    if (block) FreeEnvironmentStringsW(block);
}

// ------------------------------------------------------------------------------------------------ DBWIN listener
// The OutputDebugString protocol without a debugger (as DebugView and dcompwit.exe): the writer opens DBWIN_BUFFER
// (4 KiB: DWORD pid, then the text), waits for DBWIN_BUFFER_READY, writes, sets DBWIN_DATA_READY. The objects get a
// null DACL so that writers of every account in this session (DWM's included) may open them; a writer waits at most
// 10 s for BUFFER_READY, and the loop never holds it longer than one copy. Only lines of the listed image names are
// recorded (DWM's hosted UMD reports there); this process's and the peer's lines come through their own capture.
struct Dbwin {
    HANDLE bufferReady = nullptr, dataReady = nullptr, section = nullptr, thread = nullptr;
    const unsigned char *view = nullptr;
    std::vector<std::string> also;
    std::map<DWORD, std::string> names;
    std::map<std::string, unsigned> counts;
    unsigned recorded = 0, others = 0;
    std::atomic<bool> stop{false};
};
static Dbwin g_dbwin;

static std::string ImageName(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "";
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(p, 0, path, &size)) {
        const wchar_t *base = wcsrchr(path, L'\\');
        name = Lower(Narrow(base ? base + 1 : path));
    }
    CloseHandle(p);
    return name;
}

static DWORD WINAPI DbwinThread(LPVOID)
{
    while (!g_dbwin.stop.load()) {
        if (WaitForSingleObject(g_dbwin.dataReady, 100) != WAIT_OBJECT_0) continue;
        DWORD pid = 0;
        memcpy(&pid, g_dbwin.view, sizeof(pid));
        char text[1100];
        CopyDebugText(text, sizeof(text), g_dbwin.view + sizeof(DWORD), false, 4096 - sizeof(DWORD));
        SetEvent(g_dbwin.bufferReady);
        auto it = g_dbwin.names.find(pid);
        if (it == g_dbwin.names.end()) it = g_dbwin.names.emplace(pid, ImageName(pid)).first;
        bool wanted = false;
        for (const std::string &a : g_dbwin.also)
            if (it->second == a) wanted = true;
        if (!wanted) {
            ++g_dbwin.others;
            continue;
        }
        ++g_dbwin.counts[it->second];
        if (g_dbwin.recorded < 20000) {
            ++g_dbwin.recorded;
            Log("DBWIN pid=%lu src=%s %s", pid, it->second.c_str(), text);
        }
    }
    return 0;
}

bool DbwinStart(const std::vector<std::string> &also)
{
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), &sd, FALSE};
    g_dbwin.bufferReady = CreateEventW(&sa, FALSE, FALSE, L"DBWIN_BUFFER_READY");
    const DWORD e1 = GetLastError();
    g_dbwin.dataReady = CreateEventW(&sa, FALSE, FALSE, L"DBWIN_DATA_READY");
    const DWORD e2 = GetLastError();
    g_dbwin.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
    const DWORD e3 = GetLastError();
    if (!g_dbwin.bufferReady || !g_dbwin.dataReady || !g_dbwin.section || e1 == ERROR_ALREADY_EXISTS ||
        e2 == ERROR_ALREADY_EXISTS || e3 == ERROR_ALREADY_EXISTS) {
        Log("DBWIN listener-unavailable errors=%lu,%lu,%lu (another listener owns DBWIN_BUFFER?)", e1, e2, e3);
        return false;
    }
    g_dbwin.view = (const unsigned char *)MapViewOfFile(g_dbwin.section, FILE_MAP_READ, 0, 0, 4096);
    if (!g_dbwin.view) {
        Log("DBWIN map-failed error=%lu", GetLastError());
        return false;
    }
    for (const std::string &a : also) g_dbwin.also.push_back(Lower(a));
    SetEvent(g_dbwin.bufferReady);
    g_dbwin.thread = CreateThread(nullptr, 0, DbwinThread, nullptr, 0, nullptr);
    Log("DBWIN listening names=%zu", also.size());
    return g_dbwin.thread != nullptr;
}

void DbwinStop()
{
    if (!g_dbwin.thread) return;
    g_dbwin.stop = true;
    WaitForSingleObject(g_dbwin.thread, 3000);
    CloseHandle(g_dbwin.thread);
    g_dbwin.thread = nullptr;
    std::string sources;
    for (const auto &e : g_dbwin.counts) sources += (sources.empty() ? "" : ",") + e.first + ":" + std::to_string(e.second);
    Log("DBWIN summary recorded=%u sources=%s others_counted=%u", g_dbwin.recorded, sources.empty() ? "-" : sources.c_str(),
        g_dbwin.others);
    VerdictNote("dbwin_recorded", std::to_string(g_dbwin.recorded));
}

// ------------------------------------------------------------------------------------------------ cell table
static const CellInfo g_cells[] = {
    {"km11", Kind::Keyed, Api::D3D11, Api::D3D11, true,
     "D3D11 shared texture -> D3D11 OpenSharedResource1 in the peer, keyed mutex handshake (--handle kmt: legacy handle)"},
    {"km12to11", Kind::Keyed, Api::D3D12, Api::D3D11, true,
     "D3D12 keyed-mutex texture (ID3D12CompatibilityDevice, mutex via D3D11On12) -> D3D11 OpenSharedResource1"},
    {"km11to12", Kind::Keyed, Api::D3D11, Api::D3D12, true,
     "D3D11 keyed-mutex texture -> D3D12 OpenSharedHandle, mutex via D3D11On12"},
    {"s11to11", Kind::Shared, Api::D3D11, Api::D3D11, true, "D3D11 -> D3D11 shared texture, CPU or shared-fence sync"},
    {"s12to11", Kind::Shared, Api::D3D12, Api::D3D11, true,
     "D3D12 CreateSharedHandle -> D3D11 OpenSharedResource1, CPU or shared-fence sync"},
    {"s11to12", Kind::Shared, Api::D3D11, Api::D3D12, true,
     "D3D11 CreateSharedHandle -> D3D12 OpenSharedHandle, CPU or shared-fence sync"},
    {"s12to12", Kind::Shared, Api::D3D12, Api::D3D12, true, "D3D12 -> D3D12 across processes, CPU or shared-fence sync"},
    {"f11to11", Kind::Fence, Api::D3D11, Api::D3D11, true, "D3D11 shared fences -> D3D11 OpenSharedFence, GPU waits both ways"},
    {"f12to11", Kind::Fence, Api::D3D12, Api::D3D11, true,
     "D3D12 shared fences -> D3D11 ID3D11Device5::OpenSharedFence, GPU waits both ways"},
    {"f11to12", Kind::Fence, Api::D3D11, Api::D3D12, true, "D3D11 shared fences -> D3D12 OpenSharedHandle, GPU waits both ways"},
    {"f12to12", Kind::Fence, Api::D3D12, Api::D3D12, true, "D3D12 shared fences -> D3D12 across processes, GPU waits both ways"},
    {"w11", Kind::LocalWait, Api::D3D11, Api::D3D11, false, "control: local D3D11 fence signal and wait on one context"},
    {"w12", Kind::LocalWait, Api::D3D12, Api::D3D12, false,
     "control: local D3D12 queue waits (settled, CPU-signalled pending, cross-queue)"},
    {"ipc", Kind::Ipc, Api::D3D11, Api::D3D11, true, "harness only: peer process, pipe, handle duplication, shared log"},
    {"capdry", Kind::CapDry, Api::D3D11, Api::D3D11, false,
     "headless check of the capture region oracle on a synthetic desktop texture"},
    {"dda", Kind::Dda, Api::D3D11, Api::D3D12, true,
     "Desktop Duplication (D3D11 consumer) of a producer window (D3D12 swap chain, --producer gdi for GDI)"},
    {"wgc", Kind::Wgc, Api::D3D11, Api::D3D12, true,
     "Windows.Graphics.Capture (D3D11 frame pool) of a producer window (D3D12 swap chain, --producer gdi for GDI)"},
};

const CellInfo *FindCell(const std::string &name)
{
    for (const CellInfo &c : g_cells)
        if (name == c.name) return &c;
    return nullptr;
}

std::string CellList()
{
    std::string s;
    for (const CellInfo &c : g_cells) s += std::string("  ") + c.name + std::string(10 - strlen(c.name), ' ') + c.what + "\n";
    return s;
}
