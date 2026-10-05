// SPDX-License-Identifier: MIT
// capture.cpp - the capture cells. The peer is the producer: a borderless topmost no-activate popup of the cell size
// that shows pattern A (opaque, alpha ignored) through a D3D12 flip-model swap chain (default) or GDI, placed away from
// the cursor and from windows above it, re-presented every 100 ms. The parent is the consumer: a D3D11 device on the
// same adapter that captures the window's rectangle through Desktop Duplication (dda) or Windows.Graphics.Capture
// (wgc), until a frame matches pattern A exactly (RGB after channel-order normalisation; alpha ignored; --tolerance
// to allow a per-channel delta), then asks the producer to switch to pattern B and waits for a frame with B, which
// proves that capture delivers updates and not one stale image. A mismatch reports the closest frame, its first
// differing pixel, what the region holds (pattern-a, zero, constant, mixed, ...) and a displacement if the pattern was
// found nearby (shift=dx,dy).
#include "capshare.h"
#include <unknwn.h>
#include <inspectable.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>

#pragma comment(lib, "WindowsApp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ------------------------------------------------------------------------------------------------ frame oracle
struct StagingCache {
    ComPtr<ID3D11Texture2D> tex;
    ID3D11Device *dev = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
static StagingCache g_staging;

static void LogTexture(const char *what, ID3D11Texture2D *t)
{
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    Log("TEXTURE11 %s %ux%u format=%s mips=%u array=%u samples=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", what, d.Width,
        d.Height, FormatText(d.Format).c_str(), d.MipLevels, d.ArraySize, d.SampleDesc.Count, (unsigned)d.Usage,
        d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
}

// Copies `rect` of the frame plus a 16-pixel margin to a staging texture and compares the rectangle with the pattern.
bool CompareFrame(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const RECT &rect, Pattern p,
                  int tolerance, Check &c)
{
    const char *name = p == Pattern::A ? "A" : "B";
    c = Check();
    c.what = name;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    if (!IsBgra(d.Format) && !IsRgba(d.Format)) {
        c.content = "unsupported-frame-format";
        c.note = FormatText(d.Format);
        return false;
    }
    const int margin = 16;
    const RECT big = {std::max<LONG>(0, rect.left - margin), std::max<LONG>(0, rect.top - margin),
                      std::min<LONG>((LONG)d.Width, rect.right + margin), std::min<LONG>((LONG)d.Height, rect.bottom + margin)};
    if (big.right <= big.left || big.bottom <= big.top) {
        c.content = "rect-outside-frame";
        c.note = std::to_string(d.Width) + "x" + std::to_string(d.Height);
        return false;
    }
    const UINT bw = (UINT)(big.right - big.left), bh = (UINT)(big.bottom - big.top);
    if (!g_staging.tex || g_staging.dev != dev || g_staging.w != bw || g_staging.h != bh || g_staging.format != d.Format) {
        g_staging = StagingCache();
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = bw;
        sd.Height = bh;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &g_staging.tex);
        if (FAILED(hr)) {
            c.content = "staging-create-failed";
            c.note = HrText(hr);
            return false;
        }
        g_staging.dev = dev;
        g_staging.w = bw;
        g_staging.h = bh;
        g_staging.format = d.Format;
    }
    const D3D11_BOX box = {(UINT)big.left, (UINT)big.top, 0, (UINT)big.right, (UINT)big.bottom, 1};
    ctx->CopySubresourceRegion(g_staging.tex.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = ctx->Map(g_staging.tex.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        c.content = "map-failed";
        c.note = HrText(hr);
        return false;
    }
    Image bigImg;
    ImageFromRows(bigImg, m.pData, m.RowPitch, (int)bw, (int)bh, d.Format);
    ctx->Unmap(g_staging.tex.Get(), 0);
    const int ox = rect.left - big.left, oy = rect.top - big.top, w = rect.right - rect.left, h = rect.bottom - rect.top;
    c = CompareImage(name, Crop(bigImg, ox, oy, w, h), p, true, true, tolerance);
    if (!c.pass) {
        int dx = 0, dy = 0;
        if (FindShift(bigImg, ox, oy, w, h, p, margin, tolerance, dx, dy))
            c.note = "shift=" + std::to_string(dx) + "," + std::to_string(dy);
    }
    return c.pass;
}

// ------------------------------------------------------------------------------------------------ consumer phases
struct PhaseState {
    Pattern want = Pattern::A;
    bool doneA = false, doneB = false, switchPending = false;
    Check bestA, bestB;
    bool haveBestA = false, haveBestB = false;
    unsigned frames = 0, mismatches = 0;
    ULONGLONG atA = 0, atB = 0;
};

static bool OnFrame(PhaseState &st, ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, const RECT &rect)
{
    ++st.frames;
    Check c;
    if (CompareFrame(dev, ctx, tex, rect, st.want, g_opt.tolerance, c)) {
        VerdictCheck('A', c);
        if (st.want == Pattern::A) {
            st.doneA = true;
            st.atA = Now();
            st.want = Pattern::B;
            st.switchPending = true;
            return false;
        }
        st.doneB = true;
        st.atB = Now();
        return true;
    }
    ++st.mismatches;
    if (st.mismatches == 1 || st.mismatches % 25 == 0) Log("FRAME %u mismatch %s", st.frames, CheckText(c).c_str());
    Check &best = st.want == Pattern::A ? st.bestA : st.bestB;
    bool &have = st.want == Pattern::A ? st.haveBestA : st.haveBestB;
    if (!have || c.diff < best.diff || (c.diff == best.diff && c.total > best.total)) {
        best = c;
        have = true;
    }
    return false;
}

static bool DoSwitch(PhaseState &st, const char *stageB)
{
    st.switchPending = false;
    std::string m;
    g_ipc.Send("SWITCH");
    if (!PeerStep("SWITCHED", m, "producer-switch-to-b")) return false;
    SetStage(stageB);
    return true;
}

static void FinishPhases(const PhaseState &st)
{
    VerdictNote("frames", std::to_string(st.frames));
    VerdictNote("mismatched_frames", std::to_string(st.mismatches));
    VerdictNote("a_at_ms", st.doneA ? std::to_string(st.atA) : "-");
    VerdictNote("b_at_ms", st.doneB ? std::to_string(st.atB) : "-");
    if (VerdictFailed()) return;
    if (!st.doneA) {
        if (st.haveBestA) VerdictCheck('A', st.bestA);
        else VerdictFail('A', Stage(), "no-frame-arrived", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    } else if (!st.doneB) {
        if (st.haveBestB) VerdictCheck('A', st.bestB);
        else VerdictFail('A', Stage(), "no-frame-after-switch", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
}

// ------------------------------------------------------------------------------------------------ Desktop Duplication
static void RunDda(ID3D11Device *dev, ID3D11DeviceContext *ctx, IDXGIAdapter1 *adapter, const RECT &win)
{
    SetStage("find-output");
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC od = {};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> o;
        if (adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC d = {};
        o->GetDesc(&d);
        const RECT r = d.DesktopCoordinates;
        Log("OUTPUT index=%u attached=%u desktop=%ld,%ld,%ld,%ld rotation=%d", i, d.AttachedToDesktop ? 1u : 0u, r.left,
            r.top, r.right, r.bottom, (int)d.Rotation);
        if (!output && d.AttachedToDesktop && win.left >= r.left && win.top >= r.top && win.right <= r.right &&
            win.bottom <= r.bottom) {
            output = o;
            od = d;
        }
    }
    if (!output) {
        VerdictFail('A', "find-output", "no-output-of-the-adapter-holds-the-window", DXGI_ERROR_NOT_FOUND);
        return;
    }
    ComPtr<IDXGIOutput6> o6;
    if (SUCCEEDED(output.As(&o6))) {
        DXGI_OUTPUT_DESC1 d1 = {};
        if (SUCCEEDED(o6->GetDesc1(&d1)))
            Log("OUTPUT chosen bits_per_color=%u color_space=%d max_luminance=%.0f", d1.BitsPerColor, (int)d1.ColorSpace,
                d1.MaxLuminance);
    }
    const RECT local = {win.left - od.DesktopCoordinates.left, win.top - od.DesktopCoordinates.top,
                        win.right - od.DesktopCoordinates.left, win.bottom - od.DesktopCoordinates.top};
    ComPtr<IDXGIOutputDuplication> dup;
    auto duplicate = [&]() -> HRESULT {
        dup.Reset();
        ComPtr<IDXGIOutput1> o1;
        HRESULT hr = output.As(&o1);
        if (FAILED(hr)) return hr;
        hr = o1->DuplicateOutput(dev, &dup);
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            ComPtr<IDXGIOutput5> o5;
            if (SUCCEEDED(output.As(&o5))) {
                const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
                hr = o5->DuplicateOutput1(dev, 0, 1, formats, &dup);
                Log("DuplicateOutput unsupported; DuplicateOutput1(B8G8R8A8) hr=%s", HrText(hr).c_str());
            }
        }
        return hr;
    };
    SetStage("duplicate-output");
    HRESULT hr = duplicate();
    if (FAILED(hr)) {
        VerdictFail('A', "duplicate-output", "IDXGIOutput1::DuplicateOutput", hr);
        return;
    }
    DXGI_OUTDUPL_DESC dd = {};
    dup->GetDesc(&dd);
    Log("DUPLICATION mode=%ux%u format=%s rotation=%d desktop_image_in_system_memory=%u", dd.ModeDesc.Width,
        dd.ModeDesc.Height, FormatText(dd.ModeDesc.Format).c_str(), (int)dd.Rotation,
        dd.DesktopImageInSystemMemory ? 1u : 0u);
    VerdictNote("dda_system_memory", dd.DesktopImageInSystemMemory ? "1" : "0");
    PhaseState st;
    unsigned timeouts = 0, lost = 0;
    SetStage("dda-frames-a");
    while (Remaining(WaitDeadline()) && !st.doneB) {
        DXGI_OUTDUPL_FRAME_INFO fi = {};
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(std::min<DWORD>(250, Remaining(WaitDeadline())), &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            ++timeouts;
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            Log("DDA access lost (%u)", ++lost);
            if (lost > 20 || FAILED(hr = duplicate())) {
                VerdictFail('A', Stage(), "IDXGIOutput1::DuplicateOutput(after-access-lost)", hr);
                break;
            }
            continue;
        }
        if (FAILED(hr)) {
            VerdictFail('A', Stage(), "IDXGIOutputDuplication::AcquireNextFrame", hr);
            break;
        }
        ComPtr<ID3D11Texture2D> tex;
        hr = res.As(&tex);
        if (FAILED(hr)) {
            dup->ReleaseFrame();
            VerdictFail('A', Stage(), "QueryInterface(ID3D11Texture2D,frame)", hr);
            break;
        }
        if (!st.frames) {
            LogTexture("dda-frame", tex.Get());
            Log("DDA first frame accumulated=%u last_present=%lld pointer_visible=%u", fi.AccumulatedFrames,
                (long long)fi.LastPresentTime.QuadPart, fi.PointerPosition.Visible ? 1u : 0u);
        }
        const bool done = OnFrame(st, dev, ctx, tex.Get(), local);
        tex.Reset();
        res.Reset();
        dup->ReleaseFrame();
        if (st.switchPending && !DoSwitch(st, "dda-frames-b")) break;
        if (done) break;
    }
    VerdictNote("dda_wait_timeouts", std::to_string(timeouts));
    VerdictNote("dda_access_lost", std::to_string(lost));
    FinishPhases(st);
}

// ------------------------------------------------------------------------------------------------ Windows.Graphics.Capture
static void RunWgc(ID3D11Device *dev, ID3D11DeviceContext *ctx, HWND hwnd, int w, int h)
{
    namespace wgc = winrt::Windows::Graphics::Capture;
    namespace wgd = winrt::Windows::Graphics::DirectX;
    const char *call = "winrt::init_apartment";
    try {
        SetStage("wgc-init");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        call = "GraphicsCaptureSession::IsSupported";
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            VerdictFail('A', Stage(), call, E_NOTIMPL, "capture-not-supported");
            return;
        }
        call = "IGraphicsCaptureItemInterop::CreateForWindow";
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        const auto size = item.Size();
        Log("WGC item %dx%d", size.Width, size.Height);
        call = "CreateDirect3D11DeviceFromDXGIDevice";
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(dev->QueryInterface(IID_PPV_ARGS(&dxgi)));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        const auto device = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
        // The free-threaded pool may use the device from its worker thread while this one copies frames.
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE);
        SetStage("wgc-frame-pool");
        call = "Direct3D11CaptureFramePool::CreateFreeThreaded";
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                         2, size);
        call = "Direct3D11CaptureFramePool::CreateCaptureSession";
        auto session = pool.CreateCaptureSession(item);
        try {
            session.IsCursorCaptureEnabled(false);
        } catch (winrt::hresult_error const &e) {
            Log("WGC IsCursorCaptureEnabled(false) hr=%s", HrText((HRESULT)(int32_t)e.code()).c_str());
        }
        try {
            session.IsBorderRequired(false);
        } catch (winrt::hresult_error const &e) {
            Log("WGC IsBorderRequired(false) hr=%s (the border is drawn on screen, not into the frames)",
                HrText((HRESULT)(int32_t)e.code()).c_str());
        }
        SetStage("wgc-start");
        call = "GraphicsCaptureSession::StartCapture";
        session.StartCapture();
        const RECT rect = {0, 0, w, h};
        PhaseState st;
        SetStage("wgc-frames-a");
        while (Remaining(WaitDeadline()) && !st.doneB) {
            call = "Direct3D11CaptureFramePool::TryGetNextFrame";
            auto frame = pool.TryGetNextFrame();
            if (!frame) {
                Sleep(10);
                continue;
            }
            call = "IDirect3DDxgiInterfaceAccess::GetInterface";
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> tex;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&tex)));
            if (!st.frames) {
                const auto content = frame.ContentSize();
                Log("WGC first frame content=%dx%d", content.Width, content.Height);
                LogTexture("wgc-frame", tex.Get());
            }
            const bool done = OnFrame(st, dev, ctx, tex.Get(), rect);
            tex.Reset();
            frame.Close();
            if (st.switchPending && !DoSwitch(st, "wgc-frames-b")) break;
            if (done) break;
        }
        session.Close();
        pool.Close();
        FinishPhases(st);
    } catch (winrt::hresult_error const &e) {
        VerdictFail('A', Stage(), call, (HRESULT)(int32_t)e.code(), winrt::to_string(e.message()));
    }
}

// ------------------------------------------------------------------------------------------------ consumer frame
int RunCaptureParent(const CellInfo &cell)
{
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid = {};
    std::string err, m;
    SetStage("adapter");
    if (!PickAdapter(adapter, luid, err)) {
        VerdictFail('A', "adapter", err, E_FAIL);
        return 0;
    }
    g_opt.luid = luid;
    g_opt.haveLuid = true;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                               D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
    SetStage("device");
    const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (FAILED(hr)) {
        VerdictFail('A', "device", "D3D11CreateDevice", hr);
        return 0;
    }
    Log("DEVICE api=11 fl=%s (consumer)", FlText(level));
    LogModules("device");
    VerdictRoute('A', RouteTag(nullptr), FlText(level));
    SetStage("spawn-producer");
    if (!SpawnPeer(err)) {
        VerdictFail('A', "spawn-producer", err, E_FAIL);
        return 0;
    }
    if (!PeerStep("HELLO", m, "producer-device")) return 0;
    VerdictRoute('B', Field(m, "route"), Field(m, "fl"));
    if (!PeerStep("READY", m, "producer-window")) return 0;
    const HWND hwnd = (HWND)(ULONG_PTR)_strtoui64(Field(m, "hwnd").c_str(), nullptr, 16);
    const RECT win = {atol(Field(m, "x").c_str()), atol(Field(m, "y").c_str()),
                      atol(Field(m, "x").c_str()) + atol(Field(m, "w").c_str()),
                      atol(Field(m, "y").c_str()) + atol(Field(m, "h").c_str())};
    VerdictNote("window", Field(m, "x") + "," + Field(m, "y") + "," + Field(m, "w") + "x" + Field(m, "h"));
    VerdictNote("producer", g_opt.producerGdi ? "gdi" : "d3d12");
    if (Field(m, "occluders") != "0") VerdictNote("occluders", Field(m, "occluders") + ":" + Field(m, "by"));
    if (cell.kind == Kind::Dda) RunDda(dev.Get(), ctx.Get(), adapter.Get(), win);
    else RunWgc(dev.Get(), ctx.Get(), hwnd, win.right - win.left, win.bottom - win.top);
    // the runtime removes the consumer's device when its UMD refuses to open the capture surface with a critical error
    VerdictRemoved('A', dev->GetDeviceRemovedReason());
    return 0;
}

// ------------------------------------------------------------------------------------------------ producer
static Pattern g_shown = Pattern::A;
static bool g_gdi = false;
static std::vector<uint32_t> g_gdiBits[2];

static LRESULT CALLBACK ProducerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_gdi) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)g_opt.w;
            bi.bmiHeader.biHeight = -(LONG)g_opt.h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, 0, 0, g_opt.w, g_opt.h, 0, 0, 0, g_opt.h, g_gdiBits[(int)g_shown].data(), &bi,
                              DIB_RGB_COLORS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Producer12 {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> upload[2];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 value = 0;
    HANDLE event = nullptr;
    std::string fl = "-";
    const char *call = "-";
    HRESULT hr = S_OK;

    bool Fail(const char *c, HRESULT h)
    {
        call = c;
        hr = h;
        return false;
    }

    bool Init(IDXGIAdapter1 *adapter, HWND hwnd)
    {
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr)) return Fail("D3D12CreateDevice", hr);
        static const D3D_FEATURE_LEVEL req[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {ARRAYSIZE(req), req, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof(fls))))
            fls.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        fl = FlText(fls.MaxSupportedFeatureLevel);
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr)) return Fail("ID3D12Device::CreateCommandQueue", hr);
        ComPtr<IDXGIFactory2> factory;
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return Fail("CreateDXGIFactory2", hr);
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = g_opt.w;
        d.Height = g_opt.h;
        d.Format = g_opt.format;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_NONE;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        ComPtr<IDXGISwapChain1> sc1;
        hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &d, nullptr, nullptr, &sc1);
        if (FAILED(hr)) return Fail("IDXGIFactory2::CreateSwapChainForHwnd", hr);
        hr = sc1.As(&chain);
        if (FAILED(hr)) return Fail("QueryInterface(IDXGISwapChain3)", hr);
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (FAILED(hr)) return Fail("ID3D12Device::CreateCommandAllocator", hr);
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr)) return Fail("ID3D12Device::CreateCommandList", hr);
        list->Close();
        hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return Fail("ID3D12Device::CreateFence", hr);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<ID3D12Resource> bb;
        hr = chain->GetBuffer(0, IID_PPV_ARGS(&bb));
        if (FAILED(hr)) return Fail("IDXGISwapChain::GetBuffer", hr);
        const D3D12_RESOURCE_DESC bd = bb->GetDesc();
        UINT64 total = 0;
        dev->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = total;
        ud.Height = 1;
        ud.DepthOrArraySize = 1;
        ud.MipLevels = 1;
        ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (int i = 0; i < 2; ++i) {
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&upload[i]));
            if (FAILED(hr)) return Fail("ID3D12Device::CreateCommittedResource(upload)", hr);
            void *p = nullptr;
            const D3D12_RANGE none = {0, 0};
            hr = upload[i]->Map(0, &none, &p);
            if (FAILED(hr)) return Fail("ID3D12Resource::Map(upload)", hr);
            const std::vector<uint32_t> mem = MakeImageMemory((Pattern)i, g_opt.w, g_opt.h, bd.Format, true);
            for (UINT y = 0; y < g_opt.h; ++y)
                memcpy((unsigned char *)p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, mem.data() + (size_t)y * g_opt.w,
                       (size_t)g_opt.w * 4);
            upload[i]->Unmap(0, nullptr);
        }
        Log("PRODUCER d3d12 swap chain %ux%u format=%s flip-discard buffers=2", g_opt.w, g_opt.h,
            FormatText(bd.Format).c_str());
        return true;
    }

    bool Frame(Pattern p)
    {
        ComPtr<ID3D12Resource> bb;
        hr = chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        if (FAILED(hr)) return Fail("IDXGISwapChain::GetBuffer", hr);
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = bb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload[(int)p].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        hr = list->Close();
        if (FAILED(hr)) return Fail("ID3D12GraphicsCommandList::Close", hr);
        ID3D12CommandList *ls[] = {list.Get()};
        queue->ExecuteCommandLists(1, ls);
        hr = chain->Present(1, 0);
        if (FAILED(hr)) return Fail("IDXGISwapChain::Present", hr);
        ++value;
        hr = queue->Signal(fence.Get(), value);
        if (FAILED(hr)) return Fail("ID3D12CommandQueue::Signal", hr);
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            if (WaitForSingleObject(event, 2000) != WAIT_OBJECT_0) {
                const HRESULT r = dev->GetDeviceRemovedReason();
                return Fail("frame-fence-wait", r != S_OK ? r : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            }
        }
        const HRESULT r = dev->GetDeviceRemovedReason();
        return r == S_OK ? true : Fail("GetDeviceRemovedReason(after-present)", r);
    }
};

// Candidate positions in the primary monitor's work area, the first ones away from the cursor.
static std::vector<POINT> Candidates(int w, int h)
{
    if (g_opt.x != INT_MIN) return {POINT{g_opt.x, g_opt.y}};
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT wa = mi.rcWork;
    const POINT all[] = {{wa.left + 48, wa.top + 48},
                         {wa.right - w - 48, wa.top + 48},
                         {wa.left + 48, wa.bottom - h - 48},
                         {wa.right - w - 48, wa.bottom - h - 48},
                         {(wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2}};
    POINT cursor = {};
    GetCursorPos(&cursor);
    std::vector<POINT> clear, underCursor;
    for (const POINT &p : all) {
        const RECT guard = {p.x - 48, p.y - 48, p.x + w + 48, p.y + h + 48};
        (PtInRect(&guard, cursor) ? underCursor : clear).push_back(p);
    }
    clear.insert(clear.end(), underCursor.begin(), underCursor.end());
    return clear;
}

// Visible, uncloaked top-level windows above ours (in z-order) that intersect its rectangle.
static int Occluders(HWND hwnd, std::string &names)
{
    RECT r = {};
    GetWindowRect(hwnd, &r);
    int n = 0;
    for (HWND w = GetWindow(hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w)) continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        RECT wr = {}, x = {};
        if (!GetWindowRect(w, &wr) || !IntersectRect(&x, &wr, &r)) continue;
        ++n;
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof(cls));
        if (names.size() < 160) names += (names.empty() ? "" : ",") + std::string(cls);
    }
    return n;
}

static void Pump(DWORD ms)
{
    const ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)std::min<ULONGLONG>(end - now, 10), QS_ALLINPUT);
    }
}

int RunProducerPeer(const CellInfo &cell)
{
    (void)cell;
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid = {};
    std::string err, m;
    SetStage("adapter");
    if (!PickAdapter(adapter, luid, err)) return PeerFailText("adapter", "PickAdapter", E_FAIL);
    const int w = (int)g_opt.w, h = (int)g_opt.h;
    g_gdi = g_opt.producerGdi;
    for (int i = 0; i < 2; ++i) g_gdiBits[i] = MakeImageMemory((Pattern)i, g_opt.w, g_opt.h, DXGI_FORMAT_B8G8R8A8_UNORM, true);

    SetStage("window");
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ProducerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"capshare-producer";
    RegisterClassExW(&wc);
    const std::vector<POINT> spots = Candidates(w, h);
    // No WS_EX_TOOLWINDOW: capture pickers skip tool windows and WGC's window item may refuse one; WS_EX_NOACTIVATE
    // already keeps the window off the taskbar.
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"capshare producer", WS_POPUP,
                                spots[0].x, spots[0].y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return PeerFailText("window", "CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const COLORREF noBorder = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
    const BOOL noTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));

    Producer12 p12;
    std::string fl = "-";
    if (!g_gdi) {
        SetStage("producer-device");
        if (!p12.Init(adapter.Get(), hwnd)) return PeerFailText(Stage(), p12.call, p12.hr);
        fl = p12.fl;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    auto render = [&]() -> bool {
        if (g_gdi) {
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            return true;
        }
        return p12.Frame(g_shown);
    };
    SetStage("first-frame");
    if (!render()) return PeerFailText(Stage(), p12.call, p12.hr);
    LogModules("producer");
    std::string umds;
    const std::string route = g_gdi ? "gdi" : RouteTag(&umds);
    g_ipc.Send("HELLO route=%s fl=%s umds=%s", route.c_str(), fl.c_str(), umds.empty() ? "-" : umds.c_str());

    SetStage("placement");
    std::string names;
    int occluded = -1;
    for (size_t i = 0; i < spots.size(); ++i) {
        if (i) SetWindowPos(hwnd, HWND_TOPMOST, spots[i].x, spots[i].y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        Pump(150);
        if (!render()) return PeerFailText(Stage(), p12.call, p12.hr);
        names.clear();
        occluded = Occluders(hwnd, names);
        if (!occluded) break;
        Log("PRODUCER spot %zu has %d occluder(s): %s", i, occluded, names.c_str());
    }
    Pump(300);
    RECT r = {};
    GetWindowRect(hwnd, &r);
    POINT cursor = {};
    GetCursorPos(&cursor);
    g_ipc.Send("READY hwnd=%llx x=%ld y=%ld w=%ld h=%ld occluders=%d by=%s cursor=%ld,%ld",
               (unsigned long long)(ULONG_PTR)hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, occluded,
               names.empty() ? "-" : names.c_str(), cursor.x, cursor.y);

    SetStage("presenting-a");
    ULONGLONG last = GetTickCount64();
    unsigned frames = 0;
    for (;;) {
        Pump(10);
        std::string msg;
        while (g_ipc.TryNext(msg)) {
            if (msg == "EOF" || msg.rfind("DONE", 0) == 0) {
                Log("PRODUCER end after %u frames (%s)", frames, msg.c_str());
                DestroyWindow(hwnd);
                return 0;
            }
            if (msg.rfind("SWITCH", 0) == 0) {
                g_shown = Pattern::B;
                SetStage("presenting-b");
                if (!render()) return PeerFailText(Stage(), p12.call, p12.hr);
                ++frames;
                last = GetTickCount64();
                g_ipc.Send("SWITCHED frames=%u", frames);
            }
        }
        if (!Remaining(g_opt.deadline)) {
            Log("PRODUCER bound reached after %u frames", frames);
            DestroyWindow(hwnd);
            return 3;
        }
        if (GetTickCount64() - last >= 100) {
            if (!render()) return PeerFailText(Stage(), p12.call, p12.hr);
            ++frames;
            last = GetTickCount64();
        }
    }
}
