// PROVENANCE: Linux AMD display code (MIT), v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449:
// optc1_program_timing/get_otg_active_size, dcn201_clk_mgr_construct, and
// dce_clock_source get_pixel_clk_frequency_100hz. Readback only, no modeset writes.
#pragma once
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"

#include "display_timing_snapshot.h"

static NTSTATUS DisplayDecodeTiming(const ULONG Values[TimingCount], ULONG Width, ULONG Height,
                                    D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    ULONG htotal,vtotal,hstart,hend,vstart,vend;
    ULONGLONG referenceHz,pixel;
    // This inherited path supports the fixed progressive DP DTO mode only.
    // Other clock/interlace/DRR programming needs a separately validated decoder.
    if(!(Values[TimingControl]&OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) ||
       !(Values[TimingPixelControl]&OTG0_PIXEL_RATE_CNTL__DP_DTO0_ENABLE_MASK) ||
       (Values[TimingPixelControl]&(OTG0_PIXEL_RATE_CNTL__OTG0_DISPOUT_HALF_RATE_EN_MASK |
           OTG0_PIXEL_RATE_CNTL__OTG0_ADD_PIXEL_MASK | OTG0_PIXEL_RATE_CNTL__OTG0_DROP_PIXEL_MASK)) ||
       (Values[TimingInterlace]&OTG0_OTG_INTERLACE_CONTROL__OTG_INTERLACE_ENABLE_MASK) ||
       (Values[TimingVTotalControl]&(OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MIN_SEL_MASK |
           OTG0_OTG_V_TOTAL_CONTROL__OTG_V_TOTAL_MAX_SEL_MASK |
           OTG0_OTG_V_TOTAL_CONTROL__OTG_VTOTAL_MID_REPLACING_MAX_EN_MASK |
           OTG0_OTG_V_TOTAL_CONTROL__OTG_VTOTAL_MID_REPLACING_MIN_EN_MASK)))
        return STATUS_NOT_SUPPORTED;
    htotal=((Values[TimingHTotal]&OTG0_OTG_H_TOTAL__OTG_H_TOTAL_MASK)>>OTG0_OTG_H_TOTAL__OTG_H_TOTAL__SHIFT)+1;
    vtotal=((Values[TimingVTotal]&OTG0_OTG_V_TOTAL__OTG_V_TOTAL_MASK)>>OTG0_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT)+1;
    hstart=(Values[TimingHBlank]&OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START_MASK)>>OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_START__SHIFT;
    hend=(Values[TimingHBlank]&OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END_MASK)>>OTG0_OTG_H_BLANK_START_END__OTG_H_BLANK_END__SHIFT;
    vstart=(Values[TimingVBlank]&OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK)>>OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT;
    vend=(Values[TimingVBlank]&OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK)>>OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT;
    if(hstart>=htotal || hend>=hstart || vstart>=vtotal || vend>=vstart ||
       hstart-hend!=Width || vstart-vend!=Height || !Width || !Height)
        return STATUS_DEVICE_DATA_ERROR;
    // DCN201's reference counter is in 100 kHz units. Unlike assuming PHASE is
    // Hz, the ratio also handles a MODULO different from the reference clock.
    // Do not inherit Linux's nominal600MHz fallback if the counter reads zero.
    referenceHz=(ULONGLONG)Values[TimingReference]*100000u;
    if(!referenceHz || referenceHz>MAXULONG || !Values[TimingPhase] || !Values[TimingModulo])
        return STATUS_DEVICE_DATA_ERROR;
    pixel=((ULONGLONG)Values[TimingPhase]*referenceHz+Values[TimingModulo]/2)/Values[TimingModulo];
    // PixelRate is integral Hz. Round the DTO ratio to nearest Hz (<0.5 Hz error)
    // and express both scan frequencies as ratios using that same pixel rate.
    if(!pixel || pixel>=D3DKMDT_FREQUENCY_NOTSPECIFIED)return STATUS_DEVICE_DATA_ERROR;
    Signal->TotalSize.cx=htotal;Signal->TotalSize.cy=vtotal;
    Signal->ActiveSize.cx=Width;Signal->ActiveSize.cy=Height;
    Signal->PixelRate=(SIZE_T)pixel;
    Signal->HSyncFreq.Numerator=(UINT)pixel;Signal->HSyncFreq.Denominator=htotal;
    Signal->VSyncFreq.Numerator=(UINT)pixel;Signal->VSyncFreq.Denominator=htotal*vtotal;
    return STATUS_SUCCESS;
}

static NTSTATUS DisplayReadInheritedTiming(const BC250_DEVICE* Device, D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    ULONG first[TimingCount],second[TimingCount],i;
    NTSTATUS status;
    // VidPn mode queries are PASSIVE_LEVEL; PnP owns the MMIO mapping lifetime.
    // This driver preserves firmware timing. Refuse a changed snapshot rather
    // than combine totals and clock fields from different mode programming.
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    if(!Device->Mmio)return STATUS_DEVICE_NOT_READY;
    status=DisplayTimingSnapshot(Device,first);
    if(!NT_SUCCESS(status))return status;
    status=DisplayTimingSnapshot(Device,second);
    if(!NT_SUCCESS(status))return status;
    for(i=0;i<TimingCount;i++)if(first[i]!=second[i])return STATUS_DEVICE_BUSY;
    return DisplayDecodeTiming(first,Device->Post.Width,Device->Post.Height,Signal);
}
