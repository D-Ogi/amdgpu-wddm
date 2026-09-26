// Read-only inherited timing tuple shared by the decoder and named diagnostics.
#pragma once
#include "regs.generated.h"

enum { TimingControl, TimingHTotal, TimingVTotal, TimingHBlank, TimingVBlank,
    TimingPixelControl, TimingPhase, TimingModulo, TimingInterlace, TimingVTotalControl,
    TimingReference, TimingCount };

static NTSTATUS DisplayTimingSnapshot(const BC250_DEVICE* Device, ULONG Values[TimingCount])
{
    static const ULONG offsets[] = {
        BC250_REG_DMU_OTG0_OTG_CONTROL, BC250_REG_DMU_OTG0_OTG_H_TOTAL,
        BC250_REG_DMU_OTG0_OTG_V_TOTAL, BC250_REG_DMU_OTG0_OTG_H_BLANK_START_END,
        BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END, BC250_REG_DMU_OTG0_PIXEL_RATE_CNTL,
        BC250_REG_DMU_DP_DTO0_PHASE, BC250_REG_DMU_DP_DTO0_MODULO,
        BC250_REG_DMU_OTG0_OTG_INTERLACE_CONTROL, BC250_REG_DMU_OTG0_OTG_V_TOTAL_CONTROL
    };
    ULONG i;
    NTSTATUS status;
    for(i=0;i<RTL_NUMBER_OF(offsets);i++) {
        status=MmioDcnRead(Device,offsets[i],&Values[i]);
        if(!NT_SUCCESS(status))return status;
    }
    return MmioRead(Device,BC250_REG_CLK_CLK4_0_CLK4_CLK2_CURRENT_CNT,&Values[TimingReference]);
}

