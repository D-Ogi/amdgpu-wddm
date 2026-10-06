// SPDX-License-Identifier: MIT
// capshare.exe - M15.13 witness: desktop/window capture and cross-process, cross-API shared resources on the route
// that serves the process (router -> CPU or GPU D3D11 UMD, native D3D12 UMD), with exact oracles.
//
//   capshare.exe --cell <cell> [--bound <s>] [--out stdout.txt] [--json result.json] [--stderr stderr.txt] [options]
//   capshare.exe --self-test [--out ...] [--json ...]
//
// Cells (one per run; --help lists them):
//   km11 km12to11 km11to12     keyed mutex handshake on a shared texture across two processes
//   s11to11 s12to11 s11to12 s12to12   shared texture, CPU sync (default) or --sync fence (the f-cells' two fences)
//   f11to11 f12to11 f11to12 f12to12   two shared fences only (one signaller each), GPU waits in both directions
//   w11 w12                    controls without sharing: local fence waits
//   ipc                        harness only (no device)
//   capdry                     headless check of the capture-region oracle on a synthetic desktop texture
//   dda wgc                    capture of a producer window; need --interactive-ok (a window appears, the screen is read)
// The parent process is the creator (shared cells) or the consumer (capture cells); the peer process (the same
// executable, or --peer-exe, e.g. a renamed copy so that the router allowlist can send the two processes to
// different UMDs) is the opener or the producer. Both write into the --out file (lines "A ..." and "B ...").
//
// Output: the --out text file, one record per line, ending with exactly one line
//   VERDICT cell=.. result=pass|mismatch|fail|timeout side=A|B|- stage=.. call=.. hr=.. at=x,y got=rgba:.. want=rgba:..
//           diff=n/total max_delta=.. content=.. gate=held|violated:..|- route=A:..,B:.. fl=A:..,B:.. checks=..
//           elapsed_ms=.. note=.. [extra key=value ...]
// and the same fields in --json (result.json, which app-run.ps1 reads). Exit code 0 pass, 1 mismatch, 2 fail,
// 3 timeout, 4 usage. Every run ends within --bound seconds (at most 170): a watchdog reports the stage it hung in.
// --stderr relaunches the program with that file as its standard output and error from the start, so the C runtime
// of every DLL (the UMDs' "engine-ddi: ..." lines, Mesa and RADV messages) writes there, the peer included.
#include "capshare.h"
#include <shellapi.h>
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static const char *Usage =
    "capshare --cell <cell> [options] | --self-test\n"
    "  --bound <s>            whole run bound, 1..170 (default 30)\n"
    "  --out <file>           text log, both processes (default capshare.txt)\n"
    "  --json <file>          verdict as JSON (default result.json)\n"
    "  --stderr <file>        relaunch with stdout/stderr redirected to <file> (UMD diagnostics)\n"
    "  --size <W>x<H>         texture/window size (default 256x256)\n"
    "  --format <name>        bgra8 (default), rgba8, bgra8-srgb, rgba8-srgb\n"
    "  --adapter auto|<index> (default: the first hardware adapter)\n"
    "  --peer-exe <path>      executable of the peer (default: this one; relative to this one's folder)\n"
    "  --sync cpu|fence       s-cells: CPU waits (default) or a shared fence\n"
    "  --handle nt|kmt        D3D11-only cells: NT handle (default) or legacy global handle\n"
    "  --delay-copies <n>     large copies ahead of every pattern write (default 128)\n"
    "  --delay-size <n>       their square size (default 2048)\n"
    "  --gate-ms <ms>         how long a gate is held before it is checked (default 300)\n"
    "  --simultaneous         D3D12 shared textures with ALLOW_SIMULTANEOUS_ACCESS\n"
    "  --inject skip-wait     negative control (f-cells, s-cells with --sync fence): the opener leaves out its first\n"
    "                         GPU wait, so the run must end in mismatch with gate=violated\n"
    "  --producer d3d12|gdi   capture cells: how the producer draws (default d3d12)\n"
    "  --x <px> --y <px>      producer position (default: a corner away from the cursor)\n"
    "  --tolerance <n>        capture cells: allowed per-channel delta (default 0)\n"
    "  --interactive-ok       required by dda and wgc: they show a window and read the screen\n"
    "  --dbwin <a.exe,...>    also record OutputDebugString lines of these processes (e.g. dwm.exe)\n"
    "  --env NAME=VALUE       set before any device is created; the peer inherits it (repeatable)\n";

static bool ParseU(const wchar_t *s, unsigned lo, unsigned hi, unsigned &out)
{
    wchar_t *end = nullptr;
    const unsigned long v = wcstoul(s, &end, 10);
    if (!end || *end || v < lo || v > hi) return false;
    out = (unsigned)v;
    return true;
}

static std::wstring FullPath(const std::wstring &p)
{
    wchar_t buf[2048];
    const DWORD n = GetFullPathNameW(p.c_str(), 2048, buf, nullptr);
    return n && n < 2048 ? std::wstring(buf) : p;
}

static bool Parse(int argc, wchar_t **argv, std::string &error, std::vector<std::wstring> &envs)
{
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](const wchar_t *&v) -> bool {
            if (i + 1 >= argc) {
                error = "missing value after " + Narrow(a);
                return false;
            }
            v = argv[++i];
            return true;
        };
        const wchar_t *v = nullptr;
        unsigned u = 0;
        if (a == L"--help" || a == L"-h") {
            error = "help";
            return false;
        } else if (a == L"--self-test") {
            g_opt.selfTest = true;
        } else if (a == L"--interactive-ok") {
            g_opt.interactiveOk = true;
        } else if (a == L"--simultaneous") {
            g_opt.simultaneous = true;
        } else if (a == L"--inject") {
            if (!next(v)) return false;
            if (wcscmp(v, L"skip-wait")) return error = "--inject must be skip-wait", false;
            g_opt.skipWait = true;
        } else if (a == L"--relaunched") {
            g_opt.relaunched = true;
        } else if (!next(v)) {
            return false;
        } else if (a == L"--cell") {
            g_opt.cell = Narrow(v);
        } else if (a == L"--bound") {
            if (!ParseU(v, 1, 170, g_opt.boundS)) return error = "--bound must be 1..170", false;
        } else if (a == L"--out") {
            g_opt.out = v;
        } else if (a == L"--json") {
            g_opt.json = v;
        } else if (a == L"--stderr") {
            g_opt.stderrPath = v;
        } else if (a == L"--size") {
            unsigned w = 0, h = 0;
            if (swscanf_s(v, L"%ux%u", &w, &h) != 2 || w < 16 || h < 16 || w > 4096 || h > 4096)
                return error = "--size must be <W>x<H>, 16..4096", false;
            g_opt.w = w;
            g_opt.h = h;
        } else if (a == L"--format") {
            // The sRGB views share the storage row of their format on the shared-surface wire, so a shared
            // cell may ask for one. Every image here is compared as raw storage bytes, which an sRGB view does
            // not change: both sides write and read the same bytes through the same format.
            if (!wcscmp(v, L"bgra8")) g_opt.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            else if (!wcscmp(v, L"rgba8")) g_opt.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (!wcscmp(v, L"bgra8-srgb")) g_opt.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            else if (!wcscmp(v, L"rgba8-srgb")) g_opt.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            else return error = "--format must be bgra8, rgba8, bgra8-srgb or rgba8-srgb", false;
        } else if (a == L"--adapter") {
            g_opt.adapter = v;
        } else if (a == L"--peer-exe") {
            g_opt.peerExe = v;
        } else if (a == L"--sync") {
            if (!wcscmp(v, L"cpu")) g_opt.syncFence = false;
            else if (!wcscmp(v, L"fence")) g_opt.syncFence = true;
            else return error = "--sync must be cpu or fence", false;
        } else if (a == L"--handle") {
            if (!wcscmp(v, L"nt")) g_opt.kmt = false;
            else if (!wcscmp(v, L"kmt")) g_opt.kmt = true;
            else return error = "--handle must be nt or kmt", false;
        } else if (a == L"--delay-copies") {
            if (!ParseU(v, 0, 4096, g_opt.delayCopies)) return error = "--delay-copies must be 0..4096", false;
        } else if (a == L"--delay-size") {
            if (!ParseU(v, 64, 8192, g_opt.delaySize)) return error = "--delay-size must be 64..8192", false;
        } else if (a == L"--gate-ms") {
            if (!ParseU(v, 0, 10000, g_opt.gateMs)) return error = "--gate-ms must be 0..10000", false;
        } else if (a == L"--producer") {
            if (!wcscmp(v, L"d3d12")) g_opt.producerGdi = false;
            else if (!wcscmp(v, L"gdi")) g_opt.producerGdi = true;
            else return error = "--producer must be d3d12 or gdi", false;
        } else if (a == L"--x") {
            g_opt.x = _wtoi(v);
        } else if (a == L"--y") {
            g_opt.y = _wtoi(v);
        } else if (a == L"--tolerance") {
            if (!ParseU(v, 0, 255, u)) return error = "--tolerance must be 0..255", false;
            g_opt.tolerance = (int)u;
        } else if (a == L"--dbwin") {
            std::string list = Narrow(v), item;
            for (char c : list + ",")
                if (c == ',') {
                    if (!item.empty()) g_opt.dbwin.push_back(item);
                    item.clear();
                } else {
                    item += c;
                }
        } else if (a == L"--env") {
            if (!wcschr(v, L'=')) return error = "--env needs NAME=VALUE", false;
            envs.push_back(v);
        } else if (a == L"--role") {
            if (wcscmp(v, L"peer")) return error = "--role must be peer", false;
            g_opt.peer = true;
        } else if (a == L"--ipc") {
            unsigned long long r = 0, w = 0;
            if (swscanf_s(v, L"%llx,%llx", &r, &w) != 2) return error = "--ipc r,w", false;
            g_opt.ipcIn = (HANDLE)(ULONG_PTR)r;
            g_opt.ipcOut = (HANDLE)(ULONG_PTR)w;
        } else if (a == L"--luid") {
            unsigned long hi = 0, lo = 0;
            if (swscanf_s(v, L"%lx:%lx", &hi, &lo) != 2) return error = "--luid hi:lo", false;
            g_opt.luid.HighPart = (LONG)hi;
            g_opt.luid.LowPart = lo;
            g_opt.haveLuid = true;
        } else if (a == L"--t0") {
            g_opt.t0 = _wcstoui64(v, nullptr, 10);
        } else if (a == L"--deadline") {
            g_opt.deadline = _wcstoui64(v, nullptr, 10);
        } else {
            error = "unknown option " + Narrow(a);
            return false;
        }
    }
    if (g_opt.selfTest) {
        g_opt.cell = "self-test";
        return true;
    }
    const CellInfo *cell = FindCell(g_opt.cell);
    if (!cell) return error = "unknown or missing --cell '" + g_opt.cell + "'", false;
    if ((g_opt.x == INT_MIN) != (g_opt.y == INT_MIN)) return error = "--x and --y go together", false;
    if (g_opt.kmt && (cell->creator != Api::D3D11 || cell->opener != Api::D3D11 ||
                      (cell->kind != Kind::Keyed && cell->kind != Kind::Shared)))
        return error = "--handle kmt applies to km11 and s11to11 only (D3D12 opens NT handles only)", false;
    if (g_opt.skipWait && cell->kind != Kind::Fence && !(cell->kind == Kind::Shared && g_opt.syncFence))
        return error = "--inject skip-wait applies to the f-cells and to the s-cells with --sync fence", false;
    if ((cell->kind == Kind::Dda || cell->kind == Kind::Wgc) && !g_opt.interactiveOk)
        return error = "cell " + g_opt.cell + " shows a window and reads the screen: run it only in the lab's interactive "
                                              "session, with --interactive-ok",
               false;
    return true;
}

// --stderr: run this program again with the file as standard output and error from process creation, so that every C
// runtime in the process (static or shared, the UMDs' included) writes its stderr there; wait for it and pass on its
// exit code. A child that outlives the bound is ended and reported here.
static int Launch()
{
    {
        HANDLE t = CreateFileW(g_opt.stderrPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE err = CreateFileW(g_opt.stderrPath.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (err == INVALID_HANDLE_VALUE) return -1;
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = err;
    si.StartupInfo.hStdError = err;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attr(size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &err, sizeof(HANDLE), nullptr,
                              nullptr);
    std::wstring cmd = std::wstring(GetCommandLineW()) + L" --relaunched --t0 " + std::to_wstring(g_opt.t0);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    PROCESS_INFORMATION pi = {};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    const BOOL ok = CreateProcessW(g_opt.self.c_str(), line.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(err);
    if (!ok) return -1;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    const DWORD waited = WaitForSingleObject(pi.hProcess, Remaining(g_opt.t0 + g_opt.boundS * 1000ull + 5000));
    DWORD code = 3;
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        if (job) TerminateJobObject(job, 3);
        else TerminateProcess(pi.hProcess, 3);
        LogOpen(g_opt.out, false, false);
        VerdictInit(g_opt.cell);
        VerdictTimeout('A', "launcher", "the relaunched process outlived the bound and was ended");
        code = (DWORD)VerdictEmit();
    }
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void ParentTimeout()
{
    VerdictTimeout('A', Stage());
    KillPeer();
    VerdictEmit();
}

static void PeerTimeout()
{
    g_ipc.Send("FAIL kind=timeout stage=%s note=peer-bound-reached", Stage());
    Sleep(100);
}

static int Run()
{
    const CellInfo *cell = FindCell(g_opt.cell);
    if (g_opt.peer) {
        switch (cell->kind) {
        case Kind::Ipc: return RunIpcPeer();
        case Kind::Dda:
        case Kind::Wgc: return RunProducerPeer(*cell);
        default: return RunTwoProcessPeer(*cell);
        }
    }
    VerdictInit(g_opt.cell);
    if (g_opt.skipWait) VerdictNote("inject", "skip-wait");
    if (g_opt.selfTest) {
        RunSelfTest();
    } else {
        switch (cell->kind) {
        case Kind::Keyed:
        case Kind::Shared:
        case Kind::Fence: RunTwoProcessParent(*cell); break;
        case Kind::LocalWait: RunLocalWait(*cell); break;
        case Kind::Ipc: RunIpcParent(); break;
        case Kind::CapDry: RunCapDry(); break;
        case Kind::Dda:
        case Kind::Wgc: RunCaptureParent(*cell); break;
        }
    }
    VerdictCellEnded();
    if (g_peerProcess.process) {
        SetStage("end-peer");
        g_ipc.Send("DONE");
        const DWORD code = PeerExit(3000);
        Log("PEER exit=%s", HrText((HRESULT)code).c_str());
        if (code == STILL_ACTIVE) KillPeer();
        VerdictNote("peer_exit", code == STILL_ACTIVE ? "killed" : HrText((HRESULT)code));
    }
    DbwinStop();
    return VerdictEmit();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t self[4096] = {};
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    g_opt.self = self;
    std::string error;
    std::vector<std::wstring> envs;
    const bool parsed = argv && Parse(argc, argv, error, envs);
    if (!g_opt.t0) g_opt.t0 = GetTickCount64();
    if (!g_opt.deadline) g_opt.deadline = g_opt.t0 + g_opt.boundS * 1000ull - 500;
    g_opt.out = FullPath(g_opt.out);
    g_opt.json = FullPath(g_opt.json);
    if (!g_opt.stderrPath.empty()) g_opt.stderrPath = FullPath(g_opt.stderrPath);
    if (!parsed) {
        // No console on this subsystem: the message goes to the log, result.json and stderr (if any).
        LogOpen(g_opt.out, true, true);
        if (error == "help") {
            Log("%s\ncells:\n%s", Usage, CellList().c_str());
            TerminateProcess(GetCurrentProcess(), 4);
        }
        Log("USAGE %s\n%s", error.c_str(), Usage);
        VerdictInit(g_opt.cell.empty() ? "-" : g_opt.cell);
        VerdictFail('A', "usage", error, E_INVALIDARG);
        VerdictEmit();
        TerminateProcess(GetCurrentProcess(), 4);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!g_opt.stderrPath.empty() && !g_opt.relaunched && !g_opt.peer) TerminateProcess(GetCurrentProcess(), (UINT)Launch());
    for (const std::wstring &e : envs) {
        const size_t eq = e.find(L'=');
        SetEnvironmentVariableW(e.substr(0, eq).c_str(), e.substr(eq + 1).c_str());
    }
    // The peer's lines are in the shared --out file already; its standard output, when it has one, is the parent's
    // --stderr file, which keeps only what the C runtimes write.
    LogOpen(g_opt.out, !g_opt.peer, !g_opt.relaunched && !g_opt.peer);
    InstallDebugStringCapture();
    if (g_opt.peer && !g_ipc.Start()) TerminateProcess(GetCurrentProcess(), 2);
    LogEnvironment();
    if (!g_opt.peer && !g_opt.dbwin.empty()) DbwinStart(g_opt.dbwin);
    StartWatchdog(g_opt.peer ? PeerTimeout : ParentTimeout);
    const int code = Run();
    FlushOds();
    Log("EXIT code=%d", code);
    // No DLL teardown: a UMD that hangs in its detach must not turn a finished verdict into a timeout.
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    return code;
}
