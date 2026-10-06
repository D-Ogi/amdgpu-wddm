// wowprobe: what a process of this build's bitness gets from the Direct3D runtimes on each display adapter. Built for
// x86 it is the witness of the WoW64 route (UserModeDriverNameWow); built for x64 it is the control on the same
// machine. Per DXGI adapter it prints:
//   - the DXGI description (DedicatedVideoMemory is a SIZE_T: a 32-bit process sees it clamped below 4 GiB);
//   - the user-mode driver names dxgkrnl hands this process for D3D9, D3D10, D3D11 and D3D12
//     (D3DKMTQueryAdapterInfo KMTQAITYPE_UMDRIVERNAME), i.e. whether it reads UserModeDriverNameWow for us;
//   - D3D11CreateDevice on that adapter with the levels 12_1 down to 9_1: HRESULT and the level obtained, then one
//     clear of a 4x4 RGBA8 target read back through a staging copy and compared with the expected bytes.
// No window, no swap chain, nothing persistent. --adapter <substring> limits the run to matching adapters; --out <file>
// copies every line to a file. Exit 0 when every probed adapter whose description matched gave a device of at least
// --min-level (default 11_0) and an exact readback; 1 otherwise; 2 on usage errors.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>  // NTSTATUS; d3dkmthk.h needs it and does not include it
#include <d3dkmthk.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

FILE *g_out = nullptr;

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
  std::fflush(stdout);
}

const char *LevelName(D3D_FEATURE_LEVEL level) {
  switch (level) {
  case D3D_FEATURE_LEVEL_12_1: return "12_1";
  case D3D_FEATURE_LEVEL_12_0: return "12_0";
  case D3D_FEATURE_LEVEL_11_1: return "11_1";
  case D3D_FEATURE_LEVEL_11_0: return "11_0";
  case D3D_FEATURE_LEVEL_10_1: return "10_1";
  case D3D_FEATURE_LEVEL_10_0: return "10_0";
  case D3D_FEATURE_LEVEL_9_3: return "9_3";
  case D3D_FEATURE_LEVEL_9_2: return "9_2";
  case D3D_FEATURE_LEVEL_9_1: return "9_1";
  default: return "unknown";
  }
}

bool ParseLevel(const wchar_t *text, D3D_FEATURE_LEVEL *level) {
  static const struct { const wchar_t *name; D3D_FEATURE_LEVEL level; } names[] = {
      {L"12_1", D3D_FEATURE_LEVEL_12_1}, {L"12_0", D3D_FEATURE_LEVEL_12_0}, {L"11_1", D3D_FEATURE_LEVEL_11_1},
      {L"11_0", D3D_FEATURE_LEVEL_11_0}, {L"10_1", D3D_FEATURE_LEVEL_10_1}, {L"10_0", D3D_FEATURE_LEVEL_10_0},
      {L"9_3", D3D_FEATURE_LEVEL_9_3},   {L"9_2", D3D_FEATURE_LEVEL_9_2},   {L"9_1", D3D_FEATURE_LEVEL_9_1}};
  for (const auto &n : names)
    if (!wcscmp(text, n.name)) { *level = n.level; return true; }
  return false;
}

void PrintUmdNames(const LUID &luid) {
  D3DKMT_OPENADAPTERFROMLUID open = {};
  open.AdapterLuid = luid;
  NTSTATUS status = D3DKMTOpenAdapterFromLuid(&open);
  if (status != STATUS_SUCCESS) {
    Print("  umd: D3DKMTOpenAdapterFromLuid 0x%08lX\n", static_cast<unsigned long>(status));
    return;
  }
  static const struct { KMTUMDVERSION version; const char *name; } versions[] = {
      {KMTUMDVERSION_DX9, "dx9"}, {KMTUMDVERSION_DX10, "dx10"}, {KMTUMDVERSION_DX11, "dx11"}, {KMTUMDVERSION_DX12, "dx12"}};
  for (const auto &v : versions) {
    D3DKMT_UMDFILENAMEINFO info = {};
    info.Version = v.version;
    D3DKMT_QUERYADAPTERINFO query = {};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_UMDRIVERNAME;
    query.pPrivateDriverData = &info;
    query.PrivateDriverDataSize = sizeof(info);
    status = D3DKMTQueryAdapterInfo(&query);
    if (status == STATUS_SUCCESS)
      Print("  umd %s: \"%ls\"\n", v.name, info.UmdFileName);
    else
      Print("  umd %s: query 0x%08lX\n", v.name, static_cast<unsigned long>(status));
  }
  D3DKMT_CLOSEADAPTER close = {};
  close.hAdapter = open.hAdapter;
  D3DKMTCloseAdapter(&close);
}

// One clear of a 4x4 RGBA8 target, copied to a staging texture and compared byte for byte.
bool ClearReadback(ID3D11Device *device, ID3D11DeviceContext *context) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = desc.Height = 4;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> view;
  HRESULT hr = device->CreateTexture2D(&desc, nullptr, &target);
  if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(target.Get(), nullptr, &view);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(hr)) {
    Print("  readback: resource creation 0x%08lX\n", static_cast<unsigned long>(hr));
    return false;
  }
  // k/255 for each channel, so the float-to-UNORM conversion lands on the integers; the comparison still allows the
  // one unit of rounding the D3D11 conversion rules permit (an RTX 4090 returns 127 for a clear of 0.5).
  const uint8_t want[4] = {64, 128, 191, 255};
  const float color[4] = {want[0] / 255.0f, want[1] / 255.0f, want[2] / 255.0f, want[3] / 255.0f};
  context->ClearRenderTargetView(view.Get(), color);
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    Print("  readback: Map 0x%08lX\n", static_cast<unsigned long>(hr));
    return false;
  }
  unsigned bad = 0;
  uint8_t first[4] = {};
  for (UINT y = 0; y < 4; ++y) {
    const uint8_t *row = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch;
    for (UINT x = 0; x < 4; ++x) {
      if (!x && !y) std::memcpy(first, row, 4);
      for (UINT c = 0; c < 4; ++c)
        if (std::abs(int(row[4 * x + c]) - int(want[c])) > 1) { ++bad; break; }
    }
  }
  context->Unmap(staging.Get(), 0);
  Print("  readback: texel0 %u,%u,%u,%u, %u of 16 texels off by more than 1 from 64,128,191,255\n", first[0], first[1], first[2],
        first[3], bad);
  return bad == 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
  const wchar_t *filter = nullptr;
  D3D_FEATURE_LEVEL minLevel = D3D_FEATURE_LEVEL_11_0;
  for (int i = 1; i < argc; ++i) {
    if (!wcscmp(argv[i], L"--adapter") && i + 1 < argc) {
      filter = argv[++i];
    } else if (!wcscmp(argv[i], L"--min-level") && i + 1 < argc) {
      if (!ParseLevel(argv[++i], &minLevel)) { std::printf("unknown level %ls\n", argv[i]); return 2; }
    } else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) {
      if (_wfopen_s(&g_out, argv[++i], L"w")) { std::printf("cannot open %ls\n", argv[i]); return 2; }
    } else {
      std::printf("usage: wowprobe [--adapter <description substring>] [--min-level 11_0] [--out <file>]\n");
      return !wcscmp(argv[i], L"--help") ? 0 : 2;
    }
  }
  USHORT processMachine = 0, nativeMachine = 0;
  IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine);
  Print("wowprobe: pointer %u bytes, process machine 0x%04X (0 = native), native machine 0x%04X, pid %lu\n",
        static_cast<unsigned>(sizeof(void *)), processMachine, nativeMachine, GetCurrentProcessId());

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    Print("CreateDXGIFactory1 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }
  unsigned probed = 0, failed = 0;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index, adapter.Reset()) {
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    Print("adapter %u: \"%ls\" vendor 0x%04X device 0x%04X luid %08lX:%08lX flags 0x%X dedicated %llu shared %llu\n",
          index, desc.Description, desc.VendorId, desc.DeviceId, static_cast<unsigned long>(desc.AdapterLuid.HighPart),
          desc.AdapterLuid.LowPart, desc.Flags, static_cast<unsigned long long>(desc.DedicatedVideoMemory),
          static_cast<unsigned long long>(desc.SharedSystemMemory));
    const bool selected = !filter || wcsstr(desc.Description, filter);
    PrintUmdNames(desc.AdapterLuid);
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                               D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got = static_cast<D3D_FEATURE_LEVEL>(0);
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &device, &got, &context);
    Print("  d3d11: D3D11CreateDevice 0x%08lX level %s\n", static_cast<unsigned long>(hr),
          SUCCEEDED(hr) ? LevelName(got) : "none");
    bool ok = SUCCEEDED(hr) && got >= minLevel;
    if (SUCCEEDED(hr)) ok = ClearReadback(device.Get(), context.Get()) && ok;
    if (selected) {
      ++probed;
      if (!ok) ++failed;
      Print("  verdict: %s (minimum level %s)\n", ok ? "PASS" : "FAIL", LevelName(minLevel));
    }
  }
  Print("wowprobe: %u adapters selected, %u failed\n", probed, failed);
  if (g_out) std::fclose(g_out);
  return probed && !failed ? 0 : 1;
}
