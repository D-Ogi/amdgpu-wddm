// SPDX-License-Identifier: MIT
// A real D3D11 producer/import/sample oracle. No synthetic runtime/KMT blobs.
#include "capshare.h"
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <cwctype>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {
struct Failure { const char *call; HRESULT hr; };
std::string FileHash(const wchar_t *path) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE; std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "ERROR";
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            unsigned char buffer[65536], digest[32]; DWORD count = 0; bool ok = true;
            for (;;) {
                if (!ReadFile(file, buffer, sizeof buffer, &count, nullptr)) { ok = false; break; }
                if (!count) break;
                if (BCryptHashData(hash, buffer, count, 0) < 0) { ok = false; break; }
            }
            if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0) {
                const char digits[] = "0123456789abcdef";
                for (auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
            }
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result.empty() ? "ERROR" : result;
}
void Identity() {
    FILETIME created = {}, exited = {}, kernel = {}, user = {};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw Failure{"GetProcessTimes", HRESULT_FROM_WIN32(GetLastError())};
    Log("ALPHA identity pid=%lu creation_filetime=%llu", GetCurrentProcessId(),
        (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) throw Failure{"module-snapshot", HRESULT_FROM_WIN32(GetLastError())};
    MODULEENTRY32W module = {}; module.dwSize = sizeof module;
    bool valid = true;
    BOOL more = Module32FirstW(snap, &module);
    if (!more) { CloseHandle(snap); throw Failure{"module-first", E_FAIL}; }
    for (; more; more = Module32NextW(snap, &module)) {
        std::wstring name = module.szModule;
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (module.hModule != GetModuleHandleW(nullptr) && name.find(L"bc250") != 0 && name.find(L"amdgpu_") != 0 &&
            name.find(L"d3d11") != 0 && name.find(L"dxgi") != 0 && name.find(L"vulkan") != 0 &&
            name.find(L"d3dcompiler") != 0) continue;
        const auto sha = FileHash(module.szExePath);
        Log("ALPHA loaded_path=%s base=%p size=%lu disk_sha256=%s (not-memory-hash)",
            Narrow(module.szExePath).c_str(), module.modBaseAddr, module.modBaseSize, sha.c_str());
        if (sha == "ERROR") valid = false;
    }
    CloseHandle(snap);
    if (!valid) throw Failure{"module-disk-hash", E_FAIL};
}
void Require(HRESULT hr, const char *call) {
    Log("ALPHA call=%s hr=%s", call, HrText(hr).c_str());
    if (FAILED(hr)) throw Failure{call, hr};
}
struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
    void Init() {
        ComPtr<IDXGIAdapter1> adapter;
        LUID luid = {}; std::string error;
        SetStage("alpha-adapter");
        if (!PickAdapter(adapter, luid, error)) throw Failure{"PickAdapter", E_FAIL};
        g_opt.luid = luid; g_opt.haveLuid = true;
        SetStage("alpha-device");
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                           D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        Require(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &dev, &level, &ctx), "D3D11CreateDevice");
        LogModules("alpha-device");
        Identity();
        UINT support = 0;
        Require(dev->CheckFormatSupport(g_opt.format, &support), "CheckFormatSupport");
        Log("ALPHA format=%s support=%08x", FormatText(g_opt.format).c_str(), support);
        const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        if ((support & required) != required) throw Failure{"format-not-sampleable", DXGI_ERROR_UNSUPPORTED};
    }
    void Finish() {
        ComPtr<ID3D11Query> query;
        D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
        Require(dev->CreateQuery(&desc, &query), "CreateQuery(event)");
        ctx->End(query.Get()); ctx->Flush();
        HRESULT hr;
        while ((hr = ctx->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
            if (!Remaining(g_opt.deadline > 300 ? g_opt.deadline - 300 : g_opt.deadline))
                throw Failure{"GPU-event-deadline", HRESULT_FROM_WIN32(ERROR_TIMEOUT)};
            Sleep(1);
        }
        Require(hr, "GetData(event)");
        Require(dev->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
    }
};

// Producer and oracle deliberately use separate representations. Row phase
// changes every row; six exact UNORM edge values straddle the 127/128 boundary.
std::vector<unsigned char> Upload(UINT w, UINT h, UINT pitch, UINT phase, bool a8) {
    const unsigned char values[] = {0, 1, 127, 128, 254, 255};
    std::vector<unsigned char> bytes(size_t(pitch) * h, 0x5a);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        const auto a = values[(x + 5 * y + phase) % 6];
        unsigned char *p = bytes.data() + size_t(y) * pitch + x * (a8 ? 1 : 4);
        if (a8) *p = a;
        else { p[0] = 203; p[1] = static_cast<unsigned char>(y); p[2] = 17; p[3] = a; }
    }
    return bytes;
}
uint32_t Expected(UINT x, UINT y, UINT phase, bool a8) {
    UINT alpha = 0;
    switch ((x % 6 + (6 - y % 6) % 6 + phase) % 6) {
    case 0: alpha = 0; break; case 1: alpha = 1; break; case 2: alpha = 127; break;
    case 3: alpha = 128; break; case 4: alpha = 254; break; case 5: alpha = 255; break;
    }
    return a8 ? alpha | ((255 - alpha) << 8) | (alpha << 24)
              : 17u | ((y & 255u) << 8) | (203u << 16) | (alpha << 24);
}
ComPtr<ID3DBlob> Compile(const char *source, const char *entry, const char *profile) {
    ComPtr<ID3DBlob> code, error;
    HRESULT hr = D3DCompile(source, strlen(source), "alpha-sample", nullptr, nullptr, entry, profile,
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (error) Log("ALPHA shader=%.*s", int(error->GetBufferSize()), static_cast<const char *>(error->GetBufferPointer()));
    Require(hr, "D3DCompile"); return code;
}

Check Sample(Device &d, ID3D11Texture2D *opened, UINT phase) {
    SetStage("alpha-sample");
    D3D11_TEXTURE2D_DESC desc = {}; opened->GetDesc(&desc);
    Log("ALPHA opened %ux%u format=%u bind=%x usage=%u cpu=%x misc=%x", desc.Width, desc.Height,
        desc.Format, desc.BindFlags, desc.Usage, desc.CPUAccessFlags, desc.MiscFlags);
    if (desc.Width != g_opt.w || desc.Height != g_opt.h || desc.Format != g_opt.format ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
        throw Failure{"opened-description", E_UNEXPECTED};
    ComPtr<ID3D11ShaderResourceView> srv;
    Require(d.dev->CreateShaderResourceView(opened, nullptr, &srv), "CreateShaderResourceView(imported)");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.MiscFlags = 0; desc.CPUAccessFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    Require(d.dev->CreateTexture2D(&desc, nullptr, &target), "CreateTexture2D(RGBA-target)");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Require(d.dev->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D(readback)");
    ComPtr<ID3D11RenderTargetView> rtv;
    Require(d.dev->CreateRenderTargetView(target.Get(), nullptr, &rtv), "CreateRenderTargetView");
    const char *vsText = "float4 main(uint n:SV_VertexID):SV_Position { return float4(n==2?3:-1,n==1?3:-1,0,1); }";
    std::string psText = "Texture2D<float4> img:register(t0); SamplerState smp:register(s0);"
        "float4 main(float4 pos:SV_Position):SV_Target { uint w,h; img.GetDimensions(w,h);"
        "float4 v=img.SampleLevel(smp,pos.xy/float2(w,h),0);";
    if (g_opt.format == DXGI_FORMAT_A8_UNORM)
        psText += g_opt.alphaRed ? "return float4(v.r,1-v.r,v.r+v.g+v.b,v.r); }"
                                : "return float4(v.a,1-v.a,v.r+v.g+v.b,v.a); }";
    else psText += "return v; }";
    auto vsCode = Compile(vsText, "main", "vs_4_0");
    auto psCode = Compile(psText.c_str(), "main", "ps_4_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    Require(d.dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
    Require(d.dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "CreatePixelShader");
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    Require(d.dev->CreateSamplerState(&sd, &sampler), "CreateSamplerState");
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    Require(d.dev->CreateRasterizerState(&rd, &raster), "CreateRasterizerState");
    const float poison[] = {0.25f, 0.5f, 0.75f, 1}; d.ctx->ClearRenderTargetView(rtv.Get(), poison);
    D3D11_VIEWPORT vp = {0, 0, float(g_opt.w), float(g_opt.h), 0, 1};
    d.ctx->RSSetViewports(1, &vp); d.ctx->RSSetState(raster.Get());
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->VSSetShader(vs.Get(), nullptr, 0); d.ctx->PSSetShader(ps.Get(), nullptr, 0);
    d.ctx->PSSetSamplers(0, 1, sampler.GetAddressOf()); d.ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
    d.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr); d.ctx->Draw(3, 0);
    d.ctx->CopyResource(staging.Get(), target.Get()); d.Finish();
    SetStage("alpha-readback");
    D3D11_MAPPED_SUBRESOURCE map = {};
    Require(d.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map(RGBA-readback)");
    Check c; c.what = "sample" + std::to_string(phase); c.total = size_t(g_opt.w) * g_opt.h;
    for (UINT y = 0; y < g_opt.h; ++y) for (UINT x = 0; x < g_opt.w; ++x) {
        uint32_t got; memcpy(&got, static_cast<const char *>(map.pData) + size_t(y) * map.RowPitch + x * 4, 4);
        const uint32_t want = Expected(x, y, phase, g_opt.format == DXGI_FORMAT_A8_UNORM);
        if (got != want) {
            if (!c.diff) { c.x = int(x); c.y = int(y); c.got = got; c.want = want; }
            ++c.diff;
            for (UINT shift = 0; shift < 32; shift += 8)
                c.maxDelta = std::max(c.maxDelta, abs(int((got >> shift) & 255) - int((want >> shift) & 255)));
        }
    }
    Log("ALPHA readback-pitch=%u checked=%zu differences=%zu", map.RowPitch, c.total, c.diff);
    d.ctx->Unmap(staging.Get(), 0); d.ctx->ClearState(); d.Finish();
    c.pass = c.diff == 0; return c;
}
} // namespace

bool AlphaOracleSelfTest() {
    for (bool a8 : {false, true}) for (UINT phase = 0; phase < 3; ++phase) {
        const UINT w = 701, h = 19, pitch = ((w * (a8 ? 1u : 4u) + 255) & ~255u);
        const auto image = Upload(w, h, pitch, phase, a8);
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
            const auto *p = image.data() + size_t(y) * pitch + x * (a8 ? 1 : 4);
            const UINT alpha = p[a8 ? 0 : 3];
            const uint32_t simulated = a8 ? alpha | ((255-alpha) << 8) | (alpha << 24)
                : UINT(p[2]) | (UINT(p[1]) << 8) | (UINT(p[0]) << 16) | (alpha << 24);
            if (simulated != Expected(x, y, phase, a8)) return false;
        }
    }
    // This checks the oracle only. It is not a GPU or imported-image measurement.
    return Expected(1, 0, 0, true) != 0x0000ff00u && Expected(5, 0, 0, true) == 0xff0000ffu;
}

int RunAlphaSampleParent() {
    Device d;
    try {
        d.Init(); VerdictRoute('A', RouteTag(nullptr), FlText(d.level));
        D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
        HRESULT hr = d.dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options);
        Log("ALPHA ExtendedResourceSharing query=%s value=%u", HrText(hr).c_str(), options.ExtendedResourceSharing);
        if (g_opt.format == DXGI_FORMAT_A8_UNORM && (FAILED(hr) || !options.ExtendedResourceSharing)) {
            VerdictSkip("producer-contract", "A8-needs-ExtendedResourceSharing"); return 0;
        }
        std::string error, message;
        if (!SpawnPeer(error)) throw Failure{"SpawnPeer", E_FAIL};
        if (!PeerStep("HELLO", message, "alpha-peer-device")) return 0;
        VerdictRoute('B', Field(message, "route"), Field(message, "fl"));
        for (UINT phase = 0; phase < 3; ++phase) {
            SetStage("alpha-producer-create");
            const UINT bpp = g_opt.format == DXGI_FORMAT_A8_UNORM ? 1u : 4u;
            const UINT pitch = (g_opt.w * bpp + 255) & ~255u;
            const auto image = Upload(g_opt.w, g_opt.h, pitch, phase, bpp == 1);
            D3D11_TEXTURE2D_DESC td = {}; td.Width = g_opt.w; td.Height = g_opt.h;
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.Format = g_opt.format;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            D3D11_SUBRESOURCE_DATA init = {image.data(), pitch, 0};
            ComPtr<ID3D11Texture2D> texture;
            Require(d.dev->CreateTexture2D(&td, &init, &texture), "CreateTexture2D(shared-producer)");
            d.Finish();
            ComPtr<IDXGIResource> resource; Require(texture.As(&resource), "QueryInterface(IDXGIResource)");
            HANDLE handle = nullptr; Require(resource->GetSharedHandle(&handle), "GetSharedHandle");
            Log("ALPHA producer phase=%u size=%ux%u upload-pitch=%u handle=%p", phase, g_opt.w, g_opt.h, pitch, handle);
            g_ipc.Send("SAMPLE res=%llx phase=%u", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(handle)), phase);
            if (!PeerStep("SAMPLED", message, "alpha-peer-sample-close")) return 0;
            VerdictCheck('B', CheckFromText(message));
            // Consumer has completed GPU work, released the opened image and
            // acknowledged CLOSED before this producer releases its owner.
            if (!PeerStep("CLOSED", message, "alpha-peer-close")) return 0;
            if (VerdictFailed()) return 0;
        }
        VerdictNote("import_map", "not-measured-default-shared-textures-are-not-mappable");
    } catch (const Failure &f) {
        VerdictFail('A', Stage(), f.call, f.hr);
        if (d.dev) VerdictRemoved('A', d.dev->GetDeviceRemovedReason());
    }
    return 0;
}

int RunAlphaSamplePeer() {
    Device d;
    try {
        d.Init(); g_ipc.Send("HELLO route=%s fl=%s", RouteTag(nullptr).c_str(), FlText(d.level));
        for (UINT phase = 0; phase < 3; ++phase) {
            std::string message;
            if (!PeerExpect("SAMPLE", message)) return 2;
            if (Field(message, "phase") != std::to_string(phase)) throw Failure{"phase-order", E_UNEXPECTED};
            HANDLE handle = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(_strtoui64(Field(message, "res").c_str(), nullptr, 16)));
            SetStage("alpha-open-shared"); ComPtr<ID3D11Texture2D> opened;
            Require(d.dev->OpenSharedResource(handle, IID_PPV_ARGS(&opened)), "OpenSharedResource(A8/BGRA)");
            Check c = Sample(d, opened.Get(), phase);
            opened.Reset(); d.Finish();
            g_ipc.Send("SAMPLED %s", CheckText(c).c_str()); g_ipc.Send("CLOSED phase=%u", phase);
            if (!c.pass) break;
        }
        std::string message; return PeerExpect("DONE", message) ? 0 : 2;
    } catch (const Failure &f) {
        return PeerFailText(Stage(), f.call, f.hr, "-", d.dev ? d.dev->GetDeviceRemovedReason() : S_OK);
    }
}
