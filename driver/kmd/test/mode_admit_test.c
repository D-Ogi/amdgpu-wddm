// BD-139: the timings of the field report and of the lab monitors through the real admission chain behind
// connector_link.c's Plan: the EDID gate (edid.c), the DP rate ladder (bc250_dp_train.c), the DCN 2.0.1 display plan
// with the input Plan builds (bc250_dml201.c) and the OPTC gate (bc250_optc.c). connector_link_test.c fakes the last
// three; here they run as the miniport compiles them, so each timing either passes or names the term that refuses it.
// The clocks are unit A's (DCHUB reference 48 MHz, DENTIST VCO 3 GHz, as dcn201_pipe_test.c reads them back).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../edid.h"
#include "../../shim/bc250_dml201.h"
#include "../../shim/bc250_optc.h"
#include "../../shim/bc250_dp_train.h"
#include "../../shim/dpcd_constants.inc"
#include "edid_lab_redacted.h"
static unsigned checks,failures;
#define CHECK(x) do { ++checks;if(!(x)) {++failures;printf("FAIL %u: %s\n",(unsigned)__LINE__,#x);} } while(0)
typedef struct RESULT { int Dml,Optc;unsigned Rate;BC250_DML201_PLAN Plan; } RESULT;
static void* workspace;
// The DML input of Plan (connector_link.c), field for field.
static RESULT admit(const BC250_TARGET_TIMING* t,const unsigned char caps[16],int dp)
{
    BC250_DML201_INPUT in;BC250_OPTC_SYNC sync;RESULT r;
    memset(&in,0,sizeof(in));memset(&r,0,sizeof(r));r.Optc=-1;
    CHECK(Bc250TargetTimingValid(t));
    if(dp)r.Rate=Bc250DpFirstRate(caps,t->PixelClockKhz);
    in.HActive=t->HActive;in.HTotal=t->HActive+t->HBlank;in.HFrontPorch=t->HSyncOffset;in.HSyncWidth=t->HSyncWidth;
    in.VActive=t->VActive;in.VTotal=t->VActive+t->VBlank;in.VFrontPorch=t->VSyncOffset;in.VSyncWidth=t->VSyncWidth;
    in.PixelClockKhz=t->PixelClockKhz;in.PitchPixels=((t->HActive*4u+255u)&~255u)/4u;
    in.Output=dp?BC250_DML201_DP:BC250_DML201_HDMI;in.DpLanes=dp?(caps[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK):4u;
    in.CursorWidth=0;in.AudioSampleRateKhz=192;in.DchubRefClockKhz=48000;in.DentistVcoKhz=3000000;
    r.Dml=Bc250Dml201Plan(&in,workspace,Bc250Dml201WorkspaceSize(),&r.Plan);
    if(!r.Dml && r.Plan.Valid) {
        sync.VReady=r.Plan.VReady;sync.VStartup=r.Plan.VStartup;
        sync.VUpdateOffset=r.Plan.VUpdateOffset;sync.VUpdateWidth=r.Plan.VUpdateWidth;
        r.Optc=Bc250OptcTimingReason(t,&sync);
    }
    printf("  %4lux%-4lu %7lu kHz %s rate %2u dml %d valid %u vstartup %3u vblank %3lu optc %-20s dispclk %7u dppclk %7u level %u\n",
        t->HActive,t->VActive,t->PixelClockKhz,dp?"DP  ":"TMDS",r.Rate,r.Dml,r.Plan.Valid,r.Plan.VStartup,t->VBlank,
        r.Optc<0?"-":Bc250OptcReasonName(r.Optc),r.Plan.DispClockKhz,r.Plan.DppClockKhz,r.Plan.VoltageLevel);
    return r;
}
static int admitted(const RESULT* r)
{
    // PipeApply (bc250_dcn201_pipe.c) takes DISPCLK and DPPCLK up to 1.2 GHz through the DENTIST, no SMU message.
    return !r->Dml && r->Plan.Valid && !r->Plan.Assertions && r->Optc==BC250_OPTC_REASON_OK &&
        r->Plan.DispClockKhz && r->Plan.DispClockKhz<=1200000 && r->Plan.DppClockKhz && r->Plan.DppClockKhz<=1200000;
}
int main(void)
{
    // The HP X32 of the field report (2560x1440, 165 Hz by its maker, on DP directly). 75 Hz is the tester's firmware
    // timing (total 2720x1492, 304.25 MHz, BD-122); 120, 144 and 165 Hz are reduced-blanking reconstructions with the
    // pixel clocks of the report, not that monitor's EDID, which nobody has read yet.
    static const BC250_TARGET_TIMING hpx32[4]={
        {304250,2560,160,48,32,1440,52,3,5,0x1a},{497750,2560,160,48,32,1440,85,3,5,0x1a},
        {586600,2560,80,8,32,1440,103,3,8,0x1a},{645100,2560,80,8,32,1440,41,3,8,0x1a}};
    static const unsigned rates[4]={DP_LINK_BW_2_7,DP_LINK_BW_5_4,DP_LINK_BW_5_4,DP_LINK_BW_5_4};
    // 165 Hz with the full CVT-RB2 vertical blanking (460 us, 119 lines) needs 679.1 MHz: still HBR2 x4.
    static const BC250_TARGET_TIMING rb2={679100,2560,80,8,32,1440,119,105,8,0x1a};
    // Model caps for that sink: DPCD 1.2, HBR2, four lanes, TPS3, enhanced framing. The Lenovo LT2452pwC on unit A's
    // DP reads 0x11 0x0A 0x84 (DPCD 1.1, HBR, four lanes, enhanced framing).
    static const unsigned char hbr2[16]={0x12,DP_LINK_BW_5_4,0xc4,0x01},lenovo[16]={0x11,DP_LINK_BW_2_7,0x84,0x01};
    // The LG FHD behind the passive DP-to-HDMI adapter (TMDS): 174.5 MHz total 2080x1119 sets on unit A (BD-195
    // evidence); 228.8 MHz total 2080x1100 is its preferred 100 Hz timing, refused on unit A (BD-177). The sync
    // placement is reconstructed; the totals are measured. With the real display plan the 20 lines of vertical
    // blanking are under DCN 2.0.1's min_vblank_lines (30, dcn201_resource.c), so the plan refuses the input before
    // any prefetch exists: Plan's check is dml-call, not the optc-vstartup-blank of BD-177's fixture.
    static const BC250_TARGET_TIMING lg75={174500,1920,160,48,32,1080,39,3,5,0x1a},lg100={228800,1920,160,48,32,1080,20,3,5,0x1a};
    static BC250_EDID_INFO info;static BC250_TARGET_TIMING targets[BC250_EDID_MAX_MODES];
    RESULT r;unsigned i,n,ok;
    workspace=malloc(Bc250Dml201WorkspaceSize());CHECK(workspace!=NULL);if(!workspace)return 1;
    printf("HP X32 (model HBR2 caps)\n");
    for(i=0;i<4;++i) {
        r=admit(&hpx32[i],hbr2,1);
        CHECK(r.Rate==rates[i] && admitted(&r));
        CHECK(r.Plan.VStartup<hpx32[i].VBlank);
    }
    r=admit(&rb2,hbr2,1);CHECK(r.Rate==DP_LINK_BW_5_4 && admitted(&r));
    // A 4-lane HBR2 link carries 720 MHz at RGB8; one pixel more needs HBR3, and two lanes halve it.
    {
        BC250_TARGET_TIMING t=rb2;unsigned char two[16];
        t.PixelClockKhz=720001;CHECK(Bc250DpFirstRate(hbr2,t.PixelClockKhz)==0);
        memcpy(two,hbr2,sizeof(two));two[DP_MAX_LANE_COUNT]=0xc2;
        CHECK(Bc250DpFirstRate(two,360000)==DP_LINK_BW_5_4 && Bc250DpFirstRate(two,360001)==0);
    }
    // The 165 Hz timing on the Lenovo's HBR link: the ladder has no rate for it, which is Plan's check link-rate.
    CHECK(Bc250DpFirstRate(lenovo,645100)==0 && Bc250DpFirstRate(lenovo,304250)==DP_LINK_BW_2_7);
    printf("Lenovo LT2452pwC EDID on DP (HBR x4)\n");
    CHECK(Bc250EdidParse(g_LabEdid,sizeof(g_LabEdid),&info)==BC250_EDID_OK);
    n=Bc250EdidTargetList(&info,Bc250DpMaxPixelClock(lenovo),1920,1200,targets,BC250_EDID_MAX_MODES);
    CHECK(n>0 && Bc250DpMaxPixelClock(lenovo)==360000);
    for(i=ok=0;i<n;++i) {
        r=admit(&targets[i],lenovo,1);CHECK(r.Rate==DP_LINK_BW_2_7);
        if(admitted(&r))++ok;
        // Its preferred 1920x1200 at 154 MHz is the mode the lab sets today, at HBR with TPS2.
        if(!i)CHECK(targets[0].PixelClockKhz==154000 && admitted(&r));
    }
    printf("Lenovo: %u of %u EDID targets admitted\n",ok,n);
    printf("LG FHD behind the passive adapter (TMDS)\n");
    r=admit(&lg75,hbr2,0);CHECK(admitted(&r));
    r=admit(&lg100,hbr2,0);CHECK(r.Dml==BC250_DML201_INVALID && !r.Plan.Valid && !admitted(&r));
    free(workspace);
    printf("mode_admit_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
