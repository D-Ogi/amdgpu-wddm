// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: original AMD MIT HUBP1 functions in dc_surface_sequence.inc.
// Capture only the linear mono path. Windows owns the full display transaction.
#include "dcn_surface_write.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../shim/bc250_dc_types.h"
#include "../shim/generated/dc_surface_sequence.inc"

enum { L_PITCH = SURFACE_SEQUENCE_REG_COUNT, L_PITCH_C, L_FORMAT, L_CROSSBAR };
static const unsigned sequence[BC250_SURFACE_WRITE_WORDS] = {
    DCSURF_SURFACE_CONTROL, DCSURF_ADDR_CONFIG, DCSURF_TILING_CONFIG,
    L_PITCH, L_PITCH_C, L_FORMAT, L_CROSSBAR,
    DCSURF_FLIP_CONTROL, DCSURF_FLIP_CONTROL, DCSURF_FLIP_CONTROL,
    DCSURF_SURFACE_CONTROL, DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, DCSURF_PRIMARY_SURFACE_ADDRESS
};
static const unsigned long masks[BC250_SURFACE_WRITE_WORDS] = {
    MASK_PRIMARY_SURFACE_DCC_EN | MASK_PRIMARY_SURFACE_DCC_IND_64B_BLK |
        MASK_SECONDARY_SURFACE_DCC_EN | MASK_SECONDARY_SURFACE_DCC_IND_64B_BLK,
    MASK_NUM_PIPES | MASK_NUM_BANKS | MASK_PIPE_INTERLEAVE | MASK_NUM_SE | MASK_NUM_RB_PER_SE | MASK_MAX_COMPRESSED_FRAGS,
    MASK_SW_MODE | MASK_META_LINEAR | MASK_RB_ALIGNED | MASK_PIPE_ALIGNED,
    HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK | HUBPREQ0_DCSURF_SURFACE_PITCH__META_PITCH_MASK,
    HUBPREQ0_DCSURF_SURFACE_PITCH_C__PITCH_C_MASK | HUBPREQ0_DCSURF_SURFACE_PITCH_C__META_PITCH_C_MASK,
    HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK,
    HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK | HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK,
    MASK_SURFACE_FLIP_TYPE, MASK_SURFACE_FLIP_MODE_FOR_STEREOSYNC, MASK_SURFACE_FLIP_IN_STEREOSYNC,
    MASK_PRIMARY_SURFACE_TMZ | MASK_PRIMARY_META_SURFACE_TMZ,
    0xfffffffful, 0xfffffffful // AMD REG_SET(address, 0, ...) overwrites the register.
};
#define OFFSETS(p) { \
    BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_CONTROL, \
    BC250_REG_DMU_HUBP##p##_DCSURF_ADDR_CONFIG, BC250_REG_DMU_HUBP##p##_DCSURF_TILING_CONFIG, \
    BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_PITCH, BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_PITCH_C, \
    BC250_REG_DMU_HUBP##p##_DCSURF_SURFACE_CONFIG, BC250_REG_DMU_HUBPRET##p##_HUBPRET_CONTROL, \
    BC250_REG_DMU_HUBPREQ##p##_DCSURF_FLIP_CONTROL, BC250_REG_DMU_HUBPREQ##p##_DCSURF_FLIP_CONTROL, \
    BC250_REG_DMU_HUBPREQ##p##_DCSURF_FLIP_CONTROL, BC250_REG_DMU_HUBPREQ##p##_DCSURF_SURFACE_CONTROL, \
    BC250_REG_DMU_HUBPREQ##p##_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, BC250_REG_DMU_HUBPREQ##p##_DCSURF_PRIMARY_SURFACE_ADDRESS }
static const unsigned long offsets[2][BC250_SURFACE_WRITE_WORDS] = { OFFSETS(0), OFFSETS(1) };
#define SAME(block,reg,field) _Static_assert(block##0_##reg##__##field##_MASK == block##1_##reg##__##field##_MASK, "surface instance mask mismatch")
SAME(HUBP,DCHUBP_CNTL,HUBP_VTG_SEL);
SAME(HUBPREQ,DCSURF_FLIP_CONTROL,SURFACE_FLIP_PENDING);
SAME(HUBPREQ,DCSURF_FLIP_CONTROL,SURFACE_UPDATE_LOCK);
SAME(HUBPREQ,DCSURF_FLIP_CONTROL,HUBPREQ_MASTER_UPDATE_LOCK_STATUS);
SAME(HUBPREQ,DCSURF_FLIP_CONTROL2,SURFACE_TRIPLE_BUFFER_ENABLE);
SAME(HUBPREQ,DCSURF_SURFACE_PITCH,PITCH);
SAME(HUBPREQ,DCSURF_SURFACE_PITCH,META_PITCH);
SAME(HUBPREQ,DCSURF_SURFACE_PITCH_C,PITCH_C);
SAME(HUBPREQ,DCSURF_SURFACE_PITCH_C,META_PITCH_C);
SAME(HUBP,DCSURF_SURFACE_CONFIG,SURFACE_PIXEL_FORMAT);
SAME(HUBPRET,HUBPRET_CONTROL,CROSSBAR_SRC_CB_B);
SAME(HUBPRET,HUBPRET_CONTROL,CROSSBAR_SRC_CR_R);

// Private capture context, not a Linux object ABI. No MMIO callback exists here.
struct hubp { struct dc_plane_address request_address; };
struct dcn10_hubp { struct hubp base; BC250_SURFACE_WRITE_PLAN plan; unsigned count, invalid; };
struct dc_tiling_info { struct {
    unsigned num_pipes, num_banks, pipe_interleave, num_shader_engines, num_rb_per_se, max_compressed_frags;
    unsigned swizzle, meta_linear, rb_aligned, pipe_aligned;
} gfx9; };
#define TO_DCN10_HUBP(p) ((struct dcn10_hubp*)(p))
static unsigned log_2(unsigned n) { unsigned v = 0; while (n > 1) { n >>= 1; ++v; } return v; }
static unsigned long Field(struct dcn10_hubp* p, unsigned long mask, unsigned long value)
{
    if (!mask || value > Bc250DispGet(mask, mask)) p->invalid = 1;
    return Bc250DispField(mask, value);
}
static void Capture(struct dcn10_hubp* p, unsigned reg, unsigned long mask, unsigned long value)
{
    if (p->count >= BC250_SURFACE_WRITE_WORDS || sequence[p->count] != reg ||
        masks[p->count] != mask || (value & ~mask)) { p->invalid = 1; return; }
    p->plan.Words[p->count++] = value;
}
#define F(f,v) Field(hubp1, MASK_##f, (unsigned long)(v))
#define REG_UPDATE(r,f,v) Capture(hubp1,r,MASK_##f,F(f,v))
#define REG_UPDATE_2(r,a,av,b,bv) Capture(hubp1,r,MASK_##a|MASK_##b,F(a,av)|F(b,bv))
#define REG_UPDATE_4(r,a,av,b,bv,c,cv,d,dv) Capture(hubp1,r,MASK_##a|MASK_##b|MASK_##c|MASK_##d,F(a,av)|F(b,bv)|F(c,cv)|F(d,dv))
#define REG_UPDATE_6(r,a,av,b,bv,c,cv,d,dv,e,ev,f,fv) Capture(hubp1,r,MASK_##a|MASK_##b|MASK_##c|MASK_##d|MASK_##e|MASK_##f,F(a,av)|F(b,bv)|F(c,cv)|F(d,dv)|F(e,ev)|F(f,fv))
#define REG_UPDATE_8(r,a,av,b,bv,c,cv,d,dv,e,ev,f,fv,g,gv,h,hv) Capture(hubp1,r,MASK_##a|MASK_##b|MASK_##c|MASK_##d|MASK_##e|MASK_##f|MASK_##g|MASK_##h,F(a,av)|F(b,bv)|F(c,cv)|F(d,dv)|F(e,ev)|F(f,fv)|F(g,gv)|F(h,hv))
#define REG_SET(r,initial,f,v) Capture(hubp1,r,0xfffffffful,(initial)|F(f,v))
#define BREAK_TO_DEBUGGER() (hubp1->invalid = 1)
// The original tiling function does not use its format argument.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4100 4127) // original parameters and constant capture masks
#endif
#define BC250_SURFACE_SEQUENCE_FUNCTIONS
#include "../shim/generated/dc_surface_sequence.inc"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

static int PowerOfTwo(unsigned value) { return value && !(value & (value - 1)); }
int Bc250SurfaceWritePrepare(const BC250_SURFACE_WRITE_REQUEST* Request,
                            BC250_SURFACE_WRITE_PLAN* Plan)
{
    BC250_SURFACE_WRITE_REQUEST in = {0};
    BC250_SURFACE_LAYOUT layout;
    struct dcn10_hubp state = {0};
    struct dc_tiling_info tiling = {0};
    struct dc_plane_address address = {0};
    if (!Plan) return 0;
    if (Request) in = *Request;
    *Plan = (BC250_SURFACE_WRITE_PLAN){0};
    if (!Request || !PowerOfTwo(in.NumPipes) || !PowerOfTwo(in.NumBanks) ||
        !PowerOfTwo(in.NumShaderEngines) || !PowerOfTwo(in.NumRbPerSe) ||
        !PowerOfTwo(in.MaxCompressedFrags) || !Bc250SurfaceLayoutPrepare(&in.Surface, &layout)) return 0;
    tiling.gfx9.num_pipes = in.NumPipes; tiling.gfx9.num_banks = in.NumBanks;
    tiling.gfx9.num_shader_engines = in.NumShaderEngines; tiling.gfx9.num_rb_per_se = in.NumRbPerSe;
    tiling.gfx9.max_compressed_frags = in.MaxCompressedFrags; tiling.gfx9.pipe_interleave = in.PipeInterleave;
    tiling.gfx9.swizzle = DC_SW_LINEAR;
    hubp1_dcc_control(&state.base, false, hubp_ind_block_unconstrained);
    hubp1_program_tiling(&state.base, &tiling, SURFACE_PIXEL_FORMAT_GRPH_ARGB8888);
    // These four fields were captured from the original size/format functions.
    // Deviation: also clear the unused chroma pitch for these single-plane
    // formats. This is not two-plane YUV support; DML/requestor state is separate.
    Capture(&state,L_PITCH,masks[3],layout.Pitch);
    Capture(&state,L_PITCH_C,masks[4],layout.PitchC);
    Capture(&state,L_FORMAT,masks[5],layout.SurfaceConfig);
    Capture(&state,L_CROSSBAR,masks[6],layout.Crossbar);
    address.type = PLN_ADDR_TYPE_GRAPHICS;
    address.grph.addr.quad_part = (int64_t)layout.Physical;
    if (!hubp1_program_surface_flip_and_addr(&state.base,&address,false) ||
        state.invalid || state.count != BC250_SURFACE_WRITE_WORDS) return 0;
    state.plan.Layout = layout; state.plan.Ready = 1; *Plan = state.plan;
    return 1;
}

#define TRY(x) do { long rc = (x); if (rc < 0) return rc; } while (0)
long Bc250SurfaceWriteApplyLocked(BC250_DISP_IO* Io, unsigned Pipe,
    const BC250_SURFACE_WRITE_PLAN* Plan, unsigned* WritesAttempted)
{
    unsigned i, j;
    unsigned long value, final[BC250_SURFACE_WRITE_WORDS];
    static const unsigned long hubp[] = {BC250_REG_DMU_HUBP0_DCHUBP_CNTL, BC250_REG_DMU_HUBP1_DCHUBP_CNTL};
    static const unsigned long flip2[] = {BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL2, BC250_REG_DMU_HUBPREQ1_DCSURF_FLIP_CONTROL2};
    const unsigned long held = OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                              OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
    if (WritesAttempted) *WritesAttempted = 0;
    if (!Io || !Io->Read || !Io->Write || Pipe > 1 || !Plan || !WritesAttempted ||
        Plan->Ready != 1 || Plan->Layout.Ready != 1) return BC250_DISP_STATUS_INVALID;
    for (i = 0; i < BC250_SURFACE_WRITE_WORDS; ++i)
        if (Plan->Words[i] & ~masks[i]) return BC250_DISP_STATUS_INVALID;
    if (Plan->Words[11] != Plan->Layout.AddressHigh || Plan->Words[12] != Plan->Layout.AddressLow)
        return BC250_DISP_STATUS_INVALID;
    TRY(Bc250DispRead(Io,BC250_REG_DMU_OTG0_OTG_CONTROL,&value));
    if (!(value & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK)) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(Io,BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,&value));
    if (value & OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(Io,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,&value));
    if ((value & held) != held) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(Io,hubp[Pipe],&value));
    if (value & HUBP0_DCHUBP_CNTL__HUBP_VTG_SEL_MASK) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(Io,flip2[Pipe],&value));
    if (value & HUBPREQ0_DCSURF_FLIP_CONTROL2__SURFACE_TRIPLE_BUFFER_ENABLE_MASK) return BC250_DISP_STATUS_MISMATCH;
    for (i = 0; i < BC250_SURFACE_WRITE_WORDS; ++i) {
        value = 0;
        if (masks[i] != 0xfffffffful) {
            for (j = i; j > 0; --j) if (offsets[Pipe][j-1] == offsets[Pipe][i]) break;
            if (j) value = final[j-1];
            else TRY(Bc250DispRead(Io,offsets[Pipe][i],&value));
        }
        if (i == 7) {
            if (value & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return BC250_DISP_STATUS_BUSY;
            if (!(value & HUBPREQ0_DCSURF_FLIP_CONTROL__HUBPREQ_MASTER_UPDATE_LOCK_STATUS_MASK) ||
                (value & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK)) return BC250_DISP_STATUS_MISMATCH;
        }
        final[i] = (value & ~masks[i]) | Plan->Words[i];
    }
    for (i = 0; i < BC250_SURFACE_WRITE_WORDS; ++i) {
        ++*WritesAttempted;
        TRY(Bc250DispWrite(Io,offsets[Pipe][i],final[i]));
    }
    return BC250_DISP_STATUS_SUCCESS;
}
