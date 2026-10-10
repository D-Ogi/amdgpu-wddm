// dotprobe.hip - does gfx1013 silicon execute the GFX10.1 dot-product instructions that LLVM
// says it does not have?
//
// Milestone M16, route B (owner decisions D014, D015). The question is not what the compiler
// permits: that is already measured (evidence/m16/gfx1013-dp4a-2026-10-09, clang refuses
// __builtin_amdgcn_sdot4 for gfx1013 because the target has no dot1-insts feature). The question
// is what the hardware does when the instruction bytes reach it anyway.
//
// ONE kernel, ONE code object, ONE process. The kernel takes an opcode index and a uniform
// switch runs exactly one instruction per launch, so the instructions that are not selected are
// never decoded. The host launches the indexes in order, least risk first, and appends one line
// to the result log on disk, flushed, after every launch. If the device hangs, the last line in
// that log names the instruction that did it.
//
// Every instruction is written as inline assembly, never as a builtin, because the frontend
// refuses the builtins for this target. The assembly text is not assembled by this compilation:
// build.ps1 stops at -S and assembles the device text with llvm-mc, which takes
// -mattr=+dot1-insts and friends. See build.ps1 for the five stages and for the encoding
// comparison against gfx1012 and gfx1030.
//
// Operands are indexed by the lane, so no two lanes compute the same thing and a kernel that
// writes one constant cannot pass. Every lane's inputs come from the host, and the host checks
// every lane against its own reference, computed in integer or in exactly representable
// floating point so that the comparison needs no tolerance.
//
// Exit codes: 0 every instruction correct, 1 a HIP call failed, 2 usage, 3 at least one wrong
// result, 4 the named instruction is not in this build.

#include "hip/hip_runtime.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LANES 4096u
#define BLOCK 64u
#define GRID  (LANES / BLOCK)

// The opcode indexes. The order is the order of least risk: the three positive controls, then
// the two dot instructions that would change a backend decision, then the rest.
#define OP_PK_FMA_F16       0
#define OP_FMA_MIX_F32      1
#define OP_MUL_I32_I24_SDWA 2
#define OP_DOT4_I32_I8      3
#define OP_DOT2_F32_F16     4
#define OP_DOT4_U32_U8      5
#define OP_DOT4C_I32_I8     6
#define OP_DOT2C_F32_F16    7
#define OP_DOT2_I32_I16     8
#define OP_DOT2_U32_U16     9
#define OP_DOT8_I32_I4      10
#define OP_DOT8_U32_U4      11
// The VOP2 opcode-slot probe. On SI and CI, VOP2 0x00d and 0x00e held V_MIN_LEGACY_F32 and
// V_MAX_LEGACY_F32. On GFX10 both slots are empty in LLVM's table, which runs 0x00c, then jumps
// to 0x00f = V_MIN_F32 and 0x010 = V_MAX_F32; the two dot accumulate forms were layered on top
// of 0x00d and 0x002 by GFX10.1. The first run found that 0x00d writes something that is not a
// dot product, so these indexes ask the slots directly, with the two instructions the part does
// have as the yardstick.
#define OP_MIN_F32   12 /* v_min_f32, by mnemonic: VOP2 0x00f, gfx1013 has it */
#define OP_MAX_F32   13 /* v_max_f32, by mnemonic: VOP2 0x010, gfx1013 has it */
#define OP_RAW_0X0F  14 /* the same 0x00f word, hand-encoded: the raw route's control */
#define OP_RAW_0X10  15 /* the same 0x010 word, hand-encoded: the raw route's control */
#define OP_RAW_0X0D  16 /* the slot v_dot4c_i32_i8 occupies */
#define OP_RAW_0X02  17 /* the slot v_dot2c_f32_f16 occupies */
#define OP_RAW_0X0E  18 /* no GFX10 mnemonic at all: last, because it is the least known word */
#define OP_COUNT     19

// ----------------------------------------------------------------------------------------------
// The one kernel. Three 32-bit operands per lane in, one 32-bit result out, and exactly one
// instruction between them. `op` is a kernel argument, so it is uniform over the whole launch
// and the switch is a scalar branch: one case is reached, the others are not fetched.
//
// The inline assembly binds every operand to a VGPR ("v"), which is what these instructions
// take; a scalar operand would change the encoding and the probe would not test the opcode the
// name says. The VOP2 accumulate forms tie the destination to the third source, so their
// operand is "+v".
// ----------------------------------------------------------------------------------------------

// The destination register is loaded with a sentinel first, and the constraint is "+v", so the
// register is an input as well as the output. That distinguishes two outcomes an opcode the part
// may not have can give: a destination that comes back as the sentinel was not written at all,
// and a destination that comes back as something else was written. Without the sentinel a
// write-only "=v" destination holds an undefined value when the instruction writes nothing, and
// "the result was zero" could not be told from "the register happened to be zero".
#define DOTPROBE_SENTINEL 0xcafef00du

#define VOP3P(mnemonic)                                                                       \
    d = DOTPROBE_SENTINEL;                                                                    \
    asm volatile(mnemonic " %0, %1, %2, %3" : "+v"(d) : "v"(x), "v"(y), "v"(z))
#define VOP2C(mnemonic)                                                                       \
    d = z;                                                                                    \
    asm volatile(mnemonic " %0, %1, %2" : "+v"(d) : "v"(x), "v"(y))

// A VOP2 word written out by hand, for the opcode slots that have no GFX10 mnemonic. The
// operands cannot be left to the register allocator here, because the register numbers are part
// of the word: the constraints name the physical registers v5, v8 and v9, which clang honours
// for AMDGPU and surrounds with the copies the kernel needs. The destination constraint is tied
// ("+"), so v5 is loaded with the third operand before the instruction runs; that covers the
// accumulate reading of the slot as well as the write-only reading.
//
// VOP2 on GFX10: src0 in bits [8:0] (a VGPR is 256 + n), vsrc1 in [16:9], vdst in [24:17],
// opcode in [30:25], bit 31 clear. With vdst = v5, src0 = v8 and vsrc1 = v9 the constant part
// is 0x000a1308, and the opcode field is the top byte: 0x1a = 0x00d, 0x1c = 0x00e, 0x1e = 0x00f,
// 0x20 = 0x010, 0x04 = 0x002. Each word below was produced by llvm-mc for the instruction that
// does occupy that slot on some generation, and compared against this arithmetic.
#define VOP2RAW(word)                                                                         \
    d = z;                                                                                    \
    asm volatile(".long " word : "+{v5}"(d) : "{v8}"(x), "{v9}"(y))

__global__ void __launch_bounds__(BLOCK) probe(uint32_t *out, const uint32_t *a,
                                               const uint32_t *b, const uint32_t *c, int op) {
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t       x = a[i], y = b[i], z = c[i];
    uint32_t       d = 0u;

    switch (op) {
    /* The positive controls. gfx1013 has all three without any forced feature, so a failure
     * here is a failure of this probe (its build, its reference, the runtime, the loader or the
     * dispatch), not a statement about the dot instructions. */
    case OP_PK_FMA_F16:
        VOP3P("v_pk_fma_f16");
        break;
    case OP_FMA_MIX_F32:
        VOP3P("v_fma_mix_f32");
        break;
    case OP_MUL_I32_I24_SDWA:
        /* The instruction the compiler already emits on this part when it expands a byte dot
         * product by hand (evidence/m16/gfx1013-dp4a-2026-10-09). */
        d = DOTPROBE_SENTINEL;
        asm volatile("v_mul_i32_i24_sdwa %0, sext(%1), sext(%2) dst_sel:DWORD "
                     "dst_unused:UNUSED_PAD src0_sel:BYTE_0 src1_sel:BYTE_0"
                     : "+v"(d)
                     : "v"(x), "v"(y));
        break;

    /* The seven VOP3P dot instructions of GFX10.1 (opcodes 0x13 to 0x19). */
    case OP_DOT4_I32_I8:
        VOP3P("v_dot4_i32_i8");
        break;
    case OP_DOT2_F32_F16:
        VOP3P("v_dot2_f32_f16");
        break;
    case OP_DOT4_U32_U8:
        VOP3P("v_dot4_u32_u8");
        break;
    case OP_DOT2_I32_I16:
        VOP3P("v_dot2_i32_i16");
        break;
    case OP_DOT2_U32_U16:
        VOP3P("v_dot2_u32_u16");
        break;
    case OP_DOT8_I32_I4:
        VOP3P("v_dot8_i32_i4");
        break;
    case OP_DOT8_U32_U4:
        VOP3P("v_dot8_u32_u4");
        break;

    /* The two VOP2 accumulate forms of GFX10.1 (opcodes 0x02 and 0x0d). */
    case OP_DOT4C_I32_I8:
        VOP2C("v_dot4c_i32_i8");
        break;
    case OP_DOT2C_F32_F16:
        VOP2C("v_dot2c_f32_f16");
        break;

    /* The two VOP2 f32 minimum and maximum instructions gfx1013 is documented to have. They are
     * the yardstick: whatever rule the hardware follows for NaN, signed zero and denormals shows
     * up here, measured on this part, and the slot probes below are compared against it lane for
     * lane rather than against a guess about the mode register. */
    case OP_MIN_F32:
        VOP2C("v_min_f32");
        break;
    case OP_MAX_F32:
        VOP2C("v_max_f32");
        break;

    /* The same two instructions, hand-encoded. These are the positive controls for the raw
     * route: they must agree with the mnemonic cases above in every lane, or the route itself
     * (the physical register constraints, the word arithmetic, the assembler) is at fault and
     * the slot probes below say nothing. */
    case OP_RAW_0X0F:
        VOP2RAW("0x1e0a1308");
        break;
    case OP_RAW_0X10:
        VOP2RAW("0x200a1308");
        break;

    /* The slots themselves. */
    case OP_RAW_0X0D:
        VOP2RAW("0x1a0a1308");
        break;
    case OP_RAW_0X02:
        VOP2RAW("0x040a1308");
        break;
    case OP_RAW_0X0E:
        VOP2RAW("0x1c0a1308");
        break;

    default:
        d = 0xdeadbeefu;
        break;
    }
    out[i] = d;
}

// ----------------------------------------------------------------------------------------------
// Half precision on the host, by bit pattern. The host compiler needs no _Float16 support: the
// probe picks its half operands out of a fixed table of exactly representable values and decodes
// them with ten lines of integer code, so the reference is exact and portable.
// ----------------------------------------------------------------------------------------------

static float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) & 1u;
    const uint32_t exp = (uint32_t)(h >> 10) & 0x1fu;
    const uint32_t man = (uint32_t)h & 0x3ffu;
    float          value;
    if (exp == 0u) {
        value = (float)man * 5.9604644775390625e-8f; /* 2^-24 */
    } else if (exp == 31u) {
        value = (man == 0u) ? 1e38f * 1e38f : 0.0f / 0.0f;
    } else {
        /* 2^(exp-15) * (1 + man/1024), built by multiplication so no strtof is needed */
        float scale = 1.0f;
        int   e = (int)exp - 15;
        for (; e > 0; --e) { scale *= 2.0f; }
        for (; e < 0; ++e) { scale *= 0.5f; }
        value = scale * (1.0f + (float)man / 1024.0f);
    }
    return sign ? -value : value;
}

/* Eleven halves, every one a small power of two or zero, so every product of two of them and
 * every sum of two such products is exact in a 32-bit float. The probe therefore compares for
 * equality and never for a tolerance. */
static const uint16_t kHalves[] = {
    0x0000u, /*  0    */ 0x3c00u, /*  1    */ 0xbc00u, /* -1    */ 0x4000u, /*  2    */
    0xc000u, /* -2    */ 0x3800u, /*  0.5  */ 0xb800u, /* -0.5  */ 0x4400u, /*  4    */
    0xc400u, /* -4    */ 0x4800u, /*  8    */ 0xc800u, /* -8    */
};
#define HALF_COUNT ((unsigned)(sizeof(kHalves) / sizeof(kHalves[0])))

/* The f32 accumulators of the two f16 dot instructions, also exact. */
static const float kAccF32[] = { 0.0f, 1.0f, -3.0f, 16.0f, 0.25f, -0.5f, 64.0f };
#define ACCF32_COUNT ((unsigned)(sizeof(kAccF32) / sizeof(kAccF32[0])))

static uint32_t f32_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static float bits_f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* f16 -> bits, for the small set of values this probe can produce. Returns 0xffffffff when the
 * value is not exactly representable in the set, which build_operands prevents. */
static uint32_t float_to_half_bits(float v) {
    uint32_t sign = 0u;
    int      e;
    if (v == 0.0f) { return 0u; }
    if (v < 0.0f) {
        sign = 0x8000u;
        v = -v;
    }
    for (e = -24; e <= 15; ++e) {
        float scale = 1.0f;
        int   t = e;
        for (; t > 0; --t) { scale *= 2.0f; }
        for (; t < 0; ++t) { scale *= 0.5f; }
        if (v >= scale && v < scale * 2.0f) {
            const float    frac = v / scale - 1.0f;
            const float    man = frac * 1024.0f;
            const uint32_t m = (uint32_t)(man + 0.5f);
            if ((float)m != man) { return 0xffffffffu; }
            if (e < -14) { return 0xffffffffu; } /* subnormal: not used by this probe */
            return sign | ((uint32_t)(e + 15) << 10) | m;
        }
    }
    return 0xffffffffu;
}

// ----------------------------------------------------------------------------------------------
// The integer operand generator. Every lane gets a different 32-bit pattern, and the first lanes
// hold the edge cases on purpose: the most negative byte (0x80), the largest positive byte
// (0x7f), the all-ones word, the all-zero word, and 0x80 against 0x80, whose signed product
// 16384 per byte is the largest a signed byte pair can give. The accumulators cross the signed
// boundary (0x7fffffff, 0x80000000, 0xffffffff).
// ----------------------------------------------------------------------------------------------

static const uint32_t kEdgeA[] = { 0x00000000u, 0xffffffffu, 0x80808080u, 0x7f7f7f7fu,
                                   0x80808080u, 0x7f80017fu, 0x01020304u, 0xfefdfcfbu,
                                   0x8000007fu, 0x0f0f0f0fu, 0xf0f0f0f0u, 0x88888888u };
static const uint32_t kEdgeB[] = { 0xffffffffu, 0xffffffffu, 0x80808080u, 0x7f7f7f7fu,
                                   0x7f7f7f7fu, 0x807f7f01u, 0x04030201u, 0x01010101u,
                                   0x7f000080u, 0xf0f0f0f0u, 0x0f0f0f0fu, 0x77777777u };
static const uint32_t kEdgeC[] = { 0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu,
                                   0x80000000u, 0x00010000u, 0x0000ffffu, 0x12345678u,
                                   0x00000000u, 0xfffffffeu, 0x00000002u, 0xdeadbeefu };
#define EDGE_COUNT ((unsigned)(sizeof(kEdgeA) / sizeof(kEdgeA[0])))

/* A cheap deterministic word for the lanes past the edge table. */
static uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ----------------------------------------------------------------------------------------------
// The references. All integer arithmetic is done in uint32_t, so a sum that leaves the signed
// range wraps in a defined way instead of being undefined behaviour. None of these instructions
// clamps here: the clamp bit of every encoding is zero, which build.ps1 checks in the
// disassembly of the built code object.
// ----------------------------------------------------------------------------------------------

static int32_t sext8(uint32_t v) { return (int32_t)(int8_t)(uint8_t)(v & 0xffu); }
static int32_t sext4(uint32_t v) {
    const uint32_t n = v & 0xfu;
    return (int32_t)((n & 0x8u) ? (int32_t)n - 16 : (int32_t)n);
}
static int32_t sext16(uint32_t v) { return (int32_t)(int16_t)(uint16_t)(v & 0xffffu); }

static uint32_t ref_dot4_i32_i8(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t acc = c;
    unsigned k;
    for (k = 0; k < 4u; ++k) { acc += (uint32_t)(sext8(a >> (8u * k)) * sext8(b >> (8u * k))); }
    return acc;
}

static uint32_t ref_dot4_u32_u8(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t acc = c;
    unsigned k;
    for (k = 0; k < 4u; ++k) {
        acc += ((a >> (8u * k)) & 0xffu) * ((b >> (8u * k)) & 0xffu);
    }
    return acc;
}

static uint32_t ref_dot8_i32_i4(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t acc = c;
    unsigned k;
    for (k = 0; k < 8u; ++k) { acc += (uint32_t)(sext4(a >> (4u * k)) * sext4(b >> (4u * k))); }
    return acc;
}

static uint32_t ref_dot8_u32_u4(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t acc = c;
    unsigned k;
    for (k = 0; k < 8u; ++k) { acc += ((a >> (4u * k)) & 0xfu) * ((b >> (4u * k)) & 0xfu); }
    return acc;
}

static uint32_t ref_dot2_i32_i16(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t acc = c;
    acc += (uint32_t)(sext16(a) * sext16(b));
    acc += (uint32_t)(sext16(a >> 16) * sext16(b >> 16));
    return acc;
}

static uint32_t ref_dot2_u32_u16(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t acc = c;
    acc += (a & 0xffffu) * (b & 0xffffu);
    acc += ((a >> 16) & 0xffffu) * ((b >> 16) & 0xffffu);
    return acc;
}

static uint32_t ref_dot2_f32_f16(uint32_t a, uint32_t b, uint32_t c) {
    const float p0 =
        half_to_float((uint16_t)(a & 0xffffu)) * half_to_float((uint16_t)(b & 0xffffu));
    const float p1 = half_to_float((uint16_t)(a >> 16)) * half_to_float((uint16_t)(b >> 16));
    return f32_bits(p0 + p1 + bits_f32(c));
}

static uint32_t ref_pk_fma_f16(uint32_t a, uint32_t b, uint32_t c) {
    const float lo = half_to_float((uint16_t)(a & 0xffffu))
                         * half_to_float((uint16_t)(b & 0xffffu))
                     + half_to_float((uint16_t)(c & 0xffffu));
    const float hi = half_to_float((uint16_t)(a >> 16)) * half_to_float((uint16_t)(b >> 16))
                     + half_to_float((uint16_t)(c >> 16));
    const uint32_t lob = float_to_half_bits(lo);
    const uint32_t hib = float_to_half_bits(hi);
    if (lob == 0xffffffffu || hib == 0xffffffffu) { return 0xdeadbeefu; }
    return (hib << 16) | (lob & 0xffffu);
}

static uint32_t ref_fma_mix_f32(uint32_t a, uint32_t b, uint32_t c) {
    /* v_fma_mix_f32 with op_sel_hi all zero takes all three sources as f32. */
    return f32_bits(bits_f32(a) * bits_f32(b) + bits_f32(c));
}

static uint32_t ref_mul_i32_i24_sdwa(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (uint32_t)(sext8(a) * sext8(b));
}

// ----------------------------------------------------------------------------------------------
// Candidate references: what an opcode wrote, when it did not write the dot product.
//
// These are not the probe's expected answer. They are evaluated beside it over all 256 lanes and
// reported as match counts, so that one run can tell several hypotheses apart instead of needing
// one run each. They cost no device time: the host already holds every operand and every result.
//
// The three f32 minimum rules differ only in what they do with a NaN operand, and the integer
// edge operands of this probe contain NaNs and denormals on purpose, so the lanes that separate
// the rules are present:
//
//   min-lt-select   (a < b) ? a : b. A NaN makes the comparison false, so the answer is src1.
//   min-ieee        minNum: a NaN operand is ignored and the other one is the answer.
//   min-nanprop     a NaN operand is the answer.
// ----------------------------------------------------------------------------------------------

/* The f32 bit patterns the identification turns on: both zeros, both infinities, quiet and
 * signalling NaNs of both signs, the smallest and largest denormal of both signs, the smallest
 * normal, the largest finite, and a few ordinary values. Every ordered pair of these is a lane. */
static const uint32_t kSpecialF32[] = {
    0x00000000u, /* +0 */
    0x80000000u, /* -0 */
    0x3f800000u, /* +1 */
    0xbf800000u, /* -1 */
    0x40000000u, /* +2 */
    0xc0000000u, /* -2 */
    0x3f000000u, /* +0.5 */
    0xbf000000u, /* -0.5 */
    0x3eaaaaabu, /* +1/3 */
    0xbeaaaaabu, /* -1/3 */
    0x7f800000u, /* +inf */
    0xff800000u, /* -inf */
    0x7fc00000u, /* +qNaN */
    0xffc00000u, /* -qNaN */
    0x7f800001u, /* +sNaN, smallest payload */
    0xff800001u, /* -sNaN, smallest payload */
    0x7fffffffu, /* +NaN, every payload bit set */
    0x00000001u, /* +smallest denormal */
    0x80000001u, /* -smallest denormal */
    0x007fffffu, /* +largest denormal */
    0x807fffffu, /* -largest denormal */
    0x00800000u, /* +smallest normal */
    0x80800000u, /* -smallest normal */
    0x7f7fffffu, /* +largest finite */
    0xff7fffffu, /* -largest finite */
};
#define SPECIAL_F32_COUNT ((unsigned)(sizeof(kSpecialF32) / sizeof(kSpecialF32[0])))

static int is_nan_bits(uint32_t b) {
    return ((b & 0x7f800000u) == 0x7f800000u) && ((b & 0x007fffffu) != 0u);
}

static uint32_t ref_min_lt_select(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (bits_f32(a) < bits_f32(b)) ? a : b;
}

static uint32_t ref_min_ieee(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    if (is_nan_bits(a)) { return b; }
    if (is_nan_bits(b)) { return a; }
    return (bits_f32(a) <= bits_f32(b)) ? a : b;
}

static uint32_t ref_min_nanprop(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    if (is_nan_bits(a)) { return a; }
    if (is_nan_bits(b)) { return b; }
    return (bits_f32(a) < bits_f32(b)) ? a : b;
}

static uint32_t ref_max_gt_select(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (bits_f32(a) > bits_f32(b)) ? a : b;
}

/* The tie direction. The two rules differ in exactly one place: a pair that compares equal but
 * has different bits, which for f32 means +0 against -0. The SI and CI ISA define
 * V_MIN_LEGACY_F32 as S0 < S1 ? S0 : S1 and V_MAX_LEGACY_F32 as S0 >= S1 ? S0 : S1, so the
 * minimum takes S1 on a tie and the maximum takes S0. These two rows measure that instead of
 * taking it on trust. */
static uint32_t ref_min_le_select(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (bits_f32(a) <= bits_f32(b)) ? a : b;
}

static uint32_t ref_max_ge_select(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (bits_f32(a) >= bits_f32(b)) ? a : b;
}

static uint32_t ref_max_ieee(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    if (is_nan_bits(a)) { return b; }
    if (is_nan_bits(b)) { return a; }
    return (bits_f32(a) >= bits_f32(b)) ? a : b;
}

static uint32_t ref_max_nanprop(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    if (is_nan_bits(a)) { return a; }
    if (is_nan_bits(b)) { return b; }
    return (bits_f32(a) > bits_f32(b)) ? a : b;
}

/* A denormal flushed to a zero of the same sign. The host's x87/SSE arithmetic keeps denormals;
 * the shader core may not, and a candidate that matches every lane except the denormal ones
 * would otherwise be dismissed for the wrong reason. */
static uint32_t flush_denorm(uint32_t b) {
    if (((b & 0x7f800000u) == 0u) && ((b & 0x007fffffu) != 0u)) { return b & 0x80000000u; }
    return b;
}

static uint32_t ref_min_legacy_ftz(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t fa = flush_denorm(a);
    const uint32_t fb = flush_denorm(b);
    (void)c;
    return flush_denorm((bits_f32(fa) < bits_f32(fb)) ? fa : fb);
}

static uint32_t ref_max_legacy_ftz(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t fa = flush_denorm(a);
    const uint32_t fb = flush_denorm(b);
    (void)c;
    return flush_denorm((bits_f32(fa) > bits_f32(fb)) ? fa : fb);
}

static uint32_t ref_min_i32(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return ((int32_t)a < (int32_t)b) ? a : b;
}

static uint32_t ref_max_i32(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return ((int32_t)a > (int32_t)b) ? a : b;
}

static uint32_t ref_min_u32(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (a < b) ? a : b;
}

static uint32_t ref_max_u32(uint32_t a, uint32_t b, uint32_t c) {
    (void)c;
    return (a > b) ? a : b;
}

static uint32_t ref_src0(uint32_t a, uint32_t b, uint32_t c) {
    (void)b;
    (void)c;
    return a;
}

static uint32_t ref_src1(uint32_t a, uint32_t b, uint32_t c) {
    (void)a;
    (void)c;
    return b;
}

static uint32_t ref_src2(uint32_t a, uint32_t b, uint32_t c) {
    (void)a;
    (void)b;
    return c; /* the destination came back holding what it was loaded with */
}

static uint32_t ref_zero(uint32_t a, uint32_t b, uint32_t c) {
    (void)a;
    (void)b;
    (void)c;
    return 0u;
}

// ----------------------------------------------------------------------------------------------
// The table
// ----------------------------------------------------------------------------------------------

typedef enum operand_kind {
    OPERAND_INT,     /* integer edge words */
    OPERAND_F16_F32, /* a and b are half pairs, c is an f32 accumulator */
    OPERAND_F16_F16, /* a, b and c are half pairs */
    OPERAND_F32,     /* all three are f32 */
    OPERAND_F32_PAIR /* a and b cover the f32 special values and a random stream, c is a third
                      * unrelated word, so "the destination was not written" is visible */
} operand_kind;

typedef uint32_t (*ref_fn)(uint32_t, uint32_t, uint32_t);

typedef struct alt_ref {
    const char *name;
    ref_fn      fn;
} alt_ref;

/* Evaluated for every instruction, because they cost no device time and an unexpected match is
 * worth seeing wherever it turns up. */
static const alt_ref kAlts[] = {
    { "zero", ref_zero },
    { "src0", ref_src0 },
    { "src1", ref_src1 },
    { "src2-unwritten", ref_src2 },
    /* V_MIN_LEGACY_F32 of SI and CI: D = S0 < S1 ? S0 : S1, so a NaN in S1 is the answer and a
     * NaN in S0 is not. The -ftz row is the same rule with input and output denormals flushed. */
    { "min-f32-legacy", ref_min_lt_select },
    { "min-f32-legacy-le", ref_min_le_select },
    { "min-f32-legacy-ftz", ref_min_legacy_ftz },
    { "min-f32-ieee", ref_min_ieee },
    { "min-f32-nanprop", ref_min_nanprop },
    /* V_MAX_LEGACY_F32 of SI and CI. */
    { "max-f32-legacy", ref_max_gt_select },
    { "max-f32-legacy-ge", ref_max_ge_select },
    { "max-f32-legacy-ftz", ref_max_legacy_ftz },
    { "max-f32-ieee", ref_max_ieee },
    { "max-f32-nanprop", ref_max_nanprop },
    { "min-i32", ref_min_i32 },
    { "max-i32", ref_max_i32 },
    { "min-u32", ref_min_u32 },
    { "max-u32", ref_max_u32 },
};
#define ALT_COUNT ((unsigned)(sizeof(kAlts) / sizeof(kAlts[0])))

typedef struct probe_entry {
    int          op;        /* the index the kernel switches on */
    const char  *name;      /* the instruction, as the assembler spells it */
    ref_fn       reference;
    operand_kind operands;
    int          control;   /* 1 = gfx1013 has this instruction without a forced feature */
    const char  *feature;   /* the LLVM target feature that gates it, or "" for a control */
} probe_entry;

static const probe_entry kProbes[] = {
    { OP_PK_FMA_F16, "v_pk_fma_f16", ref_pk_fma_f16, OPERAND_F16_F16, 1, "" },
    { OP_FMA_MIX_F32, "v_fma_mix_f32", ref_fma_mix_f32, OPERAND_F32, 1, "" },
    { OP_MUL_I32_I24_SDWA, "v_mul_i32_i24_sdwa", ref_mul_i32_i24_sdwa, OPERAND_INT, 1, "" },
    { OP_DOT4_I32_I8, "v_dot4_i32_i8", ref_dot4_i32_i8, OPERAND_INT, 0, "dot1-insts" },
    { OP_DOT2_F32_F16, "v_dot2_f32_f16", ref_dot2_f32_f16, OPERAND_F16_F32, 0, "dot10-insts" },
    { OP_DOT4_U32_U8, "v_dot4_u32_u8", ref_dot4_u32_u8, OPERAND_INT, 0, "dot7-insts" },
    { OP_DOT4C_I32_I8, "v_dot4c_i32_i8", ref_dot4_i32_i8, OPERAND_INT, 0, "dot6-insts" },
    { OP_DOT2C_F32_F16, "v_dot2c_f32_f16", ref_dot2_f32_f16, OPERAND_F16_F32, 0, "dot5-insts" },
    { OP_DOT2_I32_I16, "v_dot2_i32_i16", ref_dot2_i32_i16, OPERAND_INT, 0, "dot2-insts" },
    { OP_DOT2_U32_U16, "v_dot2_u32_u16", ref_dot2_u32_u16, OPERAND_INT, 0, "dot2-insts" },
    { OP_DOT8_I32_I4, "v_dot8_i32_i4", ref_dot8_i32_i4, OPERAND_INT, 0, "dot1-insts" },
    { OP_DOT8_U32_U4, "v_dot8_u32_u4", ref_dot8_u32_u4, OPERAND_INT, 0, "dot7-insts" },
    /* The VOP2 slot probe. The reference for all five is the legacy minimum, because that is the
     * claim under test; the candidate table below each result reports every other rule, and the
     * agreement matrix at the end of the run compares the five to each other on the silicon. The
     * two hand-encoded controls must agree with their mnemonic twins in all 4096 lanes. */
    { OP_MIN_F32, "v_min_f32", ref_min_lt_select, OPERAND_F32_PAIR, 1, "" },
    { OP_MAX_F32, "v_max_f32", ref_max_ge_select, OPERAND_F32_PAIR, 1, "" },
    { OP_RAW_0X0F, "raw-vop2-0x00f", ref_min_lt_select, OPERAND_F32_PAIR, 1, "= v_min_f32" },
    { OP_RAW_0X10, "raw-vop2-0x010", ref_max_ge_select, OPERAND_F32_PAIR, 1, "= v_max_f32" },
    { OP_RAW_0X0D, "raw-vop2-0x00d", ref_min_lt_select, OPERAND_F32_PAIR, 0, "v_dot4c slot" },
    { OP_RAW_0X02, "raw-vop2-0x002", ref_zero, OPERAND_F32_PAIR, 0, "v_dot2c slot" },
    { OP_RAW_0X0E, "raw-vop2-0x00e", ref_max_ge_select, OPERAND_F32_PAIR, 0, "empty on GFX10" },
};
#define PROBE_COUNT ((unsigned)(sizeof(kProbes) / sizeof(kProbes[0])))

static void build_operands(operand_kind kind, uint32_t *a, uint32_t *b, uint32_t *c) {
    unsigned i;
    for (i = 0; i < LANES; ++i) {
        switch (kind) {
        case OPERAND_INT:
            if (i < EDGE_COUNT) {
                a[i] = kEdgeA[i];
                b[i] = kEdgeB[i];
                c[i] = kEdgeC[i];
            } else {
                a[i] = mix(i * 3u + 1u);
                b[i] = mix(i * 5u + 2u);
                c[i] = mix(i * 7u + 3u) & 0x0000ffffu;
            }
            break;
        case OPERAND_F16_F32: {
            const uint16_t a0 = kHalves[(i * 3u + 0u) % HALF_COUNT];
            const uint16_t a1 = kHalves[(i * 5u + 1u) % HALF_COUNT];
            const uint16_t b0 = kHalves[(i * 7u + 2u) % HALF_COUNT];
            const uint16_t b1 = kHalves[(i * 11u + 3u) % HALF_COUNT];
            a[i] = ((uint32_t)a1 << 16) | a0;
            b[i] = ((uint32_t)b1 << 16) | b0;
            c[i] = f32_bits(kAccF32[i % ACCF32_COUNT]);
            break;
        }
        case OPERAND_F16_F16: {
            /* All operands are in {0, +-0.5, +-1, +-2, +-4, +-8}, so every product is at most
             * 64 and every fma result is a multiple of 0.25 below 128: exact in f16. */
            const uint16_t a0 = kHalves[(i * 3u + 0u) % HALF_COUNT];
            const uint16_t a1 = kHalves[(i * 5u + 1u) % HALF_COUNT];
            const uint16_t b0 = kHalves[(i * 7u + 2u) % HALF_COUNT];
            const uint16_t b1 = kHalves[(i * 11u + 3u) % HALF_COUNT];
            const uint16_t c0 = kHalves[(i * 13u + 4u) % HALF_COUNT];
            const uint16_t c1 = kHalves[(i * 17u + 5u) % HALF_COUNT];
            a[i] = ((uint32_t)a1 << 16) | a0;
            b[i] = ((uint32_t)b1 << 16) | b0;
            c[i] = ((uint32_t)c1 << 16) | c0;
            break;
        }
        case OPERAND_F32_PAIR: {
            /* Every ordered pair of the special values first: 25 * 25 = 625 lanes, which covers
             * NaN against NaN, NaN against a number in both orders, the two zeros in both
             * orders, the infinities and the denormals. The remaining lanes are a deterministic
             * random word stream, with one operand of every third lane replaced by a special
             * value, so the asymmetric rules are also exercised against ordinary neighbours.
             * The third operand is an unrelated word: if it comes back, nothing was written. */
            const unsigned n = SPECIAL_F32_COUNT;
            if (i < n * n) {
                a[i] = kSpecialF32[i / n];
                b[i] = kSpecialF32[i % n];
            } else {
                const unsigned j = i - n * n;
                uint32_t       ra = mix(j * 2u + 1u);
                uint32_t       rb = mix(j * 2u + 2u);
                if ((j % 3u) == 1u) { ra = kSpecialF32[j % n]; }
                if ((j % 3u) == 2u) { rb = kSpecialF32[j % n]; }
                a[i] = ra;
                b[i] = rb;
            }
            c[i] = mix(i * 11u + 7u);
            break;
        }
        case OPERAND_F32:
        default: {
            static const float kF32[] = { 0.0f,  1.0f,   -1.0f, 2.0f,  -2.0f, 0.5f,
                                          -0.5f, 4.0f,   -4.0f, 8.0f,  -8.0f, 0.25f,
                                          16.0f, -32.0f, 64.0f, 128.0f };
            const unsigned n = (unsigned)(sizeof(kF32) / sizeof(kF32[0]));
            a[i] = f32_bits(kF32[(i * 3u + 0u) % n]);
            b[i] = f32_bits(kF32[(i * 5u + 1u) % n]);
            c[i] = f32_bits(kF32[(i * 7u + 2u) % n]);
            break;
        }
        }
    }
}

// ----------------------------------------------------------------------------------------------
// The run
// ----------------------------------------------------------------------------------------------

static int       g_api_failures;
static FILE     *g_log;
static uint32_t *g_saved[OP_COUNT]; /* one opcode's lanes, kept for the agreement matrix */

static void check(hipError_t err, const char *what) {
    if (err != hipSuccess) {
        printf("dotprobe: %s failed: %s (%s)\n", what, hipGetErrorName(err),
               hipGetErrorString(err));
        g_api_failures++;
    }
}

/* One line in the result log, on disk, flushed before the next launch starts. If the device
 * hangs, the last line here names the instruction that did it. */
static void logline(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (g_log != NULL) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static int set_wait_total(const char *ms) {
    if (ms == NULL || ms[0] == '\0') { return 0; }
#if defined(_WIN32)
    return _putenv_s("BC250_HIP_WAIT_TOTAL_MS", ms) == 0;
#else
    return setenv("BC250_HIP_WAIT_TOTAL_MS", ms, 1) == 0;
#endif
}

static void print_list(void) {
    unsigned i;
    printf("dotprobe: %u instructions, in the order of least risk:\n", PROBE_COUNT);
    for (i = 0; i < PROBE_COUNT; ++i) {
        printf("  %2d %-20s %-9s %s\n", kProbes[i].op, kProbes[i].name,
               kProbes[i].control ? "control" : "probe",
               kProbes[i].control ? "gfx1013 has it" : kProbes[i].feature);
    }
}

/* One launch of the one kernel, with one opcode index. Returns 0 correct, 3 wrong result,
 * 1 a HIP call failed. */
static int run_one(const probe_entry *p, uint32_t *host_a, uint32_t *host_b, uint32_t *host_c,
                   uint32_t *host_out, uint32_t *dev_a, uint32_t *dev_b, uint32_t *dev_c,
                   uint32_t *dev_out) {
    unsigned i;
    unsigned mismatches = 0;
    unsigned sentinels = 0;
    unsigned zeros = 0;
    unsigned untouched = 0;
    int      first_mismatch = -1;
    int      before = g_api_failures;
    clock_t  t0;
    double   ms;

    build_operands(p->operands, host_a, host_b, host_c);
    for (i = 0; i < LANES; ++i) { host_out[i] = 0xa5a5a5a5u; }

    check(hipMemcpy(dev_a, host_a, sizeof(uint32_t) * LANES, hipMemcpyHostToDevice), "copy a");
    check(hipMemcpy(dev_b, host_b, sizeof(uint32_t) * LANES, hipMemcpyHostToDevice), "copy b");
    check(hipMemcpy(dev_c, host_c, sizeof(uint32_t) * LANES, hipMemcpyHostToDevice), "copy c");
    check(hipMemcpy(dev_out, host_out, sizeof(uint32_t) * LANES, hipMemcpyHostToDevice),
          "copy out");
    if (g_api_failures != before) {
        logline("RESULT %-20s API-FAILURE (the operand upload failed)\n", p->name);
        return 1;
    }

    /* Named before the launch, and flushed, so that a hang is attributed. */
    logline("LAUNCH %-20s op %2d %s %s\n", p->name, p->op, p->control ? "control" : "probe",
            p->control ? "(gfx1013 has it)" : p->feature);

    (void)hipGetLastError();
    t0 = clock();
    {
        void *args[5] = { &dev_out, &dev_a, &dev_b, &dev_c, (void *)&p->op };
        check(hipLaunchKernel((const void *)probe, dim3(GRID, 1, 1), dim3(BLOCK, 1, 1), args, 0,
                              0),
              "hipLaunchKernel");
    }
    check(hipDeviceSynchronize(), "hipDeviceSynchronize");
    ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    if (g_api_failures != before) {
        logline("RESULT %-20s API-FAILURE (the dispatch did not complete) after %.1f ms\n",
                p->name, ms);
        return 1;
    }
    check(hipMemcpy(host_out, dev_out, sizeof(uint32_t) * LANES, hipMemcpyDeviceToHost),
          "copy back");
    if (g_api_failures != before) {
        logline("RESULT %-20s API-FAILURE (the copy back failed) after %.1f ms\n", p->name, ms);
        return 1;
    }

    for (i = 0; i < LANES; ++i) {
        const uint32_t want_value = p->reference(host_a[i], host_b[i], host_c[i]);
        if (host_out[i] == 0xcafef00du) { sentinels++; }
        if (host_out[i] == 0u) { zeros++; }
        if (host_out[i] == host_c[i]) { untouched++; }
        if (host_out[i] != want_value) {
            if (mismatches < 8u) {
                logline("  lane %3u a %08x b %08x c %08x -> got %08x want %08x\n", i, host_a[i],
                        host_b[i], host_c[i], host_out[i], want_value);
            }
            if (first_mismatch < 0) { first_mismatch = (int)i; }
            mismatches++;
        }
    }
    /* The lanes are kept, so that the end of the run can compare one opcode against another on
     * the same operands. Two opcodes that agreed in every lane are the same operation on this
     * part, and that comparison needs no assumption about the mode register, because both ran
     * in the same kernel under the same mode. */
    if (p->operands == OPERAND_F32_PAIR && g_saved[p->op] == NULL) {
        g_saved[p->op] = (uint32_t *)malloc(sizeof(uint32_t) * LANES);
        if (g_saved[p->op] != NULL) {
            memcpy(g_saved[p->op], host_out, sizeof(uint32_t) * LANES);
        }
    }

    if (mismatches == 0) {
        logline("RESULT %-20s EXECUTES-CORRECTLY lanes %u mismatches 0 in %.1f ms\n", p->name,
                LANES, ms);
    } else {
        /* What the destination held instead. A destination equal to the sentinel 0xcafef00d was
         * not written at all; the other counts say what was written instead. */
        logline("RESULT %-20s WRONG-RESULT lanes %u mismatches %u first %d sentinel %u zero %u "
                "equal-to-src2 %u in %.1f ms\n",
                p->name, LANES, mismatches, first_mismatch, sentinels, zeros, untouched, ms);
    }

    /* What it wrote, against every candidate rule, over all lanes. A candidate that matches
     * every lane is the rule the hardware followed; anything short of that is not it. This is
     * printed for the slot probes even when they match their reference, because the interesting
     * reading is the whole column, not one row. No device time is spent here. */
    if (mismatches != 0 || p->operands == OPERAND_F32_PAIR) {
        unsigned k;
        for (k = 0; k < ALT_COUNT; ++k) {
            unsigned hit = 0;
            for (i = 0; i < LANES; ++i) {
                if (host_out[i] == kAlts[k].fn(host_a[i], host_b[i], host_c[i])) { hit++; }
            }
            logline("  ALT %-20s %-18s %4u of %u%s\n", p->name, kAlts[k].name, hit, LANES,
                    (hit == LANES) ? "   <= all lanes" : "");
        }
    }
    return (mismatches == 0) ? 0 : 3;
}

/* Every pair of the opcodes that ran on the same f32 operands, compared lane for lane. */
static void agreement_matrix(void) {
    unsigned i, j;
    int      any = 0;

    for (i = 0; i < PROBE_COUNT; ++i) {
        if (kProbes[i].operands == OPERAND_F32_PAIR && g_saved[kProbes[i].op] != NULL) {
            any = 1;
        }
    }
    if (!any) { return; }

    logline("dotprobe: agreement between opcodes, lane for lane, out of %u lanes\n", LANES);
    for (i = 0; i < PROBE_COUNT; ++i) {
        const uint32_t *x;
        if (kProbes[i].operands != OPERAND_F32_PAIR) { continue; }
        x = g_saved[kProbes[i].op];
        if (x == NULL) { continue; }
        for (j = i + 1u; j < PROBE_COUNT; ++j) {
            const uint32_t *y;
            unsigned        lane, hit = 0, first = LANES;
            if (kProbes[j].operands != OPERAND_F32_PAIR) { continue; }
            y = g_saved[kProbes[j].op];
            if (y == NULL) { continue; }
            for (lane = 0; lane < LANES; ++lane) {
                if (x[lane] == y[lane]) {
                    hit++;
                } else if (first == LANES) {
                    first = lane;
                }
            }
            logline("  SAME %-16s %-16s %4u of %u%s\n", kProbes[i].name, kProbes[j].name, hit,
                    LANES, (hit == LANES) ? "   <= identical" : "");
            if (hit != LANES) {
                unsigned shown = 0;
                for (lane = first; lane < LANES && shown < 4u; ++lane) {
                    if (x[lane] != y[lane]) {
                        logline("       lane %4u %s %08x  %s %08x\n", lane, kProbes[i].name,
                                x[lane], kProbes[j].name, y[lane]);
                        shown++;
                    }
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *want = NULL;
    const char *logpath = "dotprobe-results.txt";
    uint32_t   *host_a = NULL, *host_b = NULL, *host_c = NULL, *host_out = NULL;
    uint32_t   *dev_a = NULL, *dev_b = NULL, *dev_c = NULL, *dev_out = NULL;
    int         i;
    int         worst = 0;
    int         ran = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list") == 0) {
            print_list();
            return 0;
        } else if (strcmp(argv[i], "--op") == 0 && i + 1 < argc) {
            want = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logpath = argv[++i];
        } else if (strcmp(argv[i], "--wait-total") == 0 && i + 1 < argc) {
            ++i;
            if (!set_wait_total(argv[i])) {
                printf("dotprobe: cannot set the wait bound to '%s'\n", argv[i]);
                return 2;
            }
        } else {
            printf("dotprobe: unknown argument '%s'\n", argv[i]);
            printf("dotprobe: usage: dotprobe.exe [--op <instruction>] [--log <file>] "
                   "[--wait-total <ms>]\n");
            printf("dotprobe:        dotprobe.exe --list\n");
            printf("dotprobe: with no --op it runs every instruction in the order of least "
                   "risk\n");
            return 2;
        }
    }
    if (want != NULL) {
        int found = 0;
        for (i = 0; i < (int)PROBE_COUNT; ++i) {
            if (strcmp(kProbes[i].name, want) == 0) { found = 1; }
        }
        if (!found) {
            printf("dotprobe: this build has no probe for '%s'\n", want);
            return 4;
        }
    }

    g_log = fopen(logpath, "a");
    if (g_log == NULL) { printf("dotprobe: cannot append to %s, stdout only\n", logpath); }

    host_a = (uint32_t *)malloc(sizeof(uint32_t) * LANES);
    host_b = (uint32_t *)malloc(sizeof(uint32_t) * LANES);
    host_c = (uint32_t *)malloc(sizeof(uint32_t) * LANES);
    host_out = (uint32_t *)malloc(sizeof(uint32_t) * LANES);
    if (!host_a || !host_b || !host_c || !host_out) {
        printf("dotprobe: out of host memory\n");
        return 1;
    }

    logline("dotprobe: start, lanes %u, grid %u, block %u, one kernel, one code object\n", LANES,
            GRID, BLOCK);
    check(hipInit(0), "hipInit");
    {
        hipDeviceProp_t prop;
        memset(&prop, 0, sizeof(prop));
        check(hipGetDeviceProperties(&prop, 0), "hipGetDeviceProperties");
        logline("dotprobe: device '%s' arch '%s' warp %d cu %d\n", prop.name, prop.gcnArchName,
                prop.warpSize, prop.multiProcessorCount);
    }
    check(hipMalloc((void **)&dev_a, sizeof(uint32_t) * LANES), "hipMalloc a");
    check(hipMalloc((void **)&dev_b, sizeof(uint32_t) * LANES), "hipMalloc b");
    check(hipMalloc((void **)&dev_c, sizeof(uint32_t) * LANES), "hipMalloc c");
    check(hipMalloc((void **)&dev_out, sizeof(uint32_t) * LANES), "hipMalloc out");
    if (g_api_failures != 0) {
        logline("dotprobe: setup failed, nothing ran\n");
        return 1;
    }

    for (i = 0; i < (int)PROBE_COUNT; ++i) {
        int rc;
        if (want != NULL && strcmp(kProbes[i].name, want) != 0) { continue; }
        rc = run_one(&kProbes[i], host_a, host_b, host_c, host_out, dev_a, dev_b, dev_c, dev_out);
        ran++;
        if (rc == 1) {
            /* A failed dispatch means the device may be in recovery: stop rather than ask it
             * for the next instruction. The log already names this one. */
            logline("dotprobe: STOPPING after an API failure on %s\n", kProbes[i].name);
            worst = 1;
            break;
        }
        if (rc == 3 && worst == 0) { worst = 3; }
    }

    agreement_matrix();
    for (i = 0; i < OP_COUNT; ++i) { free(g_saved[i]); }

    logline("dotprobe: ran %d of %u instructions, worst %s\n", ran, PROBE_COUNT,
            (worst == 0) ? "EXECUTES-CORRECTLY" : (worst == 3) ? "WRONG-RESULT" : "API-FAILURE");

    check(hipFree(dev_out), "hipFree out");
    check(hipFree(dev_c), "hipFree c");
    check(hipFree(dev_b), "hipFree b");
    check(hipFree(dev_a), "hipFree a");
    free(host_out);
    free(host_c);
    free(host_b);
    free(host_a);
    if (g_log != NULL) { fclose(g_log); }
    return worst;
}
