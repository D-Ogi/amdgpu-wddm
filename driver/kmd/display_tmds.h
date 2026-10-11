// PROVENANCE: Linux AMD display (MIT), v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449:
// dcn10_link_encoder_setup/is_dig_enabled, enc1_dig_source_otg, optc1_get_vblank_counter.
// Read-only OTG0 -> stream encoder -> enabled TMDS back end. No PLL or PHY writes.
#pragma once
#include "edid.h"

enum { TmdsFe0, TmdsFe1, TmdsBe0, TmdsBe1, TmdsEnable0, TmdsEnable1, TmdsRouteCount };

static NTSTATUS DisplayTmdsRoute(const BC250_DEVICE* Device, ULONG Route[TmdsRouteCount])
{
    static const ULONG offsets[] = { BC250_REG_DMU_DIG0_DIG_FE_CNTL, BC250_REG_DMU_DIG1_DIG_FE_CNTL,
        BC250_REG_DMU_DIG0_DIG_BE_CNTL, BC250_REG_DMU_DIG1_DIG_BE_CNTL,
        BC250_REG_DMU_DIG0_DIG_BE_EN_CNTL, BC250_REG_DMU_DIG1_DIG_BE_EN_CNTL };
    ULONG i, matches=0;
    NTSTATUS status;
    for(i=0;i<TmdsRouteCount;i++) {
        status=MmioDcnRead(Device,offsets[i],&Route[i]);
        if(!NT_SUCCESS(status))return status;
    }
    for(i=0;i<2;i++) {
        ULONG be=Route[TmdsBe0+i], fe, source, mode;
        if(!(Route[TmdsEnable0+i]&DIG0_DIG_BE_EN_CNTL__DIG_ENABLE_MASK))continue;
        source=(be&DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK)>>DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT__SHIFT;
        // The front-end selection is a bit mask, not an index. Only a single
        // front end and a single-link output are admitted by this inherited path.
        if(source!=1 && source!=2)return STATUS_NOT_SUPPORTED;
        fe=source==1 ? Route[TmdsFe0] : Route[TmdsFe1];
        if((fe&DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT_MASK)!=0)continue; // another OTG
        mode=(be&DIG0_DIG_BE_CNTL__DIG_MODE_MASK)>>DIG0_DIG_BE_CNTL__DIG_MODE__SHIFT;
        if((mode!=2 && mode!=3) || (be&DIG0_DIG_BE_CNTL__DIG_DUAL_LINK_ENABLE_MASK))return STATUS_NOT_SUPPORTED;
        matches++;
    }
    return matches==1 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

// This stage has no DDC transport. Its caller supplies no DTD. Kept here so an
// eventual validated descriptor can refine the measurement without substituting
// the monitor's preferred mode for the mode the firmware actually programmed.
static ULONG DisplayTmdsPixel(ULONG Measured, ULONG Width, ULONG Height, ULONG HTotal, ULONG VTotal,
                              const BC250_EDID_TIMING* Dtd)
{
    ULONGLONG pixel, difference;
    if(Dtd && !(Dtd->Flags&0x80u) && Dtd->HActive==Width && Dtd->VActive==Height &&
       (ULONGLONG)Dtd->HActive+Dtd->HBlank==HTotal && (ULONGLONG)Dtd->VActive+Dtd->VBlank==VTotal) {
        pixel=(ULONGLONG)Dtd->PixelClock10Khz*10000u;
        difference=pixel>Measured ? pixel-Measured : Measured-pixel;
        if(pixel && pixel<D3DKMDT_FREQUENCY_NOTSPECIFIED && difference*200u<=Measured)return (ULONG)pixel;
    }
    // One immutable 10 kHz value for this start, never a new value per query.
    pixel=((ULONGLONG)Measured+5000u)/10000u*10000u;
    return pixel && pixel<D3DKMDT_FREQUENCY_NOTSPECIFIED ? (ULONG)pixel : 0;
}

// Nine observed edges delimit eight whole frames, in two equal halves. Edge
// timestamps are bracketed by QPC reads, so a scheduler interruption cannot
// silently become a precise-looking refresh estimate. Frame counter wrap is
// 24-bit (dcn_2_0_1_sh_mask.h). A skipped edge or stalled clock fails closed.
// PASSIVE_LEVEL, no locks. Ten-us stalls only during this one bounded startup
// measurement; the QPC deadline and an independent poll cap both stop it.
static NTSTATUS DisplayMeasureTmds(const BC250_DEVICE* Device, ULONG HTotal, ULONG VTotal,
                                  ULONG* Pixel, ULONG* RefreshMilliHz)
{
    LARGE_INTEGER frequency, stamp;
    ULONGLONG start, before, after, previousBefore, edge[3]={0}, uncertainty[3]={0};
    ULONGLONG half0,half1,difference,span,framePixels,product,measured,rate;
    ULONG previous, current, edges=0, poll;
    NTSTATUS status;
    *Pixel=*RefreshMilliHz=0;
    stamp=KeQueryPerformanceCounter(&frequency);
    if(frequency.QuadPart<=0 || frequency.QuadPart>10000000000ll || stamp.QuadPart<0)
        return STATUS_DEVICE_DATA_ERROR;
    start=previousBefore=(ULONGLONG)stamp.QuadPart;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT,&previous);
    if(!NT_SUCCESS(status))return status;
    previous&=OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
    for(poll=0;poll<50000;poll++) {
        KeStallExecutionProcessor(10);
        before=(ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT,&current);
        if(!NT_SUCCESS(status))return status;
        after=(ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
        if(before<previousBefore || after<before || after-start>(ULONGLONG)frequency.QuadPart/2)
            return STATUS_DEVICE_DATA_ERROR;
        current&=OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
        if(current!=previous) {
            ULONG delta=(current-previous)&OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
            if(delta!=1)return STATUS_DEVICE_DATA_ERROR;
            if(edges==0 || edges==4 || edges==8) {
                ULONG slot=edges/4;
                uncertainty[slot]=after-previousBefore;
                edge[slot]=previousBefore+uncertainty[slot]/2;
            }
            edges++;
            if(edges==9)break;
        }
        previous=current;previousBefore=before;
    }
    if(edges!=9 || edge[1]<=edge[0] || edge[2]<=edge[1])return STATUS_DEVICE_DATA_ERROR;
    half0=edge[1]-edge[0];half1=edge[2]-edge[1];span=edge[2]-edge[0];
    difference=half0>half1 ? half0-half1 : half1-half0;
    // Less than 0.5% disagreement between halves; uncertainty below 0.25%
    // per half, including both of that half's timestamp brackets.
    if(difference>(half0<half1 ? half0 : half1)/200u ||
       uncertainty[0]+uncertainty[1]>half0/400u || uncertainty[1]+uncertainty[2]>half1/400u)
        return STATUS_DEVICE_DATA_ERROR;
    rate=(ULONGLONG)frequency.QuadPart*8u;
    if(rate<span*23u || rate>span*240u)return STATUS_NOT_SUPPORTED;
    framePixels=(ULONGLONG)HTotal*VTotal;
    if(!framePixels || framePixels>(~(ULONGLONG)0-span/2)/rate)return STATUS_DEVICE_DATA_ERROR;
    product=framePixels*rate;
    measured=(product+span/2)/span;
    if(!measured || measured>=D3DKMDT_FREQUENCY_NOTSPECIFIED)return STATUS_DEVICE_DATA_ERROR;
    *Pixel=(ULONG)measured;
    *RefreshMilliHz=(ULONG)((rate*1000u+span/2)/span);
    return STATUS_SUCCESS;
}
