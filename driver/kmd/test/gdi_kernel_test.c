/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the GDI compute path (docs/design/gdi-hw-accel.md increment 2): the dispatch
 * emitter of driver/kmd/gdi_dispatch.c and the six kernels of driver/kmd/gdi_kernels.cl.
 *
 * What one case does:
 *
 *   1. admits a command and plans one sub-rectangle of it (gdi_render.c), which gives the item
 *   2. builds the argument block from it (Bc250GdiKernarg)
 *   3. emits the code block and the dispatch into a buffer with a chosen GPU virtual address
 *   4. DECODES that buffer strictly: the NOP that hides the code, the code against the generated
 *      table, every SET_SH_REG with its offset and its values, the grid, the initiator and the
 *      flush behind it. A packet with a field this driver never sets fails the test.
 *   5. RUNS the grid the decode found: for every work group and work item, the kernels' own source
 *      text compiled as host C (gdi_kernel_run.c), over the addresses the decoded argument block
 *      names, which are mapped back into the arena below
 *   6. compares the whole arena with a reference model of the operation, byte for byte
 *
 * The reference model addresses its pixels independently of the kernels: it walks the destination
 * rectangle and uses Bc250GdiSourceX/Y of the item, while the kernel recomputes the transform from
 * the extents in the argument block. The arithmetic is shared on purpose - it is gdi_pixel.h, whose
 * every operation gdi_render_test.c holds against a double-precision oracle - so what these cases
 * prove is the addressing, the grid, the argument packing and the register programming.
 *
 * What it does NOT prove, stated plainly: the gfx1013 machine code. Step 5 runs the C, not the
 * instructions the build extracted from the code object; it therefore says nothing about
 * instruction selection, wavefront behaviour or the real cache. tools/gpu/gdi_kernels.py gates the
 * code object's shape (no scratch, no LDS, no call, the argument layout, the work-group shape) and
 * docs/testing/gdi-hw-accel-lab.md is where the instructions themselves run.
 */

#include <stdio.h>
#include <string.h>
#include "../gdi_dispatch.h"
#include "../gdi_batch.h"
#include "../gdi_emit.h"
#include "../gfx_copy.h"
#include "../gdi_kernels.generated.h"
#include "../../amdgpu-import/nvd.h"
#include "gdi_kernel_run.h"

static int g_failures, g_checks;
/* Set around a case whose planned item is an engine item and must be run through its kernel. */
static int g_forceKernel;
/* Nonzero to fill the alpha surface with one word instead of the pattern, so that a case can reach
 * the two unconditional ClearType sentences (a component of A that is 0 or 0xFF) deliberately. */
static unsigned int g_alphaFill;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define CHECK_EQ(a, b) \
    do { g_checks++; if ((a) != (b)) { fprintf(stderr, "FAIL %s:%d: %s (0x%llX) != %s (0x%llX)\n", \
        __FILE__, __LINE__, #a, (unsigned long long)(a), #b, (unsigned long long)(b)); g_failures++; } } while (0)

#define STATIC_CHECK(name, cond) typedef char static_check_##name[(cond) ? 1 : -1]

/* The argument block the emitter writes is the structure the driver fills: 104 bytes, four
 * addresses and eighteen dwords, which is what the compiler said the kernels take. */
STATIC_CHECK(kernarg_size, sizeof(BC250_GDI_KERNARG) == BC250_GDI_KERNARG_BYTES);
STATIC_CHECK(kernarg_scalars, BC250_GDI_RUN_SCALARS ==
             (BC250_GDI_KERNARG_BYTES / 4u) - BC250_GDI_ARG_DSTPITCH);
STATIC_CHECK(kernel_count, BC250_GDI_KERNEL_COUNT == (unsigned)Bc250GdiKernels - 1u);

/* Rule 3 of gdi_batch.h: BC250_GDI_IB_DWORDS_MAX against the packet field the submitter encodes it
 * into. PACKET3_INDIRECT_BUFFER__IB_SIZE() masks to the field and shifts by nothing, so a length
 * that comes back unchanged is a length the packet can carry, which is the question
 * bc250_gfx_emit_ib() asks of every submission; the next granule up does not survive it, which is
 * what makes the constant a maximum. gdi_dispatch.c holds the same statements in the driver. */
STATIC_CHECK(ib_max_fits,
             PACKET3_INDIRECT_BUFFER__IB_SIZE(BC250_GDI_IB_DWORDS_MAX) == BC250_GDI_IB_DWORDS_MAX);
STATIC_CHECK(ib_max_is_max,
             PACKET3_INDIRECT_BUFFER__IB_SIZE(BC250_GDI_IB_DWORDS_MAX + BC250_GDI_RING_GRANULE) !=
             BC250_GDI_IB_DWORDS_MAX + BC250_GDI_RING_GRANULE);
STATIC_CHECK(ib_max_granule, (BC250_GDI_IB_DWORDS_MAX % BC250_GDI_RING_GRANULE) == 0u);
/* The two command-buffer lengths of the r3 review's reproducer, against the bound: the dense
 * BitBlt is above it and the ColorFill of tall strips is far inside it. */
STATIC_CHECK(ib_case_dense_above_bound, 224080u > BC250_GDI_CMDBUF_BOUND);
STATIC_CHECK(ib_case_strips_inside_bound, 2096u < BC250_GDI_CMDBUF_BOUND);

/* ================================================================================================
 * the arena: one flat space with a GPU virtual address of its own
 * ============================================================================================== */

#define MEM_BYTES (1u << 21)
#define ARENA_VA 0x0000000800000000ull          /* 256-byte aligned, far from the buffer's */
#define IB_VA    0x0000000900000020ull          /* 32-byte aligned, as a GDI DMA buffer is */

static unsigned char g_memory[MEM_BYTES];
static unsigned char g_expected[MEM_BYTES];

/* Where each allocation of a case lives in the arena. The gamma table is last, because its
 * allocation is the one that is not four bytes a pixel. */
#define SRC_AT   0x00000u
#define DST_AT   0x80000u
#define ALPHA_AT 0x100000u
#define GAMMA_AT 0x180000u

static unsigned int Load32(const unsigned char* at)
{
    unsigned int v;
    memcpy(&v, at, 4u);
    return v;
}

static void Store32(unsigned char* at, unsigned int v)
{
    memcpy(at, &v, 4u);
}

static void SeedMemory(void)
{
    unsigned int i;
    /* A pattern with every byte value in it, and an alpha byte that is neither 0 nor 255 in most
     * pixels, so that the ClearType edge cases are reached deliberately and not by luck. */
    for (i = 0; i < MEM_BYTES; i++) g_memory[i] = (unsigned char)((i * 37u + 11u) & 0xFFu);
    /* One gamma row pair per row of the lookup allocation: a curve and its inverse, built so that
     * the two are not the same table (an inverse table that equalled the forward one would hide a
     * swapped pair of pointers). */
    for (i = 0; i < 512u * 16u; i++) {
        unsigned int row = i / 512u, byte = i % 512u, value = byte & 0xFFu;
        g_memory[GAMMA_AT + i] = byte < 256u ? (unsigned char)((value * (row + 1u) / 16u) & 0xFFu)
                                             : (unsigned char)(255u - value);
    }
    if (g_alphaFill != 0u) {
        /* The alpha allocation of Scene: 256 by 128, four bytes a pixel. */
        for (i = 0; i < 256u * 128u * 4u; i += 4u) Store32(g_memory + ALPHA_AT + i, g_alphaFill);
    }
    memcpy(g_expected, g_memory, MEM_BYTES);
}

/* ================================================================================================
 * the decode
 * ============================================================================================== */

/* What one dispatch said, as the decode read it back out of the buffer. */
typedef struct DECODED {
    unsigned long long CodeVa;              /* from the code block */
    unsigned long long ProgramVa;           /* from COMPUTE_PGM_LO and PGM_HI */
    unsigned long long ArgVa;               /* from COMPUTE_USER_DATA_4 and 5 */
    unsigned int Block[BC250_GDI_KERNARG_BYTES / 4u];
    unsigned int Rsrc1, Rsrc2, Rsrc3, TmpRing, ResourceLimits;
    unsigned int Threads[3], Groups[3], Initiator, CuMask[4];
    unsigned int UserData[4];               /* USER_DATA_0..3, which must be zeros */
    unsigned int ReqCtrl[6];
    unsigned int Event, Dwords;
} DECODED;

/* The payload length of a type-3 NOP, in dwords, or 0 if this is not one. */
static unsigned int NopPayload(unsigned int header)
{
    if ((header >> 30) != 3u) return 0;
    if (((header >> 8) & 0xFFu) != (unsigned int)PACKET3_NOP) return 0;
    if (header & 2u) return 0;                          /* no shader-type bit on a NOP */
    return ((header >> 16) & 0x3FFFu) + 1u;
}

/* The code block: the NOP, the padding, and the code itself against the generated table. */
static int DecodeCode(const unsigned int* buffer, unsigned int dwords, unsigned long long bufferVa,
                      unsigned long long* codeVa)
{
    unsigned int payload, pad, i;
    if (dwords < 1u) return 0;
    payload = NopPayload(buffer[0]);
    if (payload == 0u || payload < BC250_GDI_CODE_DWORDS) return 0;
    pad = payload - BC250_GDI_CODE_DWORDS;
    if (1u + payload != dwords) return 0;
    for (i = 0; i < pad; i++)
        if (buffer[1 + i] != 0u) return 0;
    for (i = 0; i < BC250_GDI_CODE_DWORDS; i++)
        if (buffer[1 + pad + i] != g_Bc250GdiCode[i]) return 0;
    *codeVa = bufferVa + (unsigned long long)(1u + pad) * 4u;
    if (*codeVa & (unsigned long long)(BC250_GDI_CODE_ALIGN - 1u)) return 0;
    return 1;
}

/* A SET_SH_REG of `count` registers at `offset`, with the shader-type bit set. Returns the values
 * or 0, and advances *at by the whole packet. */
static const unsigned int* TakeSetSh(const unsigned int* buffer, unsigned int dwords,
                                     unsigned int* at, unsigned int offset, unsigned int count)
{
    const unsigned int* values;
    if (*at + count + 2u > dwords) return 0;
    if (buffer[*at] != (unsigned int)PACKET3_COMPUTE(PACKET3_SET_SH_REG, count)) return 0;
    if (buffer[*at + 1u] != offset) return 0;
    values = buffer + *at + 2u;
    *at += count + 2u;
    return values;
}

static int DecodeDispatch(const unsigned int* buffer, unsigned int dwords,
                          unsigned long long bufferVa, DECODED* out)
{
    const unsigned int* values;
    unsigned int at = 0, payload, pad, i;

    memset(out, 0, sizeof(*out));
    if (dwords < 1u) return 0;
    payload = NopPayload(buffer[0]);
    if (payload == 0u || payload < BC250_GDI_KERNARG_BYTES / 4u) return 0;
    pad = payload - BC250_GDI_KERNARG_BYTES / 4u;
    for (i = 0; i < pad; i++)
        if (buffer[1 + i] != 0u) return 0;
    out->ArgVa = bufferVa + (unsigned long long)(1u + pad) * 4u;
    if (out->ArgVa & 7u) return 0;                      /* the kernarg alignment the compiler asked for */
    memcpy(out->Block, buffer + 1u + pad, sizeof(out->Block));
    at = 1u + pad + BC250_GDI_KERNARG_BYTES / 4u;

    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_TMPRING_SIZE, 1u))) return 0;
    out->TmpRing = values[0];
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_REQ_CTRL, 6u))) return 0;
    memcpy(out->ReqCtrl, values, sizeof(out->ReqCtrl));
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_STATIC_THREAD_MGMT_SE0, 2u))) return 0;
    out->CuMask[0] = values[0]; out->CuMask[1] = values[1];
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_STATIC_THREAD_MGMT_SE2, 2u))) return 0;
    out->CuMask[2] = values[0]; out->CuMask[3] = values[1];
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_PGM_LO, 2u))) return 0;
    /* COMPUTE_PGM_LO is the program address shifted right by 8, PGM_HI bits 47:40 of it. */
    out->ProgramVa = ((unsigned long long)values[1] << 40) | ((unsigned long long)values[0] << 8);
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_PGM_RSRC1, 2u))) return 0;
    out->Rsrc1 = values[0]; out->Rsrc2 = values[1];
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_PGM_RSRC3, 1u))) return 0;
    out->Rsrc3 = values[0];
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_NUM_THREAD_X, 3u))) return 0;
    memcpy(out->Threads, values, sizeof(out->Threads));
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_USER_DATA_0, 6u))) return 0;
    memcpy(out->UserData, values, sizeof(out->UserData));
    if (out->ArgVa != (((unsigned long long)values[5] << 32) | values[4])) return 0;
    if (!(values = TakeSetSh(buffer, dwords, &at, BC250_GDI_SH_RESOURCE_LIMITS, 1u))) return 0;
    out->ResourceLimits = values[0];

    if (at + 5u > dwords) return 0;
    if (buffer[at] != (unsigned int)PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3)) return 0;
    out->Groups[0] = buffer[at + 1u];
    out->Groups[1] = buffer[at + 2u];
    out->Groups[2] = buffer[at + 3u];
    out->Initiator = buffer[at + 4u];
    at += 5u;

    if (at + 2u > dwords) return 0;
    if (buffer[at] != (unsigned int)PACKET3(PACKET3_EVENT_WRITE, 0)) return 0;
    out->Event = buffer[at + 1u];
    at += 2u;
    out->Dwords = at;
    return at == dwords;
}

/* ================================================================================================
 * the reference model
 * ============================================================================================== */

/* The arena offset of each allocation slot of a case. */
typedef struct LAYOUT { unsigned int At[4]; } LAYOUT;

static void ReferenceKernel(const BC250_GDI_OP* op, const BC250_GDI_SURFACE* surfaces,
                            const BC250_GDI_ITEM* item, const LAYOUT* layout)
{
    unsigned int x, y;
    unsigned int redFirst = surfaces[op->DstAllocationIndex].RedFirst;
    unsigned int hasSource = (unsigned int)Bc250GdiOpHasSource(op->OpCode);

    for (y = 0; y < item->DstHeight; y++) {
        for (x = 0; x < item->DstWidth; x++) {
            unsigned char* d = g_expected + layout->At[op->DstAllocationIndex] + item->DstOffset +
                (unsigned long long)y * item->DstPitch + 4u * (unsigned long long)x;
            unsigned int dst = Load32(d), src = 0, out;
            if (hasSource) {
                /* The item's own transform, which is the independent path: the kernel rebuilds it
                 * from the extents in the argument block instead. */
                int sx = Bc250GdiSourceX(item, (int)item->DstLeft + (int)x);
                int sy = Bc250GdiSourceY(item, (int)item->DstTop + (int)y);
                src = Load32(g_expected + layout->At[op->SrcAllocationIndex] + item->SrcBase +
                             (unsigned long long)(unsigned int)sy * item->SrcPitch +
                             4u * (unsigned long long)(unsigned int)sx);
            }
            switch (item->Kernel) {
            case Bc250GdiKernelBitBltRop:
                out = Bc250GdiBitBltPixel(op->Rop, src, dst);
                break;
            case Bc250GdiKernelColorFillRop:
                /* The WDK header's reading of Color, written here and not taken from the driver:
                 * the field is already in the destination surface's format, so a bitwise fill uses
                 * the dword as it arrived whatever the channel order is. */
                out = Bc250GdiColorFillPixel(op->Rop, op->Color, dst);
                break;
            case Bc250GdiKernelStretchBlt:
                out = src;
                break;
            case Bc250GdiKernelAlphaBlend:
                out = Bc250GdiFromArgb(
                    Bc250GdiAlphaBlendPixel(Bc250GdiToArgb(src, redFirst),
                                            Bc250GdiToArgb(dst, redFirst),
                                            op->SourceConstantAlpha, op->SourceHasAlpha),
                    redFirst);
                break;
            case Bc250GdiKernelTransparentBlt:
                /* The key is "the physical transparent color in the source surface, in 32-bit ARGB
                 * unsigned pixel format (as defined by the D3DDDIFMT_A8R8G8B8 value ...)"
                 * (ref/ddi-display/d3dkmddi.md:22037), so it enters the comparison as it arrived
                 * while the source and destination words are converted into that order. */
                out = Bc250GdiFromArgb(
                    Bc250GdiTransparentPixel(Bc250GdiToArgb(src, redFirst),
                                             Bc250GdiToArgb(dst, redFirst),
                                             op->Color,
                                             op->HonorAlpha),
                    redFirst);
                break;
            case Bc250GdiKernelClearTypeBlend: {
                unsigned int alphaRedFirst = surfaces[op->AlphaSurfAllocationIndex].RedFirst;
                const unsigned char* gamma = g_expected +
                    layout->At[op->GammaSurfAllocationIndex] + item->GammaOffset;
                unsigned int a = Load32(g_expected + layout->At[op->AlphaSurfAllocationIndex] +
                                        item->AlphaOffset +
                                        (unsigned long long)y * item->AlphaPitch +
                                        4u * (unsigned long long)x);
                /* Both foreground colours are "in 32-bit ARGB unsigned pixel format", gamma
                 * corrected and not (:21701, :21721), so they are not converted either. */
                out = Bc250GdiFromArgb(
                    Bc250GdiClearTypePixel(Bc250GdiToArgb(dst, redFirst),
                                           Bc250GdiToArgb(a, alphaRedFirst),
                                           op->Color,
                                           op->Color2,
                                           item->GammaValid,
                                           gamma, gamma + BC250_GDI_GAMMA_ENTRIES),
                    redFirst);
                break;
            }
            default:
                CHECK(0);
                return;
            }
            Store32(d, out);
        }
    }
}

/* ================================================================================================
 * one case, end to end
 * ============================================================================================== */

static unsigned char* ArenaAt(unsigned long long va, unsigned long long bytes)
{
    if (va < ARENA_VA || va - ARENA_VA + bytes > MEM_BYTES) return 0;
    return g_memory + (va - ARENA_VA);
}

static void RunCase(const char* what, const BC250_GDI_OP* op, const BC250_GDI_SURFACE* surfaces,
                    unsigned int count, const LAYOUT* layout, const BC250_GDI_RECT* sub)
{
    static unsigned int buffer[8192];
    BC250_GDI_ITEM item;
    BC250_GDI_KERNARG arg;
    DECODED decoded;
    const BC250_GDI_CODE_KERNEL* program;
    unsigned long long vas[4], codeVa = 0;
    unsigned int i, code, dispatch, gx, gy, wx, wy, unrun = 0;
    unsigned char* dst;

    for (i = 0; i < count; i++) vas[i] = ARENA_VA + layout->At[i];

    if (Bc250GdiPlanItem(op, surfaces, count, sub, &item) != Bc250GdiOk) {
        fprintf(stderr, "FAIL %s: the command was refused\n", what);
        g_checks++; g_failures++;
        return;
    }
    /* An engine item taken through its kernel, which is the path gdi_batch.c falls back to. */
    if (g_forceKernel) CHECK_EQ(Bc250GdiItemToKernel(op, &item), 1);
    if (item.Work != (unsigned int)Bc250GdiWorkKernel || item.DstWidth == 0u) {
        fprintf(stderr, "FAIL %s: not a kernel item (work %u, %ux%u)\n", what, item.Work,
                item.DstWidth, item.DstHeight);
        g_checks++; g_failures++;
        return;
    }

    SeedMemory();
    ReferenceKernel(op, surfaces, &item, layout);

    CHECK_EQ(Bc250GdiKernarg(op, surfaces, vas, count, &item, &arg), 1);
    code = Bc250EmitGdiCode(buffer, 8192u, IB_VA, &codeVa);
    CHECK_EQ(code, Bc250GdiCodeDwords(IB_VA));
    CHECK(code != 0u);
    if (!code) return;
    CHECK_EQ(DecodeCode(buffer, code, IB_VA, &codeVa), 1);

    dispatch = Bc250EmitGdiDispatch(item.Kernel, &arg, codeVa, buffer + code, 8192u - code,
                                    IB_VA + (unsigned long long)code * 4u);
    CHECK_EQ(dispatch, Bc250GdiDispatchDwords(IB_VA + (unsigned long long)code * 4u));
    CHECK(dispatch != 0u);
    if (!dispatch) return;
    CHECK_EQ(DecodeDispatch(buffer + code, dispatch, IB_VA + (unsigned long long)code * 4u,
                            &decoded), 1);

    /* What the decode must find, whatever the operation is. */
    program = &g_Bc250GdiCodeKernels[item.Kernel];
    CHECK_EQ(decoded.ProgramVa, codeVa + (unsigned long long)program->Offset * 4u);
    CHECK_EQ(decoded.Rsrc1, program->Rsrc1);
    CHECK_EQ(decoded.Rsrc2, program->Rsrc2);
    CHECK_EQ(decoded.Rsrc3, 0u);
    CHECK_EQ(decoded.TmpRing, 0u);
    CHECK_EQ(decoded.ResourceLimits, 0u);
    CHECK_EQ(decoded.Threads[0], BC250_GDI_GROUP_X);
    CHECK_EQ(decoded.Threads[1], BC250_GDI_GROUP_Y);
    CHECK_EQ(decoded.Threads[2], 1u);
    CHECK_EQ(decoded.Groups[0], (arg.Width + BC250_GDI_GROUP_X - 1u) / BC250_GDI_GROUP_X);
    CHECK_EQ(decoded.Groups[1], (arg.Height + BC250_GDI_GROUP_Y - 1u) / BC250_GDI_GROUP_Y);
    CHECK_EQ(decoded.Groups[2], 1u);
    CHECK_EQ(decoded.Initiator, BC250_GDI_DISPATCH_INITIATOR);
    CHECK_EQ(decoded.Event, (unsigned int)(EVENT_TYPE(BC250_GDI_EVENT_CS_PARTIAL_FLUSH) |
             EVENT_INDEX(PACKET3_EVENT_WRITE__EVENT_INDEX__CS_PARTIAL_FLUSH)));
    for (i = 0; i < 4u; i++) {
        CHECK_EQ(decoded.CuMask[i], 0xFFFFFFFFu);
        CHECK_EQ(decoded.UserData[i], 0u);
    }
    for (i = 0; i < 6u; i++) CHECK_EQ(decoded.ReqCtrl[i], 0u);
    /* The argument block in the buffer must be the structure the driver filled, field for field. */
    CHECK_EQ(decoded.Block[BC250_GDI_ARG_DST], (unsigned int)arg.Dst);
    CHECK_EQ(decoded.Block[BC250_GDI_ARG_DST + 1u], (unsigned int)(arg.Dst >> 32));
    CHECK_EQ(decoded.Block[BC250_GDI_ARG_SRC], (unsigned int)arg.Src);
    CHECK_EQ(decoded.Block[BC250_GDI_ARG_WIDTH], arg.Width);
    CHECK_EQ(decoded.Block[BC250_GDI_ARG_HEIGHT], arg.Height);
    CHECK_EQ(decoded.Block[BC250_GDI_ARG_FLAGS], arg.Flags);

    /* And the run: every work group of the decoded grid, every work item of it. */
    dst = ArenaAt(((unsigned long long)decoded.Block[BC250_GDI_ARG_DST + 1u] << 32) |
                  decoded.Block[BC250_GDI_ARG_DST], 4u);
    CHECK(dst != 0);
    if (!dst) return;
    for (gy = 0; gy < decoded.Groups[1]; gy++) {
        for (gx = 0; gx < decoded.Groups[0]; gx++) {
            for (wy = 0; wy < BC250_GDI_GROUP_Y; wy++) {
                for (wx = 0; wx < BC250_GDI_GROUP_X; wx++) {
                    const unsigned char* src = 0;
                    const unsigned char* alpha = 0;
                    const unsigned char* gamma = 0;
                    unsigned long long va;
                    g_Bc250GdiWorkgroupX = gx;
                    g_Bc250GdiWorkgroupY = gy;
                    g_Bc250GdiWorkitemX = wx;
                    g_Bc250GdiWorkitemY = wy;
                    va = ((unsigned long long)decoded.Block[BC250_GDI_ARG_SRC + 1u] << 32) |
                        decoded.Block[BC250_GDI_ARG_SRC];
                    if (va) src = ArenaAt(va, 4u);
                    va = ((unsigned long long)decoded.Block[BC250_GDI_ARG_ALPHA + 1u] << 32) |
                        decoded.Block[BC250_GDI_ARG_ALPHA];
                    if (va) alpha = ArenaAt(va, 4u);
                    va = ((unsigned long long)decoded.Block[BC250_GDI_ARG_GAMMA + 1u] << 32) |
                        decoded.Block[BC250_GDI_ARG_GAMMA];
                    if (va) gamma = ArenaAt(va, BC250_GDI_GAMMA_ENTRIES * 2u);
                    if (!Bc250GdiRunKernel(item.Kernel, dst, src, alpha, gamma,
                                           &decoded.Block[BC250_GDI_ARG_DSTPITCH]))
                        unrun++;
                }
            }
        }
    }
    CHECK_EQ(unrun, 0u);

    /* The whole arena: a kernel that wrote one byte outside its rectangle fails here, and so does
     * one that read the wrong source pixel. */
    if (memcmp(g_memory, g_expected, MEM_BYTES) != 0) {
        unsigned int at;
        for (at = 0; at < MEM_BYTES; at++)
            if (g_memory[at] != g_expected[at]) break;
        fprintf(stderr, "FAIL %s: first difference at 0x%X (got 0x%02X, want 0x%02X)\n",
                what, at, g_memory[at], g_expected[at]);
        g_failures++;
    }
    g_checks++;
}

/* ================================================================================================
 * the cases
 * ============================================================================================== */

static BC250_GDI_SURFACE Surface(unsigned int w, unsigned int h, unsigned int bpp,
                                 unsigned int type, unsigned int redFirst)
{
    BC250_GDI_SURFACE s;
    memset(&s, 0, sizeof(s));
    s.Width = w; s.Height = h; s.Bpp = bpp;
    s.Pitch = ((w * bpp) + 255u) & ~255u;
    s.Bytes = (unsigned long long)s.Pitch * h;
    s.Type = type; s.RedFirst = redFirst; s.Present = 1;
    return s;
}

static BC250_GDI_RECT Rect(int l, int t, int r, int b)
{
    BC250_GDI_RECT v; v.Left = l; v.Top = t; v.Right = r; v.Bottom = b; return v;
}

/* The four slots every case uses: 0 the destination, 1 the source, 2 the ClearType alpha surface,
 * 3 the gamma lookup table. The layout puts each at its own place in the arena. */
static void Scene(BC250_GDI_SURFACE* s, LAYOUT* layout, unsigned int redFirst,
                  unsigned int alphaRedFirst)
{
    s[0] = Surface(256, 128, 4, BC250_GDISURFACE_TEXTURE, redFirst);
    s[1] = Surface(256, 128, 4, BC250_GDISURFACE_TEXTURE, redFirst);
    s[2] = Surface(256, 128, 4, BC250_GDISURFACE_TEXTURE, alphaRedFirst);
    s[3] = Surface(BC250_GDI_GAMMA_WIDTH, BC250_GDI_GAMMA_HEIGHT, 1,
                   BC250_GDISURFACE_LOOKUPTABLE, 0);
    layout->At[0] = DST_AT;
    layout->At[1] = SRC_AT;
    layout->At[2] = ALPHA_AT;
    layout->At[3] = GAMMA_AT;
}

static BC250_GDI_OP Op(unsigned int opcode, BC250_GDI_RECT src, BC250_GDI_RECT dst)
{
    BC250_GDI_OP op;
    memset(&op, 0, sizeof(op));
    op.OpCode = opcode;
    op.SrcRect = src;
    op.DstRect = dst;
    op.DstAllocationIndex = 0;
    op.SrcAllocationIndex = 1;
    op.AlphaSurfAllocationIndex = 2;
    op.GammaSurfAllocationIndex = 3;
    op.Gamma = BC250_GDI_INVALID_GAMMA;
    op.NumSubRects = 1;
    return op;
}

static void TestBitBltRops(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    unsigned int rop;
    Scene(s, &layout, 0, 0);
    for (rop = BC250_GDI_ROP_SRCINVERT; rop <= BC250_GDI_ROP_SRCOR; rop++) {
        BC250_GDI_OP op = Op(BC250_GDIOP_BITBLT, Rect(3, 5, 67, 45), Rect(100, 60, 164, 100));
        BC250_GDI_RECT sub = op.DstRect;
        op.Rop = rop;
        RunCase("bitblt rop", &op, s, 4, &layout, &sub);
        /* A rectangle that is not a whole number of work groups in either axis, so the last group
         * of every row and column overshoots it. */
        op = Op(BC250_GDIOP_BITBLT, Rect(0, 0, 13, 7), Rect(5, 9, 18, 16));
        op.Rop = rop;
        sub = op.DstRect;
        RunCase("bitblt rop, ragged", &op, s, 4, &layout, &sub);
        /* One pixel. */
        op = Op(BC250_GDIOP_BITBLT, Rect(255, 127, 256, 128), Rect(0, 0, 1, 1));
        op.Rop = rop;
        sub = op.DstRect;
        RunCase("bitblt rop, 1x1", &op, s, 4, &layout, &sub);
    }
    /* A red-first pair: the three ROPs are bitwise and the channel order must not enter them, so
     * the same reference model must still agree. */
    Scene(s, &layout, 1, 0);
    {
        BC250_GDI_OP op = Op(BC250_GDIOP_BITBLT, Rect(3, 5, 67, 45), Rect(100, 60, 164, 100));
        BC250_GDI_RECT sub = op.DstRect;
        op.Rop = BC250_GDI_ROP_SRCAND;
        RunCase("bitblt rop, red first", &op, s, 4, &layout, &sub);
    }
}

static void TestColorFillRops(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    unsigned int rop, redFirst;
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        Scene(s, &layout, redFirst, 0);
        for (rop = BC250_GDI_CF_PATINVERT; rop <= BC250_GDI_CF_PATOR; rop++) {
            BC250_GDI_OP op = Op(BC250_GDIOP_COLORFILL, Rect(0, 0, 0, 0), Rect(7, 11, 71, 51));
            BC250_GDI_RECT sub = op.DstRect;
            op.Rop = rop;
            op.Color = 0x8041C3A5u;
            RunCase("colorfill rop", &op, s, 4, &layout, &sub);
        }
    }
}

static void TestStretchBlt(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    BC250_GDI_OP op;
    BC250_GDI_RECT sub;
    Scene(s, &layout, 0, 0);

    /* Up, down, and both at once: a point resample in each direction. */
    op = Op(BC250_GDIOP_STRETCHBLT, Rect(0, 0, 16, 16), Rect(32, 32, 96, 96));
    sub = op.DstRect;
    RunCase("stretchblt up", &op, s, 4, &layout, &sub);
    op = Op(BC250_GDIOP_STRETCHBLT, Rect(0, 0, 200, 100), Rect(10, 10, 60, 35));
    sub = op.DstRect;
    RunCase("stretchblt down", &op, s, 4, &layout, &sub);
    op = Op(BC250_GDIOP_STRETCHBLT, Rect(5, 7, 105, 27), Rect(20, 20, 60, 100));
    sub = op.DstRect;
    RunCase("stretchblt mixed", &op, s, 4, &layout, &sub);
    /* One source pixel over a whole rectangle, and one destination pixel out of a whole source. */
    op = Op(BC250_GDIOP_STRETCHBLT, Rect(9, 9, 10, 10), Rect(0, 0, 24, 24));
    sub = op.DstRect;
    RunCase("stretchblt 1 to many", &op, s, 4, &layout, &sub);
    op = Op(BC250_GDIOP_STRETCHBLT, Rect(0, 0, 64, 64), Rect(5, 5, 6, 6));
    sub = op.DstRect;
    RunCase("stretchblt many to 1", &op, s, 4, &layout, &sub);

    /* A sub-rectangle smaller than DstRect: the transform must still measure from DstRect's own
     * origin, so this case samples exactly the pixels the unclipped one did. */
    op = Op(BC250_GDIOP_STRETCHBLT, Rect(0, 0, 16, 16), Rect(32, 32, 96, 96));
    sub = Rect(50, 40, 70, 90);
    RunCase("stretchblt clipped sub-rectangle", &op, s, 4, &layout, &sub);
}

static void TestAlphaBlend(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    unsigned int alphas[5], i, hasAlpha, redFirst;
    alphas[0] = 0u; alphas[1] = 1u; alphas[2] = 128u; alphas[3] = 254u; alphas[4] = 255u;
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        Scene(s, &layout, redFirst, 0);
        for (hasAlpha = 0; hasAlpha <= 1u; hasAlpha++) {
            for (i = 0; i < 5u; i++) {
                BC250_GDI_OP op = Op(BC250_GDIOP_ALPHABLEND, Rect(1, 2, 65, 42),
                                     Rect(30, 40, 94, 80));
                BC250_GDI_RECT sub = op.DstRect;
                op.SourceConstantAlpha = alphas[i];
                op.SourceHasAlpha = hasAlpha;
                RunCase("alphablend", &op, s, 4, &layout, &sub);
            }
        }
        /* With a stretch, which the ALPHABLEND Remarks define the same way STRETCHBLT does. */
        {
            BC250_GDI_OP op = Op(BC250_GDIOP_ALPHABLEND, Rect(0, 0, 20, 10), Rect(8, 8, 68, 38));
            BC250_GDI_RECT sub = op.DstRect;
            op.SourceConstantAlpha = 200u;
            op.SourceHasAlpha = 1u;
            RunCase("alphablend stretched", &op, s, 4, &layout, &sub);
        }
    }
}

static void TestTransparentBlt(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    unsigned int honor, redFirst;
    /* The colour keys below are read out of the seeded source, so seed it first. */
    SeedMemory();
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        Scene(s, &layout, redFirst, 0);
        for (honor = 0; honor <= 1u; honor++) {
            BC250_GDI_OP op = Op(BC250_GDIOP_TRANSPARENTBLT, Rect(0, 0, 64, 40),
                                 Rect(16, 24, 80, 64));
            BC250_GDI_RECT sub = op.DstRect;
            op.HonorAlpha = honor;
            /* A key that the seeded pattern does hit: the pixel at (0,0) of the source, so at
             * least one pixel of the rectangle is left alone and the rest is copied. */
            op.Color = Bc250GdiToArgb(Load32(g_memory + SRC_AT), redFirst) &
                (honor ? 0xFFFFFFFFu : 0x00FFFFFFu);
            RunCase("transparentblt", &op, s, 4, &layout, &sub);
            /* And a key nothing matches, so every pixel is copied. */
            op.Color = 0x00123457u;
            RunCase("transparentblt, no match", &op, s, 4, &layout, &sub);
        }
        /* Stretched, which this operation also defines. */
        {
            BC250_GDI_OP op = Op(BC250_GDIOP_TRANSPARENTBLT, Rect(0, 0, 32, 16),
                                 Rect(10, 10, 74, 42));
            BC250_GDI_RECT sub = op.DstRect;
            op.Color = 0x00FFFFFFu;
            RunCase("transparentblt stretched", &op, s, 4, &layout, &sub);
        }
    }
}

static void TestClearTypeBlend(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    unsigned int gamma, redFirst, alphaRedFirst;
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        for (alphaRedFirst = 0; alphaRedFirst <= 1u; alphaRedFirst++) {
            Scene(s, &layout, redFirst, alphaRedFirst);
            for (gamma = 0; gamma <= 1u; gamma++) {
                BC250_GDI_OP op = Op(BC250_GDIOP_CLEARTYPEBLEND, Rect(0, 0, 0, 0),
                                     Rect(12, 16, 76, 56));
                BC250_GDI_RECT sub = op.DstRect;
                op.Color = 0x00203040u;
                op.Color2 = 0x00F0E0D0u;
                op.Gamma = gamma ? 9u : BC250_GDI_INVALID_GAMMA;
                op.DstToAlphaOffsetX = -4;
                op.DstToAlphaOffsetY = -6;
                RunCase("cleartypeblend", &op, s, 4, &layout, &sub);
            }
        }
    }
    /* Every gamma row, so that the row the formula reads is the row the plan selected. */
    Scene(s, &layout, 0, 0);
    for (gamma = 0; gamma < BC250_GDI_GAMMA_HEIGHT; gamma++) {
        BC250_GDI_OP op = Op(BC250_GDIOP_CLEARTYPEBLEND, Rect(0, 0, 0, 0), Rect(0, 0, 9, 5));
        BC250_GDI_RECT sub = op.DstRect;
        op.Color = 0x00112233u;
        op.Color2 = 0x00445566u;
        op.Gamma = gamma;
        RunCase("cleartypeblend gamma row", &op, s, 4, &layout, &sub);
    }
}

/* ================================================================================================
 * which channel order a Color field is in
 * ============================================================================================== */

/* The reference model above encodes the WDK header's reading of the three Color fields, and the
 * cases so far would pass under either reading as long as the driver and the model agreed. These
 * do not: each one states what the destination must hold afterwards, in bytes, for a destination
 * whose channel order is the one that tells the two readings apart.
 */
static void TestColourOrder(void)
{
    BC250_GDI_SURFACE s[4];
    LAYOUT layout;
    unsigned int redFirst;
    /* Distinct in red and in blue, so a swap cannot hide: red 0xC0, blue 0x12. */
    const unsigned int colour = 0x80C03412u;

    /* The three colour fields of this DDI do not share one encoding, so each is tested against its
     * own contract line rather than against one rule for all three. The model above now carries
     * each field's own encoding, which is what makes these cases decide anything: a conversion
     * applied twice shows up as a kernel that disagrees with the model, pixel for pixel. */

    /* ColorFill.Color: "Color in the format of the destination surface" (d3dkmddi.md:21793, the
     * WDK declaration), so the dword lands as it arrived for both channel orders. */
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        BC250_GDI_OP op = Op(BC250_GDIOP_COLORFILL, Rect(0, 0, 0, 0), Rect(4, 4, 12, 12));
        BC250_GDI_RECT sub = op.DstRect;
        Scene(s, &layout, redFirst, 0);
        op.Rop = BC250_GDI_CF_PATCOPY;
        op.Color = colour;
        /* PATCOPY is an engine item, so take the fill through the kernel the batch falls back to. */
        g_forceKernel = 1;
        RunCase("colorfill patcopy colour order", &op, s, 4, &layout, &sub);
        g_forceKernel = 0;
        CHECK_EQ(Load32(g_memory + layout.At[0] + 4u * s[0].Pitch + 4u * 4u), colour);
        CHECK(Load32(g_memory + layout.At[0] + 4u * s[0].Pitch + 4u * 4u) !=
              Bc250GdiSwapRb(colour));
    }

    /* TransparentBlt.Color: "the physical transparent color in the source surface, in 32-bit ARGB
     * unsigned pixel format (as defined by the D3DDDIFMT_A8R8G8B8 value ...)" (:22037). It is NOT
     * in the surface's order, which the review shows: on an A8B8G8R8 destination a key packed in
     * the surface's order makes the kernel copy the pixel it was asked to keep. Both cases assert
     * the destination word itself, so the outcome does not depend on the model. */
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        /* The source is taken one pixel to the right, so that the source word and the destination
         * word under it are different numbers: the seed is a single ramp and the two allocations
         * start a whole number of its periods apart. */
        BC250_GDI_OP op = Op(BC250_GDIOP_TRANSPARENTBLT, Rect(1, 0, 9, 8), Rect(0, 0, 8, 8));
        BC250_GDI_RECT sub = op.DstRect;
        unsigned int raw, kept, key;
        Scene(s, &layout, redFirst, 0);
        SeedMemory();
        raw = Load32(g_memory + layout.At[1] + 4u);     /* the source pixel at (1,0) */
        kept = Load32(g_memory + layout.At[0]);         /* the destination pixel at (0,0) */
        key = Bc250GdiToArgb(raw, redFirst);            /* the same pixel, in the key's own order */
        /* The seed gives every pixel unequal red and blue (its bytes step by 37), so the exchange
         * of this key is a different number and the two cases below cannot coincide. */
        CHECK(Bc250GdiByte(key, 16) != (key & 0xFFu));
        CHECK(raw != kept);

        op.HonorAlpha = 1u;
        op.Color = key;
        RunCase("transparentblt key in a8r8g8b8", &op, s, 4, &layout, &sub);
        /* The key matches that source pixel, so it is not copied. */
        CHECK_EQ(Load32(g_memory + layout.At[0]), kept);

        op.Color = Bc250GdiSwapRb(key);
        RunCase("transparentblt key exchanged", &op, s, 4, &layout, &sub);
        /* The exchanged key matches nothing, so the pixel is copied. */
        CHECK_EQ(Load32(g_memory + layout.At[0]), raw);

        /* And a key no pixel of the source can hold: HonorAlpha compares the whole word, so an
         * alpha byte of 0xFF over a pattern whose alpha bytes are not 0xFF matches nowhere. */
        op.Color = 0xFF000000u | (key & 0x00FFFFFFu);
        if (Bc250GdiByte(key, 24) != 0xFFu)
        {
            RunCase("transparentblt key that matches nothing", &op, s, 4, &layout, &sub);
            CHECK_EQ(Load32(g_memory + layout.At[0]), raw);
        }
    }

    /* ClearTypeBlend.Color2: the foreground colour "in 32-bit ARGB unsigned pixel format", not
     * gamma corrected (:21721). With every component of A at 0xFF the DDI's unconditional sentence
     * demands the output component be Color2's, so the destination word is Color2's three channels
     * in the destination's order - which is the direct witness for this field. */
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        BC250_GDI_OP op = Op(BC250_GDIOP_CLEARTYPEBLEND, Rect(0, 0, 0, 0), Rect(8, 4, 24, 12));
        BC250_GDI_RECT sub = op.DstRect;
        unsigned int got;
        Scene(s, &layout, redFirst, 0);
        op.Color = colour;
        op.Color2 = 0x00D01256u;                 /* red 0xD0, blue 0x56, unequal */
        g_alphaFill = 0xFFFFFFFFu;
        RunCase("cleartypeblend color2 in a8r8g8b8", &op, s, 4, &layout, &sub);
        g_alphaFill = 0u;
        /* the destination pixel at (8,4), the sub-rectangle's own corner */
        got = Bc250GdiToArgb(Load32(g_memory + layout.At[0] + 4u * s[0].Pitch + 4u * 8u),
                             redFirst);
        CHECK_EQ(got & 0x00FFFFFFu, op.Color2 & 0x00FFFFFFu);
    }

    /* ClearTypeBlend.Color: the same encoding, gamma corrected (:21701). A is 0x80 everywhere, so
     * neither sentence fires and the blend itself reads Color. The two runs differ only by the
     * exchange of red and blue in Color, so a driver that converted the field would produce the
     * same pixels twice; the model comparison inside RunCase is what fails then. */
    for (redFirst = 0; redFirst <= 1u; redFirst++) {
        unsigned int at, straight, exchanged;
        BC250_GDI_OP op = Op(BC250_GDIOP_CLEARTYPEBLEND, Rect(0, 0, 0, 0), Rect(8, 4, 24, 12));
        BC250_GDI_RECT sub = op.DstRect;
        Scene(s, &layout, redFirst, 0);
        at = 4u * s[0].Pitch + 4u * 8u;                  /* the pixel at (8,4) */
        op.Color2 = 0x00445566u;
        g_alphaFill = 0x80808080u;
        op.Color = colour & 0x00FFFFFFu;
        RunCase("cleartypeblend color in a8r8g8b8", &op, s, 4, &layout, &sub);
        straight = Load32(g_memory + layout.At[0] + at);
        op.Color = Bc250GdiSwapRb(colour & 0x00FFFFFFu);
        RunCase("cleartypeblend color exchanged", &op, s, 4, &layout, &sub);
        exchanged = Load32(g_memory + layout.At[0] + at);
        g_alphaFill = 0u;
        CHECK(straight != exchanged);
    }
}

/* ================================================================================================
 * the capacity contract of a whole call (gdi_batch.h)
 * ============================================================================================== */

/* What wddm.c asks dxgkrnl for, for a command buffer of BC250_GDI_CMDBUF_BOUND bytes. The test
 * checks the two against each other rather than repeating the arithmetic. */
/* Large enough for the longest stream the submitter can encode (BC250_GDI_IB_DWORDS_MAX), because
 * the review's dense row-path case is translated here at exactly that capacity. */
#define BATCH_BYTES (4u * 1024u * 1024u)
#define BATCH_DWORDS (BATCH_BYTES / 4u)
/* What Begin works inside when it is handed this whole buffer: rule 3 clamps the capacity to what
 * one indirect buffer can carry, which is a granule below the buffer's own size. */
#define BATCH_CAP_DWORDS (BATCH_DWORDS < BC250_GDI_IB_DWORDS_MAX ? BATCH_DWORDS : \
                          BC250_GDI_IB_DWORDS_MAX)
/* The capacity the hand-picked 256 KiB buffer used to give. Kept as a tight case: it is where an
 * item that could take the per-row engine path must give it up to the items behind it. */
#define BATCH_TIGHT_DWORDS (256u * 1024u / 4u)
/* The most sub-rectangles the smallest bound this driver asks for can carry. The dense cases that
 * are translated and compared drive that many: the whole bound is twice as large, and running
 * 12287 dispatches through the host model says nothing the 6143 below do not. The budget cases use
 * the whole bound, which is where the capacity theorem has to hold. */
#define BATCH_SUBRECTS ((BC250_GDI_CMDBUF_FLOOR - BC250_GDI_RECT_FLOOR_BYTES) / \
                        BC250_GDI_SUBRECT_BYTES)
#define BOUND_SUBRECTS ((BC250_GDI_CMDBUF_BOUND - BC250_GDI_RECT_FLOOR_BYTES) / \
                        BC250_GDI_SUBRECT_BYTES)

static unsigned int g_batch[BATCH_DWORDS];
static unsigned int g_scratch[BATCH_DWORDS];

/* A tall, narrow pair of surfaces: the shape of the counterexample that increment 1's per-item
 * capacity lost. Columns pixels wide, one dword a pixel, no pitch padding. */
static void TallScene(BC250_GDI_SURFACE* s, LAYOUT* layout, unsigned int columns,
                      unsigned int height, unsigned int redFirst)
{
    unsigned int i;
    memset(s, 0, 4u * sizeof(*s));
    for (i = 0; i < 2u; i++) {
        s[i].Width = columns;
        s[i].Height = height;
        s[i].Bpp = 4u;
        s[i].Pitch = 4u * columns;
        s[i].Bytes = (unsigned long long)s[i].Pitch * height;
        s[i].Type = BC250_GDISURFACE_TEXTURE;
        s[i].RedFirst = redFirst;
        s[i].Present = 1u;
    }
    layout->At[0] = DST_AT;
    layout->At[1] = SRC_AT;
    layout->At[2] = ALPHA_AT;
    layout->At[3] = GAMMA_AT;
}

/* What the DMA_DATA rows of an engine item do, applied to the arena: this is the engine's side of
 * the comparison, so the kernel the batch falls back to has to agree with it pixel for pixel. */
static void ApplyRows(const BC250_GDI_ITEM* item, const LAYOUT* layout, const BC250_GDI_OP* op,
                      unsigned int pattern)
{
    unsigned int row, i;
    for (row = 0; row < item->DstHeight; row++) {
        unsigned char* d = g_memory + layout->At[op->DstAllocationIndex] + item->DstOffset +
            (unsigned long long)row * item->DstPitch;
        if (item->Work == (unsigned int)Bc250GdiWorkFill) {
            for (i = 0; i < item->RowBytes / 4u; i++) Store32(d + 4u * i, pattern);
        } else {
            const unsigned char* src = g_memory + layout->At[op->SrcAllocationIndex] +
                item->SrcOffset + (unsigned long long)row * item->SrcPitch;
            memcpy(d, src, item->RowBytes);
        }
    }
}

/* Decode and run the dispatch at Buffer, which may be preceded by the code block the first time.
 * Returns the dwords consumed, or 0 on a decode failure. */
static unsigned int RunDispatchSpan(const unsigned int* buffer, unsigned int dwords,
                                    unsigned long long bufferVa, unsigned int kernel,
                                    unsigned long long* codeVa)
{
    DECODED decoded;
    unsigned int at = 0, code, dispatch, gx, gy, wx, wy, unrun = 0;
    unsigned char* dst;

    if (*codeVa == 0) {
        code = Bc250GdiCodeDwords(bufferVa);
        if (code == 0u || code > dwords) return 0;
        if (!DecodeCode(buffer, code, bufferVa, codeVa)) return 0;
        at = code;
    }
    dispatch = Bc250GdiDispatchDwords(bufferVa + (unsigned long long)at * 4u);
    if (dispatch == 0u || at + dispatch > dwords) return 0;
    if (!DecodeDispatch(buffer + at, dispatch, bufferVa + (unsigned long long)at * 4u, &decoded))
        return 0;
    /* The routing itself: the program the dispatch names must be the kernel of this operation. */
    if (decoded.ProgramVa != *codeVa + (unsigned long long)g_Bc250GdiCodeKernels[kernel].Offset * 4u)
        return 0;
    dst = ArenaAt(((unsigned long long)decoded.Block[BC250_GDI_ARG_DST + 1u] << 32) |
                  decoded.Block[BC250_GDI_ARG_DST], 4u);
    if (!dst) return 0;
    for (gy = 0; gy < decoded.Groups[1]; gy++)
        for (gx = 0; gx < decoded.Groups[0]; gx++)
            for (wy = 0; wy < BC250_GDI_GROUP_Y; wy++)
                for (wx = 0; wx < BC250_GDI_GROUP_X; wx++) {
                    const unsigned char* src = 0;
                    unsigned long long va;
                    g_Bc250GdiWorkgroupX = gx;
                    g_Bc250GdiWorkgroupY = gy;
                    g_Bc250GdiWorkitemX = wx;
                    g_Bc250GdiWorkitemY = wy;
                    va = ((unsigned long long)decoded.Block[BC250_GDI_ARG_SRC + 1u] << 32) |
                        decoded.Block[BC250_GDI_ARG_SRC];
                    if (va) src = ArenaAt(va, 4u);
                    if (!Bc250GdiRunKernel(kernel, dst, src, 0, 0,
                                           &decoded.Block[BC250_GDI_ARG_DSTPITCH]))
                        unrun++;
                }
    return unrun == 0u ? at + dispatch : 0u;
}

/* One batch: `columns` sub-rectangles of one column each, over a surface `height` rows tall, into a
 * DMA buffer of `capacity` dwords. Every item must be drawn, by whichever path the capacity allows,
 * and the arena must end up exactly as the reference model says. */
static void RunBatch(const char* what, unsigned int opcode, unsigned int rop, unsigned int colour,
                     unsigned int columns, unsigned int height, unsigned int capacity,
                     unsigned int expectFallbacks)
{
    BC250_GDI_SURFACE s[4];
    BC250_GDI_BATCH batch;
    LAYOUT layout;
    BC250_GDI_OP op;
    /* Static, because a dense call has thousands of sub-rectangles and these are per-item records,
     * not per-pixel ones. */
    static BC250_GDI_ITEM items[BATCH_SUBRECTS];
    static unsigned int results[BATCH_SUBRECTS], starts[BATCH_SUBRECTS], ends[BATCH_SUBRECTS];
    unsigned long long vas[4], codeVa = 0, ib = IB_VA;
    unsigned int i, used, pattern = 0;

    if (columns > BATCH_SUBRECTS) { CHECK(0); return; }
    TallScene(s, &layout, columns, height, 0);
    for (i = 0; i < 4u; i++) vas[i] = ARENA_VA + layout.At[i];
    op = Op(opcode, Rect(0, 0, (int)columns, (int)height), Rect(0, 0, (int)columns, (int)height));
    op.Rop = rop;
    op.Color = colour;
    op.NumSubRects = columns;
    if (opcode == BC250_GDIOP_COLORFILL) pattern = colour;

    SeedMemory();
    /* The reference: every column, through the kernel model, which is independent of the path the
     * batch will choose. */
    for (i = 0; i < columns; i++) {
        BC250_GDI_RECT sub = Rect((int)i, 0, (int)i + 1, (int)height);
        BC250_GDI_ITEM ref;
        CHECK_EQ(Bc250GdiPlanItem(&op, s, 4, &sub, &ref), Bc250GdiOk);
        items[i] = ref;
        CHECK(Bc250GdiItemToKernel(&op, &ref) == 1);
        ReferenceKernel(&op, s, &ref, &layout);
    }

    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, capacity, ib, columns), 1);
    for (i = 0; i < columns; i++) {
        /* The first item pays for the cache operation, so its own packets start behind it. */
        starts[i] = batch.Used == 0u ? BC250_GFX_ACQUIRE_DWORDS : batch.Used;
        results[i] = (unsigned int)Bc250GdiBatchItem(&batch, &op, s, vas, 4, &items[i], pattern,
                                                     columns - i);
        ends[i] = batch.Used;
        /* The contract: after Begin said yes, no admitted sub-rectangle may be dropped. */
        CHECK(results[i] == BC250_GDI_BATCH_ROWS || results[i] == BC250_GDI_BATCH_DISPATCH);
        if (results[i] != BC250_GDI_BATCH_ROWS && results[i] != BC250_GDI_BATCH_DISPATCH) return;
        CHECK(ends[i] > starts[i]);
    }
    used = Bc250GdiBatchEnd(&batch);
    CHECK(used <= capacity);
    CHECK_EQ(used % BC250_GDI_RING_GRANULE, 0u);
    CHECK_EQ(batch.Items, columns);
    CHECK_EQ(batch.Fallbacks, expectFallbacks);

    /* The cache operation is first, and it is the production one. */
    CHECK_EQ(Bc250EmitGfxAcquire(g_scratch, capacity), BC250_GFX_ACQUIRE_DWORDS);
    CHECK_EQ(memcmp(g_batch, g_scratch, BC250_GFX_ACQUIRE_DWORDS * 4u), 0);

    /* Each item's span, in order: either the production rows, byte for byte, or a dispatch of the
     * kernel of this operation, which is decoded and run. */
    for (i = 0; i < columns; i++) {
        unsigned int span = ends[i] - starts[i];
        unsigned int start = starts[i];
        if (results[i] == BC250_GDI_BATCH_ROWS) {
            unsigned int rows = 0, written = 0;
            unsigned long long source = Bc250GdiOpHasSource(op.OpCode) ? vas[1] : 0;
            CHECK_EQ(Bc250EmitGdiItem(&items[i], source, vas[0], pattern, 0u, &rows, g_scratch,
                                      BATCH_DWORDS, &written), BC250_GDI_EMIT_DONE);
            CHECK_EQ(written, span);
            if (written == span) CHECK_EQ(memcmp(g_batch + start, g_scratch, span * 4u), 0);
            ApplyRows(&items[i], &layout, &op, pattern);
        } else {
            BC250_GDI_ITEM asKernel = items[i];
            CHECK_EQ(Bc250GdiItemToKernel(&op, &asKernel), 1);
            CHECK_EQ(RunDispatchSpan(g_batch + start, span,
                                     ib + (unsigned long long)start * 4u, asKernel.Kernel,
                                     &codeVa), span);
        }
    }

    if (memcmp(g_memory, g_expected, MEM_BYTES) != 0) {
        unsigned int off;
        for (off = 0; off < MEM_BYTES; off++)
            if (g_memory[off] != g_expected[off]) break;
        fprintf(stderr, "FAIL %s: first difference at 0x%X (got 0x%02X, want 0x%02X)\n",
                what, off, g_memory[off], g_expected[off]);
        g_failures++;
    }
    g_checks++;
}

static void TestBatchCapacity(void)
{
    BC250_GDI_SURFACE s[4];
    BC250_GDI_BATCH batch;
    LAYOUT layout;
    unsigned int i;

    /* The arithmetic of the budget, and the two bounds it is built from. */
    CHECK(Bc250GdiDispatchDwordsMax() >= Bc250GdiDispatchDwords(IB_VA));
    CHECK(Bc250GdiDispatchDwordsMax() >= Bc250GdiDispatchDwords(IB_VA + 4u));
    CHECK(Bc250GdiCodeDwordsMax() >= Bc250GdiCodeDwords(IB_VA));
    CHECK(Bc250GdiCodeDwordsMax() >= Bc250GdiCodeDwords(IB_VA + 4u));
    CHECK_EQ(Bc250GdiBatchDwords(0), BC250_GFX_ACQUIRE_DWORDS + Bc250GdiCodeDwordsMax() +
             BC250_GDI_RING_GRANULE - 1u);
    CHECK_EQ(Bc250GdiBatchDwords(1u) - Bc250GdiBatchDwords(0), Bc250GdiDispatchDwordsMax());
    /* A count that cannot be budgeted at all refuses the call rather than wrapping. */
    CHECK_EQ(Bc250GdiBatchDwords(0xFFFFFFFFu), 0u);

    /* The producer's input bound: a command buffer of L bytes carries at most (L - 8) / 16
     * sub-rectangles, because the array of a command lies inside that command. The floor is the
     * command header, because the array may overlap the fixed part behind it. */
    CHECK_EQ(Bc250GdiMaxSubRects(0), 0u);
    CHECK_EQ(Bc250GdiMaxSubRects(BC250_GDI_RECT_FLOOR_BYTES), 0u);
    CHECK_EQ(Bc250GdiMaxSubRects(BC250_GDI_RECT_FLOOR_BYTES + BC250_GDI_SUBRECT_BYTES - 1u), 0u);
    CHECK_EQ(Bc250GdiMaxSubRects(BC250_GDI_RECT_FLOOR_BYTES + BC250_GDI_SUBRECT_BYTES), 1u);
    /* The smallest command that can carry one is still a ColorFill of 48 bytes, and the bound
     * gives it two, which is what fits when its array starts at the header. */
    CHECK_EQ(Bc250GdiMaxSubRects(BC250_GDI_MIN_COMMAND_BYTES), 2u);
    CHECK_EQ(Bc250GdiMaxSubRects(BC250_GDI_CMDBUF_FLOOR), BATCH_SUBRECTS);
    CHECK_EQ(Bc250GdiMaxSubRects(BC250_GDI_CMDBUF_BOUND), BOUND_SUBRECTS);
    CHECK_EQ(Bc250GdiCallDwords(BC250_GDI_CMDBUF_BOUND), Bc250GdiBatchDwords(BOUND_SUBRECTS));

    /* And the size that follows from it: a whole number of pages, enough for that worst case, and
     * no larger than the test's own buffer, which is what the dense cases below translate into. */
    {
        unsigned int dma = Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_BOUND);
        CHECK(dma != 0u);
        CHECK_EQ(dma & 4095u, 0u);
        CHECK(Bc250GdiCallDwords(BC250_GDI_CMDBUF_BOUND) <= dma / 4u);
        /* The bound is the largest candidate on the ladder that both ceilings allow, so its worst
         * case fits them and the next candidate's does not. That is what makes the number derived
         * rather than chosen. Which ceiling stops it is asserted in TestIbBound. */
        CHECK(dma <= BC250_GDI_DMA_BYTES_MAX);
        CHECK(Bc250GdiCallDwords(BC250_GDI_CMDBUF_BOUND) <= BC250_GDI_IB_DWORDS_MAX);
        CHECK_EQ(Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_BOUND * 2u), 0u);
        /* And the floor always fits, whatever the ceiling is. */
        CHECK(Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_FLOOR) != 0u);
        CHECK(Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_FLOOR) <= BATCH_BYTES);

        /* The bound a context is created with: the whole bound where the aperture can afford an
         * eighth of it, stepped down where it cannot, the floor where the aperture is unknown.
         * The apertures below are the ones this driver accepts (256 MiB minimum, 384 default,
         * 447 maximum) plus two it does not, because the function is pure arithmetic and the
         * caller's value comes from a capture it does not control. */
        CHECK_EQ(Bc250GdiContextBound(0ull, 0u), BC250_GDI_CMDBUF_FLOOR);
        CHECK_EQ(Bc250GdiContextBound(256ull * 1024u * 1024u, 0u), BC250_GDI_CMDBUF_BOUND);
        CHECK_EQ(Bc250GdiContextBound(384ull * 1024u * 1024u, 0u), BC250_GDI_CMDBUF_BOUND);
        CHECK_EQ(Bc250GdiContextBound(447ull * 1024u * 1024u, 0u), BC250_GDI_CMDBUF_BOUND);
        /* The step down, at the apertures where the share is the binding number. 32 MiB allows
         * 4 MiB of DMA buffer, which the whole bound fits inside; 16 MiB allows 2 MiB, which it
         * does not, and the ladder has nothing between the bound and the floor. */
        CHECK_EQ(Bc250GdiContextBound(32ull * 1024u * 1024u, 0u), BC250_GDI_CMDBUF_BOUND);
        CHECK_EQ(Bc250GdiContextBound(16ull * 1024u * 1024u, 0u), BC250_GDI_CMDBUF_FLOOR);
        /* And an aperture too small for even the floor still gets the floor: a GDI context with no
         * DMA buffer at all would draw nothing ever, which is worse than a large share. */
        CHECK_EQ(Bc250GdiContextBound(1ull * 1024u * 1024u, 0u), BC250_GDI_CMDBUF_FLOOR);
        /* What an earlier short call asked for is taken when it is larger than the bound and both
         * ceilings can still meet it, and ignored when neither can. The range where that happens is
         * narrow on purpose: one indirect buffer carries the worst case of 200 KiB and not of
         * 256 KiB, whatever the aperture is. */
        CHECK_EQ(Bc250GdiContextBound(447ull * 1024u * 1024u, 200u * 1024u), 200u * 1024u);
        CHECK_EQ(Bc250GdiContextBound(447ull * 1024u * 1024u, 256u * 1024u),
                 BC250_GDI_CMDBUF_BOUND);
        CHECK_EQ(Bc250GdiContextBound(447ull * 1024u * 1024u, 4096u * 1024u),
                 BC250_GDI_CMDBUF_BOUND);
        CHECK_EQ(Bc250GdiContextBound(447ull * 1024u * 1024u, 0xFFFFFFFFu),
                 BC250_GDI_CMDBUF_BOUND);
        /* A wanted below the bound changes nothing, and the answer never falls as the aperture
         * grows. The step is one MiB, which is the unit the aperture setting itself takes. */
        CHECK_EQ(Bc250GdiContextBound(384ull * 1024u * 1024u, 4096u), BC250_GDI_CMDBUF_BOUND);
        {
            unsigned int previous = 0;
            unsigned long long aperture;
            for (aperture = 0; aperture <= 512ull * 1024u * 1024u;
                 aperture += 1024u * 1024u) {
                unsigned int chosen = Bc250GdiContextBound(aperture, 0u);
                unsigned int need = Bc250GdiContextDmaBytes(chosen);
                CHECK(chosen >= BC250_GDI_CMDBUF_FLOOR && chosen <= BC250_GDI_CMDBUF_BOUND);
                CHECK(need != 0u && need <= BC250_GDI_DMA_BYTES_MAX);
                /* The share is kept wherever the floor itself leaves room for it. */
                if (chosen != BC250_GDI_CMDBUF_FLOOR)
                    CHECK((unsigned long long)need <= aperture / BC250_GDI_DMA_APERTURE_SHARE);
                if (aperture != 0) CHECK(chosen >= previous);
                previous = chosen;
            }
        }
        /* A bound no DMA buffer may be asked for is refused, not rounded down. */
        CHECK_EQ(Bc250GdiContextDmaBytes(0xFFFFFFFFu), 0u);
        CHECK(Bc250GdiContextDmaBytes(BC250_GDI_DMA_BYTES_MAX) == 0u ||
              Bc250GdiContextDmaBytes(BC250_GDI_DMA_BYTES_MAX) <= BC250_GDI_DMA_BYTES_MAX);

        /* The theorem: with that capacity, Begin accepts every command buffer up to the bound,
         * whatever the producer put in it. One step of the loop is one possible CommandLength. */
        for (i = 0; i <= BC250_GDI_CMDBUF_BOUND; i += BC250_GDI_SUBRECT_BYTES) {
            /* Begin writes nothing, so the arena pointer is not dereferenced here; what is being
             * checked is the arithmetic of the whole-call budget against the capacity. */
            if (!Bc250GdiBatchBegin(&batch, g_batch, dma / 4u, IB_VA,
                                    Bc250GdiMaxSubRects(i))) {
                CHECK_EQ(i, 0u);                   /* which must never happen; i names the case */
                break;
            }
        }
        g_checks++;                                 /* the loop above is one assertion */
        /* The review's own case: one ColorFill with 793 disjoint one-pixel sub-rectangles, which
         * the old 256 KiB buffer refused whole. Its command buffer is 12736 bytes, well inside the
         * bound, and nothing about it is special any more. */
        /* The bound is an upper bound of what a command buffer of that size can carry, so it gives
         * this one two more than it holds: the floor is the eight-byte header, and this command's
         * own array starts behind a fixed part of 48 bytes. */
        CHECK_EQ(Bc250GdiMaxSubRects(48u + 793u * 16u), 795u);
        CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, dma / 4u, IB_VA, 793u), 1);
        CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, dma / 4u, IB_VA, BATCH_SUBRECTS), 1);
    }

    /* Begin refuses a batch it cannot guarantee, and writes nothing. */
    TallScene(s, &layout, 2u, 16u, 0);
    memset(g_batch, 0xA5, sizeof(g_batch));
    memset(g_scratch, 0xA5, sizeof(g_scratch));
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, BATCH_DWORDS,
                                IB_VA, 0xFFFFFFFFu), 0);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, BATCH_DWORDS, IB_VA,
                                BATCH_CAP_DWORDS / Bc250GdiDispatchDwordsMax()), 0);
    CHECK_EQ(memcmp(g_batch, g_scratch, sizeof(g_batch)), 0);
    /* The largest batch a given capacity guarantees, and one above it. This is the refusal the
     * review objected to, and it is still here, one sub-rectangle above whatever the buffer can
     * hold - but now the buffer is sized so that no command buffer inside the bound can reach it,
     * which the theorem above asserts. */
    for (i = BATCH_CAP_DWORDS / Bc250GdiDispatchDwordsMax(); i > 0u; i--)
        if (Bc250GdiBatchDwords(i) <= BATCH_CAP_DWORDS) break;
    CHECK(i > BATCH_SUBRECTS);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, BATCH_DWORDS, IB_VA, i), 1);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, BATCH_DWORDS, IB_VA, i + 1u), 0);
    CHECK_EQ(memcmp(g_batch, g_scratch, sizeof(g_batch)), 0);
    /* A buffer address that is not dword aligned, and no buffer at all. */
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, BATCH_DWORDS, IB_VA + 1u, 1u), 0);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, 0, BATCH_DWORDS, IB_VA, 1u), 0);

    /* The counterexample of the review, in the buffer size the driver asks for: two columns of
     * 8192 rows. The first takes 57344 dwords of rows, which leaves the second nowhere near enough
     * for its own, so it is dispatched instead of being lost. */
    RunBatch("colorfill 2x8192, the lost column", BC250_GDIOP_COLORFILL, BC250_GDI_CF_PATCOPY,
             0x80C03412u, 2u, 8192u, BATCH_TIGHT_DWORDS, 1u);
    /* The same two columns in a buffer a granule short of what the engine path for the first and
     * the bounded path for the second would need together. This is where rule 1 alone is not
     * enough: the first column must give up its rows although they would fit, because the second
     * column's dispatch has to fit as well. Without the reserve of rule 2 the first column takes
     * the rows and the second has nowhere to go, which is the defect itself. */
    {
        unsigned int rowCost = Bc250GdiFillDwords(4u) * 8192u;
        unsigned long long behindRows = IB_VA +
            (unsigned long long)(BC250_GFX_ACQUIRE_DWORDS + rowCost) * 4u;
        unsigned int bounded = Bc250GdiCodeDwords(behindRows);
        unsigned int tight;
        bounded += Bc250GdiDispatchDwords(behindRows + (unsigned long long)bounded * 4u);
        tight = BC250_GFX_ACQUIRE_DWORDS + rowCost + bounded - 1u;
        CHECK(Bc250GdiBatchDwords(2u) <= tight);
        RunBatch("colorfill 2x8192, one dword short", BC250_GDIOP_COLORFILL, BC250_GDI_CF_PATCOPY,
                 0x80C03412u, 2u, 8192u, tight, 2u);
    }
    /* The review's positive control: at 512 rows both columns fit the engine path. */
    RunBatch("colorfill 2x512, both on the engine", BC250_GDIOP_COLORFILL, BC250_GDI_CF_PATCOPY,
             0x80C03412u, 2u, 512u, BATCH_DWORDS, 0u);
    /* The same for a copy and for a 1:1 stretch, which are the other two engine items. */
    RunBatch("bitblt srccopy 2x8192", BC250_GDIOP_BITBLT, BC250_GDI_ROP_SRCCOPY, 0,
             2u, 8192u, BATCH_TIGHT_DWORDS, 1u);
    RunBatch("bitblt srccopy 2x512", BC250_GDIOP_BITBLT, BC250_GDI_ROP_SRCCOPY, 0,
             2u, 512u, BATCH_DWORDS, 0u);
    RunBatch("stretchblt 1:1 2x8192", BC250_GDIOP_STRETCHBLT, 0, 0, 2u, 8192u,
             BATCH_TIGHT_DWORDS, 1u);
    /* Four columns of 4096 rows: the first two fit the engine path and the rest do not, so a batch
     * mixes the two paths and still draws everything. */
    RunBatch("colorfill 4x4096, mixed paths", BC250_GDIOP_COLORFILL, BC250_GDI_CF_PATCOPY,
             0x2011AA33u, 4u, 4096u, BATCH_TIGHT_DWORDS, 2u);
    /* A kernel operation needs no fallback and no engine path at all. */
    RunBatch("bitblt srcand 2x8192, kernels only", BC250_GDIOP_BITBLT, BC250_GDI_ROP_SRCAND, 0,
             2u, 8192u, BATCH_DWORDS, 0u);

    /* The review's case, translated and run rather than merely budgeted: 793 disjoint one-pixel
     * sub-rectangles of one ColorFill, every one of them drawn. Then the densest call the bound
     * admits, which is the worst case the DMA buffer was sized for. */
    {
        unsigned int dwords = Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_FLOOR) / 4u;
        RunBatch("colorfill 793 one-pixel subrects", BC250_GDIOP_COLORFILL, BC250_GDI_CF_PATCOPY,
                 0x80C03412u, 793u, 1u, dwords, 0u);
        RunBatch("colorfill the densest call of the floor", BC250_GDIOP_COLORFILL,
                 BC250_GDI_CF_PATCOPY, 0x2011AA33u, BATCH_SUBRECTS, 1u, dwords, 0u);
        RunBatch("bitblt srccopy 793 one-pixel subrects", BC250_GDIOP_BITBLT,
                 BC250_GDI_ROP_SRCCOPY, 0, 793u, 1u, dwords, 0u);
    }
}

/* ================================================================================================
 * rule 3: what one indirect buffer can carry, and the three cases of the r3 review
 * ============================================================================================== */

/* One call of `count` disjoint sub-rectangles of `width` by `height`, out of a command buffer of
 * `commandBytes` bytes, into a capacity of `capacity` dwords. Nothing is decoded and nothing is
 * run, so the surfaces are described rather than placed in the arena and the addresses are outside
 * it: what is asserted is the fate of every item and the length of the finished stream. */
static void IbBoundCase(const char* what, unsigned int opcode, unsigned int rop,
                        unsigned int colour, unsigned int commandBytes, unsigned int count,
                        unsigned int width, unsigned int height, unsigned int capacity,
                        int expectBegin)
{
    BC250_GDI_SURFACE s[4];
    BC250_GDI_BATCH batch;
    BC250_GDI_OP op;
    BC250_GDI_RECT whole;
    unsigned long long vas[4];
    unsigned int i, used, planned, drawn = 0, pattern = 0;

    memset(s, 0, sizeof(s));
    for (i = 0; i < 4u; i++) {
        s[i].Width = count * width;
        s[i].Height = height;
        s[i].Bpp = 4u;
        s[i].Pitch = 4u * count * width;
        s[i].Bytes = (unsigned long long)s[i].Pitch * height;
        s[i].Type = BC250_GDISURFACE_TEXTURE;
        s[i].Present = 1u;
        /* Far from the arena and far from the indirect buffer: an address nothing here reads. */
        vas[i] = 0x0000000A00000000ull + (unsigned long long)i * 0x10000000ull;
    }
    whole = Rect(0, 0, (int)(count * width), (int)height);
    op = Op(opcode, whole, whole);
    op.Rop = rop;
    op.Color = colour;
    op.NumSubRects = count;
    if (opcode == BC250_GDIOP_COLORFILL) pattern = colour;

    /* What the walk would promise Begin for a command buffer of that size, which is the number the
     * production wrapper passes and is at least the rectangles this command holds. */
    planned = Bc250GdiMaxSubRects(commandBytes);
    CHECK(planned >= count);
    if (!expectBegin) {
        /* Above the bound: the counted refusal of rule 2, and nothing written. */
        CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, capacity, IB_VA, planned), 0);
        return;
    }
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, capacity, IB_VA, planned), 1);
    for (i = 0; i < count; i++) {
        BC250_GDI_RECT sub = Rect((int)(i * width), 0, (int)((i + 1u) * width), (int)height);
        BC250_GDI_ITEM item;
        int result;
        CHECK_EQ(Bc250GdiPlanItem(&op, s, 4, &sub, &item), Bc250GdiOk);
        result = Bc250GdiBatchItem(&batch, &op, s, vas, 4, &item, pattern, planned - i);
        /* The contract: after Begin said yes, every admitted sub-rectangle is drawn by one path or
         * the other. A row path that ran out of budget takes the dispatch, it does not drop. */
        CHECK(result == BC250_GDI_BATCH_ROWS || result == BC250_GDI_BATCH_DISPATCH);
        if (result == BC250_GDI_BATCH_ROWS || result == BC250_GDI_BATCH_DISPATCH) drawn++;
    }
    used = Bc250GdiBatchEnd(&batch);
    CHECK_EQ(drawn, count);
    CHECK_EQ(batch.Items, count);
    CHECK(used <= capacity);
    /* The end of it: the stream the submitter would encode, against the predicate the ring emitter
     * applies to every submission (bc250_gfx_emit_ib, with this macro). */
    CHECK(used <= BC250_GDI_IB_DWORDS_MAX);
    CHECK_EQ((unsigned int)PACKET3_INDIRECT_BUFFER__IB_SIZE(used), used);
    if (used == 0u || used > BC250_GDI_IB_DWORDS_MAX)
        fprintf(stderr, "FAIL %s: %u dwords of a capacity of %u\n", what, used, capacity);
}

static void TestIbBound(void)
{
    BC250_GDI_BATCH batch;
    unsigned int dma = Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_BOUND);
    unsigned int shipped = Bc250GdiCallCapacity(dma / 4u, 0u);
    unsigned int ceiling = Bc250GdiCallCapacity(BC250_GDI_DMA_BYTES_MAX / 4u, 0u);

    /* The constant itself is held against the packet field by the STATIC_CHECKs at the top of this
     * file, which is where a constant expression belongs. What is left for run time is the
     * arithmetic that uses it. */

    /* The capacity of one call: the space it was given, what one indirect buffer can carry, and
     * that length less the dwords the same buffer already holds. */
    CHECK_EQ(Bc250GdiCallCapacity(1024u, 0u), 1024u);
    CHECK_EQ(ceiling, BC250_GDI_IB_DWORDS_MAX);
    CHECK_EQ(Bc250GdiCallCapacity(BC250_GDI_DMA_BYTES_MAX / 4u, 1048000u),
             BC250_GDI_IB_DWORDS_MAX - 1048000u);
    CHECK_EQ(Bc250GdiCallCapacity(1024u, BC250_GDI_IB_DWORDS_MAX - 16u), 16u);
    CHECK_EQ(Bc250GdiCallCapacity(1024u, BC250_GDI_IB_DWORDS_MAX), 0u);
    CHECK_EQ(Bc250GdiCallCapacity(1024u, 0xFFFFFFFFu), 0u);
    CHECK_EQ(Bc250GdiCallCapacity(0u, 0u), 0u);

    /* And the same clamp inside Begin, for a caller that hands over a whole DMA buffer without
     * rule 3's arithmetic: the batch works inside the encodable length, so the row path cannot be
     * given space the submitter is unable to name. This is the review's case 1 at its root. */
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, 0xFFFFFFFFu, IB_VA, 1u), 1);
    CHECK_EQ(batch.Capacity, BC250_GDI_IB_DWORDS_MAX);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, BC250_GDI_DMA_BYTES_MAX / 4u, IB_VA, 1u), 1);
    CHECK_EQ(batch.Capacity, BC250_GDI_IB_DWORDS_MAX);

    /* Which ceiling chooses the bound: the next candidate on the ladder is refused because no
     * single indirect buffer can carry its worst case, and its DMA buffer would have fitted the
     * policy ceiling with room to spare. */
    CHECK(dma != 0u);
    CHECK(shipped != 0u && shipped <= BC250_GDI_IB_DWORDS_MAX);
    CHECK(Bc250GdiCallDwords(BC250_GDI_CMDBUF_BOUND) <= BC250_GDI_IB_DWORDS_MAX);
    CHECK(Bc250GdiCallDwords(BC250_GDI_CMDBUF_BOUND * 2u) > BC250_GDI_IB_DWORDS_MAX);
    CHECK(Bc250GdiCallDwords(BC250_GDI_CMDBUF_BOUND * 2u) * 4u < BC250_GDI_DMA_BYTES_MAX);
    CHECK_EQ(Bc250GdiContextDmaBytes(BC250_GDI_CMDBUF_BOUND * 2u), 0u);

    /* Case 1 of the review. One ColorFill of 128 separate 1 by 1200 strips is 2096 command bytes,
     * and every strip is a row item of 8400 dwords: offered the whole of a large DMA buffer, the
     * batch emitted 1075208 dwords, which the submitter cannot encode and does not truncate. The
     * capacity is what stops the row path now, and every strip is still drawn. Twice: with the
     * capacity a context of this bound has, and with the whole policy ceiling offered, which is
     * what the reproducer did. */
    IbBoundCase("colorfill 128 strips of 1x1200, the shipped capacity", BC250_GDIOP_COLORFILL,
                BC250_GDI_CF_PATCOPY, 0x80C03412u, 2096u, 128u, 1u, 1200u, shipped, 1);
    IbBoundCase("colorfill 128 strips of 1x1200, the whole ceiling", BC250_GDIOP_COLORFILL,
                BC250_GDI_CF_PATCOPY, 0x80C03412u, 2096u, 128u, 1u, 1200u, ceiling, 1);
    /* The positive control of the same shape at a quarter of the height: 64 strips of 1 by 300
     * cost 134400 dwords together, so the clamp is nowhere near them and the call is translated
     * the way it was before this change. */
    IbBoundCase("colorfill 64 strips of 1x300, well inside the clamp", BC250_GDIOP_COLORFILL,
                BC250_GDI_CF_PATCOPY, 0x2011AA33u, 1072u, 64u, 1u, 300u, shipped, 1);

    /* Case 2 of the review. A BitBlt of 14000 separate one-pixel rectangles is 224080 command
     * bytes, above this driver's bound, and its worst case is 1135366 dwords, which no single
     * indirect buffer can carry at any DMA size. So the whole of its behaviour is the counted
     * refusal of rule 2, at both capacities. */
    CHECK(Bc250GdiCallDwords(224080u) > BC250_GDI_IB_DWORDS_MAX);
    IbBoundCase("bitblt 14000 one-pixel rects, the shipped capacity", BC250_GDIOP_BITBLT,
                BC250_GDI_ROP_SRCINVERT, 0, 224080u, 14000u, 1u, 1u, shipped, 0);
    IbBoundCase("bitblt 14000 one-pixel rects, the whole ceiling", BC250_GDIOP_BITBLT,
                BC250_GDI_ROP_SRCINVERT, 0, 224080u, 14000u, 1u, 1u, ceiling, 0);

    /* Case 3 of the review: 98313 rectangles in 1573088 command bytes, which this design refused
     * before the change as well. The refusal is the same, and now it is the encodable length and
     * not a policy number that makes it the right one. */
    CHECK(Bc250GdiMaxSubRects(1573088u) >= 98313u);
    CHECK(Bc250GdiBatchDwords(98313u) > BC250_GDI_IB_DWORDS_MAX);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, ceiling, IB_VA, 98313u), 0);
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, shipped, IB_VA, 98313u), 0);

    /* The densest call the bound does guarantee, at the capacity it is sized for: 12287
     * sub-rectangles of one dispatch each, which is the worst case of the whole design. */
    CHECK_EQ(Bc250GdiBatchBegin(&batch, g_batch, shipped, IB_VA, BOUND_SUBRECTS), 1);
    CHECK(Bc250GdiBatchDwords(BOUND_SUBRECTS) <= shipped);
}

/* ================================================================================================
 * rule 3: which DMA buffer a call lies in, and what lies ahead of it
 * ============================================================================================== */

/* One call of `count` one-pixel BitBlt sub-rectangles into `capacity` dwords at the window address
 * `windowVa`, which is what a claim answered with. Returns the dwords the batch wrote. The
 * surfaces are described and never read, like IbBoundCase above. */
static unsigned int DmaCallDwords(unsigned long long windowVa, unsigned int capacity,
                                  unsigned int count)
{
    BC250_GDI_SURFACE s[4];
    BC250_GDI_BATCH batch;
    BC250_GDI_OP op;
    BC250_GDI_RECT whole;
    unsigned long long vas[4];
    unsigned int i;

    memset(s, 0, sizeof(s));
    for (i = 0; i < 4u; i++) {
        s[i].Width = count;
        s[i].Height = 1u;
        s[i].Bpp = 4u;
        s[i].Pitch = 4u * count;
        s[i].Bytes = (unsigned long long)s[i].Pitch;
        s[i].Type = BC250_GDISURFACE_TEXTURE;
        s[i].Present = 1u;
        vas[i] = 0x0000000B00000000ull + (unsigned long long)i * 0x10000000ull;
    }
    whole = Rect(0, 0, (int)count, 1);
    op = Op(BC250_GDIOP_BITBLT, whole, whole);
    op.Rop = BC250_GDI_ROP_SRCCOPY;
    op.NumSubRects = count;
    if (!Bc250GdiBatchBegin(&batch, g_batch, capacity, windowVa, count)) return 0u;
    for (i = 0; i < count; i++) {
        BC250_GDI_RECT sub = Rect((int)i, 0, (int)i + 1, 1);
        BC250_GDI_ITEM item;
        int result;
        CHECK_EQ(Bc250GdiPlanItem(&op, s, 4, &sub, &item), Bc250GdiOk);
        result = Bc250GdiBatchItem(&batch, &op, s, vas, 4, &item, 0u, count - i);
        CHECK(result == BC250_GDI_BATCH_ROWS || result == BC250_GDI_BATCH_DISPATCH);
    }
    return Bc250GdiBatchEnd(&batch);
}

/* The tracker of rule 3: which DMA buffer a call lies in, and how many dwords of ours are ahead of
 * it. Review r4 showed that the earlier answer - the lowest address seen since the last submission
 * of the context - is not an identity: two independent buffers of one context made the second call
 * believe two million dwords lay ahead of it, and a submission of one buffer cleared the state of
 * the other. The cases below are the ones that review asked for, and the two refusals are what a
 * window nobody can place now gets instead of an assumption. */
static void TestDmaMap(void)
{
    const unsigned long long a = 0x0000000900000000ull;     /* page aligned, as a DMA buffer is */
    const unsigned long long b = a + 8ull * 1024ull * 1024ull;
    BC250_GDI_DMA_MAP map;
    unsigned int ahead = 0xFFFFFFFFu, used, second, i;

    /* An independent buffer is not a continuation of another. This is the review's counterexample:
     * each buffer carries the same small paint, no submission between them, and the second call
     * must see an empty buffer and not the distance between two allocations. */
    memset(&map, 0, sizeof(map));
    CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, &ahead), BC250_GDI_DMA_FRESH);
    CHECK_EQ(ahead, 0u);
    used = DmaCallDwords(a, 4096u, 1u);
    CHECK(used != 0u);
    CHECK_EQ(Bc250GdiDmaWrote(&map, a, 0u, used), 1);
    CHECK_EQ(Bc250GdiDmaClaim(&map, b, 4096u, &ahead), BC250_GDI_DMA_FRESH);
    CHECK_EQ(ahead, 0u);
    second = DmaCallDwords(b, 4096u, 1u);
    CHECK_EQ(second, used);
    CHECK_EQ(Bc250GdiDmaWrote(&map, b, 0u, second), 1);
    /* Both buffers are tracked at once, and each knows its own length. */
    CHECK_EQ(Bc250GdiDmaClaim(&map, a + (unsigned long long)used * 4u, 4096u, &ahead),
             BC250_GDI_DMA_APPEND);
    CHECK_EQ(ahead, used);
    CHECK_EQ(Bc250GdiDmaClaim(&map, b + (unsigned long long)second * 4u, 4096u, &ahead),
             BC250_GDI_DMA_APPEND);
    CHECK_EQ(ahead, second);

    /* A submission of A releases A and leaves B exactly as it was. */
    CHECK_EQ(Bc250GdiDmaSubmitted(&map, a, used * 4u), 1u);
    CHECK_EQ(Bc250GdiDmaClaim(&map, b + (unsigned long long)second * 4u, 4096u, &ahead),
             BC250_GDI_DMA_APPEND);
    CHECK_EQ(ahead, second);
    /* And A is a fresh buffer again, with nothing of ours ahead of its next call. */
    CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, &ahead), BC250_GDI_DMA_FRESH);
    CHECK_EQ(ahead, 0u);
    /* A submission that names neither buffer releases nothing. */
    CHECK_EQ(Bc250GdiDmaSubmitted(&map, a + 0x40000000ull, 4096u), 0u);
    CHECK_EQ(Bc250GdiDmaClaim(&map, b + (unsigned long long)second * 4u, 4096u, &ahead),
             BC250_GDI_DMA_APPEND);
    CHECK_EQ(ahead, second);

    /* Appends inside one buffer, at the offsets the emitter actually leaves. Three calls in a row,
     * each one placed by the length of the ones before it, and the capacity of each taken from
     * rule 3: the sum stays inside what one indirect buffer can carry. */
    memset(&map, 0, sizeof(map));
    {
        unsigned int total = 0, call;
        for (call = 0; call < 3u; call++) {
            unsigned int capacity;
            CHECK_EQ(Bc250GdiDmaClaim(&map, a + (unsigned long long)total * 4u, 4096u, &ahead),
                     call == 0u ? BC250_GDI_DMA_FRESH : BC250_GDI_DMA_APPEND);
            CHECK_EQ(ahead, total);
            capacity = Bc250GdiCallCapacity(BATCH_CAP_DWORDS - total, ahead);
            used = DmaCallDwords(a + (unsigned long long)total * 4u, capacity, 8u);
            CHECK(used != 0u);
            CHECK(used <= capacity);
            CHECK_EQ(Bc250GdiDmaWrote(&map, a + (unsigned long long)total * 4u, ahead, used), 1);
            total += used;
            CHECK(total <= BC250_GDI_IB_DWORDS_MAX);
            CHECK_EQ((unsigned int)PACKET3_INDIRECT_BUFFER__IB_SIZE(total), total);
        }
        /* A buffer filled to the encodable length leaves the next call no room, which is the
         * ib-full drop and not an overflow of the packet. */
        memset(&map, 0, sizeof(map));
        CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, &ahead), BC250_GDI_DMA_FRESH);
        CHECK_EQ(Bc250GdiDmaWrote(&map, a, 0u, BC250_GDI_IB_DWORDS_MAX), 1);
        CHECK_EQ(Bc250GdiDmaClaim(&map, a + (unsigned long long)BC250_GDI_IB_DWORDS_MAX * 4u, 4096u,
                                  &ahead), BC250_GDI_DMA_APPEND);
        CHECK_EQ(ahead, BC250_GDI_IB_DWORDS_MAX);
        CHECK_EQ(Bc250GdiCallCapacity(4096u, ahead), 0u);
        /* And a record that would pass that length is not kept. */
        CHECK_EQ(Bc250GdiDmaWrote(&map, a + (unsigned long long)BC250_GDI_IB_DWORDS_MAX * 4u,
                                  BC250_GDI_IB_DWORDS_MAX, 8u), 0);
    }

    /* The same buffer offered again from its start is a fill that starts again, whether or not this
     * driver saw the submission that emptied it. */
    memset(&map, 0, sizeof(map));
    CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, &ahead), BC250_GDI_DMA_FRESH);
    CHECK_EQ(Bc250GdiDmaWrote(&map, a, 0u, 4096u), 1);
    CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, &ahead), BC250_GDI_DMA_RESTART);
    CHECK_EQ(ahead, 0u);
    /* And the length it had before the restart is gone: the window where its old output ended is
     * not an append any more, because that output is not in the buffer this driver is writing now.
     * A page boundary there would be the start of another buffer, so the window off the boundary is
     * the one this test can ask about. */
    CHECK_EQ(Bc250GdiDmaClaim(&map, a + 4096ull * 4u + 32u, 4096u, &ahead), BC250_GDI_DMA_UNKNOWN);

    /* A window nobody can place: inside output of ours, and off the page boundary the DDI gives the
     * start of a DMA buffer. Both drop the call instead of assuming the buffer is empty. */
    memset(&map, 0, sizeof(map));
    CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, &ahead), BC250_GDI_DMA_FRESH);
    CHECK_EQ(Bc250GdiDmaWrote(&map, a, 0u, 64u), 1);
    CHECK_EQ(Bc250GdiDmaClaim(&map, a + 32u, 4096u, &ahead), BC250_GDI_DMA_UNKNOWN);
    CHECK_EQ(ahead, 0u);
    CHECK_EQ(Bc250GdiDmaClaim(&map, b + 32u, 4096u, &ahead), BC250_GDI_DMA_UNKNOWN);
    CHECK_EQ(Bc250GdiDmaClaim(&map, 0ull, 4096u, &ahead), BC250_GDI_DMA_UNKNOWN);
    CHECK_EQ(Bc250GdiDmaClaim(NULL, a, 4096u, &ahead), BC250_GDI_DMA_UNKNOWN);
    CHECK_EQ(Bc250GdiDmaClaim(&map, a, 4096u, NULL), BC250_GDI_DMA_UNKNOWN);

    /* Every slot holding output of ours, and one more buffer: the call is dropped rather than a
     * buffer this driver has written into being forgotten. A slot that holds no output of ours is
     * free for another buffer, so a context that is merely offered many buffers never runs out. */
    memset(&map, 0, sizeof(map));
    for (i = 0; i < BC250_GDI_DMA_SLOTS; i++) {
        unsigned long long va = a + (unsigned long long)i * 0x10000ull;
        CHECK_EQ(Bc250GdiDmaClaim(&map, va, 4096u, &ahead), BC250_GDI_DMA_FRESH);
        CHECK_EQ(Bc250GdiDmaWrote(&map, va, 0u, 8u), 1);
    }
    CHECK_EQ(Bc250GdiDmaClaim(&map, b, 4096u, &ahead), BC250_GDI_DMA_FULL);
    CHECK_EQ(Bc250GdiDmaSubmitted(&map, a, 0x10000u * BC250_GDI_DMA_SLOTS), BC250_GDI_DMA_SLOTS);
    CHECK_EQ(Bc250GdiDmaClaim(&map, b, 4096u, &ahead), BC250_GDI_DMA_FRESH);
    memset(&map, 0, sizeof(map));
    for (i = 0; i < BC250_GDI_DMA_SLOTS + 4u; i++)
        CHECK_EQ(Bc250GdiDmaClaim(&map, a + (unsigned long long)i * 0x10000ull, 4096u, &ahead),
                 BC250_GDI_DMA_FRESH);
}

/* ================================================================================================
 * what the emitters refuse, and what the image says about itself
 * ============================================================================================== */

static void TestRefusals(void)
{
    static unsigned int buffer[8192];
    static const unsigned int clean[8192] = {0};
    BC250_GDI_SURFACE s[4];
    BC250_GDI_KERNARG arg;
    LAYOUT layout;
    BC250_GDI_ITEM item;
    BC250_GDI_OP op;
    BC250_GDI_RECT sub;
    unsigned long long vas[4], codeVa = 0;
    unsigned int i, code;

    Scene(s, &layout, 0, 0);
    for (i = 0; i < 4u; i++) vas[i] = ARENA_VA + layout.At[i];
    op = Op(BC250_GDIOP_BITBLT, Rect(3, 5, 67, 45), Rect(100, 60, 164, 100));
    op.Rop = BC250_GDI_ROP_SRCOR;
    sub = op.DstRect;
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 4, &sub, &item), Bc250GdiOk);
    CHECK_EQ(Bc250GdiKernarg(&op, s, vas, 4, &item, &arg), 1);

    /* An address that is not dword aligned, and one that would wrap. */
    CHECK_EQ(Bc250GdiCodeDwords(IB_VA + 1u), 0u);
    CHECK_EQ(Bc250GdiCodeDwords(0), 0u);
    CHECK_EQ(Bc250GdiCodeDwords(~0ull - 16u), 0u);
    CHECK_EQ(Bc250GdiDispatchDwords(IB_VA + 2u), 0u);

    /* Short capacity, by one dword, for each emitter, and nothing written. */
    memset(buffer, 0, sizeof(buffer));
    code = Bc250GdiCodeDwords(IB_VA);
    CHECK_EQ(Bc250EmitGdiCode(buffer, code - 1u, IB_VA, &codeVa), 0u);
    CHECK_EQ(codeVa, 0u);
    CHECK_EQ(memcmp(buffer, clean, sizeof(buffer)), 0);
    CHECK_EQ(Bc250EmitGdiCode(buffer, code, IB_VA, &codeVa), code);
    CHECK(codeVa != 0u);

    memset(buffer, 0, sizeof(buffer));
    CHECK_EQ(Bc250EmitGdiDispatch(item.Kernel, &arg, codeVa, buffer,
                                  Bc250GdiDispatchDwords(IB_VA) - 1u, IB_VA), 0u);
    CHECK_EQ(memcmp(buffer, clean, sizeof(buffer)), 0);
    /* A program address that is not on a BC250_GDI_CODE_ALIGN boundary cannot be named by
     * COMPUTE_PGM_LO at all. */
    CHECK_EQ(Bc250EmitGdiDispatch(item.Kernel, &arg, codeVa + 4u, buffer, 8192u, IB_VA), 0u);
    CHECK_EQ(Bc250EmitGdiDispatch(item.Kernel, &arg, 0, buffer, 8192u, IB_VA), 0u);
    /* An index outside the table, and an empty rectangle. */
    CHECK_EQ(Bc250EmitGdiDispatch(0, &arg, codeVa, buffer, 8192u, IB_VA), 0u);
    CHECK_EQ(Bc250EmitGdiDispatch((unsigned int)Bc250GdiKernels, &arg, codeVa, buffer, 8192u, IB_VA), 0u);
    {
        BC250_GDI_KERNARG empty = arg;
        empty.Width = 0u;
        CHECK_EQ(Bc250EmitGdiDispatch(item.Kernel, &empty, codeVa, buffer, 8192u, IB_VA), 0u);
        empty = arg;
        empty.Height = 0u;
        CHECK_EQ(Bc250EmitGdiDispatch(item.Kernel, &empty, codeVa, buffer, 8192u, IB_VA), 0u);
    }
    CHECK_EQ(memcmp(buffer, clean, sizeof(buffer)), 0);

    /* The argument block of an item that is not a kernel item, and of an absent allocation. */
    {
        BC250_GDI_OP copy = Op(BC250_GDIOP_BITBLT, Rect(3, 5, 67, 45), Rect(100, 60, 164, 100));
        BC250_GDI_RECT copySub = copy.DstRect;
        BC250_GDI_ITEM copyItem;
        copy.Rop = BC250_GDI_ROP_SRCCOPY;
        CHECK_EQ(Bc250GdiPlanItem(&copy, s, 4, &copySub, &copyItem), Bc250GdiOk);
        CHECK_EQ(copyItem.Work, (unsigned int)Bc250GdiWorkCopy);
        CHECK_EQ(Bc250GdiKernarg(&copy, s, vas, 4, &copyItem, &arg), 0);
    }
    {
        unsigned long long broken[4];
        for (i = 0; i < 4u; i++) broken[i] = vas[i];
        broken[1] = 0;
        CHECK_EQ(Bc250GdiKernarg(&op, s, broken, 4, &item, &arg), 0);
        CHECK_EQ(Bc250GdiKernarg(&op, s, vas, 1, &item, &arg), 0);
    }
}

static void TestImage(void)
{
    static const char* const names[] = BC250_GDI_CODE_NAME_LIST;
    unsigned int i;
    /* The image carries the six kernels, so the capability bit may be set. */
    CHECK_EQ(Bc250GdiKernelsPresent(), 1);
    CHECK_EQ(Bc250GdiCodeKernelCount(), BC250_GDI_KERNEL_COUNT);
    CHECK_EQ(Bc250GdiCodeBytes(), BC250_GDI_CODE_DWORDS * 4u);
    CHECK_EQ(strlen(Bc250GdiCodeHash()), 64u);
    /* The order of the table in gdi_kernel_run.c is the order of the enum, which is the order the
     * build read out of gdi_render.h. A kernel that moved fails here and not on the screen. */
    for (i = 1u; i <= BC250_GDI_KERNEL_COUNT; i++) {
        CHECK(names[i] != 0 && Bc250GdiRunKernelName(i) != 0);
        if (names[i] && Bc250GdiRunKernelName(i))
            CHECK_EQ(strcmp(names[i], Bc250GdiRunKernelName(i)), 0);
        /* Every slot of the generated table is a real slice of the code. */
        CHECK(g_Bc250GdiCodeKernels[i].Dwords != 0u);
        CHECK(g_Bc250GdiCodeKernels[i].Offset + g_Bc250GdiCodeKernels[i].Dwords <=
              BC250_GDI_CODE_DWORDS);
        /* A slice starts on a BC250_GDI_CODE_ALIGN boundary, which is what lets COMPUTE_PGM_LO
         * name it. */
        CHECK_EQ((g_Bc250GdiCodeKernels[i].Offset * 4u) % BC250_GDI_CODE_ALIGN, 0u);
    }
    CHECK_EQ(g_Bc250GdiCodeKernels[0].Dwords, 0u);
    CHECK_EQ(Bc250GdiRunKernelName(0), (const char*)0);
    CHECK_EQ(Bc250GdiRunKernelName(BC250_GDI_KERNEL_COUNT + 1u), (const char*)0);
}

int main(void)
{
    TestImage();
    TestRefusals();
    TestBitBltRops();
    TestColorFillRops();
    TestStretchBlt();
    TestAlphaBlend();
    TestTransparentBlt();
    TestClearTypeBlend();
    TestColourOrder();
    TestBatchCapacity();
    TestIbBound();
    TestDmaMap();
    printf("gdi_kernel_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
