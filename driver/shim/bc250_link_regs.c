// PROVENANCE: AMD Linux 7d0a66e4 register names/layouts, MIT.
#include "bc250_link_regs.h"
#include "bc250_hdmi_link.h"
#include <stddef.h>
#include "link_control_masks.inc"
typedef struct LINK_REGISTER { uint32_t Dword; int Writable; } LINK_REGISTER;
static const LINK_REGISTER Registers[] = {
#include "link_registers.inc"
};
int Bc250LinkRegisterAllowed(uint32_t dword, int write)
{
    size_t i;
    if (Bc250HdmiLinkAllowed(dword, write)) return 1;
    for (i=0;i<sizeof(Registers)/sizeof(Registers[0]);++i)
        if (Registers[i].Dword==dword) return !write || Registers[i].Writable;
    return 0;
}
int Bc250LinkRegisterWriteNeedsBefore(uint32_t dword)
{
    return dword==LINK_OTG_CONTROL || dword==LINK_OTG_COUNT_CONTROL || dword==LINK_VTG_CONTROL;
}
int Bc250LinkRegisterWriteValueAllowed(uint32_t dword, uint32_t before, uint32_t value)
{
    uint32_t mask;
    if (!Bc250LinkRegisterAllowed(dword,1)) return 0;
    if (dword==LINK_OTG_CONTROL) {
        mask=LINK_OTG_MASTER_EN|LINK_OTG_DISABLE_POINT_CNTL;
        if (value&LINK_OTG_MASTER_EN) return 0;
        if (((before^value)&LINK_OTG_DISABLE_POINT_CNTL) &&
            (value&LINK_OTG_DISABLE_POINT_CNTL)!=LINK_OTG_DISABLE_POINT_CNTL) return 0;
    } else if (dword==LINK_OTG_COUNT_CONTROL) {
        mask=LINK_OTG_HORZ_COUNT_BY2_EN|LINK_OTG_HORZ_REPETITION_COUNT;
        if (value&mask) return 0;
    } else if (dword==LINK_VTG_CONTROL) {
        mask=LINK_VTG_ENABLE;
        if (value&mask) return 0;
    } else return 1;
    // Nested ATOM clock disable may only clear the named controls. Preserve
    // status, reserved and unrelated timing fields, including nonzero values.
    return ((before^value)&~mask)==0;
}

int Bc250LinkRegisterPrepareWrite(uint32_t dword, uint32_t before,
    uint32_t requested, uint32_t* effective, uint32_t* reason)
{
    uint32_t value = requested, controls = 0, coarse = 0, normalized = 0;
    if (effective) *effective = 0;
    if (reason) *reason = BC250_LINK_WRITE_INVALID_OUTPUT;
    if (!effective || !reason) return 0;
    if (!Bc250LinkRegisterAllowed(dword, 1)) {
        *reason = BC250_LINK_WRITE_REFUSED_ADDRESS;
        return 0;
    }
    if (dword == LINK_OTG_CONTROL) {
        controls = LINK_OTG_MASTER_EN | LINK_OTG_DISABLE_POINT_CNTL;
        coarse = UINT32_C(0xff) & ~LINK_OTG_MASTER_EN;
        /* CURRENT_MASTER_EN_STATE is read-only and may change between the
         * interpreter RMW read and this callback's current read. */
        if ((before ^ value) & LINK_OTG_CURRENT_MASTER_EN_STATE) {
            value = (value & ~LINK_OTG_CURRENT_MASTER_EN_STATE) |
                    (before & LINK_OTG_CURRENT_MASTER_EN_STATE);
            normalized |= BC250_LINK_WRITE_READONLY_STATUS;
        }
    } else if (dword == LINK_OTG_COUNT_CONTROL) {
        controls = LINK_OTG_HORZ_COUNT_BY2_EN | LINK_OTG_HORZ_REPETITION_COUNT;
        coarse = UINT32_C(0xff) & ~controls;
    } else if (dword == LINK_VTG_CONTROL) {
        controls = LINK_VTG_ENABLE;
    } else {
        *effective = value;
        *reason = BC250_LINK_WRITE_UNCHANGED;
        return 1;
    }
    /* The retained ROM's enablecrtc(disable) clears an entire low byte.
     * Translate only that exact clear footprint to named fields. Linux AMD
     * optc1_disable_crtc (7d0a66e4, dcn10_optc.c:554-574) uses field RMW;
     * DCN201 masks define no writable low-byte OTG fields except MASTER_EN.
     * A partial byte clear, unknown set or unrelated change remains refused. */
    if (coarse && !(requested & UINT32_C(0xff)) &&
        !((before ^ value) & ~(coarse | controls))) {
        if ((before ^ value) & coarse) normalized |= BC250_LINK_WRITE_COARSE_CLEAR;
        value = (value & ~coarse) | (before & coarse);
    }
    if ((before ^ value) & ~controls) {
        *reason = BC250_LINK_WRITE_REFUSED_UNKNOWN;
        return 0;
    }
    if (!Bc250LinkRegisterWriteValueAllowed(dword, before, value)) {
        *reason = BC250_LINK_WRITE_REFUSED_CONTROL;
        return 0;
    }
    *effective = value;
    *reason = normalized;
    return 1;
}
