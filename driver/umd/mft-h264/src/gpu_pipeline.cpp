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

} // namespace

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
    };
    for (const Entry& e : entries) {
        HRESULT hr = m_device->CreateComputeShader(e.code, e.size, nullptr, e.out);
        if (FAILED(hr)) {
            return hr;
        }
    }
    return S_OK;
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
        m_device.CopyFrom(device);
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
            if (!chosen) {
                chosen = static_cast<ComPtr<IDXGIAdapter>&&>(a);
            }
        }
        const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(chosen.Get(),
                               chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
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
    hr = CreateStaging(m_levels.bytes, &m_levelsStaging);
    if (FAILED(hr)) { return hr; }
    hr = CreateStaging(m_mbinfo.bytes, &m_mbinfoStaging);
    if (FAILED(hr)) { return hr; }
    hr = CreateStaging(lumaBytes, &m_planeStaging);
    if (FAILED(hr)) { return hr; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(Constants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbd, nullptr, &m_cb);
    if (FAILED(hr)) { return hr; }

    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    m_device->CreateQuery(&qd, &m_tsDisjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    m_device->CreateQuery(&qd, &m_tsBegin);
    m_device->CreateQuery(&qd, &m_tsEnd);

    // The reconstruction textures are created without initial data, so their first contents are
    // whatever Direct3D leaves there. Nothing reads them before they are written: the first picture
    // of every stream is an IDR (Encoder::EncodeFrame, and the transform arms a key frame at every
    // start of stream and after every flush), an IDR is intra coded throughout, and it writes every
    // macroblock of the reconstruction before the first P picture reads any of it.
    m_cur = 0;
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
    for (int i = 0; i < 3; ++i) {
        m_src[i] = Plane();
        m_upload[i] = Plane();
        for (int s = 0; s < 2; ++s) {
            m_rec[s][i] = Plane();
        }
    }
    m_levels = Plane();
    m_mbinfo = Plane();
    m_levelsStaging.Reset();
    m_mbinfoStaging.Reset();
    m_planeStaging.Reset();
    m_ownNv12.Reset();
    m_ownBgra.Reset();
    m_cb.Reset();
    m_tsDisjoint.Reset();
    m_tsBegin.Reset();
    m_tsEnd.Reset();
    m_ctx.Reset();
    m_device.Reset();
    // The last picture's measurements belong to the device that produced them.
    m_lastQueryWaitMs = 0.0;
    m_lastGpuMs = 0.0;
    m_lastReadbackMs = 0.0;
    m_lastGpuMsValid = false;
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

HRESULT GpuEncoder::ShaderReadableTexture(ID3D11Texture2D* src, bool nv12, uint32_t slice,
                                          ID3D11Texture2D** out)
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
    ComPtr<ID3D11Texture2D>& own = nv12 ? m_ownNv12 : m_ownBgra;
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

HRESULT GpuEncoder::UploadBgraSystem(const uint8_t* rgb, uint32_t pitch, ID3D11Texture2D** out)
{
    if (rgb == nullptr) {
        return E_POINTER;
    }
    if (!m_ownBgra) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = m_visW;
        td.Height = m_visH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_ownBgra);
        if (FAILED(hr)) {
            return hr;
        }
    }
    m_ctx->UpdateSubresource(m_ownBgra.Get(), 0, nullptr, rgb, pitch, 0);
    *out = m_ownBgra.Get();
    return S_OK;
}

HRESULT GpuEncoder::EncodeFrame(const GpuFrameInput& in, const GpuFrameParams& p,
                                std::vector<uint32_t>& levels, std::vector<MbInfo>& info)
{
    ID3D11UnorderedAccessView* nullUavs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    ID3D11ShaderResourceView* nullSrvs[13] = {};

    m_ctx->Begin(m_tsDisjoint.Get());
    m_ctx->End(m_tsBegin.Get());
    m_lastWritten = m_cur;

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
                             ? UploadBgraSystem(in.rgb, in.pitchRgb, &tex)
                             : ShaderReadableTexture(in.texture, false, in.slice, &tex);
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
            HRESULT hr = ShaderReadableTexture(in.texture, true, in.slice, &tex);
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
        m_ctx->Dispatch(kDivUp(kDivUp(m_padW, 8u), 8u), kDivUp(kDivUp(m_padH, 2u), 8u), 1);
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
            }
        } else {
            m_ctx->CSSetShader(m_csMotion.Get(), nullptr, 0);
            m_ctx->Dispatch(m_widthMb, m_heightMb, 1);
            m_ctx->CSSetShader(m_csEncodeInter.Get(), nullptr, 0);
            m_ctx->Dispatch(m_widthMb, m_heightMb, 1);
        }

        // ---- deblocking ------------------------------------------------------------------------
        if (p.deblockIdc != 1u) {
            ID3D11ShaderResourceView* mb[1] = { m_mbinfo.srv.Get() };
            ID3D11UnorderedAccessView* duav[5] = {
                m_rec[m_cur][0].uav.Get(), m_rec[m_cur][1].uav.Get(), m_rec[m_cur][2].uav.Get(),
                nullptr, nullptr
            };
            m_ctx->CSSetUnorderedAccessViews(0, 5, duav, nullptr);
            m_ctx->CSSetShaderResources(12, 1, mb);
            // t = mbx + 2 * mby, see the header of cs_deblock.hlsl: the filter writes into its left
            // and above neighbours, so the anti-diagonal of the intra pass is not a legal order here.
            // One wave holds the macroblock rows mby with 0 <= t - 2*mby <= widthMb - 1.
            // Diagnostic: one macroblock per dispatch, in raster order, which is clause 8.7 read
            // literally. Only for telling a wavefront defect from a filter or bS defect; it costs one
            // dispatch per macroblock (2400 for 640x480).
            if (GetEnvironmentVariableA("BC250_MFT_SERIAL_DEBLOCK", nullptr, 0) != 0) {
                for (uint32_t my = 0; my < m_heightMb; ++my) {
                    for (uint32_t mx = 0; mx < m_widthMb; ++mx) {
                        UpdateConstants(p, mx + 2u * my, my);
                        m_ctx->CSSetShader(m_csDeblock.Get(), nullptr, 0);
                        m_ctx->Dispatch(1, 1, 1);
                    }
                }
            } else {
                const uint32_t waves = m_widthMb + 2u * m_heightMb - 2u;
                for (uint32_t t = 0; t <= waves; ++t) {
                    const uint32_t deficit = (t + 1u > m_widthMb) ? (t + 1u - m_widthMb) : 0u;
                    const uint32_t first = (deficit + 1u) / 2u;
                    const uint32_t lastRow = (t / 2u < m_heightMb - 1u) ? (t / 2u) : (m_heightMb - 1u);
                    if (first > lastRow) {
                        continue;
                    }
                    UpdateConstants(p, t, first);
                    m_ctx->CSSetShader(m_csDeblock.Get(), nullptr, 0);
                    m_ctx->Dispatch(lastRow - first + 1u, 1, 1);
                }
            }
        }

        m_ctx->CSSetUnorderedAccessViews(0, 5, nullUavs, nullptr);
        m_ctx->CSSetShaderResources(0, 13, nullSrvs);
    }

    m_ctx->End(m_tsEnd.Get());
    m_ctx->End(m_tsDisjoint.Get());

    // ---- read back the levels and the macroblock info -------------------------------------------
    //
    // Its own timed stage. The two CopyResource calls queue the copies, the first Map blocks until
    // everything queued for this picture has executed on the GPU, and the two memcpy calls move the
    // result into the caller's storage. So this block holds the whole wait for the GPU plus the cost
    // of the transfer, and none of the entropy coding that follows it: without it the frame level
    // code could only report "GPU" and "CPU" and the wait would hide inside the GPU number.
    const double readback0 = NowMs();
    m_ctx->CopyResource(m_levelsStaging.Get(), m_levels.buffer.Get());
    m_ctx->CopyResource(m_mbinfoStaging.Get(), m_mbinfo.buffer.Get());

    levels.resize(static_cast<size_t>(MbCount()) * kLevelsWordsPerMb);
    info.resize(MbCount());
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = m_ctx->Map(m_levelsStaging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        return hr;
    }
    memcpy(levels.data(), m.pData, levels.size() * 4u);
    m_ctx->Unmap(m_levelsStaging.Get(), 0);
    hr = m_ctx->Map(m_mbinfoStaging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        return hr;
    }
    memcpy(info.data(), m.pData, info.size() * sizeof(MbInfo));
    m_ctx->Unmap(m_mbinfoStaging.Get(), 0);
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
    if (WaitForQuery(m_tsDisjoint.Get(), &dj, sizeof(dj)) == S_OK &&
        WaitForQuery(m_tsBegin.Get(), &gpu0, sizeof(gpu0)) == S_OK &&
        WaitForQuery(m_tsEnd.Get(), &gpu1, sizeof(gpu1)) == S_OK &&
        !dj.Disjoint && dj.Frequency != 0 && gpu1 >= gpu0) {
        m_lastGpuMs = 1000.0 * static_cast<double>(gpu1 - gpu0) / static_cast<double>(dj.Frequency);
        m_lastGpuMsValid = true;
    }
    m_lastQueryWaitMs = NowMs() - query0;
    MftTrace("gpu picture: readback %.3f ms, query wait %.3f ms, gpu %.3f ms (valid %d)\n",
             m_lastReadbackMs, m_lastQueryWaitMs, m_lastGpuMs, m_lastGpuMsValid ? 1 : 0);
    return S_OK;
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
    // m_lastWritten, not m_cur: a caller reads the reconstruction after EncodeFrame, and the frame
    // level code calls SwapReference in between, which moves m_cur to the other buffer.
    Plane* p = m_rec[m_lastWritten];
    return ReadPlanes(&p[0], &p[1], &p[2], y, cb, cr);
}

HRESULT GpuEncoder::ReadSource(std::vector<uint8_t>& y, std::vector<uint8_t>& cb,
                               std::vector<uint8_t>& cr)
{
    return ReadPlanes(&m_src[0], &m_src[1], &m_src[2], y, cb, cr);
}

void GpuEncoder::SwapReference()
{
    m_cur = 1u - m_cur;
}

} // namespace bc250h264
