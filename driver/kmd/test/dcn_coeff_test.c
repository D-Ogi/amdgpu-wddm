#include "../dcn_coeff.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
typedef struct MODEL {
    unsigned long mode[2], select[2], power[2], power_status[2], lock, otg, global;
    uint16_t coef[2][2][4][33][8];
    unsigned reads, writes, fail_read, fail_write, fail_after, stalls;
} MODEL;
static MODEL hw;
static const unsigned long mode_reg[] = {BC250_REG_DMU_DSCL0_SCL_MODE, BC250_REG_DMU_DSCL1_SCL_MODE};
static const unsigned long select_reg[] = {BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT, BC250_REG_DMU_DSCL1_SCL_COEF_RAM_TAP_SELECT};
static const unsigned long data_reg[] = {BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_DATA, BC250_REG_DMU_DSCL1_SCL_COEF_RAM_TAP_DATA};
static const unsigned long power_reg[] = {BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL, BC250_REG_DMU_DSCL1_DSCL_MEM_PWR_CTRL};
static const unsigned long power_status_reg[] = {BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_STATUS, BC250_REG_DMU_DSCL1_DSCL_MEM_PWR_STATUS};

static long Read(void* context, unsigned long reg, unsigned long* value)
{
    unsigned p;
    (void)context;
    if (++hw.reads == hw.fail_read) return BC250_DISP_STATUS_DEVICE;
    if (reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) { *value = hw.lock; return 0; }
    if (reg == BC250_REG_DMU_OTG0_OTG_CONTROL) { *value = hw.otg; return 0; }
    if (reg == BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0) { *value = hw.global; return 0; }
    for (p = 0; p < 2; ++p) {
        if (reg == mode_reg[p]) { *value = hw.mode[p]; return 0; }
        if (reg == power_reg[p]) { *value = hw.power[p]; return 0; }
        if (reg == power_status_reg[p]) { *value = hw.power_status[p]; return 0; }
    }
    CHECK(0); return BC250_DISP_STATUS_ACCESS_DENIED;
}

static long Write(void* context, unsigned long reg, unsigned long value)
{
    unsigned p;
    int fail;
    (void)context;
    fail = ++hw.writes == hw.fail_write;
    if (fail && !hw.fail_after) return BC250_DISP_STATUS_DEVICE;
    for (p = 0; p < 2; ++p) {
        if (reg == mode_reg[p]) {
            hw.mode[p] = (value & ~DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK) |
                (hw.mode[p] & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK);
            return fail ? BC250_DISP_STATUS_DEVICE : 0;
        }
        if (reg == select_reg[p]) {
            hw.select[p] = value;
            return fail ? BC250_DISP_STATUS_DEVICE : 0;
        }
        if (reg == data_reg[p]) {
            unsigned type = (unsigned)Bc250DispGet(hw.select[p], DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_FILTER_TYPE_MASK);
            unsigned phase = (unsigned)Bc250DispGet(hw.select[p], DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_PHASE_MASK);
            unsigned pair = (unsigned)Bc250DispGet(hw.select[p], DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_TAP_PAIR_IDX_MASK);
            unsigned bank = !(hw.mode[p] & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK);
            if (type >= 4 || phase >= 33 || pair >= 4) { CHECK(0); return BC250_DISP_STATUS_INVALID; }
            hw.coef[p][bank][type][phase][pair * 2] = (uint16_t)Bc250DispGet(value, DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_EVEN_TAP_COEF_MASK);
            hw.coef[p][bank][type][phase][pair * 2 + 1] = (uint16_t)Bc250DispGet(value, DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_ODD_TAP_COEF_MASK);
            // Deliberately incompatible auto-advance: current hardware has two
            // taps, while the prepared plan has eight. Each next explicit
            // selector must make this auto-increment state irrelevant.
            hw.select[p] += Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_PHASE_MASK, 1);
            return fail ? BC250_DISP_STATUS_DEVICE : 0;
        }
    }
    CHECK(0); return BC250_DISP_STATUS_ACCESS_DENIED;
}
static void Stall(void* c, unsigned long us) { (void)c; (void)us; ++hw.stalls; }
static void Setup(BC250_DISP_IO* io, unsigned current)
{
    memset(&hw, 0, sizeof(hw)); memset(io, 0, sizeof(*io));
    memset(hw.coef, 0x5a, sizeof(hw.coef));
    hw.otg = OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK;
    hw.lock = OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
              OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
    hw.mode[0] = hw.mode[1] = 1 | (current ? DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK |
                                                 DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK : 0);
    io->Read = Read; io->Write = Write; io->Stall = Stall;
}
static void Latch(unsigned pipe)
{
    hw.lock = 0;
    hw.mode[pipe] &= ~DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK;
    if (hw.mode[pipe] & DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK)
        hw.mode[pipe] |= DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK;
}

static void Lifecycle(const BC250_SCALER_PREPARED* plan)
{
    unsigned pipe, current, phase, tap, type;
    const BC250_SCALER_FILTER_PLAN* filters[] = {&plan->LumaV, &plan->LumaH, &plan->ChromaV, &plan->ChromaH};
    for (pipe = 0; pipe < 2; ++pipe) for (current = 0; current < 2; ++current) {
        BC250_DISP_IO io;
        BC250_COEF_STATE s = {0};
        uint64_t ticket, next;
        Setup(&io, current);
        CHECK(Bc250CoefStageLocked(&io, &s, pipe, 1, plan, &ticket) == 0 && ticket == 1);
        CHECK(s.Phase == BC250_COEF_READY && hw.writes == 1056 && hw.stalls == 0);
        CHECK(hw.mode[pipe] == (1ul | (current ? 0x1100ul : 0ul)));
        for (type = 0; type < 4; ++type) for (phase = 0; phase < 33; ++phase) for (tap = 0; tap < 8; ++tap) {
            CHECK(hw.coef[pipe][!current][type][phase][tap] == filters[type]->Coefficients[phase * 8 + tap]);
            CHECK(hw.coef[pipe][current][type][phase][tap] == 0x5a5a);
            CHECK(hw.coef[!pipe][current][type][phase][tap] == 0x5a5a);
        }
        CHECK(Bc250CoefStageLocked(&io, &s, pipe, 1, plan, &next) == BC250_DISP_STATUS_BUSY && next == 0);
        CHECK(Bc250CoefSelectLocked(&io, &s, ticket + 1) == BC250_DISP_STATUS_INVALID);
        CHECK(Bc250CoefSelectLocked(&io, &s, ticket) == 0 && s.Phase == BC250_COEF_SUBMITTED);
        CHECK(Bc250CoefCancel(&s, ticket) == BC250_DISP_STATUS_BUSY);
        CHECK(Bc250CoefObserve(&io, &s, ticket) == BC250_DISP_STATUS_BUSY);
        hw.lock = 0;
        CHECK(Bc250CoefObserve(&io, &s, ticket) == BC250_DISP_STATUS_BUSY);
        Latch(pipe);
        CHECK(Bc250CoefObserve(&io, &s, ticket) == 0 && s.Phase == BC250_COEF_IDLE);
        Setup(&io, !current);
        CHECK(Bc250CoefStageLocked(&io, &s, pipe, 1, plan, &next) == 0 && next > ticket);
        CHECK(Bc250CoefCancel(&s, ticket) == BC250_DISP_STATUS_INVALID);
        CHECK(Bc250CoefCancel(&s, next) == 0);
        CHECK(Bc250CoefSelectLocked(&io, &s, next) == BC250_DISP_STATUS_BUSY);
        CHECK(Bc250CoefInvalidateQuiesced(&s) == 0 && s.Generation > next);
        s.Generation = UINT64_MAX;
        CHECK(Bc250CoefInvalidateQuiesced(&s) == BC250_DISP_STATUS_BUDGET);
        CHECK(s.Phase == BC250_COEF_POISONED);
    }
}

static void Failures(const BC250_SCALER_PREPARED* plan)
{
    unsigned fail, after, pipe;
    for (pipe = 0; pipe < 2; ++pipe) for (after = 0; after < 2; ++after)
    for (fail = 1; fail <= 1056; ++fail) {
        BC250_DISP_IO io;
        BC250_COEF_STATE s = {0};
        uint64_t ticket = 99;
        Setup(&io, 0); hw.fail_write = fail; hw.fail_after = after;
        CHECK(Bc250CoefStageLocked(&io, &s, pipe, 1, plan, &ticket) == BC250_DISP_STATUS_DEVICE);
        CHECK(ticket == 0 && s.Phase == BC250_COEF_POISONED && hw.writes == fail);
        CHECK(hw.mode[pipe] == 1 && hw.stalls == 0);
        CHECK(Bc250CoefSelectLocked(&io, &s, s.Generation) == BC250_DISP_STATUS_BUSY);
    }
    for (fail = 1; fail <= 7; ++fail) {
        BC250_DISP_IO io;
        BC250_COEF_STATE s = {0};
        uint64_t ticket = 99;
        Setup(&io, 0); hw.fail_read = fail;
        CHECK(Bc250CoefStageLocked(&io, &s, 1, 1, plan, &ticket) == BC250_DISP_STATUS_DEVICE);
        CHECK(ticket == 0 && (fail == 7 ? s.Phase == BC250_COEF_POISONED : hw.writes == 0));
    }
    for (after = 0; after < 2; ++after) {
        BC250_DISP_IO io;
        BC250_COEF_STATE s = {0};
        uint64_t ticket;
        Setup(&io, 0);
        CHECK(Bc250CoefStageLocked(&io, &s, 1, 1, plan, &ticket) == 0);
        hw.fail_write = hw.writes + 1; hw.fail_after = after;
        CHECK(Bc250CoefSelectLocked(&io, &s, ticket) == BC250_DISP_STATUS_DEVICE);
        CHECK(s.Phase == BC250_COEF_POISONED);
        Latch(1);
        CHECK(Bc250CoefObserve(&io, &s, ticket) == BC250_DISP_STATUS_BUSY);
    }
}

static void Preconditions(const BC250_SCALER_PREPARED* p)
{
    BC250_DISP_IO io;
    BC250_COEF_STATE s = {0};
    BC250_SCALER_PREPARED bad = *p;
    uint64_t ticket;
    unsigned gate;
    for (gate = 0; gate < 6; ++gate) {
        Setup(&io, 0);
        if (gate == 0) hw.otg = 0;
        if (gate == 1) hw.global = OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK;
        if (gate == 2) hw.lock = 0;
        if (gate == 3) hw.power[1] = DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK;
        if (gate == 4) hw.power_status[1] = DSCL0_DSCL_MEM_PWR_STATUS__LUT_MEM_PWR_STATE_MASK;
        if (gate == 5) hw.mode[1] |= DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK;
        CHECK(Bc250CoefStageLocked(&io, &s, 1, 1, p, &ticket) < 0 && ticket == 0);
        CHECK(hw.writes == 0 && s.Phase == BC250_COEF_IDLE);
    }
    Setup(&io, 0); bad.LumaV.CoefficientCount++;
    CHECK(Bc250CoefStageLocked(&io, &s, 0, 0, &bad, &ticket) == BC250_DISP_STATUS_INVALID);
    CHECK(hw.reads == 0 && hw.writes == 0);
    bad = *p; bad.LumaV.Coefficients[0] |= 1;
    CHECK(Bc250CoefStageLocked(&io, &s, 0, 0, &bad, &ticket) == BC250_DISP_STATUS_INVALID);
    CHECK(Bc250CoefStageLocked(&io, &s, 2, 0, p, &ticket) == BC250_DISP_STATUS_INVALID);
}

int main(void)
{
    BC250_SCALER_PREPARED plan = {0};
    CHECK(Bc250ScalerFilterPlan(0, 8, 2u << 19, &plan.LumaH));
    plan.LumaV = plan.ChromaH = plan.ChromaV = plan.LumaH;
    Lifecycle(&plan); Failures(&plan); Preconditions(&plan);
    printf("coefficient ownership: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
