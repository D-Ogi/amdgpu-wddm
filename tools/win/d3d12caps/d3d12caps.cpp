// SPDX-License-Identifier: MIT
// Dumps what a D3D12 application can learn about an adapter and device before it renders, as one JSON document
// with sorted keys, so that two routes on the same hardware diff line by line. No window, no swap chain, no
// command list. Every failed call is recorded as data; only a failure to write the document ends with nonzero.
//
// Usage: amdgpu_wddm_d3d12caps.exe [adapter-index] [output-path] [--progress]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <psapi.h>
#include <fcntl.h>
#include <io.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>
using Microsoft::WRL::ComPtr;

// Built with -AgilitySdkVersion <n>: d3d12.dll reads these exports and loads .\D3D12_0\D3D12Core.dll.
#ifdef CAPS_AGILITY_SDK_VERSION
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = CAPS_AGILITY_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12_0\\"; }
#endif

namespace {

// ---------------------------------------------------------------------------------------------------------------
// A minimal JSON tree. Objects are std::map, so keys come out sorted; lists are objects with zero-padded keys.
struct Json {
    enum class Kind { Obj, Null, Bool, Int, Real, Str };
    Kind kind = Kind::Obj;
    bool b = false;
    long long i = 0;
    double r = 0;
    std::string s;
    std::map<std::string, Json> obj;
    Json& operator[](const std::string& key) { kind = Kind::Obj; return obj[key]; }
};

Json null_value() { Json j; j.kind = Json::Kind::Null; return j; }
Json jbool(BOOL v) { Json j; j.kind = Json::Kind::Bool; j.b = v != FALSE; return j; }
template <class T> Json num(T v) {
    Json j; j.kind = Json::Kind::Int;
    if constexpr (std::is_enum_v<T>) j.i = static_cast<long long>(static_cast<std::underlying_type_t<T>>(v));
    else j.i = static_cast<long long>(v);
    return j;
}
Json real(double v) { Json j; j.kind = Json::Kind::Real; j.r = v; return j; }
Json str(const std::string& v) { Json j; j.kind = Json::Kind::Str; j.s = v; return j; }
template <class T> Json hex(T v) {
    unsigned long long u;
    if constexpr (std::is_enum_v<T>) u = static_cast<std::make_unsigned_t<std::underlying_type_t<T>>>(v);
    else u = static_cast<std::make_unsigned_t<T>>(v);
    char buf[32];
    snprintf(buf, sizeof buf, "0x%llX", u);
    return str(buf);
}
// Sizes: a UINT64_MAX returned by the runtime is its error marker, recorded as such.
Json size(UINT64 v) { return v == UINT64_MAX ? str("UINT64_MAX") : num(v); }

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string key(unsigned index, const char* name = nullptr) {
    char buf[96];
    if (name) snprintf(buf, sizeof buf, "%03u_%s", index, name);
    else snprintf(buf, sizeof buf, "%02u", index);
    return buf;
}

std::string guid(const GUID& g) {
    char buf[64];
    snprintf(buf, sizeof buf, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

Json hresult(HRESULT hr) {
    static const struct { HRESULT hr; const char* name; } names[] = {
        {S_OK, "S_OK"}, {S_FALSE, "S_FALSE"}, {E_FAIL, "E_FAIL"}, {E_INVALIDARG, "E_INVALIDARG"},
        {E_NOTIMPL, "E_NOTIMPL"}, {E_NOINTERFACE, "E_NOINTERFACE"}, {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
        {E_POINTER, "E_POINTER"}, {E_UNEXPECTED, "E_UNEXPECTED"}, {E_ACCESSDENIED, "E_ACCESSDENIED"},
        {DXGI_ERROR_UNSUPPORTED, "DXGI_ERROR_UNSUPPORTED"}, {DXGI_ERROR_NOT_FOUND, "DXGI_ERROR_NOT_FOUND"},
        {DXGI_ERROR_INVALID_CALL, "DXGI_ERROR_INVALID_CALL"}, {DXGI_ERROR_DEVICE_REMOVED, "DXGI_ERROR_DEVICE_REMOVED"},
        {DXGI_ERROR_DEVICE_HUNG, "DXGI_ERROR_DEVICE_HUNG"}, {DXGI_ERROR_DEVICE_RESET, "DXGI_ERROR_DEVICE_RESET"},
        {DXGI_ERROR_DRIVER_INTERNAL_ERROR, "DXGI_ERROR_DRIVER_INTERNAL_ERROR"},
        {DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE"},
        {D3D12_ERROR_ADAPTER_NOT_FOUND, "D3D12_ERROR_ADAPTER_NOT_FOUND"},
        {D3D12_ERROR_DRIVER_VERSION_MISMATCH, "D3D12_ERROR_DRIVER_VERSION_MISMATCH"},
        {D3D12_ERROR_INVALID_REDIST, "D3D12_ERROR_INVALID_REDIST"},
        {HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), "ERROR_MOD_NOT_FOUND"},
        {HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "ERROR_PROC_NOT_FOUND"},
    };
    char buf[80];
    const char* name = "";
    for (const auto& n : names) if (n.hr == hr) name = n.name;
    snprintf(buf, sizeof buf, "0x%08lX%s%s", static_cast<unsigned long>(hr), *name ? " " : "", name);
    return str(buf);
}

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04X", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
    out += '"';
}

// One leaf per line, so that a plain text diff is as useful as diff-caps.py.
void serialize(std::string& out, const Json& j, int depth) {
    switch (j.kind) {
    case Json::Kind::Null: out += "null"; return;
    case Json::Kind::Bool: out += j.b ? "true" : "false"; return;
    case Json::Kind::Int: out += std::to_string(j.i); return;
    case Json::Kind::Real: {
        char b[40];
        if (std::isfinite(j.r)) snprintf(b, sizeof b, "%.9g", j.r); else snprintf(b, sizeof b, "null");
        out += b; return;
    }
    case Json::Kind::Str: escape(out, j.s); return;
    case Json::Kind::Obj:
        if (j.obj.empty()) { out += "{}"; return; }
        out += "{\n";
        size_t n = 0;
        for (const auto& [k, v] : j.obj) {
            out.append(static_cast<size_t>(depth + 1), ' ');
            escape(out, k);
            out += ": ";
            serialize(out, v, depth + 1);
            out += ++n < j.obj.size() ? ",\n" : "\n";
        }
        out.append(static_cast<size_t>(depth), ' ');
        out += '}';
    }
}

bool g_progress = false;
void progress(const char* what) { if (g_progress) { fprintf(stderr, "d3d12caps: %s\n", what); fflush(stderr); } }

// ---------------------------------------------------------------------------------------------------------------
// Names from the SDK headers (10.0.26100). Gaps in the DXGI_FORMAT range are recorded as UNDEFINED_<n>.
const char* format_name(unsigned f) {
    static const std::map<unsigned, const char*> names = {
        {0, "UNKNOWN"}, {1, "R32G32B32A32_TYPELESS"}, {2, "R32G32B32A32_FLOAT"}, {3, "R32G32B32A32_UINT"},
        {4, "R32G32B32A32_SINT"}, {5, "R32G32B32_TYPELESS"}, {6, "R32G32B32_FLOAT"}, {7, "R32G32B32_UINT"},
        {8, "R32G32B32_SINT"}, {9, "R16G16B16A16_TYPELESS"}, {10, "R16G16B16A16_FLOAT"}, {11, "R16G16B16A16_UNORM"},
        {12, "R16G16B16A16_UINT"}, {13, "R16G16B16A16_SNORM"}, {14, "R16G16B16A16_SINT"}, {15, "R32G32_TYPELESS"},
        {16, "R32G32_FLOAT"}, {17, "R32G32_UINT"}, {18, "R32G32_SINT"}, {19, "R32G8X24_TYPELESS"},
        {20, "D32_FLOAT_S8X24_UINT"}, {21, "R32_FLOAT_X8X24_TYPELESS"}, {22, "X32_TYPELESS_G8X24_UINT"},
        {23, "R10G10B10A2_TYPELESS"}, {24, "R10G10B10A2_UNORM"}, {25, "R10G10B10A2_UINT"}, {26, "R11G11B10_FLOAT"},
        {27, "R8G8B8A8_TYPELESS"}, {28, "R8G8B8A8_UNORM"}, {29, "R8G8B8A8_UNORM_SRGB"}, {30, "R8G8B8A8_UINT"},
        {31, "R8G8B8A8_SNORM"}, {32, "R8G8B8A8_SINT"}, {33, "R16G16_TYPELESS"}, {34, "R16G16_FLOAT"},
        {35, "R16G16_UNORM"}, {36, "R16G16_UINT"}, {37, "R16G16_SNORM"}, {38, "R16G16_SINT"}, {39, "R32_TYPELESS"},
        {40, "D32_FLOAT"}, {41, "R32_FLOAT"}, {42, "R32_UINT"}, {43, "R32_SINT"}, {44, "R24G8_TYPELESS"},
        {45, "D24_UNORM_S8_UINT"}, {46, "R24_UNORM_X8_TYPELESS"}, {47, "X24_TYPELESS_G8_UINT"}, {48, "R8G8_TYPELESS"},
        {49, "R8G8_UNORM"}, {50, "R8G8_UINT"}, {51, "R8G8_SNORM"}, {52, "R8G8_SINT"}, {53, "R16_TYPELESS"},
        {54, "R16_FLOAT"}, {55, "D16_UNORM"}, {56, "R16_UNORM"}, {57, "R16_UINT"}, {58, "R16_SNORM"}, {59, "R16_SINT"},
        {60, "R8_TYPELESS"}, {61, "R8_UNORM"}, {62, "R8_UINT"}, {63, "R8_SNORM"}, {64, "R8_SINT"}, {65, "A8_UNORM"},
        {66, "R1_UNORM"}, {67, "R9G9B9E5_SHAREDEXP"}, {68, "R8G8_B8G8_UNORM"}, {69, "G8R8_G8B8_UNORM"},
        {70, "BC1_TYPELESS"}, {71, "BC1_UNORM"}, {72, "BC1_UNORM_SRGB"}, {73, "BC2_TYPELESS"}, {74, "BC2_UNORM"},
        {75, "BC2_UNORM_SRGB"}, {76, "BC3_TYPELESS"}, {77, "BC3_UNORM"}, {78, "BC3_UNORM_SRGB"}, {79, "BC4_TYPELESS"},
        {80, "BC4_UNORM"}, {81, "BC4_SNORM"}, {82, "BC5_TYPELESS"}, {83, "BC5_UNORM"}, {84, "BC5_SNORM"},
        {85, "B5G6R5_UNORM"}, {86, "B5G5R5A1_UNORM"}, {87, "B8G8R8A8_UNORM"}, {88, "B8G8R8X8_UNORM"},
        {89, "R10G10B10_XR_BIAS_A2_UNORM"}, {90, "B8G8R8A8_TYPELESS"}, {91, "B8G8R8A8_UNORM_SRGB"},
        {92, "B8G8R8X8_TYPELESS"}, {93, "B8G8R8X8_UNORM_SRGB"}, {94, "BC6H_TYPELESS"}, {95, "BC6H_UF16"},
        {96, "BC6H_SF16"}, {97, "BC7_TYPELESS"}, {98, "BC7_UNORM"}, {99, "BC7_UNORM_SRGB"}, {100, "AYUV"},
        {101, "Y410"}, {102, "Y416"}, {103, "NV12"}, {104, "P010"}, {105, "P016"}, {106, "420_OPAQUE"}, {107, "YUY2"},
        {108, "Y210"}, {109, "Y216"}, {110, "NV11"}, {111, "AI44"}, {112, "IA44"}, {113, "P8"}, {114, "A8P8"},
        {115, "B4G4R4A4_UNORM"}, {130, "P208"}, {131, "V208"}, {132, "V408"},
        {189, "SAMPLER_FEEDBACK_MIN_MIP_OPAQUE"}, {190, "SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE"},
        {191, "A4B4G4R4_UNORM"},
    };
    auto it = names.find(f);
    if (it != names.end()) return it->second;
    static thread_local char buf[32];
    snprintf(buf, sizeof buf, "UNDEFINED_%u", f);
    return buf;
}

struct FlagName { UINT bit; const char* name; };
const FlagName kSupport1[] = {
    {0x1, "BUFFER"}, {0x2, "IA_VERTEX_BUFFER"}, {0x4, "IA_INDEX_BUFFER"}, {0x8, "SO_BUFFER"}, {0x10, "TEXTURE1D"},
    {0x20, "TEXTURE2D"}, {0x40, "TEXTURE3D"}, {0x80, "TEXTURECUBE"}, {0x100, "SHADER_LOAD"}, {0x200, "SHADER_SAMPLE"},
    {0x400, "SHADER_SAMPLE_COMPARISON"}, {0x800, "SHADER_SAMPLE_MONO_TEXT"}, {0x1000, "MIP"},
    {0x4000, "RENDER_TARGET"}, {0x8000, "BLENDABLE"}, {0x10000, "DEPTH_STENCIL"}, {0x40000, "MULTISAMPLE_RESOLVE"},
    {0x80000, "DISPLAY"}, {0x100000, "CAST_WITHIN_BIT_LAYOUT"}, {0x200000, "MULTISAMPLE_RENDERTARGET"},
    {0x400000, "MULTISAMPLE_LOAD"}, {0x800000, "SHADER_GATHER"}, {0x1000000, "BACK_BUFFER_CAST"},
    {0x2000000, "TYPED_UNORDERED_ACCESS_VIEW"}, {0x4000000, "SHADER_GATHER_COMPARISON"},
    {0x8000000, "DECODER_OUTPUT"}, {0x10000000, "VIDEO_PROCESSOR_OUTPUT"}, {0x20000000, "VIDEO_PROCESSOR_INPUT"},
    {0x40000000, "VIDEO_ENCODER"},
};
const FlagName kSupport2[] = {
    {0x1, "UAV_ATOMIC_ADD"}, {0x2, "UAV_ATOMIC_BITWISE_OPS"}, {0x4, "UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE"},
    {0x8, "UAV_ATOMIC_EXCHANGE"}, {0x10, "UAV_ATOMIC_SIGNED_MIN_OR_MAX"}, {0x20, "UAV_ATOMIC_UNSIGNED_MIN_OR_MAX"},
    {0x40, "UAV_TYPED_LOAD"}, {0x80, "UAV_TYPED_STORE"}, {0x100, "OUTPUT_MERGER_LOGIC_OP"}, {0x200, "TILED"},
    {0x4000, "MULTIPLANE_OVERLAY"}, {0x8000, "SAMPLER_FEEDBACK"}, {0x10000, "DISPLAYABLE"},
};
const FlagName kShaderCache[] = {
    {0x1, "SINGLE_PSO"}, {0x2, "LIBRARY"}, {0x4, "AUTOMATIC_INPROC_CACHE"}, {0x8, "AUTOMATIC_DISK_CACHE"},
    {0x10, "DRIVER_MANAGED_CACHE"}, {0x20, "SHADER_CONTROL_CLEAR"}, {0x40, "SHADER_SESSION_DELETE"},
};
const FlagName kAdapterFlags3[] = {
    {0x1, "REMOTE"}, {0x2, "SOFTWARE"}, {0x4, "ACG_COMPATIBLE"}, {0x8, "SUPPORT_MONITORED_FENCES"},
    {0x10, "SUPPORT_NON_MONITORED_FENCES"}, {0x20, "KEYED_MUTEX_CONFORMANCE"},
};

// "A|B|C", unknown bits as a trailing hex value, "" for none; diff-caps.py reports added and removed names.
template <size_t N> Json flags(UINT v, const FlagName (&table)[N]) {
    std::string s;
    for (const auto& f : table) if (v & f.bit) { if (!s.empty()) s += '|'; s += f.name; v &= ~f.bit; }
    if (v) { char b[24]; snprintf(b, sizeof b, "%s0x%X", s.empty() ? "" : "|", v); s += b; }
    return str(s);
}

const char* feature_level_name(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_1_0_GENERIC: return "1_0_GENERIC";
    case D3D_FEATURE_LEVEL_1_0_CORE: return "1_0_CORE";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    default: return nullptr;
    }
}
Json feature_level(D3D_FEATURE_LEVEL fl) {
    const char* n = feature_level_name(fl);
    return n ? str(n) : hex(static_cast<UINT>(fl));
}

// ---------------------------------------------------------------------------------------------------------------
template <class T> bool query(ID3D12Device* d, D3D12_FEATURE f, T& data, Json& out) {
    HRESULT hr = d->CheckFeatureSupport(f, &data, sizeof data);
    out["hr"] = hresult(hr);
    return SUCCEEDED(hr);
}
#define B(f) j[#f] = jbool(o.f)
#define N(f) j[#f] = num(o.f)
#define X(f) j[#f] = hex(o.f)

void features(ID3D12Device* d, Json& out) {
    { D3D12_FEATURE_DATA_D3D12_OPTIONS o{}; Json& j = out["D3D12_OPTIONS"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS, o, j)) {
        B(DoublePrecisionFloatShaderOps); B(OutputMergerLogicOp); X(MinPrecisionSupport); N(TiledResourcesTier);
        N(ResourceBindingTier); B(PSSpecifiedStencilRefSupported); B(TypedUAVLoadAdditionalFormats);
        B(ROVsSupported); N(ConservativeRasterizationTier); N(MaxGPUVirtualAddressBitsPerResource);
        B(StandardSwizzle64KBSupported); N(CrossNodeSharingTier); B(CrossAdapterRowMajorTextureSupported);
        B(VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation); N(ResourceHeapTier); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS1 o{}; Json& j = out["D3D12_OPTIONS1"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS1, o, j)) {
        B(WaveOps); N(WaveLaneCountMin); N(WaveLaneCountMax); N(TotalLaneCount); B(ExpandedComputeResourceStates);
        B(Int64ShaderOps); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS2 o{}; Json& j = out["D3D12_OPTIONS2"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS2, o, j)) { B(DepthBoundsTestSupported); N(ProgrammableSamplePositionsTier); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS3 o{}; Json& j = out["D3D12_OPTIONS3"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS3, o, j)) {
        B(CopyQueueTimestampQueriesSupported); B(CastingFullyTypedFormatSupported); X(WriteBufferImmediateSupportFlags);
        N(ViewInstancingTier); B(BarycentricsSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS4 o{}; Json& j = out["D3D12_OPTIONS4"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS4, o, j)) {
        B(MSAA64KBAlignedTextureSupported); N(SharedResourceCompatibilityTier); B(Native16BitShaderOpsSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS5 o{}; Json& j = out["D3D12_OPTIONS5"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS5, o, j)) { B(SRVOnlyTiledResourceTier3); N(RenderPassesTier); N(RaytracingTier); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS6 o{}; Json& j = out["D3D12_OPTIONS6"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS6, o, j)) {
        B(AdditionalShadingRatesSupported); B(PerPrimitiveShadingRateSupportedWithViewportIndexing);
        N(VariableShadingRateTier); N(ShadingRateImageTileSize); B(BackgroundProcessingSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS7 o{}; Json& j = out["D3D12_OPTIONS7"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS7, o, j)) { N(MeshShaderTier); N(SamplerFeedbackTier); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS8 o{}; Json& j = out["D3D12_OPTIONS8"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS8, o, j)) { B(UnalignedBlockTexturesSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS9 o{}; Json& j = out["D3D12_OPTIONS9"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS9, o, j)) {
        B(MeshShaderPipelineStatsSupported); B(MeshShaderSupportsFullRangeRenderTargetArrayIndex);
        B(AtomicInt64OnTypedResourceSupported); B(AtomicInt64OnGroupSharedSupported);
        B(DerivativesInMeshAndAmplificationShadersSupported); N(WaveMMATier); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS10 o{}; Json& j = out["D3D12_OPTIONS10"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS10, o, j)) {
        B(VariableRateShadingSumCombinerSupported); B(MeshShaderPerPrimitiveShadingRateSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS11 o{}; Json& j = out["D3D12_OPTIONS11"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS11, o, j)) { B(AtomicInt64OnDescriptorHeapResourceSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS12 o{}; Json& j = out["D3D12_OPTIONS12"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS12, o, j)) {
        N(MSPrimitivesPipelineStatisticIncludesCulledPrimitives); B(EnhancedBarriersSupported);
        B(RelaxedFormatCastingSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS13 o{}; Json& j = out["D3D12_OPTIONS13"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS13, o, j)) {
        B(UnrestrictedBufferTextureCopyPitchSupported); B(UnrestrictedVertexElementAlignmentSupported);
        B(InvertedViewportHeightFlipsYSupported); B(InvertedViewportDepthFlipsZSupported);
        B(TextureCopyBetweenDimensionsSupported); B(AlphaBlendFactorSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS14 o{}; Json& j = out["D3D12_OPTIONS14"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS14, o, j)) {
        B(AdvancedTextureOpsSupported); B(WriteableMSAATexturesSupported); B(IndependentFrontAndBackStencilRefMaskSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS15 o{}; Json& j = out["D3D12_OPTIONS15"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS15, o, j)) { B(TriangleFanSupported); B(DynamicIndexBufferStripCutSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS16 o{}; Json& j = out["D3D12_OPTIONS16"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS16, o, j)) { B(DynamicDepthBiasSupported); B(GPUUploadHeapSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS17 o{}; Json& j = out["D3D12_OPTIONS17"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS17, o, j)) {
        B(NonNormalizedCoordinateSamplersSupported); B(ManualWriteTrackingResourceSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS18 o{}; Json& j = out["D3D12_OPTIONS18"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS18, o, j)) { B(RenderPassesValid); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS19 o{}; Json& j = out["D3D12_OPTIONS19"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS19, o, j)) {
        B(MismatchingOutputDimensionsSupported); X(SupportedSampleCountsWithNoOutputs);
        B(PointSamplingAddressesNeverRoundUp); B(RasterizerDesc2Supported); B(NarrowQuadrilateralLinesSupported);
        B(AnisoFilterWithPointMipSupported); N(MaxSamplerDescriptorHeapSize);
        N(MaxSamplerDescriptorHeapSizeWithStaticSamplers); N(MaxViewDescriptorHeapSize);
        B(ComputeOnlyCustomHeapSupported); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS20 o{}; Json& j = out["D3D12_OPTIONS20"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS20, o, j)) { B(ComputeOnlyWriteWatchSupported); N(RecreateAtTier); } }
    { D3D12_FEATURE_DATA_D3D12_OPTIONS21 o{}; Json& j = out["D3D12_OPTIONS21"];
      if (query(d, D3D12_FEATURE_D3D12_OPTIONS21, o, j)) {
        N(WorkGraphsTier); N(ExecuteIndirectTier); B(SampleCmpGradientAndBiasSupported); B(ExtendedCommandInfoSupported); } }

    { D3D12_FEATURE_DATA_ARCHITECTURE o{}; Json& j = out["ARCHITECTURE"];
      if (query(d, D3D12_FEATURE_ARCHITECTURE, o, j)) { N(NodeIndex); B(TileBasedRenderer); B(UMA); B(CacheCoherentUMA); } }
    { D3D12_FEATURE_DATA_ARCHITECTURE1 o{}; Json& j = out["ARCHITECTURE1"];
      if (query(d, D3D12_FEATURE_ARCHITECTURE1, o, j)) {
        N(NodeIndex); B(TileBasedRenderer); B(UMA); B(CacheCoherentUMA); B(IsolatedMMU); } }
    { D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT o{}; Json& j = out["GPU_VIRTUAL_ADDRESS_SUPPORT"];
      if (query(d, D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, o, j)) {
        N(MaxGPUVirtualAddressBitsPerResource); N(MaxGPUVirtualAddressBitsPerProcess); } }
    { D3D12_FEATURE_DATA_SHADER_CACHE o{}; Json& j = out["SHADER_CACHE"];
      if (query(d, D3D12_FEATURE_SHADER_CACHE, o, j)) { X(SupportFlags); j["SupportFlagNames"] = flags(o.SupportFlags, kShaderCache); } }
    { D3D12_FEATURE_DATA_EXISTING_HEAPS o{}; Json& j = out["EXISTING_HEAPS"];
      if (query(d, D3D12_FEATURE_EXISTING_HEAPS, o, j)) { B(Supported); } }
    { D3D12_FEATURE_DATA_SERIALIZATION o{}; Json& j = out["SERIALIZATION"];
      if (query(d, D3D12_FEATURE_SERIALIZATION, o, j)) { N(NodeIndex); N(HeapSerializationTier); } }
    { D3D12_FEATURE_DATA_CROSS_NODE o{}; Json& j = out["CROSS_NODE"];
      if (query(d, D3D12_FEATURE_CROSS_NODE, o, j)) { N(SharingTier); B(AtomicShaderInstructions); } }
    { D3D12_FEATURE_DATA_DISPLAYABLE o{}; Json& j = out["DISPLAYABLE"];
      if (query(d, D3D12_FEATURE_DISPLAYABLE, o, j)) { B(DisplayableTexture); N(SharedResourceCompatibilityTier); } }
    { D3D12_FEATURE_DATA_PREDICATION o{}; Json& j = out["PREDICATION"];
      if (query(d, D3D12_FEATURE_PREDICATION, o, j)) { B(Supported); } }
    { D3D12_FEATURE_DATA_HARDWARE_COPY o{}; Json& j = out["HARDWARE_COPY"];
      if (query(d, D3D12_FEATURE_HARDWARE_COPY, o, j)) { B(Supported); } }
    { D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT o{}; Json& j = out["PROTECTED_RESOURCE_SESSION_SUPPORT"];
      if (query(d, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT, o, j)) { N(NodeIndex); X(Support); } }
    { D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT o{}; Json& j = out["PROTECTED_RESOURCE_SESSION_TYPE_COUNT"];
      if (query(d, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, o, j)) {
        N(NodeIndex); N(Count);
        if (o.Count && o.Count < 256) {
            std::vector<GUID> types(o.Count);
            D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES t{0, o.Count, types.data()};
            Json& k = out["PROTECTED_RESOURCE_SESSION_TYPES"];
            if (query(d, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES, t, k))
                for (UINT i = 0; i < o.Count; i++) k["pTypes"][key(i)] = str(guid(types[i]));
        } } }

    // FEATURE_LEVELS: the whole list the headers define, the graphics levels alone, and each level on its own.
    {
        const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_1_0_GENERIC, D3D_FEATURE_LEVEL_1_0_CORE, D3D_FEATURE_LEVEL_9_1,
            D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_2};
        Json& j = out["FEATURE_LEVELS"];
        auto one = [&](const D3D_FEATURE_LEVEL* list, UINT count, Json& o) {
            D3D12_FEATURE_DATA_FEATURE_LEVELS fl{count, list, static_cast<D3D_FEATURE_LEVEL>(0)};
            if (query(d, D3D12_FEATURE_FEATURE_LEVELS, fl, o)) o["MaxSupportedFeatureLevel"] = feature_level(fl.MaxSupportedFeatureLevel);
        };
        one(all, ARRAYSIZE(all), j["list_all"]);
        one(all + 2, ARRAYSIZE(all) - 2, j["list_9_1_to_12_2"]);
        for (const auto& fl : all) one(&fl, 1, j["single"][feature_level_name(fl)]);
    }
    // SHADER_MODEL: probed from the newest the headers name down to 5_1; each attempt is kept.
    {
        const D3D_SHADER_MODEL models[] = {D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7,
            D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
            D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
        Json& j = out["SHADER_MODEL"];
        bool found = false;
        for (auto m : models) {
            D3D12_FEATURE_DATA_SHADER_MODEL o{m};
            Json& a = j["probe"][hex(static_cast<UINT>(m)).s];
            if (query(d, D3D12_FEATURE_SHADER_MODEL, o, a)) {
                a["HighestShaderModel"] = hex(static_cast<UINT>(o.HighestShaderModel));
                if (!found) { j["HighestShaderModel"] = hex(static_cast<UINT>(o.HighestShaderModel)); found = true; }
            }
        }
        if (!found) j["HighestShaderModel"] = null_value();
    }
    {
        const D3D_ROOT_SIGNATURE_VERSION versions[] = {D3D_ROOT_SIGNATURE_VERSION_1_2, D3D_ROOT_SIGNATURE_VERSION_1_1,
            D3D_ROOT_SIGNATURE_VERSION_1_0};
        Json& j = out["ROOT_SIGNATURE"];
        bool found = false;
        for (auto v : versions) {
            D3D12_FEATURE_DATA_ROOT_SIGNATURE o{v};
            Json& a = j["probe"][hex(static_cast<UINT>(v)).s];
            if (query(d, D3D12_FEATURE_ROOT_SIGNATURE, o, a)) {
                a["HighestVersion"] = hex(static_cast<UINT>(o.HighestVersion));
                if (!found) { j["HighestVersion"] = hex(static_cast<UINT>(o.HighestVersion)); found = true; }
            }
        }
        if (!found) j["HighestVersion"] = null_value();
    }
    {
        const struct { D3D12_COMMAND_LIST_TYPE type; const char* name; } types[] = {
            {D3D12_COMMAND_LIST_TYPE_DIRECT, "DIRECT"}, {D3D12_COMMAND_LIST_TYPE_COMPUTE, "COMPUTE"},
            {D3D12_COMMAND_LIST_TYPE_COPY, "COPY"}, {D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, "VIDEO_DECODE"},
            {D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, "VIDEO_PROCESS"}, {D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, "VIDEO_ENCODE"}};
        const struct { UINT priority; const char* name; } priorities[] = {
            {D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, "NORMAL"}, {D3D12_COMMAND_QUEUE_PRIORITY_HIGH, "HIGH"},
            {D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME, "GLOBAL_REALTIME"}};
        for (const auto& t : types) for (const auto& p : priorities) {
            D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY o{t.type, p.priority, FALSE};
            Json& j = out["COMMAND_QUEUE_PRIORITY"][t.name][p.name];
            if (query(d, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, o, j)) B(PriorityForTypeIsSupported);
        }
    }
    {
        const struct { DXGI_FORMAT format; D3D12_RESOURCE_DIMENSION dim; const char* name; } resources[] = {
            {DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_DIMENSION_BUFFER, "BUFFER"},
            {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_DIMENSION_TEXTURE2D, "TEXTURE2D_R8G8B8A8_UNORM"},
            {DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE2D, "TEXTURE2D_D32_FLOAT"},
            {DXGI_FORMAT_BC7_UNORM, D3D12_RESOURCE_DIMENSION_TEXTURE2D, "TEXTURE2D_BC7_UNORM"}};
        const struct { D3D12_HEAP_TYPE type; const char* name; } heaps[] = {
            {D3D12_HEAP_TYPE_DEFAULT, "DEFAULT"}, {D3D12_HEAP_TYPE_UPLOAD, "UPLOAD"},
            {D3D12_HEAP_TYPE_READBACK, "READBACK"}, {D3D12_HEAP_TYPE_GPU_UPLOAD, "GPU_UPLOAD"}};
        for (const auto& r : resources) for (const auto& h : heaps) {
            D3D12_FEATURE_DATA_PLACED_RESOURCE_SUPPORT_INFO o{};
            o.Format = r.format; o.Dimension = r.dim; o.DestHeapProperties.Type = h.type;
            Json& j = out["PLACED_RESOURCE_SUPPORT_INFO"][r.name][h.name];
            if (query(d, D3D12_FEATURE_PLACED_RESOURCE_SUPPORT_INFO, o, j)) B(Supported);
        }
    }
}
#undef B
#undef N
#undef X

// Feature values past the newest one the headers name (OPTIONS21 = 53): an Agility SDK core newer than the headers
// may answer them. The structure size is found by trying 4..1024 bytes with a zeroed buffer; the first size the
// runtime does not reject with E_INVALIDARG is recorded with the raw bytes. SEH guards a runtime that trusts a
// zero pointer inside an unknown structure.
HRESULT raw_query(ID3D12Device* d, UINT feature, void* data, UINT size, bool* faulted) {
    __try { return d->CheckFeatureSupport(static_cast<D3D12_FEATURE>(feature), data, size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *faulted = true; return E_UNEXPECTED; }
}
void raw_features(ID3D12Device* d, Json& out) {
    for (UINT f = 54; f < 80; f++) {
        Json& j = out[key(f, "feature")];
        HRESULT hr = E_INVALIDARG;
        for (UINT size = 4; size <= 1024; size += 4) {
            std::vector<unsigned char> data(size, 0);
            bool faulted = false;
            hr = raw_query(d, f, data.data(), size, &faulted);
            if (faulted) { j["faulted_at_size"] = num(size); break; }
            if (hr == E_INVALIDARG) continue;
            j["size"] = num(size);
            if (SUCCEEDED(hr)) {
                std::string bytes;
                for (auto b : data) { char h[4]; snprintf(h, sizeof h, "%02X", b); bytes += h; }
                j["bytes"] = str(bytes);
            }
            break;
        }
        j["hr"] = hresult(hr);
    }
}

void formats(ID3D12Device* d, Json& out, std::map<unsigned, UINT8>& planes) {
    std::vector<unsigned> list;
    for (unsigned f = 0; f <= 132; f++) list.push_back(f);
    list.push_back(189); list.push_back(190); list.push_back(191);
    for (unsigned f : list) {
        Json& j = out[key(f, format_name(f))];
        D3D12_FEATURE_DATA_FORMAT_SUPPORT s{static_cast<DXGI_FORMAT>(f), D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
        HRESULT hr = d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof s);
        const bool supported = SUCCEEDED(hr);
        Json& fs = j["FORMAT_SUPPORT"];
        fs["hr"] = hresult(hr);
        if (supported) {
            fs["Support1"] = hex(static_cast<UINT>(s.Support1));
            fs["Support1Names"] = flags(static_cast<UINT>(s.Support1), kSupport1);
            fs["Support2"] = hex(static_cast<UINT>(s.Support2));
            fs["Support2Names"] = flags(static_cast<UINT>(s.Support2), kSupport2);
        }
        D3D12_FEATURE_DATA_FORMAT_INFO info{static_cast<DXGI_FORMAT>(f), 0};
        hr = d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof info);
        j["FORMAT_INFO"]["hr"] = hresult(hr);
        if (SUCCEEDED(hr)) { j["FORMAT_INFO"]["PlaneCount"] = num(info.PlaneCount); planes[f] = info.PlaneCount; }
        const UINT rtds = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
        if (supported && (s.Support1 & rtds)) {
            for (UINT count : {1u, 2u, 4u, 8u}) {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS m{static_cast<DXGI_FORMAT>(f), count,
                    D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
                hr = d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &m, sizeof m);
                Json& q = j["MULTISAMPLE_QUALITY_LEVELS"][std::to_string(count)];
                q["hr"] = hresult(hr);
                if (SUCCEEDED(hr)) q["NumQualityLevels"] = num(m.NumQualityLevels);
            }
        }
    }
}

UINT16 full_mips(UINT64 w, UINT h, UINT16 depth, D3D12_RESOURCE_DIMENSION dim) {
    UINT64 m = w > h ? w : h;
    if (dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D && depth > m) m = depth;
    UINT16 n = 1;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

void allocations(ID3D12Device* d, Json& out, const std::map<unsigned, UINT8>& planes) {
    constexpr auto T2 = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    constexpr auto T3 = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    constexpr auto BUF = D3D12_RESOURCE_DIMENSION_BUFFER;
    constexpr auto RT = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    constexpr auto DS = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    constexpr auto UAV = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr auto NONE = D3D12_RESOURCE_FLAG_NONE;
    constexpr auto SIM = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    constexpr UINT16 FULL = 0;  // full mip chain, computed below
    const struct Case {
        const char* name; D3D12_RESOURCE_DIMENSION dim; UINT64 w; UINT h; UINT16 depth; UINT16 mips;
        DXGI_FORMAT format; UINT samples; D3D12_RESOURCE_FLAGS flags; UINT64 alignment;
    } cases[] = {
        {"rgba8_1920x1200_rt", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, RT, 0},
        {"rgba8_1920x1200_rt_msaa4", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, RT, 0},
        {"rgba8_1920x1200_rt_uav", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, RT | UAV, 0},
        {"rgba8_1920x1200_none", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, NONE, 0},
        {"rgba8_typeless_2560x1440_rt", T2, 2560, 1440, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1, RT, 0},
        {"bgra8_1920x1200_rt", T2, 1920, 1200, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1, RT, 0},
        {"rgb10a2_1920x1200_rt", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, 1, RT, 0},
        {"rgba16f_1920x1200_rt", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, RT, 0},
        {"rgba16f_1920x1200_rt_msaa4", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, RT, 0},
        {"rgba16f_1920x1200_uav", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, UAV, 0},
        {"rgba16f_1920x1200_rt_uav", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, RT | UAV, 0},
        {"r11g11b10_1920x1200_rt", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, 1, RT, 0},
        {"r11g11b10_1920x1200_uav", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, 1, UAV, 0},
        {"r11g11b10_1920x1200_rt_msaa4", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, 4, RT, 0},
        {"r32f_1920x1200_uav", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R32_FLOAT, 1, UAV, 0},
        {"d32_1920x1200_ds", T2, 1920, 1200, 1, 1, DXGI_FORMAT_D32_FLOAT, 1, DS, 0},
        {"d32_1920x1200_ds_msaa4", T2, 1920, 1200, 1, 1, DXGI_FORMAT_D32_FLOAT, 4, DS, 0},
        {"r32_typeless_1920x1200_ds", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R32_TYPELESS, 1, DS, 0},
        {"d24s8_1920x1200_ds", T2, 1920, 1200, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, DS, 0},
        {"d24s8_1920x1200_ds_msaa4", T2, 1920, 1200, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, DS, 0},
        {"r24g8_typeless_1920x1200_ds", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, DS, 0},
        {"r32g8x24_typeless_1920x1200_ds", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 1, DS, 0},
        {"r32g8x24_typeless_1920x1200_ds_msaa4", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 4, DS, 0},
        {"d32s8_1920x1200_ds", T2, 1920, 1200, 1, 1, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 1, DS, 0},
        // The size-class pool descriptions of The Witcher 3 5.0 (render targets shared between passes).
        {"rgba8_1920x1200_rt_uav_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, RT | UAV | SIM, 0},
        {"rgba8_1920x1200_rt_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, RT | SIM, 0},
        {"rgba8_1920x1200_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, SIM, 0},
        {"rgba16f_1920x1200_rt_uav_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, RT | UAV | SIM, 0},
        {"rgba16f_1920x1200_rt_uav_simultaneous_mips9", T2, 1920, 1200, 1, 9, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, RT | UAV | SIM, 0},
        {"r11g11b10_1920x1200_rt_uav_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, 1, RT | UAV | SIM, 0},
        {"r16g16f_1920x1200_rt_uav_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R16G16_FLOAT, 1, RT | UAV | SIM, 0},
        {"r32f_1920x1200_rt_uav_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R32_FLOAT, 1, RT | UAV | SIM, 0},
        {"r8g8_1920x1200_rt_uav_simultaneous", T2, 1920, 1200, 1, 1, DXGI_FORMAT_R8G8_UNORM, 1, RT | UAV | SIM, 0},
        {"d32_2048x2048x4_ds_shadow_array", T2, 2048, 2048, 4, 1, DXGI_FORMAT_D32_FLOAT, 1, DS, 0},
        {"bc1_2048_mips", T2, 2048, 2048, 1, FULL, DXGI_FORMAT_BC1_UNORM, 1, NONE, 0},
        {"bc3_2048_mips", T2, 2048, 2048, 1, FULL, DXGI_FORMAT_BC3_UNORM, 1, NONE, 0},
        {"bc5_2048_mips", T2, 2048, 2048, 1, FULL, DXGI_FORMAT_BC5_UNORM, 1, NONE, 0},
        {"bc7_2048_mips", T2, 2048, 2048, 1, FULL, DXGI_FORMAT_BC7_UNORM, 1, NONE, 0},
        {"bc7_srgb_2048_mips", T2, 2048, 2048, 1, FULL, DXGI_FORMAT_BC7_UNORM_SRGB, 1, NONE, 0},
        {"rgba8_2048_mips", T2, 2048, 2048, 1, FULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, NONE, 0},
        {"rgba8_1024_cube_array6_mips", T2, 1024, 1024, 6, FULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, NONE, 0},
        {"rgba16f_256_cube_array6_mips_rt", T2, 256, 256, 6, FULL, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, RT, 0},
        {"rgba16f_128x128x128_3d_uav", T3, 128, 128, 128, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, UAV, 0},
        // Small placement (4 KB, total at most 64 KB) and small MSAA placement (64 KB, total at most 4 MB):
        // engines ask for these first and fall back when the answer is UINT64_MAX.
        {"small_rgba8_64_align4k", T2, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, NONE, 4096},
        {"small_rgba8_64_mips_align4k", T2, 64, 64, 1, FULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, NONE, 4096},
        {"small_bc7_128_mips_align4k", T2, 128, 128, 1, FULL, DXGI_FORMAT_BC7_UNORM, 1, NONE, 4096},
        {"small_bc1_64_mips_align4k", T2, 64, 64, 1, FULL, DXGI_FORMAT_BC1_UNORM, 1, NONE, 4096},
        {"small_rgba8_256_align4k_too_large", T2, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, NONE, 4096},
        {"small_rgba8_256_rt_msaa4_align64k", T2, 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, RT, 65536},
        {"buffer_64k", BUF, 65536, 1, 1, 1, DXGI_FORMAT_UNKNOWN, 1, NONE, 0},
        {"buffer_256_align64k", BUF, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, 1, NONE, 65536},
        {"buffer_16m_uav", BUF, 16u << 20, 1, 1, 1, DXGI_FORMAT_UNKNOWN, 1, UAV, 0},
    };
    for (const auto& c : cases) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = c.dim;
        desc.Alignment = c.alignment;
        desc.Width = c.w;
        desc.Height = c.h;
        desc.DepthOrArraySize = c.depth;
        desc.MipLevels = c.mips == FULL ? full_mips(c.w, c.h, c.depth, c.dim) : c.mips;
        desc.Format = c.format;
        desc.SampleDesc = {c.samples, 0};
        desc.Layout = c.dim == BUF ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = c.flags;
        Json& j = out[c.name];
        j["desc"]["Format"] = str(format_name(c.format));
        j["desc"]["Width"] = num(desc.Width);
        j["desc"]["Height"] = num(desc.Height);
        j["desc"]["DepthOrArraySize"] = num(desc.DepthOrArraySize);
        j["desc"]["MipLevels"] = num(desc.MipLevels);
        j["desc"]["SampleCount"] = num(c.samples);
        j["desc"]["Flags"] = hex(static_cast<UINT>(c.flags));
        j["desc"]["Alignment"] = num(c.alignment);
        D3D12_RESOURCE_ALLOCATION_INFO info = d->GetResourceAllocationInfo(0, 1, &desc);
        j["GetResourceAllocationInfo"]["SizeInBytes"] = size(info.SizeInBytes);
        j["GetResourceAllocationInfo"]["Alignment"] = num(info.Alignment);
        // Footprints only for single-sampled resources, where they are defined.
        if (c.samples == 1) {
            auto p = planes.find(static_cast<unsigned>(c.format));
            UINT plane_count = (p != planes.end() && p->second) ? p->second : 1;
            UINT sub = c.dim == BUF ? 1 : desc.MipLevels * (c.dim == T3 ? 1u : c.depth) * plane_count;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(sub);
            std::vector<UINT> rows(sub);
            std::vector<UINT64> row_sizes(sub);
            UINT64 total = 0;
            d->GetCopyableFootprints(&desc, 0, sub, 0, layouts.data(), rows.data(), row_sizes.data(), &total);
            Json& f = j["GetCopyableFootprints"];
            f["NumSubresources"] = num(sub);
            f["TotalBytes"] = size(total);
            f["sub0"]["RowPitch"] = num(layouts[0].Footprint.RowPitch);
            f["sub0"]["NumRows"] = num(rows[0]);
            f["sub0"]["RowSizeInBytes"] = size(row_sizes[0]);
            f["sub0"]["Width"] = num(layouts[0].Footprint.Width);
            f["sub0"]["Height"] = num(layouts[0].Footprint.Height);
            f["last"]["Offset"] = size(layouts[sub - 1].Offset);
            f["last"]["RowPitch"] = num(layouts[sub - 1].Footprint.RowPitch);
        }
    }
}

void device_properties(ID3D12Device* d, Json& out) {
    out["GetNodeCount"] = num(d->GetNodeCount());
    const struct { D3D12_DESCRIPTOR_HEAP_TYPE type; const char* name; } heaps[] = {
        {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, "CBV_SRV_UAV"}, {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, "SAMPLER"},
        {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, "RTV"}, {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, "DSV"}};
    for (const auto& h : heaps) out["GetDescriptorHandleIncrementSize"][h.name] = num(d->GetDescriptorHandleIncrementSize(h.type));

    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    const bool gpu_upload = SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16)) &&
        o16.GPUUploadHeapSupported;
    const struct { D3D12_HEAP_TYPE type; const char* name; } types[] = {
        {D3D12_HEAP_TYPE_DEFAULT, "DEFAULT"}, {D3D12_HEAP_TYPE_UPLOAD, "UPLOAD"},
        {D3D12_HEAP_TYPE_READBACK, "READBACK"}, {D3D12_HEAP_TYPE_GPU_UPLOAD, "GPU_UPLOAD"}};
    for (const auto& t : types) {
        Json& j = out["GetCustomHeapProperties"][t.name];
        if (t.type == D3D12_HEAP_TYPE_GPU_UPLOAD && !gpu_upload) { j = str("skipped: GPUUploadHeapSupported is false"); continue; }
        D3D12_HEAP_PROPERTIES p = d->GetCustomHeapProperties(0, t.type);
        j["Type"] = num(p.Type);
        j["CPUPageProperty"] = num(p.CPUPageProperty);
        j["MemoryPoolPreference"] = num(p.MemoryPoolPreference);
        j["CreationNodeMask"] = num(p.CreationNodeMask);
        j["VisibleNodeMask"] = num(p.VisibleNodeMask);
    }

    const struct { const IID& iid; const char* name; } ifaces[] = {
        {__uuidof(ID3D12Device1), "ID3D12Device1"}, {__uuidof(ID3D12Device2), "ID3D12Device2"},
        {__uuidof(ID3D12Device3), "ID3D12Device3"}, {__uuidof(ID3D12Device4), "ID3D12Device4"},
        {__uuidof(ID3D12Device5), "ID3D12Device5"}, {__uuidof(ID3D12Device6), "ID3D12Device6"},
        {__uuidof(ID3D12Device7), "ID3D12Device7"}, {__uuidof(ID3D12Device8), "ID3D12Device8"},
        {__uuidof(ID3D12Device9), "ID3D12Device9"}, {__uuidof(ID3D12Device10), "ID3D12Device10"},
        {__uuidof(ID3D12Device11), "ID3D12Device11"}, {__uuidof(ID3D12Device12), "ID3D12Device12"},
        {__uuidof(ID3D12Device13), "ID3D12Device13"}, {__uuidof(ID3D12Device14), "ID3D12Device14"},
        {__uuidof(ID3D12DeviceConfiguration), "ID3D12DeviceConfiguration"},
        {__uuidof(ID3D12DeviceConfiguration1), "ID3D12DeviceConfiguration1"},
    };
    for (const auto& i : ifaces) {
        ComPtr<IUnknown> unk;
        out["QueryInterface"][i.name] = hresult(d->QueryInterface(i.iid, reinterpret_cast<void**>(unk.GetAddressOf())));
    }

    ComPtr<ID3D12DeviceConfiguration> config;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&config)))) {
        D3D12_DEVICE_CONFIGURATION_DESC desc = config->GetDesc();
        out["DeviceConfiguration"]["SDKVersion"] = num(desc.SDKVersion);
        out["DeviceConfiguration"]["Flags"] = hex(static_cast<UINT>(desc.Flags));
        out["DeviceConfiguration"]["GpuBasedValidationFlags"] = hex(static_cast<UINT>(desc.GpuBasedValidationFlags));
    }

    ComPtr<ID3D12Device5> d5;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5)))) {
        UINT count = 0;
        HRESULT hr = d5->EnumerateMetaCommands(&count, nullptr);
        Json& j = out["EnumerateMetaCommands"];
        j["hr"] = hresult(hr);
        j["count"] = num(count);
        if (SUCCEEDED(hr) && count && count < 256) {
            std::vector<D3D12_META_COMMAND_DESC> descs(count);
            hr = d5->EnumerateMetaCommands(&count, descs.data());
            j["hr_list"] = hresult(hr);
            if (SUCCEEDED(hr)) for (UINT i = 0; i < count; i++) {
                Json& m = j["commands"][guid(descs[i].Id)];
                m["Name"] = str(utf8(descs[i].Name));
                m["InitializationDirtyState"] = hex(static_cast<UINT>(descs[i].InitializationDirtyState));
                m["ExecutionDirtyState"] = hex(static_cast<UINT>(descs[i].ExecutionDirtyState));
            }
        }
    }

    // One queue of each common type, only for its timestamp frequency; nothing is recorded or submitted.
    const struct { D3D12_COMMAND_LIST_TYPE type; const char* name; } queues[] = {
        {D3D12_COMMAND_LIST_TYPE_DIRECT, "DIRECT"}, {D3D12_COMMAND_LIST_TYPE_COMPUTE, "COMPUTE"},
        {D3D12_COMMAND_LIST_TYPE_COPY, "COPY"}};
    for (const auto& q : queues) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = q.type;
        ComPtr<ID3D12CommandQueue> queue;
        Json& j = out["CommandQueue"][q.name];
        HRESULT hr = d->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        j["CreateCommandQueue"] = hresult(hr);
        if (SUCCEEDED(hr)) {
            UINT64 freq = 0;
            hr = queue->GetTimestampFrequency(&freq);
            j["GetTimestampFrequency"]["hr"] = hresult(hr);
            if (SUCCEEDED(hr)) j["GetTimestampFrequency"]["value"] = num(freq);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
void adapter_desc(IDXGIAdapter1* a, Json& j) {
    DXGI_ADAPTER_DESC1 d{};
    HRESULT hr = a->GetDesc1(&d);
    j["GetDesc1"]["hr"] = hresult(hr);
    if (SUCCEEDED(hr)) {
        Json& o = j["GetDesc1"];
        o["Description"] = str(utf8(d.Description));
        o["VendorId"] = hex(d.VendorId);
        o["DeviceId"] = hex(d.DeviceId);
        o["SubSysId"] = hex(d.SubSysId);
        o["Revision"] = hex(d.Revision);
        o["DedicatedVideoMemory"] = num(d.DedicatedVideoMemory);
        o["DedicatedSystemMemory"] = num(d.DedicatedSystemMemory);
        o["SharedSystemMemory"] = num(d.SharedSystemMemory);
        o["Flags"] = hex(d.Flags);
    }
    ComPtr<IDXGIAdapter4> a4;
    hr = a->QueryInterface(IID_PPV_ARGS(&a4));
    j["IDXGIAdapter4"] = hresult(hr);
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC3 d3{};
        hr = a4->GetDesc3(&d3);
        j["GetDesc3"]["hr"] = hresult(hr);
        if (SUCCEEDED(hr)) {
            j["GetDesc3"]["Flags"] = hex(static_cast<UINT>(d3.Flags));
            j["GetDesc3"]["FlagNames"] = flags(static_cast<UINT>(d3.Flags), kAdapterFlags3);
            j["GetDesc3"]["GraphicsPreemptionGranularity"] = num(d3.GraphicsPreemptionGranularity);
            j["GetDesc3"]["ComputePreemptionGranularity"] = num(d3.ComputePreemptionGranularity);
        }
    }
    // The user-mode driver version DXGI reports (the D3D10/11 UMD version for IDXGIDevice).
    LARGE_INTEGER umd{};
    hr = a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    j["CheckInterfaceSupport_IDXGIDevice"]["hr"] = hresult(hr);
    if (SUCCEEDED(hr)) {
        char b[48];
        snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        j["CheckInterfaceSupport_IDXGIDevice"]["UMDVersion"] = str(b);
    }
}

void outputs(IDXGIAdapter1* a, Json& out) {
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIOutput> o;
        HRESULT hr = a->EnumOutputs(i, &o);
        if (hr == DXGI_ERROR_NOT_FOUND) { out["count"] = num(i); break; }
        Json& j = out[key(i)];
        j["EnumOutputs"] = hresult(hr);
        if (FAILED(hr)) { out["count"] = num(i); out["enum_error"] = hresult(hr); break; }
        ComPtr<IDXGIOutput6> o6;
        hr = o.As(&o6);
        j["IDXGIOutput6"] = hresult(hr);
        if (SUCCEEDED(hr)) {
            DXGI_OUTPUT_DESC1 d{};
            hr = o6->GetDesc1(&d);
            Json& g = j["GetDesc1"];
            g["hr"] = hresult(hr);
            if (SUCCEEDED(hr)) {
                g["DesktopCoordinates"] = str(std::to_string(d.DesktopCoordinates.left) + "," +
                    std::to_string(d.DesktopCoordinates.top) + "," + std::to_string(d.DesktopCoordinates.right) + "," +
                    std::to_string(d.DesktopCoordinates.bottom));
                g["AttachedToDesktop"] = jbool(d.AttachedToDesktop);
                g["Rotation"] = num(d.Rotation);
                g["BitsPerColor"] = num(d.BitsPerColor);
                g["ColorSpace"] = num(d.ColorSpace);
                auto pair = [](const FLOAT (&v)[2]) { Json p; p["x"] = real(v[0]); p["y"] = real(v[1]); return p; };
                g["RedPrimary"] = pair(d.RedPrimary);
                g["GreenPrimary"] = pair(d.GreenPrimary);
                g["BluePrimary"] = pair(d.BluePrimary);
                g["WhitePoint"] = pair(d.WhitePoint);
                g["MinLuminance"] = real(d.MinLuminance);
                g["MaxLuminance"] = real(d.MaxLuminance);
                g["MaxFullFrameLuminance"] = real(d.MaxFullFrameLuminance);
            }
            UINT hw = 0;
            hr = o6->CheckHardwareCompositionSupport(&hw);
            j["CheckHardwareCompositionSupport"]["hr"] = hresult(hr);
            if (SUCCEEDED(hr)) j["CheckHardwareCompositionSupport"]["flags"] = hex(hw);
        }
        const DXGI_FORMAT mode_formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
        for (auto f : mode_formats) {
            Json& m = j["GetDisplayModeList"][format_name(f)];
            UINT count = 0;
            hr = o->GetDisplayModeList(f, 0, &count, nullptr);
            m["hr"] = hresult(hr);
            m["count"] = num(count);
            if (SUCCEEDED(hr) && count) {
                std::vector<DXGI_MODE_DESC> modes(count);
                hr = o->GetDisplayModeList(f, 0, &count, modes.data());
                m["hr_list"] = hresult(hr);
                if (SUCCEEDED(hr) && count) {
                    const DXGI_MODE_DESC* best = &modes[0];
                    for (UINT k = 0; k < count; k++) {
                        const auto& x = modes[k];
                        const double r = x.RefreshRate.Denominator ? double(x.RefreshRate.Numerator) / x.RefreshRate.Denominator : 0;
                        const double rb = best->RefreshRate.Denominator ? double(best->RefreshRate.Numerator) / best->RefreshRate.Denominator : 0;
                        if (UINT64(x.Width) * x.Height > UINT64(best->Width) * best->Height ||
                            (UINT64(x.Width) * x.Height == UINT64(best->Width) * best->Height && r > rb)) best = &x;
                    }
                    m["largest"]["Width"] = num(best->Width);
                    m["largest"]["Height"] = num(best->Height);
                    m["largest"]["RefreshRate"] = str(std::to_string(best->RefreshRate.Numerator) + "/" +
                        std::to_string(best->RefreshRate.Denominator));
                    m["largest"]["ScanlineOrdering"] = num(best->ScanlineOrdering);
                    m["largest"]["Scaling"] = num(best->Scaling);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
std::wstring lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
std::wstring dir_of(const std::wstring& path) { auto p = path.find_last_of(L"\\/"); return p == std::wstring::npos ? L"" : path.substr(0, p); }

std::set<HMODULE> module_set() {
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) return {};
    mods.resize(needed / sizeof(HMODULE) < mods.size() ? needed / sizeof(HMODULE) : mods.size());
    return {mods.begin(), mods.end()};
}

// Modules loaded since start, by base name: where they come from (relative to the executable, System32 or a
// DriverStore package) and their file version. Enough to tell which runtime, core and driver answered.
void modules(const std::set<HMODULE>& before, Json& out) {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    const std::wstring exe_dir = lower(dir_of(buf));
    GetSystemDirectoryW(buf, ARRAYSIZE(buf));
    const std::wstring sys_dir = lower(buf);
    for (HMODULE m : module_set()) {
        if (before.count(m)) continue;
        if (!GetModuleFileNameW(m, buf, ARRAYSIZE(buf))) continue;
        const std::wstring path = buf;
        const std::wstring lpath = lower(path);
        const std::wstring dir = dir_of(lpath);
        const std::wstring base = lpath.substr(dir.size() + 1);
        std::string location;
        const size_t ds = lpath.find(L"\\driverstore\\filerepository\\");
        if (dir == exe_dir) location = "exe_dir";
        else if (dir.rfind(exe_dir + L"\\", 0) == 0) location = "exe_dir" + utf8(dir.substr(exe_dir.size()).c_str());
        else if (dir == sys_dir) location = "system32";
        else if (ds != std::wstring::npos) {
            std::wstring rest = lpath.substr(ds + wcslen(L"\\driverstore\\filerepository\\"));
            location = "driver_store\\" + utf8(rest.substr(0, rest.find(L'\\')).c_str());
        } else location = "other";
        Json& j = out[utf8(base.c_str())];
        j["location"] = str(location);
        DWORD handle = 0;
        DWORD len = GetFileVersionInfoSizeW(path.c_str(), &handle);
        std::vector<unsigned char> info(len ? len : 1);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT flen = 0;
        if (len && GetFileVersionInfoW(path.c_str(), 0, len, info.data()) &&
            VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &flen) && fixed) {
            char b[48];
            snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
                HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
            j["file_version"] = str(b);
        } else j["file_version"] = null_value();
    }
}

bool write_durably(const wchar_t* path, const std::string& text, std::string& why) {
    std::wstring tmp = std::wstring(path) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { why = "cannot create " + utf8(tmp.c_str()) + " (" + std::to_string(GetLastError()) + ")"; return false; }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
        written == text.size() && FlushFileBuffers(h);
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) { why = "write failed (" + std::to_string(err) + ")"; DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        why = "rename failed (" + std::to_string(GetLastError()) + ")"; return false;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    UINT index = 0;
    const wchar_t* out_path = nullptr;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            puts("amdgpu_wddm_d3d12caps [adapter-index] [output-path] [--progress]\n"
                 "Writes one JSON document with the D3D12 and DXGI capabilities of the adapter (default 0)\n"
                 "to stdout or durably to output-path. --progress names each section on stderr.");
            return 0;
        }
        if (!wcscmp(argv[i], L"--progress")) { g_progress = true; continue; }
        if (positional == 0) {
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(argv[i], &end, 10);
            if (!*argv[i] || *end || v > 64) { fprintf(stderr, "d3d12caps: invalid adapter index\n"); return 2; }
            index = static_cast<UINT>(v);
        } else if (positional == 1) out_path = argv[i];
        else { fprintf(stderr, "d3d12caps: too many arguments\n"); return 2; }
        positional++;
    }

    const std::set<HMODULE> before = module_set();
    Json doc;
    doc["schema"] = num(1);
    doc["tool"]["headers"] = str("Windows SDK 10.0.26100 (D3D12_FEATURE_D3D12_OPTIONS21 newest)");
#ifdef CAPS_AGILITY_SDK_VERSION
    doc["tool"]["agility_sdk_version"] = num(CAPS_AGILITY_SDK_VERSION);
    doc["tool"]["agility_sdk_path"] = str(".\\D3D12_0\\");
#else
    doc["tool"]["agility_sdk_version"] = null_value();
#endif
    doc["tool"]["adapter_index"] = num(index);

    // Default search order on purpose: an application-local d3d12.dll / dxgi.dll (the per-application route) wins
    // over System32, exactly as it does for the game.
    progress("dxgi");
    Json& dx = doc["dxgi"];
    ComPtr<IDXGIFactory1> factory;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    dx["LoadLibrary"] = hresult(dxgi ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    if (dxgi) {
        using Create2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
        using Create1 = HRESULT(WINAPI*)(REFIID, void**);
        auto c2 = reinterpret_cast<Create2>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
        auto c1 = reinterpret_cast<Create1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
        HRESULT hr = c2 ? c2(0, IID_PPV_ARGS(&factory)) : c1 ? c1(IID_PPV_ARGS(&factory)) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        dx["CreateDXGIFactory"]["entry"] = str(c2 ? "CreateDXGIFactory2" : c1 ? "CreateDXGIFactory1" : "none");
        dx["CreateDXGIFactory"]["hr"] = hresult(hr);
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (factory) {
        ComPtr<IDXGIFactory5> f5;
        if (SUCCEEDED(factory.As(&f5))) {
            BOOL tearing = FALSE;
            HRESULT hr = f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
            dx["CheckFeatureSupport_PRESENT_ALLOW_TEARING"]["hr"] = hresult(hr);
            if (SUCCEEDED(hr)) dx["CheckFeatureSupport_PRESENT_ALLOW_TEARING"]["value"] = jbool(tearing);
        } else dx["CheckFeatureSupport_PRESENT_ALLOW_TEARING"]["hr"] = str("no IDXGIFactory5");
        for (UINT i = 0;; i++) {
            ComPtr<IDXGIAdapter1> a;
            HRESULT hr = factory->EnumAdapters1(i, &a);
            if (hr == DXGI_ERROR_NOT_FOUND) { dx["adapter_count"] = num(i); break; }
            if (FAILED(hr)) { dx["adapter_count"] = num(i); dx["EnumAdapters1_error"] = hresult(hr); break; }
            adapter_desc(a.Get(), dx["adapters"][key(i)]);
            if (i == index) adapter = a;
        }
    }

    Json& sel = doc["adapter"];
    if (!adapter) sel["error"] = str("adapter index not present");
    else {
        progress("adapter");
        ComPtr<IDXGIAdapter3> a3;
        HRESULT hr = adapter.As(&a3);
        sel["IDXGIAdapter3"] = hresult(hr);
        if (SUCCEEDED(hr)) {
            for (auto [group, name] : {std::pair{DXGI_MEMORY_SEGMENT_GROUP_LOCAL, "LOCAL"}, std::pair{DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, "NON_LOCAL"}}) {
                DXGI_QUERY_VIDEO_MEMORY_INFO m{};
                hr = a3->QueryVideoMemoryInfo(0, group, &m);
                Json& j = sel["QueryVideoMemoryInfo"][name];
                j["hr"] = hresult(hr);
                if (SUCCEEDED(hr)) {
                    j["Budget"] = num(m.Budget);
                    j["CurrentUsage"] = num(m.CurrentUsage);
                    j["AvailableForReservation"] = num(m.AvailableForReservation);
                    j["CurrentReservation"] = num(m.CurrentReservation);
                }
            }
        }
        progress("outputs");
        outputs(adapter.Get(), sel["outputs"]);
    }

    Json& dev = doc["device"];
    ComPtr<ID3D12Device> device;
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    dev["LoadLibrary"] = hresult(d3d12 ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"))) : nullptr;
    if (d3d12 && !create) dev["D3D12CreateDevice_export"] = str("missing");
    if (create && adapter) {
        progress("device");
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        for (auto fl : levels) {
            HRESULT hr = create(adapter.Get(), fl, IID_PPV_ARGS(&device));
            dev["D3D12CreateDevice"][feature_level_name(fl)] = hresult(hr);
            if (SUCCEEDED(hr)) { dev["created_at"] = str(feature_level_name(fl)); break; }
        }
        if (!device) dev["created_at"] = null_value();
    }
    if (device) {
        progress("features");
        features(device.Get(), dev["features"]);
        progress("raw features");
        raw_features(device.Get(), dev["raw_features"]);
        progress("formats");
        std::map<unsigned, UINT8> planes;
        formats(device.Get(), dev["formats"], planes);
        progress("allocations");
        allocations(device.Get(), dev["allocations"], planes);
        progress("device properties");
        device_properties(device.Get(), dev["properties"]);
        dev["GetDeviceRemovedReason"] = hresult(device->GetDeviceRemovedReason());
    }

    progress("modules");
    modules(before, doc["modules"]);

    std::string text;
    serialize(text, doc, 0);
    text += '\n';
    progress("write");
    if (out_path) {
        std::string why;
        if (!write_durably(out_path, text, why)) { fprintf(stderr, "d3d12caps: %s\n", why.c_str()); return 1; }
        return 0;
    }
    _setmode(_fileno(stdout), _O_BINARY);
    if (fwrite(text.data(), 1, text.size(), stdout) != text.size() || fflush(stdout)) {
        fprintf(stderr, "d3d12caps: stdout write failed\n");
        return 1;
    }
    return 0;
}
