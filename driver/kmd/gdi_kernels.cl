/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The six GDI operations that the command processor cannot perform, as compute kernels for
 * gfx1013. Increment 2 of docs/design/gdi-hw-accel.md.
 *
 * The arithmetic is NOT here. Every line of it is gdi_pixel.h, which compiles as OpenCL C in this
 * file, as kernel C in the miniport and as host C in the test. One text, three compilations: a
 * host test that proves the arithmetic proves the arithmetic this kernel runs.
 *
 * What IS here is addressing: which pixel a work item owns, where that pixel lies in each
 * allocation, and the contract's destination-to-source transform. Nothing in this file validates
 * anything. Every bound was proved on the processor by Bc250GdiAdmitOp and Bc250GdiPlanItem before
 * the dispatch was emitted, and the one check below (x < width, y < height) is there because the
 * grid is rounded up to whole work groups, not because the rectangle is in doubt.
 *
 * The six kernels share one argument list, so the kernarg block has one layout, one writer in
 * gdi_dispatch.c and one gate in tools/gpu/gdi_kernels.py. An argument a kernel does not read
 * costs nothing but its 4 or 8 bytes.
 *
 * Build: tools/gpu/gdi_kernels.py, with the pinned clang of toolchain/llvm-amdgpu-22.1.8. Nothing
 * here may use scratch memory, LDS, a jump table or a device-library call: the build refuses the
 * code object if it does, because the dispatch programs no scratch descriptor and no LDS size.
 */

#include "gdi_pixel.h"

/* gdi_pixel.h has two arms and only one of them gives these kernels global pointers and the exact
 * integer types; a compiler that does not predefine what that arm tests would take the host arm
 * and still compile here, with flat loads instead of global ones (see the comment on the arm). */
#if !defined(BC250_GDI_PIXEL_CL)
#error "gdi_pixel.h took its host arm: this must be compiled as OpenCL C, or with BC250_GDI_CL"
#endif

/* 8 by 8 work items, which is BC250_GDI_GROUP_X by BC250_GDI_GROUP_Y of gdi_pixel.h. The attribute
 * is what puts the shape into the code object's metadata, where the build reads it back. */
#define GDI_GROUP __attribute__((reqd_work_group_size(BC250_GDI_GROUP_X, BC250_GDI_GROUP_Y, 1)))

/* The one argument list. The byte offsets are the AMDHSA kernarg layout, in order, each argument
 * at its natural alignment; gdi_dispatch.c writes exactly this and the build gates the total.
 *
 *    0 dst      the destination pixel at (DstLeft, DstTop) of this sub-rectangle
 *    8 src      the source surface's origin (0,0), because the transform gives absolute
 *                 source coordinates
 *   16 alpha    the ClearType alpha pixel under (DstLeft, DstTop)
 *   24 gamma    the selected gamma row: 256 gamma bytes then 256 inverse gamma bytes
 *   32 dstPitch, 36 srcPitch, 40 alphaPitch     bytes per row
 *   44 width, 48 height                         the sub-rectangle, in pixels
 *   52 color, 56 color2, 60 rop, 64 flags, 68 sca
 *   72 srcLeft, 76 srcTop                       the command's SrcRect origin
 *   80 dx0, 84 dy0       this sub-rectangle's offset inside DstRect, which is what the transform
 *                          measures from, so a clipped rectangle samples the pixels it would have
 *   88 ws, 92 wd, 96 hs, 100 hd                 the command's source and destination extents
 */
#define GDI_ARGS                                                                                  \
    __global uchar* dst, __global const uchar* src,                                               \
    __global const uchar* alpha, __global const uchar* gamma,                                     \
    uint dstPitch, uint srcPitch, uint alphaPitch, uint width, uint height,                       \
    uint color, uint color2, uint rop, uint flags, uint sca,                                      \
    int srcLeft, int srcTop, uint dx0, uint dy0, uint ws, uint wd, uint hs, uint hd

/* The work item's pixel, and out of the kernel when the rounded-up grid overshoots. */
#define GDI_PIXEL                                                                                 \
    uint x = __builtin_amdgcn_workgroup_id_x() * BC250_GDI_GROUP_X +                              \
             __builtin_amdgcn_workitem_id_x();                                                    \
    uint y = __builtin_amdgcn_workgroup_id_y() * BC250_GDI_GROUP_Y +                              \
             __builtin_amdgcn_workitem_id_y();                                                    \
    if (x >= width || y >= height) return

/* The destination word of (x, y). Every colour format this driver admits is 32 bits a pixel; the
 * A8 lookup row is the one surface here that is read a byte at a time. */
static inline __global uint* GdiDstAt(__global uchar* dst, uint dstPitch, uint x, uint y)
{
    return (__global uint*)(dst + (ulong)y * dstPitch + 4u * (ulong)x);
}

/* The source word the transform sends (x, y) to, from the source surface's origin. */
static inline uint GdiSrcWord(__global const uchar* src, uint srcPitch,
                              int srcLeft, int srcTop, uint dx0, uint dy0,
                              uint x, uint y, uint ws, uint wd, uint hs, uint hd)
{
    int sx = Bc250GdiSourceCoord(srcLeft, dx0 + x, ws, wd);
    int sy = Bc250GdiSourceCoord(srcTop, dy0 + y, hs, hd);
    return *(__global const uint*)(src + (ulong)(uint)sy * srcPitch + 4u * (ulong)(uint)sx);
}

#define GDI_SRC_WORD \
    GdiSrcWord(src, srcPitch, srcLeft, srcTop, dx0, dy0, x, y, ws, wd, hs, hd)

/* ---- BITBLT with a raster operation -------------------------------------------------------------
 * SRCINVERT, SRCAND and SRCOR, on the surface words as they lie in memory: the three operations
 * are bitwise and need no channel order. Equal extents, so the transform is a translation.
 */
GDI_GROUP kernel void gdi_bitblt_rop(GDI_ARGS)
{
    GDI_PIXEL;
    __global uint* d = GdiDstAt(dst, dstPitch, x, y);
    *d = Bc250GdiBitBltPixel(rop, GDI_SRC_WORD, *d);
}

/* ---- COLORFILL with a raster operation ----------------------------------------------------------
 * The six operations other than PATCOPY. Also bitwise, and `color` arrives in the destination's
 * own channel order because gdi_dispatch.c converts it once for the whole command.
 */
GDI_GROUP kernel void gdi_colorfill_rop(GDI_ARGS)
{
    GDI_PIXEL;
    __global uint* d = GdiDstAt(dst, dstPitch, x, y);
    *d = Bc250GdiColorFillPixel(rop, color, *d);
}

/* ---- STRETCHBLT ---------------------------------------------------------------------------------
 * A point resample: the destination pixel takes the source pixel the transform names, with no
 * filtering and no arithmetic. COLORONCOLOR is the only mode this driver admits, and a point
 * resample is what COLORONCOLOR is ("deletes the eliminated lines").
 */
GDI_GROUP kernel void gdi_stretchblt(GDI_ARGS)
{
    GDI_PIXEL;
    *GdiDstAt(dst, dstPitch, x, y) = GDI_SRC_WORD;
}

/* ---- ALPHABLEND ---------------------------------------------------------------------------------
 * The Win32 BLENDFUNCTION tables, in gdi_pixel.h. Both words become A8R8G8B8 first, because the
 * tables are per channel; the admission has already proved that the source and the destination
 * share one channel order.
 */
GDI_GROUP kernel void gdi_alphablend(GDI_ARGS)
{
    GDI_PIXEL;
    uint redFirst = flags & BC250_GDI_FLAG_RED_FIRST;
    __global uint* d = GdiDstAt(dst, dstPitch, x, y);
    uint out = Bc250GdiAlphaBlendPixel(Bc250GdiToArgb(GDI_SRC_WORD, redFirst),
                                       Bc250GdiToArgb(*d, redFirst), sca,
                                       flags & BC250_GDI_FLAG_SRC_HAS_ALPHA);
    *d = Bc250GdiFromArgb(out, redFirst);
}

/* ---- TRANSPARENTBLT -----------------------------------------------------------------------------
 * One colour key test for each pixel. `color` is the key, in A8R8G8B8 as the DDI documents every
 * Color field, so the source word is converted and the key is not.
 */
GDI_GROUP kernel void gdi_transparentblt(GDI_ARGS)
{
    GDI_PIXEL;
    uint redFirst = flags & BC250_GDI_FLAG_RED_FIRST;
    __global uint* d = GdiDstAt(dst, dstPitch, x, y);
    uint out = Bc250GdiTransparentPixel(Bc250GdiToArgb(GDI_SRC_WORD, redFirst),
                                        Bc250GdiToArgb(*d, redFirst), color,
                                        flags & BC250_GDI_FLAG_HONOR_ALPHA);
    *d = Bc250GdiFromArgb(out, redFirst);
}

/* ---- CLEARTYPEBLEND -----------------------------------------------------------------------------
 * The per channel blend through the gamma table pair. NoTempSurfaceForClearTypeBlend is set, so
 * this kernel reads and writes the destination itself and there is no temporary surface. The alpha
 * surface is 32 bits a pixel (the admission requires it) and carries the three subpixel coverages
 * in its R, G and B channels, so it is converted by ITS OWN channel order.
 */
GDI_GROUP kernel void gdi_cleartypeblend(GDI_ARGS)
{
    GDI_PIXEL;
    uint redFirst = flags & BC250_GDI_FLAG_RED_FIRST;
    __global uint* d = GdiDstAt(dst, dstPitch, x, y);
    uint a = *(__global const uint*)(alpha + (ulong)y * alphaPitch + 4u * (ulong)x);
    uint out = Bc250GdiClearTypePixel(Bc250GdiToArgb(*d, redFirst),
                                      Bc250GdiToArgb(a, flags & BC250_GDI_FLAG_ALPHA_RED_FIRST),
                                      color, color2, flags & BC250_GDI_FLAG_GAMMA_VALID,
                                      gamma, gamma + BC250_GDI_GAMMA_ENTRIES);
    *d = Bc250GdiFromArgb(out, redFirst);
}
