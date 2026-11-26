// SPDX-License-Identifier: MIT
// The record pair test: the producer code of every shared-surface record against the consumer code that reads it,
// with the preconditions that follow the decode on that path, over a sweep of sizes, formats and bind flags. A
// refusal of a valid producer result is a defect. driver/contract/record-pairs.json lists every producer and every
// consumer, and for every pair the row of this test that covers it or the reason why no caller reaches it.
//
// Real code: Bc250StdAllocDecide, Bc250Lb7aAdmit and WddmGdiRecordPolicy (the kernel driver), Bc250SharedSurfaceEncode
// and the Bc250SharedSurfaceDecode* family (the contract), Bc250Nv12PrivateValid. A consumer rule that lives in a
// file that needs the API headers or the engine is stated again here, next to a RULE line:
//     // RULE <file> "<text>"
// tools/quality/record_pairs.py fails when <text> is no longer in <file>, so a restated rule cannot drift away from
// the rule it states without the gate seeing it.
//
// Output: one line a pair, tab-separated: status, pair, shapes tried, shapes refused, the smallest refused shape and
// why, and the note of a NOTE pair. Status FAIL is a pair with a refusal; NOTE is a pair whose refusals do not fail
// (by design, or reachability unknown); ok is a pair with no refusal. record_pairs.py --results compares the FAIL
// rows with the expected-red list of the registry. The exit code is 1 when any row is FAIL.
//
// With RECORD_PAIR_IMPORT_MAPPED the D3D11 import names the page-rounded mapped range (BD-412); run_record_pairs.ps1
// defines it when the tree's runtime-surface.cpp does that.

// The admission counters are evidence only; C++20 deprecates ++ on a volatile object.
#define BC250_ADMISSION_COUNT(p) ((void)(*(p) = *(p) + 1))
#include "driver/kmd/gdi_admission.h"
#include "driver/contract/bc250_shared_surface.h"
#include "driver/contract/bc250_umd_submit.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
// t39/cross-adapter-tier1: the answer of the D3D12 shell to a CROSS_ADAPTER create (sweep_cross_adapter).
#if __has_include("driver/umd/d3d12/engine-ddi/cross-adapter.h")
#include "driver/umd/d3d12/engine-ddi/cross-adapter.h"
#define RECORD_PAIR_CROSS_ADAPTER 1
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr uint64_t kPage = 4096;
uint64_t round_page(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }
uint64_t align4(uint64_t v) { return (v + 3) & ~3ull; }

// The row pitch of the engine's LINEAR image (RADV on GFX10): the row rounded up to 256 bytes for every pixel size of
// the table. Measured on unit A: 2816 for 656x519 and 5888 for 1440x843 at 4 bytes (BD-412, BD-409).
// RULE driver/umd/dxvk/runtime-surface-format.h "return (width*bytesPerPixel+255)&~255u;"
uint64_t radv_pitch(uint64_t width, uint64_t bpp) { return (width * bpp + 255) & ~255ull; }
// VkMemoryRequirements.size of that image, one mip, one layer: pitch * height. Inferred, not measured.
uint64_t radv_memory(uint64_t width, uint64_t height, uint64_t bpp) { return radv_pitch(width, bpp) * height; }

// The hosted winsys import: va and size whole pages, and radv_AllocateMemory refuses a host size below
// allocationSize.
// RULE driver/icd/mesa-wddm2-runtime-queues/0007-host-import-cpu-map.patch "(va & 4095) || (size & 4095)"
// RULE driver/icd/mesa-wddm2-runtime-queues/0007-host-import-cpu-map.patch "host_import->size<pAllocateInfo->allocationSize"
bool winsys_import(uint64_t size, uint64_t allocation_size) { return size && !(size & 4095) && size >= allocation_size; }

struct Shape { unsigned w, h; };
struct Result {
    unsigned long long tried = 0, refused = 0;
    std::string first;  // the smallest refused shape (by w*h)
    unsigned long long first_area = ~0ull;
    void ok() { ++tried; }
    void fail(unsigned w, unsigned h, const std::string &what) {
        ++tried; ++refused;
        const unsigned long long area = 1ull * w * h;
        if (area < first_area) {
            first_area = area;
            char t[96]; std::snprintf(t, sizeof t, "%ux%u: ", w, h);
            first = t + what;
        }
    }
};
std::map<std::string, Result> g_results;
Result &pair(const std::string &name) { return g_results[name]; }
// Pairs whose refusals print but do not fail, with the reason:
//   "by design"            the consumer refuses the record on purpose; the row shows that the refusal still holds,
//                          and an admission fails the row (a change of contract).
//   "reachability unknown" the producer result is valid, but no lab line shows that a caller makes it.
std::map<std::string, std::string> g_note;
void note(const std::string &name, const std::string &why) { g_note[name] = why; }

std::vector<Shape> shapes() {
    static const unsigned heights[] = {1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255, 256, 257,
                                       441, 519, 843, 1080, 1081, 2160, 4096};
    std::vector<Shape> s;
    for (unsigned w = 1; w <= 4096; ++w)
        for (unsigned h : heights) s.push_back({w, h});
    for (unsigned w : {8192u, 8193u, 16383u, 16384u})
        for (unsigned h : {1u, 7u, 16384u}) s.push_back({w, h});
    // The lab's shapes: FlipQueue (BD-408), DXGIFrameStats (BD-412), dxvp (BD-409).
    for (Shape l : {Shape{624, 441}, Shape{656, 519}, Shape{1440, 843}, Shape{16, 16}}) s.push_back(l);
    return s;
}

// The D3D11 OpenResource of an allocation with no resource record.
// RULE driver/umd/dxvk/ddi-resource.cpp "decode_bare_allocation"
int d3d11_bare(const void *blob, unsigned bytes, BC250_SHARED_SURFACE *out, const char **why) {
#if defined(BC250_SHARED_SURFACE_WHY_BARE_LENGTH)
    return Bc250SharedSurfaceDecodeBareWhy(blob, bytes, out, why);
#else
    // The rule of ddi-resource.cpp decode_bare_allocation on a tree that has no contract form of it.
    BC250_GDI_PRIVATE g{};
    *why = "bare";
    if (bytes == sizeof(BC250_GDI_PRIVATE)) {
        std::memcpy(&g, blob, sizeof g);
        if (g.Magic != BC250_GDI_PRIVATE_MAGIC || g.Type != 1 || g.Flags || g.Reserved) { *why = "bare type"; return 1; }
    } else if (bytes == sizeof(BC250_WDDM_ALLOCATION_PRIVATE)) std::memcpy(&g.Surface, blob, sizeof g.Surface);
    else { *why = "bare length"; return 1; }
    const auto &s = g.Surface;
    if (s.Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || s.Version != 1) { *why = "bare record"; return 1; }
    const auto *row = amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_d3dddi(unsigned(s.Format)),
                                                AMDGPU_WDDM_SURFACE_COMPOSED);
    if (!row || !row->dxgi) { *why = "bare format (E_NOTIMPL)"; return 2; }
    if (!s.Width || !s.Height || s.Width > BC250_SHARED_MAX_EDGE || s.Height > BC250_SHARED_MAX_EDGE ||
        !Bc250SharedSurfaceGeometry(&s, row->bytes_per_pixel)) { *why = "bare geometry"; return 1; }
    out->Width = s.Width; out->Height = s.Height; out->Pitch = s.Pitch; out->DxgiFormat = row->dxgi;
    out->D3dDdiFormat = s.Format; out->BytesPerPixel = row->bytes_per_pixel; out->Size = s.Size;
    out->BindFlags = BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET; out->MiscFlags = 0;
    out->Shared = 1; out->Access = 0;
    *why = "record";
    return 0;
#endif
}

// What the D3D11 shell does after a successful decode, up to the import: the import size is the record's size, or
// on a tree with BD-412 the page-rounded mapped range; then the row and import checks of the image memory, the
// engine row pitch against the record's, and the winsys import.
// RULE driver/umd/dxvk/runtime-surface.cpp "imported.size=surface."
// RULE driver/umd/dxvk/runtime-image-memory.cpp "if (rowBytes>source.size || VkDeviceSize(info.extent.height-1)>(source.size-rowBytes)/pitch) return E_INVALIDARG;"
// RULE driver/umd/dxvk/runtime-image-memory.cpp "if (!layout.offset && layout.rowPitch==pitch && layout.size && layout.size<=source.size &&"
// RULE driver/umd/dxvk/runtime-image-memory.cpp "if (!requirements.size || requirements.size>source.size || !requirements.alignment ||"
// The adoption takes the surface table row of the record's two formats; the D3D11 table has no X view rule.
// RULE driver/umd/dxvk/runtime-surface.cpp "const auto *row=runtime_surface_format(metadata.Format,desc.Format);"
std::string d3d11_after_decode(const BC250_SHARED_SURFACE &s) {
    const AMDGPU_WDDM_SURFACE_FORMAT *format = Bc250SharedSurfaceFormat(s.DxgiFormat);
    if (!format || format->d3dddi != (unsigned int)s.D3dDdiFormat) return "no D3D11 surface row for the formats";
#if defined(RECORD_PAIR_IMPORT_MAPPED)
    const uint64_t source = round_page(s.Size);
#else
    const uint64_t source = s.Size;
#endif
    const uint64_t row = 1ull * s.Width * s.BytesPerPixel;
    if (row > source || uint64_t(s.Height - 1) > (source - row) / s.Pitch) return "import rows do not fit";
    if (radv_pitch(s.Width, s.BytesPerPixel) != s.Pitch) return "engine row pitch != record pitch";
    const uint64_t mem = radv_memory(s.Width, s.Height, s.BytesPerPixel);
    if (mem > source) return "requirements.size > import size";
    if (!winsys_import(source, mem)) return "winsys import: size not whole pages";
    return "";
}

// What the D3D12 shell does after a successful decode: backing = round_page(max(RowPitch * align4(H), MemorySize)),
// RowPitch == Pitch and backing <= Size (a record with no resource record compares with round_page(Size) on a tree
// that has the bare decode), and the adopt refuses a byte size that is not whole pages.
// RULE driver/umd/d3d12/engine-ddi/resources.cpp "const uint64_t need = std::max<uint64_t>(rows, info.MemorySize);"
// RULE driver/umd/d3d12/engine-ddi/resources.cpp "out->backing_size = (need + kLinearPage - 1) & ~(kLinearPage - 1);"
// RULE driver/umd/d3d12/engine-ddi/resources.cpp "if (surface.info.RowPitch != shared.Pitch || surface.backing_size > shared"
// RULE driver/umd/d3d12/heap-import.cpp "(request->alignment&(request->alignment-1)) || (request->byte_size&4095)"
std::string d3d12_after_decode(const BC250_SHARED_SURFACE &s, bool bare) {
    const uint64_t pitch = radv_pitch(s.Width, s.BytesPerPixel);
    const uint64_t backing = round_page(std::max<uint64_t>(pitch * align4(s.Height),
                                                           radv_memory(s.Width, s.Height, s.BytesPerPixel)));
#if defined(BC250_SHARED_SURFACE_WHY_BARE_LENGTH)
    const uint64_t allocated = bare ? round_page(s.Size) : s.Size;
#else
    (void)bare;
    const uint64_t allocated = s.Size;
#endif
    if (pitch != s.Pitch) return "layout agreement: pitch";
    if (backing > allocated) return "layout agreement: backing > size";
    return "";
}

// The kernel driver's CreateAllocation and OpenAllocation admission of an LB7A blob with its resource record.
std::string kmd_admits(const void *alloc, unsigned alloc_bytes, const void *record, unsigned record_bytes, int open) {
    BC250_GDI_ALLOCATION_POLICY placement{}, policy{};
    unsigned long type = 0;
    if (!WddmGdiRecordPolicy(record, record_bytes, &placement)) return "resource record refused";
    int shared = 0, cached = 0;
    (void)Bc250SurfaceResourcePolicy(record, record_bytes, &shared, &cached);
    const int r = Bc250Lb7aAdmit(alloc, alloc_bytes, open ? 0 : shared, open ? 0 : cached, 1, &type, &policy);
    if (r == BC250_LB7A_UNREAD) return "LB7A unread";
    if (r == BC250_LB7A_REFUSED) return "LB7A refused";
    return "";
}

void record(const std::string &name, unsigned w, unsigned h, const std::string &why) {
    why.empty() ? pair(name).ok() : pair(name).fail(w, h, why);
}

// ---- P1: the kernel driver's standard allocations ----
void sweep_kmd_standard(const std::vector<Shape> &all) {
    static const unsigned long gdi_formats[] = {AMDGPU_WDDM_D3DDDI_A8R8G8B8, AMDGPU_WDDM_D3DDDI_X8R8G8B8,
                                                AMDGPU_WDDM_D3DDDI_A8B8G8R8, BC250_FORMAT_X8B8G8R8,
                                                AMDGPU_WDDM_D3DDDI_A8};
    char name[128];
    // F2: an X8 GDI texture (22, 33) has no COMPOSED row, so both bare opens refuse it. Whether dxgkrnl makes a
    // type-1 X8 surface that a user-mode driver then opens is not measured.
    for (unsigned long fmt : {(unsigned long)AMDGPU_WDDM_D3DDDI_X8R8G8B8, (unsigned long)BC250_FORMAT_X8B8G8R8})
        for (const char *api : {"D3D11", "D3D12"}) {
            std::snprintf(name, sizeof name, "P1 GDI type 1 fmt %lu -> %s open", fmt, api);
            note(name, "reachability unknown (F2)");
        }
    for (const Shape &sh : all) {
        for (unsigned long type = 1; type <= 4; ++type)
            for (unsigned long fmt : gdi_formats) {
                BC250_STDALLOC_REQUEST r{}; BC250_STDALLOC_ANSWER a{};
                r.Kind = BC250_STDALLOC_GDI; r.Described = 1; r.Width = sh.w; r.Height = sh.h; r.Format = fmt;
                r.GdiType = type; r.Fill = 1; r.Bytes = 48; r.OsBuild = 26100;
                if (Bc250StdAllocDecide(&r, &a) != BC250_STDALLOC_OK) continue;  // not a producer result
                BC250_GDI_PRIVATE g{}; g.Surface = a.Surface; g.Magic = BC250_GDI_PRIVATE_MAGIC; g.Type = type;
                for (int open = 0; open < 2; ++open) {
                    std::snprintf(name, sizeof name, "P1 GDI type %lu fmt %lu -> KMD %s", type, fmt, open ? "open" : "create");
                    record(name, sh.w, sh.h, kmd_admits(&g, sizeof g, nullptr, 0, open));
                }
                // The D3D11 OpenResource of the same allocation, no resource record (DWM redirection, BD-253, BD-412).
                std::snprintf(name, sizeof name, "P1 GDI type %lu fmt %lu -> D3D11 open", type, fmt);
                BC250_SHARED_SURFACE s{}; const char *why = "";
                const int d = d3d11_bare(&g, sizeof g, &s, &why);
                if (type != 1) {  // the bare decode takes type 1 only, by design
                    note(name, "by design: a GDI type other than 1 is not a texture");
                    d ? pair(name).fail(sh.w, sh.h, std::string("refused: ") + why) : pair(name).ok();
                    continue;
                }
                if (d) pair(name).fail(sh.w, sh.h, std::string("decode: ") + why);
                else record(name, sh.w, sh.h, d3d11_after_decode(s));
                // The D3D12 OpenHeapAndResource of the same allocation (D3D9On12 Present, BD-408).
                std::snprintf(name, sizeof name, "P1 GDI type 1 fmt %lu -> D3D12 open", fmt);
                BC250_SHARED_SURFACE s12{}; const char *why12 = "";
#if defined(BC250_SHARED_SURFACE_WHY_BARE_LENGTH)
                const int d12 = Bc250SharedSurfaceDecodeBareWhy(&g, sizeof g, &s12, &why12);
#else
                const int d12 = Bc250SharedSurfaceDecodeWhy(nullptr, 0, &g, sizeof g, &s12, &why12);
#endif
                if (d12) pair(name).fail(sh.w, sh.h, std::string("decode: ") + why12);
                else record(name, sh.w, sh.h, d3d12_after_decode(s12, true));
            }
        for (unsigned long kind : {BC250_STDALLOC_PRIMARY, BC250_STDALLOC_SHADOW, BC250_STDALLOC_STAGING})
            for (unsigned long fmt : {AMDGPU_WDDM_D3DDDI_A8R8G8B8, AMDGPU_WDDM_D3DDDI_X8R8G8B8}) {
                BC250_STDALLOC_REQUEST r{}; BC250_STDALLOC_ANSWER a{};
                r.Kind = kind; r.Described = 1; r.Width = sh.w; r.Height = sh.h; r.Format = fmt; r.Fill = 1;
                r.Bytes = 48; r.OsBuild = 26100; r.StagingAperture = 1; r.ResourceFill = 1; r.ResourceBytes = 16;
                if (Bc250StdAllocDecide(&r, &a) != BC250_STDALLOC_OK) continue;
                for (int open = 0; open < 2; ++open) {
                    std::snprintf(name, sizeof name, "P1 kind %lu fmt %lu -> KMD %s", kind, fmt, open ? "open" : "create");
                    record(name, sh.w, sh.h, kmd_admits(&a.Surface, a.PrivateBytes, a.ResourceBytes ? a.Resource : nullptr,
                                                        a.ResourceBytes, open));
                }
            }
    }
}

// The bind sets of a shared texture. DECODER and VIDEO_ENCODER are the D3D11 values (d3d11.h); the D3D11 shell
// passes them from the runtime unchanged.
// RULE driver/umd/dxvk/ddi-resource.cpp "BIND(D3D11_DDI_BIND_DECODER,D3D11_BIND_DECODER);"
// RULE driver/umd/dxvk/ddi-resource.cpp "BIND(D3D11_DDI_BIND_VIDEO_ENCODER,D3D11_BIND_VIDEO_ENCODER);"
constexpr unsigned long kDecoder = 0x200, kVideoEncoder = 0x400;
const unsigned long kBinds[] = {
    BC250_SHARED_BIND_SHADER_RESOURCE, BC250_SHARED_BIND_RENDER_TARGET,
    BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET,
    BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET | BC250_SHARED_BIND_UNORDERED_ACCESS,
    BC250_SHARED_BIND_SHADER_RESOURCE | kDecoder, BC250_SHARED_BIND_SHADER_RESOURCE | kVideoEncoder};
const char *bind_name(unsigned long bind) {
    return (bind & kDecoder) ? " +DECODER" : (bind & kVideoEncoder) ? " +VIDEO_ENCODER" : "";
}

// ---- P5: the D3D12 shared surface, Bc250SharedSurfaceEncode at the engine's linear shape ----
// prepare_shared_surface takes pitch and size from query_linear_surface: pitch = RowPitch, size = backing (whole
// pages); width and height at most 8192.
// RULE driver/umd/d3d12/engine-ddi/resources.cpp "inline constexpr uint32_t kLinearMaxEdge = 8192;"
// ---- P2: the D3D11 shell's shared runtime surface ----
// convert_runtime_resource: pitch align256(W*bpp), size round_page(pitch*align4(H)), and an E26R v3 record with the
// descriptor's bind flags and Access 0 for a plain shared texture. Restated here: the function needs the DDI and
// engine headers.
// RULE driver/umd/dxvk/ddi-resource.cpp "const UINT64 bytes=(UINT64(pitch)*((d.Height+3)&~3u)+4095)&~UINT64(4095);"
// RULE driver/umd/dxvk/ddi-resource.cpp "r.texture={BC250_SURFACE_RESOURCE_MAGIC,BC250_SURFACE_RESOURCE_TEXTURE_VERSION,UINT(r.shared),r.primary ? 1u : 0u,"
// The D3D11 open admits a plain record or one with access bits (decode_open_resource).
// RULE driver/umd/dxvk/ddi-resource.cpp "admit.AccessMask=BC250_SURFACE_RESOURCE_ACCESS_MASK;"
void sweep_shells(const std::vector<Shape> &all) {
    unsigned count = 0;
    const AMDGPU_WDDM_SURFACE_FORMAT *rows = amdgpu_wddm_surface_formats(&count);
    char name[160];
    for (unsigned i = 0; i < count; ++i) {
        const auto &row = rows[i];
        if (!amdgpu_wddm_surface_admit(&row, AMDGPU_WDDM_SURFACE_COMPOSED) || !row.dxgi) continue;
        for (unsigned v = 0; v < 2; ++v) {
            const unsigned dxgi = v ? row.dxgi_srgb : row.dxgi;
            if (!dxgi) continue;
            const char *srgb = v ? " sRGB" : "";
            for (const Shape &sh : all) {
                const uint64_t pitch = radv_pitch(sh.w, row.bytes_per_pixel);
                for (unsigned long bind : kBinds) {
                    // P5: only the binds the D3D12 heap flags give (heap-import.cpp shared_bind).
                    if (sh.w <= 8192 && sh.h <= 8192 && !(bind & (kDecoder | kVideoEncoder))) {
                        BC250_SHARED_SURFACE in{};
                        in.Width = sh.w; in.Height = sh.h; in.Pitch = (unsigned long)pitch; in.DxgiFormat = dxgi;
                        in.D3dDdiFormat = row.d3dddi; in.BindFlags = bind; in.BytesPerPixel = row.bytes_per_pixel;
                        in.Size = round_page(std::max<uint64_t>(pitch * align4(sh.h), radv_memory(sh.w, sh.h, row.bytes_per_pixel)));
                        in.Shared = 1;
                        BC250_WDDM_ALLOCATION_PRIVATE a{}; BC250_SURFACE_RESOURCE_PRIVATE r{};
                        if (Bc250SharedSurfaceEncode(&in, &a, &r) == BC250_SHARED_SURFACE_OK) {
                            std::snprintf(name, sizeof name, "P5 D3D12 shared %s%s -> D3D12 open", row.name, srgb);
                            BC250_SHARED_SURFACE s{}; const char *why = "";
                            if (Bc250SharedSurfaceDecodeWhy(&r, sizeof r, &a, sizeof a, &s, &why)) pair(name).fail(sh.w, sh.h, why);
                            else record(name, sh.w, sh.h, d3d12_after_decode(s, false));
                            std::snprintf(name, sizeof name, "P5 D3D12 shared %s%s -> D3D11 open", row.name, srgb);
                            BC250_SHARED_SURFACE_ADMIT admit{0, BC250_SURFACE_RESOURCE_ACCESS_MASK};
                            if (Bc250SharedSurfaceDecodeAdmitted(&r, sizeof r, &a, sizeof a, &admit, &s)) pair(name).fail(sh.w, sh.h, "decode");
                            else record(name, sh.w, sh.h, d3d11_after_decode(s));
                            std::snprintf(name, sizeof name, "P5 D3D12 shared %s%s -> KMD create+open", row.name, srgb);
                            auto k = kmd_admits(&a, sizeof a, &r, sizeof r, 0);
                            if (k.empty()) k = kmd_admits(&a, sizeof a, &r, sizeof r, 1);
                            record(name, sh.w, sh.h, k);
                        } else {  // the producer refuses its own engine's shape: every opener row of it fails
                            for (const char *to : {"D3D12 open", "D3D11 open", "KMD create+open"}) {
                                std::snprintf(name, sizeof name, "P5 D3D12 shared %s%s -> %s", row.name, srgb, to);
                                pair(name).fail(sh.w, sh.h, "encode refused the engine's shape");
                            }
                        }
                    }
                    if (sh.w > 16384 || sh.h > 16384) continue;
                    BC250_WDDM_ALLOCATION_PRIVATE a{BC250_WDDM_ALLOCATION_PRIVATE_MAGIC, 1, sh.w, sh.h, (unsigned long)pitch,
                                                    row.d3dddi, round_page(pitch * align4(sh.h))};
                    BC250_SURFACE_RESOURCE_PRIVATE r{BC250_SURFACE_RESOURCE_MAGIC, BC250_SURFACE_RESOURCE_TEXTURE_VERSION, 1, 0,
                                                     sh.w, sh.h, 1, 1, dxgi, 1, 0, 0, bind, 0, 0, 0};
                    std::snprintf(name, sizeof name, "P2 D3D11 shared %s%s%s -> D3D11 open", row.name, srgb, bind_name(bind));
                    BC250_SHARED_SURFACE_ADMIT admit{0, BC250_SURFACE_RESOURCE_ACCESS_MASK};
                    BC250_SHARED_SURFACE s{}; const char *why = "";
                    if (Bc250SharedSurfaceDecodeDetail(&r, sizeof r, &a, sizeof a, &admit, &s, &why)) pair(name).fail(sh.w, sh.h, std::string("decode: ") + why);
                    else record(name, sh.w, sh.h, d3d11_after_decode(s));
                    std::snprintf(name, sizeof name, "P2 D3D11 shared %s%s%s -> D3D12 open", row.name, srgb, bind_name(bind));
                    if (Bc250SharedSurfaceDecodeWhy(&r, sizeof r, &a, sizeof a, &s, &why)) pair(name).fail(sh.w, sh.h, std::string("decode: ") + why);
                    else record(name, sh.w, sh.h, d3d12_after_decode(s, false));
                    std::snprintf(name, sizeof name, "P2 D3D11 shared %s%s%s -> KMD create+open", row.name, srgb, bind_name(bind));
                    auto k = kmd_admits(&a, sizeof a, &r, sizeof r, 0);
                    if (k.empty()) k = kmd_admits(&a, sizeof a, &r, sizeof r, 1);
                    record(name, sh.w, sh.h, k);
                }
            }
        }
    }
}

// ---- P5 X views: a D3D12 shared B8G8R8X8 surface (BD-408) ----
// The D3D12 shell writes an X format (88, 93) over the storage row of its A format (87, 91): LB7A A8R8G8B8, E26R the
// X format. The D3D11 open must take it as well: D3D9On12 creates such a surface for a D3D9 X8R8G8B8 texture.
void sweep_x_views(const std::vector<Shape> &all) {
    struct XView { unsigned dxgi, storage; const char *name; };
    static const XView kViews[] = {{88, 87, "B8G8R8X8_UNORM"}, {93, 91, "B8G8R8X8_UNORM_SRGB"}};
    char name[160];
    for (const XView &v : kViews) {
        const AMDGPU_WDDM_SURFACE_FORMAT *row = Bc250SharedSurfaceFormat(v.storage);
        std::snprintf(name, sizeof name, "P5 D3D12 shared %s (%u) -> D3D11 open", v.name, v.dxgi);
        for (const Shape &sh : all) {
            if (sh.w > 8192 || sh.h > 8192) continue;
            const uint64_t pitch = radv_pitch(sh.w, row->bytes_per_pixel);
            BC250_SHARED_SURFACE in{};
            in.Width = sh.w; in.Height = sh.h; in.Pitch = (unsigned long)pitch; in.DxgiFormat = v.dxgi;
            in.D3dDdiFormat = row->d3dddi; in.BytesPerPixel = row->bytes_per_pixel; in.Shared = 1;
            in.BindFlags = BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET;
            in.Size = round_page(std::max<uint64_t>(pitch * align4(sh.h), radv_memory(sh.w, sh.h, row->bytes_per_pixel)));
            BC250_WDDM_ALLOCATION_PRIVATE a{}; BC250_SURFACE_RESOURCE_PRIVATE r{};
            if (Bc250SharedSurfaceEncode(&in, &a, &r) != BC250_SHARED_SURFACE_OK) { pair(name).fail(sh.w, sh.h, "encode refused"); continue; }
            BC250_SHARED_SURFACE_ADMIT admit{0, BC250_SURFACE_RESOURCE_ACCESS_MASK};
            BC250_SHARED_SURFACE s{};
            if (Bc250SharedSurfaceDecodeAdmitted(&r, sizeof r, &a, sizeof a, &admit, &s)) pair(name).fail(sh.w, sh.h, "decode");
            else record(name, sh.w, sh.h, d3d11_after_decode(s));
        }
    }
}

// ---- P2 NV12: the D3D11 shell's shared NV12 surface (a decoder's output) ----
// The shell writes an LB7A v2 record from the planes of the engine's retained image. The plane layout is stated
// here, not measured: luma at offset 0 with pitch align256(W), chroma right after it with the same pitch, the size
// page-rounded. Bc250Nv12PrivateValid is the real record check.
// The record has one writer, in the contract, and the shell states the plane layout in the contract's terms.
// RULE driver/contract/bc250_shared_surface.h "n.Surface.Version = BC250_WDDM_NV12_VERSION;"
// RULE driver/umd/dxvk/runtime-surface.cpp "if (Bc250SharedNv12Encode(&surface,&record)!=BC250_SHARED_SURFACE_OK) return E_INVALIDARG;"
// The D3D11 open of it.
// RULE driver/umd/dxvk/ddi-resource.cpp "if (a.PrivateDriverDataSize==sizeof(BC250_WDDM_NV12_PRIVATE)) {"
// RULE driver/umd/dxvk/ddi-resource.cpp "(r.BindFlags&~BC250_SHARED_BIND_MASK) || (r.MiscFlags&~BC250_SHARED_MISC_MASK) ||"
std::string d3d11_open_nv12(const BC250_WDDM_NV12_PRIVATE &n, const BC250_SURFACE_RESOURCE_PRIVATE &r) {
    int shared = 0, cached = 0;
    if (!Bc250Nv12PrivateValid(&n, sizeof n)) return "LB7A v2 record";
    if (r.Magic != BC250_SURFACE_RESOURCE_MAGIC || r.Version != BC250_SURFACE_RESOURCE_TEXTURE_VERSION ||
        !Bc250SurfaceResourcePolicy(&r, sizeof r, &shared, &cached))
        return "resource record";
    if (r.Width != n.Surface.Width || r.Height != n.Surface.Height || r.Format != BC250_NV12_DXGI_FORMAT ||
        r.MipLevels != 1 || r.ArraySize != 1 || r.SampleCount != 1 || r.SampleQuality || r.Usage || r.CpuAccessFlags ||
        r.TextureLayout)
        return "description";
    if ((r.BindFlags & ~BC250_SHARED_BIND_MASK) || (r.MiscFlags & ~BC250_SHARED_MISC_MASK)) return "bind or misc flags";
    if (r.Access & (BC250_SURFACE_RESOURCE_CPU_READ | BC250_SURFACE_RESOURCE_SCANOUT)) return "access";
    return "";
}
void sweep_nv12(const std::vector<Shape> &all) {
    static const unsigned long binds[] = {BC250_SHARED_BIND_SHADER_RESOURCE, BC250_SHARED_BIND_SHADER_RESOURCE | kDecoder,
                                          BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET | kDecoder};
    char name[160];
    for (const Shape &sh : all) {
        if ((sh.w | sh.h) & 1u || sh.w > 16383 || sh.h > 16383) continue;  // convert_runtime_resource refuses these
        const uint64_t pitch = radv_pitch(sh.w, 1);
        BC250_WDDM_NV12_PRIVATE n{};
        n.Surface = {BC250_WDDM_ALLOCATION_PRIVATE_MAGIC, BC250_WDDM_NV12_VERSION, sh.w, sh.h, (unsigned long)pitch,
                     BC250_NV12_D3DDDI_FORMAT, round_page(pitch * sh.h * 3 / 2)};
        n.ChromaPitch = (unsigned long)pitch; n.Layout = BC250_WDDM_NV12_LINEAR;
        n.SurfaceOffset = 0; n.ChromaOffset = pitch * sh.h; n.Alignment = 4096;
        for (unsigned long bind : binds) {
            const BC250_SURFACE_RESOURCE_PRIVATE r{BC250_SURFACE_RESOURCE_MAGIC, BC250_SURFACE_RESOURCE_TEXTURE_VERSION, 1, 0,
                                                   sh.w, sh.h, 1, 1, BC250_NV12_DXGI_FORMAT, 1, 0, 0, bind, 0, 0, 0};
            std::snprintf(name, sizeof name, "P2 D3D11 shared NV12%s%s -> D3D11 open", bind_name(bind),
                          (bind & BC250_SHARED_BIND_RENDER_TARGET) ? " +RT" : "");
            record(name, sh.w, sh.h, d3d11_open_nv12(n, r));
            // The D3D12 open: the strict decode of the shared surface, which knows the 32-byte LB7A v1 only.
            std::snprintf(name, sizeof name, "P2 D3D11 shared NV12%s%s -> D3D12 open", bind_name(bind),
                          (bind & BC250_SHARED_BIND_RENDER_TARGET) ? " +RT" : "");
            BC250_SHARED_SURFACE s{}; const char *why = "";
            if (Bc250SharedSurfaceDecodeWhy(&r, sizeof r, &n, sizeof n, &s, &why)) pair(name).fail(sh.w, sh.h, std::string("decode: ") + why);
            else pair(name).ok();
            std::snprintf(name, sizeof name, "P2 D3D11 shared NV12%s%s -> KMD create+open", bind_name(bind),
                          (bind & BC250_SHARED_BIND_RENDER_TARGET) ? " +RT" : "");
            auto k = kmd_admits(&n, sizeof n, &r, sizeof r, 0);
            if (k.empty()) k = kmd_admits(&n, sizeof n, &r, sizeof r, 1);
            record(name, sh.w, sh.h, k);
        }
    }
}

// ---- P7: the D3D12 raw allocation, BC2A version 2 with no resource record ----
// A D3D12 create outside the shareable envelope takes this form; the D3D12 DDI does not say that a create is shared,
// so a shared create of such a resource is P7 too (BD-408 case 2). No opener reads it, by design: the record that
// describes the texels is not there.
// RULE driver/umd/d3d12/allocation-request.h "blob.version=BC250_UMD_ALLOC_VERSION_CACHE_POLICY;blob.size=sizeof(blob);"
struct bc250_umd_alloc_private raw_blob(uint64_t bytes) {
    struct bc250_umd_alloc_private b{};
    const uint64_t rounded = (bytes + 65535) & ~65535ull;
    b.magic = BC250_UMD_ALLOC_MAGIC; b.version = BC250_UMD_ALLOC_VERSION_CACHE_POLICY; b.size = sizeof b;
    b.alloc_size = rounded; b.phys_alignment = 65536; b.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
    b.gem_flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS; b.va_size = rounded;
    return b;
}
void sweep_raw(const std::vector<Shape> &all) {
    const std::string n12 = "P7 D3D12 raw BC2A v2, no record -> D3D12 open";
    const std::string n11 = "P7 D3D12 raw BC2A v2, no record -> D3D11 open";
    note(n12, "by design: no resource record"); note(n11, "by design: no resource record");
    // The refusal of an open that no record describes is the one loud answer for a shared create outside the
    // envelope (S2 decision 2026-11-25): it must name its check, never the bare word of a successful decode.
    const std::string loud = "S1 open of a P7 allocation names its refusal";
    const auto named = [](const char *why) {
        return why && *why && std::strcmp(why, BC250_SHARED_SURFACE_WHY_OK) != 0;
    };
    for (const Shape &sh : all) {
        if (sh.w > 8192 || sh.h > 8192) continue;
        const auto b = raw_blob(radv_memory(sh.w, sh.h, 4));
        BC250_SHARED_SURFACE s{}; const char *why = "";
        if (Bc250SharedSurfaceDecodeWhy(nullptr, 0, &b, sizeof b, &s, &why)) {
            pair(n12).fail(sh.w, sh.h, std::string("refused: ") + why);
            named(why) ? pair(loud).ok() : pair(loud).fail(sh.w, sh.h, "D3D12 refusal without a name");
        } else pair(n12).ok();
        why = "";
        if (d3d11_bare(&b, sizeof b, &s, &why)) {
            pair(n11).fail(sh.w, sh.h, std::string("refused: ") + why);
            named(why) ? pair(loud).ok() : pair(loud).fail(sh.w, sh.h, "D3D11 refusal without a name");
        } else pair(n11).ok();
    }
}

// ---- The shared-create rule: every format and shape that a shell accepts for a shared create gives a record that
// an opener reads, or the create is refused. ----
// The formats are the D3D11.1 list (learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-1-features,
// "Extended support for shared Texture2D resources", read 2026-11-25). Tier 1 is shareable on every device (mip 1,
// array 1, SHADER_RESOURCE|RENDER_TARGET, DEFAULT usage). Tier 2 is shareable when ExtendedResourceSharing is TRUE,
// which DXVK reports for shared resource tier 1 and up, with UAV and DECODER binds, mips and arrays.
// The D3D12 producer takes P5 when the description is in the envelope (shared_surface_shape -> linear_surface_shape:
// a COMPOSED row by Bc250SharedSurfaceFormat, TEXTURE2D, edges 1..8192, mip 1, array 1, one sample), otherwise P7.
// RULE driver/umd/d3d12/engine-ddi/resources.cpp "if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality)"
// RULE driver/umd/d3d12/engine-ddi/resources.cpp "return Bc250SharedSurfaceFormat(static_cast<unsigned long>(f));"
// The D3D11 producer sends every shared description outside the LINEAR SURFACE shape to the ENGINE LAYOUT form (P3),
// which both openers read; that pair is the host test of runtime-engine-resource, not this one.
// RULE driver/umd/dxvk/ddi-resource.cpp "(s.Format==DXGI_FORMAT_NV12 || runtime_surface_format(s.Format)!=nullptr);"
struct SharedFormat { unsigned dxgi; const char *name; int tier; };
const SharedFormat kShared[] = {
    {28, "R8G8B8A8_UNORM", 1}, {29, "R8G8B8A8_UNORM_SRGB", 1}, {87, "B8G8R8A8_UNORM", 1},
    {91, "B8G8R8A8_UNORM_SRGB", 1}, {88, "B8G8R8X8_UNORM", 1}, {93, "B8G8R8X8_UNORM_SRGB", 1},
    {24, "R10G10B10A2_UNORM", 1}, {10, "R16G16B16A16_FLOAT", 1},
    {1, "R32G32B32A32_TYPELESS", 2}, {2, "R32G32B32A32_FLOAT", 2}, {3, "R32G32B32A32_UINT", 2},
    {4, "R32G32B32A32_SINT", 2}, {9, "R16G16B16A16_TYPELESS", 2}, {11, "R16G16B16A16_UNORM", 2},
    {12, "R16G16B16A16_UINT", 2}, {13, "R16G16B16A16_SNORM", 2}, {14, "R16G16B16A16_SINT", 2},
    {25, "R10G10B10A2_UINT", 2}, {27, "R8G8B8A8_TYPELESS", 2}, {30, "R8G8B8A8_UINT", 2},
    {31, "R8G8B8A8_SNORM", 2}, {32, "R8G8B8A8_SINT", 2}, {90, "B8G8R8A8_TYPELESS", 2},
    {92, "B8G8R8X8_TYPELESS", 2}, {39, "R32_TYPELESS", 2}, {41, "R32_FLOAT", 2}, {42, "R32_UINT", 2},
    {43, "R32_SINT", 2}, {53, "R16_TYPELESS", 2}, {54, "R16_FLOAT", 2}, {56, "R16_UNORM", 2},
    {57, "R16_UINT", 2}, {58, "R16_SNORM", 2}, {59, "R16_SINT", 2}, {60, "R8_TYPELESS", 2},
    {61, "R8_UNORM", 2}, {62, "R8_UINT", 2}, {63, "R8_SNORM", 2}, {64, "R8_SINT", 2}, {65, "A8_UNORM", 2},
    {100, "AYUV", 2}, {107, "YUY2", 2}, {103, "NV12", 2}, {110, "NV11", 2}, {105, "P016", 2}, {104, "P010", 2},
    {109, "Y216", 2}, {108, "Y210", 2}, {102, "Y416", 2}, {101, "Y410", 2}};

// ---- The CROSS_ADAPTER create: the one sign of sharing that the D3D12 DDI gives ----
// D3D12DDI_RESOURCE_FLAG_0003_CROSS_ADAPTER (0x4) marks a texture as shared across adapters. Such a create that falls
// outside the envelope must get a record an opener reads (P6) or a loud refusal at the create, never a silent P7.
// The answer comes from engine-ddi/cross-adapter.h (t39/cross-adapter-tier1): create_refused refuses every
// CROSS_ADAPTER texture, with a named log line, before any memory request. A tree without that header gives P7.
// Buffers have no row: a cross-adapter buffer is placed in a shared heap, and the heap is the shared object.
// The header is included at the top of the file.
bool cross_adapter_refused(D3D12_RESOURCE_DIMENSION dimension) {
#if defined(RECORD_PAIR_CROSS_ADAPTER)
    return engine_ddi::cross_adapter::create_refused(dimension, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
#else
    (void)dimension;
    return false;
#endif
}
void sweep_cross_adapter(const std::vector<Shape> &all) {
    const std::string formats = "S2 D3D12 CROSS_ADAPTER create outside the envelope -> an opener";
    const std::string mips = "S2 D3D12 CROSS_ADAPTER create of mips > 1 or array > 1 -> an opener";
    for (const SharedFormat &f : kShared) {
        if (Bc250SharedSurfaceFormat(f.dxgi)) continue;  // inside the envelope: P5, the rows above
        for (const Shape &sh : all) {
            if (sh.w > 8192 || sh.h > 8192) continue;
            if (cross_adapter_refused(D3D12_RESOURCE_DIMENSION_TEXTURE2D)) { pair(formats).ok(); continue; }
            char why[96];
            std::snprintf(why, sizeof why, "%s (%u): the create takes P7, no opener reads it", f.name, f.dxgi);
            pair(formats).fail(sh.w, sh.h, why);
        }
    }
    for (D3D12_RESOURCE_DIMENSION d : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                       D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
        if (cross_adapter_refused(d)) pair(mips).ok();
        else pair(mips).fail(1, 1, "outside the envelope (linear_surface_shape): the create takes P7");
    }
}

void sweep_shared_create_rule(const std::vector<Shape> &all) {
    char name[160];
    for (const SharedFormat &f : kShared) {
        std::snprintf(name, sizeof name, "S%d D3D12 shared create %s (%u) -> an opener", f.tier, f.name, f.dxgi);
        // A plain shared create cannot be told from a private one: the DDI has no sharing bit, and a D3D12 shared
        // committed texture arrives with the heap flags 0x26 of an ordinary one (docs/d3d12-shared-resources.md,
        // measured 2026-10-05). The answer for the unflagged create is the named refusal of the open (row above).
        if (f.tier == 2) note(name, "DDI carries no sharing bit (heap flags 0x26, measured 2026-10-05); the open refusal is named");
        const AMDGPU_WDDM_SURFACE_FORMAT *row = Bc250SharedSurfaceFormat(f.dxgi);
        for (const Shape &sh : all) {
            if (sh.w > 8192 || sh.h > 8192) continue;  // larger: the D3D12 shell refuses the create
            if (!row) { pair(name).fail(sh.w, sh.h, "outside the envelope: the create takes P7, no opener reads it"); continue; }
            const uint64_t pitch = radv_pitch(sh.w, row->bytes_per_pixel);
            BC250_SHARED_SURFACE in{};
            in.Width = sh.w; in.Height = sh.h; in.Pitch = (unsigned long)pitch; in.DxgiFormat = f.dxgi;
            in.D3dDdiFormat = row->d3dddi; in.BytesPerPixel = row->bytes_per_pixel; in.Shared = 1;
            in.BindFlags = BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET;
            in.Size = round_page(std::max<uint64_t>(pitch * align4(sh.h), radv_memory(sh.w, sh.h, row->bytes_per_pixel)));
            BC250_WDDM_ALLOCATION_PRIVATE a{}; BC250_SURFACE_RESOURCE_PRIVATE r{};
            if (Bc250SharedSurfaceEncode(&in, &a, &r) != BC250_SHARED_SURFACE_OK) { pair(name).fail(sh.w, sh.h, "encode refused"); continue; }
            BC250_SHARED_SURFACE s{}; const char *why = "";
            BC250_SHARED_SURFACE_ADMIT admit{0, BC250_SURFACE_RESOURCE_ACCESS_MASK};
            const bool d12 = !Bc250SharedSurfaceDecodeWhy(&r, sizeof r, &a, sizeof a, &s, &why) && d3d12_after_decode(s, false).empty();
            const bool d11 = !Bc250SharedSurfaceDecodeAdmitted(&r, sizeof r, &a, sizeof a, &admit, &s) && d3d11_after_decode(s).empty();
            (d12 || d11) ? pair(name).ok() : pair(name).fail(sh.w, sh.h, "no opener reads the P5 record");
        }
    }
    const std::string mips = "S2 D3D12 shared create of mips > 1 or array > 1 -> an opener";
    note(mips, "DDI carries no sharing bit (heap flags 0x26, measured 2026-10-05); the open refusal is named");
    pair(mips).fail(1, 1, "outside the envelope (linear_surface_shape): the create takes P7, no opener reads it");
    sweep_cross_adapter(all);
}
}  // namespace

int main() {
    static_assert(sizeof(BC250_WDDM_ALLOCATION_PRIVATE) == 32 && sizeof(BC250_GDI_PRIVATE) == 48 &&
                  sizeof(BC250_SURFACE_RESOURCE_PRIVATE) == 64 && sizeof(BC250_WDDM_NV12_PRIVATE) == 64,
                  "wire sizes");
    const auto all = shapes();
    sweep_kmd_standard(all);
    sweep_shells(all);
    sweep_x_views(all);
    sweep_nv12(all);
    sweep_raw(all);
    sweep_shared_create_rule(all);
    unsigned long long failed = 0, noted = 0;
    for (const auto &[name, r] : g_results) {
        const auto it = g_note.find(name);
        const bool is_note = it != g_note.end();
        const bool design = is_note && it->second.rfind("by design", 0) == 0;
        // A "by design" pair that the consumer admits in any shape is a change of contract: that fails too.
        const bool fail = design ? r.refused != r.tried : (!is_note && r.refused);
        std::printf("%s\t%s\t%llu\t%llu\t%s\t%s\n", fail ? "FAIL" : (is_note && r.refused) ? "NOTE" : "ok",
                    name.c_str(), r.tried, r.refused, r.first.c_str(), is_note ? it->second.c_str() : "");
        failed += fail ? 1 : 0;
        noted += (is_note && r.refused) ? 1 : 0;
    }
    std::printf("SUMMARY\t%zu pairs\t%llu failed\t%llu noted\t%s\t%s\n", g_results.size(), failed, noted,
                sizeof(void *) == 8 ? "x64" : "x86",
#if defined(RECORD_PAIR_IMPORT_MAPPED)
                "D3D11 import = mapped range"
#else
                "D3D11 import = record size"
#endif
    );
    return failed ? 1 : 0;
}
