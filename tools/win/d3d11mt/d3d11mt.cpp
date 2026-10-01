// d3d11mt - a multithreaded D3D11 client with an exact, deterministic result, for the system UMD path (owner
// 2026-09-29: multithreading is tested with our own clients before a game is trusted to exercise it). Offscreen only,
// so it runs in session 0 as well as on the desktop. README.md has the phases, the output and the exit codes.

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kFormatVersion = 1;
constexpr double kMaxDeadlineSeconds = 170.0;  // lab trials stay under three minutes
constexpr UINT kColumns = 8;                    // tiles per image row
constexpr UINT kTexel = 4;                      // per-tile source textures are 4x4

enum Exit : int { kPass = 0, kApiFailure = 1, kBadArguments = 2, kDeadline = 3, kDeviceRemoved = 4, kMismatch = 5 };

struct Options {
  bool help = false;
  UINT threads = 4, tiles = 8, tile = 32, rounds = 48, immediateRounds = 16;
  bool creator = true, warp = false;
  double deadlineSeconds = 120.0;
  std::wstring out;
  std::string expect;
};

// ---------------------------------------------------------------------------------------------------------
// Small helpers

std::string Utf8(const std::wstring &w) {
  if (w.empty())
    return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string Quote(const std::string &s) {
  std::string q = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      q += '\\';
      q += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char e[8];
      sprintf_s(e, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
      q += e;
    } else {
      q += c;
    }
  }
  return q + "\"";
}

std::string Hex64(uint64_t v) {
  char b[32];
  sprintf_s(b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

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

// lowbias32 (Chris Wellons): the per-tile source texels, the same on every implementation.
uint32_t Hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

uint32_t Texel(uint32_t phase, uint32_t round, uint32_t tile, uint32_t n) {
  return Hash32(phase * 0x9E3779B9U ^ Hash32(round * 0x85EBCA6BU ^ Hash32(tile * 0xC2B2AE35U ^ n)));
}

struct Fnv {
  uint64_t h = 0xcbf29ce484222325ULL;
  void Add(const void *data, size_t bytes) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < bytes; ++i) {
      h ^= p[i];
      h *= 0x100000001b3ULL;
    }
  }
};

// ---------------------------------------------------------------------------------------------------------
// Failure reporting shared by every thread: the first failure wins and every loop stops.

std::mutex g_errorLock;
std::string g_error;
std::atomic<bool> g_failed{false};

void Fail(const char *what, HRESULT hr) {
  std::lock_guard<std::mutex> lock(g_errorLock);
  if (!g_failed.exchange(true)) {
    char b[256];
    sprintf_s(b, "%s failed hr=%08lx thread=%lu", what, static_cast<unsigned long>(hr), GetCurrentThreadId());
    g_error = b;
  }
}

bool Ok(HRESULT hr, const char *what) {
  if (SUCCEEDED(hr))
    return true;
  Fail(what, hr);
  return false;
}

// ---------------------------------------------------------------------------------------------------------
// Shaders: integer arithmetic only, so every conforming implementation writes the same bits.

const char kShaders[] = R"(
cbuffer C : register(b0) { uint4 p; };
Texture2D<uint> S : register(t0);
Texture2D<uint> F : register(t1);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// Phase 1 (deferred contexts): p = round, tile, tile origin; F = the previous round's image.
uint ps1(float4 pos : SV_Position) : SV_Target {
  uint2 xy = uint2(pos.xy);
  uint2 l = xy - p.zw;
  uint t = S.Load(int3(l & 3u, 0));
  uint prev = F.Load(int3(xy, 0));
  return (prev * 16777619u) ^ (t + p.x * 2654435769u + l.x * 73856093u + l.y * 19349663u + p.y * 83492791u);
}
// Phase 2 (immediate context from every thread): p = sub-round, tile, tile origin; F = phase 1's final image.
uint ps2(float4 pos : SV_Position) : SV_Target {
  uint2 xy = uint2(pos.xy);
  uint2 l = xy - p.zw;
  return F.Load(int3(xy, 0)) ^ (S.Load(int3(l & 3u, 0)) * 2246822519u + p.x * 3266489917u + p.y);
}
)";

bool Compile(const char *entry, const char *target, ComPtr<ID3DBlob> &blob) {
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "d3d11mt", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
  if (FAILED(hr)) {
    if (errors)
      fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
    Fail("D3DCompile", hr);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Run state

struct Tile {
  UINT id, x, y;  // image tile id and pixel origin
};

struct Gpu {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps1, ps2;
  ComPtr<ID3DBlob> ps1Code;
  ComPtr<ID3D11Texture2D> image[2], target2, mosaic;
  ComPtr<ID3D11RenderTargetView> imageRtv[2], target2Rtv;
  ComPtr<ID3D11ShaderResourceView> imageSrv[2];
  UINT width = 0, height = 0;
};

struct Worker {
  ComPtr<ID3D11DeviceContext> deferred;
  ComPtr<ID3D11CommandList> list;
  ComPtr<ID3D11Buffer> cb;        // dynamic, mapped with WRITE_DISCARD on the deferred and the immediate context
  ComPtr<ID3D11Texture2D> upload;  // phase 2: UpdateSubresource source of the tile's texels
  ComPtr<ID3D11ShaderResourceView> uploadSrv;
  std::vector<Tile> tiles;
  double recordMs = 0;
};

// A reusable barrier for the round structure: the main thread opens a round, the workers report back.
struct Rounds {
  std::mutex lock;
  std::condition_variable cv;
  int generation = -1;  // round the workers may record
  UINT done = 0;
  bool quit = false;
};

D3D11_VIEWPORT Viewport(const Tile &t, UINT size) {
  D3D11_VIEWPORT v = {};
  v.TopLeftX = float(t.x);
  v.TopLeftY = float(t.y);
  v.Width = v.Height = float(size);
  v.MaxDepth = 1.0f;
  return v;
}

bool MapConstants(ID3D11DeviceContext *ctx, ID3D11Buffer *cb, uint32_t a, const Tile &t) {
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (!Ok(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map constants"))
    return false;
  const uint32_t v[4] = {a, t.id, t.x, t.y};
  memcpy(m.pData, v, sizeof(v));
  ctx->Unmap(cb, 0);
  return true;
}

// Phase 1, one worker's round: a fresh immutable source texture per tile (concurrent creates), its constants by
// WRITE_DISCARD on the deferred context, one draw per tile, then a command list for the main thread.
void RecordRound(const Gpu &g, Worker &w, UINT round, UINT size) {
  const double start = NowMs();
  ID3D11DeviceContext *ctx = w.deferred.Get();
  for (const Tile &t : w.tiles) {
    if (g_failed)
      return;
    uint32_t texels[kTexel * kTexel];
    for (UINT n = 0; n < kTexel * kTexel; ++n)
      texels[n] = Texel(1, round, t.id, n);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = kTexel;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_UINT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init = {texels, kTexel * 4, 0};
    ComPtr<ID3D11Texture2D> source;
    ComPtr<ID3D11ShaderResourceView> sourceSrv;
    if (!Ok(g.device->CreateTexture2D(&d, &init, &source), "CreateTexture2D source") ||
        !Ok(g.device->CreateShaderResourceView(source.Get(), nullptr, &sourceSrv), "CreateShaderResourceView source") ||
        !MapConstants(ctx, w.cb.Get(), round, t))
      return;
    ID3D11RenderTargetView *rtv = g.imageRtv[round & 1].Get();
    ID3D11ShaderResourceView *srvs[2] = {sourceSrv.Get(), g.imageSrv[(round + 1) & 1].Get()};
    ID3D11Buffer *cb = w.cb.Get();
    const D3D11_VIEWPORT v = Viewport(t, size);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->RSSetViewports(1, &v);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g.ps1.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    // The command list keeps the source alive; this thread's references go now (deferred destruction).
  }
  Ok(ctx->FinishCommandList(FALSE, &w.list), "FinishCommandList");
  w.recordMs += NowMs() - start;
}

// Phase 2, one worker: every call on the immediate context, serialized by the application's lock as D3D11 requires,
// so the driver sees one context driven from several OS threads in turn.
void ImmediateRounds(const Gpu &g, Worker &w, UINT rounds, UINT size, std::mutex &immediateLock, UINT finalImage) {
  for (UINT j = 0; j < rounds && !g_failed; ++j) {
    for (const Tile &t : w.tiles) {
      uint32_t texels[kTexel * kTexel];
      for (UINT n = 0; n < kTexel * kTexel; ++n)
        texels[n] = Texel(2, j, t.id, n);
      std::lock_guard<std::mutex> lock(immediateLock);
      ID3D11DeviceContext *ctx = g.immediate.Get();
      ctx->UpdateSubresource(w.upload.Get(), 0, nullptr, texels, kTexel * 4, 0);
      const UINT tileX = t.id % kColumns, tileY = t.id / kColumns;
      ctx->CopySubresourceRegion(g.mosaic.Get(), 0, tileX * kTexel, tileY * kTexel, 0, w.upload.Get(), 0, nullptr);
      if (!MapConstants(ctx, w.cb.Get(), j, t))
        return;
      ID3D11RenderTargetView *rtv = g.target2Rtv.Get();
      ID3D11ShaderResourceView *srvs[2] = {w.uploadSrv.Get(), g.imageSrv[finalImage].Get()};
      ID3D11Buffer *cb = w.cb.Get();
      const D3D11_VIEWPORT v = Viewport(t, size);
      ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->PSSetShaderResources(0, 2, srvs);
      ctx->PSSetConstantBuffers(0, 1, &cb);
      ctx->RSSetViewports(1, &v);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(g.vs.Get(), nullptr, 0);
      ctx->PSSetShader(g.ps2.Get(), nullptr, 0);
      ctx->Draw(3, 0);
      ID3D11ShaderResourceView *none[2] = {};
      ctx->PSSetShaderResources(0, 2, none);
      ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
  }
}

// Concurrent creates while phase 1 records and executes: textures and buffers with initial data, views and pixel
// shaders, created and released at once.
void Creator(const Gpu &g, std::atomic<bool> &stop, std::atomic<uint64_t> &iterations) {
  std::vector<uint32_t> pixels(64 * 64);
  for (UINT i = 0; i < pixels.size(); ++i)
    pixels[i] = Hash32(i);
  while (!stop && !g_failed) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = 64;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init = {pixels.data(), 64 * 4, 0};
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> v;
    ComPtr<ID3D11Buffer> b;
    ComPtr<ID3D11PixelShader> s;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 4096;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA binit = {pixels.data(), 0, 0};
    if (!Ok(g.device->CreateTexture2D(&d, &init, &t), "creator CreateTexture2D") ||
        !Ok(g.device->CreateShaderResourceView(t.Get(), nullptr, &v), "creator CreateShaderResourceView") ||
        !Ok(g.device->CreateBuffer(&bd, &binit, &b), "creator CreateBuffer") ||
        !Ok(g.device->CreatePixelShader(g.ps1Code->GetBufferPointer(), g.ps1Code->GetBufferSize(), nullptr, &s),
            "creator CreatePixelShader"))
      return;
    ++iterations;
  }
}

bool ImageTarget(ID3D11Device *device, UINT width, UINT height, bool srv, ComPtr<ID3D11Texture2D> &tex,
                 ComPtr<ID3D11RenderTargetView> &rtv, ComPtr<ID3D11ShaderResourceView> *view) {
  std::vector<uint32_t> zeros(size_t(width) * height, 0);
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = width;
  d.Height = height;
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R32_UINT;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
  const D3D11_SUBRESOURCE_DATA init = {zeros.data(), width * 4, 0};
  return Ok(device->CreateTexture2D(&d, &init, &tex), "CreateTexture2D image") &&
         Ok(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv), "CreateRenderTargetView") &&
         (!srv || Ok(device->CreateShaderResourceView(tex.Get(), nullptr, view->GetAddressOf()), "CreateShaderResourceView image"));
}

// FNV-1a 64 of an R32_UINT texture's rows (width * 4 bytes each, top row first), folded into run as well.
bool Checksum(const Gpu &g, ID3D11Texture2D *tex, uint64_t &out, Fnv &run) {
  D3D11_TEXTURE2D_DESC d = {};
  tex->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (!Ok(g.device->CreateTexture2D(&d, nullptr, &staging), "CreateTexture2D staging"))
    return false;
  g.immediate->CopyResource(staging.Get(), tex);
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (!Ok(g.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map staging"))
    return false;
  Fnv f;
  for (UINT y = 0; y < d.Height; ++y) {
    const auto *row = static_cast<const unsigned char *>(m.pData) + size_t(y) * m.RowPitch;
    f.Add(row, size_t(d.Width) * 4);
    run.Add(row, size_t(d.Width) * 4);
  }
  g.immediate->Unmap(staging.Get(), 0);
  out = f.h;
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Identity of the run: which d3d11.dll and which driver modules the process actually loaded (as d3d11bench).

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

// Graphics modules with path and SHA-256: the D3D runtime, DXGI, the project's bc250* and amdgpu_wddm_* modules,
// anything from the driver store, every Vulkan driver (a module exporting vk_icdGetInstanceProcAddr).
std::string ModulesJson(std::string &d3d11Location) {
  const std::wstring exe = Lower(ModulePath(nullptr));
  const std::wstring exeDir = exe.substr(0, exe.find_last_of(L"\\/"));
  std::vector<HMODULE> modules(1024);
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed))
    return "[]";
  modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
  static const wchar_t *const names[] = {L"d3d11.dll", L"dxgi.dll", L"d3d10core.dll", L"d3d11on12.dll", L"vulkan-1.dll"};
  std::string json = "[";
  for (HMODULE m : modules) {
    const std::wstring path = ModulePath(m), lower = Lower(path);
    const std::wstring base = lower.substr(lower.find_last_of(L"\\/") + 1);
    const bool icd = GetProcAddress(m, "vk_icdGetInstanceProcAddr") != nullptr;
    bool wanted = icd || base.rfind(L"bc250", 0) == 0 || base.rfind(L"amdgpu_wddm_", 0) == 0 ||
                  lower.find(L"\\driverstore\\") != std::wstring::npos;
    for (const wchar_t *name : names)
      wanted = wanted || base == name;
    if (!wanted)
      continue;
    if (base == L"d3d11.dll")
      d3d11Location = lower.substr(0, lower.find_last_of(L"\\/")) == exeDir ? "app-local" : "system";
    if (json.size() > 1)
      json += ",";
    json += "{\"path\":" + Quote(Utf8(path)) + ",\"sha256\":" + Quote(FileSha256(path)) +
            ",\"vulkan_driver\":" + (icd ? "true" : "false") + "}";
  }
  return json + "]";
}

// ---------------------------------------------------------------------------------------------------------
// Output

std::string g_settings, g_adapter = "{}", g_featureLevel, g_threading = "{}";
std::mutex g_outputLock;
bool g_written = false;

void WriteResult(const Options &o, int exitCode, const char *result, const std::string &rest) {
  std::lock_guard<std::mutex> lock(g_outputLock);
  if (g_written)
    return;
  g_written = true;
  std::string d3d11Location = "unknown";
  const std::string modules = ModulesJson(d3d11Location);
  std::string error;
  {
    std::lock_guard<std::mutex> e(g_errorLock);
    error = g_error;
  }
  const std::string json = "{\"tool\":\"d3d11mt\",\"format\":" + std::to_string(kFormatVersion) + ",\"result\":" +
                           Quote(result) + ",\"exit\":" + std::to_string(exitCode) + ",\"settings\":" + g_settings +
                           ",\"adapter\":" + g_adapter + ",\"feature_level\":" + Quote(g_featureLevel) +
                           ",\"threading\":" + g_threading + rest + ",\"d3d11\":" + Quote(d3d11Location) +
                           ",\"modules\":" + modules + ",\"error\":" + Quote(error) + "}\n";
  if (o.out.empty()) {
    fputs(json.c_str(), stdout);
    fflush(stdout);
    return;
  }
  const std::wstring temp = o.out + L".partial";
  FILE *f = nullptr;
  if (_wfopen_s(&f, temp.c_str(), L"wb") == 0 && f) {
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    MoveFileExW(temp.c_str(), o.out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  } else {
    fputs(json.c_str(), stdout);
  }
  fflush(stdout);
}

const char *LevelName(D3D_FEATURE_LEVEL l) {
  switch (l) {
  case D3D_FEATURE_LEVEL_12_1: return "12_1";
  case D3D_FEATURE_LEVEL_12_0: return "12_0";
  case D3D_FEATURE_LEVEL_11_1: return "11_1";
  case D3D_FEATURE_LEVEL_11_0: return "11_0";
  case D3D_FEATURE_LEVEL_10_1: return "10_1";
  case D3D_FEATURE_LEVEL_10_0: return "10_0";
  default: return "other";
  }
}

bool ParseUint(const wchar_t *s, UINT lo, UINT hi, UINT &out) {
  wchar_t *end = nullptr;
  const unsigned long v = wcstoul(s, &end, 10);
  if (!*s || *end || v < lo || v > hi)
    return false;
  out = UINT(v);
  return true;
}

bool ParseArgs(int argc, wchar_t **argv, Options &o) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool more = i + 1 < argc;
    if (a == L"--help" || a == L"-h") {
      o.help = true;
    } else if (a == L"--no-creator") {
      o.creator = false;
    } else if (a == L"--threads" && more) {
      if (!ParseUint(argv[++i], 1, 16, o.threads)) return false;
    } else if (a == L"--tiles" && more) {
      if (!ParseUint(argv[++i], 1, 64, o.tiles)) return false;
    } else if (a == L"--tile" && more) {
      if (!ParseUint(argv[++i], 8, 256, o.tile)) return false;
    } else if (a == L"--rounds" && more) {
      if (!ParseUint(argv[++i], 1, 2000, o.rounds)) return false;
    } else if (a == L"--immediate-rounds" && more) {
      if (!ParseUint(argv[++i], 0, 2000, o.immediateRounds)) return false;
    } else if (a == L"--adapter" && more) {
      const std::wstring v = argv[++i];
      if (v == L"warp") o.warp = true;
      else if (v != L"default") return false;
    } else if (a == L"--deadline" && more) {
      o.deadlineSeconds = _wtof(argv[++i]);
      if (!(o.deadlineSeconds > 0.0 && o.deadlineSeconds <= kMaxDeadlineSeconds)) return false;
    } else if (a == L"--out" && more) {
      o.out = argv[++i];
    } else if (a == L"--expect" && more) {
      o.expect = Utf8(argv[++i]);
      if (o.expect.size() != 16 || o.expect.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    } else {
      return false;
    }
  }
  return true;
}

const char kUsage[] =
    "usage: d3d11mt [--threads N] [--tiles N] [--tile PX] [--rounds N] [--immediate-rounds N] [--no-creator]\n"
    "               [--adapter default|warp] [--deadline S] [--out FILE] [--expect CHECKSUM]\n"
    "  defaults: 4 threads, 8 tiles of 32x32 per thread, 48 deferred rounds, 16 immediate rounds, creator on,\n"
    "  deadline 120 s (at most 170). Offscreen. Exit 0 pass, 1 API failure, 2 bad arguments, 3 deadline,\n"
    "  4 device removed, 5 checksum differs from --expect.\n";

}  // namespace

int wmain(int argc, wchar_t **argv) {
  Options o;
  if (!ParseArgs(argc, argv, o)) {
    fputs(kUsage, stderr);
    return kBadArguments;
  }
  if (o.help) {
    fputs(kUsage, stdout);
    return kPass;
  }
  const double t0 = NowMs();
  char settings[512];
  sprintf_s(settings,
            "{\"threads\":%u,\"tiles_per_thread\":%u,\"tile\":%u,\"rounds\":%u,\"immediate_rounds\":%u,"
            "\"creator\":%s,\"adapter\":\"%s\",\"deadline_s\":%.1f}",
            o.threads, o.tiles, o.tile, o.rounds, o.immediateRounds, o.creator ? "true" : "false",
            o.warp ? "warp" : "default", o.deadlineSeconds);
  g_settings = settings;

  // Deadline: a watchdog ends the process with exit 3, whatever the GPU or a lock is doing.
  HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread watchdog([&] {
    if (WaitForSingleObject(finished, DWORD(o.deadlineSeconds * 1000.0)) == WAIT_TIMEOUT) {
      {
        std::lock_guard<std::mutex> e(g_errorLock);
        if (g_error.empty()) g_error = "deadline";
      }
      WriteResult(o, kDeadline, "deadline", "");
      fputs("FAIL deadline\n", stdout);
      fflush(stdout);
      TerminateProcess(GetCurrentProcess(), kDeadline);
    }
  });
  auto finish = [&](int code, const char *result, const std::string &rest) {
    WriteResult(o, code, result, rest);
    SetEvent(finished);
    watchdog.join();
    return code;
  };

  Gpu g;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL level = {};
  // Not D3D11_CREATE_DEVICE_SINGLETHREADED: the runtime's thread-safe device is the subject of the test.
  HRESULT hr = D3D11CreateDevice(nullptr, o.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                 UINT(std::size(levels)), D3D11_SDK_VERSION, &g.device, &level, &g.immediate);
  if (!Ok(hr, "D3D11CreateDevice"))
    return finish(kApiFailure, "failed", "");
  g_featureLevel = LevelName(level);
  {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC d = {};
    if (SUCCEEDED(g.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&d))) {
      char b[160];
      sprintf_s(b, "{\"vendor\":\"%04x\",\"device\":\"%04x\",\"luid\":\"%08lx%08lx\",\"description\":", d.VendorId,
                d.DeviceId, static_cast<unsigned long>(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart);
      g_adapter = std::string(b) + Quote(Utf8(d.Description)) + "}";
    }
    D3D11_FEATURE_DATA_THREADING t = {};
    if (SUCCEEDED(g.device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &t, sizeof(t))))
      g_threading = std::string("{\"driver_concurrent_creates\":") + (t.DriverConcurrentCreates ? "true" : "false") +
                    ",\"driver_command_lists\":" + (t.DriverCommandLists ? "true" : "false") + "}";
  }

  ComPtr<ID3DBlob> vsCode, ps2Code;
  if (!Compile("vs", "vs_4_0", vsCode) || !Compile("ps1", "ps_4_0", g.ps1Code) || !Compile("ps2", "ps_4_0", ps2Code) ||
      !Ok(g.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g.vs), "CreateVertexShader") ||
      !Ok(g.device->CreatePixelShader(g.ps1Code->GetBufferPointer(), g.ps1Code->GetBufferSize(), nullptr, &g.ps1), "CreatePixelShader ps1") ||
      !Ok(g.device->CreatePixelShader(ps2Code->GetBufferPointer(), ps2Code->GetBufferSize(), nullptr, &g.ps2), "CreatePixelShader ps2"))
    return finish(kApiFailure, "failed", "");

  const UINT total = o.threads * o.tiles;
  const UINT columns = std::min(total, kColumns), rows = (total + kColumns - 1) / kColumns;
  g.width = columns * o.tile;
  g.height = rows * o.tile;
  if (!ImageTarget(g.device.Get(), g.width, g.height, true, g.image[0], g.imageRtv[0], &g.imageSrv[0]) ||
      !ImageTarget(g.device.Get(), g.width, g.height, true, g.image[1], g.imageRtv[1], &g.imageSrv[1]) ||
      !ImageTarget(g.device.Get(), g.width, g.height, false, g.target2, g.target2Rtv, nullptr))
    return finish(kApiFailure, "failed", "");
  {
    std::vector<uint32_t> zeros(size_t(kColumns) * kTexel * rows * kTexel, 0);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kColumns * kTexel;
    d.Height = rows * kTexel;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_UINT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init = {zeros.data(), kColumns * kTexel * 4, 0};
    if (!Ok(g.device->CreateTexture2D(&d, &init, &g.mosaic), "CreateTexture2D mosaic"))
      return finish(kApiFailure, "failed", "");
  }

  std::vector<Worker> workers(o.threads);
  for (UINT t = 0; t < o.threads; ++t) {
    Worker &w = workers[t];
    for (UINT k = 0; k < o.tiles; ++k) {
      const UINT id = t * o.tiles + k;
      w.tiles.push_back({id, (id % kColumns) * o.tile, (id / kColumns) * o.tile});
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = kTexel;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_UINT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!Ok(g.device->CreateDeferredContext(0, &w.deferred), "CreateDeferredContext") ||
        !Ok(g.device->CreateBuffer(&bd, nullptr, &w.cb), "CreateBuffer constants") ||
        !Ok(g.device->CreateTexture2D(&d, nullptr, &w.upload), "CreateTexture2D upload") ||
        !Ok(g.device->CreateShaderResourceView(w.upload.Get(), nullptr, &w.uploadSrv), "CreateShaderResourceView upload"))
      return finish(kApiFailure, "failed", "");
  }

  // Phase 1: the workers record round r on their deferred contexts in parallel (and create their source textures
  // while doing so), the main thread executes the lists in worker order, then opens round r + 1.
  std::atomic<bool> stopCreator{false};
  std::atomic<uint64_t> creatorIterations{0};
  std::thread creator;
  if (o.creator)
    creator = std::thread(Creator, std::cref(g), std::ref(stopCreator), std::ref(creatorIterations));
  Rounds sync;
  std::vector<std::thread> threads;
  for (UINT t = 0; t < o.threads; ++t) {
    threads.emplace_back([&, t] {
      for (int seen = -1;;) {
        int round;
        {
          std::unique_lock<std::mutex> lock(sync.lock);
          sync.cv.wait(lock, [&] { return sync.quit || sync.generation != seen; });
          if (sync.quit)
            return;
          round = seen = sync.generation;
        }
        RecordRound(g, workers[t], UINT(round), o.tile);
        {
          std::lock_guard<std::mutex> lock(sync.lock);
          ++sync.done;
        }
        sync.cv.notify_all();
      }
    });
  }
  const double p1 = NowMs();
  double executeMs = 0;
  ComPtr<ID3D11Query> event;
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  Ok(g.device->CreateQuery(&qd, &event), "CreateQuery");
  for (UINT r = 0; r < o.rounds && !g_failed; ++r) {
    {
      std::lock_guard<std::mutex> lock(sync.lock);
      sync.done = 0;
      sync.generation = int(r);
    }
    sync.cv.notify_all();
    {
      std::unique_lock<std::mutex> lock(sync.lock);
      sync.cv.wait(lock, [&] { return sync.done == o.threads; });
    }
    if (g_failed)
      break;
    const double e = NowMs();
    for (Worker &w : workers) {
      g.immediate->ExecuteCommandList(w.list.Get(), FALSE);
      w.list.Reset();
    }
    g.immediate->Flush();
    // At most a few rounds in flight: wait for the GPU every eighth round.
    if ((r & 7) == 7 && event) {
      g.immediate->End(event.Get());
      while (g.immediate->GetData(event.Get(), nullptr, 0, 0) == S_FALSE && !g_failed)
        SwitchToThread();
    }
    executeMs += NowMs() - e;
  }
  {
    std::lock_guard<std::mutex> lock(sync.lock);
    sync.quit = true;
  }
  sync.cv.notify_all();
  for (std::thread &t : threads)
    t.join();
  threads.clear();
  stopCreator = true;
  if (creator.joinable())
    creator.join();
  const double phase1Ms = NowMs() - p1;

  // Phase 2: every worker drives the immediate context in turn under the application's lock.
  const UINT finalImage = (o.rounds - 1) & 1;
  std::mutex immediateLock;
  const double p2 = NowMs();
  if (!g_failed) {
    for (UINT t = 0; t < o.threads; ++t)
      threads.emplace_back([&, t] { ImmediateRounds(g, workers[t], o.immediateRounds, o.tile, immediateLock, finalImage); });
    for (std::thread &t : threads)
      t.join();
  }
  const double phase2Ms = NowMs() - p2;

  uint64_t sum1 = 0, sum2 = 0, sum3 = 0;
  Fnv run;
  const bool read = !g_failed && Checksum(g, g.image[finalImage].Get(), sum1, run) && Checksum(g, g.target2.Get(), sum2, run) &&
                    Checksum(g, g.mosaic.Get(), sum3, run);
  const HRESULT removed = g.device->GetDeviceRemovedReason();
  double recordMax = 0;
  for (const Worker &w : workers)
    recordMax = std::max(recordMax, w.recordMs);
  char rest[768];
  sprintf_s(rest,
            ",\"image\":{\"width\":%u,\"height\":%u},\"creator_iterations\":%llu,"
            "\"ms\":{\"phase1\":%.3f,\"phase1_execute\":%.3f,\"phase1_record_max_thread\":%.3f,\"phase2\":%.3f,\"total\":%.3f},"
            "\"device_removed_reason\":\"%08lx\",\"checksums\":{\"phase1_image\":\"%s\",\"phase2_target\":\"%s\","
            "\"phase2_mosaic\":\"%s\",\"run\":\"%s\"},\"expect\":%s",
            g.width, g.height, static_cast<unsigned long long>(creatorIterations.load()), phase1Ms, executeMs, recordMax,
            phase2Ms, NowMs() - t0, static_cast<unsigned long>(removed), Hex64(sum1).c_str(), Hex64(sum2).c_str(),
            Hex64(sum3).c_str(), Hex64(run.h).c_str(), o.expect.empty() ? "null" : Quote(o.expect).c_str());
  printf("D3D11MT threads=%u tiles=%u tile=%u rounds=%u immediate_rounds=%u fl=%s creator_iterations=%llu checksum=%s\n",
         o.threads, o.tiles, o.tile, o.rounds, o.immediateRounds, g_featureLevel.c_str(),
         static_cast<unsigned long long>(creatorIterations.load()), Hex64(run.h).c_str());
  if (removed != S_OK) {
    Fail("device removed", removed);
    puts("FAIL device removed");
    return finish(kDeviceRemoved, "device-removed", rest);
  }
  if (!read || g_failed) {
    printf("FAIL %s\n", g_error.c_str());
    return finish(kApiFailure, "failed", rest);
  }
  if (!o.expect.empty() && o.expect != Hex64(run.h)) {
    puts("FAIL checksum differs from --expect");
    return finish(kMismatch, "mismatch", rest);
  }
  puts("PASS");
  return finish(kPass, "pass", rest);
}
