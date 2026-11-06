// SPDX-License-Identifier: MIT
// vpblt: a picture check of the D3D11 video processor (BD-223). It blits colour-bar patterns through
// ID3D11VideoContext::VideoProcessorBlt, reads the result back and compares the centre of each bar with a
// reference that it computes itself from ITU-R BT.601 and BT.709 (Kr/Kb, studio and full range). It uses only the
// public D3D11 video API, so the same binary is the positive control on any other GPU (the development PC) and the
// check of our driver on the lab. No window, no swap chain; the run takes a few seconds.
//
//   vpblt.exe --out <file.json> [--adapter <index>] [--debug] [--mutant matrix|chroma]
//
// --debug turns the D3D11 debug layer on and prints its messages (host diagnosis; the lab has no debug layer).
// --mutant replaces the reference with a wrong one (601 and 709 exchanged, or the studio chroma range unexpanded as
// DXVK did before BD-223 V6): a correct processor must then FAIL, which shows that the comparison can see that error.
//
// Exit 0: every case that ran matched (VPBLT-PASS). Exit 1: a case gave a wrong pixel (VPBLT-FAIL). Exit 3: no
// D3D11 device or no video device (VPBLT-SETUP). Exit 2: usage. Last line:
//   VPBLT <PASS|FAIL|SETUP> bits=<32|64> vendor=<hex> cases=<n> pass=<p> skipped=<s> first=<case/bar/channel got want|->
// A case whose formats the processor does not admit (CheckVideoProcessorFormat) is SKIPPED, not failed; the JSON
// names each such case, so a skipped case is visible.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

template <class T> struct Ref {
    T *p = nullptr;
    Ref() = default;
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    ~Ref() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T **put() { reset(); return &p; }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

constexpr UINT kBars = 8, kBarWidth = 32, kWidth = kBars * kBarWidth, kHeight = 64;

// The bars as R'G'B' in 0..1: 100 % white, 75 % yellow, cyan, green, magenta, 100 % red, 75 % blue, black.
struct Rgb { double r, g, b; };
const Rgb kBarColours[kBars] = {
    {1.0, 1.0, 1.0}, {0.75, 0.75, 0.0}, {0.0, 0.75, 0.75}, {0.0, 0.75, 0.0},
    {0.75, 0.0, 0.75}, {1.0, 0.0, 0.0}, {0.0, 0.0, 0.75}, {0.0, 0.0, 0.0},
};

struct Space { bool bt709; bool studio; };

void coefficients(bool bt709, double &kr, double &kb) {
    kr = bt709 ? 0.2126 : 0.299;
    kb = bt709 ? 0.0722 : 0.114;
}

// R'G'B' to Y' 0..1 and Cb, Cr -0.5..0.5.
void to_ycbcr(const Rgb &c, bool bt709, double &y, double &cb, double &cr) {
    double kr, kb;
    coefficients(bt709, kr, kb);
    y = kr * c.r + (1.0 - kr - kb) * c.g + kb * c.b;
    cb = (c.b - y) / (2.0 * (1.0 - kb));
    cr = (c.r - y) / (2.0 * (1.0 - kr));
}

Rgb to_rgb(double y, double cb, double cr, bool bt709) {
    double kr, kb;
    coefficients(bt709, kr, kb);
    const double kg = 1.0 - kr - kb;
    const double r = y + 2.0 * (1.0 - kr) * cr;
    const double b = y + 2.0 * (1.0 - kb) * cb;
    const double g = (y - kr * r - kb * b) / kg;
    return {r, g, b};
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Codes of a YCbCr sample at <bits> (8 or 10): studio Y 16..235 and C 16..240 scaled by 2^(bits-8), full 0..max with
// the chroma centre at 2^(bits-1).
struct YccCodes { double y, cb, cr; };
YccCodes encode_ycc(const Rgb &c, Space s, int bits) {
    double y, cb, cr;
    to_ycbcr(c, s.bt709, y, cb, cr);
    const double scale = double(1 << (bits - 8)), maxc = double((1 << bits) - 1), centre = double(1 << (bits - 1));
    YccCodes out;
    if (s.studio) {
        out.y = (16.0 + 219.0 * y) * scale;
        out.cb = centre + 224.0 * scale * cb;
        out.cr = centre + 224.0 * scale * cr;
    } else {
        out.y = maxc * y;
        out.cb = centre + maxc * cb;
        out.cr = centre + maxc * cr;
    }
    out.y = clampd(std::round(out.y), 0.0, maxc);
    out.cb = clampd(std::round(out.cb), 0.0, maxc);
    out.cr = clampd(std::round(out.cr), 0.0, maxc);
    return out;
}

// --mutant (negative control of the probe itself): a wrong reference that a correct processor must fail against.
// "matrix" decodes with 601 and 709 exchanged; "chroma" leaves the studio chroma range unexpanded (DXVK before
// BD-223 V6).
enum class Mutant { None, Matrix, Chroma };
Mutant g_mutant = Mutant::None;

Rgb decode_ycc(const YccCodes &c, Space s, int bits) {
    const double scale = double(1 << (bits - 8)), maxc = double((1 << bits) - 1), centre = double(1 << (bits - 1));
    double y, cb, cr;
    if (g_mutant == Mutant::Matrix) s.bt709 = !s.bt709;
    if (s.studio) {
        const double chroma = g_mutant == Mutant::Chroma ? 255.0 : 224.0;
        y = (c.y / scale - 16.0) / 219.0;
        cb = (c.cb - centre) / (chroma * scale);
        cr = (c.cr - centre) / (chroma * scale);
    } else {
        y = c.y / maxc;
        cb = (c.cb - centre) / maxc;
        cr = (c.cr - centre) / maxc;
    }
    return to_rgb(y, cb, cr, s.bt709);
}

// 8-bit RGB codes, full or studio (16..235).
double rgb_code(double v, bool studio) {
    v = clampd(v, 0.0, 1.0);
    return studio ? 16.0 + 219.0 * v : 255.0 * v;
}

enum class Kind { Nv12, P010, Yuy2, Bgra };

DXGI_FORMAT dxgi_of(Kind k) {
    switch (k) {
    case Kind::Nv12: return DXGI_FORMAT_NV12;
    case Kind::P010: return DXGI_FORMAT_P010;
    case Kind::Yuy2: return DXGI_FORMAT_YUY2;
    default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

const char *name_of(Kind k) {
    switch (k) {
    case Kind::Nv12: return "NV12";
    case Kind::P010: return "P010";
    case Kind::Yuy2: return "YUY2";
    default: return "BGRA8";
    }
}

struct Case {
    const char *name;
    Kind in;
    Space inSpace;        // for a YCbCr input; an RGB input uses inRgbStudio
    bool inRgbStudio;
    Kind out;
    Space outSpace;       // for a YCbCr output
    bool outRgbStudio;
    bool colorSpace1;     // set the spaces through ID3D11VideoContext1 (DXGI_COLOR_SPACE_TYPE), as Chromium does
    // The upload of a software-decoded frame as Chromium does it (UploadVideoImage): an NV12 texture with
    // USAGE_DYNAMIC, BIND_DECODER and CPU write, filled through Map(WRITE_DISCARD), then an input view on it.
    bool dynamicUpload = false;
};

const Case kCases[] = {
    {"nv12-709s-bgra", Kind::Nv12, {true, true}, false, Kind::Bgra, {true, true}, false, false},
    {"nv12-601s-bgra", Kind::Nv12, {false, true}, false, Kind::Bgra, {true, true}, false, false},
    {"nv12-709f-bgra", Kind::Nv12, {true, false}, false, Kind::Bgra, {true, true}, false, false},
    {"nv12-709s-bgra-studio", Kind::Nv12, {true, true}, false, Kind::Bgra, {true, true}, true, false},
    {"p010-709s-bgra", Kind::P010, {true, true}, false, Kind::Bgra, {true, true}, false, false},
    {"yuy2-709s-bgra", Kind::Yuy2, {true, true}, false, Kind::Bgra, {true, true}, false, false},
    {"yuy2-601s-bgra", Kind::Yuy2, {false, true}, false, Kind::Bgra, {true, true}, false, false},
    {"bgra-nv12-709s", Kind::Bgra, {true, true}, false, Kind::Nv12, {true, true}, false, false},
    {"bgra-nv12-601s", Kind::Bgra, {true, true}, false, Kind::Nv12, {false, true}, false, false},
    {"nv12-709s-bgra-cs1", Kind::Nv12, {true, true}, false, Kind::Bgra, {true, true}, false, true},
    {"nv12-601s-bgra-cs1", Kind::Nv12, {false, true}, false, Kind::Bgra, {true, true}, false, true},
    {"bgra-nv12-709s-cs1", Kind::Bgra, {true, true}, false, Kind::Nv12, {true, true}, false, true},
    {"nv12dyn-709s-bgra", Kind::Nv12, {true, true}, false, Kind::Bgra, {true, true}, false, false, true},
    {"nv12dyn-709s-bgra-cs1", Kind::Nv12, {true, true}, false, Kind::Bgra, {true, true}, false, true, true},
};

bool is_ycc(Kind k) { return k != Kind::Bgra; }

DXGI_COLOR_SPACE_TYPE dxgi_space(bool ycc, Space s, bool rgbStudio) {
    if (!ycc) return rgbStudio ? DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (s.bt709) return s.studio ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    return s.studio ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE legacy_space(bool ycc, Space s, bool rgbStudio) {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE c = {};
    c.RGB_Range = (!ycc && rgbStudio) ? 1 : 0;
    c.YCbCr_Matrix = s.bt709 ? 1 : 0;
    c.Nominal_Range = s.studio ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    return c;
}

// The initial data of the input texture: Y plane then the CbCr plane for NV12/P010 (same pitch), packed for YUY2.
std::vector<uint8_t> make_input(const Case &c, UINT &pitch) {
    std::vector<uint8_t> data;
    switch (c.in) {
    case Kind::Nv12: {
        pitch = kWidth;
        data.assign(size_t(pitch) * kHeight * 3 / 2, 0);
        for (UINT x = 0; x < kWidth; ++x) {
            const auto v = encode_ycc(kBarColours[x / kBarWidth], c.inSpace, 8);
            for (UINT y = 0; y < kHeight; ++y) data[size_t(y) * pitch + x] = uint8_t(v.y);
            if (x % 2 == 0)
                for (UINT y = 0; y < kHeight / 2; ++y) {
                    uint8_t *uv = &data[size_t(kHeight + y) * pitch + x];
                    uv[0] = uint8_t(v.cb);
                    uv[1] = uint8_t(v.cr);
                }
        }
        break;
    }
    case Kind::P010: {
        pitch = kWidth * 2;
        data.assign(size_t(pitch) * kHeight * 3 / 2, 0);
        auto put = [&](size_t at, double code) { const uint16_t w = uint16_t(unsigned(code) << 6); std::memcpy(&data[at], &w, 2); };
        for (UINT x = 0; x < kWidth; ++x) {
            const auto v = encode_ycc(kBarColours[x / kBarWidth], c.inSpace, 10);
            for (UINT y = 0; y < kHeight; ++y) put(size_t(y) * pitch + size_t(x) * 2, v.y);
            if (x % 2 == 0)
                for (UINT y = 0; y < kHeight / 2; ++y) {
                    const size_t at = size_t(kHeight + y) * pitch + size_t(x) * 2;
                    put(at, v.cb);
                    put(at + 2, v.cr);
                }
        }
        break;
    }
    case Kind::Yuy2: {
        pitch = kWidth * 2;
        data.assign(size_t(pitch) * kHeight, 0);
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; x += 2) {
                const auto v = encode_ycc(kBarColours[x / kBarWidth], c.inSpace, 8);
                uint8_t *p = &data[size_t(y) * pitch + size_t(x) * 2];
                p[0] = uint8_t(v.y); p[1] = uint8_t(v.cb); p[2] = uint8_t(v.y); p[3] = uint8_t(v.cr);
            }
        break;
    }
    case Kind::Bgra: {
        pitch = kWidth * 4;
        data.assign(size_t(pitch) * kHeight, 0);
        for (UINT y = 0; y < kHeight; ++y)
            for (UINT x = 0; x < kWidth; ++x) {
                const Rgb &col = kBarColours[x / kBarWidth];
                uint8_t *p = &data[size_t(y) * pitch + size_t(x) * 4];
                p[0] = uint8_t(std::round(rgb_code(col.b, c.inRgbStudio)));
                p[1] = uint8_t(std::round(rgb_code(col.g, c.inRgbStudio)));
                p[2] = uint8_t(std::round(rgb_code(col.r, c.inRgbStudio)));
                p[3] = 255;
            }
        break;
    }
    }
    return data;
}

// The colour each bar's input codes stand for: the stored input decoded exactly, so the input quantization is not
// counted as an error of the processor.
Rgb input_colour(const Case &c, UINT bar) {
    if (c.in == Kind::Bgra) {
        const Rgb &col = kBarColours[bar];
        auto back = [&](double v) {
            const double code = std::round(rgb_code(v, c.inRgbStudio));
            return c.inRgbStudio ? (code - 16.0) / 219.0 : code / 255.0;
        };
        return {back(col.r), back(col.g), back(col.b)};
    }
    const int bits = c.in == Kind::P010 ? 10 : 8;
    return decode_ycc(encode_ycc(kBarColours[bar], c.inSpace, bits), c.inSpace, bits);
}

struct Result {
    std::string name, status, detail;   // status: PASS, FAIL, SKIPPED, ERROR
    double maxError = 0.0;
    std::vector<std::string> samples;
};

std::string hr_text(HRESULT hr) { char b[16]; std::snprintf(b, sizeof(b), "%08lX", static_cast<unsigned long>(hr)); return b; }

constexpr double kTolerance = 2.0;   // 8-bit codes

Result run_case(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11VideoDevice *video, ID3D11VideoContext *vctx,
                const Case &c) {
    Result r;
    r.name = c.name;
    auto fail = [&](const char *step, HRESULT hr) { r.status = "ERROR"; r.detail = std::string(step) + " hr=" + hr_text(hr); return r; };

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {30, 1};
    cd.InputWidth = kWidth; cd.InputHeight = kHeight;
    cd.OutputFrameRate = {30, 1};
    cd.OutputWidth = kWidth; cd.OutputHeight = kHeight;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    Ref<ID3D11VideoProcessorEnumerator> en;
    HRESULT hr = video->CreateVideoProcessorEnumerator(&cd, en.put());
    if (FAILED(hr)) return fail("CreateVideoProcessorEnumerator", hr);
    UINT inFlags = 0, outFlags = 0;
    hr = en->CheckVideoProcessorFormat(dxgi_of(c.in), &inFlags);
    if (FAILED(hr)) return fail("CheckVideoProcessorFormat(in)", hr);
    hr = en->CheckVideoProcessorFormat(dxgi_of(c.out), &outFlags);
    if (FAILED(hr)) return fail("CheckVideoProcessorFormat(out)", hr);
    if (!(inFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(outFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        r.status = "SKIPPED";
        r.detail = std::string(name_of(c.in)) + " input " + std::to_string(inFlags) + ", " + name_of(c.out) + " output " + std::to_string(outFlags);
        return r;
    }
    Ref<ID3D11VideoProcessor> vp;
    hr = video->CreateVideoProcessor(en.p, 0, vp.put());
    if (FAILED(hr)) return fail("CreateVideoProcessor", hr);

    UINT pitch = 0;
    const auto initial = make_input(c, pitch);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kWidth; td.Height = kHeight; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = dxgi_of(c.in); td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    // The runtime admits an input surface with DECODER, VIDEO_ENCODER, RENDER_TARGET, UNORDERED_ACCESS or no bind flag
    // (D3D11 debug layer, CreateVideoProcessorInputView).
    td.BindFlags = 0;
    D3D11_SUBRESOURCE_DATA sd = {initial.data(), pitch, 0};
    Ref<ID3D11Texture2D> inTex;
    if (c.dynamicUpload) {
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_DECODER;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateTexture2D(&td, nullptr, inTex.put());
        if (FAILED(hr)) return fail("CreateTexture2D(in, DYNAMIC DECODER)", hr);
        D3D11_MAPPED_SUBRESOURCE wm = {};
        hr = context->Map(inTex.p, 0, D3D11_MAP_WRITE_DISCARD, 0, &wm);
        if (FAILED(hr)) return fail("Map(in, WRITE_DISCARD)", hr);
        // NV12 as mapped: kHeight rows of Y, then kHeight / 2 rows of CbCr, each at RowPitch.
        auto *dst = static_cast<uint8_t *>(wm.pData);
        for (UINT row = 0; row < kHeight * 3 / 2; ++row)
            std::memcpy(dst + size_t(row) * wm.RowPitch, initial.data() + size_t(row) * pitch, pitch);
        context->Unmap(inTex.p, 0);
    } else {
        hr = device->CreateTexture2D(&td, &sd, inTex.put());
        if (FAILED(hr)) return fail("CreateTexture2D(in)", hr);
    }
    D3D11_TEXTURE2D_DESC od = td;
    od.Format = dxgi_of(c.out);
    od.Usage = D3D11_USAGE_DEFAULT;
    od.CPUAccessFlags = 0;
    od.BindFlags = D3D11_BIND_RENDER_TARGET;
    Ref<ID3D11Texture2D> outTex;
    hr = device->CreateTexture2D(&od, nullptr, outTex.put());
    if (FAILED(hr)) return fail("CreateTexture2D(out)", hr);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Ref<ID3D11VideoProcessorInputView> iv;
    hr = video->CreateVideoProcessorInputView(inTex.p, en.p, &ivd, iv.put());
    if (FAILED(hr)) return fail("CreateVideoProcessorInputView", hr);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Ref<ID3D11VideoProcessorOutputView> ov;
    hr = video->CreateVideoProcessorOutputView(outTex.p, en.p, &ovd, ov.put());
    if (FAILED(hr)) return fail("CreateVideoProcessorOutputView", hr);

    if (c.colorSpace1) {
        Ref<ID3D11VideoContext1> v1;
        hr = vctx->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(v1.put()));
        if (FAILED(hr)) return fail("QueryInterface(ID3D11VideoContext1)", hr);
        v1->VideoProcessorSetStreamColorSpace1(vp.p, 0, dxgi_space(is_ycc(c.in), c.inSpace, c.inRgbStudio));
        v1->VideoProcessorSetOutputColorSpace1(vp.p, dxgi_space(is_ycc(c.out), c.outSpace, c.outRgbStudio));
    } else {
        const auto ins = legacy_space(is_ycc(c.in), c.inSpace, c.inRgbStudio);
        const auto outs = legacy_space(is_ycc(c.out), c.outSpace, c.outRgbStudio);
        vctx->VideoProcessorSetStreamColorSpace(vp.p, 0, &ins);
        vctx->VideoProcessorSetOutputColorSpace(vp.p, &outs);
    }
    vctx->VideoProcessorSetStreamFrameFormat(vp.p, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.p, 0, FALSE);
    const RECT full = {0, 0, LONG(kWidth), LONG(kHeight)};
    vctx->VideoProcessorSetStreamSourceRect(vp.p, 0, TRUE, &full);
    vctx->VideoProcessorSetStreamDestRect(vp.p, 0, TRUE, &full);
    vctx->VideoProcessorSetOutputTargetRect(vp.p, TRUE, &full);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = iv.p;
    hr = vctx->VideoProcessorBlt(vp.p, ov.p, 0, 1, &stream);
    if (FAILED(hr)) return fail("VideoProcessorBlt", hr);

    D3D11_TEXTURE2D_DESC st = od;
    st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Ref<ID3D11Texture2D> staging;
    hr = device->CreateTexture2D(&st, nullptr, staging.put());
    if (FAILED(hr)) return fail("CreateTexture2D(staging)", hr);
    context->CopyResource(staging.p, outTex.p);
    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = context->Map(staging.p, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return fail("Map(staging)", hr);
    const auto *base = static_cast<const uint8_t *>(m.pData);
    r.status = "PASS";
    const UINT sy = kHeight / 2;
    for (UINT bar = 0; bar < kBars; ++bar) {
        const UINT sx = bar * kBarWidth + kBarWidth / 2;
        const Rgb col = input_colour(c, bar);
        double want[3], got[3];
        const char *ch[3];
        if (c.out == Kind::Bgra) {
            const uint8_t *p = base + size_t(sy) * m.RowPitch + size_t(sx) * 4;
            want[0] = rgb_code(col.r, c.outRgbStudio); want[1] = rgb_code(col.g, c.outRgbStudio); want[2] = rgb_code(col.b, c.outRgbStudio);
            got[0] = p[2]; got[1] = p[1]; got[2] = p[0];
            ch[0] = "R"; ch[1] = "G"; ch[2] = "B";
        } else {
            double y, cb, cr;
            to_ycbcr({clampd(col.r, 0, 1), clampd(col.g, 0, 1), clampd(col.b, 0, 1)}, c.outSpace.bt709, y, cb, cr);
            want[0] = c.outSpace.studio ? 16.0 + 219.0 * y : 255.0 * y;
            want[1] = 128.0 + (c.outSpace.studio ? 224.0 : 255.0) * cb;
            want[2] = 128.0 + (c.outSpace.studio ? 224.0 : 255.0) * cr;
            const uint8_t *uv = base + size_t(kHeight + sy / 2) * m.RowPitch + size_t(sx & ~1u);
            got[0] = base[size_t(sy) * m.RowPitch + sx]; got[1] = uv[0]; got[2] = uv[1];
            ch[0] = "Y"; ch[1] = "Cb"; ch[2] = "Cr";
        }
        char line[160];
        std::snprintf(line, sizeof(line), "bar%u %s=%.0f/%.1f %s=%.0f/%.1f %s=%.0f/%.1f", bar, ch[0], got[0], want[0],
                      ch[1], got[1], want[1], ch[2], got[2], want[2]);
        r.samples.emplace_back(line);
        for (int i = 0; i < 3; ++i) {
            const double e = std::fabs(got[i] - want[i]);
            if (e > r.maxError) r.maxError = e;
            if (e > kTolerance && r.status == "PASS") {
                r.status = "FAIL";
                char d[96];
                std::snprintf(d, sizeof(d), "%s/bar%u/%s got %.0f want %.1f", c.name, bar, ch[i], got[i], want[i]);
                r.detail = d;
            }
        }
    }
    context->Unmap(staging.p, 0);
    return r;
}

std::string json_text(const std::string &s) {
    std::string o;
    for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
    return o;
}

}  // namespace

int main(int argc, char **argv) {
    std::string out;
    UINT adapterIndex = 0;
    bool debugLayer = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { std::printf("vpblt.exe --out <file.json> [--adapter <index>] [--debug] [--mutant matrix|chroma]\n"); return 0; }
        if (a == "--debug") { debugLayer = true; continue; }
        if (a == "--mutant" && i + 1 < argc) {
            const std::string m = argv[++i];
            if (m == "matrix") g_mutant = Mutant::Matrix;
            else if (m == "chroma") g_mutant = Mutant::Chroma;
            else { std::fprintf(stderr, "vpblt: unknown mutant %s\n", m.c_str()); return 2; }
            continue;
        }
        if (a == "--out" && i + 1 < argc) { out = argv[++i]; continue; }
        if (a == "--adapter" && i + 1 < argc) { adapterIndex = UINT(std::strtoul(argv[++i], nullptr, 10)); continue; }
        std::fprintf(stderr, "vpblt: unknown argument %s\n", a.c_str());
        return 2;
    }
    if (out.empty()) { std::fprintf(stderr, "vpblt: --out is required\n"); return 2; }
    const unsigned bits = unsigned(sizeof(void *) * 8);

    std::string setup, vendor = "-", adapterName;
    Ref<IDXGIFactory1> factory;
    Ref<IDXGIAdapter1> adapter;
    Ref<ID3D11Device> device;
    Ref<ID3D11DeviceContext> context;
    Ref<ID3D11VideoDevice> video;
    Ref<ID3D11VideoContext> vctx;
    std::string interfaces;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.put()));
    if (FAILED(hr)) setup = "CreateDXGIFactory1 hr=" + hr_text(hr);
    if (setup.empty()) {
        hr = factory->EnumAdapters1(adapterIndex, adapter.put());
        if (FAILED(hr)) setup = "EnumAdapters1 hr=" + hr_text(hr);
    }
    if (setup.empty()) {
        DXGI_ADAPTER_DESC1 ad = {};
        adapter->GetDesc1(&ad);
        char v[32];
        std::snprintf(v, sizeof(v), "%04X:%04X", ad.VendorId, ad.DeviceId);
        vendor = v;
        char n[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, n, sizeof(n) - 1, nullptr, nullptr);
        adapterName = n;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        // Chromium creates its decode and overlay device with VIDEO_SUPPORT; so does the probe.
        const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | (debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0u);
        hr = D3D11CreateDevice(adapter.p, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                               D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        if (FAILED(hr)) setup = "D3D11CreateDevice(VIDEO_SUPPORT) hr=" + hr_text(hr);
    }
    if (setup.empty()) {
        hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(video.put()));
        if (FAILED(hr)) setup = "QueryInterface(ID3D11VideoDevice) hr=" + hr_text(hr);
    }
    if (setup.empty()) {
        hr = context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(vctx.put()));
        if (FAILED(hr)) setup = "QueryInterface(ID3D11VideoContext) hr=" + hr_text(hr);
    }
    if (setup.empty()) {
        // The interfaces Chromium asks for (overlayprobe step 2 needs ID3D11VideoDevice1 and ID3D11VideoContext1).
        IUnknown *u = nullptr;
        const struct { const char *name; IUnknown *on; REFIID iid; } q[] = {
            {"ID3D11VideoDevice1", video.p, __uuidof(ID3D11VideoDevice1)},
            {"ID3D11VideoContext1", vctx.p, __uuidof(ID3D11VideoContext1)},
            {"ID3D11VideoContext2", vctx.p, __uuidof(ID3D11VideoContext2)},
        };
        for (const auto &e : q) {
            const HRESULT qh = e.on->QueryInterface(e.iid, reinterpret_cast<void **>(&u));
            if (SUCCEEDED(qh)) { u->Release(); u = nullptr; }
            interfaces += std::string(interfaces.empty() ? "" : ",") + "\"" + e.name + "\":\"" + hr_text(qh) + "\"";
        }
    }

    std::vector<Result> results;
    if (setup.empty())
        for (const auto &c : kCases) results.push_back(run_case(device.p, context.p, video.p, vctx.p, c));
    // --debug (host diagnosis only; the debug layer is not on the lab): the messages of the D3D11 debug layer.
    if (debugLayer && device) {
        Ref<ID3D11InfoQueue> iq;
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void **>(iq.put())))) {
            const UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n && i < 40; ++i) {
                SIZE_T size = 0;
                iq->GetMessage(i, nullptr, &size);
                std::vector<uint8_t> buffer(size);
                auto *msg = reinterpret_cast<D3D11_MESSAGE *>(buffer.data());
                if (SUCCEEDED(iq->GetMessage(i, msg, &size)))
                    std::printf("DEBUG %.*s\n", int(msg->DescriptionByteLength), msg->pDescription);
            }
        }
    }

    unsigned pass = 0, skipped = 0;
    std::string first = "-";
    for (const auto &r : results) {
        if (r.status == "PASS") ++pass;
        else if (r.status == "SKIPPED") ++skipped;
        else if (first == "-") first = r.status == "FAIL" ? r.detail : r.name + ": " + r.detail;
    }
    const char *verdict = !setup.empty() ? "SETUP" : (first == "-" && pass > 0 ? "PASS" : "FAIL");

    FILE *f = nullptr;
    if (fopen_s(&f, out.c_str(), "wb") == 0 && f) {
        std::fprintf(f, "{\n  \"tool\": \"vpblt\",\n  \"bits\": %u,\n  \"adapter\": \"%s\",\n  \"adapter_name\": \"%s\",\n  \"mutant\": \"%s\",\n", bits,
                     vendor.c_str(), json_text(adapterName).c_str(),
                     g_mutant == Mutant::Matrix ? "matrix" : (g_mutant == Mutant::Chroma ? "chroma" : "none"));
        std::fprintf(f, "  \"setup\": \"%s\",\n  \"interfaces\": {%s},\n  \"tolerance_codes\": %.1f,\n  \"cases\": [\n",
                     json_text(setup).c_str(), interfaces.c_str(), kTolerance);
        for (size_t i = 0; i < results.size(); ++i) {
            const auto &r = results[i];
            std::fprintf(f, "    {\"name\": \"%s\", \"status\": \"%s\", \"detail\": \"%s\", \"max_error\": %.1f, \"samples\": [",
                         r.name.c_str(), r.status.c_str(), json_text(r.detail).c_str(), r.maxError);
            for (size_t s = 0; s < r.samples.size(); ++s)
                std::fprintf(f, "%s\"%s\"", s ? ", " : "", json_text(r.samples[s]).c_str());
            std::fprintf(f, "]}%s\n", i + 1 < results.size() ? "," : "");
        }
        std::fprintf(f, "  ],\n  \"verdict\": \"VPBLT-%s\"\n}\n", verdict);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "vpblt: cannot write %s\n", out.c_str());
    }
    for (const auto &r : results)
        std::printf("CASE %s %s max=%.1f %s\n", r.name.c_str(), r.status.c_str(), r.maxError, r.detail.c_str());
    if (!setup.empty()) first = setup;
    std::printf("VPBLT %s bits=%u vendor=%s cases=%zu pass=%u skipped=%u first=%s\n", verdict, bits, vendor.c_str(),
                results.size(), pass, skipped, first.c_str());
    return !setup.empty() ? 3 : (std::strcmp(verdict, "PASS") == 0 ? 0 : 1);
}
