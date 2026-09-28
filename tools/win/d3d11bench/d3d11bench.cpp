// d3d11bench - the workload behind the M14 5 % bound (D004). The same executable runs through the Microsoft
// runtime and the system UMD, or through application-local DXVK DLLs placed next to it; compare.py holds the
// results of the two paths against the bound. README.md has the scenes, the output and the protocol.

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT kThrottle = 3;       // frames in flight without a swap chain: DXGI's default frame latency
constexpr UINT kTimingSlots = 16;
constexpr UINT kShaderTarget = 256;
constexpr double kMaxDeadlineSeconds = 170.0;   // lab trials stay under three minutes
// The scenes' shaders and constants. 2: images exact (draws) or contracting (fill, shaders) across
// implementations; results of revision 1 carry no scene_revision.
constexpr int kSceneRevision = 2;

enum Exit : int { kPass = 0, kApiFailure = 1, kBadArguments = 2, kDeadline = 3, kDeviceRemoved = 4 };

struct Options {
  bool help = false;
  bool window = false;
  bool modeGiven = false;
  UINT width = 1280, height = 720;
  UINT frames = 300, warmup = 30;
  UINT draws = 2000, layers = 8, shaders = 64;
  UINT vendor = 0, device = 0;
  bool warp = false;
  double deadlineSeconds = 120.0;
  std::vector<std::string> scenes;
  std::wstring out;
  std::wstring dump;
};

double g_deadline = 0.0;
bool g_expired = false;

double NowMs() {
  static const double scale = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 / double(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) * scale;
}

bool Expired() {
  if (!g_expired && NowMs() > g_deadline) {
    g_expired = true;
    puts("FAIL deadline reached");
  }
  return g_expired;
}

bool Ok(HRESULT hr, const char *what) {
  if (SUCCEEDED(hr))
    return true;
  printf("FAIL %s hr=0x%08lx\n", what, static_cast<unsigned long>(hr));
  char diagnostic[256];
  snprintf(diagnostic, sizeof(diagnostic), "d3d11bench FAIL %.180s hr=0x%08lx\n", what, static_cast<unsigned long>(hr));
  OutputDebugStringA(diagnostic);
  return false;
}

uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// ---------------------------------------------------------------------------------------------------------
// Numbers and JSON

struct Stats {
  size_t count = 0;
  double median = 0, p5 = 0, p95 = 0, mean = 0, min = 0, max = 0;
};

double NearestRank(const std::vector<double> &sorted, double p) {
  size_t i = size_t(std::ceil(p * double(sorted.size())));
  i = std::clamp<size_t>(i, 1, sorted.size());
  return sorted[i - 1];
}

Stats Summarize(std::vector<double> v) {
  Stats s;
  if (v.empty())
    return s;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  s.count = n;
  s.median = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
  s.p5 = NearestRank(v, 0.05);
  s.p95 = NearestRank(v, 0.95);
  double sum = 0;
  for (double x : v)
    sum += x;
  s.mean = sum / double(n);
  s.min = v.front();
  s.max = v.back();
  return s;
}

std::string Utf8(const std::wstring &w) {
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(n, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string Quote(const std::string &s) {
  std::string r = "\"";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      r += '\\';
      r += c;
    } else if (u < 0x20) {
      char b[8];
      sprintf_s(b, "\\u%04x", u);
      r += b;
    } else {
      r += c;
    }
  }
  return r + "\"";
}

std::string Number(double v) {
  char b[32];
  sprintf_s(b, "%.4f", v);
  return b;
}

std::string Hex(uint64_t v, int digits) {
  char b[24];
  sprintf_s(b, "%0*llx", digits, static_cast<unsigned long long>(v));
  return b;
}

std::string StatsJson(const Stats &s) {
  return "{\"count\":" + std::to_string(s.count) + ",\"median\":" + Number(s.median) + ",\"p5\":" + Number(s.p5) +
         ",\"p95\":" + Number(s.p95) + ",\"mean\":" + Number(s.mean) + ",\"min\":" + Number(s.min) +
         ",\"max\":" + Number(s.max) + "}";
}

bool WriteAtomically(const std::wstring &path, const std::string &text);

// FNV-1a over the visible bytes of each row, so that row padding never enters the checksum. With --dump the
// same bytes also go to <dir>\<scene>.pam (PAM, RGB_ALPHA, top row first) for imgdiff.py: a checksum only says
// that two images differ, the image says by how much.
bool Checksum(ID3D11DeviceContext *ctx, ID3D11Texture2D *staging, UINT width, UINT height, const Options &o,
              const char *scene, std::string &out) {
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (!Ok(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m), "map staging"))
    return false;
  const bool dump = !o.dump.empty();
  std::string image;
  if (dump)
    image = "P7\nWIDTH " + std::to_string(width) + "\nHEIGHT " + std::to_string(height) +
            "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
  uint64_t h = 14695981039346656037ull;
  for (UINT y = 0; y < height; y++) {
    const uint8_t *row = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
    for (size_t x = 0; x < size_t(width) * 4; x++) {
      h ^= row[x];
      h *= 1099511628211ull;
    }
    if (dump) {
      static_assert(kFormat == DXGI_FORMAT_B8G8R8A8_UNORM, "the dump swaps B and R");
      for (size_t x = 0; x < size_t(width) * 4; x += 4) {
        const char rgba[4] = { char(row[x + 2]), char(row[x + 1]), char(row[x]), char(row[x + 3]) };
        image.append(rgba, 4);
      }
    }
  }
  ctx->Unmap(staging, 0);
  out = Hex(h, 16);
  if (dump && !WriteAtomically(o.dump + L"\\" + std::wstring(scene, scene + strlen(scene)) + L".pam", image)) {
    printf("FAIL writing the %s image\n", scene);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// GPU work helpers

bool WaitEvent(ID3D11DeviceContext *ctx, ID3D11Query *query) {
  for (;;) {
    const HRESULT hr = ctx->GetData(query, nullptr, 0, 0);
    if (hr == S_OK)
      return true;
    if (hr != S_FALSE)
      return Ok(hr, "event query");
    if (Expired())
      return false;
    SwitchToThread();
  }
}

// Timestamps of every frame, collected without flushing while the frame loop runs. Warm-up frames are
// measured too but not recorded.
class GpuTimer {
public:
  bool Init(ID3D11Device *dev) {
    for (Slot &s : m_slots) {
      D3D11_QUERY_DESC d = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
      if (!Ok(dev->CreateQuery(&d, &s.disjoint), "disjoint query"))
        return false;
      d.Query = D3D11_QUERY_TIMESTAMP;
      if (!Ok(dev->CreateQuery(&d, &s.begin), "timestamp query") ||
          !Ok(dev->CreateQuery(&d, &s.end), "timestamp query"))
        return false;
    }
    return true;
  }

  void Begin(ID3D11DeviceContext *ctx, bool record) {
    Slot &s = m_slots[m_next];
    if (s.pending)
      Collect(ctx, s, true);
    s.pending = true;
    s.record = record;
    ctx->Begin(s.disjoint.Get());
    ctx->End(s.begin.Get());
  }

  void End(ID3D11DeviceContext *ctx) {
    Slot &s = m_slots[m_next];
    ctx->End(s.end.Get());
    ctx->End(s.disjoint.Get());
    m_next = (m_next + 1) % kTimingSlots;
  }

  void Poll(ID3D11DeviceContext *ctx) {
    for (Slot &s : m_slots)
      if (s.pending)
        Collect(ctx, s, false);
  }

  void Drain(ID3D11DeviceContext *ctx) {
    for (Slot &s : m_slots)
      if (s.pending)
        Collect(ctx, s, true);
  }

  std::vector<double> samples;
  UINT disjoint = 0, failures = 0;

private:
  struct Slot {
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false, record = false;
  };

  template <typename T> bool Get(ID3D11DeviceContext *ctx, Slot &s, ID3D11Query *q, T &data, bool wait) {
    for (;;) {
      const HRESULT hr = ctx->GetData(q, &data, sizeof data, wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (hr == S_OK)
        return true;
      if (hr != S_FALSE || (wait && Expired())) {
        s.pending = false;
        failures++;
        return false;
      }
      if (!wait)
        return false;
      SwitchToThread();
    }
  }

  void Collect(ID3D11DeviceContext *ctx, Slot &s, bool wait) {
    // The disjoint query ends last, so once it is available both timestamps are.
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    UINT64 t0 = 0, t1 = 0;
    if (!Get(ctx, s, s.disjoint.Get(), dj, wait) || !Get(ctx, s, s.begin.Get(), t0, true) ||
        !Get(ctx, s, s.end.Get(), t1, true))
      return;
    s.pending = false;
    if (!s.record)
      return;
    if (dj.Disjoint || !dj.Frequency || t1 < t0)
      disjoint++;
    else
      samples.push_back(double(t1 - t0) * 1000.0 / double(dj.Frequency));
  }

  Slot m_slots[kTimingSlots];
  UINT m_next = 0;
};

bool Compile(const char *source, const char *target, const D3D_SHADER_MACRO *macros, ComPtr<ID3DBlob> &blob) {
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(source, strlen(source), nullptr, macros, nullptr, "main", target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
  if (SUCCEEDED(hr))
    return true;
  printf("FAIL compile %s hr=0x%08lx %s\n", target, static_cast<unsigned long>(hr),
         errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
  return false;
}

bool PatternTexture(ID3D11Device *dev, UINT size, UINT levels, UINT seed, ComPtr<ID3D11ShaderResourceView> &srv) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = d.Height = size;
  d.MipLevels = levels;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  std::vector<std::vector<uint32_t>> texels(levels);
  std::vector<D3D11_SUBRESOURCE_DATA> init(levels);
  for (UINT l = 0; l < levels; l++) {
    const UINT s = std::max(size >> l, 1u);
    texels[l].resize(size_t(s) * s);
    for (UINT y = 0; y < s; y++)
      for (UINT x = 0; x < s; x++)
        texels[l][size_t(y) * s + x] = Hash(seed * 7919u + l * 104729u + (x << 16 | y)) | 0xff000000u;
    init[l] = { texels[l].data(), s * 4, 0 };
  }
  ComPtr<ID3D11Texture2D> texture;
  return Ok(dev->CreateTexture2D(&d, init.data(), &texture), "pattern texture") &&
         Ok(dev->CreateShaderResourceView(texture.Get(), nullptr, &srv), "pattern view");
}

bool Sampler(ID3D11Device *dev, D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address,
             ComPtr<ID3D11SamplerState> &sampler) {
  D3D11_SAMPLER_DESC d = {};
  d.Filter = filter;
  d.AddressU = d.AddressV = d.AddressW = address;
  d.MaxAnisotropy = 1;
  d.ComparisonFunc = D3D11_COMPARISON_NEVER;
  d.MaxLOD = D3D11_FLOAT32_MAX;
  return Ok(dev->CreateSamplerState(&d, &sampler), "sampler");
}

bool Rasterizer(ID3D11Device *dev, ComPtr<ID3D11RasterizerState> &raster) {
  D3D11_RASTERIZER_DESC d = {};
  d.FillMode = D3D11_FILL_SOLID;
  d.CullMode = D3D11_CULL_NONE;
  d.DepthClipEnable = TRUE;
  return Ok(dev->CreateRasterizerState(&d, &raster), "rasterizer");
}

void Viewport(ID3D11DeviceContext *ctx, UINT width, UINT height) {
  const D3D11_VIEWPORT vp = { 0, 0, float(width), float(height), 0, 1 };
  ctx->RSSetViewports(1, &vp);
}

const char kFullScreenVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";

bool FullScreenVs(ID3D11Device *dev, ComPtr<ID3D11VertexShader> &vs) {
  ComPtr<ID3DBlob> blob;
  return Compile(kFullScreenVs, "vs_4_0", nullptr, blob) &&
         Ok(dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs), "vertex shader");
}

// ---------------------------------------------------------------------------------------------------------
// Scenes

class FrameScene {
public:
  virtual ~FrameScene() = default;
  virtual const char *Name() const = 0;
  virtual bool Init(ID3D11Device *dev, const Options &o) = 0;
  virtual void Record(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, UINT width, UINT height) = 0;
  virtual std::string Parameters() const = 0;
  virtual UINT Failures() const { return 0; }
};

// CPU-bound: many small draws, each with a constant buffer DISCARD and a texture switch, the pattern that
// makes the application thread's cost per draw visible.
//
// The image is exact on every conforming implementation, so that a checksum compares a CPU rasterizer with a
// GPU and not only two paths on one GPU: vertices snap to chosen positions and no edge touches a pixel centre
// (see Init), the texel comes from Load at the pixel's integer position rather than from an interpolated
// coordinate, and the tint is a 0/1 channel mask, so no product needs rounding and 8-bit texels survive a 16-bit
// colour export. A wrong constant still shows: it moves a triangle or changes its mask. (Revision 1 sampled with
// an interpolated uv and tints of (5 + k) / 20: exact .5 products and texel-boundary pixels made its checksum
// differ between implementations that were all correct.)
class DrawsScene : public FrameScene {
public:
  const char *Name() const override { return "draws"; }

  bool Init(ID3D11Device *dev, const Options &o) override {
    static const char vsSource[] =
        "cbuffer c : register(b0) { float4 place; float4 tint; };\n"
        "struct O { float4 pos : SV_Position; float4 tint : COLOR; };\n"
        "O main(uint id : SV_VertexID) {\n"
        "  float2 p = float2(id & 1, id >> 1);\n"
        "  O o;\n"
        "  o.pos = float4(place.xy + p * place.zw, 0, 1);\n"
        "  o.tint = tint;\n"
        "  return o;\n"
        "}\n";
    static const char psSource[] =
        "Texture2D<float4> t : register(t0);\n"
        "float4 main(float4 pos : SV_Position, float4 tint : COLOR) : SV_Target {\n"
        "  return t.Load(int3(int2(pos.xy) & 63, 0)) * tint;\n"
        "}\n";
    ComPtr<ID3DBlob> vb, pb;
    if (!Compile(vsSource, "vs_4_0", nullptr, vb) || !Compile(psSource, "ps_4_0", nullptr, pb) ||
        !Ok(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &m_vs), "draws VS") ||
        !Ok(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &m_ps), "draws PS"))
      return false;
    for (UINT i = 0; i < 4; i++)
      if (!PatternTexture(dev, 64, 1, i + 1, m_views[i]))
        return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!Ok(dev->CreateBuffer(&bd, nullptr, &m_constants), "draws constants") || !Rasterizer(dev, m_raster))
      return false;
    m_data.resize(o.draws);
    // Placed in pixels, then converted: the corner at (a + 1/4, b + 1/4) and whole-pixel legs whose lengths
    // differ by an odd number, so no edge ever passes through a pixel centre and the fill rule never decides
    // (WARP and NVIDIA disagreed on exactly such pixels). The float error of the conversion is far below the
    // 1/256-pixel snap step, so every vertex snaps to exactly the chosen position.
    const UINT legX = std::max(2u, (5 * o.width + 64) / 128);
    UINT legY = std::max(2u, (6 * o.height + 64) / 128);
    if ((legY - legX) % 2 == 0)
      legY++;
    for (UINT d = 0; d < o.draws; d++) {
      const uint32_t h = Hash(d + 1);
      Constants &c = m_data[d];
      const double x = double((h & 255) * o.width / 256) + 0.25;           // the right-angle corner, pixels
      const double y = double(((h >> 8) & 255) * o.height / 256) + 0.25;   // y down
      c.place[0] = float(x * 2.0 / o.width - 1.0);
      c.place[1] = float(1.0 - y * 2.0 / o.height);
      c.place[2] = float(legX * 2.0 / o.width);
      c.place[3] = float(legY * 2.0 / o.height);
      const uint32_t mask = 1 + (h >> 16) % 7;   // one to three channels, never black
      for (UINT k = 0; k < 3; k++)
        c.tint[k] = (mask >> k) & 1 ? 1.0f : 0.0f;
      c.tint[3] = 1.0f;
    }
    return true;
  }

  void Record(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, UINT width, UINT height) override {
    const float clear[4] = { 0, 0, 0, 1 };
    ctx->ClearRenderTargetView(rtv, clear);
    Viewport(ctx, width, height);
    ctx->RSSetState(m_raster.Get());
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *cb = m_constants.Get();
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    for (size_t d = 0; d < m_data.size(); d++) {
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        m_failures++;
        continue;
      }
      memcpy(m.pData, &m_data[d], sizeof(Constants));
      ctx->Unmap(cb, 0);
      ID3D11ShaderResourceView *view = m_views[d % 4].Get();
      ctx->PSSetShaderResources(0, 1, &view);
      ctx->Draw(3, 0);
    }
  }

  std::string Parameters() const override { return "\"draws\":" + std::to_string(m_data.size()); }
  UINT Failures() const override { return m_failures; }

private:
  struct Constants {
    float place[4];
    float tint[4];
  };
  ComPtr<ID3D11VertexShader> m_vs;
  ComPtr<ID3D11PixelShader> m_ps;
  ComPtr<ID3D11ShaderResourceView> m_views[4];
  ComPtr<ID3D11Buffer> m_constants;
  ComPtr<ID3D11RasterizerState> m_raster;
  std::vector<Constants> m_data;
  UINT m_failures = 0;
};

// GPU-bound: a few blended full-screen layers, each sampling a mipmapped texture four times, plus an ALU loop.
// The loop contracts (derivative at most 0.5 + 0.07 * 6.28 < 1), so the rounding differences that trilinear
// filtering and sin legitimately have between implementations shrink instead of growing. (Revision 1 iterated
// frac(v * 1.37 + sin(v * 6.28 + i)), a chaotic map: one step of filter weight became full-range noise.)
class FillScene : public FrameScene {
public:
  const char *Name() const override { return "fill"; }

  bool Init(ID3D11Device *dev, const Options &o) override {
    static const char psSource[] =
        "Texture2D<float4> t : register(t0);\n"
        "SamplerState s : register(s0);\n"
        "cbuffer c : register(b0) { float4 layer; };\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "  float2 uv = pos.xy * layer.xy + layer.zw;\n"
        "  float4 a = t.Sample(s, uv) + t.Sample(s, uv * 1.7 + 0.1) + t.Sample(s, uv * 0.6 + 0.3) +\n"
        "             t.Sample(s, uv.yx);\n"
        "  float v = a.x;\n"
        "  [unroll] for (int i = 0; i < 8; i++) v = 0.5 * v + 0.25 + 0.07 * sin(v * 6.28 + i);\n"
        "  return float4(a.rgb * 0.25 * v, 0.35);\n"
        "}\n";
    ComPtr<ID3DBlob> pb;
    if (!FullScreenVs(dev, m_vs) || !Compile(psSource, "ps_4_0", nullptr, pb) ||
        !Ok(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &m_ps), "fill PS") ||
        !PatternTexture(dev, 512, 10, 11, m_view) ||
        !Sampler(dev, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, m_sampler) ||
        !Rasterizer(dev, m_raster))
      return false;
    D3D11_BLEND_DESC bd = {};
    D3D11_RENDER_TARGET_BLEND_DESC &rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!Ok(dev->CreateBlendState(&bd, &m_blend), "fill blend"))
      return false;
    m_layers.resize(o.layers);
    for (UINT i = 0; i < o.layers; i++) {
      const float scale = (1.0f + 0.37f * float(i)) / 512.0f;
      const float layer[4] = { scale, scale * 1.1f, 0.13f * float(i), 0.07f * float(i) };
      D3D11_BUFFER_DESC cd = {};
      cd.ByteWidth = sizeof layer;
      cd.Usage = D3D11_USAGE_IMMUTABLE;
      cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      const D3D11_SUBRESOURCE_DATA init = { layer, 0, 0 };
      if (!Ok(dev->CreateBuffer(&cd, &init, &m_layers[i]), "fill constants"))
        return false;
    }
    return true;
  }

  void Record(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, UINT width, UINT height) override {
    const float clear[4] = { 0.1f, 0.1f, 0.1f, 1 };
    ctx->ClearRenderTargetView(rtv, clear);
    Viewport(ctx, width, height);
    ctx->RSSetState(m_raster.Get());
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(m_blend.Get(), nullptr, 0xffffffffu);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11ShaderResourceView *view = m_view.Get();
    ID3D11SamplerState *sampler = m_sampler.Get();
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &view);
    ctx->PSSetSamplers(0, 1, &sampler);
    for (const ComPtr<ID3D11Buffer> &layer : m_layers) {
      ID3D11Buffer *cb = layer.Get();
      ctx->PSSetConstantBuffers(0, 1, &cb);
      ctx->Draw(3, 0);
    }
  }

  std::string Parameters() const override { return "\"layers\":" + std::to_string(m_layers.size()); }

private:
  ComPtr<ID3D11VertexShader> m_vs;
  ComPtr<ID3D11PixelShader> m_ps;
  ComPtr<ID3D11ShaderResourceView> m_view;
  ComPtr<ID3D11SamplerState> m_sampler;
  ComPtr<ID3D11RasterizerState> m_raster;
  ComPtr<ID3D11BlendState> m_blend;
  std::vector<ComPtr<ID3D11Buffer>> m_layers;
};

// ---------------------------------------------------------------------------------------------------------
// Render target: the swap chain's back buffer, or an offscreen image paced like a swap chain

struct Target {
  HWND window = nullptr;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11Texture2D> staging;
  ComPtr<ID3D11Query> throttle[kThrottle];
  UINT width = 0, height = 0;
};

LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w, m, a, b); }

void Pump() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

bool Staging(ID3D11Device *dev, UINT width, UINT height, ComPtr<ID3D11Texture2D> &staging) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = width;
  d.Height = height;
  d.MipLevels = d.ArraySize = 1;
  d.Format = kFormat;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  return Ok(dev->CreateTexture2D(&d, nullptr, &staging), "staging texture");
}

bool RenderTexture(ID3D11Device *dev, UINT width, UINT height, ComPtr<ID3D11Texture2D> &texture,
                   ComPtr<ID3D11RenderTargetView> &rtv) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = width;
  d.Height = height;
  d.MipLevels = d.ArraySize = 1;
  d.Format = kFormat;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  return Ok(dev->CreateTexture2D(&d, nullptr, &texture), "render texture") &&
         Ok(dev->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "render target view");
}

bool CreateTarget(ID3D11Device *dev, const Options &o, Target &t) {
  t.width = o.width;
  t.height = o.height;
  if (!Staging(dev, o.width, o.height, t.staging))
    return false;
  if (!o.window) {
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    for (ComPtr<ID3D11Query> &q : t.throttle)
      if (!Ok(dev->CreateQuery(&qd, &q), "throttle query"))
        return false;
    return RenderTexture(dev, o.width, o.height, t.texture, t.rtv);
  }
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"BC250D3D11Bench";
  if (!RegisterClassW(&wc)) {
    puts("FAIL RegisterClassW");
    return false;
  }
  t.window = CreateWindowExW(0, wc.lpszClassName, L"d3d11bench", WS_POPUP | WS_VISIBLE, 32, 32, int(o.width),
                             int(o.height), nullptr, nullptr, wc.hInstance, nullptr);
  if (!t.window) {
    puts("FAIL CreateWindowExW");
    return false;
  }
  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (!Ok(dev->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "IDXGIDevice") ||
      !Ok(dxgiDevice->GetAdapter(&adapter), "device adapter") ||
      !Ok(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2"))
    return false;
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = o.width;
  sd.Height = o.height;
  sd.Format = kFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  if (!Ok(factory->CreateSwapChainForHwnd(dev, t.window, &sd, nullptr, nullptr, &t.swap), "swap chain") ||
      !Ok(factory->MakeWindowAssociation(t.window, DXGI_MWA_NO_ALT_ENTER), "window association"))
    return false;
  // With the flip model, buffer 0 is always the current back buffer: the runtime rotates the resource
  // identities at each Present, and a view created on buffer 0 follows them.
  return Ok(t.swap->GetBuffer(0, IID_PPV_ARGS(&t.texture)), "back buffer") &&
         Ok(dev->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv), "back buffer view");
}

bool BeginFrame(Target &t, ID3D11DeviceContext *ctx, UINT frame) {
  if (t.swap || frame < kThrottle)
    return true;
  return WaitEvent(ctx, t.throttle[frame % kThrottle].Get());
}

bool EndFrame(Target &t, ID3D11DeviceContext *ctx, UINT frame) {
  if (t.swap) {
    // An occluded window skips frames, which would make the numbers meaningless: treat it as a failure.
    const HRESULT hr = t.swap->Present(0, 0);
    if (hr != S_OK) {
      printf("FAIL Present hr=0x%08lx\n", static_cast<unsigned long>(hr));
      return false;
    }
    return true;
  }
  ctx->End(t.throttle[frame % kThrottle].Get());
  ctx->Flush();
  return true;
}

// Functional ownership control, deliberately synchronized for exact readback.
// It does not enter the performance comparisons of draws/fill/shaders.
bool RunResize(ID3D11Device *dev, ID3D11DeviceContext *ctx, Target &t, const Options &o,
               std::string &json, std::string &summary) {
  if (!t.swap || !t.window) return false;
  const UINT sizes[][2] = {{o.width,o.height},{65,33},{127,79},{o.width,o.height}};
  const float colors[][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1}};
  const uint8_t pixels[][4] = {{0,0,255,255},{0,255,0,255},{255,0,0,255}};
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT,0};
  ComPtr<ID3D11Query> retired;
  if (!Ok(dev->CreateQuery(&qd,&retired),"resize completion query")) return false;
  std::string steps;
  for (UINT step=0; step<ARRAYSIZE(sizes); ++step) {
    if (Expired()) return false;
    if (step) {
      // No context binding or retained back-buffer reference may cross ResizeBuffers.
      ctx->ClearState();
      t.rtv.Reset(); t.texture.Reset(); t.staging.Reset();
      ctx->Flush();
      t.width=sizes[step][0]; t.height=sizes[step][1];
      if (!SetWindowPos(t.window,nullptr,32,32,int(t.width),int(t.height),SWP_NOZORDER|SWP_NOACTIVATE))
        return Ok(HRESULT_FROM_WIN32(GetLastError()),"resize window");
      Pump();
      if (!Ok(t.swap->ResizeBuffers(2,t.width,t.height,kFormat,0),"ResizeBuffers") ||
          !Ok(t.swap->GetBuffer(0,IID_PPV_ARGS(&t.texture)),"resized back buffer") ||
          !Ok(dev->CreateRenderTargetView(t.texture.Get(),nullptr,&t.rtv),"resized RTV") ||
          !Staging(dev,t.width,t.height,t.staging)) return false;
    }
    D3D11_TEXTURE2D_DESC actual{}; t.texture->GetDesc(&actual);
    if (actual.Width!=t.width || actual.Height!=t.height || actual.Format!=kFormat) return false;
    std::string frames;
    for (UINT frame=0; frame<3; ++frame) {
      if (Expired()) return false;
      Pump();
      const UINT color=(step+frame)%3;
      ctx->ClearRenderTargetView(t.rtv.Get(),colors[color]);
      ctx->CopyResource(t.staging.Get(),t.texture.Get());
      if (!EndFrame(t,ctx,frame)) return false;
      ctx->End(retired.Get()); ctx->Flush();
      if (!WaitEvent(ctx,retired.Get())) return false;
      std::string checksum;
      const std::string name="resize-"+std::to_string(step)+"-"+std::to_string(frame);
      if (!Checksum(ctx,t.staging.Get(),t.width,t.height,o,name.c_str(),checksum)) return false;
      uint64_t expected=14695981039346656037ull;
      for (UINT64 n=0; n<UINT64(t.width)*t.height; ++n)
        for (uint8_t b:pixels[color]) { expected^=b; expected*=1099511628211ull; }
      if (checksum!=Hex(expected,16)) {
        printf("FAIL resize pixels step=%u frame=%u got=%s expected=%s\n",step,frame,checksum.c_str(),Hex(expected,16).c_str());
        return false;
      }
      frames+=(frame ? "," : "")+Quote(checksum);
    }
    steps+=(step ? "," : "")+std::string("{\"width\":")+std::to_string(t.width)+
      ",\"height\":"+std::to_string(t.height)+",\"presents\":3,\"checksums\":["+frames+"]}";
  }
  ctx->ClearState(); ctx->Flush();
  json="{\"name\":\"resize\",\"resizes\":3,\"presents\":12,\"pixel_checks\":12,\"steps\":["+steps+"]}";
  summary+="PASS resize: 3 ResizeBuffers, 12 Presents and exact color checks\n";
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Runs

bool RunFrames(FrameScene &scene, ID3D11Device *dev, ID3D11DeviceContext *ctx, Target &t, const Options &o,
               std::string &json, std::string &summary) {
  GpuTimer timer;
  if (!timer.Init(dev))
    return false;
  std::vector<double> starts, record, present;
  const UINT total = o.warmup + o.frames;
  for (UINT f = 0; f < total; f++) {
    if (Expired())
      return false;
    if (t.window)
      Pump();
    const bool measured = f >= o.warmup;
    const double start = NowMs();
    if (!BeginFrame(t, ctx, f))
      return false;
    const double recordStart = NowMs();
    timer.Begin(ctx, measured);
    scene.Record(ctx, t.rtv.Get(), t.width, t.height);
    timer.End(ctx);
    if (f + 1 == total)
      ctx->CopyResource(t.staging.Get(), t.texture.Get());
    const double presentStart = NowMs();
    if (!EndFrame(t, ctx, f))
      return false;
    const double end = NowMs();
    timer.Poll(ctx);
    if (measured) {
      starts.push_back(start);
      record.push_back(presentStart - recordStart);
      present.push_back(end - presentStart);
    }
  }
  timer.Drain(ctx);
  if (Expired())
    return false;
  std::string checksum;
  if (!Checksum(ctx, t.staging.Get(), t.width, t.height, o, scene.Name(), checksum))
    return false;
  std::vector<double> intervals;
  for (size_t i = 1; i < starts.size(); i++)
    intervals.push_back(starts[i] - starts[i - 1]);
  const Stats frame = Summarize(intervals), gpu = Summarize(timer.samples);
  json = "{\"name\":" + Quote(scene.Name()) + ",\"frames\":" + std::to_string(o.frames) +
         ",\"warmup\":" + std::to_string(o.warmup) + "," + scene.Parameters() + ",\"frame_ms\":" + StatsJson(frame) +
         ",\"record_ms\":" + StatsJson(Summarize(record)) + ",\"present_ms\":" + StatsJson(Summarize(present)) +
         ",\"gpu_ms\":" + StatsJson(gpu) + ",\"gpu_disjoint\":" + std::to_string(timer.disjoint) +
         ",\"gpu_failures\":" + std::to_string(timer.failures) +
         ",\"api_failures\":" + std::to_string(scene.Failures()) + ",\"checksum\":" + Quote(checksum) + "}";
  char line[256];
  sprintf_s(line, "SCENE %s frame_ms=%.4f gpu_ms=%.4f record_ms=%.4f checksum=%s\n", scene.Name(), frame.median,
            gpu.median, Summarize(record).median, checksum.c_str());
  summary += line;
  if (scene.Failures() || timer.failures) {
    printf("FAIL %s: %u API and %u timestamp failures\n", scene.Name(), scene.Failures(), timer.failures);
    return false;
  }
  return true;
}

// Load cost: the first use of each new pixel shader. Every variant differs in its constants, so each one is
// translated and gets its own pipeline. Each variant draws its own tile of the target, so a variant that did
// not run leaves a black tile in the checksum, and rounding differences do not pile up the way they did under
// revision 1's additive blend of all variants. The loop contracts and the output has no frac, whose jumps
// turned rounding differences into full steps (revision 1).
bool RunShaders(ID3D11Device *dev, ID3D11DeviceContext *ctx, const Options &o, std::string &json,
                std::string &summary) {
  static const char psSource[] =
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  float v = pos.x * K1 + pos.y * K2;\n"
      "  [unroll] for (int i = 0; i < K3; i++) v = 0.5 * v + 0.25 + 0.15 * sin(v * 3.0 + i);\n"
      "  return float4(v, saturate(v * K1 * 97.0), saturate(v * K2 * 89.0), 1);\n"
      "}\n";
  std::vector<ComPtr<ID3DBlob>> blobs(o.shaders);
  for (UINT k = 0; k < o.shaders; k++) {
    char k1[32], k2[32], k3[16];
    sprintf_s(k1, "%.6f", 0.001 * double(k + 1));
    sprintf_s(k2, "%.6f", 0.0007 * double(k + 3));
    sprintf_s(k3, "%u", 1 + k % 8);
    const D3D_SHADER_MACRO macros[] = { { "K1", k1 }, { "K2", k2 }, { "K3", k3 }, { nullptr, nullptr } };
    if (!Compile(psSource, "ps_4_0", macros, blobs[k]))
      return false;
  }
  UINT tiles = 1;
  while (tiles * tiles < o.shaders)
    tiles++;
  const UINT tile = kShaderTarget / tiles;   // 32 pixels for 64 variants, 8 for the maximum of 1024
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11Texture2D> texture, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11Query> idle;
  const D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
  if (!FullScreenVs(dev, vs) || !RenderTexture(dev, kShaderTarget, kShaderTarget, texture, rtv) ||
      !Staging(dev, kShaderTarget, kShaderTarget, staging) || !Rasterizer(dev, raster) ||
      !Ok(dev->CreateQuery(&qd, &idle), "idle query"))
    return false;

  const float clear[4] = { 0, 0, 0, 0 };
  ID3D11RenderTargetView *view = rtv.Get();
  ctx->ClearRenderTargetView(view, clear);
  ctx->RSSetState(raster.Get());
  ctx->OMSetRenderTargets(1, &view, nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(), nullptr, 0);
  ctx->End(idle.Get());
  ctx->Flush();
  if (!WaitEvent(ctx, idle.Get()))
    return false;

  std::vector<ComPtr<ID3D11PixelShader>> shaders(o.shaders);
  std::vector<double> create(o.shaders), draw(o.shaders);
  const double start = NowMs();
  for (UINT k = 0; k < o.shaders; k++) {
    if (Expired())
      return false;
    const double t0 = NowMs();
    if (!Ok(dev->CreatePixelShader(blobs[k]->GetBufferPointer(), blobs[k]->GetBufferSize(), nullptr, &shaders[k]),
            "variant PS"))
      return false;
    const double t1 = NowMs();
    const D3D11_VIEWPORT vp = { float(k % tiles * tile), float(k / tiles * tile), float(tile), float(tile), 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(shaders[k].Get(), nullptr, 0);
    ctx->Draw(3, 0);
    create[k] = t1 - t0;
    draw[k] = NowMs() - t1;
  }
  ctx->CopyResource(staging.Get(), texture.Get());
  ctx->End(idle.Get());
  ctx->Flush();
  if (!WaitEvent(ctx, idle.Get()))
    return false;
  const double totalMs = NowMs() - start;
  std::string checksum;
  if (!Checksum(ctx, staging.Get(), kShaderTarget, kShaderTarget, o, "shaders", checksum))
    return false;
  ctx->ClearState();
  json = "{\"name\":\"shaders\",\"shaders\":" + std::to_string(o.shaders) + ",\"total_ms\":" + Number(totalMs) +
         ",\"create_ms\":" + StatsJson(Summarize(create)) + ",\"draw_ms\":" + StatsJson(Summarize(draw)) +
         ",\"checksum\":" + Quote(checksum) + "}";
  char line[256];
  sprintf_s(line, "SCENE shaders total_ms=%.4f checksum=%s\n", totalMs, checksum.c_str());
  summary += line;
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Identity of the run: which d3d11.dll and which driver modules the process actually loaded

std::wstring Directory(const std::wstring &path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Lower(std::wstring s) {
  for (wchar_t &c : s)
    c = static_cast<wchar_t>(towlower(c));
  return s;
}

std::wstring ModulePath(HMODULE module) {
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  path.resize(n);
  return path;
}

// SHA-256 of a file in upper-case hex, as Get-FileHash prints it; empty on any failure.
std::string FileSha256(const std::wstring &path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return std::string();
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  UCHAR digest[32] = {};
  bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
            BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
  std::vector<UCHAR> buffer(1u << 20);
  while (ok) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr))
      ok = false;
    else if (!read)
      break;
    else
      ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0));
  }
  ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
  if (hash)
    BCryptDestroyHash(hash);
  if (alg)
    BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(file);
  std::string hex;
  for (UCHAR b : digest) {
    static const char digits[] = "0123456789ABCDEF";
    hex += digits[b >> 4];
    hex += digits[b & 15];
  }
  return ok ? hex : std::string();
}

// icds: every loaded Vulkan driver (a module exporting vk_icdGetInstanceProcAddr) with the SHA-256 of its file. On
// the per-application path the Vulkan loader picks it, and in an elevated process the loader ignores
// VK_DRIVER_FILES and VK_ICD_FILENAMES; on the system path the UMD loads its own. Only the hash says both paths
// ran the same driver.
std::string ModulesJson(std::string &d3d11Location, std::string &icds) {
  const std::wstring exeDir = Lower(Directory(ModulePath(nullptr)));
  icds = "[]";
  std::vector<HMODULE> modules(1024);
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed))
    return "[]";
  modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
  static const wchar_t *const names[] = { L"d3d11.dll", L"dxgi.dll", L"d3d10core.dll", L"d3d11on12.dll",
                                          L"vulkan-1.dll", L"d3dcompiler_47.dll" };
  std::string json = "[";
  icds = "[";
  for (HMODULE m : modules) {
    const std::wstring path = ModulePath(m), lower = Lower(path);
    const std::wstring base = lower.substr(lower.find_last_of(L"\\/") + 1);
    const bool icd = GetProcAddress(m, "vk_icdGetInstanceProcAddr") != nullptr;
    bool wanted = icd || base.rfind(L"bc250", 0) == 0 || lower.find(L"\\driverstore\\") != std::wstring::npos;
    for (const wchar_t *name : names)
      wanted = wanted || base == name;
    if (!wanted)
      continue;
    if (icd)
      icds += std::string(icds.size() > 1 ? "," : "") + "{\"path\":" + Quote(Utf8(path)) +
              ",\"sha256\":" + Quote(FileSha256(path)) + "}";
    if (base == L"d3d11.dll")
      d3d11Location = Directory(lower) == exeDir ? "app-local" : "system";
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    const uint64_t bytes = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)
                               ? (uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow)
                               : 0;
    if (json.size() > 1)
      json += ",";
    json += "{\"path\":" + Quote(Utf8(path)) + ",\"bytes\":" + std::to_string(bytes) + "}";
  }
  icds += "]";
  return json + "]";
}

// Variables that change what either path runs: DXVK's own, and those of the Vulkan loader and the Mesa ICD, which
// both paths share on the lab (its shader cache included).
bool ConfigurationVariable(const std::wstring &entry) {
  const std::wstring name = Lower(entry.substr(0, entry.find(L'=')));
  for (const wchar_t *prefix : { L"dxvk_", L"vk_", L"mesa_", L"radv_", L"aco_", L"bc250_" })
    if (name.compare(0, wcslen(prefix), prefix) == 0)
      return true;
  return false;
}

std::string EnvironmentJson() {
  std::string json = "{";
  LPWCH block = GetEnvironmentStringsW();
  if (!block)
    return "{}";
  for (LPWCH p = block; *p; p += wcslen(p) + 1) {
    const std::wstring entry = p;
    if (!ConfigurationVariable(entry))
      continue;
    const size_t eq = entry.find(L'=');
    if (eq == std::wstring::npos)
      continue;
    if (json.size() > 1)
      json += ",";
    json += Quote(Utf8(entry.substr(0, eq))) + ":" + Quote(Utf8(entry.substr(eq + 1)));
  }
  FreeEnvironmentStringsW(block);
  return json + "}";
}

bool FileExists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

const char *LevelName(D3D_FEATURE_LEVEL level) {
  switch (level) {
  case D3D_FEATURE_LEVEL_11_1: return "11_1";
  case D3D_FEATURE_LEVEL_11_0: return "11_0";
  case D3D_FEATURE_LEVEL_10_1: return "10_1";
  case D3D_FEATURE_LEVEL_10_0: return "10_0";
  default: return "other";
  }
}

std::string UtcNow() {
  SYSTEMTIME t;
  GetSystemTime(&t);
  char b[32];
  sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds);
  return b;
}

bool WriteAtomically(const std::wstring &path, const std::string &text) {
  const std::wstring temp = path + L".tmp";
  HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE)
    return false;
  DWORD wrote = 0;
  bool ok = WriteFile(f, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
  ok = ok && FlushFileBuffers(f);
  CloseHandle(f);
  return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// ---------------------------------------------------------------------------------------------------------
// Arguments

const char kUsage[] =
    "d3d11bench.exe --mode window|offscreen [options]\n"
    "  --scenes LIST     draws,fill,shaders (default); resize alone in window mode\n"
    "  --size WxH        render size (default 1280x720)\n"
    "  --frames N        measured frames per frame scene (default 300)\n"
    "  --warmup N        unmeasured frames before them (default 30)\n"
    "  --draws N         draws per frame in the draws scene (default 2000)\n"
    "  --layers N        full-screen layers per frame in the fill scene (default 8)\n"
    "  --shaders N       pixel shader variants in the shaders scene (default 64)\n"
    "  --adapter V[:D]   hexadecimal vendor and optional device id (default: first hardware adapter)\n"
    "  --adapter warp    Microsoft's software rasterizer, a CPU reference image on any Windows machine\n"
    "  --deadline S      whole-run limit in seconds, at most 170 (default 120)\n"
    "  --out FILE        write the result JSON there instead of standard output\n"
    "  --dump DIR        also write each scene's checksummed image to DIR\\<scene>.pam (existing directory)\n"
    "Exit: 0 measured, 1 API failure, 2 bad arguments, 3 deadline, 4 device removed.\n"
    "--mode window needs an interactive session with DWM; offscreen runs anywhere.\n";

bool ParseUint(const wchar_t *s, UINT lo, UINT hi, UINT &v, int base = 10) {
  wchar_t *end = nullptr;
  const unsigned long x = wcstoul(s, &end, base);
  if (!*s || *end || x < lo || x > hi)
    return false;
  v = UINT(x);
  return true;
}

bool ParseArgs(int argc, wchar_t **argv, Options &o) {
  for (int i = 1; i < argc; i++) {
    const std::wstring a = argv[i];
    if (a == L"--help") {
      o.help = true;
      return true;
    }
    if (i + 1 >= argc)
      return false;
    const wchar_t *v = argv[++i];
    if (a == L"--mode") {
      if (!wcscmp(v, L"window"))
        o.window = true;
      else if (!wcscmp(v, L"offscreen"))
        o.window = false;
      else
        return false;
      o.modeGiven = true;
    } else if (a == L"--scenes") {
      const std::string list = Utf8(v);
      size_t pos = 0;
      while (pos <= list.size()) {
        const size_t comma = std::min(list.find(',', pos), list.size());
        const std::string name = list.substr(pos, comma - pos);
        if (name != "draws" && name != "fill" && name != "shaders" && name != "resize")
          return false;
        o.scenes.push_back(name);
        pos = comma + 1;
      }
    } else if (a == L"--size") {
      const wchar_t *x = wcschr(v, L'x');
      if (!x)
        return false;
      const std::wstring w(v, x);
      if (!ParseUint(w.c_str(), 16, 8192, o.width) || !ParseUint(x + 1, 16, 8192, o.height))
        return false;
    } else if (a == L"--frames") {
      if (!ParseUint(v, 2, 100000, o.frames))
        return false;
    } else if (a == L"--warmup") {
      if (!ParseUint(v, 0, 100000, o.warmup))
        return false;
    } else if (a == L"--draws") {
      if (!ParseUint(v, 1, 100000, o.draws))
        return false;
    } else if (a == L"--layers") {
      if (!ParseUint(v, 1, 64, o.layers))
        return false;
    } else if (a == L"--shaders") {
      if (!ParseUint(v, 1, 1024, o.shaders))
        return false;
    } else if (a == L"--adapter" && !wcscmp(v, L"warp")) {
      o.warp = true;
    } else if (a == L"--adapter") {
      const wchar_t *colon = wcschr(v, L':');
      const std::wstring vendor = colon ? std::wstring(v, colon) : std::wstring(v);
      if (!ParseUint(vendor.c_str(), 1, 0xffff, o.vendor, 16) || (colon && !ParseUint(colon + 1, 1, 0xffff, o.device, 16)))
        return false;
    } else if (a == L"--deadline") {
      UINT s = 0;
      if (!ParseUint(v, 1, UINT(kMaxDeadlineSeconds), s))
        return false;
      o.deadlineSeconds = double(s);
    } else if (a == L"--out") {
      o.out = v;
    } else if (a == L"--dump") {
      o.dump = v;
      const DWORD attributes = GetFileAttributesW(v);
      if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    } else {
      return false;
    }
  }
  if (std::find(o.scenes.begin(),o.scenes.end(),"resize")!=o.scenes.end() && (!o.window || o.scenes.size()!=1))
    return false;
  return o.modeGiven;
}

// ---------------------------------------------------------------------------------------------------------

int Run(const Options &o) {
  const double started = NowMs();
  g_deadline = started + o.deadlineSeconds * 1000.0;
  SetProcessDPIAware();

  ComPtr<IDXGIFactory1> factory;
  if (!Ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory"))
    return kApiFailure;
  ComPtr<IDXGIAdapter1> adapter;
  DXGI_ADAPTER_DESC1 desc = {};
  for (UINT n = 0; !o.warp; n++) {
    ComPtr<IDXGIAdapter1> a;
    const HRESULT hr = factory->EnumAdapters1(n, &a);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (!Ok(hr, "enumerate adapters"))
      return kApiFailure;
    DXGI_ADAPTER_DESC1 d = {};
    if (!Ok(a->GetDesc1(&d), "adapter description"))
      return kApiFailure;
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
      continue;
    if ((o.vendor && d.VendorId != o.vendor) || (o.device && d.DeviceId != o.device))
      continue;
    adapter = a;
    desc = d;
    break;
  }
  if (!adapter && !o.warp) {
    puts("FAIL no matching hardware adapter");
    return kApiFailure;
  }

  const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                       D3D_FEATURE_LEVEL_10_0 };
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level = {};
  if (!Ok(D3D11CreateDevice(adapter.Get(), o.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &level, &ctx),
          "D3D11CreateDevice"))
    return kApiFailure;
  if (o.warp) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> warpAdapter;
    if (!Ok(dev.As(&dxgi), "WARP DXGI device") || !Ok(dxgi->GetAdapter(&warpAdapter), "WARP adapter") ||
        !Ok(warpAdapter.As(&adapter), "WARP adapter1") || !Ok(adapter->GetDesc1(&desc), "WARP adapter description"))
      return kApiFailure;
  }
  UINT latency = 0;
  ComPtr<IDXGIDevice1> dxgiDevice1;
  if (SUCCEEDED(dev.As(&dxgiDevice1)))
    dxgiDevice1->GetMaximumFrameLatency(&latency);

  Target target;
  std::vector<std::string> scenes = o.scenes;
  if (scenes.empty())
    scenes = { "draws", "fill", "shaders" };
  std::string scenesJson, summary;
  int result = kPass;
  if (!CreateTarget(dev.Get(), o, target)) {
    result = kApiFailure;
  } else {
    for (const std::string &name : scenes) {
      std::string json;
      bool ok = false;
      if (name == "resize") {
        ok = RunResize(dev.Get(),ctx.Get(),target,o,json,summary);
      } else if (name == "shaders") {
        ok = RunShaders(dev.Get(), ctx.Get(), o, json, summary);
      } else {
        std::unique_ptr<FrameScene> scene;
        if (name == "draws")
          scene = std::make_unique<DrawsScene>();
        else
          scene = std::make_unique<FillScene>();
        ok = scene->Init(dev.Get(), o) && RunFrames(*scene, dev.Get(), ctx.Get(), target, o, json, summary);
        ctx->ClearState();
      }
      if (!ok) {
        result = g_expired ? kDeadline : kApiFailure;
        break;
      }
      scenesJson += (scenesJson.empty() ? "" : ",") + json;
    }
  }
  const HRESULT removed = dev->GetDeviceRemovedReason();
  if (FAILED(removed)) {
    printf("FAIL device removed reason=0x%08lx\n", static_cast<unsigned long>(removed));
    result = kDeviceRemoved;
  }

  std::string d3d11Location = "unknown", icds;
  const std::string modules = ModulesJson(d3d11Location, icds);
  const std::wstring exeDir = Directory(ModulePath(nullptr));
  wchar_t cwd[MAX_PATH] = {};
  GetCurrentDirectoryW(MAX_PATH, cwd);
  const std::string json =
      "{\"tool\":\"d3d11bench\",\"format\":1,\"scene_revision\":" + std::to_string(kSceneRevision) +
      ",\"utc\":" + Quote(UtcNow()) + ",\"result\":" +
      Quote(result == kPass ? "measured" : "failed") + ",\"exit\":" + std::to_string(result) + ",\"mode\":" +
      Quote(o.window ? "window" : "offscreen") + ",\"width\":" + std::to_string(o.width) +
      ",\"height\":" + std::to_string(o.height) + ",\"feature_level\":" + Quote(LevelName(level)) +
      ",\"frame_latency\":" + std::to_string(latency) + ",\"adapter\":{\"vendor\":" + Quote(Hex(desc.VendorId, 4)) +
      ",\"device\":" + Quote(Hex(desc.DeviceId, 4)) + ",\"description\":" + Quote(Utf8(desc.Description)) +
      "},\"d3d11\":" + Quote(d3d11Location) + ",\"modules\":" + modules + ",\"icds\":" + icds +
      ",\"environment\":" + EnvironmentJson() +
      ",\"dxvk_conf\":{\"exe_dir\":" + (FileExists(exeDir + L"\\dxvk.conf") ? "true" : "false") +
      ",\"cwd\":" + (FileExists(std::wstring(cwd) + L"\\dxvk.conf") ? "true" : "false") +
      "},\"scenes\":[" + scenesJson + "],\"elapsed_ms\":" + Number(NowMs() - started) + "}\n";

  fputs(summary.c_str(), stdout);
  if (o.out.empty()) {
    fputs(json.c_str(), stdout);
  } else if (!WriteAtomically(o.out, json)) {
    puts("FAIL writing the result file");
    if (result == kPass)
      result = kApiFailure;
  }
  printf("%s d3d11bench mode=%s d3d11=%s adapter=%04x:%04x level=%s\n", result == kPass ? "PASS" : "FAIL",
         o.window ? "window" : "offscreen", d3d11Location.c_str(), desc.VendorId, desc.DeviceId, LevelName(level));

  ctx->ClearState();
  ctx->Flush();
  target.rtv.Reset();
  target.texture.Reset();
  target.swap.Reset();
  if (target.window)
    DestroyWindow(target.window);
  return result;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  // Buffered on purpose: an unbuffered line into a PowerShell pipe costs milliseconds.
  setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
  Options o;
  if (!ParseArgs(argc, argv, o)) {
    fputs(kUsage, stdout);
    fflush(stdout);
    return kBadArguments;
  }
  if (o.help) {
    fputs(kUsage, stdout);
    fflush(stdout);
    return kPass;
  }
  const int result = Run(o);
  fflush(stdout);
  return result;
}
