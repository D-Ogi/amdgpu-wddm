// d3d11fl12: bounded witness for a D3D11 device at FL12 through the system runtime. It creates the device on one
// hardware adapter, prints the feature level and the OPTIONS1/2/3 answers, and runs one tiled-resource sequence whose
// every step is checked against an exact CPU expectation:
//   1. a 256x128 RGBA8 tiled texture (two 64 KiB tiles) with only tile 0 mapped (UpdateTileMappings);
//   2. a full-screen draw of a position gradient, a TiledResourceBarrier, CopyResource readback: tile 0 holds the
//      gradient, the unmapped tile reads zero (tier 2 and above);
//   3. tile 1 mapped, UpdateTiles with a pattern, CopyTiles of tile 0 into a buffer, readback of both;
//   4. CopyTileMappings onto a second tiled texture, which then reads the same bytes; ResizeTilePool grows the pool.
// No window, no swap chain. Exit 0 only when every check passed. The whole run is limited by --deadline.
#include <windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth = 256, kHeight = 128, kTile = 128, kTileBytes = 65536;
unsigned g_failures = 0;
FILE *g_out = nullptr;  // --out: a copy of every line, for runners whose debugger wrapper drops stdout

void Print(const char *format, ...) {
  va_list args;
  va_start(args, format);
  std::vprintf(format, args);
  va_end(args);
  if (g_out) {
    va_start(args, format);
    std::vfprintf(g_out, format, args);
    va_end(args);
    std::fflush(g_out);
  }
}

void Check(bool ok, const char *what) {
  Print("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

bool Hr(HRESULT hr, const char *what) {
  if (FAILED(hr)) {
    Print("FAIL %s: 0x%08lX\n", what, static_cast<unsigned long>(hr));
    ++g_failures;
    return false;
  }
  return true;
}

struct Options {
  UINT vendor = 0, device = 0;  // 0: first hardware adapter
  D3D_FEATURE_LEVEL minimum = D3D_FEATURE_LEVEL_12_1;
  UINT deadline = 60;
  std::wstring out;
};

void Usage() {
  Print(
      "d3d11fl12.exe [--adapter V[:D]] [--min 12_0|12_1] [--deadline S]\n"
      "  --adapter V[:D]  hexadecimal vendor and optional device id (default: first hardware adapter)\n"
      "  --min LEVEL      lowest accepted device feature level (default 12_1)\n"
      "  --deadline S     whole-run limit in seconds, at most 170 (default 60)\n"
      "  --out FILE       also write every output line to FILE (created, never overwritten)\n");
}

bool ParseArgs(int argc, wchar_t **argv, Options &o) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--help") {
      Usage();
      std::exit(0);
    }
    if (i + 1 >= argc) return false;
    const std::wstring v = argv[++i];
    if (a == L"--adapter") {
      wchar_t *end = nullptr;
      o.vendor = static_cast<UINT>(std::wcstoul(v.c_str(), &end, 16));
      if (end && *end == L':') o.device = static_cast<UINT>(std::wcstoul(end + 1, &end, 16));
      if (!o.vendor || !end || *end) return false;
    } else if (a == L"--min") {
      if (v == L"12_0") o.minimum = D3D_FEATURE_LEVEL_12_0;
      else if (v == L"12_1") o.minimum = D3D_FEATURE_LEVEL_12_1;
      else return false;
    } else if (a == L"--out") {
      o.out = v;
    } else if (a == L"--deadline") {
      o.deadline = static_cast<UINT>(std::wcstoul(v.c_str(), nullptr, 10));
      if (!o.deadline || o.deadline > 170) return false;
    } else {
      return false;
    }
  }
  return true;
}

// The gradient the pixel shader writes: exact n/255 values, so UNORM conversion returns n.
uint32_t Gradient(UINT x, UINT y) { return (x & 255u) | ((y & 255u) << 8) | (0x40u << 16) | (0xFFu << 24); }
uint32_t Pattern(UINT x, UINT y) { return ((x * 7u) ^ (y * 13u)) * 0x01010101u ^ 0x00A5005Au; }

const char kShader[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  uint2 p = uint2(pos.xy) & 255;
  return float4(p.x / 255.0, p.y / 255.0, 64 / 255.0, 1);
}
)";

bool Compile(const char *entry, const char *target, ComPtr<ID3DBlob> &blob) {
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "d3d11fl12", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
  if (FAILED(hr) && errors) Print("%s\n", static_cast<const char *>(errors->GetBufferPointer()));
  return Hr(hr, entry);
}

// Reads a whole 256x128 RGBA8 texture through a staging copy.
bool ReadTexture(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, std::vector<uint32_t> &out) {
  D3D11_TEXTURE2D_DESC d{};
  src->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.MiscFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (!Hr(dev->CreateTexture2D(&d, nullptr, &staging), "staging texture")) return false;
  ctx->CopyResource(staging.Get(), src);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (!Hr(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "map staging texture")) return false;
  out.resize(kWidth * kHeight);
  for (UINT y = 0; y < kHeight; ++y)
    std::memcpy(&out[y * kWidth], static_cast<const uint8_t *>(m.pData) + y * m.RowPitch, kWidth * 4);
  ctx->Unmap(staging.Get(), 0);
  return true;
}

// Compares the image against expectations per tile column: 0 unmapped (zero), 1 gradient, 2 pattern.
bool Matches(const std::vector<uint32_t> &image, int left, int right, const char *what) {
  UINT bad = 0, firstX = 0, firstY = 0;
  uint32_t got = 0, want = 0;
  for (UINT y = 0; y < kHeight; ++y)
    for (UINT x = 0; x < kWidth; ++x) {
      const int kind = x < kTile ? left : right;
      const uint32_t e = kind == 0 ? 0u : kind == 1 ? Gradient(x, y) : Pattern(x - kTile, y);
      if (image[y * kWidth + x] != e && !bad++) {
        firstX = x, firstY = y, got = image[y * kWidth + x], want = e;
      }
    }
  if (bad)
    Print("     %u texels differ, first (%u,%u) got %08X want %08X\n", bad, firstX, firstY, got, want);
  Check(!bad, what);
  return !bad;
}

int Run(const Options &o) {
  ComPtr<IDXGIFactory1> factory;
  if (!Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return 1;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (o.vendor && (d.VendorId != o.vendor || (o.device && d.DeviceId != o.device))) continue;
    Print("adapter %04X:%04X %ls\n", d.VendorId, d.DeviceId, d.Description);
    adapter = a;
    break;
  }
  if (!adapter) {
    Check(false, "adapter found");
    return 1;
  }

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL level{};
  if (!Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels),
                            D3D11_SDK_VERSION, &dev, &level, &ctx),
          "D3D11CreateDevice"))
    return 1;
  Print("FEATURE_LEVEL 0x%X\n", static_cast<unsigned>(level));
  Check(level >= o.minimum, "device feature level at or above --min");

  D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1{};
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
  D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
  if (Hr(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof(o1)), "OPTIONS1"))
    Print("OPTIONS1 TiledResourcesTier=%d MinMaxFiltering=%d ClearViewDepthOnly=%d MapOnDefaultBuffers=%d\n",
                o1.TiledResourcesTier, o1.MinMaxFiltering, o1.ClearViewAlsoSupportsDepthOnlyFormats,
                o1.MapOnDefaultBuffers);
  if (Hr(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2)), "OPTIONS2"))
    Print("OPTIONS2 PSStencilRef=%d TypedUAVLoadAdditionalFormats=%d ROVs=%d ConservativeTier=%d "
                "TiledResourcesTier=%d MapOnDefaultTextures=%d StandardSwizzle=%d UMA=%d\n",
                o2.PSSpecifiedStencilRefSupported, o2.TypedUAVLoadAdditionalFormats, o2.ROVsSupported,
                o2.ConservativeRasterizationTier, o2.TiledResourcesTier, o2.MapOnDefaultTextures,
                o2.StandardSwizzle, o2.UnifiedMemoryArchitecture);
  if (Hr(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3)), "OPTIONS3"))
    Print("OPTIONS3 VPAndRTArrayIndexFromAnyShader=%d\n", o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
  // OPTIONS1 predates tier 3: the runtime reports tier 3 there as tier 2 (measured on a reference GPU).
  Check(o2.TiledResourcesTier >= D3D11_TILED_RESOURCES_TIER_2 &&
            o1.TiledResourcesTier == (o2.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2 ? o2.TiledResourcesTier
                                                                                            : D3D11_TILED_RESOURCES_TIER_2),
        "tiled resources tier 2 or higher, OPTIONS1 consistent with OPTIONS2");
  if (level >= D3D_FEATURE_LEVEL_12_0)
    Check(o2.TypedUAVLoadAdditionalFormats, "FL12_0: typed UAV additional formats");
  if (level >= D3D_FEATURE_LEVEL_12_1)
    Check(o2.ROVsSupported && o2.ConservativeRasterizationTier >= D3D11_CONSERVATIVE_RASTERIZATION_TIER_1,
          "FL12_1: ROVs and conservative rasterization");
  if (o2.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2) return 1;

  ComPtr<ID3D11Device2> dev2;
  ComPtr<ID3D11DeviceContext2> ctx2;
  if (!Hr(dev.As(&dev2), "ID3D11Device2") || !Hr(ctx.As(&ctx2), "ID3D11DeviceContext2")) return 1;

  // The pool: two tiles now, grown to three at the end.
  D3D11_BUFFER_DESC pd{};
  pd.ByteWidth = 2 * kTileBytes;
  pd.Usage = D3D11_USAGE_DEFAULT;
  pd.MiscFlags = D3D11_RESOURCE_MISC_TILE_POOL;
  ComPtr<ID3D11Buffer> pool;
  if (!Hr(dev->CreateBuffer(&pd, nullptr, &pool), "tile pool")) return 1;

  D3D11_TEXTURE2D_DESC td{};
  td.Width = kWidth;
  td.Height = kHeight;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_TILED;
  ComPtr<ID3D11Texture2D> tiled, alias;
  if (!Hr(dev->CreateTexture2D(&td, nullptr, &tiled), "tiled texture") ||
      !Hr(dev->CreateTexture2D(&td, nullptr, &alias), "second tiled texture"))
    return 1;

  UINT tiles = 0;
  D3D11_PACKED_MIP_DESC packed{};
  D3D11_TILE_SHAPE shape{};
  UINT subresources = 1;
  D3D11_SUBRESOURCE_TILING tiling{};
  dev2->GetResourceTiling(tiled.Get(), &tiles, &packed, &shape, &subresources, 0, &tiling);
  Print("tiling tiles=%u shape=%ux%ux%u packed=%u subresource0=%ux%ux%u\n", tiles, shape.WidthInTexels,
              shape.HeightInTexels, shape.DepthInTexels, packed.NumPackedMips, tiling.WidthInTiles,
              tiling.HeightInTiles, tiling.DepthInTiles);
  Check(tiles == 2 && shape.WidthInTexels == kTile && shape.HeightInTexels == kTile && packed.NumPackedMips == 0 &&
            tiling.WidthInTiles == 2 && tiling.HeightInTiles == 1,
        "GetResourceTiling: two 128x128 tiles, no packed mips");

  // Step 1: tile 0 -> pool tile 1, tile 1 stays unmapped.
  D3D11_TILED_RESOURCE_COORDINATE coord{};
  D3D11_TILE_REGION_SIZE one{};
  one.NumTiles = 1;
  UINT rangeFlags = 0, poolStart = 1, rangeCount = 1;
  Hr(ctx2->UpdateTileMappings(tiled.Get(), 1, &coord, &one, pool.Get(), 1, &rangeFlags, &poolStart, &rangeCount, 0),
     "UpdateTileMappings tile 0");

  // Step 2: draw the gradient.
  ComPtr<ID3DBlob> vsb, psb;
  if (!Compile("vs", "vs_5_0", vsb) || !Compile("ps", "ps_5_0", psb)) return 1;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11RenderTargetView> rtv;
  if (!Hr(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs), "vertex shader") ||
      !Hr(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps), "pixel shader") ||
      !Hr(dev->CreateRenderTargetView(tiled.Get(), nullptr, &rtv), "tiled render target view"))
    return 1;
  const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
  ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(), nullptr, 0);
  ctx->PSSetShader(ps.Get(), nullptr, 0);
  ctx->Draw(3, 0);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ctx2->TiledResourceBarrier(tiled.Get(), tiled.Get());
  std::vector<uint32_t> image;
  if (ReadTexture(dev.Get(), ctx.Get(), tiled.Get(), image)) Matches(image, 1, 0, "draw into mapped tile, unmapped tile reads zero");

  // Step 3: tile 1 -> pool tile 0, UpdateTiles writes the pattern; CopyTiles copies tile 0 into a buffer.
  coord.X = 1;
  poolStart = 0;
  Hr(ctx2->UpdateTileMappings(tiled.Get(), 1, &coord, &one, pool.Get(), 1, &rangeFlags, &poolStart, &rangeCount, 0),
     "UpdateTileMappings tile 1");
  std::vector<uint32_t> pattern(kTile * kTile);
  for (UINT y = 0; y < kTile; ++y)
    for (UINT x = 0; x < kTile; ++x) pattern[y * kTile + x] = Pattern(x, y);
  ctx2->UpdateTiles(tiled.Get(), &coord, &one, pattern.data(), 0);
  if (ReadTexture(dev.Get(), ctx.Get(), tiled.Get(), image)) Matches(image, 1, 2, "UpdateTiles into newly mapped tile 1");

  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = kTileBytes;
  bd.Usage = D3D11_USAGE_DEFAULT;
  ComPtr<ID3D11Buffer> linear, linearStaging;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  Hr(dev->CreateBuffer(&bd, nullptr, &linear), "CopyTiles buffer");
  bd.Usage = D3D11_USAGE_STAGING;
  bd.BindFlags = 0;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Hr(dev->CreateBuffer(&bd, nullptr, &linearStaging), "CopyTiles staging buffer");
  if (linear && linearStaging) {
    coord.X = 0;
    ctx2->CopyTiles(tiled.Get(), &coord, &one, linear.Get(), 0,
                    D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    ctx->CopyResource(linearStaging.Get(), linear.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (Hr(ctx->Map(linearStaging.Get(), 0, D3D11_MAP_READ, 0, &m), "map CopyTiles staging")) {
      const auto *texels = static_cast<const uint32_t *>(m.pData);
      UINT bad = 0;
      for (UINT y = 0; y < kTile; ++y)
        for (UINT x = 0; x < kTile; ++x) bad += texels[y * kTile + x] != Gradient(x, y);
      ctx->Unmap(linearStaging.Get(), 0);
      if (bad) Print("     %u texels differ\n", bad);
      Check(!bad, "CopyTiles tile 0 to a linear buffer");
    }
  }

  // Step 4: the second texture takes both mappings and reads the same bytes; the pool grows.
  D3D11_TILE_REGION_SIZE both{};
  both.NumTiles = 2;
  D3D11_TILED_RESOURCE_COORDINATE origin{};
  Hr(ctx2->CopyTileMappings(alias.Get(), &origin, tiled.Get(), &origin, &both, 0), "CopyTileMappings");
  if (ReadTexture(dev.Get(), ctx.Get(), alias.Get(), image)) Matches(image, 1, 2, "CopyTileMappings alias reads the same tiles");
  Hr(ctx2->ResizeTilePool(pool.Get(), 3 * kTileBytes), "ResizeTilePool to three tiles");
  if (ReadTexture(dev.Get(), ctx.Get(), tiled.Get(), image)) Matches(image, 1, 2, "contents kept across ResizeTilePool");

  const HRESULT removed = dev->GetDeviceRemovedReason();
  Check(removed == S_OK, "device not removed");
  return g_failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Options o;
  if (!ParseArgs(argc, argv, o)) {
    Usage();
    return 2;
  }
  if (!o.out.empty() && (_wfopen_s(&g_out, o.out.c_str(), L"wx") || !g_out)) {
    std::printf("FAIL cannot create %ls\n", o.out.c_str());
    return 2;
  }
  // The deadline ends a hung run; the lab runner has its own outer bound as well.
  static UINT deadline = o.deadline;
  CreateThread(nullptr, 0, [](void *) -> DWORD {
    Sleep(deadline * 1000);
    Print("FAIL deadline of %u s reached\n", deadline);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
  }, nullptr, 0, nullptr);
  const int code = Run(o);
  Print("%s: %u failure(s)\n", code ? "FAILED" : "PASSED", g_failures);
  return code;
}
