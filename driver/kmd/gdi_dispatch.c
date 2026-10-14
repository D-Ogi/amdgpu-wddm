/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gdi_dispatch.h"
#include "gdi_kernels.generated.h"
/* For BC250_GDI_IB_DWORDS_MAX and the ring granule only: this is the one file of the GDI path that
 * has AMD's packet definitions, so it is where that constant is held against them. */
#include "gdi_batch.h"
#include "../amdgpu-import/nvd.h"

/*
 * A compute dispatch of one of the six GDI kernels, written into the GDI DMA buffer.
 *
 * The packet sequence follows driver/shim/bc250_dispatch.c, which follows libdrm 2.4.114's
 * tests/amdgpu/shader_test_util.c, the sequence that is measured working on this silicon (facts
 * M49). Three deliberate differences, each because the GDI buffer is an indirect buffer on the
 * graphics ring and not a submission of our own to a kernel-owned compute queue:
 *
 *   1. COMPUTE_START_X/Y/Z is not written. The initiator carries FORCE_START_AT_000, which is what
 *      RADV does for a grid with no offset (ref/mesa/src/amd/vulkan/radv_cmd_buffer.c, the
 *      DISPATCH_DIRECT emitter), and a bit in our own packet cannot be stale.
 *   2. CP_COHER_START_DELAY is not written. It is a uconfig register, which is ring-wide state this
 *      driver does not own in a GDI buffer; amdgpu sets it when it brings the block up.
 *   3. The CU mask goes out as an ordinary SET_SH_REG and not through SET_SH_REG_INDEX's
 *      undocumented index 3. Mesa writes these two registers the plain way
 *      (ref/mesa/src/amd/common/ac_cmdbuf.c, gfx10_init_compute_preamble_state).
 *
 * Every register offset, field bit, event type and kernel descriptor value comes from
 * gdi_kernels.generated.h, which tools/gpu/gdi_kernels.py derives from tools/regcalc, AMD's field
 * masks and the code object the compiler produced. Nothing here is typed (CLAUDE.md hard rules 1
 * and 2), and the dword counts below are the only arithmetic.
 */

/* Rule 3 of gdi_batch.h, held against AMD's own definition of the field it comes from rather than
 * typed twice. PACKET3_INDIRECT_BUFFER__IB_SIZE() masks to the length field and shifts by nothing
 * (nvd.h), so a length that comes back unchanged is a length the packet can carry, which is the
 * question bc250_gfx_emit_ib() asks of every submission this driver makes. The second line is what
 * makes the constant a maximum: the next granule up does not survive the field. A plain typedef and
 * not C_ASSERT, because this file is compiled by the host test as well. */
typedef char bc250_gdi_ib_max_fits[
    PACKET3_INDIRECT_BUFFER__IB_SIZE(BC250_GDI_IB_DWORDS_MAX) == BC250_GDI_IB_DWORDS_MAX ? 1 : -1];
typedef char bc250_gdi_ib_max_is_max[
    PACKET3_INDIRECT_BUFFER__IB_SIZE(BC250_GDI_IB_DWORDS_MAX + BC250_GDI_RING_GRANULE) !=
    BC250_GDI_IB_DWORDS_MAX + BC250_GDI_RING_GRANULE ? 1 : -1];
typedef char bc250_gdi_ib_max_granule[
    (BC250_GDI_IB_DWORDS_MAX % BC250_GDI_RING_GRANULE) == 0u ? 1 : -1];

/* A type-3 packet carrying n+1 body dwords costs n+2 dwords in all (nvd.h's PACKET3 writes COUNT
 * as n). A SET_SH_REG of `count` registers has one offset dword and `count` values, so n = count. */
#define SH_DWORDS(count) ((count) + 2u)

#define GDI_REG_DWORDS (SH_DWORDS(1u) +   /* COMPUTE_TMPRING_SIZE */                                \
                        SH_DWORDS(6u) +   /* COMPUTE_REQ_CTRL and the five behind it */             \
                        SH_DWORDS(2u) +   /* COMPUTE_STATIC_THREAD_MGMT_SE0, SE1 */                 \
                        SH_DWORDS(2u) +   /* COMPUTE_STATIC_THREAD_MGMT_SE2, SE3 */                 \
                        SH_DWORDS(2u) +   /* COMPUTE_PGM_LO, PGM_HI */                              \
                        SH_DWORDS(2u) +   /* COMPUTE_PGM_RSRC1, RSRC2 */                            \
                        SH_DWORDS(1u) +   /* COMPUTE_PGM_RSRC3 */                                   \
                        SH_DWORDS(3u) +   /* COMPUTE_NUM_THREAD_X, Y, Z */                          \
                        SH_DWORDS(6u) +   /* COMPUTE_USER_DATA_0..5 */                              \
                        SH_DWORDS(1u))    /* COMPUTE_RESOURCE_LIMITS */
#define GDI_DISPATCH_DWORDS 5u            /* DISPATCH_DIRECT: DIM_X, DIM_Y, DIM_Z, initiator */
#define GDI_EVENT_DWORDS 2u               /* EVENT_WRITE: the partial flush */
#define GDI_KERNARG_DWORDS (BC250_GDI_KERNARG_BYTES / 4u)

/* The alignment of the kernarg block: the compiler asks for 8 bytes, and the two dword halves of
 * every address in it must not straddle it. */
#define GDI_KERNARG_ALIGN 8u

/* Everything one dispatch needs apart from the padding in front of the kernarg block. */
#define GDI_DISPATCH_FIXED (1u + GDI_KERNARG_DWORDS + GDI_REG_DWORDS + GDI_DISPATCH_DWORDS + \
                            GDI_EVENT_DWORDS)

/* Padding dwords that put a dword-aligned address on an `align` boundary. */
static unsigned int Bc250GdiPadTo(unsigned long long address, unsigned int align)
{
    unsigned int inside = (unsigned int)(address & (unsigned long long)(align - 1u));
    return inside ? (align - inside) / 4u : 0u;
}

/* A NOP whose payload is `dwords` dwords of something else: the code, or an argument block. The
 * CP skips it, which is how data lives inside a command stream. The payload is never empty here,
 * so the one-dword special case (Bc250GfxCopyNop) does not arise. */
static unsigned int Bc250GdiNop(unsigned int dwords)
{
    return (unsigned int)PACKET3(PACKET3_NOP, dwords - 1u);
}

static unsigned int Bc250GdiSetSh(unsigned int* buffer, unsigned int offset,
                                 const unsigned int* values, unsigned int count)
{
    unsigned int i;
    /* The shader-type bit of the header is what makes these writes name the COMPUTE register space
     * and not the graphics one; nvd.h's PACKET3_COMPUTE sets it. */
    buffer[0] = (unsigned int)PACKET3_COMPUTE(PACKET3_SET_SH_REG, count);
    buffer[1] = offset;
    for (i = 0; i < count; i++) buffer[2 + i] = values[i];
    return SH_DWORDS(count);
}

static unsigned int Bc250GdiSetSh1(unsigned int* buffer, unsigned int offset, unsigned int value)
{
    return Bc250GdiSetSh(buffer, offset, &value, 1u);
}

unsigned int Bc250GdiCodeKernelCount(void) { return BC250_GDI_KERNEL_COUNT; }
unsigned int Bc250GdiCodeBytes(void) { return BC250_GDI_CODE_DWORDS * 4u; }
const char* Bc250GdiCodeHash(void) { return BC250_GDI_CODE_SHA256; }

/* The six kernels are in the image, so the capability bit may be set. The one caller is wddm.c,
 * through gdi_render.h; the count is read from the generated table rather than assumed, so a build
 * that lost a kernel answers 0 and the bit stays clear. */
int Bc250GdiKernelsPresent(void)
{
    return BC250_GDI_KERNEL_COUNT == (unsigned int)Bc250GdiKernels - 1u &&
           BC250_GDI_CODE_DWORDS != 0u;
}

unsigned int Bc250GdiCodeDwords(unsigned long long bufferVa)
{
    unsigned int pad, total;
    if (bufferVa == 0 || (bufferVa & 3u)) return 0;
    pad = Bc250GdiPadTo(bufferVa + 4u, BC250_GDI_CODE_ALIGN);
    total = 1u + pad + BC250_GDI_CODE_DWORDS;
    /* The NOP's COUNT field is 14 bits, and the whole block must not wrap the address space. */
    if (pad + BC250_GDI_CODE_DWORDS > 0x3FFFu) return 0;
    if (bufferVa > ~0ull - (unsigned long long)total * 4u) return 0;
    return total;
}

unsigned int Bc250EmitGdiCode(unsigned int* buffer, unsigned int capacity,
                              unsigned long long bufferVa, unsigned long long* codeVa)
{
    unsigned int pad, total, i;
    if (!buffer || !codeVa) return 0;
    *codeVa = 0;
    total = Bc250GdiCodeDwords(bufferVa);
    if (!total || capacity < total) return 0;
    pad = total - 1u - BC250_GDI_CODE_DWORDS;
    buffer[0] = Bc250GdiNop(pad + BC250_GDI_CODE_DWORDS);
    for (i = 0; i < pad; i++) buffer[1 + i] = 0;
    for (i = 0; i < BC250_GDI_CODE_DWORDS; i++) buffer[1 + pad + i] = g_Bc250GdiCode[i];
    *codeVa = bufferVa + (unsigned long long)(1u + pad) * 4u;
    return total;
}

/* The worst case of the two functions above: the largest padding each can need. The code block is
 * aligned to BC250_GDI_CODE_ALIGN and the argument block to GDI_KERNARG_ALIGN, and a dword-aligned
 * address is at most align - 4 bytes short of a boundary. */
unsigned int Bc250GdiCodeDwordsMax(void)
{
    return 1u + (BC250_GDI_CODE_ALIGN / 4u - 1u) + BC250_GDI_CODE_DWORDS;
}

unsigned int Bc250GdiDispatchDwordsMax(void)
{
    return GDI_DISPATCH_FIXED + (GDI_KERNARG_ALIGN / 4u - 1u);
}

unsigned int Bc250GdiDispatchDwords(unsigned long long bufferVa)
{
    unsigned int pad, total;
    if (bufferVa == 0 || (bufferVa & 3u)) return 0;
    pad = Bc250GdiPadTo(bufferVa + 4u, GDI_KERNARG_ALIGN);
    total = GDI_DISPATCH_FIXED + pad;
    if (bufferVa > ~0ull - (unsigned long long)total * 4u) return 0;
    return total;
}

unsigned int Bc250EmitGdiDispatch(unsigned int kernel, const BC250_GDI_KERNARG* arg,
                                  unsigned long long code, unsigned int* buffer,
                                  unsigned int capacity, unsigned long long bufferVa)
{
    const BC250_GDI_CODE_KERNEL* program;
    unsigned long long argVa;
    unsigned int total, pad, used, i, groups[3], values[6], zero[6] = {0, 0, 0, 0, 0, 0};
    unsigned int* block;

    if (!buffer || !arg) return 0;
    if (kernel == 0u || kernel >= (unsigned int)Bc250GdiKernels) return 0;
    program = &g_Bc250GdiCodeKernels[kernel];
    if (program->Dwords == 0u) return 0;
    /* COMPUTE_PGM_LO carries the program address shifted right by eight, so a program that is not
     * on a 256-byte boundary cannot be named at all. */
    if (code == 0 || (code & (unsigned long long)(BC250_GDI_CODE_ALIGN - 1u))) return 0;
    if (arg->Width == 0u || arg->Height == 0u) return 0;
    total = Bc250GdiDispatchDwords(bufferVa);
    if (!total || capacity < total) return 0;

    /* The grid, in work groups, rounded up: the last group of a row or a column overshoots the
     * rectangle and the kernel drops those work items itself. */
    groups[0] = (arg->Width + BC250_GDI_GROUP_X - 1u) / BC250_GDI_GROUP_X;
    groups[1] = (arg->Height + BC250_GDI_GROUP_Y - 1u) / BC250_GDI_GROUP_Y;
    groups[2] = 1u;

    pad = total - GDI_DISPATCH_FIXED;
    buffer[0] = Bc250GdiNop(pad + GDI_KERNARG_DWORDS);
    for (i = 0; i < pad; i++) buffer[1 + i] = 0;
    block = buffer + 1u + pad;
    argVa = bufferVa + (unsigned long long)(1u + pad) * 4u;

    /* The argument block, at the dword offsets the compiler chose for gdi_kernels.cl's own
     * parameter list (BC250_GDI_ARG_*, from the code object's metadata). The four addresses are
     * written as two dwords each, little end first, which is what the kernel's s_load_dwordx2
     * reads back. */
    block[BC250_GDI_ARG_DST] = (unsigned int)arg->Dst;
    block[BC250_GDI_ARG_DST + 1u] = (unsigned int)(arg->Dst >> 32);
    block[BC250_GDI_ARG_SRC] = (unsigned int)arg->Src;
    block[BC250_GDI_ARG_SRC + 1u] = (unsigned int)(arg->Src >> 32);
    block[BC250_GDI_ARG_ALPHA] = (unsigned int)arg->Alpha;
    block[BC250_GDI_ARG_ALPHA + 1u] = (unsigned int)(arg->Alpha >> 32);
    block[BC250_GDI_ARG_GAMMA] = (unsigned int)arg->Gamma;
    block[BC250_GDI_ARG_GAMMA + 1u] = (unsigned int)(arg->Gamma >> 32);
    block[BC250_GDI_ARG_DSTPITCH] = arg->DstPitch;
    block[BC250_GDI_ARG_SRCPITCH] = arg->SrcPitch;
    block[BC250_GDI_ARG_ALPHAPITCH] = arg->AlphaPitch;
    block[BC250_GDI_ARG_WIDTH] = arg->Width;
    block[BC250_GDI_ARG_HEIGHT] = arg->Height;
    block[BC250_GDI_ARG_COLOR] = arg->Color;
    block[BC250_GDI_ARG_COLOR2] = arg->Color2;
    block[BC250_GDI_ARG_ROP] = arg->Rop;
    block[BC250_GDI_ARG_FLAGS] = arg->Flags;
    block[BC250_GDI_ARG_SCA] = arg->Sca;
    block[BC250_GDI_ARG_SRCLEFT] = (unsigned int)arg->SrcLeft;
    block[BC250_GDI_ARG_SRCTOP] = (unsigned int)arg->SrcTop;
    block[BC250_GDI_ARG_DX0] = arg->Dx0;
    block[BC250_GDI_ARG_DY0] = arg->Dy0;
    block[BC250_GDI_ARG_WS] = arg->Ws;
    block[BC250_GDI_ARG_WD] = arg->Wd;
    block[BC250_GDI_ARG_HS] = arg->Hs;
    block[BC250_GDI_ARG_HD] = arg->Hd;

    used = 1u + pad + GDI_KERNARG_DWORDS;

    /* No scratch, and no request-control state from whatever ran here before. */
    used += Bc250GdiSetSh1(buffer + used, BC250_GDI_SH_TMPRING_SIZE, 0);
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_REQ_CTRL, zero, 6u);

    /* Every compute unit of all four shader engines. A unit the board harvested or the CU mode
     * turned off is not reachable through this mask and stays off; this only says that the dispatch
     * itself asks for no restriction. */
    values[0] = values[1] = 0xFFFFFFFFu;
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_STATIC_THREAD_MGMT_SE0, values, 2u);
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_STATIC_THREAD_MGMT_SE2, values, 2u);

    /* The program: COMPUTE_PGM_LO is the address shifted right by 8 and PGM_HI is bits 47:40 of
     * it, the one shift this file performs and the reason for the alignment above. */
    values[0] = (unsigned int)((code + (unsigned long long)program->Offset * 4u) >> 8);
    values[1] = (unsigned int)((code + (unsigned long long)program->Offset * 4u) >> 40);
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_PGM_LO, values, 2u);

    /* The compiler's own resource description of this kernel, from its kernel descriptor. */
    values[0] = program->Rsrc1;
    values[1] = program->Rsrc2;
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_PGM_RSRC1, values, 2u);
    used += Bc250GdiSetSh1(buffer + used, BC250_GDI_SH_PGM_RSRC3, 0);

    values[0] = BC250_GDI_GROUP_X;
    values[1] = BC250_GDI_GROUP_Y;
    values[2] = 1u;
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_NUM_THREAD_X, values, 3u);

    /* The user SGPRs the kernel descriptor asked for: s[0:3] a private segment buffer descriptor
     * the kernel never uses (it has no private segment, which the build gate checks), and s[4:5]
     * the pointer to the argument block above. */
    values[0] = values[1] = values[2] = values[3] = 0;
    values[4] = (unsigned int)argVa;
    values[5] = (unsigned int)(argVa >> 32);
    used += Bc250GdiSetSh(buffer + used, BC250_GDI_SH_USER_DATA_0, values, 6u);

    used += Bc250GdiSetSh1(buffer + used, BC250_GDI_SH_RESOURCE_LIMITS, 0);

    buffer[used++] = (unsigned int)PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3);
    buffer[used++] = groups[0];
    buffer[used++] = groups[1];
    buffer[used++] = groups[2];
    buffer[used++] = BC250_GDI_DISPATCH_INITIATOR;

    /* The waves must retire before anything behind this dispatch reads the destination: the next
     * item of the same command buffer may be a DMA_DATA copy out of it, and the Present that
     * follows certainly reads it. */
    buffer[used++] = (unsigned int)PACKET3(PACKET3_EVENT_WRITE, 0);
    buffer[used++] = (unsigned int)(EVENT_TYPE(BC250_GDI_EVENT_CS_PARTIAL_FLUSH) |
                                    EVENT_INDEX(PACKET3_EVENT_WRITE__EVENT_INDEX__CS_PARTIAL_FLUSH));
    return used == total ? used : 0;
}

/* ---------------------------------------------------------------------------------------------
 * The argument block of one item
 * ------------------------------------------------------------------------------------------- */

static int Bc250GdiSlotVa(const BC250_GDI_SURFACE* surfaces, const unsigned long long* vas,
                          unsigned int count, unsigned int slot, unsigned long long offset,
                          unsigned long long* out)
{
    if (slot >= count || !surfaces[slot].Present || vas[slot] == 0) return 0;
    if (offset >= surfaces[slot].Bytes) return 0;
    if (vas[slot] > ~0ull - offset) return 0;
    *out = vas[slot] + offset;
    return 1;
}

int Bc250GdiKernarg(const BC250_GDI_OP* op, const BC250_GDI_SURFACE* surfaces,
                    const unsigned long long* vas, unsigned int count,
                    const BC250_GDI_ITEM* item, BC250_GDI_KERNARG* arg)
{
    unsigned int dstRedFirst, i;

    if (!op || !surfaces || !vas || !item || !arg) return 0;
    if (item->Work != (unsigned int)Bc250GdiWorkKernel) return 0;
    if (item->Kernel == 0u || item->Kernel >= (unsigned int)Bc250GdiKernels) return 0;
    if (item->DstWidth == 0u || item->DstHeight == 0u) return 0;

    /* Every field, so that a kernel reading an argument its operation does not use reads a zero
     * and not whatever was on the caller's stack. */
    for (i = 0; i < sizeof(*arg); i++) ((unsigned char*)arg)[i] = 0;

    if (!Bc250GdiSlotVa(surfaces, vas, count, op->DstAllocationIndex, item->DstOffset, &arg->Dst))
        return 0;
    dstRedFirst = surfaces[op->DstAllocationIndex].RedFirst;
    arg->DstPitch = item->DstPitch;
    arg->Width = item->DstWidth;
    arg->Height = item->DstHeight;
    if (dstRedFirst) arg->Flags |= BC250_GDI_FLAG_RED_FIRST;

    if (Bc250GdiOpHasSource(op->OpCode))
    {
        if (!Bc250GdiSlotVa(surfaces, vas, count, op->SrcAllocationIndex, item->SrcBase, &arg->Src))
            return 0;
        arg->SrcPitch = item->SrcPitch;
    }
    /* The transform's four extents and the origin it measures from, as the plan holds them: a
     * clipped sub-rectangle samples the source pixels it would have sampled without the clip. */
    arg->SrcLeft = item->SrcLeft;
    arg->SrcTop = item->SrcTop;
    if ((int)item->DstLeft < item->DstLeft0 || (int)item->DstTop < item->DstTop0) return 0;
    arg->Dx0 = (unsigned int)((int)item->DstLeft - item->DstLeft0);
    arg->Dy0 = (unsigned int)((int)item->DstTop - item->DstTop0);
    arg->Ws = item->Ws;
    arg->Wd = item->Wd;
    arg->Hs = item->Hs;
    arg->Hd = item->Hd;

    /* The colour fields, each in the channel order its own kernel reads it in. The three fields do
     * not share one rule, and the headers say which is which:
     *
     *   DXGK_GDIARG_COLORFILL.Color is "Color in the format of the destination surface"
     *   (ref/ddi-display/d3dkmddi.md:21793, the WDK 10.0.26100 declaration; the reference page's
     *   own prose says A8R8G8B8 at :21825 instead, and the header wins), and the fill kernel writes
     *   it into the destination word without looking at a channel: it is a pass-through.
     *
     *   DXGK_GDIARG_TRANSPARENTBLT.Color is "the physical transparent color in the source surface,
     *   in 32-bit ARGB unsigned pixel format (as defined by the D3DDDIFMT_A8R8G8B8 value ...)"
     *   (:22037), and DXGK_GDIARG_CLEARTYPEBLEND.Color/Color2 are the foreground colour "in 32-bit
     *   ARGB unsigned pixel format" gamma corrected and not (:21701, :21721); neither header line
     *   (:21654, :21659) names the destination's format. Their kernels convert the source,
     *   destination and alpha words to A8R8G8B8 themselves - Bc250GdiToArgb at gdi_kernels.cl:150
     *   and :168-169 - and pass these fields on untouched, so a conversion here would apply the
     *   swap twice and, on a red-first destination, make the transparency key match the pixels it
     *   should have kept.
     *
     * So nothing is converted in this function, and the per-channel work stays in the kernel where
     * the destination's order is already known. */
    switch (item->Kernel) {
    case Bc250GdiKernelColorFillRop:
        arg->Rop = op->Rop;
        arg->Color = op->Color;
        break;
    case Bc250GdiKernelBitBltRop:
        arg->Rop = op->Rop;
        break;
    case Bc250GdiKernelAlphaBlend:
        arg->Sca = op->SourceConstantAlpha;
        if (op->SourceHasAlpha) arg->Flags |= BC250_GDI_FLAG_SRC_HAS_ALPHA;
        break;
    case Bc250GdiKernelTransparentBlt:
        /* The key is A8R8G8B8 and the kernel compares it with a source pixel it has converted to
         * A8R8G8B8 itself, so the key travels as it arrived. */
        arg->Color = op->Color;
        if (op->HonorAlpha) arg->Flags |= BC250_GDI_FLAG_HONOR_ALPHA;
        break;
    case Bc250GdiKernelClearTypeBlend:
        /* Both foreground colours are A8R8G8B8, like the blend the kernel performs. */
        arg->Color = op->Color;
        arg->Color2 = op->Color2;
        if (!Bc250GdiSlotVa(surfaces, vas, count, op->AlphaSurfAllocationIndex, item->AlphaOffset,
                            &arg->Alpha))
            return 0;
        arg->AlphaPitch = item->AlphaPitch;
        if (surfaces[op->AlphaSurfAllocationIndex].RedFirst)
            arg->Flags |= BC250_GDI_FLAG_ALPHA_RED_FIRST;
        if (item->GammaValid)
        {
            if (!Bc250GdiSlotVa(surfaces, vas, count, op->GammaSurfAllocationIndex,
                                item->GammaOffset, &arg->Gamma))
                return 0;
            arg->Flags |= BC250_GDI_FLAG_GAMMA_VALID;
        }
        break;
    case Bc250GdiKernelStretchBlt:
        break;                  /* a point resample reads no colour and no ROP */
    default:
        return 0;
    }
    return 1;
}
