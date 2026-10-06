// SPDX-License-Identifier: MIT
// cells.cpp - the protocols of the shared-resource, keyed-mutex and fence cells (parent = creator, peer = opener),
// the local wait controls, the harness cell, the headless capture-oracle check and the self-test.
//
// Every two-process cell starts the same way: the parent creates its device, starts the peer (same executable, or
// --peer-exe so the router can send it to another UMD), the peer creates its device on the parent's adapter (LUID)
// and answers HELLO with its route. The creator's shared texture holds a poison image first; the opener must read
// that poison (P0: the open aliases the creator's memory), then pattern A written by the creator (A), and the creator
// must read pattern B written by the opener (B). Writes follow a delay workload of large copies, so a reader that does
// not really wait for the writer's GPU work sees poison or the previous pattern instead of the new one.
#include "capshare.h"
#include <memory>

// ------------------------------------------------------------------------------------------------ helpers
static bool FailA(Side &s)
{
    VerdictFail('A', Stage(), s.lastCall, s.lastHr);
    VerdictRemoved('A', s.lastRemoved);
    return false;
}

// A failure on the parent's side that may only be the echo of the peer's failure: the peer's report wins.
static bool FailAPreferPeer(Side &s)
{
    std::string m;
    if (g_ipc.Has("FAIL") && !g_ipc.Expect("NO-SUCH-VERB", m, GetTickCount64() + 200)) {
        VerdictFromPeer(m, Stage());
        VerdictRemoved('A', s.lastRemoved);
        return false;
    }
    return FailA(s);
}

// The parent's own failure in a shared-fence wait, with its view of that fence. Its observation is the closer one, so
// it stays the verdict; a failure the peer reported meanwhile goes into a note.
static bool FailAFence(Side &s, int i)
{
    VerdictFail('A', Stage(), s.lastCall, s.lastHr, "f" + std::to_string(i) + "=" + std::to_string(s.Completed(i)));
    VerdictRemoved('A', s.lastRemoved);
    std::string m;
    if (g_ipc.Has("FAIL") && !g_ipc.Expect("NO-SUCH-VERB", m, GetTickCount64() + 200)) {
        VerdictNote("peer_fail", Field(m, "stage") + "/" + Field(m, "call") + "/" + Field(m, "hr"));
        VerdictRemoved('B', (HRESULT)strtoul(Field(m, "removed").c_str(), nullptr, 16));
    }
    return false;
}

int PeerFailText(const char *stage, const char *call, HRESULT hr, const std::string &note, HRESULT removed)
{
    Log("PEER-FAIL stage=%s call=%s hr=%s note=%s removed=%s", stage, call, HrText(hr).c_str(), note.c_str(),
        HrText(removed).c_str());
    FlushOds();
    g_ipc.Send("FAIL kind=fail stage=%s call=%s hr=%s note=%s removed=%s", stage, call, HrText(hr).c_str(), note.c_str(),
               HrText(removed).c_str());
    return 2;
}

static int PeerFail(Side &s) { return PeerFailText(Stage(), s.lastCall, s.lastHr, "-", s.lastRemoved); }

static int PeerFailFence(Side &s, int i)
{
    return PeerFailText(Stage(), s.lastCall, s.lastHr, "f" + std::to_string(i) + "=" + std::to_string(s.Completed(i)),
                        s.lastRemoved);
}

// The peer's GPU waits end before its watchdog, so that a stuck wait is reported as that wait.
static ULONGLONG PeerOpDeadline() { return g_opt.deadline > 300 ? g_opt.deadline - 300 : g_opt.deadline; }

bool PeerExpect(const char *verb, std::string &msg)
{
    if (g_ipc.Expect(verb, msg, g_opt.deadline > 200 ? g_opt.deadline - 200 : g_opt.deadline)) return true;
    Log("PEER lost the parent while waiting for %s: %s", verb, msg.c_str());
    return false;
}

static HANDLE ParseHandle(const std::string &m, const char *key)
{
    return (HANDLE)(ULONG_PTR)_strtoui64(Field(m, key).c_str(), nullptr, 16);
}

static bool SendHandles(HANDLE resource, HANDLE fence0, HANDLE fence1)
{
    HANDLE r = nullptr, f[2] = {fence0, fence1};
    if (resource) {
        r = g_opt.kmt ? resource : DupToPeer(resource); // a legacy (KMT) handle is a global value, not duplicated
        if (!r) {
            VerdictFail('A', "dup-handle", "DuplicateHandle(resource)", E_FAIL, "see-the-log");
            return false;
        }
    }
    for (HANDLE &h : f) {
        if (!h) continue;
        h = DupToPeer(h);
        if (!h) {
            VerdictFail('A', "dup-handle", "DuplicateHandle(fence)", E_FAIL, "see-the-log");
            return false;
        }
    }
    return g_ipc.Send("HANDLES res=%llx fence0=%llx fence1=%llx", (unsigned long long)(ULONG_PTR)r,
                      (unsigned long long)(ULONG_PTR)f[0], (unsigned long long)(ULONG_PTR)f[1]);
}

static bool ReadCheck(Side &s, const char *what, Pattern p, Check &c)
{
    Image img;
    if (!s.ReadSubmit() || !s.ReadCollect(img)) return false;
    c = CompareImage(what, img, p, false, false, 0);
    return true;
}

// ------------------------------------------------------------------------------------------------ keyed mutex
// Keys: creator 0 -> (poison) release 1 -> opener reads P0, release 2 -> creator acquires 2; the opener announces
// WAITING and blocks in AcquireSync(3); the creator holds the key for --gate-ms (gate: no ACQUIRED may arrive), writes
// A behind the delay workload, releases 3 -> opener reads A, writes B, releases 4 -> creator reads B.
static bool KeyedParent(Side &s)
{
    std::string m;
    HANDLE h = nullptr;
    Check c;
    SetStage("create-shared");
    if (!s.CreateShared(true, &h)) return FailA(s);
    SetStage("acquire-0");
    if (!s.Acquire(0)) return FailA(s);
    SetStage("write-poison");
    if (!s.WritePattern(Pattern::Poison, false) || !s.Finish()) return FailA(s);
    SetStage("release-1");
    if (!s.Release(1)) return FailA(s);
    if (!SendHandles(h, nullptr, nullptr)) return false;
    if (!PeerStep("OPENED", m, "peer-open")) return false;
    if (!PeerStep("P0", m, "peer-acquire-1-read-poison")) return false;
    VerdictCheck('B', CheckFromText(m));
    SetStage("acquire-2");
    if (!s.Acquire(2)) return FailAPreferPeer(s);
    if (!PeerStep("WAITING", m, "peer-before-acquire-3")) return false;
    SetStage("gate-key-3");
    Sleep(g_opt.gateMs);
    const bool early = g_ipc.Has("ACQUIRED");
    VerdictGate("key-3", !early, early ? "the peer acquired key 3 before the creator released it" : "");
    SetStage("write-a");
    if (!s.WritePattern(Pattern::A, true)) return FailA(s);
    // --creator-finish, the BD-075 round 2 discriminator for km12to11. By the keyed mutex's contract the release
    // belongs behind this write: the creator submitted it on the same queue the mutex's release is signalled on.
    // The lab reads poison here while s12to11 - the same memory, the same two processes, a CPU wait after the
    // write - passes. With this wait the handover cannot race the submission, so a pass says the memory path is
    // sound and the ordering of the runtime's own release against our ExecuteCommandLists is the defect, and a
    // poison read says the write itself is not reaching the reader. It is a measurement, never a fix.
    if (g_opt.creatorFinish && !s.Finish()) return FailA(s);
    SetStage("release-3");
    if (!s.Release(3)) return FailA(s);
    if (!PeerStep("ACQUIRED", m, "peer-acquire-3")) return false;
    if (!PeerStep("CHECKA", m, "peer-read-a-write-b")) return false;
    VerdictCheck('B', CheckFromText(m));
    SetStage("acquire-4");
    if (!s.Acquire(4)) return FailAPreferPeer(s);
    SetStage("read-b");
    if (!ReadCheck(s, "B", Pattern::B, c)) return FailA(s);
    VerdictCheck('A', c);
    SetStage("release-0");
    if (!s.Release(0)) return FailA(s);
    return true;
}

static int KeyedPeer(Side &s)
{
    std::string m;
    Check c0, ca;
    if (!PeerExpect("HANDLES", m)) return 2;
    SetStage("open-shared");
    if (!s.OpenShared(ParseHandle(m, "res"), true)) return PeerFail(s);
    g_ipc.Send("OPENED");
    SetStage("acquire-1");
    if (!s.Acquire(1)) return PeerFail(s);
    SetStage("read-poison");
    if (!ReadCheck(s, "P0", Pattern::Poison, c0)) return PeerFail(s);
    SetStage("release-2");
    if (!s.Release(2)) return PeerFail(s);
    g_ipc.Send("P0 %s", CheckText(c0).c_str());
    g_ipc.Send("WAITING");
    SetStage("acquire-3");
    if (!s.Acquire(3)) return PeerFail(s);
    g_ipc.Send("ACQUIRED");
    SetStage("read-a");
    if (!ReadCheck(s, "A", Pattern::A, ca)) return PeerFail(s);
    SetStage("write-b");
    if (!s.WritePattern(Pattern::B, true)) return PeerFail(s);
    SetStage("release-4");
    if (!s.Release(4)) return PeerFail(s);
    g_ipc.Send("CHECKA %s", CheckText(ca).c_str());
    return 0;
}

// ------------------------------------------------------------------------------------------------ shared, CPU sync
// Each side waits for its own GPU work on the CPU before it tells the other side: only the open and the aliasing of
// the memory are under test, no cross-process GPU synchronisation.
static bool SharedCpuParent(Side &s)
{
    std::string m;
    HANDLE h = nullptr;
    Check c;
    SetStage("create-shared");
    if (!s.CreateShared(false, &h)) return FailA(s);
    SetStage("write-poison");
    if (!s.WritePattern(Pattern::Poison, false) || !s.Finish()) return FailA(s);
    if (!SendHandles(h, nullptr, nullptr)) return false;
    if (!PeerStep("OPENED", m, "peer-open")) return false;
    if (!PeerStep("P0", m, "peer-read-poison")) return false;
    VerdictCheck('B', CheckFromText(m));
    SetStage("write-a");
    if (!s.WritePattern(Pattern::A, true) || !s.Finish()) return FailA(s);
    g_ipc.Send("WROTE_A");
    if (!PeerStep("CHECKA", m, "peer-read-a-write-b")) return false;
    VerdictCheck('B', CheckFromText(m));
    SetStage("read-b");
    if (!ReadCheck(s, "B", Pattern::B, c)) return FailA(s);
    VerdictCheck('A', c);
    return true;
}

static int SharedCpuPeer(Side &s)
{
    std::string m;
    Check c0, ca;
    if (!PeerExpect("HANDLES", m)) return 2;
    SetStage("open-shared");
    if (!s.OpenShared(ParseHandle(m, "res"), false)) return PeerFail(s);
    g_ipc.Send("OPENED");
    SetStage("read-poison");
    if (!ReadCheck(s, "P0", Pattern::Poison, c0)) return PeerFail(s);
    g_ipc.Send("P0 %s", CheckText(c0).c_str());
    if (!PeerExpect("WROTE_A", m)) return 2;
    SetStage("read-a");
    if (!ReadCheck(s, "A", Pattern::A, ca)) return PeerFail(s);
    SetStage("write-b");
    if (!s.WritePattern(Pattern::B, true) || !s.Finish()) return PeerFail(s);
    g_ipc.Send("CHECKA %s", CheckText(ca).c_str());
    return 0;
}

// ------------------------------------------------------------------------------------------------ fence cells
// Two shared fences, both made by the creator's API: F0 is signalled only by the creator, F1 only by the opener, so
// every fence's values rise in submission order (on the host, a D3D11 Signal below a value the other process had
// already queued on the same fence returned E_INVALIDARG; D3D12 accepted it). Each GPU wait is followed by a private
// mark (Side::Mark), so a stuck wait and a signal that never reaches the other process are told apart: the waiting
// side reports whether its GPU got past the wait, the other side whether it saw the signal.

// The opener's first GPU wait; --inject skip-wait leaves it out (negative control: the gate and the reads must fail).
static bool LegOneWait(Side &s)
{
    if (!g_opt.skipWait) return s.GpuWait(0, 1);
    Log("INJECT skip-wait: the opener leaves out Wait(F0,1)");
    return true;
}

// Fence only. Leg 1, the creator's GPU signal releases the opener's GPU wait: the opener queues Wait(F0,1) Mark
// Signal(F1,1); gate: F1 still 0 after --gate-ms; the creator runs the delay workload and signals F0=1; the opener's
// mark is reached (PASSED1) and the creator sees F1=1 on the CPU. Leg 2 mirrors it: the creator queues Wait(F1,2) Mark
// Signal(F0,2); gate: F0 still 1 after --gate-ms; the opener runs the delay workload and signals F1=2; the creator's
// mark is reached and the opener sees F0=2. Final check: both fences read 2 in the creator.
static bool FenceParent(Side &s)
{
    std::string m;
    HANDLE f0 = nullptr, f1 = nullptr;
    SetStage("create-fences");
    if (!s.CreateFence(0, &f0) || !s.CreateFence(1, &f1)) return FailA(s);
    if (!SendHandles(nullptr, f0, f1)) return false;
    if (!PeerStep("OPENED", m, "peer-open-fences")) return false;
    if (!PeerStep("SUBMITTED", m, "peer-wait-f0-1-signal-f1-1")) return false;
    SetStage("gate-leg-1");
    Sleep(g_opt.gateMs);
    const UINT64 seen = s.Completed(1);
    VerdictGate("leg-1", seen == 0, "f1=" + std::to_string(seen));
    SetStage("signal-f0-1");
    if (!s.Delay() || !s.GpuSignal(0, 1)) return FailA(s);
    if (!PeerStep("PASSED1", m, "peer-gpu-past-wait-f0-1")) return false;
    SetStage("cpu-wait-f1-1");
    if (!s.CpuWait(1, 1, WaitDeadline())) return FailAFence(s, 1);
    SetStage("wait-f1-2-signal-f0-2");
    if (!s.GpuWait(1, 2) || !s.Mark() || !s.GpuSignal(0, 2)) return FailA(s);
    g_ipc.Send("SUBMITTED2");
    if (!PeerStep("GATE2", m, "peer-gate-leg-2")) return false;
    VerdictGate("leg-2", Field(m, "held") == "1", "f0=" + Field(m, "f0"));
    SetStage("gpu-past-wait-f1-2");
    if (!s.WaitMark(WaitDeadline())) return FailAFence(s, 1);
    if (!PeerStep("LEG2", m, "peer-cpu-wait-f0-2")) return false;
    const UINT64 v0 = s.Completed(0), v1 = s.Completed(1);
    Check c;
    c.what = "fences";
    c.total = 2;
    c.diff = (v0 != 2 ? 1 : 0) + (v1 != 2 ? 1 : 0);
    c.pass = c.diff == 0;
    c.content = "f0=" + std::to_string(v0) + ",f1=" + std::to_string(v1);
    VerdictCheck('A', c);
    return true;
}

static int FencePeer(Side &s)
{
    std::string m;
    if (!PeerExpect("HANDLES", m)) return 2;
    SetStage("open-fences");
    if (!s.OpenFence(0, ParseHandle(m, "fence0")) || !s.OpenFence(1, ParseHandle(m, "fence1"))) return PeerFail(s);
    g_ipc.Send("OPENED");
    SetStage("wait-f0-1-signal-f1-1");
    if (!LegOneWait(s) || !s.Mark() || !s.GpuSignal(1, 1)) return PeerFail(s);
    g_ipc.Send("SUBMITTED");
    SetStage("gpu-past-wait-f0-1");
    if (!s.WaitMark(PeerOpDeadline())) return PeerFailFence(s, 0);
    g_ipc.Send("PASSED1");
    if (!PeerExpect("SUBMITTED2", m)) return 2;
    SetStage("gate-leg-2");
    Sleep(g_opt.gateMs);
    const UINT64 seen = s.Completed(0);
    g_ipc.Send("GATE2 held=%u f0=%llu", seen == 1 ? 1u : 0u, (unsigned long long)seen);
    SetStage("signal-f1-2");
    if (!s.Delay() || !s.GpuSignal(1, 2)) return PeerFail(s);
    SetStage("cpu-wait-f0-2");
    if (!s.CpuWait(0, 2, PeerOpDeadline())) return PeerFailFence(s, 0);
    g_ipc.Send("LEG2");
    return 0;
}

// Shared texture ordered by the fences. Leg 1: the opener queues Wait(F0,1) -> read -> Mark -> Signal(F1,1); gate: F1
// still 0 after --gate-ms; the creator writes A behind the delay workload and signals F0=1; the opener's read must
// show A. Leg 2: the opener writes B behind the delay workload and does not signal yet; the creator queues Wait(F1,2)
// -> read -> Mark -> Signal(F0,2); gate: F0 still 1 after --gate-ms; the opener signals F1=2; the creator's read must
// show B.
static bool SharedFenceParent(Side &s)
{
    std::string m;
    HANDLE h = nullptr, f0 = nullptr, f1 = nullptr;
    SetStage("create-shared");
    if (!s.CreateShared(false, &h)) return FailA(s);
    SetStage("write-poison");
    if (!s.WritePattern(Pattern::Poison, false) || !s.Finish()) return FailA(s);
    SetStage("create-fences");
    if (!s.CreateFence(0, &f0) || !s.CreateFence(1, &f1)) return FailA(s);
    if (!SendHandles(h, f0, f1)) return false;
    if (!PeerStep("OPENED", m, "peer-open")) return false;
    if (!PeerStep("P0", m, "peer-read-poison")) return false;
    VerdictCheck('B', CheckFromText(m));
    if (!PeerStep("SUBMITTED", m, "peer-wait-f0-1-read-signal-f1-1")) return false;
    SetStage("gate-fence-1");
    Sleep(g_opt.gateMs);
    const UINT64 seen = s.Completed(1);
    VerdictGate("fence-1", seen == 0, "f1=" + std::to_string(seen));
    SetStage("write-a-signal-f0-1");
    if (!s.WritePattern(Pattern::A, true) || !s.GpuSignal(0, 1)) return FailA(s);
    if (!PeerStep("CHECKA", m, "peer-read-a-write-b")) return false;
    VerdictCheck('B', CheckFromText(m));
    SetStage("cpu-wait-f1-1");
    if (!s.CpuWait(1, 1, WaitDeadline())) return FailAFence(s, 1);
    SetStage("wait-f1-2-read-signal-f0-2");
    if (!s.GpuWait(1, 2) || !s.ReadSubmit() || !s.Mark() || !s.GpuSignal(0, 2)) return FailA(s);
    g_ipc.Send("SUBMITTED2");
    if (!PeerStep("GATE2", m, "peer-gate-fence-2")) return false;
    VerdictGate("fence-2", Field(m, "held") == "1", "f0=" + Field(m, "f0"));
    SetStage("gpu-past-wait-f1-2");
    if (!s.WaitMark(WaitDeadline())) return FailAFence(s, 1);
    SetStage("read-b");
    Image img;
    if (!s.ReadCollect(img)) return FailA(s);
    VerdictCheck('A', CompareImage("B", img, Pattern::B, false, false, 0));
    return PeerStep("LEG2", m, "peer-cpu-wait-f0-2");
}

static int SharedFencePeer(Side &s)
{
    std::string m;
    Check c0;
    if (!PeerExpect("HANDLES", m)) return 2;
    SetStage("open-shared");
    if (!s.OpenShared(ParseHandle(m, "res"), false)) return PeerFail(s);
    SetStage("open-fences");
    if (!s.OpenFence(0, ParseHandle(m, "fence0")) || !s.OpenFence(1, ParseHandle(m, "fence1"))) return PeerFail(s);
    g_ipc.Send("OPENED");
    SetStage("read-poison");
    if (!ReadCheck(s, "P0", Pattern::Poison, c0)) return PeerFail(s);
    g_ipc.Send("P0 %s", CheckText(c0).c_str());
    SetStage("wait-f0-1-read-signal-f1-1");
    if (!LegOneWait(s) || !s.ReadSubmit() || !s.Mark() || !s.GpuSignal(1, 1)) return PeerFail(s);
    g_ipc.Send("SUBMITTED");
    SetStage("gpu-past-wait-f0-1");
    if (!s.WaitMark(PeerOpDeadline())) return PeerFailFence(s, 0);
    SetStage("read-a");
    Image img;
    if (!s.ReadCollect(img)) return PeerFail(s);
    const Check ca = CompareImage("A", img, Pattern::A, false, false, 0);
    SetStage("write-b");
    if (!s.WritePattern(Pattern::B, true)) return PeerFail(s);
    g_ipc.Send("CHECKA %s", CheckText(ca).c_str());
    if (!PeerExpect("SUBMITTED2", m)) return 2;
    SetStage("gate-fence-2");
    Sleep(g_opt.gateMs);
    const UINT64 seen = s.Completed(0);
    g_ipc.Send("GATE2 held=%u f0=%llu", seen == 1 ? 1u : 0u, (unsigned long long)seen);
    SetStage("signal-f1-2");
    if (!s.GpuSignal(1, 2)) return PeerFail(s);
    SetStage("cpu-wait-f0-2");
    if (!s.CpuWait(0, 2, PeerOpDeadline())) return PeerFailFence(s, 0);
    g_ipc.Send("LEG2");
    return 0;
}

// ------------------------------------------------------------------------------------------------ frames
static bool ParentDevice(Api api, std::unique_ptr<Side> &side)
{
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid = {};
    std::string err;
    SetStage("adapter");
    if (!PickAdapter(adapter, luid, err)) {
        VerdictFail('A', "adapter", err, E_FAIL);
        return false;
    }
    g_opt.luid = luid; // the peer opens the same adapter
    g_opt.haveLuid = true;
    side.reset(MakeSide(api));
    SetStage("device");
    if (!side->Init(adapter.Get())) {
        VerdictFail('A', "device", side->lastCall, side->lastHr);
        return false;
    }
    LogModules("device");
    VerdictRoute('A', RouteTag(nullptr), side->fl);
    return true;
}

// Which routes can order a submitted copy of the shared surface behind a GPU wait on a shared fence. A --sync
// fence cell asks exactly that of both sides: the opener submits its read inside the wait-gated batch, and the
// creator submits its leg-2 read the same way. The CPU D3D11 UMD (bc250d3d.dll, Mesa d3d10umd) performs
// CopyResource as a CPU memcpy inside Flush and lets its event query complete at once, while
// ID3D11DeviceContext4::Wait defers only the kernel monitored-fence operations of the context - so the copy runs
// before the wait is satisfied, by construction and not by a driver defect. Round 1 of BD-075 scored three such
// rows as content failures (s12to11-fence, s12to11-rgba8 read poison about 300 ms before the creator wrote
// pattern A, by their own timestamps), which sent a lens hunting a memory fault that was not there.
// The injected negative control is exempt: it exists to show the gate can be violated, and a CPU-route side can
// still signal a fence, which is what that row measures.
static std::string FenceOrderingBlocker(const std::string &routeA, const std::string &routeB)
{
    const bool a = routeA.find("cpu11") != std::string::npos, b = routeB.find("cpu11") != std::string::npos;
    if (!a && !b) return std::string();
    return std::string("a ") + (a && b ? "cpu11 side" : a ? "cpu11 creator" : "cpu11 opener") +
           " copies on the CPU inside Flush, so no GPU wait can order its read of the shared surface" +
           " (route=A:" + routeA + ",B:" + routeB + ")";
}

int RunTwoProcessParent(const CellInfo &cell)
{
    std::unique_ptr<Side> side;
    if (!ParentDevice(cell.creator, side)) return 0;
    std::string err, m;
    SetStage("spawn-peer");
    if (!SpawnPeer(err)) {
        VerdictFail('A', "spawn-peer", err, E_FAIL);
        return 0;
    }
    if (!PeerStep("HELLO", m, "peer-device")) return 0;
    VerdictRoute('B', Field(m, "route"), Field(m, "fl"));
    if (cell.kind == Kind::Shared && g_opt.syncFence && !g_opt.skipWait) {
        const std::string blocker = FenceOrderingBlocker(RouteTag(nullptr), Field(m, "route"));
        if (!blocker.empty()) {
            VerdictSkip("route-cannot-order-gpu-waits", blocker);
            return 0;
        }
    }
    switch (cell.kind) {
    case Kind::Keyed: KeyedParent(*side); break;
    case Kind::Shared: g_opt.syncFence ? SharedFenceParent(*side) : SharedCpuParent(*side); break;
    case Kind::Fence: FenceParent(*side); break;
    default: break;
    }
    return 0;
}

int RunTwoProcessPeer(const CellInfo &cell)
{
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid = {};
    std::string err, m;
    SetStage("adapter");
    if (!PickAdapter(adapter, luid, err)) return PeerFailText("adapter", "PickAdapter", E_FAIL);
    std::unique_ptr<Side> side(MakeSide(cell.opener));
    SetStage("device");
    if (!side->Init(adapter.Get())) return PeerFail(*side);
    LogModules("device");
    std::string umds;
    const std::string route = RouteTag(&umds);
    g_ipc.Send("HELLO route=%s fl=%s umds=%s", route.c_str(), side->fl.c_str(), umds.c_str());
    int code = 0;
    switch (cell.kind) {
    case Kind::Keyed: code = KeyedPeer(*side); break;
    case Kind::Shared: code = g_opt.syncFence ? SharedFencePeer(*side) : SharedCpuPeer(*side); break;
    case Kind::Fence: code = FencePeer(*side); break;
    default: break;
    }
    if (code) return code;
    SetStage("wait-done");
    return PeerExpect("DONE", m) ? 0 : 2;
}

// ------------------------------------------------------------------------------------------------ local wait controls
int RunLocalWait(const CellInfo &cell)
{
    std::unique_ptr<Side> side;
    if (!ParentDevice(cell.creator, side)) return 0;
    std::string result;
    if (!side->LocalWaits(result)) {
        VerdictFail('A', Stage(), side->lastCall, side->lastHr, result.empty() ? "-" : result);
        VerdictRemoved('A', side->lastRemoved);
        return 0;
    }
    Log("CONTROLS %s", result.c_str());
    VerdictNote("controls", result);
    Check c;
    c.what = "controls";
    c.pass = true;
    c.total = 1;
    VerdictCheck('A', c);
    return 0;
}

// ------------------------------------------------------------------------------------------------ harness cell
int RunIpcParent()
{
    std::string err, m;
    SetStage("spawn-peer");
    if (!SpawnPeer(err)) {
        VerdictFail('A', "spawn-peer", err, E_FAIL);
        return 0;
    }
    if (!PeerStep("HELLO", m, "peer-start")) return 0;
    VerdictRoute('B', "none", "-");
    VerdictNote("peer_stderr", Field(m, "stderr"));
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE remote = DupToPeer(ev);
    if (!remote) {
        VerdictFail('A', "dup-handle", "DuplicateHandle(event)", E_FAIL);
        return 0;
    }
    g_ipc.Send("EVENT h=%llx", (unsigned long long)(ULONG_PTR)remote);
    if (!PeerStep("SET", m, "peer-set-event")) return 0;
    SetStage("wait-event");
    Check c;
    c.what = "event";
    c.total = 1;
    c.pass = WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0;
    c.diff = c.pass ? 0 : 1;
    if (!c.pass) c.content = "event-not-set";
    VerdictCheck('A', c);
    CloseHandle(ev);
    return 0;
}

int RunIpcPeer()
{
    std::string m;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    const bool valid = err && err != INVALID_HANDLE_VALUE;
    if (valid) {
        static const char probe[] = "capshare peer stderr probe\n";
        DWORD written = 0;
        WriteFile(err, probe, (DWORD)(sizeof(probe) - 1), &written, nullptr);
    }
    g_ipc.Send("HELLO route=none fl=- stderr=%s", valid ? "inherited" : "none");
    if (!PeerExpect("EVENT", m)) return 2;
    SetStage("set-event");
    const BOOL ok = SetEvent(ParseHandle(m, "h"));
    g_ipc.Send("SET ok=%u", ok ? 1u : 0u);
    SetStage("wait-done");
    return PeerExpect("DONE", m) ? 0 : 2;
}

// ------------------------------------------------------------------------------------------------ capture oracle, headless
static uint32_t Noise(int x, int y)
{
    uint32_t v = (uint32_t)x * 2654435761u ^ ((uint32_t)y + 0x5EEDu) * 2246822519u;
    v ^= v >> 13;
    v *= 0x5BD1E995u;
    return v ^ (v >> 15);
}

// A synthetic desktop texture (noise) with the window pattern copied in at (100, 80): the region read and compare the
// capture cells use must find it exactly, report a 3,2 displacement as shift=-3,-2, and call pattern A content seen
// while B is expected "pattern-a". Both 8-bit channel orders.
int RunCapDry()
{
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid = {};
    std::string err;
    SetStage("adapter");
    if (!PickAdapter(adapter, luid, err)) {
        VerdictFail('A', "adapter", err, E_FAIL);
        return 0;
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0};
    SetStage("device");
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (FAILED(hr)) {
        VerdictFail('A', "device", "D3D11CreateDevice", hr);
        return 0;
    }
    LogModules("device");
    VerdictRoute('A', RouteTag(nullptr), FlText(level));
    const int dw = 1024, dh = 768, x0 = 100, y0 = 80, w = (int)g_opt.w, h = (int)g_opt.h;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    for (DXGI_FORMAT f : formats) {
        const std::string tag = IsBgra(f) ? "bgra" : "rgba";
        SetStage(IsBgra(f) ? "capdry-bgra" : "capdry-rgba");
        std::vector<uint32_t> desk((size_t)dw * dh);
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                const uint32_t v = Noise(x, y) | 0xFF000000u;
                desk[(size_t)y * dw + x] = IsBgra(f) ? SwapRB(v) : v;
            }
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = dw;
        d.Height = dh;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {desk.data(), (UINT)dw * 4, 0};
        ComPtr<ID3D11Texture2D> desktop, window;
        hr = dev->CreateTexture2D(&d, &sd, &desktop);
        const std::vector<uint32_t> pat = MakeImageMemory(Pattern::A, (UINT)w, (UINT)h, f, true);
        D3D11_TEXTURE2D_DESC wd = d;
        wd.Width = (UINT)w;
        wd.Height = (UINT)h;
        D3D11_SUBRESOURCE_DATA ws = {pat.data(), (UINT)w * 4, 0};
        if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&wd, &ws, &window);
        if (FAILED(hr)) {
            VerdictFail('A', Stage(), "ID3D11Device::CreateTexture2D", hr);
            return 0;
        }
        ctx->CopySubresourceRegion(desktop.Get(), 0, x0, y0, 0, window.Get(), 0, nullptr);
        const RECT exact = {x0, y0, x0 + w, y0 + h}, off = {x0 + 3, y0 + 2, x0 + 3 + w, y0 + 2 + h};
        Check c;
        CompareFrame(dev.Get(), ctx.Get(), desktop.Get(), exact, Pattern::A, 0, c);
        c.what = "exact-" + tag;
        VerdictCheck('A', c);
        Check s;
        CompareFrame(dev.Get(), ctx.Get(), desktop.Get(), off, Pattern::A, 0, s);
        Log("CAPDRY shifted %s", CheckText(s).c_str());
        Check sc;
        sc.what = "shift-" + tag;
        sc.total = 1;
        sc.pass = !s.pass && s.note == "shift=-3,-2";
        sc.diff = sc.pass ? 0 : 1;
        sc.note = s.note;
        sc.content = s.content;
        VerdictCheck('A', sc);
        Check b;
        CompareFrame(dev.Get(), ctx.Get(), desktop.Get(), exact, Pattern::B, 0, b);
        Log("CAPDRY stale %s", CheckText(b).c_str());
        Check bc;
        bc.what = "stale-" + tag;
        bc.total = 1;
        bc.pass = !b.pass && b.content == "pattern-a";
        bc.diff = bc.pass ? 0 : 1;
        bc.content = b.content;
        VerdictCheck('A', bc);
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------ self-test
// The oracle without any device: exact match, first differing pixel, the content classes, alpha and tolerance rules,
// both channel orders, the shift search, the message fields and the check round trip through the pipe text.
int RunSelfTest()
{
    auto record = [](const char *name, bool ok, const std::string &detail = "-") {
        Log("SELFTEST %s ok=%u %s", name, ok ? 1u : 0u, detail.c_str());
        Check c;
        c.what = name;
        c.pass = ok;
        c.total = 1;
        c.diff = ok ? 0 : 1;
        c.note = detail;
        VerdictCheck('A', c);
    };
    auto image = [](Pattern p, int w, int h, bool opaque) {
        Image img;
        img.w = w;
        img.h = h;
        img.px.resize((size_t)w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) img.px[(size_t)y * w + x] = PatternPixel(p, x, y, opaque);
        return img;
    };
    const int w = 64, h = 48;
    Image a = image(Pattern::A, w, h, false);
    Check c = CompareImage("A", a, Pattern::A, false, false, 0);
    record("exact", c.pass && c.diff == 0 && c.total == (size_t)w * h, CheckText(c));

    Image one = a;
    one.px[(size_t)5 * w + 17] ^= 0x00000100u;
    c = CompareImage("A", one, Pattern::A, false, false, 0);
    record("one-pixel", !c.pass && c.x == 17 && c.y == 5 && c.diff == 1 && c.got == one.px[(size_t)5 * w + 17] &&
                            c.want == a.px[(size_t)5 * w + 17] && c.maxDelta == 1,
           CheckText(c));

    c = CompareImage("A", image(Pattern::Poison, w, h, false), Pattern::A, false, false, 0);
    record("poison", !c.pass && c.content == "poison" && c.diff == (size_t)w * h, c.content);
    c = CompareImage("A", image(Pattern::B, w, h, false), Pattern::A, false, false, 0);
    record("stale", !c.pass && c.content == "pattern-b", c.content);
    Image zero;
    zero.w = w;
    zero.h = h;
    zero.px.assign((size_t)w * h, 0);
    c = CompareImage("A", zero, Pattern::A, false, false, 0);
    record("zero", !c.pass && c.content == "zero", c.content);
    Image swapped = a;
    for (uint32_t &v : swapped.px) v = SwapRB(v);
    c = CompareImage("A", swapped, Pattern::A, false, false, 0);
    record("swapped", !c.pass && c.content == "swapped-rb", c.content);
    Image grey = zero;
    for (uint32_t &v : grey.px) v = 0xFF404040u;
    c = CompareImage("A", grey, Pattern::A, false, false, 0);
    record("constant", !c.pass && c.content == "constant-rgba:404040ff", c.content);

    Image opaque = image(Pattern::A, w, h, true), alpha = opaque;
    for (uint32_t &v : alpha.px) v &= 0x00FFFFFFu;
    record("alpha-ignored-rgb", CompareImage("A", alpha, Pattern::A, true, true, 0).pass);
    record("alpha-counted", !CompareImage("A", alpha, Pattern::A, true, false, 0).pass);

    Image close = opaque;
    close.px[0] = (close.px[0] & 0xFFFF00FFu) | ((((close.px[0] >> 8) & 0xFFu) ^ 0x02u) << 8);
    const Check t2 = CompareImage("A", close, Pattern::A, true, true, 2), t1 = CompareImage("A", close, Pattern::A, true, true, 1);
    record("tolerance", t2.pass && t2.maxDelta == 2 && !t1.pass && t1.diff == 1, CheckText(t1));

    bool roundTrip = true;
    for (DXGI_FORMAT f : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM}) {
        const std::vector<uint32_t> mem = MakeImageMemory(Pattern::A, (UINT)w, (UINT)h, f, false);
        Image back;
        roundTrip = roundTrip && ImageFromRows(back, mem.data(), (UINT)w * 4, w, h, f) && back.px == a.px;
    }
    const std::vector<uint32_t> bgra = MakeImageMemory(Pattern::Poison, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, false);
    roundTrip = roundTrip && bgra[0] == 0xC35AA53Cu; // bytes 3C A5 5A C3: B G R A of rgba:5aa53cc3
    record("channel-orders", roundTrip);

    Image big;
    big.w = 96;
    big.h = 80;
    big.px.assign((size_t)big.w * big.h, 0xFF101010u);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x) big.px[(size_t)(y + 10) * big.w + (x + 20)] = PatternPixel(Pattern::A, x, y, true);
    int dx = 99, dy = 99;
    const bool found = FindShift(big, 21, 10, 40, 30, Pattern::A, 8, 0, dx, dy);
    record("shift", found && dx == -1 && dy == 0, std::to_string(dx) + "," + std::to_string(dy));

    const std::string msg = "HANDLES res=1a fence=0";
    const std::string fail = "FAIL kind=fail stage=open-shared call=c hr=0x80070057 note=f1=2 removed=0x887A0020";
    record("fields", Field(msg, "res") == "1a" && Field(msg, "fence") == "0" && Field(msg, "x").empty() &&
                         Field("P0 what=P0 pass=1", "what") == "P0" && Field(fail, "note") == "f1=2" &&
                         (HRESULT)strtoul(Field(fail, "removed").c_str(), nullptr, 16) == DXGI_ERROR_DRIVER_INTERNAL_ERROR &&
                         (HRESULT)strtoul(Field(msg, "removed").c_str(), nullptr, 16) == S_OK);

    c = CompareImage("A", one, Pattern::A, false, false, 0);
    const Check back = CheckFromText("CHECKA " + CheckText(c));
    record("check-text", back.what == c.what && back.pass == c.pass && back.x == c.x && back.y == c.y &&
                             back.got == c.got && back.want == c.want && back.diff == c.diff && back.total == c.total &&
                             back.maxDelta == c.maxDelta && back.content == c.content,
           CheckText(back));
    return 0;
}
