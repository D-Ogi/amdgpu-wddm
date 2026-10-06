// SPDX-License-Identifier: MIT
// capshare.h - declarations shared by the capshare sources (M15.13 capture and shared-surface witness).
// The cells, the protocol and the output format are described at the top of main.cpp.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------------------------------------ cells
enum class Api { D3D11, D3D12 };
enum class Kind { Keyed, Shared, Fence, LocalWait, Ipc, CapDry, Dda, Wgc };

struct CellInfo {
    const char *name;
    Kind kind;
    Api creator; // the parent process (creator of the shared object, or the capture consumer)
    Api opener;  // the peer process (opener, or the capture producer's API when it is D3D12)
    bool peer;   // a second process takes part
    const char *what;
};
const CellInfo *FindCell(const std::string &name);
std::string CellList();

// ------------------------------------------------------------------------------------------------ options
struct Options {
    std::string cell;
    unsigned boundS = 30;
    std::wstring out = L"capshare.txt", json = L"result.json", stderrPath;
    UINT w = 256, h = 256;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    std::wstring adapter = L"auto", peerExe;
    bool syncFence = false, kmt = false, simultaneous = false, skipWait = false;
    unsigned delayCopies = 128, delaySize = 2048, gateMs = 300;
    bool producerGdi = false;
    int x = INT_MIN, y = INT_MIN, tolerance = 0;
    bool interactiveOk = false;
    std::vector<std::string> dbwin;
    bool selfTest = false;
    // internal
    bool peer = false, relaunched = false;
    HANDLE ipcIn = nullptr, ipcOut = nullptr;
    LUID luid = {};
    bool haveLuid = false;
    ULONGLONG t0 = 0, deadline = 0;
    std::wstring self; // full path of this executable
};
extern Options g_opt;

// ------------------------------------------------------------------------------------------------ log and time
bool LogOpen(const std::wstring &path, bool truncate, bool mirrorStdout);
void Log(const char *format, ...);
void InstallDebugStringCapture();
void FlushOds();
void SetStage(const char *stage);
const char *Stage();
ULONGLONG Now();                       // milliseconds since the run started (shared by both processes)
DWORD Remaining(ULONGLONG deadlineTick); // milliseconds until an absolute GetTickCount64 value, 0 when past
ULONGLONG WaitDeadline();              // the parent's deadline for waits on the peer (before its own watchdog)
std::string HrText(HRESULT hr);
std::string Narrow(const std::wstring &w);
std::wstring Widen(const std::string &s);
std::string FormatText(DXGI_FORMAT f);
const char *FlText(D3D_FEATURE_LEVEL fl);
void StartWatchdog(void (*onTimeout)());

// ------------------------------------------------------------------------------------------------ pattern oracle
enum class Pattern { A = 0, B = 1, Poison = 2 };
const uint32_t SeedA = 0x00A11CE5u, SeedB = 0x0B0B5EEDu, PoisonRgba = 0xC33CA55Au; // logical RGBA, R in the low byte
uint32_t PatternPixel(Pattern p, int x, int y, bool opaque);
uint32_t SwapRB(uint32_t v);
bool IsBgra(DXGI_FORMAT f);
bool IsRgba(DXGI_FORMAT f);
std::vector<uint32_t> MakeImageMemory(Pattern p, UINT w, UINT h, DXGI_FORMAT f, bool opaque);

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px; // logical RGBA, R in the low byte
};
bool ImageFromRows(Image &img, const void *data, UINT rowPitch, int w, int h, DXGI_FORMAT f);
Image Crop(const Image &img, int x, int y, int w, int h);

struct Check {
    std::string what;
    bool pass = false;
    int x = -1, y = -1;
    uint32_t got = 0, want = 0;
    size_t diff = 0, total = 0;
    int maxDelta = 0;
    std::string content = "-", note = "-";
};
Check CompareImage(const char *what, const Image &img, Pattern p, bool opaque, bool rgbOnly, int tolerance);
bool FindShift(const Image &big, int bx, int by, int w, int h, Pattern p, int margin, int tolerance, int &dx, int &dy);
std::string RgbaText(uint32_t v);
std::string CheckText(const Check &c);
Check CheckFromText(const std::string &msg);
std::string Field(const std::string &msg, const char *key);

// ------------------------------------------------------------------------------------------------ verdict
void VerdictInit(const std::string &cell);
void VerdictFail(char side, const std::string &stage, const std::string &call, HRESULT hr, const std::string &note = "-");
void VerdictTimeout(char side, const std::string &stage, const std::string &note = "-");
void VerdictCheck(char side, const Check &c);
void VerdictGate(const std::string &what, bool held, const std::string &detail);
// A cell this pair of routes cannot measure at all: result=skip, exit code 5, and the reason on the line. Not a
// pass and not a failure. BD-075 round 2: a --sync fence shared cell whose D3D11 side runs on the CPU UMD scored
// three content mismatches that were the harness asking for an ordering no CPU copy can have.
void VerdictSkip(const std::string &stage, const std::string &reason);
void VerdictRoute(char side, const std::string &route, const std::string &fl);
void VerdictCellEnded();                         // the cell's own work is over; fixes decided_ms for a pass
void VerdictNote(const std::string &key, const std::string &value);
void VerdictRemoved(char side, HRESULT removed); // note removed_A or removed_B, unless removed == S_OK
bool VerdictFailed();
int VerdictEmit();

// ------------------------------------------------------------------------------------------------ peer process and pipe
struct Ipc {
    bool Start();
    bool Send(const char *format, ...);
    // Waits for the next message whose verb is `verb`. A FAIL message from the other side, the end of the pipe or
    // the deadline end the wait with false and msg = that FAIL message, "EOF" or "TIMEOUT".
    bool Expect(const char *verb, std::string &msg, ULONGLONG deadlineTick);
    bool Has(const char *verb);
    bool TryNext(std::string &msg); // any message, without waiting
};
extern Ipc g_ipc;

struct Peer {
    HANDLE process = nullptr, job = nullptr;
    DWORD pid = 0;
};
extern Peer g_peerProcess;
bool SpawnPeer(std::string &error);
HANDLE DupToPeer(HANDLE h);
DWORD PeerExit(DWORD waitMs); // exit code, STILL_ACTIVE when it has not ended
void KillPeer();
// The parent's view of a failed wait on the peer: records the verdict and returns false.
bool PeerStep(const char *verb, std::string &msg, const char *stage);
void VerdictFromPeer(const std::string &msg, const char *stage);
// The peer's side: report a failure to the parent (returns the peer's exit code), wait for the parent's next message.
// removed = the device's removed reason after the failed call (S_OK: not removed).
int PeerFailText(const char *stage, const char *call, HRESULT hr, const std::string &note = "-", HRESULT removed = S_OK);
bool PeerExpect(const char *verb, std::string &msg);

// ------------------------------------------------------------------------------------------------ adapters, modules
bool PickAdapter(ComPtr<IDXGIAdapter1> &adapter, LUID &luid, std::string &error);
std::string RouteTag(std::string *umds);
void LogModules(const char *when);
void LogEnvironment();

// ------------------------------------------------------------------------------------------------ DBWIN listener
bool DbwinStart(const std::vector<std::string> &also);
void DbwinStop();

// ------------------------------------------------------------------------------------------------ API sides
class Side {
public:
    virtual ~Side() = default;
    Api api = Api::D3D11;
    const char *lastCall = "-";
    HRESULT lastHr = S_OK;
    // The device's removed reason right after the last failed call: the runtime removes the device for a driver error
    // it treats as critical (any OpenResource error other than E_OUTOFMEMORY), which the call's HRESULT alone hides.
    HRESULT lastRemoved = S_OK;
    std::string fl = "-";

    virtual bool Init(IDXGIAdapter1 *adapter) = 0;
    virtual bool CreateShared(bool keyed, HANDLE *out) = 0; // the creator writes poison afterwards
    virtual bool OpenShared(HANDLE h, bool keyed) = 0;
    virtual bool WritePattern(Pattern p, bool delay) = 0;   // submitted, not waited for
    virtual bool Delay() = 0;                               // the delay workload alone
    virtual bool ReadSubmit() = 0;
    virtual bool ReadCollect(Image &img) = 0;               // waits for the read
    virtual bool Finish() = 0;                              // CPU wait for everything submitted so far
    virtual bool Acquire(UINT64 key) = 0;
    virtual bool Release(UINT64 key) = 0;
    // Two shared fences per cell, each signalled by one process only (fence 0 by the creator, fence 1 by the opener),
    // so that every fence's values rise in submission order; both are created by the creator's API.
    virtual bool CreateFence(int i, HANDLE *out) = 0;
    virtual bool OpenFence(int i, HANDLE h) = 0;
    virtual bool GpuWait(int i, UINT64 value) = 0;
    virtual bool GpuSignal(int i, UINT64 value) = 0;
    virtual UINT64 Completed(int i) = 0;
    virtual bool CpuWait(int i, UINT64 value, ULONGLONG deadlineTick) = 0;
    // A local completion marker behind everything submitted so far (D3D11 event query, D3D12 private fence): it tells
    // whether this device's GPU got past a shared-fence wait, independent of whether its signals reach the other process.
    virtual bool Mark() = 0;
    virtual bool WaitMark(ULONGLONG deadlineTick) = 0;
    virtual bool LocalWaits(std::string &result) = 0;       // the local wait controls (cells w11, w12)

protected:
    bool Err(const char *call, HRESULT hr);
    virtual HRESULT RemovedReason() { return S_OK; }
};
Side *MakeSide(Api api);

// ------------------------------------------------------------------------------------------------ cells
int RunTwoProcessParent(const CellInfo &cell);
int RunTwoProcessPeer(const CellInfo &cell);
int RunLocalWait(const CellInfo &cell);
int RunIpcParent();
int RunIpcPeer();
int RunCapDry();
int RunCaptureParent(const CellInfo &cell);
int RunProducerPeer(const CellInfo &cell);
int RunSelfTest();
bool CompareFrame(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const RECT &rect, Pattern p,
                  int tolerance, Check &c);
