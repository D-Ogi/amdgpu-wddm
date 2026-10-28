// d3d11shaderedge: one HLSL corpus compiled by two compilers, both bytecodes run through D3D11 with a result
// readback. FXC is Microsoft's compiler (fxc.exe of the Windows SDK); FXC2 is Michael Moroz's fxc-compatible
// compiler on Wine's vkd3d-shader (https://github.com/MichaelMoroz/FXC2), which writes DXBC SM5 of a different
// structure: more instructions, switches as if chains, more indexable temps. The expected values come from the C
// functions below, never from a compiler. A failure of one bytecode places the fault between that bytecode and
// the driver; a failure of both points at the driver (DXVK and RADV inside our D3D11 UMD on the BC-250).
//
// Cases (README.md has the details): the compute entries of shaders/corpus_cs.hlsl and the tessellation draw of
// shaders/tess_cpid.hlsl, whose hull program indexes per-control-point data with SV_OutputControlPointID (FXC2
// patch 64). --with-no64 adds a third bytecode of the tessellation case, from FXC2 without patch 64: its hull
// program reads an indexable temp indexed by vOutputControlPointID itself, the code that made an NVIDIA card
// remove the device at the first draw. That case is off by default because it can do the same here.
//
// Exit: 0 every case passed, 1 a case failed (under --negative-control: every case failed, as required), 2 usage,
// 3 D3D11 or system error, a removed device or a broken negative control, 4 deadline.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "d3d11_corpus.h"

namespace {

constexpr uint32_t kThreads = 256;
constexpr uint32_t kWords = 8;
constexpr uint32_t kTarget = 64;
constexpr uint32_t kTessCheck = 51020003u;

LARGE_INTEGER qpcFreq;
HANDLE deadlineEvent;
uint32_t deadlineSec = 60;
uint32_t negative;

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(qpcFreq.QuadPart);
}

DWORD WINAPI Watchdog(LPVOID)
{
    if (WaitForSingleObject(deadlineEvent, deadlineSec * 1000u + 2000u) == WAIT_TIMEOUT) {
        std::fprintf(stderr, "d3d11shaderedge: deadline of %u s exceeded, ending the process\n", deadlineSec);
        std::fflush(stderr);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    return 0;
}

template <typename T> void Release(T *&p)
{
    if (p) p->Release();
    p = nullptr;
}

[[noreturn]] void Fatal(const char *what, HRESULT hr)
{
    std::fprintf(stderr, "d3d11shaderedge: %s failed: 0x%08lX\n", what, static_cast<unsigned long>(hr));
    std::exit(3);
}
#define HR(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) Fatal(#call, hr_); } while (0)

bool Sha256File(const wchar_t *path, char out[65])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    std::vector<unsigned char> buffer(65536);
    bool ok = false;
    out[0] = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        DWORD read = 0;
        ok = true;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) { ok = false; break; }
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) ok = false;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (ok)
        for (int i = 0; i < 32; i++) std::snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return ok;
}

// The route witness: the user-mode driver modules in the process after device creation. Our D3D11 UMD and its
// private ICD carry "amdgpu" in their names; for another vendor every module outside the Windows directory is
// printed, which names that vendor's UMD.
void PrintDriverModules()
{
    HMODULE modules[1024];
    DWORD needed = 0;
    wchar_t windows[MAX_PATH];
    const UINT windowsLength = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    if (needed > sizeof(modules)) needed = sizeof(modules);
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        wchar_t path[MAX_PATH * 2];
        char sha[65];
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH * 2)) continue;
        const bool ours = wcsstr(path, L"amdgpu") != nullptr;
        const bool system = windowsLength && _wcsnicmp(path, windows, windowsLength) == 0;
        if (modules[i] == GetModuleHandleW(nullptr)) continue;
        if (!ours && system && !wcsstr(path, L"DriverStore")) continue;
        std::printf("module        %ls sha256 %s\n", path, Sha256File(path, sha) ? sha : "unavailable");
    }
}

// ---- expected values: C copies of shaders/corpus_cs.hlsl ------------------------------------------------------

uint32_t AsUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void ExpectFu(uint32_t x, uint32_t *o)
{
    const float f = static_cast<float>(x & 255) * 0.5f + 10.25f;
    const uint32_t u = (x >> 8) & 1023;
    const uint32_t big = 0x80000000u + ((x & 0xFFFF) << 8);
    const float fb = static_cast<float>((x >> 16) & 255) * 256.0f + 512.0f;
    const int32_t s = static_cast<int32_t>(x & 511) - 256;
    o[0] = AsUint(f - static_cast<float>(u));
    o[1] = AsUint(f - static_cast<float>(u));
    o[2] = AsUint(f * 2.0f - static_cast<float>(u >> 1));
    o[3] = AsUint(static_cast<float>(u) - f);
    o[4] = AsUint(f - static_cast<float>(s));
    o[5] = AsUint(fb - static_cast<float>(big));
    o[6] = AsUint(static_cast<float>(big));
    o[7] = static_cast<uint32_t>(f - static_cast<float>(u) + 4096.0f);
}

void ExpectLoop(uint32_t x, uint32_t *o)
{
    uint32_t acc = 0;
    for (uint32_t k = 0; k < (x & 31) + 1; ++k) {
        if ((k & 3) == 1) continue;
        acc = (acc * 31 + k) ^ x;
        if (acc > 100000u && (x & 1) != 0) break;
    }
    uint32_t nested = 0;
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t b = 0; b < (x % 5); ++b) {
            if (a * b == 2) break;
            nested += a * 7 + b;
        }
    uint32_t w = x, steps = 0;
    while (w > 1 && steps < 64) {
        w = (w & 1) ? 3 * (w >> 1) + 2 : w >> 1;
        ++steps;
    }
    o[0] = acc;
    o[1] = nested;
    o[2] = steps;
    o[3] = w;
    o[4] = o[5] = o[6] = o[7] = 0;
}

uint32_t SwitchReturn(uint32_t sel, uint32_t x)
{
    switch (sel) {
    case 0: return x * 3;
    case 1:
    case 2: return x + 100;
    case 4: return x ^ 0x55u;
    case 6: return x > 128 ? 1u : 2u;
    default: return 0xDEADu;
    }
}

void ExpectSwitch(uint32_t x, uint32_t *o)
{
    const uint32_t sel = x % 9;
    uint32_t r;
    switch (sel) {
    case 0: r = x * 3; break;
    case 1:
    case 2: r = x + 100; break;
    case 3: r = x ^ 0x55u; break;
    case 5: r = x > 128 ? 1u : 2u; break;
    case 7: {
        uint32_t t = 0;
        for (uint32_t k = 0; k < (x & 7); ++k) t += k * k;
        r = t;
        break;
    }
    default: r = 0xDEADu; break;
    }
    o[0] = r;
    o[1] = SwitchReturn(sel, x);
    o[2] = SwitchReturn((x >> 4) % 8, x >> 8);
    o[3] = o[4] = o[5] = o[6] = o[7] = 0;
}

void ExpectIndexed(uint32_t x, uint32_t *o)
{
    uint32_t arr[16];
    for (uint32_t k = 0; k < 16; ++k) arr[k] = x * k + 1;
    const uint32_t j = x % 16;
    arr[j] += 1000;
    arr[(j * 7) % 16] ^= 0xF0F0u;
    uint32_t s = 0;
    for (uint32_t m = 0; m < 16; ++m) s = s * 3 + arr[(m * 5 + j) % 16];
    const uint32_t v[4][4] = { { 1, 2, 3, 4 }, { x, x >> 1, x >> 2, x >> 3 }, { 5, 6, 7, 8 }, { 9, 10, 11, x } };
    o[0] = s;
    o[1] = arr[x % 13];
    for (int c = 0; c < 4; ++c) o[2 + c] = v[x & 3][c];
    o[6] = arr[15 - j];
    o[7] = 0;
}

uint32_t FirstBitHigh(uint32_t x)
{
    if (!x) return 0xFFFFFFFFu;
    uint32_t b = 31;
    while (!(x >> b)) --b;
    return b;
}

uint32_t FirstBitLow(uint32_t x)
{
    if (!x) return 0xFFFFFFFFu;
    uint32_t b = 0;
    while (!((x >> b) & 1)) ++b;
    return b;
}

// firstbithigh of a signed value: the first bit from the top that differs from the sign bit; -1 for 0 and -1.
uint32_t FirstBitHighSigned(uint32_t x) { return FirstBitHigh((x & 0x80000000u) ? ~x : x); }

uint32_t ReverseBits(uint32_t x)
{
    uint32_t r = 0;
    for (int b = 0; b < 32; ++b) r |= ((x >> b) & 1u) << (31 - b);
    return r;
}

uint32_t CountBits(uint32_t x)
{
    uint32_t n = 0;
    for (; x; x &= x - 1) ++n;
    return n;
}

void ExpectBits(uint32_t x, uint32_t *o)
{
    o[0] = FirstBitHigh(x);
    o[1] = FirstBitLow(x);
    o[2] = FirstBitHighSigned(x);
    o[3] = CountBits(x) | (ReverseBits(x) & 0xFFFFFF00u);
    o[4] = (x >> 5) & 0x3Fu;
    o[5] = x / 7;
    o[6] = x % 13;
    o[7] = x / ((x & 15) + 1);
}

// Input words: the corner values first, then a hash, so that every branch of every entry is taken.
uint32_t Input(uint32_t i)
{
    static const uint32_t kCorners[] = { 0u, 1u, 2u, 3u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0xFFFFu,
                                         0x10000u, 129u, 128u, 255u, 256u, 0xFFFF00FFu, 0x00FF00FFu, 0xAAAAAAAAu };
    if (i < sizeof(kCorners) / sizeof(kCorners[0])) return kCorners[i];
    uint32_t h = i * 2654435761u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

// ---- cases -----------------------------------------------------------------------------------------------------

struct Blob {
    const BYTE *code;
    SIZE_T size;
};

enum class Kind { Compute, Tess };

struct Case {
    const char *name;
    const char *compiler;
    Kind kind;
    void (*expect)(uint32_t, uint32_t *);
    Blob cs, vs, hs, ds, ps;
    bool hostile;
};

#define B(x) Blob{ x, sizeof(x) }
#define NOBLOB Blob{ nullptr, 0 }
#define CS(name, comp, fn) Case{ #name, #comp, Kind::Compute, fn, B(g_##name##_##comp), NOBLOB, NOBLOB, NOBLOB, NOBLOB, false }
#define TESS(name, hsname, comp, hostile) Case{ #name, #comp, Kind::Tess, nullptr, NOBLOB, B(g_tess_vs_##comp), \
    B(g_##hsname##_##comp), B(g_tess_ds_##comp), B(g_tess_ps_##comp), hostile }

const Case kCases[] = {
    CS(fu, fxc, ExpectFu), CS(fu, fxc2, ExpectFu),
    CS(loop, fxc, ExpectLoop), CS(loop, fxc2, ExpectLoop),
    CS(switch, fxc, ExpectSwitch), CS(switch, fxc2, ExpectSwitch),
    CS(indexed, fxc, ExpectIndexed), CS(indexed, fxc2, ExpectIndexed),
    CS(bits, fxc, ExpectBits), CS(bits, fxc2, ExpectBits),
    TESS(tess-array, tess_hs_array, fxc, false), TESS(tess-array, tess_hs_array, fxc2, false),
    TESS(tess-direct, tess_hs_direct, fxc, false), TESS(tess-direct, tess_hs_direct, fxc2, false),
    TESS(tess-array, tess_hs_array, fxc2no64, true), TESS(tess-direct, tess_hs_direct, fxc2no64, true),
};

struct Device {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Buffer *control = nullptr;
};

bool Removed(Device &d, const char *where)
{
    const HRESULT reason = d.dev->GetDeviceRemovedReason();
    if (reason == S_OK) return false;
    std::printf("DEVICE REMOVED during %s: reason 0x%08lX\n", where, static_cast<unsigned long>(reason));
    return true;
}

ID3D11Buffer *MakeBuffer(Device &d, UINT size, UINT bind, D3D11_USAGE usage, UINT cpu, UINT misc, UINT stride,
                         const void *data)
{
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = size;
    bd.Usage = usage;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = cpu;
    bd.MiscFlags = misc;
    bd.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer *b = nullptr;
    HR(d.dev->CreateBuffer(&bd, data ? &init : nullptr, &b));
    return b;
}

enum Result { kPass, kFail, kRemoved };

// --debug: the messages of the D3D11 debug layer since the previous case, errors and warnings only.
void PrintDebugMessages(Device &d)
{
    ID3D11InfoQueue *q = nullptr;
    if (FAILED(d.dev->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void **>(&q)))) return;
    const UINT64 count = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        if (FAILED(q->GetMessage(i, nullptr, &size)) || !size) continue;
        std::vector<unsigned char> buffer(size);
        D3D11_MESSAGE *m = reinterpret_cast<D3D11_MESSAGE *>(buffer.data());
        if (SUCCEEDED(q->GetMessage(i, m, &size)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            std::printf("debug layer   %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
    q->ClearStoredMessages();
    q->Release();
}

Result RunCompute(Device &d, const Case &c, char *why, size_t whySize)
{
    std::vector<uint32_t> in(kThreads), clear(kThreads * kWords, 0xCDCDCDCDu), got(kThreads * kWords);
    for (uint32_t i = 0; i < kThreads; ++i) in[i] = Input(i);
    ID3D11ComputeShader *cs = nullptr;
    HRESULT hr = d.dev->CreateComputeShader(c.cs.code, c.cs.size, nullptr, &cs);
    if (FAILED(hr)) {
        std::snprintf(why, whySize, "CreateComputeShader returned 0x%08lX", static_cast<unsigned long>(hr));
        return Removed(d, "CreateComputeShader") ? kRemoved : kFail;
    }
    ID3D11Buffer *src = MakeBuffer(d, kThreads * 4, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0,
                                   D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, in.data());
    ID3D11Buffer *dst = MakeBuffer(d, kThreads * kWords * 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0,
                                   D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, clear.data());
    ID3D11Buffer *rb = MakeBuffer(d, kThreads * kWords * 4, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, 0, 0,
                                  nullptr);
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    HR(d.dev->CreateShaderResourceView(src, nullptr, &srv));
    HR(d.dev->CreateUnorderedAccessView(dst, nullptr, &uav));
    d.ctx->CSSetShader(cs, nullptr, 0);
    d.ctx->CSSetShaderResources(0, 1, &srv);
    d.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    d.ctx->CSSetConstantBuffers(0, 1, &d.control);
    d.ctx->Dispatch(kThreads / 64, 1, 1);
    ID3D11UnorderedAccessView *noUav = nullptr;
    ID3D11ShaderResourceView *noSrv = nullptr;
    d.ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    d.ctx->CSSetShaderResources(0, 1, &noSrv);
    d.ctx->CopyResource(rb, dst);
    D3D11_MAPPED_SUBRESOURCE map = {};
    hr = d.ctx->Map(rb, 0, D3D11_MAP_READ, 0, &map);
    Result result = kPass;
    if (FAILED(hr)) {
        std::snprintf(why, whySize, "Map of the readback returned 0x%08lX", static_cast<unsigned long>(hr));
        result = Removed(d, "Map") ? kRemoved : kFail;
    } else {
        std::memcpy(got.data(), map.pData, got.size() * 4);
        d.ctx->Unmap(rb, 0);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < kThreads; ++i) {
            uint32_t want[kWords];
            c.expect(in[i], want);
            for (uint32_t k = 0; k < kWords; ++k)
                if (got[i * kWords + k] != want[k]) {
                    if (!bad)
                        std::snprintf(why, whySize, "thread %u (input 0x%08X) word %u: got 0x%08X want 0x%08X", i,
                                      in[i], k, got[i * kWords + k], want[k]);
                    ++bad;
                }
        }
        if (bad) {
            const size_t len = std::strlen(why);
            std::snprintf(why + len, whySize - len, "; %u of %u words wrong", bad, kThreads * kWords);
            result = kFail;
        } else {
            std::snprintf(why, whySize, "%u threads x %u words exact, %zu bytes of DXBC", kThreads, kWords, c.cs.size);
        }
    }
    Release(uav);
    Release(srv);
    Release(rb);
    Release(dst);
    Release(src);
    Release(cs);
    if (result == kPass && Removed(d, c.name)) return kRemoved;
    return result;
}

Result RunTess(Device &d, const Case &c, char *why, size_t whySize)
{
    ID3D11VertexShader *vs = nullptr;
    ID3D11HullShader *hs = nullptr;
    ID3D11DomainShader *ds = nullptr;
    ID3D11PixelShader *ps = nullptr;
    HRESULT hr;
    if (FAILED(hr = d.dev->CreateVertexShader(c.vs.code, c.vs.size, nullptr, &vs)) ||
        FAILED(hr = d.dev->CreateHullShader(c.hs.code, c.hs.size, nullptr, &hs)) ||
        FAILED(hr = d.dev->CreateDomainShader(c.ds.code, c.ds.size, nullptr, &ds)) ||
        FAILED(hr = d.dev->CreatePixelShader(c.ps.code, c.ps.size, nullptr, &ps))) {
        std::snprintf(why, whySize, "a Create*Shader call returned 0x%08lX", static_cast<unsigned long>(hr));
        Release(vs); Release(hs); Release(ds); Release(ps);
        return Removed(d, "Create*Shader") ? kRemoved : kFail;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = kTarget;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr, *rb = nullptr;
    HR(d.dev->CreateTexture2D(&td, nullptr, &rt));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(d.dev->CreateTexture2D(&td, nullptr, &rb));
    ID3D11RenderTargetView *rtv = nullptr;
    HR(d.dev->CreateRenderTargetView(rt, nullptr, &rtv));
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = nullptr;
    HR(d.dev->CreateRasterizerState(&rd, &rs));
    const UINT zero[4] = { 0, 0, 0, 0 };
    d.ctx->ClearRenderTargetView(rtv, reinterpret_cast<const float *>(zero));
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kTarget), static_cast<float>(kTarget), 0.0f, 1.0f };
    d.ctx->RSSetViewports(1, &vp);
    d.ctx->RSSetState(rs);
    d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    d.ctx->IASetInputLayout(nullptr);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    d.ctx->VSSetShader(vs, nullptr, 0);
    d.ctx->HSSetShader(hs, nullptr, 0);
    d.ctx->DSSetShader(ds, nullptr, 0);
    d.ctx->PSSetShader(ps, nullptr, 0);
    d.ctx->DSSetConstantBuffers(0, 1, &d.control);
    d.ctx->Draw(3, 0);
    d.ctx->HSSetShader(nullptr, nullptr, 0);
    d.ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11RenderTargetView *noRtv = nullptr;
    d.ctx->OMSetRenderTargets(1, &noRtv, nullptr);
    d.ctx->CopyResource(rb, rt);
    D3D11_MAPPED_SUBRESOURCE map = {};
    hr = d.ctx->Map(rb, 0, D3D11_MAP_READ, 0, &map);
    Result result = kPass;
    if (FAILED(hr)) {
        std::snprintf(why, whySize, "Map of the target returned 0x%08lX", static_cast<unsigned long>(hr));
        result = Removed(d, "Map") ? kRemoved : kFail;
    } else {
        const uint32_t want[4] = { kTessCheck, 0xC0FFEEu, 0, 1 };
        uint32_t bad = 0, unwritten = 0;
        for (uint32_t y = 0; y < kTarget; ++y) {
            const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(map.pData) +
                                                                     y * map.RowPitch);
            for (uint32_t x = 0; x < kTarget; ++x) {
                const uint32_t *p = row + 4 * x;
                if (!p[0] && !p[1] && !p[2] && !p[3]) { ++unwritten; continue; }
                if (std::memcmp(p, want, sizeof(want)) == 0) continue;
                if (!bad)
                    std::snprintf(why, whySize, "pixel (%u, %u): got (%u, 0x%X, %u, %u) want (%u, 0x%X, 0, 1)", x, y,
                                  p[0], p[1], p[2], p[3], want[0], want[1]);
                ++bad;
            }
        }
        d.ctx->Unmap(rb, 0);
        if (bad) {
            const size_t len = std::strlen(why);
            std::snprintf(why + len, whySize - len, "; %u wrong and %u unwritten of %u pixels", bad, unwritten,
                          kTarget * kTarget);
            result = kFail;
        } else if (unwritten) {
            std::snprintf(why, whySize, "%u of %u pixels unwritten; the patch covers the whole target", unwritten,
                          kTarget * kTarget);
            result = kFail;
        } else {
            std::snprintf(why, whySize, "%u pixels hold %u, hull DXBC %zu bytes", kTarget * kTarget, want[0], c.hs.size);
        }
    }
    Release(rs); Release(rtv); Release(rb); Release(rt);
    Release(vs); Release(hs); Release(ds); Release(ps);
    if (result == kPass && Removed(d, c.name)) return kRemoved;
    return result;
}

[[noreturn]] void Usage(FILE *out, int code)
{
    std::fprintf(out,
                 "usage: d3d11shaderedge [--adapter N | --warp] [--any-adapter] [--list-adapters] [--case NAME]\n"
                 "                       [--compiler fxc|fxc2] [--with-no64] [--debug] [--negative-control]\n"
                 "                       [--deadline SEC]\n"
                 "\n"
                 "  --adapter N         use DXGI adapter N (default: the BC-250).\n"
                 "  --warp              use the WARP software device (the reference run).\n"
                 "  --any-adapter       accept an adapter that is not a BC-250.\n"
                 "  --list-adapters     print the DXGI adapters and exit.\n"
                 "  --case NAME         run only the cases whose name starts with NAME.\n"
                 "  --compiler NAME     run only the bytecode of this compiler.\n"
                 "  --with-no64         also run the tessellation bytecode of FXC2 without patch 64, which can\n"
                 "                      remove the device on some drivers.\n"
                 "  --debug             create the device with the D3D11 debug layer and print its messages.\n"
                 "  --negative-control  every program computes a known wrong result; every case must fail.\n"
                 "  --deadline SEC      end the run after this many seconds (default 60, at most 170).\n"
                 "\n"
                 "exit: 0 every case passed, 1 a case failed, 2 usage, 3 D3D11 or system error or a removed device,\n"
                 "      4 deadline.\n");
    std::exit(code);
}

}  // namespace

int main(int argc, char **argv)
{
    const char *only = nullptr, *compiler = nullptr;
    UINT adapterIndex = UINT_MAX;
    bool warp = false, anyAdapter = false, listAdapters = false, withNo64 = false, debug = false;
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) Usage(stdout, 0);
        if (!std::strcmp(a, "--warp")) { warp = true; continue; }
        if (!std::strcmp(a, "--any-adapter")) { anyAdapter = true; continue; }
        if (!std::strcmp(a, "--list-adapters")) { listAdapters = true; continue; }
        if (!std::strcmp(a, "--with-no64")) { withNo64 = true; continue; }
        if (!std::strcmp(a, "--debug")) { debug = true; continue; }
        if (!std::strcmp(a, "--negative-control")) { negative = 1; continue; }
        if (i + 1 >= argc) Usage(stderr, 2);
        if (!std::strcmp(a, "--adapter")) adapterIndex = static_cast<UINT>(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(a, "--case")) only = argv[++i];
        else if (!std::strcmp(a, "--compiler")) compiler = argv[++i];
        else if (!std::strcmp(a, "--deadline")) deadlineSec = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else Usage(stderr, 2);
    }
    if (deadlineSec < 5 || deadlineSec > 170) Usage(stderr, 2);
    if (compiler && std::strcmp(compiler, "fxc") && std::strcmp(compiler, "fxc2") && std::strcmp(compiler, "fxc2no64"))
        Usage(stderr, 2);
    QueryPerformanceFrequency(&qpcFreq);
    const double start = NowMs();
    deadlineEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE watchdog = deadlineEvent ? CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr) : nullptr;
    if (!watchdog) {
        std::fprintf(stderr, "d3d11shaderedge: cannot start the watchdog (%lu)\n", GetLastError());
        return 3;
    }

    IDXGIFactory1 *factory = nullptr;
    HR(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)));
    IDXGIAdapter1 *chosen = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        a->GetDesc1(&desc);
        const bool bc250 = desc.VendorId == 0x1002 && desc.DeviceId == 0x13FE;
        std::printf("adapter %u     %ls (vendor 0x%04X device 0x%04X)%s\n", i, desc.Description, desc.VendorId,
                    desc.DeviceId, bc250 ? "  <- BC-250" : "");
        const bool pick = adapterIndex == UINT_MAX ? bc250 : adapterIndex == i;
        if (pick && !chosen && !warp) { chosen = a; a = nullptr; }
        Release(a);
    }
    if (listAdapters) return 0;
    if (!warp && !chosen) {
        if (adapterIndex != UINT_MAX || !anyAdapter) {
            std::fprintf(stderr, "d3d11shaderedge: no BC-250 adapter%s; --any-adapter, --adapter N or --warp\n",
                         adapterIndex != UINT_MAX ? " at that index" : "");
            return 3;
        }
        HR(factory->EnumAdapters1(0, &chosen));
    }
    if (chosen && !anyAdapter) {
        DXGI_ADAPTER_DESC1 desc = {};
        chosen->GetDesc1(&desc);
        if (!(desc.VendorId == 0x1002 && desc.DeviceId == 0x13FE)) {
            std::fprintf(stderr, "d3d11shaderedge: the adapter is not a BC-250; --any-adapter to accept it\n");
            return 3;
        }
    }

    Device d;
    const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL got = {};
    HR(D3D11CreateDevice(warp ? nullptr : chosen, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                         debug ? D3D11_CREATE_DEVICE_DEBUG : 0,
                         &want, 1, D3D11_SDK_VERSION, &d.dev, &got, &d.ctx));
    std::printf("device        %s, feature level 0x%04X\n", warp ? "WARP" : "hardware adapter", static_cast<unsigned>(got));
    PrintDriverModules();
    const uint32_t control[4] = { negative, 0, 0, 0 };
    d.control = MakeBuffer(d, sizeof(control), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, 0, 0, control);
    if (negative) std::printf("negative control: every program computes a known wrong result, every case must fail\n");

    uint32_t passed = 0, failed = 0, ran = 0, skipped = 0;
    bool removed = false, late = false;
    for (const Case &c : kCases) {
        if (only && std::strncmp(c.name, only, std::strlen(only))) continue;
        if (compiler && std::strcmp(c.compiler, compiler)) continue;
        if (c.hostile && !withNo64) { ++skipped; continue; }
        if (NowMs() - start > deadlineSec * 1000.0) {
            std::printf("deadline      %u s reached after %u cases, the run is incomplete\n", deadlineSec, ran);
            late = true;
            break;
        }
        char why[320] = "";
        const double t0 = NowMs();
        const Result r = c.kind == Kind::Compute ? RunCompute(d, c, why, sizeof(why)) : RunTess(d, c, why, sizeof(why));
        ++ran;
        if (debug) PrintDebugMessages(d);
        if (r == kRemoved) {
            std::printf("FAIL %-12s %-9s device removed (%s)\n", c.name, c.compiler, why);
            removed = true;
            ++failed;
            break;
        }
        std::printf("%s %-12s %-9s %s (%.1f ms)\n", r == kPass ? "PASS" : "FAIL", c.name, c.compiler, why, NowMs() - t0);
        if (r == kPass) ++passed; else ++failed;
    }
    std::printf("cases %u: %u PASS, %u FAIL, %u not selected (--with-no64)\nelapsed       %.1f s\n", ran, passed,
                failed, skipped, (NowMs() - start) / 1000.0);
    Release(d.control);
    if (d.ctx) d.ctx->ClearState();
    Release(d.ctx);
    Release(d.dev);
    Release(chosen);
    Release(factory);
    SetEvent(deadlineEvent);
    CloseHandle(watchdog);
    if (removed) {
        std::printf("result DEVICE-REMOVED\n");
        return 3;
    }
    if (!ran) {
        std::printf("result NOTHING-RAN (no case matched)\n");
        return 2;
    }
    if (negative) {
        if (passed) {
            std::printf("negative control BROKEN: %u cases passed although every program computed a wrong result\n",
                        passed);
            std::printf("result BROKEN\n");
            return 3;
        }
        std::printf("negative control: every one of %u cases failed, as it must\n", failed);
        std::printf("result FAILED (as required by the negative control)\n");
        return late ? 4 : 1;
    }
    std::printf("result %s\n", failed ? "FAILED" : (late ? "INCOMPLETE" : "ok"));
    if (failed) return 1;
    return late ? 4 : 0;
}
