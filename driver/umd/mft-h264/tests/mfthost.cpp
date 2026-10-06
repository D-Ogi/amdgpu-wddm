// SPDX-License-Identifier: MIT
// Host test for the BC-250 H.264 encoder MFT. Nothing here registers anything for the machine: the
// Media Foundation pipeline tests use MFTRegisterLocal, which lives and dies with the process.
//
//   mfthost.exe --selftest       table structure and bit writer, no GPU, no Media Foundation
//   mfthost.exe --encode         encode a synthetic sequence and check it against the inbox decoder
//   mfthost.exe --mft            the same through the asynchronous transform interface
//   mfthost.exe --compare        our encoder against the inbox H264 Encoder MFT at equal settings
//   mfthost.exe --sinkwriter     a Media Foundation sink writer run to an .mp4 with our MFT forced
//   mfthost.exe --all            all of the above
//
// Options: --width --height --frames --qp --bitrate --gop --fps --deblock --gpu-source --nv12-sys
//          --still --out <dir>
//          --cbr (default is constant quantiser, which is what a bit-exactness run wants)

#include "mfthost.h"
#include "h264_tables.h"
#include "bitwriter.h"
#include "gen/vs_fullscreen.h"
#include "gen/ps_testpattern.h"
#include <mfapi.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace bc250h264 {
namespace test {

double NowMs()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

void Picture::Allocate(uint32_t w, uint32_t h)
{
    width = w;
    height = h;
    y.assign(static_cast<size_t>(w) * h, 0);
    cb.assign(static_cast<size_t>(w / 2) * (h / 2), 0);
    cr.assign(static_cast<size_t>(w / 2) * (h / 2), 0);
}

void MakeSyntheticPicture(Picture& p, uint32_t frame)
{
    const int f = static_cast<int>(frame);
    const int w = static_cast<int>(p.width);
    const int h = static_cast<int>(p.height);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int v = (x + y + f * 3) & 255;
            const int barX = (x + f) % 64;
            if (y > 80 && y < 200 && barX < 24) {
                v = 240;
            }
            const int barY = (y - f + 1024) % 48;
            if (x > 300 && x < 700 && barY < 6) {
                v = 16;
            }
            if (x >= 800 && x < 960 && y >= 300 && y < 460) {
                v = (((x + y) & 1) != 0) ? 255 : 0;
            }
            if ((f % 16) == 0 && x >= 100 && x < 260 && y >= 420 && y < 560) {
                v = 128;
            }
            p.y[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(v);
        }
    }
    const int cw = w / 2, ch = h / 2;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            p.cb[static_cast<size_t>(y) * cw + x] = static_cast<uint8_t>((x * 2 - f * 5) & 255);
            p.cr[static_cast<size_t>(y) * cw + x] = static_cast<uint8_t>((x - y + f * 2) & 255);
        }
    }
}

double PlanePsnr(const uint8_t* a, const uint8_t* b, size_t count)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sum += static_cast<uint64_t>(d * d);
    }
    if (sum == 0) {
        return 99.0;
    }
    const double mse = static_cast<double>(sum) / static_cast<double>(count);
    return 10.0 * log10(255.0 * 255.0 / mse);
}

// ---------------------------------------------------------------- the test's own device

HRESULT CreateTestDevice(ID3D11Device** device)
{
    if (device == nullptr) {
        return E_POINTER;
    }
    *device = nullptr;
    ComPtr<IDXGIFactory> factory;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
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
    return D3D11CreateDevice(chosen.Get(),
                             chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                             nullptr, 0, want, 2, D3D11_SDK_VERSION, device, &got, nullptr);
}

// ---------------------------------------------------------------- the drawn source

HRESULT Pattern::Initialize(ID3D11Device* device, uint32_t width, uint32_t height,
                            uint32_t arraySize, uint32_t slice)
{
    if (arraySize == 0 || slice >= arraySize) {
        return E_INVALIDARG;
    }
    m_device.CopyFrom(device);
    m_device->GetImmediateContext(&m_ctx);
    m_width = width;
    m_height = height;
    m_slice = slice;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = arraySize;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_tex);
    if (FAILED(hr)) {
        return hr;
    }
    // One slice of the array is the render target, so the other slices stay at their initial
    // contents: if the encoder read the wrong slice the decoded picture would be black and the
    // comparison below would fail loudly rather than silently pass.
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.FirstArraySlice = slice;
    rd.Texture2DArray.ArraySize = 1;
    hr = m_device->CreateRenderTargetView(m_tex.Get(), &rd, &m_rtv);
    if (FAILED(hr)) {
        return hr;
    }
    hr = m_device->CreateVertexShader(g_VSFullscreen, sizeof(g_VSFullscreen), nullptr, &m_vs);
    if (SUCCEEDED(hr)) {
        hr = m_device->CreatePixelShader(g_PSTestPattern, sizeof(g_PSTestPattern), nullptr, &m_ps);
    }
    if (FAILED(hr)) {
        return hr;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return m_device->CreateBuffer(&bd, nullptr, &m_cb);
}

HRESULT Pattern::Draw(uint32_t frame)
{
    D3D11_MAPPED_SUBRESOURCE map = {};
    HRESULT hr = m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) {
        return hr;
    }
    uint32_t c[4] = { frame, m_width, m_height, 0 };
    memcpy(map.pData, c, sizeof(c));
    m_ctx->Unmap(m_cb.Get(), 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_width);
    vp.Height = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    ID3D11RenderTargetView* rtv = m_rtv.Get();
    ID3D11Buffer* cb = m_cb.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &cb);
    m_ctx->Draw(3, 0);
    ID3D11RenderTargetView* none = nullptr;
    m_ctx->OMSetRenderTargets(1, &none, nullptr);
    return S_OK;
}

// ---------------------------------------------------------------- NV12 source

HRESULT Nv12Source::Initialize(ID3D11Device* device, uint32_t width, uint32_t height,
                               uint32_t arraySize, uint32_t slice)
{
    if (arraySize == 0 || slice >= arraySize || (width & 1u) != 0 || (height & 1u) != 0) {
        // NV12 subsamples both axes, so an odd visible size has no NV12 representation.
        return E_INVALIDARG;
    }
    m_device.CopyFrom(device);
    m_device->GetImmediateContext(&m_ctx);
    m_width = width;
    m_height = height;
    m_arraySize = arraySize;
    m_slice = slice;
    m_pic.Allocate(width, height);
    // One contiguous NV12 image: the luma plane, then the interleaved chroma plane, both at a pitch
    // of `width` bytes. A planar texture is updated with a single call that covers both planes, the
    // same layout CreateTexture2D takes as initial data. Updating the chroma subresource on its own
    // with no destination box makes the Direct3D 11 runtime pass the full picture height to the
    // driver for a half height plane, and the driver then reads past the end of the source buffer
    // (measured: an access violation inside nvwgf2umx UpdateSubresource). One row of slack is kept
    // past the end so that a driver which rounds the row count up still reads mapped memory.
    m_chroma.assign(static_cast<size_t>(width) * (height + height / 2u + 1u), 128);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = arraySize;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    // An NV12 texture array is a decoder and frame server resource, and a driver may refuse it with
    // D3D11_BIND_SHADER_RESOURCE alone: the NVIDIA driver on the development PC answers E_INVALIDARG
    // for ArraySize > 1 with that bind flag. Each combination is tried in turn, most shader friendly
    // first, and the one that was accepted is reported, because it decides whether the encoder reads
    // the client's texture in place or has to copy the slice.
    const UINT candidates[] = {
        D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER,
        D3D11_BIND_DECODER,
    };
    HRESULT hr = E_FAIL;
    for (UINT bind : candidates) {
        td.BindFlags = bind;
        hr = m_device->CreateTexture2D(&td, nullptr, &m_tex);
        if (SUCCEEDED(hr)) {
            m_bindFlags = bind;
            return S_OK;
        }
    }
    return hr;
}

HRESULT Nv12Source::Write(uint32_t frame)
{
    MakeSyntheticPicture(m_pic, frame);
    const uint32_t cw = m_width / 2u;
    const uint32_t ch = m_height / 2u;
    memcpy(m_chroma.data(), m_pic.y.data(), m_pic.y.size());
    uint8_t* uv = m_chroma.data() + static_cast<size_t>(m_width) * m_height;
    for (uint32_t y = 0; y < ch; ++y) {
        for (uint32_t x = 0; x < cw; ++x) {
            uv[static_cast<size_t>(y) * m_width + x * 2u + 0] =
                m_pic.cb[static_cast<size_t>(y) * cw + x];
            uv[static_cast<size_t>(y) * m_width + x * 2u + 1] =
                m_pic.cr[static_cast<size_t>(y) * cw + x];
        }
    }
    // Subresource `m_slice` is mip 0 of that array slice in plane 0, and a planar update through it
    // covers the chroma plane as well (D3D11 numbers subresources
    // plane * MipLevels * ArraySize + slice * MipLevels + mip).
    m_ctx->UpdateSubresource(m_tex.Get(), m_slice, nullptr, m_chroma.data(), m_width, 0);
    return S_OK;
}

// ---------------------------------------------------------------- self test

namespace {

int g_failures = 0;

void Fail(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    ++g_failures;
}

// A bit reader over an RBSP, for the bit writer round trip only.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    uint32_t U(uint32_t n)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i) {
            v = (v << 1) | Bit();
        }
        return v;
    }
    uint32_t UE()
    {
        uint32_t zeros = 0;
        while (Bit() == 0 && zeros < 32) {
            ++zeros;
        }
        // The loop consumed the terminating 1 bit.
        uint32_t v = (1u << zeros) - 1u;
        if (zeros > 0) {
            v += U(zeros);
        }
        return v;
    }
    int32_t SE()
    {
        const uint32_t k = UE();
        const int32_t m = static_cast<int32_t>((k + 1) / 2);
        return (k & 1) ? m : -m;
    }
    size_t Position() const { return m_pos; }

private:
    uint32_t Bit()
    {
        if ((m_pos >> 3) >= m_size) {
            ++m_pos;
            return 0;
        }
        const uint32_t b = (m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1u;
        ++m_pos;
        return b;
    }
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
};

// Prefix-freeness and Kraft sum of one variable-length code table.
struct VlcCheck {
    const char* name;
    double kraft = 0.0;
    uint32_t codes = 0;
    bool prefixFree = true;
};

void CheckVlc(VlcCheck& out, const uint8_t* len, const uint8_t* bits, size_t count)
{
    struct Code { uint32_t len; uint32_t bits; };
    std::vector<Code> codes;
    for (size_t i = 0; i < count; ++i) {
        if (len[i] == 0) {
            continue;                       // marked impossible
        }
        if (len[i] > 16) {
            out.prefixFree = false;
            continue;
        }
        if (bits[i] >= (1u << len[i]) && len[i] < 8) {
            out.prefixFree = false;         // value does not fit the declared length
        }
        codes.push_back({ len[i], bits[i] });
        out.kraft += 1.0 / static_cast<double>(1u << len[i]);
        ++out.codes;
    }
    for (size_t i = 0; i < codes.size(); ++i) {
        for (size_t j = i + 1; j < codes.size(); ++j) {
            const uint32_t shorter = (codes[i].len < codes[j].len) ? codes[i].len : codes[j].len;
            const uint32_t a = codes[i].bits >> (codes[i].len - shorter);
            const uint32_t b = codes[j].bits >> (codes[j].len - shorter);
            if (a == b) {
                out.prefixFree = false;
            }
        }
    }
}

// The coeff_token tables hold values up to 16 bits, so they need their own widening pass. The stored
// arrays are uint8_t for the bits; table 0 has codes up to 16 bits long whose value does not fit a
// byte, so the generator stores the bits in a separate wide array where needed. The check below
// therefore works on the pair of arrays as declared and only verifies what a byte can express; the
// decoder oracle covers the rest.
void CheckCoeffTokenTable(uint32_t table)
{
    VlcCheck c = {};
    c.name = "coeff_token";
    CheckVlc(c, kCoeffTokenLen[table], kCoeffTokenBits[table], 4 * 17);
    printf("  coeff_token table %u: %u codes, Kraft %.6f%s\n", table, c.codes, c.kraft,
           c.prefixFree ? "" : "  PREFIX COLLISION");
    if (!c.prefixFree) {
        Fail("coeff_token table %u is not prefix free", table);
    }
    if (c.kraft > 1.0000001) {
        Fail("coeff_token table %u Kraft sum %.6f exceeds 1", table, c.kraft);
    }
}

void SelfTestTables()
{
    printf("tables\n");
    InitDerivedTables();

    for (uint32_t t = 0; t < 4; ++t) {
        CheckCoeffTokenTable(t);
    }
    {
        VlcCheck c = {};
        c.name = "coeff_token chroma DC";
        CheckVlc(c, kCoeffTokenChromaDcLen, kCoeffTokenChromaDcBits, 4 * 5);
        printf("  coeff_token chroma DC: %u codes, Kraft %.6f%s\n", c.codes, c.kraft,
               c.prefixFree ? "" : "  PREFIX COLLISION");
        if (!c.prefixFree) {
            Fail("chroma DC coeff_token is not prefix free");
        }
        if (fabs(c.kraft - 1.0) > 1e-9) {
            Fail("chroma DC coeff_token Kraft sum is %.9f, expected exactly 1", c.kraft);
        }
    }
    for (uint32_t i = 0; i < 15; ++i) {
        VlcCheck c = {};
        c.name = "total_zeros";
        CheckVlc(c, kTotalZerosLen[i], kTotalZerosBits[i], 16 - i);
        const double expect = (i == 0) ? (1.0 - 1.0 / 512.0) : 1.0;
        if (!c.prefixFree) {
            Fail("total_zeros row %u is not prefix free", i + 1);
        }
        if (fabs(c.kraft - expect) > 1e-9) {
            Fail("total_zeros row %u Kraft sum %.9f, expected %.9f", i + 1, c.kraft, expect);
        }
        if (c.codes != 16 - i) {
            Fail("total_zeros row %u has %u codes, expected %u", i + 1, c.codes, 16 - i);
        }
    }
    printf("  total_zeros: 15 rows prefix free, Kraft sums as expected\n");
    for (uint32_t i = 0; i < 3; ++i) {
        VlcCheck c = {};
        c.name = "total_zeros chroma DC";
        CheckVlc(c, kTotalZerosChromaDcLen[i], kTotalZerosChromaDcBits[i], 4 - i);
        if (!c.prefixFree || fabs(c.kraft - 1.0) > 1e-9) {
            Fail("total_zeros chroma DC row %u: prefixFree %d Kraft %.9f", i + 1,
                 c.prefixFree ? 1 : 0, c.kraft);
        }
    }
    printf("  total_zeros chroma DC: 3 rows prefix free, Kraft 1.0\n");
    for (uint32_t i = 0; i < 7; ++i) {
        VlcCheck c = {};
        c.name = "run_before";
        const size_t count = (i == 6) ? 15u : (i + 2u);
        CheckVlc(c, kRunBeforeLen[i], kRunBeforeBits[i], count);
        const double expect = (i == 6) ? (1.0 - 1.0 / 2048.0) : 1.0;
        if (!c.prefixFree) {
            Fail("run_before row %u is not prefix free", i + 1);
        }
        if (fabs(c.kraft - expect) > 1e-9) {
            Fail("run_before row %u Kraft sum %.9f, expected %.9f", i + 1, c.kraft, expect);
        }
    }
    printf("  run_before: 7 rows prefix free, Kraft sums as expected\n");

    // The zig-zag scan has to be a permutation of 0..15, or coefficients land in the wrong place.
    uint32_t seen = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        seen |= 1u << kZigZag4x4[i];
    }
    if (seen != 0xFFFFu) {
        Fail("kZigZag4x4 is not a permutation (mask 0x%04X)", seen);
    }
    // The inverse 4x4 block scan likewise covers every position exactly once.
    seen = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        seen |= 1u << (kBlk4x4Y[i] * 4u + kBlk4x4X[i]);
    }
    if (seen != 0xFFFFu) {
        Fail("kBlk4x4X/Y do not cover the macroblock (mask 0x%04X)", seen);
    }
    printf("  zig-zag and 4x4 block scans are permutations\n");

    // Table 8-15 is monotone non-decreasing and ends at 39.
    for (uint32_t i = 1; i < 22; ++i) {
        if (kChromaQpFromQpi30[i] < kChromaQpFromQpi30[i - 1]) {
            Fail("kChromaQpFromQpi30 is not monotone at %u", i + 30);
        }
    }
    if (kChromaQpFromQpi30[0] != 29 || kChromaQpFromQpi30[21] != 39) {
        Fail("kChromaQpFromQpi30 endpoints are %u..%u, expected 29..39",
             kChromaQpFromQpi30[0], kChromaQpFromQpi30[21]);
    }
    printf("  Table 8-15 chroma QP: monotone, 29..39\n");

    // Table 9-4 inverse: every coded block pattern value has to map back to itself.
    uint32_t cbpFailures = 0;
    for (uint32_t cbp = 0; cbp < 48; ++cbp) {
        const uint32_t ci = CbpToCodeNum(cbp, true);
        const uint32_t cp = CbpToCodeNum(cbp, false);
        if (ci >= 48 || kCbpIntraFromCodeNum[ci] != cbp) {
            ++cbpFailures;
        }
        if (cp >= 48 || kCbpInterFromCodeNum[cp] != cbp) {
            ++cbpFailures;
        }
    }
    if (cbpFailures != 0) {
        Fail("coded_block_pattern mapping is not invertible in %u cases", cbpFailures);
    } else {
        printf("  Table 9-4 coded_block_pattern: 48 values invertible both ways\n");
    }

    // The forward multiplier has to be the inverse of Table 8-14 scaled by the gain of the forward
    // core transform for that position class: 4 for the doubly even positions, 2.56 for the doubly
    // odd, 3.2 for the rest. Equivalently normAdjust * quantCoef is 2^17 times 1, 0.64 or 0.8. A
    // transposed digit in either table shows up here and nowhere else, because dequantisation alone
    // would still be conformant - the pictures would merely come out wrong.
    const double kGain[3] = { 1.0, 0.64, 0.8 };
    uint32_t qFailures = 0;
    double worst = 0.0;
    for (uint32_t r = 0; r < 6; ++r) {
        for (uint32_t cls = 0; cls < 3; ++cls) {
            const double q = static_cast<double>(kQuantCoef4x4[r][cls]);
            const double d = static_cast<double>(kNormAdjust4x4[r][cls]);
            const double expect = 131072.0 * kGain[cls];
            const double error = fabs(q * d - expect) / expect;
            if (error > worst) {
                worst = error;
            }
            if (error > 0.0005) {
                Fail("kQuantCoef4x4[%u][%u] = %.0f against normAdjust %.0f: product %.0f, expected "
                     "%.0f", r, cls, q, d, q * d, expect);
                ++qFailures;
            }
        }
    }
    if (qFailures == 0) {
        printf("  Table 8-14 and the forward multipliers: 18 pairs, worst error %.4f %%\n",
               worst * 100.0);
    }
}

void SelfTestBitWriter()
{
    printf("bit writer\n");
    BitWriter bw;
    bw.Clear();
    // A fixed pseudo-random sequence, so a failure is reproducible.
    uint32_t state = 0x13572468u;
    struct Item { int kind; uint32_t n; uint32_t u; int32_t s; };
    std::vector<Item> items;
    for (uint32_t i = 0; i < 2000; ++i) {
        state = state * 1664525u + 1013904223u;
        const int kind = static_cast<int>((state >> 13) % 3u);
        Item it = { kind, 0, 0, 0 };
        if (kind == 0) {
            it.n = 1u + ((state >> 7) % 24u);
            it.u = (state >> 3) & ((it.n >= 32) ? 0xFFFFFFFFu : ((1u << it.n) - 1u));
            bw.U(it.n, it.u);
        } else if (kind == 1) {
            it.u = (state >> 5) % 100000u;
            bw.UE(it.u);
        } else {
            it.s = static_cast<int32_t>((state >> 5) % 20001u) - 10000;
            bw.SE(it.s);
        }
        items.push_back(it);
    }
    const size_t bits = bw.BitCount();
    bw.RbspTrailingBits();
    const std::vector<uint8_t>& rbsp = bw.Rbsp();
    BitReader br(rbsp.data(), rbsp.size());
    uint32_t mismatches = 0;
    for (const Item& it : items) {
        if (it.kind == 0) {
            if (br.U(it.n) != it.u) { ++mismatches; }
        } else if (it.kind == 1) {
            if (br.UE() != it.u) { ++mismatches; }
        } else {
            if (br.SE() != it.s) { ++mismatches; }
        }
    }
    if (mismatches != 0) {
        Fail("bit writer round trip: %u of %zu items differ", mismatches, items.size());
    } else {
        printf("  %zu items, %zu bits, round trip exact\n", items.size(), bits);
    }
    if (br.Position() != bits) {
        Fail("bit reader consumed %zu bits, writer produced %zu", br.Position(), bits);
    }
    if ((bw.BitCount() % 8) != 0) {
        Fail("rbsp_trailing_bits left the writer unaligned");
    }
}

void SelfTestEmulationPrevention()
{
    printf("emulation prevention\n");
    struct Case { std::vector<uint8_t> in; std::vector<uint8_t> expect; };
    const Case cases[] = {
        { { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x03, 0x00 } },
        { { 0x00, 0x00, 0x01 }, { 0x00, 0x00, 0x03, 0x01 } },
        { { 0x00, 0x00, 0x02 }, { 0x00, 0x00, 0x03, 0x02 } },
        { { 0x00, 0x00, 0x03 }, { 0x00, 0x00, 0x03, 0x03 } },
        { { 0x00, 0x00, 0x04 }, { 0x00, 0x00, 0x04 } },
        { { 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x03, 0x00, 0x00 } },
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> out;
        EmitNal(out, 3, 7, c.in);
        // Skip the four byte start code and the NAL header byte.
        if (out.size() != 5 + c.expect.size() ||
            memcmp(out.data() + 5, c.expect.data(), c.expect.size()) != 0) {
            Fail("emulation prevention wrong for a %zu byte payload", c.in.size());
            continue;
        }
    }
    // No escaped stream may contain a start code of its own.
    std::vector<uint8_t> payload;
    uint32_t state = 7;
    for (uint32_t i = 0; i < 4096; ++i) {
        state = state * 1103515245u + 12345u;
        // Heavy on zeros, to provoke the escape.
        payload.push_back(static_cast<uint8_t>(((state >> 16) % 5u == 0u) ? 0u : ((state >> 8) & 0xFF)));
    }
    std::vector<uint8_t> nal;
    EmitNal(nal, 3, 1, payload);
    uint32_t starts = 0;
    for (size_t i = 5; i + 2 < nal.size(); ++i) {
        if (nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] <= 1) {
            ++starts;
        }
    }
    if (starts != 0) {
        Fail("escaped payload still contains %u start-code-like sequences", starts);
    } else {
        printf("  6 fixed cases and a 4096 byte zero-heavy payload: no start code emulation\n");
    }
}

void SelfTestParameterSets()
{
    printf("parameter sets\n");
    EncoderConfig cfg;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.fpsNum = 30;
    cfg.fpsDen = 1;
    SequenceParams sps;
    PictureParams pps;
    MakeSequenceParams(cfg, &sps, &pps);
    if (sps.widthMb != 80 || sps.heightMb != 45) {
        Fail("1280x720 gave %ux%u macroblocks, expected 80x45", sps.widthMb, sps.heightMb);
    }
    if (sps.cropRight != 0 || sps.cropBottom != 0) {
        Fail("1280x720 should need no cropping, got %u/%u", sps.cropRight, sps.cropBottom);
    }
    if (sps.levelIdc != 31) {
        // 3600 macroblocks at 30 Hz is 108000 MB/s, exactly level 3.1's MaxMBPS.
        Fail("1280x720 at 30 Hz chose level %u, expected 31", sps.levelIdc);
    }
    if (pps.picInitQp != 26) {
        // The quantiser the encoder runs at travels in slice_qp_delta, never in pic_init_qp: this
        // field goes into MF_MT_MPEG_SEQUENCE_HEADER, which a file sink copies into the container
        // before the first ICodecAPI setting is even known.
        Fail("pic_init_qp is %d, expected 26 whatever the configured quantiser",
             static_cast<int>(pps.picInitQp));
    }
    {
        EncoderConfig q = cfg;
        q.qpInit = 44;
        SequenceParams s2;
        PictureParams p2;
        MakeSequenceParams(q, &s2, &p2);
        if (p2.picInitQp != 26) {
            Fail("a configured quantiser of 44 moved pic_init_qp to %d",
                 static_cast<int>(p2.picInitQp));
        }
    }
    cfg.width = 1920;
    cfg.height = 1080;
    MakeSequenceParams(cfg, &sps, &pps);
    if (sps.widthMb != 120 || sps.heightMb != 68 || sps.cropBottom != 8) {
        Fail("1920x1080 gave %ux%u macroblocks and crop %u, expected 120x68 crop 8",
             sps.widthMb, sps.heightMb, sps.cropBottom);
    }
    // The declared level has to cover the bitrate as well as the frame size and the macroblock rate.
    // 1920x1080 at 30 Hz is 8160 macroblocks and 244800 MB/s, which fits level 4.0, but level 4.0's
    // MaxBR for this profile family is 20000 kbit/s: above that the stream needs level 4.1, and a
    // level the stream exceeds is one a hardware decoder may refuse.
    {
        struct LevelCase { uint32_t w, h, fps, bps, want; const char* why; };
        const LevelCase cases[] = {
            { 1920, 1080, 30,  6000000, 40, "1080p30 at 6 Mbit/s fits level 4.0" },
            { 1920, 1080, 30, 20000000, 40, "1080p30 at exactly level 4.0's MaxBR" },
            { 1920, 1080, 30, 20000001, 41, "one bit over level 4.0's MaxBR needs 4.1" },
            { 1920, 1080, 30, 60000000, 50, "1080p30 at 60 Mbit/s needs level 5.0" },
            { 320,   240, 30,   500000, 13, "a small picture at 500 kbit/s fits level 1.3" },
            { 320,   240, 30,  1500000, 20, "the same picture at 1.5 Mbit/s needs level 2.0" },
        };
        for (const LevelCase& c : cases) {
            EncoderConfig lc;
            lc.width = c.w;
            lc.height = c.h;
            lc.fpsNum = c.fps;
            lc.fpsDen = 1;
            lc.meanBitRate = c.bps;
            SequenceParams ls;
            PictureParams lp;
            MakeSequenceParams(lc, &ls, &lp);
            if (ls.levelIdc != c.want) {
                Fail("%s: chose level %u, expected %u", c.why, ls.levelIdc, c.want);
            }
        }
        printf("  level selection: frame size, macroblock rate and MaxBR, 6 cases\n");
    }
    // The VUI colour description comes from the configuration, which the transform fills from the
    // input media type, and it has to reach the bitstream. Two descriptions that differ in one code
    // point must give two different SPS NALs.
    {
        EncoderConfig a709;
        a709.width = 640;
        a709.height = 480;
        EncoderConfig b601 = a709;
        b601.matrixCoefficients = 6;
        b601.colourPrimaries = 6;
        b601.transferCharacteristics = 6;
        b601.fullRange = true;
        SequenceParams s1, s2;
        PictureParams p1, p2;
        MakeSequenceParams(a709, &s1, &p1);
        MakeSequenceParams(b601, &s2, &p2);
        if (s1.matrixCoefficients != 1 || s2.matrixCoefficients != 6 || s2.fullRange != true) {
            Fail("the colour description did not reach the sequence parameters: %u/%u, range %d",
                 s1.matrixCoefficients, s2.matrixCoefficients, s2.fullRange ? 1 : 0);
        }
        std::vector<uint8_t> n1, n2;
        BuildParameterSetNals(n1, s1, p1);
        BuildParameterSetNals(n2, s2, p2);
        if (n1.size() != n2.size() || n1 == n2) {
            Fail("two different colour descriptions produced the same parameter sets");
        }
        printf("  colour description: carried from the configuration into the SPS\n");
    }
    std::vector<uint8_t> nals;
    BuildParameterSetNals(nals, sps, pps);
    // Two NALs, types 7 then 8, each with a four byte start code.
    if (nals.size() < 16 || nals[0] != 0 || nals[1] != 0 || nals[2] != 0 || nals[3] != 1 ||
        (nals[4] & 31) != 7) {
        Fail("the first parameter set NAL is not an SPS");
    }
    size_t second = 0;
    for (size_t i = 5; i + 4 < nals.size(); ++i) {
        if (nals[i] == 0 && nals[i + 1] == 0 && nals[i + 2] == 0 && nals[i + 3] == 1) {
            second = i + 4;
            break;
        }
    }
    if (second == 0 || (nals[second] & 31) != 8) {
        Fail("the second parameter set NAL is not a PPS");
    } else {
        printf("  SPS and PPS built, %zu bytes, 1280x720 level 3.1, 1920x1080 crop 8\n", nals.size());
    }
}

// Which frame sizes the encoder admits. An odd size has to be refused, because the SPS crop counts
// in two luma samples and cannot express it; the encoder used to mask it down and code a picture one
// sample smaller than the client negotiated, with nothing in the bitstream or the media type saying
// so. GpuEncoder::Initialize answers before it touches Direct3D, so this needs no device.
void SelfTestFrameSizes()
{
    printf("frame sizes\n");
    struct Case { uint32_t w, h; bool want; const char* why; };
    const Case cases[] = {
        { 1920, 1080, true,  "1080p" },
        {   16,   16, true,  "the smallest picture" },
        { 4096, 4096, true,  "the largest picture" },
        {  638,  478, true,  "an even size that is not a whole number of macroblocks" },
        { 1365,  767, false, "the odd size a window capture hands over" },
        {  640,  481, false, "an odd height" },
        {  641,  480, false, "an odd width" },
        {   14,   16, false, "below the smallest picture" },
        { 4098, 1080, false, "wider than the encoder admits" },
    };
    for (const Case& c : cases) {
        if (IsCodableFrameSize(c.w, c.h) != c.want) {
            Fail("IsCodableFrameSize(%u, %u) is not %s (%s)", c.w, c.h, c.want ? "true" : "false",
                 c.why);
        }
        if (!c.want) {
            GpuEncoder gpu;
            const HRESULT hr = gpu.Initialize(nullptr, c.w, c.h);
            if (hr != E_INVALIDARG) {
                Fail("GpuEncoder::Initialize(%u, %u) returned 0x%08lX, expected E_INVALIDARG",
                     c.w, c.h, static_cast<unsigned long>(hr));
                gpu.Shutdown();
            }
        }
    }
    // And nothing is rounded: the parameter sets of an even size that is not a whole number of
    // macroblocks crop exactly the padding, never a visible column or row.
    EncoderConfig cfg;
    cfg.width = 638;
    cfg.height = 478;
    SequenceParams sps;
    PictureParams pps;
    MakeSequenceParams(cfg, &sps, &pps);
    if (sps.widthMb != 40 || sps.heightMb != 30 || sps.cropRight != 2 || sps.cropBottom != 2) {
        Fail("638x478 gave %ux%u macroblocks, crop %u right %u bottom, expected 40x30 crop 2 and 2",
             sps.widthMb, sps.heightMb, sps.cropRight, sps.cropBottom);
    } else {
        printf("  9 sizes classified, odd sizes refused by the encoder, 638x478 crops 2 and 2\n");
    }
}

// The transform encodes on the BC-250 and on no other adapter. This case needs no GPU and no device:
// it only runs where DXGI has no 1002:13FE adapter, and there GpuEncoder::Initialize without a device
// must refuse. A machine that has the BC-250 would have to create a device to answer, so the case
// says so and stops.
void SelfTestAdapterChoice()
{
    printf("adapter choice\n");
    ComPtr<IDXGIFactory> factory;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        printf("  no DXGI on this machine: not checked\n");
        return;
    }
    bool haveBc250 = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter> a;
        if (factory->EnumAdapters(i, &a) != S_OK) {
            break;
        }
        DXGI_ADAPTER_DESC ad = {};
        a->GetDesc(&ad);
        if (ad.VendorId == 0x1002 && ad.DeviceId == 0x13FE) {
            haveBc250 = true;
        }
    }
    if (haveBc250) {
        printf("  the BC-250 is in this machine: the refusal case needs a machine without it\n");
        return;
    }
    GpuEncoder gpu;
    const HRESULT hr = gpu.Initialize(nullptr, 640, 480);
    if (hr != DXGI_ERROR_NOT_FOUND) {
        Fail("GpuEncoder::Initialize(nullptr) returned 0x%08lX on a machine without a BC-250, "
             "expected DXGI_ERROR_NOT_FOUND", static_cast<unsigned long>(hr));
        gpu.Shutdown();
        return;
    }
    printf("  no BC-250 in this machine: the transform refuses to pick another adapter\n");
}

} // namespace

int RunSelfTest()
{
    g_failures = 0;
    SelfTestTables();
    SelfTestBitWriter();
    SelfTestEmulationPrevention();
    SelfTestParameterSets();
    SelfTestFrameSizes();
    SelfTestAdapterChoice();
    printf("%s: %d failure(s)\n", (g_failures == 0) ? "selftest PASS" : "selftest FAIL", g_failures);
    return (g_failures == 0) ? 0 : 1;
}

// ---------------------------------------------------------------- encode and oracle

namespace {

// Recomputes TotalCoeff per 4x4 block from the levels the GPU wrote and compares it with the
// non-zero counts the shaders reported in MbInfo. A disagreement is invisible to the decoder's
// reconstruction but corrupts the nC context of the neighbouring blocks, so it has to be checked
// separately from the bit-exactness oracle.
uint32_t CheckNnzAgreement(const Encoder& enc, uint32_t widthMb, uint32_t heightMb)
{
    const std::vector<uint32_t>& levels = enc.LastLevels();
    const std::vector<MbInfo>& info = enc.LastMbInfo();
    uint32_t bad = 0;
    for (uint32_t mb = 0; mb < widthMb * heightMb; ++mb) {
        const MbInfo& m = info[mb];
        const uint32_t* L = &levels[static_cast<size_t>(mb) * kLevelsWordsPerMb];
        const bool i16 = MbIsIntra(m);
        // The levels buffer and MbInfo both index the sixteen luma blocks in raster order inside the
        // macroblock (block = 4 * (y / 4) + x / 4), and the coefficients inside a block in zig-zag
        // scan order. The 8x8 index that selects the coded block pattern bit is derived from that
        // raster index the same way the shader derives it.
        for (uint32_t blk = 0; blk < 16; ++blk) {
            const uint32_t base = kLevelsOffLumaAc + blk * 8u;
            uint32_t n = 0;
            for (uint32_t c = i16 ? 1u : 0u; c < 16u; ++c) {
                if (LevelAt(L, base, c) != 0) {
                    ++n;
                }
            }
            const uint32_t b8 = ((blk >> 3) * 2u) + ((blk & 3u) >> 1);
            const bool coded = i16 ? (m.cbpLuma != 0u) : (((m.cbpLuma >> b8) & 1u) != 0u);
            if (MbNnzLuma(m, blk) != (coded ? n : 0u)) {
                ++bad;
            }
        }
        for (uint32_t comp = 0; comp < 2; ++comp) {
            for (uint32_t b = 0; b < 4; ++b) {
                const uint32_t base = kLevelsOffChromaAc + (comp * 4u + b) * 8u;
                uint32_t n = 0;
                for (uint32_t c = 1; c < 16; ++c) {
                    if (LevelAt(L, base, c) != 0) {
                        ++n;
                    }
                }
                const uint32_t expect = (m.cbpChroma == 2u) ? n : 0u;
                if (MbNnzChroma(m, comp, b) != expect) {
                    ++bad;
                }
            }
        }
    }
    return bad;
}

bool WriteFile(const std::wstring& path, const std::vector<uint8_t>& bytes)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) {
        return false;
    }
    const size_t n = bytes.empty() ? 0 : fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return n == bytes.size();
}

// Prints how many samples of a plane differ and the eight samples across the two 4x4 block edges
// nearest the given position: the row across the vertical edge and the column across the horizontal
// one, in the order p3 p2 p1 p0 q0 q1 q2 q3 that clause 8.7 uses.
void DumpEdges(const char* plane, const uint8_t* ours, uint32_t strideA, const uint8_t* theirs,
               uint32_t strideB, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    uint32_t count = 0;
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t c = 0; c < w; ++c) {
            if (ours[static_cast<size_t>(r) * strideA + c] != theirs[static_cast<size_t>(r) * strideB + c]) {
                ++count;
            }
        }
    }
    printf("  %s: %u of %u samples differ\n", plane, count, w * h);
    const uint32_t vedge = (x & ~3u);
    const uint32_t hedge = ((y + 4u) & ~3u);
    if (vedge >= 4u) {
        printf("    row %u across the vertical edge at %u    ours ", y, vedge);
        for (int k = -4; k < 4; ++k) {
            printf("%4d", ours[static_cast<size_t>(y) * strideA + vedge + k]);
        }
        printf("\n                                         theirs ");
        for (int k = -4; k < 4; ++k) {
            printf("%4d", theirs[static_cast<size_t>(y) * strideB + vedge + k]);
        }
        printf("\n");
    }
    if (hedge >= 4u && hedge + 3u < h) {
        printf("    column %u across the horizontal edge at %u  ours ", x, hedge);
        for (int k = -4; k < 4; ++k) {
            printf("%4d", ours[(static_cast<size_t>(hedge) + k) * strideA + x]);
        }
        printf("\n                                         theirs ");
        for (int k = -4; k < 4; ++k) {
            printf("%4d", theirs[(static_cast<size_t>(hedge) + k) * strideB + x]);
        }
        printf("\n");
    }
}

// Prints the difference between our reconstruction and the decoder's for one macroblock, as a 16x16
// grid. A uniform offset inside each 4x4 block means a wrong DC; a pattern that looks like a
// permutation of the blocks means a wrong scan or block order; noise means wrong levels.
void DumpMbDifference(const std::vector<uint8_t>& ours, const Picture& theirs, uint32_t padW,
                      uint32_t mbx, uint32_t mby)
{
    printf("  macroblock (%u,%u), ours minus decoder, luma:\n", mbx, mby);
    for (uint32_t y = 0; y < 16; ++y) {
        printf("    ");
        for (uint32_t x = 0; x < 16; ++x) {
            const uint32_t px = mbx * 16u + x;
            const uint32_t py = mby * 16u + y;
            const int a = ours[static_cast<size_t>(py) * padW + px];
            const int b = theirs.y[static_cast<size_t>(py) * theirs.width + px];
            printf("%5d", a - b);
            if ((x & 3u) == 3u) {
                printf(" |");
            }
        }
        printf("\n");
        if ((y & 3u) == 3u) {
            printf("\n");
        }
    }
}

// First differing sample of two planes, for a useful failure message.
// Our reconstruction is in coded dimensions, the decoder's picture in visible ones, so the two sides
// need their own strides: passing one for both silently compared the wrong samples for any width that
// is not a multiple of 16 (638x478 reported a difference between two samples that both read 4).
bool FirstDifference(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h, uint32_t strideA,
                     uint32_t strideB, uint32_t* ox, uint32_t* oy)
{
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (a[static_cast<size_t>(y) * strideA + x] != b[static_cast<size_t>(y) * strideB + x]) {
                *ox = x;
                *oy = y;
                return true;
            }
        }
    }
    return false;
}

// The per-stage GPU profile of the run, from GpuStageProfile. Prints nothing when
// BC250_MFT_STAGE_TIMING was unset, which is the normal case: no picture then carries a profile.
// Key and non-key pictures are reported apart because they run different shaders.
void PrintStageProfile(const uint32_t pictures[2], const uint32_t dispatches[2][GpuStageCount],
                      const double ms[2][GpuStageCount], const double totalMs[2],
                      const std::vector<GpuStageStep> steps[2])
{
    if (pictures[0] == 0 && pictures[1] == 0) {
        return;
    }
    printf("  GPU stages per picture, from the device's timestamps between the dispatches\n");
    printf("    %-7s %-9s %10s %9s %10s %8s\n", "picture", "stage", "dispatches", "ms", "ms/disp",
           "share");
    for (int k = 0; k < 2; ++k) {
        if (pictures[k] == 0) {
            continue;
        }
        const double n = static_cast<double>(pictures[k]);
        for (uint32_t s = 0; s < GpuStageCount; ++s) {
            if (dispatches[k][s] == 0) {
                continue;
            }
            printf("    %-7s %-9s %10.1f %9.3f %10.4f %7.1f %%\n", (k == 0) ? "I" : "P",
                   GpuStageName(s), static_cast<double>(dispatches[k][s]) / n, ms[k][s] / n,
                   ms[k][s] / static_cast<double>(dispatches[k][s]),
                   100.0 * ms[k][s] / ((totalMs[k] > 0.0) ? totalMs[k] : 1.0));
        }
        printf("    %-7s %-9s %10s %9.3f   over %u picture(s)\n", (k == 0) ? "I" : "P", "all", "",
               totalMs[k] / n, pictures[k]);
    }
    for (int k = 0; k < 2; ++k) {
        if (steps[k].empty()) {
            continue;
        }
        printf("  every dispatch of the first %s picture, %zu of them\n", (k == 0) ? "I" : "P",
               steps[k].size());
        printf("    %5s %-9s %8s %9s %10s\n", "step", "stage", "groups", "ms", "us/group");
        for (size_t i = 0; i < steps[k].size(); ++i) {
            const GpuStageStep& s = steps[k][i];
            printf("    %5zu %-9s %8u %9.4f %10.3f\n", i, GpuStageName(s.stage), s.groups, s.ms,
                   (s.groups != 0) ? (1000.0 * s.ms / static_cast<double>(s.groups)) : 0.0);
        }
    }
}

} // namespace

int RunEncode(const Options& o)
{
    printf("encode: %ux%u, %u frames, qp %u, gop %u, deblocking %s, source %s\n",
           o.width, o.height, o.frames, o.qp, o.gop, o.deblock ? "on" : "off",
           o.gpuSource ? "GPU draw (BGRA texture)"
                       : (o.nv12sys ? "CPU synthetic (NV12 in system memory, padded stride)"
                                    : "CPU synthetic (I420)"));

    EncoderConfig cfg;
    cfg.width = o.width;
    cfg.height = o.height;
    cfg.fpsNum = o.fps;
    cfg.fpsDen = 1;
    cfg.gopSize = o.gop;
    cfg.meanBitRate = o.bitrate;
    cfg.rateControl = o.rc;
    cfg.qpInit = o.qp;
    cfg.qpMin = o.qp;
    cfg.qpMax = o.qp;
    cfg.deblocking = o.deblock;

    // The test's own device, as a Media Foundation client gives one: the transform itself takes the
    // BC-250 adapter only (gpu_pipeline.cpp), and this test also runs on a development PC.
    ComPtr<ID3D11Device> device;
    HRESULT hr = CreateTestDevice(&device);
    if (FAILED(hr)) {
        printf("  CreateTestDevice failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    Encoder enc;
    hr = enc.Initialize(device.Get(), cfg);
    if (FAILED(hr)) {
        printf("  Encoder::Initialize failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    const uint32_t wmb = enc.Gpu().WidthMb();
    const uint32_t hmb = enc.Gpu().HeightMb();
    const uint32_t padW = enc.Gpu().PadW();
    const uint32_t padH = enc.Gpu().PadH();
    printf("  %ux%u macroblocks, coded %ux%u, level %u\n", wmb, hmb, padW, padH, enc.Sps().levelIdc);

    Pattern pattern;
    if (o.gpuSource) {
        hr = pattern.Initialize(enc.Gpu().Device(), o.width, o.height);
        if (FAILED(hr)) {
            printf("  Pattern::Initialize failed 0x%08lX\n", static_cast<unsigned long>(hr));
            return 2;
        }
    }

    H264Decoder dec;
    hr = dec.Initialize(o.width, o.height);
    if (FAILED(hr)) {
        printf("  the inbox decoder MFT would not start: 0x%08lX\n",
               static_cast<unsigned long>(hr));
        return 2;
    }

    Picture src;
    src.Allocate(o.width, o.height);
    std::vector<uint8_t> nv12y, nv12c;
    std::vector<uint8_t> stream, frame;
    std::vector<uint8_t> recY, recCb, recCr, srcY, srcCb, srcCr;
    std::vector<std::vector<uint8_t>> recHistoryY, recHistoryCb, recHistoryCr;
    double totalMs = 0.0, totalGpuMs = 0.0, psnrSum = 0.0;
    // The stages of a picture, summed over the run: the GPU stage as the thread sees it, the readback
    // inside it, and the CPU half split into entropy coding and byte stream assembly.
    double totalGpuWallMs = 0.0, totalReadbackMs = 0.0, totalCpuMs = 0.0;
    double totalCavlcMs = 0.0, totalNalMs = 0.0;
    double totalRecordMs = 0.0, totalMapWaitMs = 0.0;
    // Per-stage GPU time, kept apart for key and non-key pictures because the two run different
    // shaders: an I picture sweeps cs_mb's intra entry point along the anti-diagonals, a P picture
    // runs cs_me once and cs_mb's inter entry point once. Filled only with BC250_MFT_STAGE_TIMING
    // set; see GpuStageProfile.
    double stageMs[2][GpuStageCount] = {};
    double stageTotalMs[2] = {};
    uint32_t stageDispatches[2][GpuStageCount] = {};
    uint32_t stagePictures[2] = {};
    std::vector<GpuStageStep> firstSteps[2];
    uint64_t totalBytes = 0;
    uint32_t keyFrames = 0, skipTotal = 0, nnzBad = 0;
    uint32_t timingBad = 0;
    size_t importBad = 0;

    for (uint32_t i = 0; i < o.frames; ++i) {
        GpuFrameInput in;
        if (o.gpuSource) {
            hr = pattern.Draw(i);
            if (FAILED(hr)) {
                printf("  Pattern::Draw failed 0x%08lX\n", static_cast<unsigned long>(hr));
                return 2;
            }
            in.kind = InputKind::TextureBGRA;
            in.texture = pattern.Texture();
        } else {
            // --still repeats one picture so that every P macroblock after the first key frame has a
            // zero residual and a zero motion vector: that is the only way to exercise P_Skip and
            // mb_skip_run, and it is also what a desktop capture looks like most of the time.
            MakeSyntheticPicture(src, o.still ? 0u : i);
            if (o.nv12sys) {
                // NV12 on a stride wider than the picture: the luma plane, then one plane of
                // interleaved Cb,Cr pairs at half the vertical resolution. Both the plane order and
                // the stride are what a wrong import would get wrong, and the reconstruction check
                // and the ReadSource check below both see it.
                const uint32_t pitch = o.width + 64u;
                nv12y.assign(static_cast<size_t>(pitch) * o.height, 0x55);
                nv12c.assign(static_cast<size_t>(pitch) * (o.height / 2u), 0x55);
                for (uint32_t yy = 0; yy < o.height; ++yy) {
                    memcpy(&nv12y[static_cast<size_t>(yy) * pitch],
                           &src.y[static_cast<size_t>(yy) * o.width], o.width);
                }
                for (uint32_t yy = 0; yy < o.height / 2u; ++yy) {
                    uint8_t* dst = &nv12c[static_cast<size_t>(yy) * pitch];
                    const uint8_t* cb = &src.cb[static_cast<size_t>(yy) * (o.width / 2u)];
                    const uint8_t* cr = &src.cr[static_cast<size_t>(yy) * (o.width / 2u)];
                    for (uint32_t xx = 0; xx < o.width / 2u; ++xx) {
                        dst[xx * 2u] = cb[xx];
                        dst[xx * 2u + 1u] = cr[xx];
                    }
                }
                in.kind = InputKind::Nv12Sys;
                in.planeY = nv12y.data();
                in.planeCb = nv12c.data();
                in.pitchY = pitch;
                in.pitchC = pitch;
            } else {
                in.kind = InputKind::Planar8;
                in.planeY = src.y.data();
                in.planeCb = src.cb.data();
                in.planeCr = src.cr.data();
                in.pitchY = o.width;
                in.pitchC = o.width / 2;
            }
        }

        FrameStats st;
        const double t0 = NowMs();
        hr = enc.EncodeFrame(in, false, frame, &st);
        const double t1 = NowMs();
        if (FAILED(hr)) {
            printf("  EncodeFrame(%u) failed 0x%08lX\n", i, static_cast<unsigned long>(hr));
            return 2;
        }
        totalMs += t1 - t0;
        totalGpuMs += st.gpuMs;
        totalGpuWallMs += st.gpuWallMs;
        totalReadbackMs += st.readbackMs;
        totalCpuMs += st.cpuMs;
        totalCavlcMs += st.cavlcMs;
        totalNalMs += st.nalMs;
        totalRecordMs += st.recordMs;
        totalMapWaitMs += st.mapWaitMs;
        totalBytes += st.bytes;
        skipTotal += st.skippedMbs;
        {
            const GpuStageProfile& pr = enc.Gpu().LastStageProfile();
            if (pr.valid) {
                const int k = st.keyFrame ? 0 : 1;
                ++stagePictures[k];
                stageTotalMs[k] += pr.totalMs;
                for (uint32_t s = 0; s < GpuStageCount; ++s) {
                    stageMs[k][s] += pr.ms[s];
                    stageDispatches[k][s] += pr.dispatches[s];
                }
                if (firstSteps[k].empty()) {
                    firstSteps[k] = pr.steps;
                }
            }
        }
        // The profile has to be of this picture. A GPU time read from a query that had not retired
        // used to leave the previous picture's figure in place, which is how a profile can show a
        // picture that cost nothing; a missing measurement now says so instead. The bounds are the
        // ones the stages cannot break: the GPU cannot have been busy with our dispatches for longer
        // than the stage the thread measured around them, the readback is part of that stage, and the
        // two CPU stages are parts of the CPU half. One millisecond of slack covers the counter's own
        // resolution and the clock difference between the device and the host.
        const double slack = 1.0;
        if (!st.gpuTimingValid || st.gpuMs <= 0.0 || st.gpuMs > st.gpuWallMs + slack ||
            st.readbackMs < 0.0 || st.readbackMs > st.gpuWallMs + slack ||
            st.cavlcMs < 0.0 || st.nalMs < 0.0 ||
            st.cavlcMs + st.nalMs > st.cpuMs + slack) {
            printf("  FAIL picture %u profile: gpu %.3f ms (valid %d) of wall %.3f, readback %.3f, "
                   "cpu %.3f = cavlc %.3f + nal %.3f\n", i, st.gpuMs, st.gpuTimingValid ? 1 : 0,
                   st.gpuWallMs, st.readbackMs, st.cpuMs, st.cavlcMs, st.nalMs);
            ++timingBad;
        }
        if (st.keyFrame) {
            ++keyFrames;
        }
        stream.insert(stream.end(), frame.begin(), frame.end());

        nnzBad += CheckNnzAgreement(enc, wmb, hmb);

        hr = enc.Gpu().ReadReconstruction(recY, recCb, recCr);
        if (FAILED(hr)) {
            printf("  ReadReconstruction(%u) failed 0x%08lX\n", i, static_cast<unsigned long>(hr));
            return 2;
        }
        hr = enc.Gpu().ReadSource(srcY, srcCb, srcCr);
        if (FAILED(hr)) {
            printf("  ReadSource(%u) failed 0x%08lX\n", i, static_cast<unsigned long>(hr));
            return 2;
        }
        // The import oracle: for a picture that came from system memory we know exactly what every
        // sample should be, so the imported source planes have to repeat it. This is what tells a
        // wrong plane order, a wrong stride or a swapped Cb and Cr from a merely different-looking
        // picture, and it is the only check the NV12 system-memory import has.
        if (i == 0 && !o.gpuSource) {
            size_t bad = 0;
            for (uint32_t yy = 0; yy < o.height; ++yy) {
                for (uint32_t xx = 0; xx < o.width; ++xx) {
                    if (srcY[static_cast<size_t>(yy) * padW + xx] !=
                        src.y[static_cast<size_t>(yy) * o.width + xx]) {
                        ++bad;
                    }
                }
            }
            for (uint32_t yy = 0; yy < o.height / 2u; ++yy) {
                for (uint32_t xx = 0; xx < o.width / 2u; ++xx) {
                    const size_t g = static_cast<size_t>(yy) * (padW / 2u) + xx;
                    const size_t c = static_cast<size_t>(yy) * (o.width / 2u) + xx;
                    if (srcCb[g] != src.cb[c] || srcCr[g] != src.cr[c]) {
                        ++bad;
                    }
                }
            }
            if (bad != 0) {
                printf("  FAIL the import changed %zu of the visible samples handed over\n", bad);
                importBad += bad;
            } else {
                printf("  import exact: every visible sample of picture 0 reached the GPU unchanged\n");
            }
        }
        if (o.verbose && i == 0) {
            printf("  macroblock (0,0), reconstruction minus source, luma:\n");
            for (uint32_t yy = 0; yy < 16; ++yy) {
                printf("    ");
                for (uint32_t xx = 0; xx < 16; ++xx) {
                    const size_t p = static_cast<size_t>(yy) * padW + xx;
                    printf("%5d", static_cast<int>(recY[p]) - static_cast<int>(srcY[p]));
                }
                printf("\n");
            }
        }
        recHistoryY.push_back(recY);
        recHistoryCb.push_back(recCb);
        recHistoryCr.push_back(recCr);
        psnrSum += PlanePsnr(srcY.data(), recY.data(), static_cast<size_t>(padW) * padH);

        hr = dec.Feed(frame.data(), frame.size(),
                      static_cast<int64_t>(i) * 10000000LL * cfg.fpsDen / cfg.fpsNum);
        if (FAILED(hr)) {
            printf("  the decoder rejected frame %u: 0x%08lX %ls\n", i,
                   static_cast<unsigned long>(hr), dec.LastError().c_str());
            return 2;
        }
        if (o.verbose) {
            printf("    %4u %s qp %2u %7u bytes  skip %4u  gpu %6.2f ms  readback %6.2f ms  "
                   "cavlc %6.2f ms  nal %6.2f ms\n", i, st.keyFrame ? "IDR" : "P  ", st.qp,
                   st.bytes, st.skippedMbs, st.gpuMs, st.readbackMs, st.cavlcMs, st.nalMs);
        }
    }
    hr = dec.Drain();
    if (FAILED(hr)) {
        printf("  decoder drain failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }

    const std::wstring path = o.outDir + L"\\mfthost-encode.264";
    if (!WriteFile(path, stream)) {
        printf("  could not write %ls\n", path.c_str());
    }

    // The oracle. Our reconstruction, in coded dimensions, has to equal the decoder's output over the
    // visible area, sample for sample, for every picture.
    std::vector<Picture>& out = dec.Pictures();
    printf("  decoder returned %zu of %u pictures\n", out.size(), o.frames);
    if (out.size() != o.frames) {
        printf("  FAIL the decoder did not return one picture per access unit\n");
        return 1;
    }
    uint32_t exact = 0, differing = 0;
    for (uint32_t i = 0; i < o.frames; ++i) {
        uint32_t x = 0, y = 0;
        bool bad = false;
        if (FirstDifference(recHistoryY[i].data(), out[i].y.data(), o.width, o.height, padW,
                            o.width, &x, &y)) {
            printf("  FAIL picture %u luma differs first at (%u,%u): ours %u, decoder %u\n", i, x, y,
                   recHistoryY[i][static_cast<size_t>(y) * padW + x],
                   out[i].y[static_cast<size_t>(y) * o.width + x]);
            if (o.verbose && differing == 0) {
                DumpEdges("luma", recHistoryY[i].data(), padW, out[i].y.data(), o.width, o.width,
                          o.height, x, y);
                DumpMbDifference(recHistoryY[i], out[i], padW, x / 16u, y / 16u);
            }
            bad = true;
        }
        if (FirstDifference(recHistoryCb[i].data(), out[i].cb.data(), o.width / 2,
                            o.height / 2, padW / 2, o.width / 2, &x, &y)) {
            printf("  FAIL picture %u Cb differs first at (%u,%u)\n", i, x, y);
            if (o.verbose && differing == 0) {
                uint32_t count = 0;
                for (uint32_t cy = 0; cy < o.height / 2; ++cy) {
                    for (uint32_t cx = 0; cx < o.width / 2; ++cx) {
                        if (recHistoryCb[i][static_cast<size_t>(cy) * (padW / 2) + cx] !=
                            out[i].cb[static_cast<size_t>(cy) * (o.width / 2) + cx]) {
                            ++count;
                        }
                    }
                }
                printf("  %u of %u Cb samples differ; macroblock (%u,%u), ours minus decoder:\n",
                       count, (o.width / 2) * (o.height / 2), x / 8u, y / 8u);
                // The eight samples across the nearest vertical edge of four, p3..q3, and what the two
                // macroblocks either side of it told the filter.
                {
                    const uint32_t edge = (x & ~3u);
                    printf("    row %u, chroma columns %u..%u  ours ", y, edge - 4u, edge + 3u);
                    for (int k = -4; k < 4; ++k) {
                        printf("%4d", recHistoryCb[i][static_cast<size_t>(y) * (padW / 2) + edge + k]);
                    }
                    printf("\n                                     theirs ");
                    for (int k = -4; k < 4; ++k) {
                        printf("%4d", out[i].cb[static_cast<size_t>(y) * (o.width / 2) + edge + k]);
                    }
                    printf("\n");
                    const uint32_t hedge = ((y + 4u) & ~3u);
                    printf("    column %u, chroma rows %u..%u     ours ", x, hedge - 4u, hedge + 3u);
                    for (int k = -4; k < 4; ++k) {
                        printf("%4d", recHistoryCb[i][(static_cast<size_t>(hedge) + k) * (padW / 2) + x]);
                    }
                    printf("\n                                     theirs ");
                    for (int k = -4; k < 4; ++k) {
                        printf("%4d", out[i].cb[(static_cast<size_t>(hedge) + k) * (o.width / 2) + x]);
                    }
                    printf("\n");
                    if (i > 0) {
                        // With no chroma residual and a zero motion vector the pre-filter chroma of
                        // this picture is the previous picture's filtered chroma, so this window is
                        // the actual input to the filtering under test.
                        const uint32_t wx0 = (x >= 8u) ? (x - 8u) : 0u;
                        const uint32_t wy0 = (y >= 8u) ? (y - 8u) : 0u;
                        printf("    picture %u, Cb rows %u..%u of columns %u..%u:\n", i - 1, wy0,
                               wy0 + 15u, wx0, wx0 + 15u);
                        for (uint32_t r = 0; r < 16; ++r) {
                            printf("     ");
                            for (uint32_t c = 0; c < 16; ++c) {
                                printf("%4d", out[i - 1].cb[static_cast<size_t>(wy0 + r) *
                                                            (o.width / 2) + wx0 + c]);
                            }
                            printf("\n");
                        }
                        printf("    the same column one picture earlier    ours ");
                        for (int k = -4; k < 4; ++k) {
                            printf("%4d",
                                   recHistoryCb[i - 1][(static_cast<size_t>(hedge) + k) * (padW / 2) + x]);
                        }
                        printf("\n                                     theirs ");
                        for (int k = -4; k < 4; ++k) {
                            printf("%4d",
                                   out[i - 1].cb[(static_cast<size_t>(hedge) + k) * (o.width / 2) + x]);
                        }
                        printf("\n");
                    }
                    const std::vector<MbInfo>& info = enc.LastMbInfo();
                    const uint32_t cols[6] = { x / 8u - 1u, x / 8u, x / 8u - 1u, x / 8u,
                                               x / 8u - 1u, x / 8u };
                    const uint32_t rows[6] = { y / 8u - 1u, y / 8u - 1u, y / 8u, y / 8u,
                                               y / 8u + 1u, y / 8u + 1u };
                    for (uint32_t m = 0; m < 6; ++m) {
                        if (rows[m] >= hmb || cols[m] >= wmb) {
                            continue;
                        }
                        const MbInfo& mb = info[static_cast<size_t>(rows[m]) * wmb + cols[m]];
                        printf("    mb (%u,%u) intra %u cbpL %2u cbpC %u mv (%d,%d) nnzY",
                               cols[m], rows[m], MbIsIntra(mb) ? 1u : 0u, mb.cbpLuma, mb.cbpChroma,
                               mb.mvx, mb.mvy);
                        for (uint32_t b = 0; b < 16; ++b) {
                            printf("%2u", MbNnzLuma(mb, b));
                        }
                        printf("\n");
                    }
                }
                for (uint32_t cy = 0; cy < 8; ++cy) {
                    printf("    ");
                    for (uint32_t cx = 0; cx < 8; ++cx) {
                        const uint32_t px = (x / 8u) * 8u + cx;
                        const uint32_t py = (y / 8u) * 8u + cy;
                        printf("%5d",
                               static_cast<int>(recHistoryCb[i][static_cast<size_t>(py) * (padW / 2) + px]) -
                               static_cast<int>(out[i].cb[static_cast<size_t>(py) * (o.width / 2) + px]));
                    }
                    printf("\n");
                }
            }
            bad = true;
        }
        if (FirstDifference(recHistoryCr[i].data(), out[i].cr.data(), o.width / 2,
                            o.height / 2, padW / 2, o.width / 2, &x, &y)) {
            printf("  FAIL picture %u Cr differs first at (%u,%u)\n", i, x, y);
            bad = true;
        }
        if (o.probeX >= 0) {
            const uint32_t px = static_cast<uint32_t>(o.probeX);
            const uint32_t py = static_cast<uint32_t>(o.probeY);
            printf("    probe picture %u, Cb column %u rows %u..%u  ours ", i, px,
                   (py >= 4u) ? (py - 4u) : 0u, py + 3u);
            for (int k = -4; k < 4; ++k) {
                printf("%4d", recHistoryCb[i][(static_cast<size_t>(py) + k) * (padW / 2) + px]);
            }
            printf("\n                                            theirs ");
            for (int k = -4; k < 4; ++k) {
                printf("%4d", out[i].cb[(static_cast<size_t>(py) + k) * (o.width / 2) + px]);
            }
            printf("\n");
        }
        if (bad) {
            ++differing;
            if (differing >= 3) {
                printf("  (stopping after three differing pictures)\n");
                break;
            }
        } else {
            ++exact;
        }
    }

    const double bitrate = (totalBytes * 8.0 * o.fps) / static_cast<double>(o.frames);
    printf("  %u of %u pictures bit exact against the inbox decoder\n", exact, o.frames);
    printf("  nnz agreement GPU vs CPU: %s (%u disagreements)\n", (nnzBad == 0) ? "exact" : "FAIL",
           nnzBad);
    printf("  key frames %u, skipped macroblocks %u of %u\n", keyFrames, skipTotal,
           o.frames * wmb * hmb);
    printf("  %llu bytes, %.0f bit/s at %u Hz, mean PSNR(Y) %.2f dB\n",
           static_cast<unsigned long long>(totalBytes), bitrate, o.fps, psnrSum / o.frames);
    printf("  %.2f ms per picture total, %.2f ms of it on the GPU, %.1f pictures per second\n",
           totalMs / o.frames, totalGpuMs / o.frames, 1000.0 * o.frames / totalMs);
    // The stages, per picture. The GPU stage is wall clock around the dispatches, so it contains the
    // wait for the GPU: readback is that wait plus the transfer. The CPU half is entropy coding plus
    // byte stream assembly, and what the two do not account for is the little that lies between
    // them. Every figure is of this run; nothing is carried over from another.
    printf("  per picture: gpu stage %.2f ms (gpu busy %.2f, readback %.2f), cpu %.2f ms "
           "(cavlc %.2f, nal %.2f)\n",
           totalGpuWallMs / o.frames, totalGpuMs / o.frames, totalReadbackMs / o.frames,
           totalCpuMs / o.frames, totalCavlcMs / o.frames, totalNalMs / o.frames);
    printf("  per picture: command recording %.2f ms, map wait %.2f ms, readback transfer %.2f ms\n",
           totalRecordMs / o.frames, totalMapWaitMs / o.frames,
           (totalReadbackMs - totalMapWaitMs) / o.frames);
    printf("  per picture profile accounted for every picture: %s\n",
           (timingBad == 0) ? "yes" : "FAIL");
    PrintStageProfile(stagePictures, stageDispatches, stageMs, stageTotalMs, firstSteps);
    printf("  stream written to %ls\n", path.c_str());

    dec.Shutdown();
    enc.Shutdown();
    return (exact == o.frames && nnzBad == 0 && importBad == 0 && timingBad == 0) ? 0 : 1;
}

} // namespace test
} // namespace bc250h264

// ---------------------------------------------------------------- entry point

using namespace bc250h264;
using namespace bc250h264::test;

namespace {

struct MfInit {
    HRESULT hr = E_FAIL;
    MfInit()
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        }
    }
    ~MfInit()
    {
        if (SUCCEEDED(hr)) {
            MFShutdown();
        }
        CoUninitialize();
    }
};

uint32_t ArgU(int argc, wchar_t** argv, int i, uint32_t fallback)
{
    if (i + 1 >= argc) {
        return fallback;
    }
    return static_cast<uint32_t>(_wtoi(argv[i + 1]));
}

void Usage()
{
    printf("usage: mfthost.exe [--selftest|--encode|--mft|--compare|--sinkwriter|--all]\n");
    printf("                   [--width N] [--height N] [--frames N] [--qp N] [--bitrate N]\n");
    printf("                   [--gop N] [--fps N] [--deblock] [--gpu-source] [--still] [--cbr]\n");
    printf("                   [--verbose] [--out <directory>]\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Unbuffered: every line has to reach a redirected file before the next call, or a stall inside
    // Media Foundation leaves no trace of how far the run got. That is how the sink writer stall was
    // first mistaken for a hang before main.
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    bool doSelf = false, doEncode = false, doMft = false, doCompare = false, doSink = false;
    bool doTexin = false;
    bool any = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (wcscmp(a, L"--help") == 0 || wcscmp(a, L"-h") == 0) {
            Usage();
            return 0;
        } else if (wcscmp(a, L"--selftest") == 0) {
            doSelf = true; any = true;
        } else if (wcscmp(a, L"--encode") == 0) {
            doEncode = true; any = true;
        } else if (wcscmp(a, L"--mft") == 0) {
            doMft = true; any = true;
        } else if (wcscmp(a, L"--texin") == 0) {
            doTexin = true; any = true;
        } else if (wcscmp(a, L"--compare") == 0) {
            doCompare = true; any = true;
        } else if (wcscmp(a, L"--sinkwriter") == 0) {
            doSink = true; any = true;
        } else if (wcscmp(a, L"--all") == 0) {
            doSelf = doEncode = doMft = doCompare = doSink = doTexin = true; any = true;
        } else if (wcscmp(a, L"--width") == 0) {
            o.width = ArgU(argc, argv, i, o.width); ++i;
        } else if (wcscmp(a, L"--height") == 0) {
            o.height = ArgU(argc, argv, i, o.height); ++i;
        } else if (wcscmp(a, L"--frames") == 0) {
            o.frames = ArgU(argc, argv, i, o.frames); ++i;
        } else if (wcscmp(a, L"--qp") == 0) {
            o.qp = ArgU(argc, argv, i, o.qp); ++i;
        } else if (wcscmp(a, L"--bitrate") == 0) {
            o.bitrate = ArgU(argc, argv, i, o.bitrate); ++i;
        } else if (wcscmp(a, L"--gop") == 0) {
            o.gop = ArgU(argc, argv, i, o.gop); ++i;
        } else if (wcscmp(a, L"--fps") == 0) {
            o.fps = ArgU(argc, argv, i, o.fps); ++i;
        } else if (wcscmp(a, L"--deblock") == 0) {
            o.deblock = true;
        } else if (wcscmp(a, L"--gpu-source") == 0) {
            o.gpuSource = true;
        } else if (wcscmp(a, L"--nv12-sys") == 0) {
            o.nv12sys = true;
        } else if (wcscmp(a, L"--probe") == 0) {
            o.probeX = static_cast<int32_t>(ArgU(argc, argv, i, 0)); ++i;
            o.probeY = static_cast<int32_t>(ArgU(argc, argv, i, 0)); ++i;
        } else if (wcscmp(a, L"--no-hw-transforms") == 0) {
            o.noHwTransforms = true;
        } else if (wcscmp(a, L"--still") == 0) {
            o.still = true;
        } else if (wcscmp(a, L"--cbr") == 0) {
            o.rc = RateControl::Cbr;
        } else if (wcscmp(a, L"--verbose") == 0) {
            o.verbose = true;
        } else if (wcscmp(a, L"--out") == 0 && i + 1 < argc) {
            o.outDir = argv[i + 1]; ++i;
        } else {
            printf("unknown option %ls\n", a);
            Usage();
            return 1;
        }
    }
    if (!any) {
        Usage();
        return 1;
    }

    CreateDirectoryW(o.outDir.c_str(), nullptr);

    int rc = 0;
    if (doSelf) {
        // Deliberately before MFStartup: the table checks need nothing but the process.
        rc |= RunSelfTest();
        printf("\n");
    }
    if (doEncode || doMft || doTexin || doCompare || doSink) {
        MfInit mf;
        if (FAILED(mf.hr)) {
            printf("MFStartup failed 0x%08lX\n", static_cast<unsigned long>(mf.hr));
            return 2;
        }
        if (doEncode) {
            rc |= RunEncode(o);
            printf("\n");
        }
        if (doMft) {
            rc |= RunMft(o);
            printf("\n");
        }
        if (doTexin) {
            rc |= RunTextureInput(o);
            printf("\n");
        }
        if (doCompare) {
            rc |= RunCompare(o);
            printf("\n");
        }
        if (doSink) {
            rc |= RunSinkWriter(o);
            printf("\n");
        }
    }
    printf("%s\n", (rc == 0) ? "ALL PASS" : "FAILURES PRESENT");
    return rc;
}
