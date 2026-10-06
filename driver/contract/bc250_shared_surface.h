/* SPDX-License-Identifier: MIT */
/*
 * driver/contract/bc250_shared_surface.h - the one reader and the one writer of a shared linear
 * surface's two private-data records.
 *
 * A surface this driver shares between processes or between APIs is described on the wire by two
 * blobs the kernel driver already defines:
 *
 *   per allocation  BC250_WDDM_ALLOCATION_PRIVATE  "LB7A", version 1, exactly 32 bytes
 *                   (driver/kmd/gdi_private.h): Width, Height, Pitch, Format (a D3DDDIFORMAT),
 *                   Size.
 *   per resource    BC250_SURFACE_RESOURCE_PRIVATE "E26R", version 3, exactly 64 bytes
 *                   (driver/kmd/surface_resource_private.h): Shared, Access and a serialized
 *                   D3D11_TEXTURE2D_DESC1.
 *
 * Before this header each shell carried its own copy of the rules that admit that pair, so a D3D12
 * surface and a D3D11 surface could differ in a field neither side checked. BD-075: the D3D12 shell
 * now creates exactly the records the D3D11 shell reads and writes, and both decode them here.
 * The texture description is a D3D11 one on the wire and stays one; each shell maps the neutral
 * BC250_SHARED_SURFACE below to its own API, and neither keeps a second copy of the rules.
 *
 * Plain integers and no API headers, so the kernel driver's C, the two shells' C++ and the Mesa
 * winsys's C can all include it. The D3D11 bind-flag and misc-flag numbers below are the wire's;
 * a consumer with the SDK headers static_asserts them against the enumerations.
 *
 * What this header does NOT decide: whether a given opener admits a record that is a primary, or
 * one that asks for a cached CPU mapping. That is policy, it differs between the two shells, and it
 * is the BC250_SHARED_SURFACE_ADMIT argument of Bc250SharedSurfaceDecodeAdmitted. The strict form,
 * Bc250SharedSurfaceDecode, is the D3D12 shell's: a shared texture and nothing else.
 */
#ifndef BC250_SHARED_SURFACE_H
#define BC250_SHARED_SURFACE_H

#include "../kmd/gdi_private.h"
#include "../kmd/surface_resource_private.h"
#include "amdgpu_wddm_surface_format.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* The wire numbers of the D3D11 fields E26R v3 carries. A consumer with d3d11.h checks them. */
#define BC250_SHARED_BIND_SHADER_RESOURCE  0x8ul   /* D3D11_BIND_SHADER_RESOURCE */
#define BC250_SHARED_BIND_RENDER_TARGET    0x20ul  /* D3D11_BIND_RENDER_TARGET */
#define BC250_SHARED_BIND_UNORDERED_ACCESS 0x80ul  /* D3D11_BIND_UNORDERED_ACCESS */
#define BC250_SHARED_BIND_MASK (BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET | \
                                BC250_SHARED_BIND_UNORDERED_ACCESS)
#define BC250_SHARED_MISC_GENERATE_MIPS  0x1ul     /* D3D11_RESOURCE_MISC_GENERATE_MIPS */
#define BC250_SHARED_MISC_RESOURCE_CLAMP 0x80ul    /* D3D11_RESOURCE_MISC_RESOURCE_CLAMP */
#define BC250_SHARED_MISC_MASK (BC250_SHARED_MISC_GENERATE_MIPS | BC250_SHARED_MISC_RESOURCE_CLAMP)
#define BC250_SHARED_USAGE_DEFAULT      0ul        /* D3D11_USAGE_DEFAULT */
#define BC250_SHARED_LAYOUT_UNDEFINED   0ul        /* D3D11_TEXTURE_LAYOUT_UNDEFINED */
#define BC250_SHARED_MAX_EDGE       16384ul

/* The status of an encode or a decode. The two refusals are kept apart because the shells report
 * them differently: a format with no row of the surface format table is "not implemented", every
 * other refusal is a malformed argument. */
#define BC250_SHARED_SURFACE_OK        0
#define BC250_SHARED_SURFACE_MALFORMED 1
#define BC250_SHARED_SURFACE_FORMAT    2

/* Which check a decode decided at. One status covers seventeen checks, and a refusal line that says
 * only "record" makes a reader derive the reason from the fields printed beside it: in round 1 of
 * BD-075 a cross-API open declined because the other shell wrote a 16-byte E26R v2 record where this
 * one admits 64-byte v3, and the line did carry "resource private data 16 bytes (version 2)" - what
 * cost a lab pass was that the kit never surfaced the line (2026-10-06). The name puts the reason
 * first; it adds no information the record did not already have. Every string is a literal of this
 * header, so a caller may keep the pointer and print it. They are also what
 * docs/d3d12-shared-resources.md lists and what the lab kit matches, so a name is part of the
 * contract and is not renamed silently. */
#define BC250_SHARED_SURFACE_WHY_OK                   "record"
#define BC250_SHARED_SURFACE_WHY_ARGUMENTS            "record arguments"
#define BC250_SHARED_SURFACE_WHY_LENGTH               "record length"
#define BC250_SHARED_SURFACE_WHY_ALLOCATION_LENGTH    "allocation length"
#define BC250_SHARED_SURFACE_WHY_ALLOCATION_RECORD    "allocation record"
#define BC250_SHARED_SURFACE_WHY_MAGIC                "record magic"
#define BC250_SHARED_SURFACE_WHY_VERSION              "record version"
#define BC250_SHARED_SURFACE_WHY_POLICY               "record policy"
#define BC250_SHARED_SURFACE_WHY_SHARED               "record shared"
#define BC250_SHARED_SURFACE_WHY_ACCESS               "record access"
#define BC250_SHARED_SURFACE_WHY_GEOMETRY             "record geometry"
#define BC250_SHARED_SURFACE_WHY_EXTENT               "record extent"
#define BC250_SHARED_SURFACE_WHY_SUBRESOURCES         "record subresources"
#define BC250_SHARED_SURFACE_WHY_USAGE                "record usage"
#define BC250_SHARED_SURFACE_WHY_FLAGS                "record flags"
#define BC250_SHARED_SURFACE_WHY_FORMAT               "record format"
#define BC250_SHARED_SURFACE_WHY_ALLOCATION_FORMAT    "allocation format"
#define BC250_SHARED_SURFACE_WHY_ALLOCATION_GEOMETRY  "allocation geometry"

/* One shared surface, in neither API's terms. The first eight fields are the surface itself; the
 * last three are the record's own policy and the one D3D11 field a D3D11 opener still passes on
 * (MiscFlags), so the wrapper below can rebuild its D3D11_TEXTURE2D_DESC1 without reading the wire
 * a second time. Everything else E26R v3 carries is fixed by the checks: MipLevels 1, ArraySize 1,
 * SampleCount 1, SampleQuality 0, Usage DEFAULT, CpuAccessFlags 0, TextureLayout UNDEFINED. */
typedef struct _BC250_SHARED_SURFACE {
    unsigned long Width, Height, Pitch, DxgiFormat, D3dDdiFormat, BindFlags, BytesPerPixel;
    unsigned long long Size;
    unsigned long MiscFlags;
    unsigned long Shared;
    unsigned long Access;
} BC250_SHARED_SURFACE;

/* Which records an opener admits. RequireShared 1 takes only a record that shares; AccessMask is
 * the set of BC250_SURFACE_RESOURCE_* access bits it admits (0: a plain texture only). */
typedef struct _BC250_SHARED_SURFACE_ADMIT {
    unsigned long RequireShared;
    unsigned long AccessMask;
} BC250_SHARED_SURFACE_ADMIT;

/* The geometry rule of a received LB7A v1 description, at the format row's bytes a pixel: whole
 * pixels a row, the row fits the pitch, the size covers every row, and the size leaves room for the
 * page rounding a consumer may do. A producer may pad the pitch and the size. This is the rule the
 * D3D11 shell has always applied (runtime_surface_geometry), now in one place. */
static __inline int Bc250SharedSurfaceGeometry(const BC250_WDDM_ALLOCATION_PRIVATE *Surface,
                                               unsigned long BytesPerPixel)
{
    if (!Surface || Surface->Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || Surface->Version != 1)
        return 0;
    if (!BytesPerPixel || !Surface->Width || !Surface->Height || !Surface->Pitch)
        return 0;
    if (Surface->Pitch % BytesPerPixel)
        return 0;
    if ((unsigned long long)Surface->Width * BytesPerPixel > Surface->Pitch)
        return 0;
    if (Surface->Size > ~0ull - 4095ull)
        return 0;
    return Surface->Size >= (unsigned long long)Surface->Pitch * Surface->Height;
}

/* The COMPOSED row of a storage format or of its sRGB view; 0 for a format no shared surface has. */
static __inline const AMDGPU_WDDM_SURFACE_FORMAT *Bc250SharedSurfaceFormat(unsigned long DxgiFormat)
{
    unsigned int count = 0, i;
    const AMDGPU_WDDM_SURFACE_FORMAT *rows = amdgpu_wddm_surface_formats(&count);
    for (i = 0; i < count; ++i)
        if (rows[i].dxgi && ((unsigned int)DxgiFormat == rows[i].dxgi ||
                             (rows[i].dxgi_srgb && (unsigned int)DxgiFormat == rows[i].dxgi_srgb)))
            return amdgpu_wddm_surface_admit(&rows[i], AMDGPU_WDDM_SURFACE_COMPOSED);
    return 0;
}

/* Writes the two records of Surface. The surface is validated exactly as a decode validates it, so
 * a record this function writes is a record Bc250SharedSurfaceDecode admits. */
static __inline int Bc250SharedSurfaceEncode(const BC250_SHARED_SURFACE *Surface,
                                            BC250_WDDM_ALLOCATION_PRIVATE *Allocation,
                                            BC250_SURFACE_RESOURCE_PRIVATE *Resource)
{
    const AMDGPU_WDDM_SURFACE_FORMAT *row;
    BC250_WDDM_ALLOCATION_PRIVATE a;
    BC250_SURFACE_RESOURCE_PRIVATE r;
    int shared = 0, cached = 0;
    if (!Surface || !Allocation || !Resource)
        return BC250_SHARED_SURFACE_MALFORMED;
    row = Bc250SharedSurfaceFormat(Surface->DxgiFormat);
    if (!row)
        return BC250_SHARED_SURFACE_FORMAT;
    if (row->d3dddi != (unsigned int)Surface->D3dDdiFormat ||
        row->bytes_per_pixel != (unsigned int)Surface->BytesPerPixel)
        return BC250_SHARED_SURFACE_MALFORMED;
    if (Surface->Width > BC250_SHARED_MAX_EDGE || Surface->Height > BC250_SHARED_MAX_EDGE)
        return BC250_SHARED_SURFACE_MALFORMED;
    if (Surface->BindFlags & ~BC250_SHARED_BIND_MASK)
        return BC250_SHARED_SURFACE_MALFORMED;
    if (Surface->MiscFlags & ~BC250_SHARED_MISC_MASK)
        return BC250_SHARED_SURFACE_MALFORMED;
    if (Surface->Shared > 1 || (Surface->Access & ~BC250_SURFACE_RESOURCE_ACCESS_MASK))
        return BC250_SHARED_SURFACE_MALFORMED;
    a.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    a.Version = 1;
    a.Width = Surface->Width;
    a.Height = Surface->Height;
    a.Pitch = Surface->Pitch;
    a.Format = Surface->D3dDdiFormat;
    a.Size = Surface->Size;
    if (!Bc250SharedSurfaceGeometry(&a, row->bytes_per_pixel))
        return BC250_SHARED_SURFACE_MALFORMED;
    r.Magic = BC250_SURFACE_RESOURCE_MAGIC;
    r.Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    r.Shared = Surface->Shared;
    r.Access = Surface->Access;
    r.Width = Surface->Width;
    r.Height = Surface->Height;
    r.MipLevels = 1;
    r.ArraySize = 1;
    r.Format = Surface->DxgiFormat;
    r.SampleCount = 1;
    r.SampleQuality = 0;
    r.Usage = BC250_SHARED_USAGE_DEFAULT;
    r.BindFlags = Surface->BindFlags;
    r.CpuAccessFlags = 0;
    r.MiscFlags = Surface->MiscFlags;
    r.TextureLayout = BC250_SHARED_LAYOUT_UNDEFINED;
    /* The kernel driver's own parser must admit what we write: it decides the placement from it. */
    if (!Bc250SurfaceResourcePolicy(&r, (unsigned int)sizeof(r), &shared, &cached))
        return BC250_SHARED_SURFACE_MALFORMED;
    *Allocation = a;
    *Resource = r;
    return BC250_SHARED_SURFACE_OK;
}

/* Decodes the pair a runtime handed an opener, under the opener's own admission policy. Out is
 * written only on success. Why, when it is given, is set on every path to the check that decided -
 * one of the BC250_SHARED_SURFACE_WHY_* strings above - so that an opener's refusal line names the
 * field the producer got wrong instead of the word "record". The cross-API failures of BD-075 round
 * 1 all declined here, which is seventeen checks behind one word (2026-10-06). */
static __inline int Bc250SharedSurfaceDecodeDetail(const void *ResourceData, unsigned int ResourceBytes,
                                                   const void *AllocationData, unsigned int AllocationBytes,
                                                   const BC250_SHARED_SURFACE_ADMIT *Admit,
                                                   BC250_SHARED_SURFACE *Out, const char **Why)
{
    BC250_WDDM_ALLOCATION_PRIVATE a;
    BC250_SURFACE_RESOURCE_PRIVATE r;
    const AMDGPU_WDDM_SURFACE_FORMAT *row = 0;
    BC250_SHARED_SURFACE out;
    const char *why = BC250_SHARED_SURFACE_WHY_ARGUMENTS;
    int shared = 0, cached = 0, status = BC250_SHARED_SURFACE_MALFORMED;
    unsigned int i;
    const unsigned char *src;
    unsigned char *dst;
    /* Zeroed before the one exit this function has: the checks below leave through a single break, and a
     * compiler that cannot see which of them ran reports every field as possibly uninitialised. 96 bytes. */
    dst = (unsigned char *)&a;
    for (i = 0; i < (unsigned int)sizeof(a); ++i)
        dst[i] = 0;
    dst = (unsigned char *)&r;
    for (i = 0; i < (unsigned int)sizeof(r); ++i)
        dst[i] = 0;
    for (;;) {
        if (!Out || !Admit || !ResourceData || !AllocationData)
            break;
        /* The two blobs come from two producers' code paths and are two separate refusals: a 16-byte E26R
         * v2 resource record (the Mesa CPU UMD's, BD-075 round 1) and a short or long LB7A are different
         * mistakes, and one name for both would let a regression in either print the other's reason. */
        why = BC250_SHARED_SURFACE_WHY_LENGTH;
        if (ResourceBytes != (unsigned int)sizeof(r))
            break;
        why = BC250_SHARED_SURFACE_WHY_ALLOCATION_LENGTH;
        if (AllocationBytes != (unsigned int)sizeof(a))
            break;
        /* Byte copies: the blobs are the runtime's memory and need no alignment of ours. */
        src = (const unsigned char *)AllocationData;
        dst = (unsigned char *)&a;
        for (i = 0; i < (unsigned int)sizeof(a); ++i)
            dst[i] = src[i];
        src = (const unsigned char *)ResourceData;
        dst = (unsigned char *)&r;
        for (i = 0; i < (unsigned int)sizeof(r); ++i)
            dst[i] = src[i];
        why = BC250_SHARED_SURFACE_WHY_ALLOCATION_RECORD;
        if (a.Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || a.Version != 1)
            break;
        why = BC250_SHARED_SURFACE_WHY_MAGIC;
        if (r.Magic != BC250_SURFACE_RESOURCE_MAGIC)
            break;
        why = BC250_SHARED_SURFACE_WHY_VERSION;
        if (r.Version != BC250_SURFACE_RESOURCE_TEXTURE_VERSION)
            break;
        /* The kernel driver's parser decided this record's placement; an opener that admitted a record
         * the kernel refused would be describing memory nobody placed. */
        why = BC250_SHARED_SURFACE_WHY_POLICY;
        if (!Bc250SurfaceResourcePolicy(&r, ResourceBytes, &shared, &cached))
            break;
        why = BC250_SHARED_SURFACE_WHY_SHARED;
        if (Admit->RequireShared && r.Shared != 1)
            break;
        why = BC250_SHARED_SURFACE_WHY_ACCESS;
        if (r.Access & ~Admit->AccessMask)
            break;
        why = BC250_SHARED_SURFACE_WHY_GEOMETRY;
        if (r.Width != a.Width || r.Height != a.Height)
            break;
        why = BC250_SHARED_SURFACE_WHY_EXTENT;
        if (!r.Width || !r.Height || r.Width > BC250_SHARED_MAX_EDGE || r.Height > BC250_SHARED_MAX_EDGE)
            break;
        why = BC250_SHARED_SURFACE_WHY_SUBRESOURCES;
        if (r.MipLevels != 1 || r.ArraySize != 1 || r.SampleCount != 1 || r.SampleQuality)
            break;
        why = BC250_SHARED_SURFACE_WHY_USAGE;
        if (r.Usage != BC250_SHARED_USAGE_DEFAULT || r.CpuAccessFlags ||
            r.TextureLayout != BC250_SHARED_LAYOUT_UNDEFINED)
            break;
        why = BC250_SHARED_SURFACE_WHY_FLAGS;
        if ((r.BindFlags & ~BC250_SHARED_BIND_MASK) || (r.MiscFlags & ~BC250_SHARED_MISC_MASK))
            break;
        /* The row the creator took the LB7A format and pitch from; its pixel size bounds the geometry.
         * A format with no COMPOSED row is "not implemented", which is what both shells report. */
        why = BC250_SHARED_SURFACE_WHY_FORMAT;
        row = Bc250SharedSurfaceFormat(r.Format);
        if (!row) {
            status = BC250_SHARED_SURFACE_FORMAT;
            break;
        }
        why = BC250_SHARED_SURFACE_WHY_ALLOCATION_FORMAT;
        if (row->d3dddi != a.Format)
            break;
        why = BC250_SHARED_SURFACE_WHY_ALLOCATION_GEOMETRY;
        if (!Bc250SharedSurfaceGeometry(&a, row->bytes_per_pixel))
            break;
        status = BC250_SHARED_SURFACE_OK;
        why = BC250_SHARED_SURFACE_WHY_OK;
        break;
    }
    if (Why)
        *Why = why;
    if (status != BC250_SHARED_SURFACE_OK)
        return status;
    out.Width = a.Width;
    out.Height = a.Height;
    out.Pitch = a.Pitch;
    out.DxgiFormat = r.Format;
    out.D3dDdiFormat = a.Format;
    out.BindFlags = r.BindFlags;
    out.BytesPerPixel = row->bytes_per_pixel;
    out.Size = a.Size;
    out.MiscFlags = r.MiscFlags;
    out.Shared = r.Shared;
    out.Access = r.Access;
    *Out = out;
    return BC250_SHARED_SURFACE_OK;
}

/* The same decode without the detail, for a caller that reports the status alone. */
static __inline int Bc250SharedSurfaceDecodeAdmitted(const void *ResourceData, unsigned int ResourceBytes,
                                                     const void *AllocationData, unsigned int AllocationBytes,
                                                     const BC250_SHARED_SURFACE_ADMIT *Admit,
                                                     BC250_SHARED_SURFACE *Out)
{
    return Bc250SharedSurfaceDecodeDetail(ResourceData, ResourceBytes, AllocationData, AllocationBytes, Admit, Out, 0);
}

/* The strict form: a shared texture and nothing else. A primary, a cached surface and a record
 * that shares nothing are refused, because a D3D12 opener has no use for any of them. Why, when it
 * is given, names the check that decided. */
static __inline int Bc250SharedSurfaceDecodeWhy(const void *ResourceData, unsigned int ResourceBytes,
                                                const void *AllocationData, unsigned int AllocationBytes,
                                                BC250_SHARED_SURFACE *Out, const char **Why)
{
    BC250_SHARED_SURFACE_ADMIT admit;
    admit.RequireShared = 1;
    admit.AccessMask = 0;
    return Bc250SharedSurfaceDecodeDetail(ResourceData, ResourceBytes, AllocationData, AllocationBytes, &admit, Out,
                                          Why);
}

static __inline int Bc250SharedSurfaceDecode(const void *ResourceData, unsigned int ResourceBytes,
                                             const void *AllocationData, unsigned int AllocationBytes,
                                             BC250_SHARED_SURFACE *Out)
{
    return Bc250SharedSurfaceDecodeWhy(ResourceData, ResourceBytes, AllocationData, AllocationBytes, Out, 0);
}

#if defined(__cplusplus)
}
#endif
#endif
