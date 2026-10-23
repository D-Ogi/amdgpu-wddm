// SPDX-License-Identifier: MIT
// amdgpu_wddm_mesh: native D3D12 mesh shader witness. It draws the same eight triangles three times into three
// offscreen targets and compares the read-backs word for word:
//
//   reference   a vertex program and a pixel program, DrawInstanced(24)
//   mesh-ms     a mesh program and the same pixel program, DispatchMesh(8,1,1)
//   mesh-as     an amplification program, the mesh program and the same pixel program, DispatchMesh(1,1,1)
//
// Offscreen only: no window, no swap chain. The Microsoft runtime is loaded from System32, never an
// application-local copy. The pixel program writes a vertex tag and a primitive tag; the mesh program carries the
// primitive tag on its primitive output, so a mesh pipeline whose primitive output does not reach the pixel input
// fails on every covered pixel instead of passing with a plausible picture.
//
//   --adapter warp|bc250|hw|index:N [--out DIR] [--deadline S] [--debug-layer] [--only ms,as] [--isolate 0|1]
//           [--probe]
//                                         exit 0 all PASS, 1 a FAIL or ERROR (or a debug layer error), 4 SKIP only,
//                                         2 usage, 3 no device or GPU work unretired
//   --selftest                            CPU only: the oracle, its negative controls and the comparator
//
// BD-119. --isolate (the default) gives every pass its own command list, its own submission and its own bounded
// fence wait, and prints one PASS line per pass with the time it took to retire. A pass whose submission does not
// retire is then named: the earlier single-submission form could only say that something in the command list hung,
// which is what left the arm C hang of 2026-10-11 unattributed. --isolate 0 restores that single submission, as
// the control a host gate compares against. --only also gates recording now, not only reporting: the pass that is
// not selected is not drawn at all (the vertex reference is always drawn, because it is what the others are
// compared with).
//
// --probe. Every program also counts its work in a root UAV, one block per pass (shaders\mesh.hlsl, WITNESS_PROBE),
// and one PROBE line per drawn pass prints the counts after the comparison. The image comparison is the same. A
// mesh-as pass that fails with ms_groups=0 launched no mesh group; one with ms_groups=8 and a wrong ms_tri_mask read
// a wrong payload; one with the right masks and ps_invocations=0 declared its primitives but rasterized none.
//
// MeshShaderTier is printed on the FEATURES line. Tier 0, or a highest shader model below 6_5, is a SKIP and
// exit code 4: a mesh program is a DXIL program of shader model 6.5.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "gen/mesh_vs.h"
#include "gen/mesh_ps.h"
#include "gen/mesh_ms.h"
#include "gen/mesh_ms_payload.h"
#include "gen/mesh_as.h"
#include "gen/mesh_ps_probe.h"
#include "gen/mesh_ms_probe.h"
#include "gen/mesh_ms_payload_probe.h"
#include "gen/mesh_as_probe.h"

namespace {

const char* const usage =
    "amdgpu_wddm_mesh --adapter warp|bc250|hw|index:N [--out DIR] [--deadline SECONDS] [--debug-layer]\n"
    "                 [--only ms,as] [--isolate 0|1] [--probe]\n"
    "                 | --selftest\n"
    "Subtests: mesh-ms, mesh-as. One CONFORMANCE line each, then CONFORMANCE overall.\n"
    "--isolate 1 (default): one submission and one bounded wait per pass, one PASS line each.\n"
    "--isolate 0: all passes in one submission, as before (a hang is then not attributable).\n"
    "--probe: the programs also count their work in a root UAV; one PROBE line per drawn pass.";

template <class T> class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { if (p_) p_->Release(); p_ = o.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    ~ComPtr() { if (p_) p_->Release(); }
    T* Get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T** operator&() noexcept { if (p_) { p_->Release(); p_ = nullptr; } return &p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    template <class U> HRESULT As(ComPtr<U>& other) const { return p_ ? p_->QueryInterface(IID_PPV_ARGS(&other)) : E_POINTER; }
private:
    T* p_{};
};

// ------------------------------------------------------------------------------------------------- oracle
constexpr UINT kSide = 64;
constexpr UINT kPixels = kSide * kSide;
constexpr UINT kTris = 8;
constexpr UINT kMinPerTriangle = 50;       // the geometry covers 70 pixel centres per triangle
constexpr UINT32 kPrefill = 0x5A5A5A5Au;   // no expected word and not the clear value
constexpr UINT32 kClear = 0x00000000u;

// The word the pixel program writes for triangle tri: R vertex tag, G primitive tag, B 0xA5, A 0xFF, in a
// R8G8B8A8_UNORM target read back little-endian. The tags are n/255 in the shader, which UNORM returns exactly.
UINT32 expected_word(UINT tri) noexcept {
    const UINT32 vtag = 0x10u + tri, ptag = 0x80u + (7u - tri);
    return (0xFFu << 24) | (0xA5u << 16) | (ptag << 8) | vtag;
}

// The same word with the primitive tag of the next triangle: the negative control of the primitive output linkage.
UINT32 permuted_word(UINT tri) noexcept {
    const UINT32 vtag = 0x10u + tri, ptag = 0x80u + (7u - ((tri + 1u) & 7u));
    return (0xFFu << 24) | (0xA5u << 16) | (ptag << 8) | vtag;
}

// Triangle tri's 16x16 cell of the target: cell (tri & 3, tri >> 2). corner() insets the triangle by two pixels,
// so every pixel of triangle tri lies inside that cell and no expected word can legitimately fall outside it.
bool in_cell(UINT tri, UINT px, UINT py) noexcept {
    return px / 16u == (tri & 3u) && py / 16u == (tri >> 2);
}

// What the vertex reference must look like before any comparison means anything: every word is the clear value or
// one of the eight expected words, every expected word lies in its own triangle's cell, and every triangle covers
// at least kMinPerTriangle pixels. The positional rule is what makes the comparison a statement about the
// geometry: without it a reference that drew the eight words in the wrong cells passes the oracle, and a mesh
// image wrong in the same way then compares byte-equal to it and reports PASS.
struct RefOracle {
    unsigned covered{};
    unsigned per_triangle[kTris]{};
    unsigned invalid{};
    unsigned least{};
    UINT x{}, y{};
    UINT32 word{};
    bool ok{};
};
RefOracle reference_oracle(const UINT32* image) {
    RefOracle o{};
    for (UINT i = 0; i < kPixels; ++i) {
        const UINT32 w = image[i];
        if (w == kClear) continue;
        const UINT px = i % kSide, py = i / kSide;
        bool known = false;
        for (UINT t = 0; t < kTris && !known; ++t) {
            if (w != expected_word(t)) continue;
            known = true;
            if (!in_cell(t, px, py)) {                 // the right word in the wrong cell is not coverage
                if (!o.invalid) { o.x = px; o.y = py; o.word = w; }
                ++o.invalid;
                break;
            }
            ++o.per_triangle[t];
            ++o.covered;
        }
        if (!known) {
            if (!o.invalid) { o.x = px; o.y = py; o.word = w; }
            ++o.invalid;
        }
    }
    o.least = o.per_triangle[0];
    for (UINT t = 1; t < kTris; ++t) o.least = o.per_triangle[t] < o.least ? o.per_triangle[t] : o.least;
    o.ok = !o.invalid && o.least >= kMinPerTriangle;
    return o;
}

struct Diff {
    unsigned count{}, checked{};
    UINT x{}, y{};
    UINT32 got{}, want{};
    std::string first() const {
        if (!count) return "none";
        char text[96]{};
        sprintf_s(text, "x=%u,y=%u,got=0x%08x,want=0x%08x", x, y, got, want);
        return text;
    }
};
Diff compare_words(const UINT32* got, const UINT32* want, UINT count) {
    Diff d;
    d.checked = count;
    for (UINT i = 0; i < count; ++i) {
        if (got[i] == want[i]) continue;
        if (!d.count) { d.x = i % kSide; d.y = i / kSide; d.got = got[i]; d.want = want[i]; }
        ++d.count;
    }
    return d;
}

// The reference with every primitive tag moved to the next triangle. A pass that compares equal to this instead of
// to the reference has the primitive outputs linked wrong.
std::vector<UINT32> permute_primitive_tags(const UINT32* reference) {
    std::vector<UINT32> out(reference, reference + kPixels);
    for (UINT i = 0; i < kPixels; ++i)
        for (UINT t = 0; t < kTris; ++t)
            if (out[i] == expected_word(t)) { out[i] = permuted_word(t); break; }
    return out;
}

// The command line of one run. Declared before the self-test, which checks its defaults.
struct Options {
    std::string adapter, out;
    unsigned deadline{60}, mask{3};
    bool debug{};
    bool isolate{true};   // BD-119: one submission and one bounded wait per pass
    bool probe{};         // --probe: the programs count their work in a root UAV
};

// --probe: one block of the probe buffer per pass, PROBE_WORDS words used (shaders\mesh.hlsl).
constexpr UINT kProbeWords = 6;
constexpr UINT kProbeBlock = 64;
constexpr UINT kProbeBytes = 3 * kProbeBlock;

// --only: which subtests run. The mask gates recording as well as reporting (BD-119).
bool parse_only(const char* text, unsigned& mask);

// ------------------------------------------------------------------------------------------------- self-test
bool expect(bool condition, const char* what, unsigned& failures) {
    std::printf("SELFTEST %s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
    return condition;
}

// An image that stands in for a rendered one: an 8x8 block of triangle t's word at the corner of t's 16x16 cell.
// The comparators and the oracle read words only, so a block is as good as a triangle here.
std::vector<UINT32> synthetic_image() {
    std::vector<UINT32> image(kPixels, kClear);
    for (UINT t = 0; t < kTris; ++t) {
        const UINT x0 = (t & 3u) * 16u + 2u, y0 = (t >> 2) * 16u + 2u;
        for (UINT y = y0; y < y0 + 8u; ++y)
            for (UINT x = x0; x < x0 + 8u; ++x) image[y * kSide + x] = expected_word(t);
    }
    return image;
}

// The same image with the words of two triangles exchanged, so each of the two sits in the other's cell. Every
// word is still an expected word and every triangle still has its pixel count, so only the positional rule of
// the oracle can tell this image from the ideal one.
std::vector<UINT32> swapped_cells(const UINT32* ideal, UINT a, UINT b) {
    std::vector<UINT32> out(ideal, ideal + kPixels);
    for (UINT i = 0; i < kPixels; ++i) {
        if (out[i] == expected_word(a)) out[i] = expected_word(b);
        else if (out[i] == expected_word(b)) out[i] = expected_word(a);
    }
    return out;
}

int selftest() {
    unsigned failures = 0;
    char t[256]{};
    // The words: eight distinct words, none of them the clear value or the prefill, and the tags readable back.
    bool distinct = true;
    for (UINT i = 0; i < kTris; ++i) {
        if (expected_word(i) == kClear || expected_word(i) == kPrefill) distinct = false;
        if (expected_word(i) == permuted_word(i)) distinct = false;
        for (UINT j = i + 1; j < kTris; ++j) if (expected_word(i) == expected_word(j)) distinct = false;
    }
    sprintf_s(t, "words: 8 distinct expected words, none the clear value or the prefill (first 0x%08x, last 0x%08x)",
              expected_word(0), expected_word(kTris - 1));
    expect(distinct, t, failures);

    const std::vector<UINT32> ideal = synthetic_image();
    RefOracle o = reference_oracle(ideal.data());
    sprintf_s(t, "oracle: the ideal image accepted, covered %u, least per triangle %u, invalid %u", o.covered, o.least,
              o.invalid);
    expect(o.ok && o.covered == kTris * 64u && o.least == 64u && !o.invalid, t, failures);

    std::vector<UINT32> wrong(ideal);
    wrong[7u * kSide + 13u] = 0x12345678u;
    o = reference_oracle(wrong.data());
    sprintf_s(t, "oracle: a word of no triangle rejected, %u invalid at (%u,%u) 0x%08x", o.invalid, o.x, o.y, o.word);
    expect(!o.ok && o.invalid == 1 && o.x == 13 && o.y == 7 && o.word == 0x12345678u, t, failures);

    // The positional rule: the right words in the wrong cells. A word-only oracle accepts this image, and a mesh
    // image wrong in the same way would then compare byte-equal to it.
    const std::vector<UINT32> swapped = swapped_cells(ideal.data(), 1, 2);
    o = reference_oracle(swapped.data());
    sprintf_s(t, "oracle: two cells exchanged rejected, %u invalid, first at (%u,%u) 0x%08x, covered %u", o.invalid,
              o.x, o.y, o.word, o.covered);
    expect(!o.ok && o.invalid == 2u * 64u && o.covered == (kTris - 2u) * 64u && o.x == 18 && o.y == 2 &&
               o.word == expected_word(2),
           t, failures);

    std::vector<UINT32> missing(ideal);
    for (UINT i = 0; i < kPixels; ++i) if (missing[i] == expected_word(3)) missing[i] = kClear;
    o = reference_oracle(missing.data());
    sprintf_s(t, "oracle: a missing triangle rejected, least per triangle %u, invalid %u", o.least, o.invalid);
    expect(!o.ok && !o.invalid && o.least == 0, t, failures);

    std::vector<UINT32> thin(ideal);
    unsigned removed = 0;
    for (UINT i = 0; i < kPixels && removed < 20; ++i) if (thin[i] == expected_word(5)) { thin[i] = kClear; ++removed; }
    o = reference_oracle(thin.data());
    sprintf_s(t, "oracle: a triangle below the coverage floor rejected, least %u of %u", o.least, kMinPerTriangle);
    expect(!o.ok && o.least == 44, t, failures);

    Diff d = compare_words(ideal.data(), ideal.data(), kPixels);
    expect(!d.count && d.checked == kPixels, "comparator: the image against itself, 0 of 4096 mismatches", failures);

    std::vector<UINT32> flipped(ideal);
    flipped[42u * kSide + 17u] ^= 1u;
    d = compare_words(flipped.data(), ideal.data(), kPixels);
    sprintf_s(t, "comparator: one flipped word rejected, %u mismatch at %s", d.count, d.first().c_str());
    expect(d.count == 1 && d.x == 17 && d.y == 42, t, failures);

    const std::vector<UINT32> permuted = permute_primitive_tags(ideal.data());
    d = compare_words(permuted.data(), ideal.data(), kPixels);
    sprintf_s(t, "comparator: a wrong primitive tag on every covered pixel rejected, %u mismatches, first %s", d.count,
              d.first().c_str());
    expect(d.count == kTris * 64u, t, failures);

    const std::vector<UINT32> prefilled(kPixels, kPrefill);
    d = compare_words(prefilled.data(), ideal.data(), kPixels);
    sprintf_s(t, "comparator: a pass that never ran rejected, %u of %u mismatches", d.count, d.checked);
    expect(d.count == kPixels, t, failures);

    // BD-119: --only has to gate what is recorded, so that a pass can be left out of the submission, and
    // --isolate has to parse to the per-pass form by default. The masks below are what the run loop reads.
    unsigned mask = 0;
    expect(parse_only("ms", mask) && mask == 1, "cli: --only ms selects the mesh pass alone", failures);
    expect(parse_only("as", mask) && mask == 2, "cli: --only as selects the amplification pass alone", failures);
    expect(parse_only("ms,as", mask) && mask == 3, "cli: --only ms,as selects both", failures);
    expect(!parse_only("ms,bogus", mask), "cli: --only with an unknown name rejected", failures);
    {
        const Options defaults;
        sprintf_s(t, "cli: isolate defaults to one submission per pass (isolate=%d)", defaults.isolate ? 1 : 0);
        expect(defaults.isolate, t, failures);
    }

    std::printf("SELFTEST %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------- the run
enum class Verdict { Pass, Fail, Skip, Error };
const char* verdict_text(Verdict v) {
    switch (v) {
    case Verdict::Pass: return "PASS";
    case Verdict::Fail: return "FAIL";
    case Verdict::Skip: return "SKIP";
    default: return "ERROR";
    }
}


struct Outcome {
    const char* name{};
    Verdict verdict{Verdict::Skip};
    Diff diff;
    unsigned control{};
    std::string reason;
    HRESULT hr{S_OK};
};

unsigned seconds(const char* text) {
    unsigned value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > 1000) return 0;
        value = value * 10 + static_cast<unsigned>(*p - '0');
    }
    return value;
}

bool parse_only(const char* text, unsigned& mask) {
    mask = 0;
    const std::string list = text;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const std::string word = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (word == "ms" || word == "mesh-ms") mask |= 1;
        else if (word == "as" || word == "mesh-as") mask |= 2;
        else return false;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return mask != 0;
}

// The subobject stream of a mesh pipeline. Each subobject is its type then its value, aligned to a pointer, as the
// runtime reads D3D12_PIPELINE_STATE_STREAM_DESC (d3d12.h, CD3DX12_PIPELINE_STATE_STREAM).
#pragma warning(push)
#pragma warning(disable : 4324)   // padded by the alignment specifier: that padding is what the runtime reads
template <class T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type> struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    T value{};
    Sub& operator=(const T& v) { value = v; return *this; }
};
struct MeshStream {
    Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    Sub<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    Sub<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    Sub<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
    Sub<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
    Sub<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> formats;
    Sub<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
};
// The amplification subobject goes in front of the mesh one and is left out of the mesh-only pipeline, so that the
// driver never sees an empty amplification program where none was asked for.
struct AmplifiedStream {
    Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> as;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
    Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    Sub<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    Sub<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    Sub<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
    Sub<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
    Sub<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> formats;
    Sub<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
};
#pragma warning(pop)

D3D12_RASTERIZER_DESC raster_state() {
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;          // the cells are drawn whatever the winding comes out as
    r.DepthClipEnable = TRUE;
    r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return r;
}
D3D12_BLEND_DESC blend_state() {
    D3D12_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return b;
}
D3D12_SHADER_BYTECODE bytecode(const void* bytes, size_t size) { return {bytes, size}; }

std::string adapter_text(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 d{};
    if (!adapter || FAILED(adapter->GetDesc1(&d))) return "unknown";
    char text[256]{};
    char name[128]{};
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    sprintf_s(text, "VEN_%04X DEV_%04X %s%s", d.VendorId, d.DeviceId, name,
              (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
    return text;
}

HRESULT pick_adapter(IDXGIFactory4* factory, const std::string& which, decltype(&D3D12CreateDevice) create,
                     ComPtr<IDXGIAdapter1>& chosen) {
    if (which == "warp") {
        ComPtr<IDXGIAdapter> warp;
        const HRESULT hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        return FAILED(hr) ? hr : warp.As(chosen);
    }
    unsigned wanted = ~0u;
    if (which.rfind("index:", 0) == 0) {
        const unsigned n = seconds(which.c_str() + 6);
        if (!n && which != "index:0") return E_INVALIDARG;
        wanted = n;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return hr;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(candidate->GetDesc1(&d))) continue;
        bool take = false;
        if (wanted != ~0u) take = index == wanted;
        else if (which == "bc250") take = d.VendorId == 0x1002 && d.DeviceId == 0x13fe;
        else if (which == "hw") take = !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414 &&
                                       SUCCEEDED(create(candidate.Get(), D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr));
        if (take) { chosen = candidate; return S_OK; }
    }
    return DXGI_ERROR_NOT_FOUND;
}

const char* model_text(D3D_SHADER_MODEL model) {
    switch (model) {
    case D3D_SHADER_MODEL_6_0: return "6_0";
    case D3D_SHADER_MODEL_6_1: return "6_1";
    case D3D_SHADER_MODEL_6_2: return "6_2";
    case D3D_SHADER_MODEL_6_3: return "6_3";
    case D3D_SHADER_MODEL_6_4: return "6_4";
    case D3D_SHADER_MODEL_6_5: return "6_5";
    case D3D_SHADER_MODEL_6_6: return "6_6";
    case D3D_SHADER_MODEL_6_7: return "6_7";
    case D3D_SHADER_MODEL_6_8: return "6_8";
    default: return "below 6_0";
    }
}
const char* level_text(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    default: return "below 11_0";
    }
}

std::string json_text(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

bool write_file(const std::wstring& path, const void* bytes, size_t size) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

// Everything one pass needs: its target, its read-back buffer and its pipeline.
struct Pass {
    const char* name{};
    ComPtr<ID3D12Resource> target, readback;
    ComPtr<ID3D12PipelineState> pipeline;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    std::vector<UINT32> image;
};

int run(const Options& o) {
    // The runtime from System32 and nowhere else.
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\d3d12.dll")) return 3;
    const HMODULE runtime = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) return 3;
    auto proc = GetProcAddress(runtime, "D3D12CreateDevice");
    decltype(&D3D12CreateDevice) create = nullptr;
    static_assert(sizeof(create) == sizeof(proc), "function pointer size");
    std::memcpy(&create, &proc, sizeof(create));
    proc = GetProcAddress(runtime, "D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize = nullptr;
    std::memcpy(&serialize, &proc, sizeof(serialize));
    if (!create || !serialize) return 3;
    if (o.debug) {
        proc = GetProcAddress(runtime, "D3D12GetDebugInterface");
        decltype(&D3D12GetDebugInterface) get_debug = nullptr;
        std::memcpy(&get_debug, &proc, sizeof(get_debug));
        ComPtr<ID3D12Debug> debug;
        if (!get_debug || FAILED(get_debug(IID_PPV_ARGS(&debug)))) {
            std::fprintf(stderr, "debug layer unavailable\n");
            return 3;
        }
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = pick_adapter(factory.Get(), o.adapter, create, adapter);
    if (FAILED(hr)) {
        std::fprintf(stderr, "adapter %s not found hr=%08lx\n", o.adapter.c_str(), static_cast<unsigned long>(hr));
        return 3;
    }
    const std::string adapter_name = adapter_text(adapter.Get());
    std::printf("runtime=system32/d3d12.dll adapter=%s\n", adapter_name.c_str());
    std::fflush(stdout);
    ComPtr<ID3D12Device> device;
    hr = create(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::printf("CONFORMANCE overall ERROR reason=D3D12CreateDevice-FL12_1 hr=%08lx\n",
                    static_cast<unsigned long>(hr));
        return 3;
    }
    ComPtr<ID3D12Device2> device2;
    const HRESULT hr_device2 = device.As(device2);

    // The features this witness turns on: the mesh shader tier and the highest shader model.
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    const HRESULT hr7 = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9));
#ifdef MESH_TEST_FORCE_TIER_0
    options7.MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;   // host arm only: the skip path on any adapter
#endif
    D3D_SHADER_MODEL model = static_cast<D3D_SHADER_MODEL>(0);
    for (const D3D_SHADER_MODEL m : {D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                                     D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_0}) {
        D3D12_FEATURE_DATA_SHADER_MODEL ask{m};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &ask, sizeof(ask)))) {
            model = ask.HighestShaderModel;
            break;
        }
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                  D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels{static_cast<UINT>(std::size(levels)), levels,
                                                     D3D_FEATURE_LEVEL_11_0};
    device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &feature_levels, sizeof(feature_levels));
    std::printf("FEATURES MeshShaderTier=%d MeshShaderSupportsFullRangeRenderTargetArrayIndex=%d "
                "DerivativesInMeshAndAmplificationShaders=%d HighestShaderModel=%s MaxSupportedFeatureLevel=%s "
                "device=12_1 options7_hr=%08lx\n",
                static_cast<int>(options7.MeshShaderTier), options9.MeshShaderSupportsFullRangeRenderTargetArrayIndex ? 1 : 0,
                options9.DerivativesInMeshAndAmplificationShadersSupported ? 1 : 0, model_text(model),
                level_text(feature_levels.MaxSupportedFeatureLevel), static_cast<unsigned long>(hr7));
    std::fflush(stdout);

    Outcome ms{"mesh-ms"}, as{"mesh-as"};
    const auto publish = [&](const std::string& reason) {
        std::string json;
        const auto line = [&](const Outcome& out) {
            if (!(o.mask & (out.name[5] == 'm' ? 1u : 2u))) return;
            std::printf("CONFORMANCE %s %s mismatches=%u checked=%u first=%s control_permuted_ptag=%u%s%s hr=%08lx\n",
                        out.name, verdict_text(out.verdict), out.diff.count, out.diff.checked, out.diff.first().c_str(),
                        out.control, out.reason.empty() ? "" : " reason=", out.reason.c_str(),
                        static_cast<unsigned long>(out.hr));
            json += std::string(json.empty() ? "" : ",") + "\"" + out.name + "\":{\"verdict\":\"" +
                    verdict_text(out.verdict) + "\",\"mismatches\":" + std::to_string(out.diff.count) +
                    ",\"checked\":" + std::to_string(out.diff.checked) + ",\"first\":" + json_text(out.diff.first()) +
                    ",\"control_permuted_ptag\":" + std::to_string(out.control) + ",\"reason\":" +
                    json_text(out.reason) + ",\"hr\":" + std::to_string(static_cast<long>(out.hr)) + "}";
        };
        line(ms);
        line(as);
        Verdict overall = Verdict::Pass;
        unsigned counted = 0;
        for (const Outcome* out : {&ms, &as}) {
            if (!(o.mask & (out->name[5] == 'm' ? 1u : 2u))) continue;
            ++counted;
            if (out->verdict == Verdict::Error) overall = Verdict::Error;
            else if (out->verdict == Verdict::Fail && overall != Verdict::Error) overall = Verdict::Fail;
            else if (out->verdict == Verdict::Skip && overall == Verdict::Pass) overall = Verdict::Skip;
        }
        if (!counted) overall = Verdict::Skip;
        std::printf("CONFORMANCE overall %s%s%s\n", verdict_text(overall), reason.empty() ? "" : " reason=",
                    reason.c_str());
        std::fflush(stdout);
        if (!o.out.empty()) {
            const std::wstring dir(o.out.begin(), o.out.end());
            CreateDirectoryW(dir.c_str(), nullptr);
            const std::string body = "{\"adapter\":" + json_text(adapter_name) + ",\"mesh_shader_tier\":" +
                                     std::to_string(static_cast<int>(options7.MeshShaderTier)) +
                                     ",\"highest_shader_model\":" + json_text(model_text(model)) +
                                     ",\"overall\":\"" + verdict_text(overall) + "\",\"subtests\":{" + json + "}}\n";
            if (!write_file(dir + L"\\mesh.json", body.data(), body.size()))
                std::fprintf(stderr, "mesh.json not written\n");
        }
        if (overall == Verdict::Pass) return 0;
        if (overall == Verdict::Skip) return 4;
        return 1;
    };

    if (FAILED(hr_device2)) {
        ms.verdict = as.verdict = Verdict::Error;
        ms.reason = as.reason = "no-ID3D12Device2";
        ms.hr = as.hr = hr_device2;
        return publish("no-ID3D12Device2");
    }
    if (options7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1 || model < D3D_SHADER_MODEL_6_5) {
        ms.verdict = as.verdict = Verdict::Skip;
        char reason[64]{};
        sprintf_s(reason, "MeshShaderTier=%d,HighestShaderModel=%s", static_cast<int>(options7.MeshShaderTier),
                  model_text(model));
        ms.reason = as.reason = reason;
        return publish("");
    }

    // Three passes, one submission. The reference is always drawn: it is what the other two are compared with.
    Pass pass[3];
    pass[0].name = "reference";
    pass[1].name = "mesh-ms";
    pass[2].name = "mesh-as";
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return 3;
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return 3;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))))
        return 3;
    ComPtr<ID3D12GraphicsCommandList6> list6;
    hr = list.As(list6);
    if (FAILED(hr)) {
        ms.verdict = as.verdict = Verdict::Error;
        ms.reason = as.reason = "no-ID3D12GraphicsCommandList6";
        ms.hr = as.hr = hr;
        return publish("no-ID3D12GraphicsCommandList6");
    }
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap)))) return 3;
    const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                       D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                        D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = kSide;
    target_desc.Height = kSide;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc = {1, 0};
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = kPixels * sizeof(UINT32);
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc = {1, 0};
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT p = 0; p < 3; ++p) {
        if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                   IID_PPV_ARGS(&pass[p].target))))
            return 3;
        if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&pass[p].readback))))
            return 3;
        pass[p].rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        pass[p].rtv.ptr += static_cast<SIZE_T>(p) * rtv_size;
        device->CreateRenderTargetView(pass[p].target.Get(), nullptr, pass[p].rtv);
        // The read-back region starts at a word that no pass can write, so a pass that never ran cannot compare
        // equal to anything. The debug layer reports this CPU write to READBACK memory as warning 929.
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(pass[p].readback->Map(0, &none, &mapped)) || !mapped) return 3;
        for (UINT i = 0; i < kPixels; ++i) static_cast<UINT32*>(mapped)[i] = kPrefill;
        D3D12_RANGE whole{0, kPixels * sizeof(UINT32)};
        pass[p].readback->Unmap(0, &whole);
    }

    ComPtr<ID3D12RootSignature> root;
    {
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;   // no resources, and no input assembler in either pipeline
        // --probe: one root UAV, u0, that every stage can write.
        D3D12_ROOT_PARAMETER probe_parameter{};
        probe_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        probe_parameter.Descriptor = {0, 0};
        probe_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        if (o.probe) {
            desc.NumParameters = 1;
            desc.pParameters = &probe_parameter;
        }
        ComPtr<ID3DBlob> blob, error;
        hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        if (FAILED(hr)) {
            ms.verdict = as.verdict = Verdict::Error;
            ms.reason = as.reason = "root-signature";
            ms.hr = as.hr = hr;
            return publish("root-signature");
        }
    }

    // --probe: the counters, zero at creation (a committed resource), and their read-back buffer.
    ComPtr<ID3D12Resource> probe, probe_readback;
    if (o.probe) {
        D3D12_RESOURCE_DESC probe_desc = buffer_desc;
        probe_desc.Width = kProbeBytes;
        probe_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &probe_desc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&probe))))
            return 3;
        probe_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &probe_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&probe_readback))))
            return 3;
    }
    const D3D12_SHADER_BYTECODE ps_code = o.probe ? bytecode(g_mesh_ps_probe, sizeof(g_mesh_ps_probe))
                                                  : bytecode(g_mesh_ps, sizeof(g_mesh_ps));

    // The reference pipeline: the vertex program reads SV_VertexID, so there is no input layout and no vertex
    // buffer, which is also what a mesh pipeline must not have.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.VS = bytecode(g_mesh_vs, sizeof(g_mesh_vs));
        desc.PS = ps_code;
        desc.BlendState = blend_state();
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = raster_state();
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc = {1, 0};
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pass[0].pipeline));
        if (FAILED(hr)) {
            ms.verdict = as.verdict = Verdict::Error;
            ms.reason = as.reason = "reference-pipeline";
            ms.hr = as.hr = hr;
            return publish("reference-pipeline");
        }
    }
    // The two mesh pipelines, through the subobject stream of ID3D12Device2::CreatePipelineState.
    {
        D3D12_RT_FORMAT_ARRAY formats{};
        formats.NumRenderTargets = 1;
        formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        MeshStream mesh{};
        mesh.root = root.Get();
        mesh.ms = o.probe ? bytecode(g_mesh_ms_probe, sizeof(g_mesh_ms_probe)) : bytecode(g_mesh_ms, sizeof(g_mesh_ms));
        mesh.ps = ps_code;
        mesh.blend = blend_state();
        mesh.sample_mask = UINT_MAX;
        mesh.raster = raster_state();
        mesh.formats = formats;
        mesh.sample = DXGI_SAMPLE_DESC{1, 0};
        D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(mesh), &mesh};
        const HRESULT hr_ms = device2->CreatePipelineState(&stream, IID_PPV_ARGS(&pass[1].pipeline));
        AmplifiedStream amplified{};
        amplified.root = root.Get();
        amplified.as = o.probe ? bytecode(g_mesh_as_probe, sizeof(g_mesh_as_probe)) : bytecode(g_mesh_as, sizeof(g_mesh_as));
        amplified.ms = o.probe ? bytecode(g_mesh_ms_payload_probe, sizeof(g_mesh_ms_payload_probe))
                               : bytecode(g_mesh_ms_payload, sizeof(g_mesh_ms_payload));
        amplified.ps = ps_code;
        amplified.blend = blend_state();
        amplified.sample_mask = UINT_MAX;
        amplified.raster = raster_state();
        amplified.formats = formats;
        amplified.sample = DXGI_SAMPLE_DESC{1, 0};
        D3D12_PIPELINE_STATE_STREAM_DESC amplified_stream{sizeof(amplified), &amplified};
        const HRESULT hr_as = device2->CreatePipelineState(&amplified_stream, IID_PPV_ARGS(&pass[2].pipeline));
        if (FAILED(hr_ms)) { ms.verdict = Verdict::Fail; ms.reason = "CreatePipelineState-MS"; ms.hr = hr_ms; }
        if (FAILED(hr_as)) { as.verdict = Verdict::Fail; as.reason = "CreatePipelineState-AS-MS"; as.hr = hr_as; }
        if (FAILED(hr_ms) && FAILED(hr_as)) return publish("");
    }

    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kSide), static_cast<float>(kSide), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kSide), static_cast<LONG>(kSide)};
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 3;
    const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) return 3;
    const DWORD bound = o.deadline < 10 ? o.deadline * 1000 : 10000;
    UINT64 fence_value = 0;

    // BD-119: which passes are recorded at all. The vertex reference always is; a mesh pass only when --only
    // selects it, so that a hang of one pass can be excluded by leaving it out.
    bool record[3] = {true, (o.mask & 1u) != 0, (o.mask & 2u) != 0};
    for (UINT p = 1; p < 3; ++p) {
        if (record[p]) continue;
        Outcome& out = p == 1 ? ms : as;
        out.verdict = Verdict::Skip;
        out.reason = "not-selected";
    }

    // One pass's commands into the open list: the whole render and its read-back copy.
    const auto record_pass = [&](UINT p) {
        list->SetGraphicsRootSignature(root.Get());
        if (o.probe) list->SetGraphicsRootUnorderedAccessView(0, probe->GetGPUVirtualAddress() + p * kProbeBlock);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &pass[p].rtv, FALSE, nullptr);
        list->ClearRenderTargetView(pass[p].rtv, clear, 0, nullptr);
        list->SetPipelineState(pass[p].pipeline.Get());
        if (p == 0) {
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(kTris * 3, 1, 0, 0);
        } else {
            list6->DispatchMesh(p == 1 ? kTris : 1, 1, 1);   // the amplification program dispatches the eight groups
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = pass[p].target.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = pass[p].readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSide, kSide, 1, kSide * 4};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = pass[p].target.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };

    // Close the open list, submit it and wait the bound for its own fence value. names is what the PASS and the
    // failure line call this submission. False: the work did not retire, and the caller must not touch the GPU.
    const auto submit_and_wait = [&](const char* names) -> bool {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.Get()};
        const ULONGLONG start = GetTickCount64();
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fence_value;
        if (FAILED(queue->Signal(fence.Get(), value)) || FAILED(fence->SetEventOnCompletion(value, done)))
            return false;
        const bool retired = WaitForSingleObject(done, bound) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value;
        std::printf("PASS %s submission=%llu retired=%d ms=%llu\n", names, static_cast<unsigned long long>(value),
                    retired ? 1 : 0, static_cast<unsigned long long>(GetTickCount64() - start));
        std::fflush(stdout);
        return retired;
    };

    if (o.isolate) {
        // One submission per pass. The failing pass is the one whose PASS line says retired=0.
        for (UINT p = 0; p < 3; ++p) {
            if (!record[p] || !pass[p].pipeline) continue;
            if (p && FAILED(allocator->Reset())) return 3;            // the previous pass retired, so this is safe
            if (p && FAILED(list->Reset(allocator.Get(), nullptr))) return 3;
            record_pass(p);
            if (!submit_and_wait(pass[p].name)) {
                std::printf("CONFORMANCE overall ERROR reason=submission-not-retired pass=%s\n", pass[p].name);
                std::fflush(stdout);
                ExitProcess(3);   // GPU work that did not retire: nothing is released
            }
        }
    } else {
        // The control: every pass in one submission, as the client did before BD-119. A hang here names no pass.
        for (UINT p = 0; p < 3; ++p)
            if (record[p] && pass[p].pipeline) record_pass(p);
        if (!submit_and_wait("reference+mesh-ms+mesh-as")) {
            std::printf("CONFORMANCE overall ERROR reason=submission-not-retired\n");
            std::fflush(stdout);
            ExitProcess(3);
        }
    }

    // --probe: the counters of all passes in one more submission, after every pass retired.
    UINT32 counts[kProbeBytes / 4]{};
    if (o.probe) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return 3;
        list->CopyBufferRegion(probe_readback.Get(), 0, probe.Get(), 0, kProbeBytes);
        if (!submit_and_wait("probe-readback")) {
            std::printf("CONFORMANCE overall ERROR reason=submission-not-retired pass=probe-readback\n");
            std::fflush(stdout);
            ExitProcess(3);
        }
        void* mapped = nullptr;
        D3D12_RANGE whole{0, kProbeBytes};
        if (SUCCEEDED(probe_readback->Map(0, &whole, &mapped)) && mapped) {
            std::memcpy(counts, mapped, kProbeBytes);
            D3D12_RANGE none{0, 0};
            probe_readback->Unmap(0, &none);
        }
    }

    for (UINT p = 0; p < 3; ++p) {
        pass[p].image.assign(kPixels, kPrefill);
        if (!pass[p].pipeline || !record[p]) continue;
        void* mapped = nullptr;
        D3D12_RANGE whole{0, kPixels * sizeof(UINT32)};
        if (SUCCEEDED(pass[p].readback->Map(0, &whole, &mapped)) && mapped) {
            std::memcpy(pass[p].image.data(), mapped, kPixels * sizeof(UINT32));
            D3D12_RANGE none{0, 0};
            pass[p].readback->Unmap(0, &none);
        }
    }
    if (!o.out.empty()) {
        const std::wstring dir(o.out.begin(), o.out.end());
        CreateDirectoryW(dir.c_str(), nullptr);
        std::vector<UINT32> dump;
        for (UINT p = 0; p < 3; ++p) dump.insert(dump.end(), pass[p].image.begin(), pass[p].image.end());
        if (!write_file(dir + L"\\mesh.bin", dump.data(), dump.size() * sizeof(UINT32)))
            std::fprintf(stderr, "mesh.bin not written\n");
    }

    // The reference first: a comparison with a reference that drew nothing, or drew something else, proves nothing.
    const RefOracle oracle = reference_oracle(pass[0].image.data());
    std::printf("REFERENCE covered=%u least_per_triangle=%u invalid=%u first_invalid=%s\n", oracle.covered,
                oracle.least, oracle.invalid,
                oracle.invalid ? Diff{1, kPixels, oracle.x, oracle.y, oracle.word, 0}.first().c_str() : "none");
    if (!oracle.ok) {
        for (Outcome* out : {&ms, &as})
            if (out->verdict != Verdict::Fail) {
                out->verdict = Verdict::Error;
                out->reason = "reference-oracle";
            }
        return publish("reference-oracle");
    }
    const std::vector<UINT32> permuted = permute_primitive_tags(pass[0].image.data());
    for (UINT p = 1; p < 3; ++p) {
        Outcome& out = p == 1 ? ms : as;
        if (out.verdict == Verdict::Fail || !record[p]) continue;   // refused above, or --only left it out
        out.diff = compare_words(pass[p].image.data(), pass[0].image.data(), kPixels);
        out.control = compare_words(pass[p].image.data(), permuted.data(), kPixels).count;
        out.verdict = out.diff.count || out.control != oracle.covered ? Verdict::Fail : Verdict::Pass;
        if (!out.diff.count && out.control != oracle.covered) out.reason = "control-not-discriminating";
    }
    if (o.probe) {
        std::string json;
        for (UINT p = 0; p < 3; ++p) {
            if (!record[p] || !pass[p].pipeline) continue;
            const UINT32* c = counts + p * (kProbeBlock / 4);
            std::printf("PROBE %s as_threads=%u ms_groups=%u ms_gid_mask=0x%08x ms_tri_mask=0x%08x ms_prims=%u "
                        "ps_invocations=%u\n", pass[p].name, c[0], c[1], c[2], c[3], c[4], c[5]);
            json += std::string(json.empty() ? "" : ",") + "\"" + pass[p].name + "\":[";
            for (UINT i = 0; i < kProbeWords; ++i) json += (i ? "," : "") + std::to_string(c[i]);
            json += "]";
        }
        std::fflush(stdout);
        if (!o.out.empty()) {
            const std::wstring dir(o.out.begin(), o.out.end());
            CreateDirectoryW(dir.c_str(), nullptr);
            const std::string body = "{\"words\":[\"as_threads\",\"ms_groups\",\"ms_gid_mask\",\"ms_tri_mask\","
                                     "\"ms_prims\",\"ps_invocations\"],\"passes\":{" + json + "}}\n";
            if (!write_file(dir + L"\\probe.json", body.data(), body.size()))
                std::fprintf(stderr, "probe.json not written\n");
        }
    }
    const int code = publish("");
    // The debug layer is for the host: every stored message of severity corruption or error fails the run.
    if (o.debug) {
        ComPtr<ID3D12InfoQueue> info;
        unsigned errors = 0, warnings = 0;
        if (SUCCEEDED(device.As(info))) {
            const UINT64 stored = info->GetNumStoredMessages();
            unsigned listed = 0;
            for (UINT64 i = 0; i < stored; ++i) {
                SIZE_T bytes = 0;
                if (FAILED(info->GetMessage(i, nullptr, &bytes)) || !bytes) continue;
                std::vector<unsigned char> buffer(bytes);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(info->GetMessage(i, m, &bytes))) continue;
                const bool error = m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                                   m->Severity == D3D12_MESSAGE_SEVERITY_ERROR;
                errors += error ? 1u : 0u;
                warnings += m->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? 1u : 0u;
                if ((error || m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) && listed < 12) {
                    std::printf("DEBUGLAYER %s id=%d %s\n", error ? "error" : "warning", static_cast<int>(m->ID),
                                m->pDescription);
                    ++listed;
                }
            }
        }
        std::printf("DEBUGLAYER errors=%u warnings=%u\n", errors, warnings);
        std::fflush(stdout);
        if (errors) return 1;
    }
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--help")) { std::puts(usage); return 0; }
    if (argc == 2 && !std::strcmp(argv[1], "--selftest")) return selftest();
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool value = i + 1 < argc;
        if (!std::strcmp(a, "--adapter") && value) o.adapter = argv[++i];
        else if (!std::strcmp(a, "--out") && value) o.out = argv[++i];
        else if (!std::strcmp(a, "--deadline") && value) { o.deadline = seconds(argv[++i]); if (!o.deadline) return 2; }
        else if (!std::strcmp(a, "--only") && value) { if (!parse_only(argv[++i], o.mask)) return 2; }
        else if (!std::strcmp(a, "--isolate") && value) {
            const char* v = argv[++i];
            if (!std::strcmp(v, "0")) o.isolate = false;
            else if (!std::strcmp(v, "1")) o.isolate = true;
            else return 2;
        }
        else if (!std::strcmp(a, "--debug-layer")) o.debug = true;
        else if (!std::strcmp(a, "--probe")) o.probe = true;
        else return 2;
    }
    if (!(o.adapter == "warp" || o.adapter == "bc250" || o.adapter == "hw" || o.adapter.rfind("index:", 0) == 0))
        return 2;
    return run(o);
}
