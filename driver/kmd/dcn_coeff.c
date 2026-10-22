// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, v6.18 7d0a66e4bb90,
// dcn10_dpp_dscl.c dpp1_dscl_set_scaler_filter / dpp1_dscl_set_scl_filter.
// Deviation: explicit phase/pair selectors, checked failure and bank ownership.
#include "dcn_coeff.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stddef.h>

typedef struct COEF_REGS {
    unsigned long Select, Data, Mode, Power, PowerStatus;
} COEF_REGS;
static const COEF_REGS registers[2] = {
    {BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT, BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_DATA,
     BC250_REG_DMU_DSCL0_SCL_MODE, BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL,
     BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_STATUS},
    {BC250_REG_DMU_DSCL1_SCL_COEF_RAM_TAP_SELECT, BC250_REG_DMU_DSCL1_SCL_COEF_RAM_TAP_DATA,
     BC250_REG_DMU_DSCL1_SCL_MODE, BC250_REG_DMU_DSCL1_DSCL_MEM_PWR_CTRL,
     BC250_REG_DMU_DSCL1_DSCL_MEM_PWR_STATUS}
};
#define TRY(x) do { long stepStatus = (x); if (stepStatus < 0) return stepStatus; } while (0)
// DCN201 fields are shared across pipe instances. Fail compilation on drift.
#define SAME(reg, field) _Static_assert(DSCL0_##reg##__##field##_MASK == \
    DSCL1_##reg##__##field##_MASK, "DSCL field differs across instances")
SAME(SCL_COEF_RAM_TAP_SELECT, SCL_COEF_RAM_FILTER_TYPE);
SAME(SCL_COEF_RAM_TAP_SELECT, SCL_COEF_RAM_PHASE);
SAME(SCL_COEF_RAM_TAP_SELECT, SCL_COEF_RAM_TAP_PAIR_IDX);
SAME(SCL_COEF_RAM_TAP_DATA, SCL_COEF_RAM_EVEN_TAP_COEF);
SAME(SCL_COEF_RAM_TAP_DATA, SCL_COEF_RAM_ODD_TAP_COEF);
SAME(SCL_COEF_RAM_TAP_DATA, SCL_COEF_RAM_EVEN_TAP_COEF_EN);
SAME(SCL_COEF_RAM_TAP_DATA, SCL_COEF_RAM_ODD_TAP_COEF_EN);
SAME(SCL_MODE, SCL_COEF_RAM_SELECT);
SAME(SCL_MODE, SCL_COEF_RAM_SELECT_CURRENT);
SAME(SCL_MODE, SCL_CHROMA_COEF_MODE);
SAME(DSCL_MEM_PWR_CTRL, LUT_MEM_PWR_FORCE);
SAME(DSCL_MEM_PWR_STATUS, LUT_MEM_PWR_STATE);

static long Held(BC250_DISP_IO* io)
{
    unsigned long value;
    const unsigned long held = OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                              OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
    TRY(Bc250DispRead(io, BC250_REG_DMU_OTG0_OTG_CONTROL, &value));
    if (!(value & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK)) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(io, BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0, &value));
    if (value & OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &value));
    return (value & held) == held ? 0 : BC250_DISP_STATUS_MISMATCH;
}

static unsigned Current(unsigned long mode)
{
    return (unsigned)Bc250DispGet(mode, DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK);
}
static unsigned Selected(unsigned long mode)
{
    return (unsigned)Bc250DispGet(mode, DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK);
}
static int Valid(const BC250_SCALER_FILTER_PLAN* p)
{
    unsigned i, stride;
    if (p->Taps < 1 || p->Taps > 8) return 0;
    stride = p->Taps == 1 ? 0 : ((p->Taps + 1) / 2) * 2;
    if (p->PairsPerPhase != stride / 2 || p->CoefficientCount != 33 * stride) return 0;
    for (i = 0; i < 264; ++i) {
        if (i >= p->CoefficientCount) { if (p->Coefficients[i]) return 0; }
        else {
            if (p->Coefficients[i] > 0x3fffu || (p->Coefficients[i] & 3u)) return 0;
            if ((p->Taps & 1u) && i % stride == p->Taps && p->Coefficients[i]) return 0;
        }
    }
    return 1;
}

static long Upload(BC250_DISP_IO* io, const COEF_REGS* r,
                   unsigned type, const BC250_SCALER_FILTER_PLAN* p)
{
    unsigned phase, pair;
    for (phase = 0; phase < 33; ++phase)
        for (pair = 0; pair < p->PairsPerPhase; ++pair) {
            unsigned i = (phase * p->PairsPerPhase + pair) * 2;
            unsigned long select = Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_FILTER_TYPE_MASK, type) |
                Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_PHASE_MASK, phase) |
                Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_TAP_PAIR_IDX_MASK, pair);
            unsigned long data = Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_EVEN_TAP_COEF_MASK, p->Coefficients[i]) |
                Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_ODD_TAP_COEF_MASK, p->Coefficients[i + 1]) |
                DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_EVEN_TAP_COEF_EN_MASK |
                DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_ODD_TAP_COEF_EN_MASK;
            TRY(Bc250DispWrite(io, r->Select, select));
            TRY(Bc250DispWrite(io, r->Data, data));
        }
    return 0;
}

long Bc250CoefStageLocked(BC250_DISP_IO* io, BC250_COEF_STATE* s,
    unsigned pipe, unsigned chroma, const BC250_SCALER_PREPARED* p, uint64_t* ticket)
{
    unsigned long before, after, power;
    const COEF_REGS* r;
    long rc;
    if (ticket) *ticket = 0;
    if (!io || !s || !p || !ticket || pipe > 1 || chroma > 1 ||
        !Valid(&p->LumaH) || !Valid(&p->LumaV) || !Valid(&p->ChromaH) || !Valid(&p->ChromaV))
        return BC250_DISP_STATUS_INVALID;
    if (s->Phase != BC250_COEF_IDLE) return BC250_DISP_STATUS_BUSY;
    if (s->Generation == UINT64_MAX) return BC250_DISP_STATUS_BUDGET;
    r = &registers[pipe];
    TRY(Held(io));
    TRY(Bc250DispRead(io, r->Power, &power));
    if (power & DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(io, r->PowerStatus, &power));
    if (power & DSCL0_DSCL_MEM_PWR_STATUS__LUT_MEM_PWR_STATE_MASK) return BC250_DISP_STATUS_MISMATCH;
    TRY(Bc250DispRead(io, r->Mode, &before));
    if (Current(before) != Selected(before)) return BC250_DISP_STATUS_BUSY;
    ++s->Generation;
    s->Pipe = pipe; s->ChromaMode = chroma;
    s->CurrentBank = Current(before); s->TargetBank = !s->CurrentBank;
    // A partial data-port write is never reusable as a ready bank.
    s->Phase = BC250_COEF_POISONED;
    rc = Upload(io, r, 1, &p->LumaH); // AMD SCL_COEF_LUMA_HORZ_FILTER
    if (rc >= 0) rc = Upload(io, r, 0, &p->LumaV);
    if (rc >= 0 && chroma) rc = Upload(io, r, 3, &p->ChromaH);
    if (rc >= 0 && chroma) rc = Upload(io, r, 2, &p->ChromaV);
    if (rc < 0) return rc;
    TRY(Bc250DispRead(io, r->Mode, &after));
    if (Current(after) != s->CurrentBank || Selected(after) != s->CurrentBank)
        return BC250_DISP_STATUS_MISMATCH;
    s->Phase = BC250_COEF_READY;
    *ticket = s->Generation;
    return 0;
}

long Bc250CoefSelectLocked(BC250_DISP_IO* io, BC250_COEF_STATE* s, uint64_t ticket)
{
    unsigned long mode;
    long rc;
    if (!io || !s || !ticket || ticket != s->Generation || s->Pipe > 1) return BC250_DISP_STATUS_INVALID;
    if (s->Phase != BC250_COEF_READY) return BC250_DISP_STATUS_BUSY;
    TRY(Held(io));
    TRY(Bc250DispRead(io, registers[s->Pipe].Mode, &mode));
    if (Current(mode) != s->CurrentBank || Selected(mode) != s->CurrentBank) {
        s->Phase = BC250_COEF_POISONED;
        return BC250_DISP_STATUS_MISMATCH;
    }
    mode &= ~(DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK | DSCL0_SCL_MODE__SCL_CHROMA_COEF_MODE_MASK);
    mode |= Bc250DispField(DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK, s->TargetBank) |
            Bc250DispField(DSCL0_SCL_MODE__SCL_CHROMA_COEF_MODE_MASK, s->ChromaMode);
    s->Phase = BC250_COEF_POISONED;
    rc = Bc250DispWrite(io, registers[s->Pipe].Mode, mode);
    if (rc >= 0) s->Phase = BC250_COEF_SUBMITTED;
    return rc;
}

long Bc250CoefObserve(BC250_DISP_IO* io, BC250_COEF_STATE* s, uint64_t ticket)
{
    unsigned long mode, lock;
    if (!io || !s || !ticket || ticket != s->Generation || s->Pipe > 1) return BC250_DISP_STATUS_INVALID;
    if (s->Phase != BC250_COEF_SUBMITTED) return BC250_DISP_STATUS_BUSY;
    TRY(Bc250DispRead(io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &lock));
    if (lock & (OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK)) return BC250_DISP_STATUS_BUSY;
    TRY(Bc250DispRead(io, registers[s->Pipe].Mode, &mode));
    if (Selected(mode) != s->TargetBank) {
        s->Phase = BC250_COEF_POISONED;
        return BC250_DISP_STATUS_MISMATCH;
    }
    if (Current(mode) != s->TargetBank) return BC250_DISP_STATUS_BUSY;
    s->Phase = BC250_COEF_IDLE;
    return 0;
}

long Bc250CoefCancel(BC250_COEF_STATE* s, uint64_t ticket)
{
    if (!s || !ticket || ticket != s->Generation) return BC250_DISP_STATUS_INVALID;
    if (s->Phase != BC250_COEF_READY) return BC250_DISP_STATUS_BUSY;
    s->Phase = BC250_COEF_IDLE;
    return 0;
}

long Bc250CoefInvalidateQuiesced(BC250_COEF_STATE* s)
{
    if (!s) return BC250_DISP_STATUS_INVALID;
    s->Phase = BC250_COEF_POISONED;
    if (s->Generation == UINT64_MAX) return BC250_DISP_STATUS_BUDGET;
    ++s->Generation;
    s->Phase = BC250_COEF_IDLE;
    return 0;
}
