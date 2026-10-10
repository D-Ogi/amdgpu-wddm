// SPDX-License-Identifier: MIT
#include "gpu_pipeline.h"
#include <dxgi.h>
#include <mferror.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "gen/cs_import_nv12.h"
#include "gen/cs_import_nv12sys.h"
#include "gen/cs_import_bgra.h"
#include "gen/cs_import_planar.h"
#include "gen/cs_motion.h"
#include "gen/cs_encode_intra.h"
#include "gen/cs_encode_inter.h"
#include "gen/cs_deblock.h"
#include "gen/cs_deblock_rows.h"

namespace bc250h264 {

namespace {

struct Constants {
    uint32_t widthMb;
    uint32_t heightMb;
    uint32_t padW;
    uint32_t padH;
    uint32_t qpY;
    uint32_t qpC;
    uint32_t isIntra;
    uint32_t diagonalBase;
    uint32_t diagonal;
    uint32_t lambda;
    uint32_t skipBias;
    uint32_t srcWidth;
    uint32_t srcHeight;
    int32_t alphaOffsetDiv2;
    int32_t betaOffsetDiv2;
    uint32_t deblockIdc;
};
static_assert(sizeof(Constants) == 64, "constant buffer must match h264_common.hlsli");

constexpr uint32_t kDivUp(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Wall clock in milliseconds, from the performance counter, for the readback stage. The frame level
// code has its own copy of this; the two are deliberately independent so that the pipeline needs
// nothing from the layer above it.
double NowMs()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// How long a timestamp query may take to retire before the encode gives up on the measurement. The
// picture is already finished when we wait here, so this bound only decides when a diagnostic is
// reported as absent instead of holding the encode thread.
constexpr double kQueryWaitMs = 1000.0;

// How many stage marks one picture may place. A 1080p I picture needs 443 of them (1 import, 187
// anti-diagonals, 255 deblocking waves); BC250_MFT_SERIAL_DEBLOCK, which dispatches one macroblock
// at a time, needs far more and is told that its tail is untimed rather than allowed to allocate
// without a bound.
constexpr uint32_t kMaxStageMarks = 4096;

} // namespace

const char* GpuStageName(uint32_t stage)
{
    switch (stage) {
    case GpuStageImport:  return "import";
    case GpuStageMotion:  return "motion";
    case GpuStageMode:    return "mode";
    case GpuStageDeblock: return "deblock";
    default:              return "?";
    }
}

int MftTraceOn()
{
    static int state = -1;
    if (state < 0) {
        char buf[8];
        state = (GetEnvironmentVariableA("BC250_MFT_TRACE", buf, sizeof(buf)) > 0) ? 1 : 0;
    }
    return state;
}

void MftTrace(const char* fmt, ...)
{
    if (MftTraceOn() == 0) {
        return;
    }
    char line[256];
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[mft t%05lu] ",
                              static_cast<unsigned long>(GetCurrentThreadId()));
    if (n <= 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int m = _vsnprintf_s(line + n, sizeof(line) - static_cast<size_t>(n), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m <= 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n + m), &written, nullptr);
}

bool IsCodableFrameSize(uint32_t width, uint32_t height)
{
    if (width < 16u || height < 16u || width > 4096u || height > 4096u) {
        return false;
    }
    return ((width | height) & 1u) == 0;
}

HRESULT GpuEncoder::WaitForQuery(ID3D11Asynchronous* query, void* data, uint32_t bytes)
{
    // S_FALSE means "not retired yet", and it is the whole reason this function exists: a single
    // GetData that answers S_FALSE leaves the caller with no data at all, and reporting the previous
    // picture's number instead is worse than reporting none.
    const double deadline = NowMs() + kQueryWaitMs;
    for (;;) {
        const HRESULT hr = m_ctx->GetData(query, data, bytes, 0);
        if (hr != S_FALSE) {
            return hr;
        }
        if (NowMs() > deadline) {
            return E_PENDING;
        }
        // Yield rather than spin: the queries normally retired before the readback Map returned, so
        // this loop is the rare path, and the encode thread may be a real-time one.
        Sleep(0);
    }
}

bool GpuEncoder::PlaceMark(uint32_t stage, uint32_t groups)
{
    if (m_markCount >= kMaxStageMarks) {
        ++m_profile.marksDropped;
        return false;
    }
    if (m_marks.size() <= m_markCount) {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ComPtr<ID3D11Query> q;
        if (FAILED(m_device->CreateQuery(&qd, &q))) {
            ++m_profile.marksDropped;
            return false;
        }
        m_marks.push_back(static_cast<ComPtr<ID3D11Query>&&>(q));
        m_markStage.push_back(0);
        m_markGroups.push_back(0);
    }
    m_markStage[m_markCount] = stage;
    m_markGroups[m_markCount] = groups;
    m_ctx->End(m_marks[m_markCount].Get());
    ++m_markCount;
    return true;
}

void GpuEncoder::BeginStageTiming()
{
    m_markCount = 0;
    m_profile = GpuStageProfile();
    m_profile.level = m_stageLevel;
    if (m_stageLevel == 0) {
        return;
    }
    // Mark 0 belongs to no dispatch, so it carries GpuStageCount and is never counted into a stage.
    PlaceMark(GpuStageCount, 0);
}

void GpuEncoder::CountDispatch(uint32_t stage, uint32_t groups)
{
    if (m_stageLevel == 0) {
        return;
    }
    ++m_profile.dispatches[stage];
    m_profile.groups[stage] += groups;
    if (m_stageLevel >= 2) {
        PlaceMark(stage, groups);
    }
}

void GpuEncoder::MarkStageEnd(uint32_t stage)
{
    if (m_stageLevel != 1 || m_profile.dispatches[stage] == 0) {
        return;
    }
    PlaceMark(stage, m_profile.groups[stage]);
}

void GpuEncoder::CollectStageTiming(const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& dj)
{
    if (m_stageLevel == 0 || m_markCount < 2 || dj.Disjoint || dj.Frequency == 0) {
        return;
    }
    std::vector<UINT64> ts(m_markCount, 0);
    for (uint32_t i = 0; i < m_markCount; ++i) {
        if (WaitForQuery(m_marks[i].Get(), &ts[i], sizeof(UINT64)) != S_OK) {
            return;
        }
        if (i != 0 && ts[i] < ts[i - 1]) {
            return;   // a clock that went backwards measures nothing; report no profile instead.
        }
    }
    const double scale = 1000.0 / static_cast<double>(dj.Frequency);
    if (m_stageLevel >= 2) {
        m_profile.steps.reserve(m_markCount - 1u);
    }
    for (uint32_t i = 1; i < m_markCount; ++i) {
        const double ms = static_cast<double>(ts[i] - ts[i - 1]) * scale;
        const uint32_t stage = m_markStage[i];
        if (stage < GpuStageCount) {
            m_profile.ms[stage] += ms;
        }
        if (m_stageLevel >= 2) {
            GpuStageStep s;
            s.stage = stage;
            s.groups = m_markGroups[i];
            s.ms = ms;
            m_profile.steps.push_back(s);
        }
    }
    m_profile.totalMs = static_cast<double>(ts[m_markCount - 1] - ts[0]) * scale;
    m_profile.valid = true;
}

HRESULT GpuEncoder::CreateRawBuffer(uint32_t bytes, bool uav, Plane* out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (bytes + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    HRESULT hr = m_device->CreateBuffer(&bd, nullptr, &out->buffer);
    if (FAILED(hr)) {
        return hr;
    }
    out->bytes = bd.ByteWidth;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    sd.BufferEx.NumElements = bd.ByteWidth / 4u;
    hr = m_device->CreateShaderResourceView(out->buffer.Get(), &sd, &out->srv);
    if (FAILED(hr)) {
        return hr;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ud.Buffer.NumElements = bd.ByteWidth / 4u;
        hr = m_device->CreateUnorderedAccessView(out->buffer.Get(), &ud, &out->uav);
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

HRESULT GpuEncoder::CreateUploadBuffer(uint32_t bytes, Plane* out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (bytes + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    HRESULT hr = m_device->CreateBuffer(&bd, nullptr, &out->buffer);
    if (FAILED(hr)) {
        return hr;
    }
    out->bytes = bd.ByteWidth;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    sd.BufferEx.NumElements = bd.ByteWidth / 4u;
    return m_device->CreateShaderResourceView(out->buffer.Get(), &sd, &out->srv);
}

HRESULT GpuEncoder::CreateStaging(uint32_t bytes, ID3D11Buffer** out)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (bytes + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return m_device->CreateBuffer(&bd, nullptr, out);
}

HRESULT GpuEncoder::CompileShaders()
{
    // The out slots are raw ID3D11ComputeShader** taken through ComPtr::operator&, which releases
    // whatever the member held: a rebuild of the shader set is therefore safe.
    struct Entry { const void* code; size_t size; ID3D11ComputeShader** out; };
    const Entry entries[] = {
        { g_CSImportNV12, sizeof(g_CSImportNV12), &m_csImportNV12 },
        { g_CSImportNV12Sys, sizeof(g_CSImportNV12Sys), &m_csImportNV12Sys },
        { g_CSImportBGRA, sizeof(g_CSImportBGRA), &m_csImportBGRA },
        { g_CSImportPlanar, sizeof(g_CSImportPlanar), &m_csImportPlanar },
        { g_CSMotionEstimate, sizeof(g_CSMotionEstimate), &m_csMotion },
        { g_CSEncodeIntra, sizeof(g_CSEncodeIntra), &m_csEncodeIntra },
        { g_CSEncodeInter, sizeof(g_CSEncodeInter), &m_csEncodeInter },
        { g_CSDeblock, sizeof(g_CSDeblock), &m_csDeblock },
        { g_CSDeblockRows, sizeof(g_CSDeblockRows), &m_csDeblockRows },
    };
    for (const Entry& e : entries) {
        HRESULT hr = m_device->CreateComputeShader(e.code, e.size, nullptr, e.out);
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
}

DeblockMode DeblockModeFromEnvironment()
{
    DeblockMode mode = DeblockMode::Waves;
    char buf[16] = {};
    if (GetEnvironmentVariableA("BC250_MFT_DEBLOCK", buf, sizeof(buf)) > 0 &&
        (buf[0] == 'r' || buf[0] == 'R')) {
        mode = DeblockMode::Rows;
    }
    if (GetEnvironmentVariableA("BC250_MFT_SERIAL_DEBLOCK", nullptr, 0) != 0) {
        mode = DeblockMode::Serial;
    }
    return mode;
}

HRESULT GpuEncoder::Initialize(ID3D11Device* device, uint32_t visibleWidth, uint32_t visibleHeight)
{
    // Odd sizes are refused, not rounded down. The coded picture is a whole number of macroblocks and
    // the SPS crops the rest, but frame_crop_right_offset counts in CropUnitX == 2 luma samples for
    // 4:2:0, so an odd visible size cannot be written into an SPS at all. Masking it down encoded a
    // picture one sample smaller than the caller asked for, with nothing saying so.
    if (!IsCodableFrameSize(visibleWidth, visibleHeight)) {
        return E_INVALIDARG;
    }
    m_visW = visibleWidth;
    m_visH = visibleHeight;
    m_widthMb = kDivUp(m_visW, 16u);
    m_heightMb = kDivUp(m_visH, 16u);
    m_padW = m_widthMb * 16u;
    m_padH = m_heightMb * 16u;

    if (device != nullptr) {
        // The client's own device. The client chose that adapter and owns the frames it gives us, so
        // this path takes the device as it is. The adapter is only recorded, and the transform
        // publishes it as MFT_ENUM_ADAPTER_LUID (mft_h264.cpp).
        m_device.CopyFrom(device);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> from;
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice),
                                               reinterpret_cast<void**>(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&from)) && SUCCEEDED(from->GetDesc(&ad))) {
            MftTrace("client device on adapter %04X:%04X%s\n",
                     ad.VendorId, ad.DeviceId,
                     (ad.VendorId == 0x1002 && ad.DeviceId == 0x13FE) ? "" : " (not the BC-250)");
        }
    } else {
        ComPtr<IDXGIFactory> factory;
        HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory),
                                       reinterpret_cast<void**>(&factory));
        if (FAILED(hr)) {
            return hr;
        }
        ComPtr<IDXGIAdapter> chosen;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter> a;
            if (factory->EnumAdapters(i, &a) != S_OK) {
                break;
            }
            DXGI_ADAPTER_DESC ad = {};
            a->GetDesc(&ad);
            if (ad.VendorId == 0x1002 && ad.DeviceId == 0x13FE) {
                chosen = static_cast<ComPtr<IDXGIAdapter>&&>(a);
                break;
            }
        }
        // The BC-250 and nothing else. This transform registers itself as a hardware encoder of
        // vendor 1002 (mft_register.cpp, MFT_ENUM_FLAG_HARDWARE), and the registration stays on the
        // computer while the driver is not started: the boot-loop guard can leave the GPU on
        // Microsoft Basic Display Adapter for a boot. An earlier version took the first adapter
        // instead, so the transform then created a device on that adapter - WARP, in the case above -
        // and Media Foundation gave a client that asks for a hardware encoder a software encode that
        // is slower than the encoder of Windows. A failure here is the answer that sends the client
        // back to the encoder it would have taken by itself. A test on a computer that has no BC-250
        // creates its own device and hands it over (tests/mfthost.h, CreateTestDevice).
        if (!chosen) {
            MftTrace("no 1002:13FE adapter: the transform does not encode on another GPU\n");
            return DXGI_ERROR_NOT_FOUND;
        }
        const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                               nullptr, 0, want, 2, D3D11_SDK_VERSION, &m_device, &got, nullptr);
        if (FAILED(hr)) {
            return hr;
        }
    }
    m_device->GetImmediateContext(&m_ctx);

    HRESULT hr = CompileShaders();
    if (FAILED(hr)) {
        return hr;
    }

    const uint32_t lumaBytes = m_padW * m_padH;
    const uint32_t chromaBytes = (m_padW / 2u) * (m_padH / 2u);
    for (int i = 0; i < 3; ++i) {
        hr = CreateRawBuffer(i == 0 ? lumaBytes : chromaBytes, true, &m_src[i]);
        if (FAILED(hr)) { return hr; }
        for (int s = 0; s < 2; ++s) {
            hr = CreateRawBuffer(i == 0 ? lumaBytes : chromaBytes, true, &m_rec[s][i]);
            if (FAILED(hr)) { return hr; }
        }
        // m_upload[1] also carries an interleaved CbCr plane, so it is twice the planar size.
        hr = CreateUploadBuffer(i == 0 ? m_visW * m_visH
                                       : (m_visW / 2u) * (m_visH / 2u) * (i == 1 ? 2u : 1u),
                                &m_upload[i]);
        if (FAILED(hr)) { return hr; }
    }
    hr = CreateRawBuffer(MbCount() * kLevelsWordsPerMb * 4u, true, &m_levels);
    if (FAILED(hr)) { return hr; }
    hr = CreateRawBuffer(MbCount() * kMbInfoWords * 4u, true, &m_mbinfo);
    if (FAILED(hr)) { return hr; }
    // The deblocking row counters: word 0 is the sentinel for the row above row 0 and holds widthMb,
    // so that row never waits, and word mby+1 counts the filtered macroblocks of row mby. Written from
    // the CPU before every deblocking pass with UpdateSubresource, which is a few hundred bytes and
    // needs nothing of the device beyond what the import path already uses.
    hr = CreateRawBuffer((m_heightMb + 1u) * 4u, true, &m_progress);
    if (FAILED(hr)) { return hr; }
    m_progressReset.assign(m_progress.bytes / 4u, 0u);
    m_progressReset[0] = m_widthMb;
    hr = CreateStaging(lumaBytes, &m_planeStaging);
    if (FAILED(hr)) { return hr; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(Constants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) { return hr; }

    // Per-stage GPU timing, read once here so that the encode path only tests an integer.
    m_stageLevel = 0;
    {
        char buf[8] = {};
        if (GetEnvironmentVariableA("BC250_MFT_STAGE_TIMING", buf, sizeof(buf)) > 0) {
            m_stageLevel = (buf[0] == '2') ? 2u : 1u;
        }
    }
    // How the deblocking wavefront is driven; see DeblockMode. Read once, for the same reason.
    m_deblockMode = DeblockModeFromEnvironment();

    // The reconstruction textures are created without initial data, so their first contents are
    // whatever Direct3D leaves there. Nothing reads them before they are written: the first picture
    // of every stream is an IDR (Encoder::EncodeFrame, and the transform arms a key frame at every
    // start of stream and after every flush), an IDR is intra coded throughout, and it writes every
    // macroblock of the reconstruction before the first P picture reads any of it.
    m_cur = 0;
    // One slot: the serial shape, which is what every caller gets until it asks for more.
    m_slots.clear();
    m_depth = 0;
    m_slotWrite = 0;
    m_slotRead = 0;
    m_inFlight = 0;
    return SetPipelineDepth(1);
}

HRESULT GpuEncoder::CreateSlot(Slot* out)
{
    HRESULT hr = CreateStaging(m_levels.bytes, &out->levelsStaging);
    if (FAILED(hr)) { return hr; }
    hr = CreateStaging(m_mbinfo.bytes, &out->mbinfoStaging);
    if (FAILED(hr)) { return hr; }
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    hr = m_device->CreateQuery(&qd, &out->tsDisjoint);
    if (FAILED(hr)) { return hr; }
    qd.Query = D3D11_QUERY_TIMESTAMP;
    hr = m_device->CreateQuery(&qd, &out->tsBegin);
    if (FAILED(hr)) { return hr; }
    hr = m_device->CreateQuery(&qd, &out->tsEnd);
    return hr;
}

HRESULT GpuEncoder::SetPipelineDepth(uint32_t depth)
{
    if (!m_device) {
        return E_UNEXPECTED;
    }
    if (m_inFlight != 0) {
        return E_NOT_VALID_STATE;
    }
    if (depth == 0) {
        depth = 1;
    }
    if (depth > kMaxPipelineDepth) {
        depth = kMaxPipelineDepth;
    }
    // The per-dispatch and per-stage marks are one array of queries, recorded by Submit and read by
    // Collect, so two pictures in flight would interleave their marks and the profile would be of
    // neither of them. A measurement run is serial by construction; it says what one picture costs,
    // not what the pipeline delivers.
    if (depth > 1 && m_stageLevel != 0) {
        MftTrace("pipeline depth %u refused: BC250_MFT_STAGE_TIMING times one picture at a time\n",
                 depth);
        depth = 1;
    }
    if (depth == m_depth) {
        return S_OK;
    }
    // Grown, never shrunk: a slot's staging buffers are 3.4 MB at 1080p and a client that walks the
    // depth up and down would otherwise free and reallocate them. The ring uses the first m_depth.
    while (m_slots.size() < depth) {
        Slot s;
        HRESULT hr = CreateSlot(&s);
        if (FAILED(hr)) {
            return hr;
        }
        m_slots.push_back(std::move(s));
    }
    m_depth = depth;
    m_slotWrite = 0;
    m_slotRead = 0;
    MftTrace("pipeline depth %u\n", m_depth);
    return S_OK;
}

void GpuEncoder::Shutdown()
{
    m_csImportNV12.Reset();
    m_csImportNV12Sys.Reset();
    m_csImportBGRA.Reset();
    m_csImportPlanar.Reset();
    m_csMotion.Reset();
    m_csEncodeIntra.Reset();
    m_csEncodeInter.Reset();
    m_csDeblock.Reset();
    m_csDeblockRows.Reset();
    for (int i = 0; i < 3; ++i) {
        m_src[i] = Plane();
        m_upload[i] = Plane();
        for (int s = 0; s < 2; ++s) {
            m_rec[s][i] = Plane();
        }
    }
    m_levels = Plane();
    m_mbinfo = Plane();
    m_progress = Plane();
    m_progressReset.clear();
    m_slots.clear();
    m_depth = 0;
    m_slotWrite = 0;
    m_slotRead = 0;
    m_inFlight = 0;
    m_planeStaging.Reset();
    m_cb.Reset();
    m_marks.clear();
    m_markStage.clear();
    m_markGroups.clear();
    m_markCount = 0;
    m_ctx.Reset();
    m_device.Reset();
    // The last picture's measurements belong to the device that produced them.
    m_lastQueryWaitMs = 0.0;
    m_lastGpuMs = 0.0;
    m_lastReadbackMs = 0.0;
    m_lastRecordMs = 0.0;
    m_lastMapWaitMs = 0.0;
    m_lastGpuMsValid = false;
    m_profile = GpuStageProfile();
}

void GpuEncoder::UpdateConstants(const GpuFrameParams& p, uint32_t diagonal, uint32_t diagonalBase)
{
    Constants c = {};
    c.widthMb = m_widthMb;
    c.heightMb = m_heightMb;
    c.padW = m_padW;
    c.padH = m_padH;
    c.qpY = p.qpY;
    c.qpC = p.qpC;
    c.isIntra = p.intra ? 1u : 0u;
    c.diagonalBase = diagonalBase;
    c.diagonal = diagonal;
    c.lambda = p.lambda;
    c.skipBias = p.skipBias;
    c.srcWidth = m_visW;
    c.srcHeight = m_visH;
    c.alphaOffsetDiv2 = p.alphaOffsetDiv2;
    c.betaOffsetDiv2 = p.betaOffsetDiv2;
    c.deblockIdc = p.deblockIdc;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof(c));
        m_ctx->Unmap(m_cb.Get(), 0);
    }
}

HRESULT GpuEncoder::UploadPlanar(const GpuFrameInput& in)
{
    const uint32_t cw = m_visW / 2u;
    const uint32_t ch = m_visH / 2u;
    const bool nv12 = (in.kind == InputKind::Nv12Sys);
    const struct { const uint8_t* src; uint32_t pitch; uint32_t w; uint32_t h; } planes[3] = {
        { in.planeY, in.pitchY ? in.pitchY : m_visW, m_visW, m_visH },
        { in.planeCb, in.pitchC ? in.pitchC : (nv12 ? cw * 2u : cw), nv12 ? cw * 2u : cw, ch },
        { nv12 ? nullptr : in.planeCr, in.pitchC ? in.pitchC : cw, cw, ch },
    };
    for (int i = 0; i < (nv12 ? 2 : 3); ++i) {
        if (planes[i].src == nullptr) {
            return E_POINTER;
        }
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr = m_ctx->Map(m_upload[i].buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) {
            return hr;
        }
        uint8_t* dst = static_cast<uint8_t*>(m.pData);
        for (uint32_t y = 0; y < planes[i].h; ++y) {
            memcpy(dst + static_cast<size_t>(y) * planes[i].w,
                   planes[i].src + static_cast<size_t>(y) * planes[i].pitch, planes[i].w);
        }
        m_ctx->Unmap(m_upload[i].buffer.Get(), 0);
    }
    return S_OK;
}

HRESULT GpuEncoder::ShaderReadableTexture(Slot& slot, ID3D11Texture2D* src, bool nv12,
                                          uint32_t slice, ID3D11Texture2D** out)
{
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    const uint32_t srcMips = sd.MipLevels ? sd.MipLevels : 1u;
    if (slice >= sd.ArraySize) {
        return MF_E_UNSUPPORTED_D3D_TYPE;
    }
    // A plain, shader bindable, single slice texture is read in place: no copy at all, which is the
    // point of MF_SA_D3D11_AWARE. The compute import pass binds a TEXTURE2D view, which cannot
    // address a slice, so every other shape is copied into a private single slice texture first.
    if ((sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 && sd.SampleDesc.Count <= 1 &&
        sd.ArraySize == 1 && slice == 0) {
        *out = src;
        return S_OK;
    }
    ComPtr<ID3D11Texture2D>& own = nv12 ? slot.ownNv12 : slot.ownBgra;
    if (!own) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = m_visW;
        td.Height = m_visH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &own);
        if (FAILED(hr)) {
            return hr;
        }
    }
    // NV12 is a planar resource: its luma and chroma planes are separate subresources and have to be
    // copied one by one. D3D11 numbers them plane * MipLevels * ArraySize + slice * MipLevels + mip
    // (d3d11.h plane slice rules, learn.microsoft.com "Subresources"), and the box coordinates of a
    // planar copy are in the texels of the plane, so the chroma box is half the size in both axes.
    // Copying only subresource 0, as this function used to, left the chroma plane of an NV12 input
    // uninitialised; no test covered it because only BGRA textures were exercised.
    const uint32_t planes = nv12 ? 2u : 1u;
    for (uint32_t plane = 0; plane < planes; ++plane) {
        D3D11_BOX box = {};
        box.right = plane ? m_visW / 2u : m_visW;
        box.bottom = plane ? m_visH / 2u : m_visH;
        box.back = 1;
        const uint32_t srcSub = plane * srcMips * sd.ArraySize + slice * srcMips;
        m_ctx->CopySubresourceRegion(own.Get(), plane, 0, 0, 0, src, srcSub, &box);
    }
    *out = own.Get();
    return S_OK;
}

HRESULT GpuEncoder::UploadBgraSystem(Slot& slot, const uint8_t* rgb, uint32_t pitch,
                                     ID3D11Texture2D** out)
{
    if (rgb == nullptr) {
        return E_POINTER;
    }
    ComPtr<ID3D11Texture2D>& own = slot.ownBgra;
    if (!own) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = m_visW;
        td.Height = m_visH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &own);
        if (FAILED(hr)) {
            return hr;
        }
    }
    m_ctx->UpdateSubresource(own.Get(), 0, nullptr, rgb, pitch, 0);
    *out = own.Get();
    return S_OK;
}

HRESULT GpuEncoder::Submit(const GpuFrameInput& in, const GpuFrameParams& p)
{
    if (m_inFlight >= m_depth) {
        return E_NOT_VALID_STATE;
    }
    Slot& slot = m_slots[m_slotWrite];

    // Six slots: u0..u2 reconstruction, u3 levels, u4 macroblock info, u5 the deblocking row counters.
    ID3D11UnorderedAccessView* nullUavs[6] = {};
    ID3D11ShaderResourceView* nullSrvs[13] = {};

    const double record0 = NowMs();
    m_ctx->Begin(slot.tsDisjoint.Get());
    m_ctx->End(slot.tsBegin.Get());
    BeginStageTiming();
    slot.written = m_cur;

    // Every early return below leaves the device as Submit found it. The slot is not advanced on a
    // failure, so the next Submit reuses it, and an interval left open would make that Begin a debug
    // layer error on a query already begun; the compute bindings would stay bound to buffers the
    // caller is free to release. The disjoint interval is closed, the end timestamp is written so the
    // slot keeps three readable queries, and the bindings are dropped.
    struct Abandon {
        GpuEncoder* self;
        Slot* slot;
        bool armed = true;
        ~Abandon()
        {
            if (!armed) {
                return;
            }
            ID3D11UnorderedAccessView* none[6] = {};
            ID3D11ShaderResourceView* noSrv[13] = {};
            self->m_ctx->CSSetUnorderedAccessViews(0, 6, none, nullptr);
            self->m_ctx->CSSetShaderResources(0, 13, noSrv);
            self->m_ctx->End(slot->tsEnd.Get());
            self->m_ctx->End(slot->tsDisjoint.Get());
        }
    } abandon{ this, &slot };

    // ---- import --------------------------------------------------------------------------------
    UpdateConstants(p, 0, 0);
    ID3D11Buffer* cbs[1] = { m_cb.Get() };
    m_ctx->CSSetConstantBuffers(0, 1, cbs);

    {
        ID3D11UnorderedAccessView* uavs[3] = { m_src[0].uav.Get(), m_src[1].uav.Get(),
                                              m_src[2].uav.Get() };
        m_ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        if (in.kind == InputKind::Planar8 || in.kind == InputKind::Nv12Sys) {
            HRESULT hr = UploadPlanar(in);
            if (FAILED(hr)) {
                return hr;
            }
            ID3D11ShaderResourceView* srvs[3] = { m_upload[0].srv.Get(), m_upload[1].srv.Get(),
                                                  m_upload[2].srv.Get() };
            m_ctx->CSSetShaderResources(3, 3, srvs);
            m_ctx->CSSetShader(in.kind == InputKind::Nv12Sys ? m_csImportNV12Sys.Get()
                                                             : m_csImportPlanar.Get(),
                               nullptr, 0);
        } else if (in.kind == InputKind::TextureBGRA || in.kind == InputKind::BgraSys) {
            ID3D11Texture2D* tex = nullptr;
            HRESULT hr = (in.kind == InputKind::BgraSys)
                             ? UploadBgraSystem(slot, in.rgb, in.pitchRgb, &tex)
                             : ShaderReadableTexture(slot, in.texture, false, in.slice, &tex);
            if (FAILED(hr)) {
                return hr;
            }
            ComPtr<ID3D11ShaderResourceView> srv;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            hr = m_device->CreateShaderResourceView(tex, &sd, &srv);
            if (FAILED(hr)) {
                return hr;
            }
            ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
            m_ctx->CSSetShaderResources(2, 1, srvs);
            m_ctx->CSSetShader(m_csImportBGRA.Get(), nullptr, 0);
        } else {
            ID3D11Texture2D* tex = nullptr;
            HRESULT hr = ShaderReadableTexture(slot, in.texture, true, in.slice, &tex);
            if (FAILED(hr)) {
                return hr;
            }
            // Direct3D 11 derives the plane from the view format on a planar resource: R8_UNORM is
            // the luma plane of an NV12 texture, R8G8_UNORM the interleaved chroma plane.
            ComPtr<ID3D11ShaderResourceView> srvY;
            ComPtr<ID3D11ShaderResourceView> srvUV;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            sd.Format = DXGI_FORMAT_R8_UNORM;
            hr = m_device->CreateShaderResourceView(tex, &sd, &srvY);
            if (SUCCEEDED(hr)) {
                sd.Format = DXGI_FORMAT_R8G8_UNORM;
                hr = m_device->CreateShaderResourceView(tex, &sd, &srvUV);
            }
            if (FAILED(hr)) {
                return hr;
            }
            ID3D11ShaderResourceView* srvs[2] = { srvY.Get(), srvUV.Get() };
            m_ctx->CSSetShaderResources(0, 2, srvs);
            m_ctx->CSSetShader(m_csImportNV12.Get(), nullptr, 0);
        }
        const uint32_t gx = kDivUp(kDivUp(m_padW, 8u), 8u);
        const uint32_t gy = kDivUp(kDivUp(m_padH, 2u), 8u);
        m_ctx->Dispatch(gx, gy, 1);
        CountDispatch(GpuStageImport, gx * gy);
        MarkStageEnd(GpuStageImport);
        m_ctx->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
        m_ctx->CSSetShaderResources(0, 13, nullSrvs);
    }

    // ---- motion estimation and macroblock pass -------------------------------------------------
    {
        ID3D11UnorderedAccessView* uavs[5] = {
            m_rec[m_cur][0].uav.Get(), m_rec[m_cur][1].uav.Get(), m_rec[m_cur][2].uav.Get(),
            m_levels.uav.Get(), m_mbinfo.uav.Get()
        };
        ID3D11ShaderResourceView* srvs[6] = {
            m_src[0].srv.Get(), m_src[1].srv.Get(), m_src[2].srv.Get(),
            m_rec[1u - m_cur][0].srv.Get(), m_rec[1u - m_cur][1].srv.Get(),
            m_rec[1u - m_cur][2].srv.Get()
        };
        m_ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
        m_ctx->CSSetShaderResources(6, 6, srvs);

        if (p.intra) {
            const uint32_t diagonals = m_widthMb + m_heightMb - 1u;
            for (uint32_t d = 0; d < diagonals; ++d) {
                const uint32_t base = (d >= m_heightMb) ? (d - m_heightMb + 1u) : 0u;
                const uint32_t last = (d < m_widthMb - 1u) ? d : (m_widthMb - 1u);
                UpdateConstants(p, d, base);
                m_ctx->CSSetShader(m_csEncodeIntra.Get(), nullptr, 0);
                m_ctx->Dispatch(last - base + 1u, 1, 1);
                CountDispatch(GpuStageMode, last - base + 1u);
            }
            MarkStageEnd(GpuStageMode);
        } else {
            m_ctx->CSSetShader(m_csMotion.Get(), nullptr, 0);
            m_ctx->Dispatch(m_widthMb, m_heightMb, 1);
            CountDispatch(GpuStageMotion, m_widthMb * m_heightMb);
            MarkStageEnd(GpuStageMotion);
            m_ctx->CSSetShader(m_csEncodeInter.Get(), nullptr, 0);
            m_ctx->Dispatch(m_widthMb, m_heightMb, 1);
            CountDispatch(GpuStageMode, m_widthMb * m_heightMb);
            MarkStageEnd(GpuStageMode);
        }

        // ---- deblocking ------------------------------------------------------------------------
        if (p.deblockIdc != 1u) {
            // The counters the single-dispatch wavefront waits on, reset before the pass. Recorded in
            // the command stream, so it executes before the dispatch, and done before the UAV is bound
            // so that the runtime does not have to unbind it.
            if (m_deblockMode == DeblockMode::Rows) {
                m_ctx->UpdateSubresource(m_progress.buffer.Get(), 0, nullptr,
                                         m_progressReset.data(), 0, 0);
            }
            ID3D11ShaderResourceView* mb[1] = { m_mbinfo.srv.Get() };
            ID3D11UnorderedAccessView* duav[6] = {
                m_rec[m_cur][0].uav.Get(), m_rec[m_cur][1].uav.Get(), m_rec[m_cur][2].uav.Get(),
                nullptr, nullptr, m_progress.uav.Get()
            };
            m_ctx->CSSetUnorderedAccessViews(0, 6, duav, nullptr);
            m_ctx->CSSetShaderResources(12, 1, mb);
            // t = mbx + 2 * mby, see the header of cs_deblock.hlsl: the filter writes into its left
            // and above neighbours, so the anti-diagonal of the intra pass is not a legal order here.
            // CSDeblockRows walks that schedule inside one dispatch, one thread group per macroblock
            // row, which is what this stage costs on a GPU where the launch is the cost. The two other
            // modes keep the dispatch boundary as the synchronisation: Waves is one dispatch per value
            // of t, holding the macroblock rows mby with 0 <= t - 2*mby <= widthMb - 1, and Serial is
            // one macroblock per dispatch in raster order, which is clause 8.7 read literally and is
            // only for telling a schedule defect from a filter or bS defect (2400 dispatches at
            // 640x480).
            if (m_deblockMode == DeblockMode::Rows) {
                UpdateConstants(p, 0, 0);
                m_ctx->CSSetShader(m_csDeblockRows.Get(), nullptr, 0);
                m_ctx->Dispatch(m_heightMb, 1, 1);
                CountDispatch(GpuStageDeblock, m_heightMb);
            } else if (m_deblockMode == DeblockMode::Serial) {
                for (uint32_t my = 0; my < m_heightMb; ++my) {
                    for (uint32_t mx = 0; mx < m_widthMb; ++mx) {
                        UpdateConstants(p, mx + 2u * my, my);
                        m_ctx->CSSetShader(m_csDeblock.Get(), nullptr, 0);
                        m_ctx->Dispatch(1, 1, 1);
                        CountDispatch(GpuStageDeblock, 1);
                    }
                }
            } else {
                const uint32_t waves = DeblockWaveCount(m_widthMb, m_heightMb);
                for (uint32_t t = 0; t <= waves; ++t) {
                    const DeblockWave wave = DeblockWaveRows(t, m_widthMb, m_heightMb);
                    if (!wave.any) {
                        continue;
                    }
                    UpdateConstants(p, t, wave.first);
                    m_ctx->CSSetShader(m_csDeblock.Get(), nullptr, 0);
                    m_ctx->Dispatch(wave.last - wave.first + 1u, 1, 1);
                    CountDispatch(GpuStageDeblock, wave.last - wave.first + 1u);
                }
            }
            MarkStageEnd(GpuStageDeblock);
        }

        m_ctx->CSSetUnorderedAccessViews(0, 6, nullUavs, nullptr);
        m_ctx->CSSetShaderResources(0, 13, nullSrvs);
    }

    m_ctx->End(slot.tsEnd.Get());
    m_ctx->End(slot.tsDisjoint.Get());
    abandon.armed = false;

    // ---- capture the result ----------------------------------------------------------------------
    //
    // Recorded here rather than waited for here, and that is the whole of the pipelining: the two
    // copies take this picture's levels and macroblock info into this slot's staging buffers, and
    // because the device executes the stream in order they run before the next picture's dispatches
    // overwrite the shared buffers. Collect maps the staging copy, so the next picture can be recorded
    // and started while the CPU still holds this one's levels.
    m_ctx->CopyResource(slot.levelsStaging.Get(), m_levels.buffer.Get());
    m_ctx->CopyResource(slot.mbinfoStaging.Get(), m_mbinfo.buffer.Get());

    // One picture, one submission. Without this the runtime keeps recording into the same command
    // buffer until something forces it out, which is the Map in Collect, and this driver's Map waits
    // for the whole buffer that holds the copy rather than for the copy itself: at depth 2 the first
    // measured pipeline was slower than the serial shape (6.37 against 4.88 ms per 1080p picture) with
    // the map wait unchanged at 2.3 ms, because every wait covered two pictures of GPU work. Flushing
    // here also starts this picture at once instead of when the next Collect asks for the one before
    // it, which is the point of recording ahead. At depth 1 the Map is the flush and this would only
    // add a kernel transition, so the serial path is left exactly as it was.
    if (m_depth > 1) {
        m_ctx->Flush();
    }
    slot.recordMs = NowMs() - record0;
    // The reconstruction this picture wrote is the next picture's reference. It was GpuEncoder's
    // caller that used to do this after the entropy coding; it belongs here, because at a depth above
    // one the next picture is recorded before the entropy coding of this one has even started.
    m_cur = 1u - m_cur;
    m_slotWrite = (m_slotWrite + 1u) % m_depth;
    ++m_inFlight;
    return S_OK;
}

HRESULT GpuEncoder::Collect(std::vector<uint32_t>& levels, std::vector<MbInfo>& info)
{
    if (m_inFlight == 0) {
        return E_NOT_VALID_STATE;
    }
    // The slot is given back on every exit path, a failed Map included. A Collect that returned
    // without advancing the ring left the slot counted as in flight for ever, and the caller had
    // already taken the picture off its own list, so the two counts never agreed again: at a depth of
    // two the next two Submit calls filled the ring and every picture after that was refused with
    // E_NOT_VALID_STATE, which no flush could clear because the flush drains the caller's list and the
    // caller's list was one short. A device removal is recoverable; an encoder object that can never
    // encode again is not.
    struct ReleaseSlot {
        GpuEncoder* self;
        ~ReleaseSlot()
        {
            self->m_slotRead = (self->m_slotRead + 1u) % self->m_depth;
            --self->m_inFlight;
        }
    } releaseSlot{ this };

    Slot& slot = m_slots[m_slotRead];
    m_lastWritten = slot.written;
    m_lastRecordMs = slot.recordMs;

    // ---- read back the levels and the macroblock info -------------------------------------------
    //
    // Its own timed stage. The copies were recorded by Submit; the first Map blocks until they have
    // executed on the GPU, and the two memcpy calls move the result into the caller's storage. So this
    // block holds the whole wait for the GPU plus the cost of the transfer, and none of the entropy
    // coding that follows it: without it the frame level code could only report "GPU" and "CPU" and
    // the wait would hide inside the GPU number.
    const double readback0 = NowMs();
    levels.resize(static_cast<size_t>(MbCount()) * kLevelsWordsPerMb);
    info.resize(MbCount());
    D3D11_MAPPED_SUBRESOURCE m = {};
    // The one blocking call of the picture: the copy into this slot has to have executed before the
    // levels are readable, so this is the CPU's wait for the GPU and the rest of the stage is
    // transfer. Timed on its own because the two are what the pipeline separates: at a depth above one
    // this wait is what the entropy coding of the picture before it was hiding.
    const double map0 = NowMs();
    HRESULT hr = m_ctx->Map(slot.levelsStaging.Get(), 0, D3D11_MAP_READ, 0, &m);
    m_lastMapWaitMs = NowMs() - map0;
    if (FAILED(hr)) {
        return hr;
    }
    memcpy(levels.data(), m.pData, levels.size() * 4u);
    m_ctx->Unmap(slot.levelsStaging.Get(), 0);
    hr = m_ctx->Map(slot.mbinfoStaging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        return hr;
    }
    memcpy(info.data(), m.pData, info.size() * sizeof(MbInfo));
    m_ctx->Unmap(slot.mbinfoStaging.Get(), 0);
    m_lastReadbackMs = NowMs() - readback0;

    // ---- the GPU timing of this picture ---------------------------------------------------------
    //
    // Cleared first, then read to completion. The earlier version took whatever a single GetData
    // happened to have ready and left m_lastGpuMs untouched on S_FALSE, so a picture whose query had
    // not retired reported the previous picture's GPU time as its own, and a profile could not tell
    // a fast picture from an unavailable measurement. A disjoint interval is thrown away for the
    // same reason: the clock changed inside it, so the difference means nothing.
    m_lastGpuMs = 0.0;
    m_lastGpuMsValid = false;
    const double query0 = NowMs();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    UINT64 gpu0 = 0, gpu1 = 0;
    const bool haveDisjoint = (WaitForQuery(slot.tsDisjoint.Get(), &dj, sizeof(dj)) == S_OK);
    if (haveDisjoint &&
        WaitForQuery(slot.tsBegin.Get(), &gpu0, sizeof(gpu0)) == S_OK &&
        WaitForQuery(slot.tsEnd.Get(), &gpu1, sizeof(gpu1)) == S_OK &&
        !dj.Disjoint && dj.Frequency != 0 && gpu1 >= gpu0) {
        m_lastGpuMs = 1000.0 * static_cast<double>(gpu1 - gpu0) / static_cast<double>(dj.Frequency);
        m_lastGpuMsValid = true;
    }
    if (haveDisjoint) {
        CollectStageTiming(dj);
    }
    m_lastQueryWaitMs = NowMs() - query0;
    // One less than m_inFlight: the slot is given back by releaseSlot when this function returns.
    MftTrace("gpu picture: record %.3f ms, readback %.3f ms (map wait %.3f), query wait %.3f ms, "
             "gpu %.3f ms (valid %d), %u still in flight\n", m_lastRecordMs, m_lastReadbackMs,
             m_lastMapWaitMs, m_lastQueryWaitMs, m_lastGpuMs, m_lastGpuMsValid ? 1 : 0,
             m_inFlight - 1u);
    return S_OK;
}

HRESULT GpuEncoder::EncodeFrame(const GpuFrameInput& in, const GpuFrameParams& p,
                                std::vector<uint32_t>& levels, std::vector<MbInfo>& info)
{
    HRESULT hr = Submit(in, p);
    if (FAILED(hr)) {
        return hr;
    }
    return Collect(levels, info);
}

HRESULT GpuEncoder::ReadPlanes(Plane* y, Plane* cb, Plane* cr, std::vector<uint8_t>& oy,
                               std::vector<uint8_t>& ocb, std::vector<uint8_t>& ocr)
{
    Plane* src[3] = { y, cb, cr };
    std::vector<uint8_t>* dst[3] = { &oy, &ocb, &ocr };
    const uint32_t sizes[3] = { m_padW * m_padH, (m_padW / 2u) * (m_padH / 2u),
                                (m_padW / 2u) * (m_padH / 2u) };
    for (int i = 0; i < 3; ++i) {
        D3D11_BOX box = {};
        box.right = sizes[i];
        box.bottom = 1;
        box.back = 1;
        m_ctx->CopySubresourceRegion(m_planeStaging.Get(), 0, 0, 0, 0, src[i]->buffer.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr = m_ctx->Map(m_planeStaging.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) {
            return hr;
        }
        dst[i]->resize(sizes[i]);
        memcpy(dst[i]->data(), m.pData, sizes[i]);
        m_ctx->Unmap(m_planeStaging.Get(), 0);
    }
    return S_OK;
}

HRESULT GpuEncoder::ReadReconstruction(std::vector<uint8_t>& y, std::vector<uint8_t>& cb,
                                       std::vector<uint8_t>& cr)
{
    // There are two reconstruction buffers, which is enough for the collected picture's own
    // reconstruction to still be intact at a depth of 2 and not above it: at a depth of 3 picture k+2
    // is submitted before picture k is collected and overwrites the buffer m_lastWritten names, so this
    // would hand back another picture's samples with no error. Refused instead.
    if (m_depth > 2) {
        return E_NOT_VALID_STATE;
    }
    // m_lastWritten, not m_cur: a caller reads the reconstruction after a picture was collected, and
    // Submit has already moved m_cur on to the buffer the next picture writes.
    Plane* p = m_rec[m_lastWritten];
    return ReadPlanes(&p[0], &p[1], &p[2], y, cb, cr);
}

HRESULT GpuEncoder::ReadSource(std::vector<uint8_t>& y, std::vector<uint8_t>& cb,
                               std::vector<uint8_t>& cr)
{
    return ReadPlanes(&m_src[0], &m_src[1], &m_src[2], y, cb, cr);
}

} // namespace bc250h264
