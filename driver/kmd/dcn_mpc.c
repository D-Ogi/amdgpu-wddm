// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: imported DCN201 defaults and blend packing remain AMD MIT in
// ../shim/generated/dc_mpc_blend.inc, Linux 7d0a66e4bb90.
// Deviation: build a fixed two-instance tree before locking. Keep the bottom
// plane in TOP_LAYER_ONLY (with black background), never PASSTHROUGH after a
// removal: the remaining plane's alpha must still apply. Keep lock selectors
// assigned to OPP0 until the owner quiesces/releases these reserved instances.
#include "dcn_mpc.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdbool.h>
#include <stddef.h>

enum { W_MPCC_CONTROL, W_MPCC_TOP_GAIN, W_MPCC_BOT_GAIN_INSIDE, W_MPCC_BOT_GAIN_OUTSIDE, W_MPCC_BG_R_CR, W_MPCC_BG_G_Y, W_MPCC_BG_B_CB, W_MPCC_TOP_SEL, W_MPCC_BOT_SEL, W_MPCC_OPP_ID, W_MPCC_SM_CONTROL, W_COUNT };
_Static_assert(W_COUNT == BC250_MPC_WORDS_PER_PLANE, "MPC word count");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_MODE_MASK == MPCC1_MPCC_CONTROL__MPCC_MODE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE_MASK == MPCC1_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_ALPHA_MULTIPLIED_MODE_MASK == MPCC1_MPCC_CONTROL__MPCC_ALPHA_MULTIPLIED_MODE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_BLND_ACTIVE_OVERLAP_ONLY_MASK == MPCC1_MPCC_CONTROL__MPCC_BLND_ACTIVE_OVERLAP_ONLY_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_BG_BPC_MASK == MPCC1_MPCC_CONTROL__MPCC_BG_BPC_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_BOT_GAIN_MODE_MASK == MPCC1_MPCC_CONTROL__MPCC_BOT_GAIN_MODE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_GLOBAL_ALPHA_MASK == MPCC1_MPCC_CONTROL__MPCC_GLOBAL_ALPHA_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_CONTROL__MPCC_GLOBAL_GAIN_MASK == MPCC1_MPCC_CONTROL__MPCC_GLOBAL_GAIN_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_TOP_GAIN__MPCC_TOP_GAIN_MASK == MPCC1_MPCC_TOP_GAIN__MPCC_TOP_GAIN_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_BOT_GAIN_INSIDE__MPCC_BOT_GAIN_INSIDE_MASK == MPCC1_MPCC_BOT_GAIN_INSIDE__MPCC_BOT_GAIN_INSIDE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_BOT_GAIN_OUTSIDE__MPCC_BOT_GAIN_OUTSIDE_MASK == MPCC1_MPCC_BOT_GAIN_OUTSIDE__MPCC_BOT_GAIN_OUTSIDE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_BG_R_CR__MPCC_BG_R_CR_MASK == MPCC1_MPCC_BG_R_CR__MPCC_BG_R_CR_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_BG_G_Y__MPCC_BG_G_Y_MASK == MPCC1_MPCC_BG_G_Y__MPCC_BG_G_Y_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_BG_B_CB__MPCC_BG_B_CB_MASK == MPCC1_MPCC_BG_B_CB__MPCC_BG_B_CB_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL_MASK == MPCC1_MPCC_TOP_SEL__MPCC_TOP_SEL_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL_MASK == MPCC1_MPCC_BOT_SEL__MPCC_BOT_SEL_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_OPP_ID__MPCC_OPP_ID_MASK == MPCC1_MPCC_OPP_ID__MPCC_OPP_ID_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_SM_CONTROL__MPCC_SM_EN_MASK == MPCC1_MPCC_SM_CONTROL__MPCC_SM_EN_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_SM_CONTROL__MPCC_SM_MODE_MASK == MPCC1_MPCC_SM_CONTROL__MPCC_SM_MODE_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_SM_CONTROL__MPCC_SM_FRAME_ALT_MASK == MPCC1_MPCC_SM_CONTROL__MPCC_SM_FRAME_ALT_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_SM_CONTROL__MPCC_SM_FIELD_ALT_MASK == MPCC1_MPCC_SM_CONTROL__MPCC_SM_FIELD_ALT_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_FRAME_POL_MASK == MPCC1_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_FRAME_POL_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_TOP_POL_MASK == MPCC1_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_TOP_POL_MASK, "MPC instance mask mismatch");
_Static_assert(MPCC0_MPCC_UPDATE_LOCK_SEL__MPCC_UPDATE_LOCK_SEL_MASK == MPCC1_MPCC_UPDATE_LOCK_SEL__MPCC_UPDATE_LOCK_SEL_MASK, "MPC lock mask mismatch");
static const unsigned long offsets[2 * W_COUNT] = {
    BC250_REG_DMU_MPCC0_MPCC_CONTROL,
    BC250_REG_DMU_MPCC0_MPCC_TOP_GAIN,
    BC250_REG_DMU_MPCC0_MPCC_BOT_GAIN_INSIDE,
    BC250_REG_DMU_MPCC0_MPCC_BOT_GAIN_OUTSIDE,
    BC250_REG_DMU_MPCC0_MPCC_BG_R_CR,
    BC250_REG_DMU_MPCC0_MPCC_BG_G_Y,
    BC250_REG_DMU_MPCC0_MPCC_BG_B_CB,
    BC250_REG_DMU_MPCC0_MPCC_TOP_SEL,
    BC250_REG_DMU_MPCC0_MPCC_BOT_SEL,
    BC250_REG_DMU_MPCC0_MPCC_OPP_ID,
    BC250_REG_DMU_MPCC0_MPCC_SM_CONTROL,
    BC250_REG_DMU_MPCC1_MPCC_CONTROL,
    BC250_REG_DMU_MPCC1_MPCC_TOP_GAIN,
    BC250_REG_DMU_MPCC1_MPCC_BOT_GAIN_INSIDE,
    BC250_REG_DMU_MPCC1_MPCC_BOT_GAIN_OUTSIDE,
    BC250_REG_DMU_MPCC1_MPCC_BG_R_CR,
    BC250_REG_DMU_MPCC1_MPCC_BG_G_Y,
    BC250_REG_DMU_MPCC1_MPCC_BG_B_CB,
    BC250_REG_DMU_MPCC1_MPCC_TOP_SEL,
    BC250_REG_DMU_MPCC1_MPCC_BOT_SEL,
    BC250_REG_DMU_MPCC1_MPCC_OPP_ID,
    BC250_REG_DMU_MPCC1_MPCC_SM_CONTROL
};
static const unsigned long masks[W_COUNT] = {
    MPCC0_MPCC_CONTROL__MPCC_MODE_MASK | MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE_MASK | MPCC0_MPCC_CONTROL__MPCC_ALPHA_MULTIPLIED_MODE_MASK | MPCC0_MPCC_CONTROL__MPCC_BLND_ACTIVE_OVERLAP_ONLY_MASK | MPCC0_MPCC_CONTROL__MPCC_BG_BPC_MASK | MPCC0_MPCC_CONTROL__MPCC_BOT_GAIN_MODE_MASK | MPCC0_MPCC_CONTROL__MPCC_GLOBAL_ALPHA_MASK | MPCC0_MPCC_CONTROL__MPCC_GLOBAL_GAIN_MASK,
    MPCC0_MPCC_TOP_GAIN__MPCC_TOP_GAIN_MASK,
    MPCC0_MPCC_BOT_GAIN_INSIDE__MPCC_BOT_GAIN_INSIDE_MASK,
    MPCC0_MPCC_BOT_GAIN_OUTSIDE__MPCC_BOT_GAIN_OUTSIDE_MASK,
    MPCC0_MPCC_BG_R_CR__MPCC_BG_R_CR_MASK,
    MPCC0_MPCC_BG_G_Y__MPCC_BG_G_Y_MASK,
    MPCC0_MPCC_BG_B_CB__MPCC_BG_B_CB_MASK,
    MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL_MASK,
    MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL_MASK,
    MPCC0_MPCC_OPP_ID__MPCC_OPP_ID_MASK,
    MPCC0_MPCC_SM_CONTROL__MPCC_SM_EN_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_MODE_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_FRAME_ALT_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_FIELD_ALT_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_FRAME_POL_MASK | MPCC0_MPCC_SM_CONTROL__MPCC_SM_FORCE_NEXT_TOP_POL_MASK
};
// Context needed by the unmodified AMD functions. REG_* captures prepared
// words in private memory; it never reads or writes hardware here.
struct mpcc_blnd_cfg {
    unsigned alpha_mode;
    bool pre_multiplied_alpha, overlap_only;
    unsigned global_alpha, global_gain, background_color_bpc, bottom_gain_mode;
    unsigned top_gain, bottom_inside_gain, bottom_outside_gain;
};
struct mpcc {
    int mpcc_id, dpp_id;
    struct mpcc* mpcc_bot;
    struct mpcc_blnd_cfg blnd_cfg;
    struct { bool enable; } sm_cfg;
    bool shared_bottom;
};
struct mpc { struct mpcc items[2]; };
struct dcn20_mpc { struct mpc base; BC250_MPC_PLAN* target; };
#define TO_DCN20_MPC(p) ((struct dcn20_mpc*)(p))
static struct mpcc* mpc1_get_mpcc(struct mpc* m, int id) { return &m->items[id]; }
static const unsigned MPCC_CONTROL[2] = {W_MPCC_CONTROL, W_COUNT + W_MPCC_CONTROL};
static const unsigned MPCC_TOP_GAIN[2] = {W_MPCC_TOP_GAIN, W_COUNT + W_MPCC_TOP_GAIN};
static const unsigned MPCC_BOT_GAIN_INSIDE[2] = {W_MPCC_BOT_GAIN_INSIDE, W_COUNT + W_MPCC_BOT_GAIN_INSIDE};
static const unsigned MPCC_BOT_GAIN_OUTSIDE[2] = {W_MPCC_BOT_GAIN_OUTSIDE, W_COUNT + W_MPCC_BOT_GAIN_OUTSIDE};
#define CF(field, value) Bc250DispField(MPCC0_MPCC_CONTROL__##field##_MASK, (unsigned long)(value))
#define REG_UPDATE_7(reg,a,av,b,bv,c,cv,d,dv,e,ev,f,fv,g,gv) \
    (mpc20->target->Words[(reg)] = CF(a,av)|CF(b,bv)|CF(c,cv)|CF(d,dv)|CF(e,ev)|CF(f,fv)|CF(g,gv))
#define REG_SET(reg,zero,field,value) \
    (mpc20->target->Words[(reg)] = Bc250DispField(MPCC0_##field##__##field##_MASK, (value)))
static void mpc2_update_blending(struct mpc*, struct mpcc_blnd_cfg*, int);
#include "../shim/generated/dc_mpc_blend.inc"
#undef REG_UPDATE_7
#undef REG_SET

int Bc250MpcPrepare(const BC250_MPC_REQUEST* Request, BC250_MPC_PLAN* Plan)
{
    BC250_MPC_REQUEST in;
    struct dcn20_mpc ctx = {0};
    unsigned p, top = 15, bottom = 15, enabled = 0;
    if (!Plan) return 0;
    if (Request) in = *Request;
    *Plan = (BC250_MPC_PLAN){0};
    if (!Request) return 0;
    for (p = 0; p < 2; ++p) {
        const BC250_MPC_PLANE* r = &in.Plane[p];
        // Validate even disabled planes; hiding one cannot hide malformed data.
        if (r->Enabled > 1 || r->ZOrder > 1 || r->PerPixelAlpha > 1 ||
            r->PremultipliedAlpha > 1 || r->GlobalAlphaEnabled > 1 || r->GlobalAlpha > 255) return 0;
        if (r->Enabled) { enabled |= 1u << p; top = p; }
    }
    if (enabled == 3) {
        if (in.Plane[0].ZOrder == in.Plane[1].ZOrder) return 0;
        top = in.Plane[1].ZOrder > in.Plane[0].ZOrder ? 1 : 0;
        bottom = top ^ 1u;
    }
    ctx.target = Plan;
    for (p = 0; p < 2; ++p) {
        const BC250_MPC_PLANE* r = &in.Plane[p];
        struct mpcc_blnd_cfg blend;
        unsigned base = p * W_COUNT;
        mpc201_init_mpcc(&ctx.base.items[p], (int)p);
        blend = ctx.base.items[p].blnd_cfg;
        // dcn20_update_mpcc alpha selection; preserve its DCN201 gain defaults.
        blend.alpha_mode = r->PerPixelAlpha ?
            (r->GlobalAlphaEnabled ? MPCC_ALPHA_BLEND_MODE_PER_PIXEL_ALPHA_COMBINED_GLOBAL_GAIN :
                                    MPCC_ALPHA_BLEND_MODE_PER_PIXEL_ALPHA) :
            MPCC_ALPHA_BLEND_MODE_GLOBAL_ALPHA;
        blend.pre_multiplied_alpha = r->PerPixelAlpha && r->PremultipliedAlpha;
        blend.global_alpha = r->GlobalAlphaEnabled ? r->GlobalAlpha : 255;
        blend.global_gain = r->PerPixelAlpha && r->GlobalAlphaEnabled ? r->GlobalAlpha : 255;
        mpc2_update_blending(&ctx.base, &blend, (int)p);
        Plan->Words[base + W_MPCC_CONTROL] |= CF(MPCC_MODE, !r->Enabled ? MPCC_BLEND_MODE_BYPASS :
            p == top && bottom != 15 ? MPCC_BLEND_MODE_TOP_BOT_BLENDING : MPCC_BLEND_MODE_TOP_LAYER_ONLY);
        Plan->Words[base + W_MPCC_TOP_SEL] = r->Enabled ? p : 15;
        Plan->Words[base + W_MPCC_BOT_SEL] = r->Enabled && p == top ? bottom : 15;
        Plan->Words[base + W_MPCC_OPP_ID] = r->Enabled ? 0 : 15;
        // Black (all zero) in MPC's 12-bit background; stereo mixing disabled.
    }
    Plan->Output = top; Plan->EnabledMask = enabled; Plan->Ready = 1;
    return 1;
}
#define TRY(x) do { long rc = (x); if (rc < 0) return rc; } while (0)
long Bc250MpcApplyLocked(BC250_DISP_IO* Io, const BC250_MPC_PLAN* Plan,
    unsigned OutputBlankConfirmed, unsigned* WritesAttempted)
{
    unsigned i;
    unsigned long value, control[2], stereo[2], mux;
    const unsigned long locks[2] = {BC250_REG_DMU_MPCC0_MPCC_UPDATE_LOCK_SEL, BC250_REG_DMU_MPCC1_MPCC_UPDATE_LOCK_SEL};
    const unsigned long held = OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                              OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
    if (WritesAttempted) *WritesAttempted = 0;
    if (!Io || !Plan || !WritesAttempted || Plan->Ready != 1 || Plan->EnabledMask > 3 ||
        (Plan->Output > 1 && Plan->Output != 15) || OutputBlankConfirmed > 1)
        return BC250_DISP_STATUS_INVALID;
    if (!Plan->EnabledMask && !OutputBlankConfirmed) return BC250_DISP_STATUS_MISMATCH;
    for (i = 0; i < 2 * W_COUNT; ++i)
        if (Plan->Words[i] & ~masks[i % W_COUNT]) return BC250_DISP_STATUS_INVALID;
    TRY(Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_CONTROL, &value));
    if (!(value & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK)) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0, &value));
    if (value & OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &value));
    if ((value & held) != held) return BC250_DISP_STATUS_MISMATCH;
    for (i = 0; i < 2; ++i) {
        TRY(Bc250DispRead(Io, locks[i], &value));
        if (value & MPCC0_MPCC_UPDATE_LOCK_SEL__MPCC_UPDATE_LOCK_SEL_MASK) return BC250_DISP_STATUS_MISMATCH;
        TRY(Bc250DispRead(Io, offsets[i * W_COUNT + W_MPCC_CONTROL], &control[i]));
        TRY(Bc250DispRead(Io, offsets[i * W_COUNT + W_MPCC_SM_CONTROL], &stereo[i]));
    }
    TRY(Bc250DispRead(Io, BC250_REG_DMU_MPC_OUT0_MUX, &mux));
    // All read failures precede every write. All fields commit under the
    // already armed locks; mux is last. Do not alter output rate-control bits.
    for (i = 0; i < 2 * W_COUNT; ++i) {
        value = Plan->Words[i];
        if (i % W_COUNT == W_MPCC_CONTROL) value |= control[i / W_COUNT] & ~masks[W_MPCC_CONTROL];
        if (i % W_COUNT == W_MPCC_SM_CONTROL) value |= stereo[i / W_COUNT] & ~masks[W_MPCC_SM_CONTROL];
        ++*WritesAttempted;
        TRY(Bc250DispWrite(Io, offsets[i], value));
    }
    ++*WritesAttempted;
    return Bc250DispWrite(Io, BC250_REG_DMU_MPC_OUT0_MUX,
        (mux & ~MPC_OUT0_MUX__MPC_OUT_MUX_MASK) | Bc250DispField(MPC_OUT0_MUX__MPC_OUT_MUX_MASK, Plan->Output));
}
