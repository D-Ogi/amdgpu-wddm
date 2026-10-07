// SPDX-License-Identifier: MIT
//
// M15.14 increment 1: the front's `D3D11_1DDI_DEVICEFUNCS` table, all 155 entries the WDK declares here
// (157 with the two D3D10PSGP members, which are reserved for system use and not declared in this build).
//
// Each entry is one of four things, and the reason is at the entry:
//
//   a field copy    the typedef is identical at the D3D10.0 DDI, so the runtime's table holds the hosted
//                   driver's own function. The front is not on that call at all: no thunk, no lookup, no
//                   cost. 75 entries, two of which name a hosted entry of another name
//                   (pfnResourceConvert takes pfnResourceCopy, pfnDynamicConstantBufferMapNoOverwrite
//                   takes the generic pfnResourceMap).
//   a thunk or hook the argument struct or the signature grew between the two DDIs, or the typedef is
//                   identical but the front must see the call (OpenResource and CreateResource to record a
//                   surface, DestroyResource to forget it, DestroyDevice to retire the device). A thunk
//                   forwards what the hosted entry can express and names, once, every semantic it had to
//                   drop. It never refuses: a refused draw or create is a dead desktop, and the semantics
//                   a 10_0 pipeline never uses are exactly the ones that are dropped. 32 entries.
//   a refusal       an entry a device at 3D pipeline level 10_0 cannot reach: tessellation, compute,
//                   unordered access, indirect draws, command lists and deferred contexts, shader
//                   interfaces. The front publishes `3DPIPELINESUPPORT` with the 10_0 bit alone and
//                   `THREADING` 0, so the runtime has no reason to call them; each body counts the call
//                   and logs the entry once, which turns a wrong assumption into a line instead of a
//                   crash in address zero. A refusing CREATE also reports the failure through
//                   pfnSetErrorCb, because the entry is VOID and a runtime that was not told believes the
//                   object exists. 43 entries.
//   a real body     pfnDiscard (a no-op: there is nothing to discard below), pfnAssignDebugBinary (a
//                   no-op), pfnCheckDeferredContextHandleSizes (zero sizes), pfnClearView (amendment 4)
//                   and pfnCheckDirectFlipSupport (the point of the increment; from increment 2 it
//                   writes the rule's own answer). 5 entries;
//                   pfnRelocateDeviceFuncs, which re-fills the moved table from the front's own copy of
//                   the hosted one, is counted with the thunks because it is one of the 32 the gate names.
//
// 75 + 32 + 43 + 5 = 155. No entry is left null: `FillDeviceFuncs` writes every slot, the static asserts in
// front-adapter.h fail if the WDK changes either table's shape, and tests/test-router.cpp names every one of
// the 75 field copies with the hosted slot it must hold and every one of the 37 non-refusal own bodies, so
// neither the split nor a single stage binding can drift unnoticed.

#include "front-resource.h"
#include "front-direct-flip.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace bc250front {
namespace {

// ---------------------------------------------------------------- the device behind a handle

// Every thunk starts here. A handle the front did not publish is a bug in the runtime or in the front's
// own install, and it must not be a crash: the entry says so once and does nothing.
#define FRONT_DEVICE(handle, failed)                                                   \
    Device *dev = DeviceOf(handle);                                                    \
    if (!dev) {                                                                         \
        static volatile LONG once = 0;                                                  \
        Refuse(nullptr, "unknown-device", &once);                                        \
        return failed;                                                                  \
    }

#define REFUSE_ONCE(device, name)                     \
    do {                                              \
        static volatile LONG once = 0;                \
        Refuse(device, name, &once);                  \
    } while (0)

#define DROPPED_ONCE(device, name, what)              \
    do {                                              \
        static volatile LONG once = 0;                \
        Dropped(device, name, what, &once);           \
    } while (0)

// ---------------------------------------------------------------- argument translation

// A D3D11 signature entry is 16 bytes and a D3D10 one is 12, so the arrays cannot be shared and the
// entries are copied. The hosted frontend ignores the signatures altogether (Shader.cpp: every create
// takes pSignatures and reads none of it), so the bound below costs nothing today; a frontend that reads
// them must raise it rather than accept a clamp, and the log line says when the bound was hit.
const UINT kSignatureMax = 128;
struct SignatureBuffer {
    D3D10DDIARG_SIGNATURE_ENTRY in[kSignatureMax];
    D3D10DDIARG_SIGNATURE_ENTRY out[kSignatureMax];
    D3D10DDIARG_STAGE_IO_SIGNATURES sig;
};

void CopySignatureEntries(const D3D11_1DDIARG_SIGNATURE_ENTRY2 *from, UINT count,
                          D3D10DDIARG_SIGNATURE_ENTRY *to)
{
    for (UINT i = 0; i < count; ++i) {
        to[i].SystemValue = from[i].SystemValue;
        to[i].Register = from[i].Register;
        to[i].Mask = from[i].Mask;
        // RegisterComponentType, MinPrecision and Stream have no field at the D3D10.0 DDI.
    }
}

const D3D10DDIARG_STAGE_IO_SIGNATURES *Translate(Device *dev, const char *entry,
                                                 const D3D11_1DDIARG_STAGE_IO_SIGNATURES *from,
                                                 SignatureBuffer *buffer, volatile LONG *clamped)
{
    ZeroMemory(&buffer->sig, sizeof(buffer->sig));
    if (!from) return &buffer->sig;
    const UINT inputs = from->NumInputSignatureEntries, outputs = from->NumOutputSignatureEntries;
    if (inputs > kSignatureMax || outputs > kSignatureMax) {
        Dropped(dev, entry, "signature longer than 128 entries", clamped);
        return &buffer->sig;
    }
    if (inputs && from->pInputSignature) {
        CopySignatureEntries(from->pInputSignature, inputs, buffer->in);
        buffer->sig.pInputSignature = buffer->in;
        buffer->sig.NumInputSignatureEntries = inputs;
    }
    if (outputs && from->pOutputSignature) {
        CopySignatureEntries(from->pOutputSignature, outputs, buffer->out);
        buffer->sig.pOutputSignature = buffer->out;
        buffer->sig.NumOutputSignatureEntries = outputs;
    }
    return &buffer->sig;
}

// The stream-output declaration grew a Stream field at the front of each entry, and the single stride
// became an array with a rasterized-stream index. Only stream 0 exists at the D3D10.0 DDI.
const UINT kSoEntryMax = 128;
struct StreamOutputBuffer {
    D3D10DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY entries[kSoEntryMax];
    D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT arg;
};

bool TranslateStreamOutput(Device *dev, const char *entry,
                           const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *from,
                           StreamOutputBuffer *buffer, volatile LONG *dropped)
{
    ZeroMemory(&buffer->arg, sizeof(buffer->arg));
    if (!from) return false;
    buffer->arg.pShaderCode = from->pShaderCode;
    buffer->arg.NumEntries = from->NumEntries;
    buffer->arg.StreamOutputStrideInBytes =
        from->NumStrides && from->BufferStridesInBytes ? from->BufferStridesInBytes[0] : 0;
    if (from->NumStrides > 1 || from->RasterizedStream)
        Dropped(dev, entry, "more than one stream-output stride, or a rasterized stream other than 0",
                dropped);
    if (from->NumEntries > kSoEntryMax) {
        Dropped(dev, entry, "stream-output declaration longer than 128 entries", dropped);
        buffer->arg.NumEntries = 0;
        return true;
    }
    if (from->NumEntries && from->pOutputStreamDecl) {
        for (UINT i = 0; i < from->NumEntries; ++i) {
            buffer->entries[i].OutputSlot = from->pOutputStreamDecl[i].OutputSlot;
            buffer->entries[i].RegisterIndex = from->pOutputStreamDecl[i].RegisterIndex;
            buffer->entries[i].RegisterMask = from->pOutputStreamDecl[i].RegisterMask;
            if (from->pOutputStreamDecl[i].Stream) Dropped(dev, entry, "stream other than 0", dropped);
        }
        buffer->arg.pOutputStreamDecl = buffer->entries;
    }
    return true;
}

// D3D10DDIARG_CREATERESOURCE is a byte prefix of D3D11DDIARG_CREATERESOURCE, so the hosted entry may read
// the runtime's struct in place. These asserts are what makes the cast legitimate; a WDK that reorders a
// field fails the build here instead of handing the hosted driver a misread create.
#define FRONT_SAME_FIELD(field)                                                                       \
    static_assert(offsetof(D3D10DDIARG_CREATERESOURCE, field) ==                                      \
                      offsetof(D3D11DDIARG_CREATERESOURCE, field),                                    \
                  "D3D10DDIARG_CREATERESOURCE is no longer a prefix of D3D11DDIARG_CREATERESOURCE: "   \
                  "field " #field " moved")
FRONT_SAME_FIELD(pMipInfoList);
FRONT_SAME_FIELD(pInitialDataUP);
FRONT_SAME_FIELD(ResourceDimension);
FRONT_SAME_FIELD(Usage);
FRONT_SAME_FIELD(BindFlags);
FRONT_SAME_FIELD(MapFlags);
FRONT_SAME_FIELD(MiscFlags);
FRONT_SAME_FIELD(Format);
FRONT_SAME_FIELD(SampleDesc);
FRONT_SAME_FIELD(MipLevels);
FRONT_SAME_FIELD(ArraySize);
FRONT_SAME_FIELD(pPrimaryDesc);
#undef FRONT_SAME_FIELD
static_assert(sizeof(D3D10DDIARG_CREATERESOURCE) <= sizeof(D3D11DDIARG_CREATERESOURCE),
              "D3D11DDIARG_CREATERESOURCE must still be at least as large as the D3D10 one");
static_assert(offsetof(D3D11DDIARG_CREATERESOURCE, ByteStride) == sizeof(D3D10DDIARG_CREATERESOURCE),
              "the D3D11 create-resource fields no longer start where the D3D10 struct ends");
// The rasterizer description grew one trailing field, so the same prefix argument holds for it.
static_assert(sizeof(D3D10_DDI_RASTERIZER_DESC) <= sizeof(D3D11_1_DDI_RASTERIZER_DESC),
              "D3D11_1_DDI_RASTERIZER_DESC shrank below the D3D10 one");
static_assert(offsetof(D3D11_1_DDI_RASTERIZER_DESC, ForcedSampleCount) ==
                  sizeof(D3D10_DDI_RASTERIZER_DESC),
              "D3D10_DDI_RASTERIZER_DESC is no longer the prefix of D3D11_1_DDI_RASTERIZER_DESC");

const D3D10DDIARG_CREATERESOURCE *AsD3D10Create(Device *dev, const char *entry,
                                                const D3D11DDIARG_CREATERESOURCE *from,
                                                volatile LONG *dropped)
{
    if (from && (from->ByteStride || from->TextureLayout))
        Dropped(dev, entry, "a byte stride or a texture layout, which the D3D10.0 DDI has no field for",
                dropped);
    return reinterpret_cast<const D3D10DDIARG_CREATERESOURCE *>(from);
}

// The blend description changed shape rather than growing: D3D10 has one set of factors for every render
// target plus a per-target enable and write mask, D3D11_1 has a full description per target. At 3D
// pipeline level 10_0 the runtime's own validation forbids independent blending (10_1) and the logic op
// (11_1, and the front publishes OutputMergerLogicOp FALSE), so target 0 carries the factors. A mismatch
// is named once and target 0 still wins: a refused blend state is a dead desktop, a wrong blend is a
// visible artefact that the log explains.
void TranslateBlend(Device *dev, const char *entry, const D3D11_1_DDI_BLEND_DESC *from,
                    D3D10_DDI_BLEND_DESC *to, volatile LONG *dropped)
{
    ZeroMemory(to, sizeof(*to));
    if (!from) return;
    const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &zero = from->RenderTarget[0];
    to->AlphaToCoverageEnable = from->AlphaToCoverageEnable;
    to->SrcBlend = zero.SrcBlend;
    to->DestBlend = zero.DestBlend;
    to->BlendOp = zero.BlendOp;
    to->SrcBlendAlpha = zero.SrcBlendAlpha;
    to->DestBlendAlpha = zero.DestBlendAlpha;
    to->BlendOpAlpha = zero.BlendOpAlpha;
    for (UINT i = 0; i < D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        const D3D11_1_DDI_RENDER_TARGET_BLEND_DESC &rt =
            from->RenderTarget[from->IndependentBlendEnable ? i : 0];
        to->BlendEnable[i] = rt.BlendEnable;
        to->RenderTargetWriteMask[i] = rt.RenderTargetWriteMask;
        if (rt.LogicOpEnable) Dropped(dev, entry, "an output-merger logic op", dropped);
        if (from->IndependentBlendEnable &&
            (rt.SrcBlend != zero.SrcBlend || rt.DestBlend != zero.DestBlend ||
             rt.BlendOp != zero.BlendOp || rt.SrcBlendAlpha != zero.SrcBlendAlpha ||
             rt.DestBlendAlpha != zero.DestBlendAlpha || rt.BlendOpAlpha != zero.BlendOpAlpha))
            Dropped(dev, entry, "independent blend factors per render target", dropped);
    }
}

// The shader-resource view gained a BufferEx shape and a wider cube shape. Everything else is a field
// copy. Returns false for a shape the D3D10.0 DDI cannot express; the caller then does not create the
// view, which is the one refusal among the translations - BufferEx is a raw or structured buffer view,
// an 11_0 feature a 10_0 device cannot ask for, and inventing a Buffer view for it would be worse.
bool TranslateShaderResourceView(Device *dev, const char *entry,
                                 const D3D11DDIARG_CREATESHADERRESOURCEVIEW *from,
                                 D3D10DDIARG_CREATESHADERRESOURCEVIEW *to, volatile LONG *dropped)
{
    ZeroMemory(to, sizeof(*to));
    if (!from) return false;
    to->hDrvResource = from->hDrvResource;
    to->Format = from->Format;
    to->ResourceDimension = from->ResourceDimension;
    switch (from->ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        to->Buffer = from->Buffer;
        return true;
    case D3D10DDIRESOURCE_TEXTURE1D:
        to->Tex1D = from->Tex1D;
        return true;
    case D3D10DDIRESOURCE_TEXTURE2D:
        to->Tex2D = from->Tex2D;
        return true;
    case D3D10DDIRESOURCE_TEXTURE3D:
        to->Tex3D = from->Tex3D;
        return true;
    case D3D10DDIRESOURCE_TEXTURECUBE:
        to->TexCube.MostDetailedMip = from->TexCube.MostDetailedMip;
        to->TexCube.MipLevels = from->TexCube.MipLevels;
        if (from->TexCube.First2DArrayFace)
            Dropped(dev, entry, "a cube view that does not start at face 0", dropped);
        return true;
    default:
        // D3D11DDIRESOURCE_BUFFEREX and anything later.
        Dropped(dev, entry, "a resource dimension the D3D10.0 DDI has no view shape for", dropped);
        return false;
    }
}

// The depth-stencil view gained a Flags field in the middle of the struct, so this one is a field copy
// and not a prefix cast. The flags are READ_ONLY_DEPTH and READ_ONLY_STENCIL, both 11_0.
bool TranslateDepthStencilView(Device *dev, const char *entry,
                               const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *from,
                               D3D10DDIARG_CREATEDEPTHSTENCILVIEW *to, volatile LONG *dropped)
{
    ZeroMemory(to, sizeof(*to));
    if (!from) return false;
    to->hDrvResource = from->hDrvResource;
    to->Format = from->Format;
    to->ResourceDimension = from->ResourceDimension;
    switch (from->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D: to->Tex1D = from->Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D: to->Tex2D = from->Tex2D; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: to->TexCube = from->TexCube; break;
    default:
        Dropped(dev, entry, "a resource dimension the D3D10.0 DDI has no depth-stencil view shape for",
                dropped);
        return false;
    }
    if (from->Flags) Dropped(dev, entry, "a read-only depth or stencil view", dropped);
    return true;
}

// ---------------------------------------------------------------- the resource record

// What a create implies about a surface, which is the compositor's own side of a DirectFlip pair
// (amendment 5): the record this device's create would have produced. The SCANOUT bit is never set here -
// a compositor's own front buffer does not ask for scan-out and must not have to, which is exactly what
// WddmGdiCreatedScannable encodes.
bool FillCreatedResource(const D3D11DDIARG_CREATERESOURCE *args, Resource *out)
{
    ZeroMemory(out, sizeof(*out));
    if (!args) return false;
    const bool shared = (args->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0;
    const bool present = (args->BindFlags & D3D10_DDI_BIND_PRESENT) != 0 || args->pPrimaryDesc != nullptr;
    if (!shared && !present) return false;  // not a candidate for either side of a pair: not recorded
    out->recorded = true;
    out->opened = false;
    out->primary = present;
    out->shared = shared;
    out->format = (unsigned int)args->Format;
    if (args->pMipInfoList) {
        out->width = args->pMipInfoList[0].TexelWidth;
        out->height = args->pMipInfoList[0].TexelHeight;
    }
    // The pitch the hosted driver will give this surface when it allocates it (HostedSurfacePitch). A
    // format with no row in the shared table keeps 0, which the rule refuses as pitch-unknown.
    const AMDGPU_WDDM_SURFACE_FORMAT *row = amdgpu_wddm_surface_format_by_dxgi(out->format);
    out->pitch = row ? HostedSurfacePitch(out->width, row->bytes_per_pixel) : 0u;
    out->vidpn_source = args->pPrimaryDesc ? (unsigned int)args->pPrimaryDesc->VidPnSourceId
                                           : BC250_SCANOUT_VIDPN_SOURCE;
    out->record.Magic = BC250_SURFACE_RESOURCE_MAGIC;
    out->record.Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    out->record.Shared = shared ? 1ul : 0ul;
    out->record.Access = present ? BC250_SURFACE_RESOURCE_PRIMARY : 0ul;
    out->record.Width = out->width;
    out->record.Height = out->height;
    out->record.MipLevels = args->MipLevels;
    out->record.ArraySize = args->ArraySize;
    out->record.Format = out->format;
    out->record.SampleCount = args->SampleDesc.Count;
    out->record.SampleQuality = args->SampleDesc.Quality;
    out->record.Usage = args->Usage;
    out->record.BindFlags = args->BindFlags;
    out->record.CpuAccessFlags = args->MapFlags;
    out->record.MiscFlags = args->MiscFlags;
    return true;
}

// What an open says about a surface, which is the application's side. The geometry comes from the LB7A
// allocation blob the kernel driver wrote and the intent from the E26R resource record its creator wrote;
// a blob or record the kernel driver's own parsers refuse leaves the fields zero, and the rule then
// refuses the pair under the matching clause instead of guessing.
void FillOpenedResource(const D3D10DDIARG_OPENRESOURCE *args, Resource *out)
{
    ZeroMemory(out, sizeof(*out));
    out->recorded = true;
    out->opened = true;
    out->shared = true;
    out->vidpn_source = BC250_SCANOUT_VIDPN_SOURCE;
    if (!args) return;
    if (DecodeRecord(args->pPrivateDriverData, args->PrivateDriverDataSize, &out->record)) {
        out->primary = (out->record.Access & BC250_SURFACE_RESOURCE_PRIMARY) != 0;
        if (out->record.Version == BC250_SURFACE_RESOURCE_TEXTURE_VERSION) {
            out->width = out->record.Width;
            out->height = out->record.Height;
            out->format = out->record.Format;
        }
    }
    if (args->NumAllocations && args->pOpenAllocationInfo) {
        unsigned int width = 0, height = 0, pitch = 0, d3dddi = 0;
        const D3DDDI_OPENALLOCATIONINFO &first = args->pOpenAllocationInfo[0];
        if (DecodeAllocation(first.pPrivateDriverData, first.PrivateDriverDataSize, &width, &height,
                             &pitch, &d3dddi)) {
            if (width) out->width = width;
            if (height) out->height = height;
            out->pitch = pitch;
            // The storage format of record and blob must agree; the blob is the kernel driver's own view.
            const AMDGPU_WDDM_SURFACE_FORMAT *row = amdgpu_wddm_surface_format_by_d3dddi(d3dddi);
            if (row && row->dxgi) out->format = row->dxgi;
        }
    }
}

// ---------------------------------------------------------------- the thunks

VOID APIENTRY DefaultConstantBufferUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice,
                                                       D3D10DDI_HRESOURCE hResource, UINT subresource,
                                                       CONST D3D10_DDI_BOX *box, CONST VOID *data,
                                                       UINT rowPitch, UINT depthPitch, UINT copyFlags)
{
    FRONT_DEVICE(hDevice, )
    if (copyFlags) DROPPED_ONCE(dev, "pfnDefaultConstantBufferUpdateSubresourceUP", "a copy flag");
    dev->hosted.pfnDefaultConstantBufferUpdateSubresourceUP(hDevice, hResource, subresource, box, data,
                                                            rowPitch, depthPitch);
}

// The three constant-buffer setters gained a first-constant and a constant-count array per buffer, which
// is the 11.1 partial-constant-buffer binding. At 3D pipeline level 10_0 the runtime binds whole buffers,
// so a non-null array with a non-zero offset is the one thing worth naming.
void CheckConstantRanges(Device *dev, const char *entry, UINT buffers, CONST UINT *first,
                         CONST UINT *count, volatile LONG *dropped)
{
    (void)count;
    if (!first) return;
    for (UINT i = 0; i < buffers; ++i)
        if (first[i]) {
            Dropped(dev, entry, "a constant-buffer binding that does not start at constant 0", dropped);
            return;
        }
}

#define FRONT_SET_CONSTANT_BUFFERS(Stage)                                                            \
    VOID APIENTRY Stage##SetConstantBuffers(D3D10DDI_HDEVICE hDevice, UINT start, UINT buffers,       \
                                           CONST D3D10DDI_HRESOURCE *resources, CONST UINT *first,    \
                                           CONST UINT *count)                                         \
    {                                                                                                 \
        FRONT_DEVICE(hDevice, )                                                                       \
        {                                                                                             \
            static volatile LONG once = 0;                                                            \
            CheckConstantRanges(dev, "pfn" #Stage "SetConstantBuffers", buffers, first, count, &once); \
        }                                                                                             \
        dev->hosted.pfn##Stage##SetConstantBuffers(hDevice, start, buffers, resources);                \
    }
FRONT_SET_CONSTANT_BUFFERS(Vs)
FRONT_SET_CONSTANT_BUFFERS(Ps)
FRONT_SET_CONSTANT_BUFFERS(Gs)
#undef FRONT_SET_CONSTANT_BUFFERS

VOID APIENTRY SetRenderTargets(D3D10DDI_HDEVICE hDevice, CONST D3D10DDI_HRENDERTARGETVIEW *rtvs,
                               UINT numRtvs, UINT clearSlots, D3D10DDI_HDEPTHSTENCILVIEW dsv,
                               CONST D3D11DDI_HUNORDEREDACCESSVIEW *uavs, CONST UINT *offsets,
                               UINT uavStartSlot, UINT numUavs, UINT uavRangeStart, UINT uavRangeSize)
{
    FRONT_DEVICE(hDevice, )
    (void)uavs;
    (void)offsets;
    (void)uavStartSlot;
    (void)uavRangeStart;
    if (numUavs || uavRangeSize)
        DROPPED_ONCE(dev, "pfnSetRenderTargets", "unordered-access views bound with the render targets");
    dev->hosted.pfnSetRenderTargets(hDevice, rtvs, numRtvs, clearSlots, dsv);
}

VOID APIENTRY ResourceCopyRegion(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE dst, UINT dstSub,
                                 UINT dstX, UINT dstY, UINT dstZ, D3D10DDI_HRESOURCE src, UINT srcSub,
                                 CONST D3D10_DDI_BOX *box, UINT copyFlags)
{
    FRONT_DEVICE(hDevice, )
    if (copyFlags) DROPPED_ONCE(dev, "pfnResourceCopyRegion", "a copy flag");
    dev->hosted.pfnResourceCopyRegion(hDevice, dst, dstSub, dstX, dstY, dstZ, src, srcSub, box);
}

// The 10.1 convert entries, which the hosted frontend maps to its own copy entries exactly like this when
// it negotiates the 10_1 DDI (Device.cpp). The front does the same rather than refusing them.
VOID APIENTRY ResourceConvertRegion(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE dst, UINT dstSub,
                                    UINT dstX, UINT dstY, UINT dstZ, D3D10DDI_HRESOURCE src, UINT srcSub,
                                    CONST D3D10_DDI_BOX *box, UINT copyFlags)
{
    FRONT_DEVICE(hDevice, )
    if (copyFlags) DROPPED_ONCE(dev, "pfnResourceConvertRegion", "a copy flag");
    dev->hosted.pfnResourceCopyRegion(hDevice, dst, dstSub, dstX, dstY, dstZ, src, srcSub, box);
}

VOID APIENTRY ResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                                          UINT subresource, CONST D3D10_DDI_BOX *box, CONST VOID *data,
                                          UINT rowPitch, UINT depthPitch, UINT copyFlags)
{
    FRONT_DEVICE(hDevice, )
    if (copyFlags) DROPPED_ONCE(dev, "pfnResourceUpdateSubresourceUP", "a copy flag");
    dev->hosted.pfnResourceUpdateSubresourceUP(hDevice, hResource, subresource, box, data, rowPitch,
                                               depthPitch);
}

// The 11.1 flush returns whether the driver flushed and takes flags; the D3D10.0 one does neither. The
// hosted entry always flushes, so TRUE is the honest answer.
BOOL APIENTRY Flush(D3D10DDI_HDEVICE hDevice, UINT flushFlags)
{
    FRONT_DEVICE(hDevice, TRUE)
    if (flushFlags) DROPPED_ONCE(dev, "pfnFlush", "a flush flag");
    dev->hosted.pfnFlush(hDevice);
    return TRUE;
}

// The runtime moved its copy of the table. The front's copy of the HOSTED table lives in the front's own
// device record and did not move, so the hosted driver is not told anything: the front simply fills the
// new table again. Calling the hosted pfnRelocateDeviceFuncs here would hand it a table that is not its.
VOID APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE hDevice, D3D11_1DDI_DEVICEFUNCS *funcs)
{
    FRONT_DEVICE(hDevice, )
    if (!funcs) return;
    FillDeviceFuncs(funcs, dev->hosted);
}

SIZE_T APIENTRY CalcPrivateResourceSize(D3D10DDI_HDEVICE hDevice,
                                        CONST D3D11DDIARG_CREATERESOURCE *args)
{
    FRONT_DEVICE(hDevice, 0)
    static volatile LONG once = 0;
    return dev->hosted.pfnCalcPrivateResourceSize(
        hDevice, AsD3D10Create(dev, "pfnCalcPrivateResourceSize", args, &once));
}

VOID APIENTRY CreateResource(D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATERESOURCE *args,
                             D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource)
{
    FRONT_DEVICE(hDevice, )
    static volatile LONG once = 0;
    dev->hosted.pfnCreateResource(hDevice, AsD3D10Create(dev, "pfnCreateResource", args, &once),
                                  hResource, hRTResource);
    Resource record;
    if (FillCreatedResource(args, &record)) RecordResource(dev, hResource.pDrvPrivate, record);
}

VOID APIENTRY OpenResource(D3D10DDI_HDEVICE hDevice, CONST D3D10DDIARG_OPENRESOURCE *args,
                           D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource)
{
    FRONT_DEVICE(hDevice, )
    dev->hosted.pfnOpenResource(hDevice, args, hResource, hRTResource);
    Resource record;
    FillOpenedResource(args, &record);
    RecordResource(dev, hResource.pDrvPrivate, record);
}

VOID APIENTRY DestroyResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
    FRONT_DEVICE(hDevice, )
    ForgetResource(hResource.pDrvPrivate);
    dev->hosted.pfnDestroyResource(hDevice, hResource);
}

SIZE_T APIENTRY CalcPrivateShaderResourceViewSize(D3D10DDI_HDEVICE hDevice,
                                                  CONST D3D11DDIARG_CREATESHADERRESOURCEVIEW *args)
{
    FRONT_DEVICE(hDevice, 0)
    static volatile LONG once = 0;
    D3D10DDIARG_CREATESHADERRESOURCEVIEW translated;
    if (!TranslateShaderResourceView(dev, "pfnCalcPrivateShaderResourceViewSize", args, &translated,
                                     &once))
        return 0;
    return dev->hosted.pfnCalcPrivateShaderResourceViewSize(hDevice, &translated);
}

VOID APIENTRY CreateShaderResourceView(D3D10DDI_HDEVICE hDevice,
                                       CONST D3D11DDIARG_CREATESHADERRESOURCEVIEW *args,
                                       D3D10DDI_HSHADERRESOURCEVIEW hView,
                                       D3D10DDI_HRTSHADERRESOURCEVIEW hRTView)
{
    FRONT_DEVICE(hDevice, )
    static volatile LONG once = 0;
    D3D10DDIARG_CREATESHADERRESOURCEVIEW translated;
    // The paired CalcPrivateShaderResourceViewSize answered 0 for the same arguments, so the runtime holds a
    // view handle over no private block at all; it must learn that the view does not exist, or its
    // pfnDestroyShaderResourceView (a field copy) hands that block to the hosted driver.
    if (!TranslateShaderResourceView(dev, "pfnCreateShaderResourceView", args, &translated, &once)) {
        CreateFailed(dev);
        return;
    }
    dev->hosted.pfnCreateShaderResourceView(hDevice, &translated, hView, hRTView);
}

SIZE_T APIENTRY CalcPrivateDepthStencilViewSize(D3D10DDI_HDEVICE hDevice,
                                                CONST D3D11DDIARG_CREATEDEPTHSTENCILVIEW *args)
{
    FRONT_DEVICE(hDevice, 0)
    static volatile LONG once = 0;
    D3D10DDIARG_CREATEDEPTHSTENCILVIEW translated;
    if (!TranslateDepthStencilView(dev, "pfnCalcPrivateDepthStencilViewSize", args, &translated, &once))
        return 0;
    return dev->hosted.pfnCalcPrivateDepthStencilViewSize(hDevice, &translated);
}

VOID APIENTRY CreateDepthStencilView(D3D10DDI_HDEVICE hDevice,
                                     CONST D3D11DDIARG_CREATEDEPTHSTENCILVIEW *args,
                                     D3D10DDI_HDEPTHSTENCILVIEW hView,
                                     D3D10DDI_HRTDEPTHSTENCILVIEW hRTView)
{
    FRONT_DEVICE(hDevice, )
    static volatile LONG once = 0;
    D3D10DDIARG_CREATEDEPTHSTENCILVIEW translated;
    // Same pair as the shader-resource view above: a refused translation must be reported, or the runtime's
    // pfnDestroyDepthStencilView reaches the hosted driver with an uninitialised private block.
    if (!TranslateDepthStencilView(dev, "pfnCreateDepthStencilView", args, &translated, &once)) {
        CreateFailed(dev);
        return;
    }
    dev->hosted.pfnCreateDepthStencilView(hDevice, &translated, hView, hRTView);
}

SIZE_T APIENTRY CalcPrivateBlendStateSize(D3D10DDI_HDEVICE hDevice, CONST D3D11_1_DDI_BLEND_DESC *desc)
{
    FRONT_DEVICE(hDevice, 0)
    static volatile LONG once = 0;
    D3D10_DDI_BLEND_DESC translated;
    TranslateBlend(dev, "pfnCalcPrivateBlendStateSize", desc, &translated, &once);
    return dev->hosted.pfnCalcPrivateBlendStateSize(hDevice, &translated);
}

VOID APIENTRY CreateBlendState(D3D10DDI_HDEVICE hDevice, CONST D3D11_1_DDI_BLEND_DESC *desc,
                               D3D10DDI_HBLENDSTATE hState, D3D10DDI_HRTBLENDSTATE hRTState)
{
    FRONT_DEVICE(hDevice, )
    static volatile LONG once = 0;
    D3D10_DDI_BLEND_DESC translated;
    TranslateBlend(dev, "pfnCreateBlendState", desc, &translated, &once);
    dev->hosted.pfnCreateBlendState(hDevice, &translated, hState, hRTState);
}

SIZE_T APIENTRY CalcPrivateRasterizerStateSize(D3D10DDI_HDEVICE hDevice,
                                               CONST D3D11_1_DDI_RASTERIZER_DESC *desc)
{
    FRONT_DEVICE(hDevice, 0)
    if (desc && desc->ForcedSampleCount)
        DROPPED_ONCE(dev, "pfnCalcPrivateRasterizerStateSize", "a forced sample count");
    return dev->hosted.pfnCalcPrivateRasterizerStateSize(
        hDevice, reinterpret_cast<CONST D3D10_DDI_RASTERIZER_DESC *>(desc));
}

VOID APIENTRY CreateRasterizerState(D3D10DDI_HDEVICE hDevice, CONST D3D11_1_DDI_RASTERIZER_DESC *desc,
                                    D3D10DDI_HRASTERIZERSTATE hState,
                                    D3D10DDI_HRTRASTERIZERSTATE hRTState)
{
    FRONT_DEVICE(hDevice, )
    if (desc && desc->ForcedSampleCount)
        DROPPED_ONCE(dev, "pfnCreateRasterizerState", "a forced sample count");
    dev->hosted.pfnCreateRasterizerState(
        hDevice, reinterpret_cast<CONST D3D10_DDI_RASTERIZER_DESC *>(desc), hState, hRTState);
}

SIZE_T APIENTRY CalcPrivateShaderSize(D3D10DDI_HDEVICE hDevice, CONST UINT *code,
                                      CONST D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    FRONT_DEVICE(hDevice, 0)
    static volatile LONG once = 0;
    SignatureBuffer buffer;
    return dev->hosted.pfnCalcPrivateShaderSize(
        hDevice, code, Translate(dev, "pfnCalcPrivateShaderSize", signatures, &buffer, &once));
}

#define FRONT_CREATE_SHADER(Name)                                                                    \
    VOID APIENTRY Create##Name(D3D10DDI_HDEVICE hDevice, CONST UINT *code, D3D10DDI_HSHADER hShader,  \
                               D3D10DDI_HRTSHADER hRTShader,                                          \
                               CONST D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)                   \
    {                                                                                                 \
        FRONT_DEVICE(hDevice, )                                                                       \
        static volatile LONG once = 0;                                                                \
        SignatureBuffer buffer;                                                                       \
        dev->hosted.pfnCreate##Name(hDevice, code, hShader, hRTShader,                                \
                                    Translate(dev, "pfnCreate" #Name, signatures, &buffer, &once));   \
    }
FRONT_CREATE_SHADER(VertexShader)
FRONT_CREATE_SHADER(GeometryShader)
FRONT_CREATE_SHADER(PixelShader)
#undef FRONT_CREATE_SHADER

SIZE_T APIENTRY CalcPrivateGeometryShaderWithStreamOutput(
    D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *args,
    CONST D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    FRONT_DEVICE(hDevice, 0)
    static volatile LONG once = 0;
    StreamOutputBuffer so;
    SignatureBuffer buffer;
    if (!TranslateStreamOutput(dev, "pfnCalcPrivateGeometryShaderWithStreamOutput", args, &so, &once))
        return 0;
    return dev->hosted.pfnCalcPrivateGeometryShaderWithStreamOutput(
        hDevice, &so.arg,
        Translate(dev, "pfnCalcPrivateGeometryShaderWithStreamOutput", signatures, &buffer, &once));
}

VOID APIENTRY CreateGeometryShaderWithStreamOutput(
    D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *args,
    D3D10DDI_HSHADER hShader, D3D10DDI_HRTSHADER hRTShader,
    CONST D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    FRONT_DEVICE(hDevice, )
    static volatile LONG once = 0;
    StreamOutputBuffer so;
    SignatureBuffer buffer;
    if (!TranslateStreamOutput(dev, "pfnCreateGeometryShaderWithStreamOutput", args, &so, &once)) return;
    dev->hosted.pfnCreateGeometryShaderWithStreamOutput(
        hDevice, &so.arg, hShader, hRTShader,
        Translate(dev, "pfnCreateGeometryShaderWithStreamOutput", signatures, &buffer, &once));
}

VOID APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice)
{
    FRONT_DEVICE(hDevice, )
    LogPrintf(dev->adapter ? &dev->adapter->log : nullptr,
              "bc250d3d_front device=%p destroy direct_flip_calls=%ld direct_flip_true=%ld "
              "clear_view_calls=%ld clear_view_rect_calls=%ld refusals=%ld recorded_resources=%u\n",
              hDevice.pDrvPrivate, dev->direct_flip_calls, dev->direct_flip_true, dev->clear_view_calls,
              dev->clear_view_rect_calls, dev->refusals, RecordedResources());
    PFND3D10DDI_DESTROYDEVICE hosted = dev->hosted.pfnDestroyDevice;
    RetireDevice(dev);
    hosted(hDevice);
}

// ---------------------------------------------------------------- the real 11.1 bodies

// Nothing below the front holds a discardable copy of a resource or a view: the hosted frontend maps
// every resource straight onto a gallium one. A discard is therefore a hint the front may drop, and
// dropping it is correct rather than approximate.
VOID APIENTRY Discard(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE handleType, VOID *handle,
                      CONST D3D10_DDI_RECT *rects, UINT numRects)
{
    (void)hDevice;
    (void)handleType;
    (void)handle;
    (void)rects;
    (void)numRects;
}

// The front publishes AssignDebugBinarySupport FALSE, so the runtime has no reason to call this. A
// no-op is still the right body: the binary is a debugging aid and refusing it would be a device error.
VOID APIENTRY AssignDebugBinary(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader, UINT bytes,
                                CONST VOID *binary)
{
    (void)hDevice;
    (void)hShader;
    (void)bytes;
    (void)binary;
}

// The front publishes THREADING 0, so there are no deferred contexts and no handle sizes to report. The
// protocol is the count-then-fill one: write zero entries and nothing else.
VOID APIENTRY CheckDeferredContextHandleSizes(D3D10DDI_HDEVICE hDevice, UINT *sizes,
                                              D3D11DDI_HANDLESIZE *handleSizes)
{
    (void)hDevice;
    (void)handleSizes;
    if (sizes) *sizes = 0;
}

// Amendment 4, the route's one gate. The D3D10.0 DDI has no scissored clear: pfnClearRenderTargetView and
// pfnClearDepthStencilView clear a whole view. A rectangle list therefore cannot be expressed, and
// D3D11_1DDI_D3D11_OPTIONS_DATA has no bit to decline the entry - a refusal is silent. So the front
// forwards the whole-view shape, counts a rectangle call separately, and names it in the log. "Zero
// rectangle ClearView calls in the desktop arm" is an explicit pass condition of increment 1; if the
// compositor does call it with rectangles, this one entry moves into the Mesa frontend, where gallium's
// scissored clear_render_target can express it (DECISION-route.md section 1).
VOID APIENTRY ClearView(D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE viewType, VOID *hView,
                        CONST FLOAT color[4], CONST D3D10_DDI_RECT *rects, UINT numRects)
{
    (void)rects;  // a rectangle list is counted and named, never read: the D3D10.0 DDI cannot apply it
    FRONT_DEVICE(hDevice, )
    InterlockedIncrement(&dev->clear_view_calls);
    if (numRects) {
        InterlockedIncrement(&dev->clear_view_rect_calls);
        DROPPED_ONCE(dev, "pfnClearView", "a rectangle list, which the D3D10.0 DDI cannot express");
        return;
    }
    switch (viewType) {
    case D3D10DDI_HT_RENDERTARGETVIEW: {
        D3D10DDI_HRENDERTARGETVIEW view;
        view.pDrvPrivate = hView;
        // The D3D10.0 entry takes FLOAT[4] by value and not CONST FLOAT[4], so the colour is copied rather
        // than cast: a const cast here would be a promise about a callee the front does not own.
        FLOAT rgba[4] = {color[0], color[1], color[2], color[3]};
        dev->hosted.pfnClearRenderTargetView(hDevice, view, rgba);
        return;
    }
    case D3D10DDI_HT_DEPTHSTENCILVIEW: {
        // ClearView on a depth-stencil view clears depth to color[0]; there is no stencil value and no
        // stencil clear in this entry, so the clear flags name depth alone.
        D3D10DDI_HDEPTHSTENCILVIEW view;
        view.pDrvPrivate = hView;
        dev->hosted.pfnClearDepthStencilView(hDevice, view, D3D10_DDI_CLEAR_DEPTH, color[0], 0);
        return;
    }
    default:
        DROPPED_ONCE(dev, "pfnClearView", "a view type the D3D10.0 DDI has no whole-view clear for");
        return;
    }
}

// The entry this whole increment exists for. It counts the call, reads the kernel driver's scan-out caps
// trailer, applies the rule of front-direct-flip.h and writes its answer: TRUE exactly when every clause
// holds. One log line carries the answer, the rule's word, the trailer's flag and source geometry, and
// both surfaces with their geometry, pitch, format and record.
//
// The trailer is read again for every call and never latched. The runtime asks at least once before the
// compositor presents to a DirectFlip swap chain, again after each mode change and after the compositor
// re-creates its own swap chain (ref/ddi-display/d3d10umddi.md:8781), so this is not a per-frame cost, and a
// fresh read is what lets the geometry clause follow the source mode the kernel driver admits now rather
// than the one of the adapter open. A failed read leaves the trailer zero, which the rule's first clause
// answers "gated": the answer is then the one this stack gave before M15.14.
//
// The switches are the existing two. `DirectFlipFront` 0 removes the front, and with it this entry, from the
// compositor's device altogether (router.cpp). `EnableDirectFlipHandshake` 0 in the kernel driver, or any
// closed flip-path gate behind it, writes no trailer, and the rule answers "gated". There is no third
// switch: a TRUE needs both, and each one alone returns the desktop to composition.
//
// Why a TRUE is safe to give: the kernel driver re-derives the format, the geometry, the pitch, the size,
// the segment and the 4 KiB base at every SetVidPnSourceAddress (scanout_admit.h), and the rule here asks
// the same questions first from the same words, so that the only clauses left to refuse a flip after a TRUE
// are the base address and the segment, which VidMm decides for an allocation it pinned to scan out. A
// wrong TRUE is not a lost optimisation: after SharedPrimaryTransition the operating system does not fall
// back to composition (F19, front-direct-flip.h).
VOID APIENTRY CheckDirectFlipSupport(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource1,
                                     D3D10DDI_HRESOURCE hResource2, UINT checkFlags, BOOL *supported)
{
    if (supported) *supported = FALSE;  // before anything that could fail
    FRONT_DEVICE(hDevice, )
    const LONG call = InterlockedIncrement(&dev->direct_flip_calls);
    // The two causes of a FALSE must not print the same word. `handle` is the operating system passing a
    // null handle or the same surface twice; `record` is the front never having seen this surface
    // (FindResource misses, so the struct stays zeroed and `recorded` is false). Both structs are therefore
    // passed whenever the handles themselves are sound.
    Resource client_record, compositor_record;
    const bool found_client = FindResource(hResource1.pDrvPrivate, &client_record);
    const bool found_compositor = FindResource(hResource2.pDrvPrivate, &compositor_record);
    const bool bad_handles = !hResource1.pDrvPrivate || !hResource2.pDrvPrivate ||
                             hResource1.pDrvPrivate == hResource2.pDrvPrivate;
    const Resource *client = bad_handles ? nullptr : &client_record;
    const Resource *compositor = bad_handles ? nullptr : &compositor_record;
    bc250_scanout_caps caps;
    const HRESULT query = ReadScanoutCaps(dev->adapter, &caps);
    const FlipRefusal reason = FlipReason(caps, client, compositor);
    const bool answer = reason == FlipRefusal::none;
    if (answer) InterlockedIncrement(&dev->direct_flip_true);
    if (supported) *supported = answer ? TRUE : FALSE;
    // The log is bounded: the compositor asks at least once per swap-chain creation and after every mode
    // change, and a flood would cost more than it tells. The counters above are never bounded, and the
    // device's destroy line prints them.
    if (InterlockedIncrement(&dev->direct_flip_logged) <= 256)
        LogPrintf(dev->adapter ? &dev->adapter->log : nullptr,
                  "bc250d3d_front check_direct_flip call=%ld flags=%08x answer=%u rule=%s "
                  "caps_query=%08lx caps_flags=%08x source=%ux%u "
                  "client=%p recorded=%u opened=%u primary=%u shared=%u %ux%u pitch=%u fmt=%u "
                  "record_v=%lu record_access=%lu "
                  "compositor=%p recorded=%u opened=%u primary=%u shared=%u %ux%u pitch=%u fmt=%u "
                  "record_v=%lu record_access=%lu\n",
                  call, checkFlags, answer ? 1u : 0u, FlipRefusalText(reason), (unsigned long)query,
                  caps.flags, caps.post_width, caps.post_height, hResource1.pDrvPrivate,
                  found_client ? 1u : 0u, client && client->opened ? 1u : 0u,
                  client && client->primary ? 1u : 0u, client && client->shared ? 1u : 0u,
                  client ? client->width : 0u, client ? client->height : 0u, client ? client->pitch : 0u,
                  client ? client->format : 0u, client ? client->record.Version : 0ul,
                  client ? client->record.Access : 0ul, hResource2.pDrvPrivate,
                  found_compositor ? 1u : 0u, compositor && compositor->opened ? 1u : 0u,
                  compositor && compositor->primary ? 1u : 0u, compositor && compositor->shared ? 1u : 0u,
                  compositor ? compositor->width : 0u, compositor ? compositor->height : 0u,
                  compositor ? compositor->pitch : 0u, compositor ? compositor->format : 0u,
                  compositor ? compositor->record.Version : 0ul,
                  compositor ? compositor->record.Access : 0ul);
}

// ---------------------------------------------------------------- the refusals

// One body per signature, each naming its own entry. Every one of these is an entry a device at 3D
// pipeline level 10_0 cannot reach; a call here means the runtime asked for something the front's own
// GetCaps said it did not have, and the line is the evidence.
#define FRONT_REFUSE_VOID(Name, Params, Args)                     \
    VOID APIENTRY Name Params                                     \
    {                                                             \
        Args                                                      \
        FRONT_DEVICE(hDevice, )                                   \
        REFUSE_ONCE(dev, "pfn" #Name);                            \
    }
#define FRONT_REFUSE_SIZE(Name, Params, Args)                     \
    SIZE_T APIENTRY Name Params                                   \
    {                                                             \
        Args                                                      \
        FRONT_DEVICE(hDevice, 0)                                  \
        REFUSE_ONCE(dev, "pfn" #Name);                            \
        return 0;                                                 \
    }
#define FRONT_REFUSE_HRESULT(Name, Params, Args)                  \
    HRESULT APIENTRY Name Params                                  \
    {                                                             \
        Args                                                      \
        FRONT_DEVICE(hDevice, E_NOTIMPL)                          \
        REFUSE_ONCE(dev, "pfn" #Name);                            \
        return E_NOTIMPL;                                         \
    }
// A refused CREATE. The entry is VOID, so the runtime is told through pfnSetErrorCb; without that it holds a
// handle over an uninitialised private block and hands it to the hosted driver's own Destroy* later
// (front-adapter.h, CreateFailed). Every refusing create of this table uses this macro and not the plain one.
#define FRONT_REFUSE_CREATE(Name, Params, Args)                   \
    VOID APIENTRY Name Params                                     \
    {                                                             \
        Args                                                      \
        FRONT_DEVICE(hDevice, )                                    \
        REFUSE_ONCE(dev, "pfn" #Name);                            \
        CreateFailed(dev);                                        \
    }

// Indirect draws (11.0).
FRONT_REFUSE_VOID(DrawIndexedInstancedIndirect,
                  (D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE a, UINT b),
                  (void)a; (void)b;)
FRONT_REFUSE_VOID(DrawInstancedIndirect, (D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE a, UINT b),
                  (void)a; (void)b;)
// Hull, domain and compute stage setters (11.0 and tessellation/compute).
#define FRONT_REFUSE_STAGE(Stage)                                                                     \
    FRONT_REFUSE_VOID(Stage##SetShaderResources,                                                      \
                      (D3D10DDI_HDEVICE hDevice, UINT a, UINT b, CONST D3D10DDI_HSHADERRESOURCEVIEW *c), \
                      (void)a; (void)b; (void)c;)                                                      \
    FRONT_REFUSE_VOID(Stage##SetShader, (D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER a), (void)a;)      \
    FRONT_REFUSE_VOID(Stage##SetSamplers,                                                             \
                      (D3D10DDI_HDEVICE hDevice, UINT a, UINT b, CONST D3D10DDI_HSAMPLER *c),         \
                      (void)a; (void)b; (void)c;)                                                      \
    FRONT_REFUSE_VOID(Stage##SetConstantBuffers,                                                      \
                      (D3D10DDI_HDEVICE hDevice, UINT a, UINT b, CONST D3D10DDI_HRESOURCE *c,         \
                       CONST UINT *d, CONST UINT *e),                                                 \
                      (void)a; (void)b; (void)c; (void)d; (void)e;)
FRONT_REFUSE_STAGE(Hs)
FRONT_REFUSE_STAGE(Ds)
FRONT_REFUSE_STAGE(Cs)
#undef FRONT_REFUSE_STAGE
// Tessellation and compute shader creation.
FRONT_REFUSE_CREATE(CreateHullShader,
                  (D3D10DDI_HDEVICE hDevice, CONST UINT *a, D3D10DDI_HSHADER b, D3D10DDI_HRTSHADER c,
                   CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *d),
                  (void)a; (void)b; (void)c; (void)d;)
FRONT_REFUSE_CREATE(CreateDomainShader,
                  (D3D10DDI_HDEVICE hDevice, CONST UINT *a, D3D10DDI_HSHADER b, D3D10DDI_HRTSHADER c,
                   CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *d),
                  (void)a; (void)b; (void)c; (void)d;)
FRONT_REFUSE_SIZE(CalcPrivateTessellationShaderSize,
                  (D3D10DDI_HDEVICE hDevice, CONST UINT *a,
                   CONST D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *b),
                  (void)a; (void)b;)
FRONT_REFUSE_CREATE(CreateComputeShader,
                  (D3D10DDI_HDEVICE hDevice, CONST UINT *a, D3D10DDI_HSHADER b, D3D10DDI_HRTSHADER c),
                  (void)a; (void)b; (void)c;)
// Command lists and deferred contexts: THREADING 0 excludes every one of them.
FRONT_REFUSE_VOID(CommandListExecute, (D3D10DDI_HDEVICE hDevice, D3D11DDI_HCOMMANDLIST a), (void)a;)
FRONT_REFUSE_SIZE(CalcDeferredContextHandleSize,
                  (D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE a, VOID *b), (void)a; (void)b;)
FRONT_REFUSE_SIZE(CalcPrivateDeferredContextSize,
                  (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *a),
                  (void)a;)
FRONT_REFUSE_CREATE(CreateDeferredContext,
                  (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATEDEFERREDCONTEXT *a), (void)a;)
FRONT_REFUSE_VOID(AbandonCommandList, (D3D10DDI_HDEVICE hDevice), )
FRONT_REFUSE_SIZE(CalcPrivateCommandListSize,
                  (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATECOMMANDLIST *a), (void)a;)
FRONT_REFUSE_CREATE(CreateCommandList,
                  (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATECOMMANDLIST *a,
                   D3D11DDI_HCOMMANDLIST b, D3D11DDI_HRTCOMMANDLIST c),
                  (void)a; (void)b; (void)c;)
FRONT_REFUSE_VOID(DestroyCommandList, (D3D10DDI_HDEVICE hDevice, D3D11DDI_HCOMMANDLIST a), (void)a;)
FRONT_REFUSE_VOID(RecycleCommandList, (D3D10DDI_HDEVICE hDevice, D3D11DDI_HCOMMANDLIST a), (void)a;)
FRONT_REFUSE_HRESULT(RecycleCreateCommandList,
                     (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATECOMMANDLIST *a,
                      D3D11DDI_HCOMMANDLIST b, D3D11DDI_HRTCOMMANDLIST c),
                     (void)a; (void)b; (void)c;)
FRONT_REFUSE_HRESULT(RecycleCreateDeferredContext,
                     (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATEDEFERREDCONTEXT *a), (void)a;)
FRONT_REFUSE_VOID(RecycleDestroyCommandList, (D3D10DDI_HDEVICE hDevice, D3D11DDI_HCOMMANDLIST a),
                  (void)a;)
// Shader interfaces (11.0 class instances). The three stages the hosted driver has are forwarded when the
// interface count is zero, which is every call a 10_0 pipeline can make; the other three have no hosted
// entry at all.
#define FRONT_SET_SHADER_WITH_IFACES(Stage)                                                           \
    VOID APIENTRY Stage##SetShaderWithIfaces(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER hShader,       \
                                             UINT numClassInstances, CONST UINT *a,                   \
                                             CONST D3D11DDIARG_POINTERDATA *b)                        \
    {                                                                                                 \
        (void)a;                                                                                      \
        (void)b;                                                                                      \
        FRONT_DEVICE(hDevice, )                                                                       \
        if (numClassInstances) {                                                                      \
            REFUSE_ONCE(dev, "pfn" #Stage "SetShaderWithIfaces");                                      \
            return;                                                                                   \
        }                                                                                             \
        dev->hosted.pfn##Stage##SetShader(hDevice, hShader);                                           \
    }
FRONT_SET_SHADER_WITH_IFACES(Ps)
FRONT_SET_SHADER_WITH_IFACES(Vs)
FRONT_SET_SHADER_WITH_IFACES(Gs)
#undef FRONT_SET_SHADER_WITH_IFACES
FRONT_REFUSE_VOID(HsSetShaderWithIfaces,
                  (D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER a, UINT b, CONST UINT *c,
                   CONST D3D11DDIARG_POINTERDATA *d),
                  (void)a; (void)b; (void)c; (void)d;)
FRONT_REFUSE_VOID(DsSetShaderWithIfaces,
                  (D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER a, UINT b, CONST UINT *c,
                   CONST D3D11DDIARG_POINTERDATA *d),
                  (void)a; (void)b; (void)c; (void)d;)
FRONT_REFUSE_VOID(CsSetShaderWithIfaces,
                  (D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER a, UINT b, CONST UINT *c,
                   CONST D3D11DDIARG_POINTERDATA *d),
                  (void)a; (void)b; (void)c; (void)d;)
// Unordered access and compute dispatch (11.0).
FRONT_REFUSE_SIZE(CalcPrivateUnorderedAccessViewSize,
                  (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *a), (void)a;)
FRONT_REFUSE_CREATE(CreateUnorderedAccessView,
                  (D3D10DDI_HDEVICE hDevice, CONST D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *a,
                   D3D11DDI_HUNORDEREDACCESSVIEW b, D3D11DDI_HRTUNORDEREDACCESSVIEW c),
                  (void)a; (void)b; (void)c;)
FRONT_REFUSE_VOID(DestroyUnorderedAccessView,
                  (D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW a), (void)a;)
FRONT_REFUSE_VOID(ClearUnorderedAccessViewUint,
                  (D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW a, CONST UINT b[4]),
                  (void)a; (void)b;)
FRONT_REFUSE_VOID(ClearUnorderedAccessViewFloat,
                  (D3D10DDI_HDEVICE hDevice, D3D11DDI_HUNORDEREDACCESSVIEW a, CONST FLOAT b[4]),
                  (void)a; (void)b;)
FRONT_REFUSE_VOID(CsSetUnorderedAccessViews,
                  (D3D10DDI_HDEVICE hDevice, UINT a, UINT b, CONST D3D11DDI_HUNORDEREDACCESSVIEW *c,
                   CONST UINT *d),
                  (void)a; (void)b; (void)c; (void)d;)
FRONT_REFUSE_VOID(Dispatch, (D3D10DDI_HDEVICE hDevice, UINT a, UINT b, UINT c),
                  (void)a; (void)b; (void)c;)
FRONT_REFUSE_VOID(DispatchIndirect, (D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE a, UINT b),
                  (void)a; (void)b;)
FRONT_REFUSE_VOID(SetResourceMinLOD, (D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE a, FLOAT b),
                  (void)a; (void)b;)
FRONT_REFUSE_VOID(CopyStructureCount,
                  (D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE a, UINT b,
                   D3D11DDI_HUNORDEREDACCESSVIEW c),
                  (void)a; (void)b; (void)c;)

#undef FRONT_REFUSE_CREATE
#undef FRONT_REFUSE_VOID
#undef FRONT_REFUSE_SIZE
#undef FRONT_REFUSE_HRESULT

}  // namespace

// ---------------------------------------------------------------- the table

void FillDeviceFuncs(D3D11_1DDI_DEVICEFUNCS *out, const D3D10DDI_DEVICEFUNCS &hosted)
{
    if (!out) return;
    ZeroMemory(out, sizeof(*out));
    //  0 .. 17  the high-frequency block
    out->pfnDefaultConstantBufferUpdateSubresourceUP = DefaultConstantBufferUpdateSubresourceUP;
    out->pfnVsSetConstantBuffers = VsSetConstantBuffers;
    out->pfnPsSetShaderResources = hosted.pfnPsSetShaderResources;
    out->pfnPsSetShader = hosted.pfnPsSetShader;
    out->pfnPsSetSamplers = hosted.pfnPsSetSamplers;
    out->pfnVsSetShader = hosted.pfnVsSetShader;
    out->pfnDrawIndexed = hosted.pfnDrawIndexed;
    out->pfnDraw = hosted.pfnDraw;
    out->pfnDynamicIABufferMapNoOverwrite = hosted.pfnDynamicIABufferMapNoOverwrite;
    out->pfnDynamicIABufferUnmap = hosted.pfnDynamicIABufferUnmap;
    out->pfnDynamicConstantBufferMapDiscard = hosted.pfnDynamicConstantBufferMapDiscard;
    out->pfnDynamicIABufferMapDiscard = hosted.pfnDynamicIABufferMapDiscard;
    out->pfnDynamicConstantBufferUnmap = hosted.pfnDynamicConstantBufferUnmap;
    out->pfnPsSetConstantBuffers = PsSetConstantBuffers;
    out->pfnIaSetInputLayout = hosted.pfnIaSetInputLayout;
    out->pfnIaSetVertexBuffers = hosted.pfnIaSetVertexBuffers;
    out->pfnIaSetIndexBuffer = hosted.pfnIaSetIndexBuffer;
    // 18 .. 51  the middle-frequency block
    out->pfnDrawIndexedInstanced = hosted.pfnDrawIndexedInstanced;
    out->pfnDrawInstanced = hosted.pfnDrawInstanced;
    out->pfnDynamicResourceMapDiscard = hosted.pfnDynamicResourceMapDiscard;
    out->pfnDynamicResourceUnmap = hosted.pfnDynamicResourceUnmap;
    out->pfnGsSetConstantBuffers = GsSetConstantBuffers;
    out->pfnGsSetShader = hosted.pfnGsSetShader;
    out->pfnIaSetTopology = hosted.pfnIaSetTopology;
    out->pfnStagingResourceMap = hosted.pfnStagingResourceMap;
    out->pfnStagingResourceUnmap = hosted.pfnStagingResourceUnmap;
    out->pfnVsSetShaderResources = hosted.pfnVsSetShaderResources;
    out->pfnVsSetSamplers = hosted.pfnVsSetSamplers;
    out->pfnGsSetShaderResources = hosted.pfnGsSetShaderResources;
    out->pfnGsSetSamplers = hosted.pfnGsSetSamplers;
    out->pfnSetRenderTargets = SetRenderTargets;
    out->pfnShaderResourceViewReadAfterWriteHazard = hosted.pfnShaderResourceViewReadAfterWriteHazard;
    out->pfnResourceReadAfterWriteHazard = hosted.pfnResourceReadAfterWriteHazard;
    out->pfnSetBlendState = hosted.pfnSetBlendState;
    out->pfnSetDepthStencilState = hosted.pfnSetDepthStencilState;
    out->pfnSetRasterizerState = hosted.pfnSetRasterizerState;
    out->pfnQueryEnd = hosted.pfnQueryEnd;
    out->pfnQueryBegin = hosted.pfnQueryBegin;
    out->pfnResourceCopyRegion = ResourceCopyRegion;
    out->pfnResourceUpdateSubresourceUP = ResourceUpdateSubresourceUP;
    out->pfnSoSetTargets = hosted.pfnSoSetTargets;
    out->pfnDrawAuto = hosted.pfnDrawAuto;
    out->pfnSetViewports = hosted.pfnSetViewports;
    out->pfnSetScissorRects = hosted.pfnSetScissorRects;
    out->pfnClearRenderTargetView = hosted.pfnClearRenderTargetView;
    out->pfnClearDepthStencilView = hosted.pfnClearDepthStencilView;
    out->pfnSetPredication = hosted.pfnSetPredication;
    out->pfnQueryGetData = hosted.pfnQueryGetData;
    out->pfnFlush = Flush;
    out->pfnGenMips = hosted.pfnGenMips;
    out->pfnResourceCopy = hosted.pfnResourceCopy;
    out->pfnResourceResolveSubresource = hosted.pfnResourceResolveSubresource;
    // 52 .. 100  the infrequent paths
    out->pfnResourceMap = hosted.pfnResourceMap;
    out->pfnResourceUnmap = hosted.pfnResourceUnmap;
    out->pfnResourceIsStagingBusy = hosted.pfnResourceIsStagingBusy;
    out->pfnRelocateDeviceFuncs = RelocateDeviceFuncs;
    out->pfnCalcPrivateResourceSize = CalcPrivateResourceSize;
    out->pfnCalcPrivateOpenedResourceSize = hosted.pfnCalcPrivateOpenedResourceSize;
    out->pfnCreateResource = CreateResource;
    out->pfnOpenResource = OpenResource;
    out->pfnDestroyResource = DestroyResource;
    out->pfnCalcPrivateShaderResourceViewSize = CalcPrivateShaderResourceViewSize;
    out->pfnCreateShaderResourceView = CreateShaderResourceView;
    out->pfnDestroyShaderResourceView = hosted.pfnDestroyShaderResourceView;
    out->pfnCalcPrivateRenderTargetViewSize = hosted.pfnCalcPrivateRenderTargetViewSize;
    out->pfnCreateRenderTargetView = hosted.pfnCreateRenderTargetView;
    out->pfnDestroyRenderTargetView = hosted.pfnDestroyRenderTargetView;
    out->pfnCalcPrivateDepthStencilViewSize = CalcPrivateDepthStencilViewSize;
    out->pfnCreateDepthStencilView = CreateDepthStencilView;
    out->pfnDestroyDepthStencilView = hosted.pfnDestroyDepthStencilView;
    out->pfnCalcPrivateElementLayoutSize = hosted.pfnCalcPrivateElementLayoutSize;
    out->pfnCreateElementLayout = hosted.pfnCreateElementLayout;
    out->pfnDestroyElementLayout = hosted.pfnDestroyElementLayout;
    out->pfnCalcPrivateBlendStateSize = CalcPrivateBlendStateSize;
    out->pfnCreateBlendState = CreateBlendState;
    out->pfnDestroyBlendState = hosted.pfnDestroyBlendState;
    out->pfnCalcPrivateDepthStencilStateSize = hosted.pfnCalcPrivateDepthStencilStateSize;
    out->pfnCreateDepthStencilState = hosted.pfnCreateDepthStencilState;
    out->pfnDestroyDepthStencilState = hosted.pfnDestroyDepthStencilState;
    out->pfnCalcPrivateRasterizerStateSize = CalcPrivateRasterizerStateSize;
    out->pfnCreateRasterizerState = CreateRasterizerState;
    out->pfnDestroyRasterizerState = hosted.pfnDestroyRasterizerState;
    out->pfnCalcPrivateShaderSize = CalcPrivateShaderSize;
    out->pfnCreateVertexShader = CreateVertexShader;
    out->pfnCreateGeometryShader = CreateGeometryShader;
    out->pfnCreatePixelShader = CreatePixelShader;
    out->pfnCalcPrivateGeometryShaderWithStreamOutput = CalcPrivateGeometryShaderWithStreamOutput;
    out->pfnCreateGeometryShaderWithStreamOutput = CreateGeometryShaderWithStreamOutput;
    out->pfnDestroyShader = hosted.pfnDestroyShader;
    out->pfnCalcPrivateSamplerSize = hosted.pfnCalcPrivateSamplerSize;
    out->pfnCreateSampler = hosted.pfnCreateSampler;
    out->pfnDestroySampler = hosted.pfnDestroySampler;
    out->pfnCalcPrivateQuerySize = hosted.pfnCalcPrivateQuerySize;
    out->pfnCreateQuery = hosted.pfnCreateQuery;
    out->pfnDestroyQuery = hosted.pfnDestroyQuery;
    out->pfnCheckFormatSupport = hosted.pfnCheckFormatSupport;
    out->pfnCheckMultisampleQualityLevels = hosted.pfnCheckMultisampleQualityLevels;
    out->pfnCheckCounterInfo = hosted.pfnCheckCounterInfo;
    out->pfnCheckCounter = hosted.pfnCheckCounter;
    out->pfnDestroyDevice = DestroyDevice;
    out->pfnSetTextFilterSize = hosted.pfnSetTextFilterSize;
    // 101 .. 102  the 10.1 convert entries, as the hosted frontend itself maps them
    out->pfnResourceConvert = hosted.pfnResourceCopy;
    out->pfnResourceConvertRegion = ResourceConvertRegion;
    // 103 .. 149  the 11.0 entries
    out->pfnDrawIndexedInstancedIndirect = DrawIndexedInstancedIndirect;
    out->pfnDrawInstancedIndirect = DrawInstancedIndirect;
    out->pfnCommandListExecute = CommandListExecute;
    out->pfnHsSetShaderResources = HsSetShaderResources;
    out->pfnHsSetShader = HsSetShader;
    out->pfnHsSetSamplers = HsSetSamplers;
    out->pfnHsSetConstantBuffers = HsSetConstantBuffers;
    out->pfnDsSetShaderResources = DsSetShaderResources;
    out->pfnDsSetShader = DsSetShader;
    out->pfnDsSetSamplers = DsSetSamplers;
    out->pfnDsSetConstantBuffers = DsSetConstantBuffers;
    out->pfnCreateHullShader = CreateHullShader;
    out->pfnCreateDomainShader = CreateDomainShader;
    out->pfnCheckDeferredContextHandleSizes = CheckDeferredContextHandleSizes;
    out->pfnCalcDeferredContextHandleSize = CalcDeferredContextHandleSize;
    out->pfnCalcPrivateDeferredContextSize = CalcPrivateDeferredContextSize;
    out->pfnCreateDeferredContext = CreateDeferredContext;
    out->pfnAbandonCommandList = AbandonCommandList;
    out->pfnCalcPrivateCommandListSize = CalcPrivateCommandListSize;
    out->pfnCreateCommandList = CreateCommandList;
    out->pfnDestroyCommandList = DestroyCommandList;
    out->pfnCalcPrivateTessellationShaderSize = CalcPrivateTessellationShaderSize;
    out->pfnPsSetShaderWithIfaces = PsSetShaderWithIfaces;
    out->pfnVsSetShaderWithIfaces = VsSetShaderWithIfaces;
    out->pfnGsSetShaderWithIfaces = GsSetShaderWithIfaces;
    out->pfnHsSetShaderWithIfaces = HsSetShaderWithIfaces;
    out->pfnDsSetShaderWithIfaces = DsSetShaderWithIfaces;
    out->pfnCsSetShaderWithIfaces = CsSetShaderWithIfaces;
    out->pfnCreateComputeShader = CreateComputeShader;
    out->pfnCsSetShader = CsSetShader;
    out->pfnCsSetShaderResources = CsSetShaderResources;
    out->pfnCsSetSamplers = CsSetSamplers;
    out->pfnCsSetConstantBuffers = CsSetConstantBuffers;
    out->pfnCalcPrivateUnorderedAccessViewSize = CalcPrivateUnorderedAccessViewSize;
    out->pfnCreateUnorderedAccessView = CreateUnorderedAccessView;
    out->pfnDestroyUnorderedAccessView = DestroyUnorderedAccessView;
    out->pfnClearUnorderedAccessViewUint = ClearUnorderedAccessViewUint;
    out->pfnClearUnorderedAccessViewFloat = ClearUnorderedAccessViewFloat;
    out->pfnCsSetUnorderedAccessViews = CsSetUnorderedAccessViews;
    out->pfnDispatch = Dispatch;
    out->pfnDispatchIndirect = DispatchIndirect;
    out->pfnSetResourceMinLOD = SetResourceMinLOD;
    out->pfnCopyStructureCount = CopyStructureCount;
    out->pfnRecycleCommandList = RecycleCommandList;
    out->pfnRecycleCreateCommandList = RecycleCreateCommandList;
    out->pfnRecycleCreateDeferredContext = RecycleCreateDeferredContext;
    out->pfnRecycleDestroyCommandList = RecycleDestroyCommandList;
    // 150 .. 154  the 11.1 entries
    out->pfnDiscard = Discard;
    out->pfnAssignDebugBinary = AssignDebugBinary;
    out->pfnDynamicConstantBufferMapNoOverwrite = hosted.pfnResourceMap;
    out->pfnCheckDirectFlipSupport = CheckDirectFlipSupport;
    out->pfnClearView = ClearView;
}

// ---------------------------------------------------------------- the resource map

namespace {

// Open addressing with a tombstone, so that a deletion does not break the probe chain of an entry behind
// it and a miss still stops at the first slot that was never used. `kGone` is a pointer value no private
// block can have.
void *const kGone = (void *)(UINT_PTR)1;
struct Slot {
    void *handle;  // 0: never used; kGone: deleted
    Resource resource;
};
SRWLOCK ResourcesLock = SRWLOCK_INIT;
Slot Resources[kResourceSlots];
unsigned int ResourceCount;

unsigned int SlotOf(void *handle)
{
    // The handles are pointers into the runtime's private blocks, so the low bits are the allocator's
    // alignment and carry no entropy; the shift drops them.
    const UINT_PTR value = (UINT_PTR)handle;
    return (unsigned int)((value >> 4) & (kResourceSlots - 1));
}

}  // namespace

bool RecordResource(Device *device, void *handle, const Resource &resource)
{
    if (!handle || handle == kGone) return false;
    bool recorded = false;
    AcquireSRWLockExclusive(&ResourcesLock);
    const unsigned int start = SlotOf(handle);
    unsigned int free_slot = kResourceSlots;
    for (unsigned int i = 0; i < kResourceSlots; ++i) {
        const unsigned int index = (start + i) & (kResourceSlots - 1);
        Slot &slot = Resources[index];
        if (slot.handle == handle) {  // a handle the runtime reused: the new record replaces the old
            slot.resource = resource;
            recorded = true;
            break;
        }
        if ((!slot.handle || slot.handle == kGone) && free_slot == kResourceSlots) free_slot = index;
        if (!slot.handle) break;  // never used: the handle is not in this chain
    }
    if (!recorded && free_slot < kResourceSlots) {
        Resources[free_slot].handle = handle;
        Resources[free_slot].resource = resource;
        ++ResourceCount;
        recorded = true;
    }
    ReleaseSRWLockExclusive(&ResourcesLock);
    if (!recorded) {
        static volatile LONG once = 0;
        Dropped(device, "resource-map", "no free slot: further surfaces are answered FALSE", &once);
    }
    return recorded;
}

void ForgetResource(void *handle)
{
    if (!handle || handle == kGone) return;
    AcquireSRWLockExclusive(&ResourcesLock);
    const unsigned int start = SlotOf(handle);
    for (unsigned int i = 0; i < kResourceSlots; ++i) {
        Slot &slot = Resources[(start + i) & (kResourceSlots - 1)];
        if (!slot.handle) break;
        if (slot.handle != handle) continue;
        ZeroMemory(&slot.resource, sizeof(slot.resource));
        slot.handle = kGone;
        if (ResourceCount) --ResourceCount;
        break;
    }
    ReleaseSRWLockExclusive(&ResourcesLock);
}

bool FindResource(void *handle, Resource *out)
{
    ZeroMemory(out, sizeof(*out));
    if (!handle || handle == kGone) return false;
    bool found = false;
    AcquireSRWLockShared(&ResourcesLock);
    const unsigned int start = SlotOf(handle);
    for (unsigned int i = 0; i < kResourceSlots; ++i) {
        const Slot &slot = Resources[(start + i) & (kResourceSlots - 1)];
        if (!slot.handle) break;
        if (slot.handle != handle) continue;
        *out = slot.resource;
        found = true;
        break;
    }
    ReleaseSRWLockShared(&ResourcesLock);
    return found;
}

unsigned int RecordedResources()
{
    AcquireSRWLockShared(&ResourcesLock);
    const unsigned int count = ResourceCount;
    ReleaseSRWLockShared(&ResourcesLock);
    return count;
}

void ResetResources()
{
    AcquireSRWLockExclusive(&ResourcesLock);
    ZeroMemory(Resources, sizeof(Resources));
    ResourceCount = 0;
    ReleaseSRWLockExclusive(&ResourcesLock);
}

}  // namespace bc250front
