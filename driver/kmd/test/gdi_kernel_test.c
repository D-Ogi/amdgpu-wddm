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
#include "../gdi_kernels.generated.h"
#include "../../amdgpu-import/nvd.h"
#include "gdi_kernel_run.h"

static int g_failures, g_checks;

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
                out = Bc250GdiColorFillPixel(op->Rop, Bc250GdiFromArgb(op->Color, redFirst), dst);
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
                out = Bc250GdiFromArgb(
                    Bc250GdiTransparentPixel(Bc250GdiToArgb(src, redFirst),
                                             Bc250GdiToArgb(dst, redFirst), op->Color,
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
                out = Bc250GdiFromArgb(
                    Bc250GdiClearTypePixel(Bc250GdiToArgb(dst, redFirst),
                                           Bc250GdiToArgb(a, alphaRedFirst),
                                           op->Color, op->Color2, item->GammaValid,
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
    printf("gdi_kernel_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
