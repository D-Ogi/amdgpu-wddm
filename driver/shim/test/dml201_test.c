#include "../bc250_dml201.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned int checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL line %u: %s\n",__LINE__,#x);}} while(0)
static BC250_DML201_INPUT mode1080(void)
{
    BC250_DML201_INPUT i;
    memset(&i,0,sizeof(i));
    i.HActive=1920;i.HTotal=2200;i.HFrontPorch=88;i.HSyncWidth=44;
    i.VActive=1080;i.VTotal=1125;i.VFrontPorch=4;i.VSyncWidth=5;
    i.PixelClockKhz=148500;i.PitchPixels=1920;i.Output=BC250_DML201_HDMI;
    i.DpLanes=4;i.DchubRefClockKhz=48000;i.DentistVcoKhz=3000000;
    return i;
}
int main(void)
{
    BC250_DML201_INPUT i=mode1080(),bad;
    BC250_DML201_PLAN o,again;
    size_t size=Bc250Dml201WorkspaceSize();
    void *workspace=malloc(size);
    int status;
    CHECK(workspace!=NULL);if(!workspace)return 1;
    status=Bc250Dml201Plan(&i,workspace,size,&o);
    printf("1080p status=%d assertions=%u first=%u size=%zu sync=%u/%u/%u/%u clocks=%u/%u\n",
        status,o.Assertions,o.FirstAssertionLine,size,o.VStartup,o.VReady,o.VUpdateOffset,o.VUpdateWidth,o.DispClockKhz,o.DppClockKhz);
    CHECK(status==0);CHECK(o.Valid==1);CHECK(o.Assertions==0);CHECK(o.BboxDefaults==1);
    CHECK(o.VUpdateOffset==550);CHECK(o.VStartup>0 && o.VStartup<45);
    CHECK(o.Dlg.refcyc_per_htotal==(unsigned int)(48000.0/148500.0*2200.0*256.0));
    CHECK(o.Dlg.dlg_vblank_end==41);CHECK(o.Destination.vblank_start==1121);
    CHECK(o.Watermarks[0].UrgentNs>0);CHECK(o.CStateEnabled==0);
    CHECK(Bc250Dml201Plan(&i,workspace,size,&again)==0);CHECK(!memcmp(&o,&again,sizeof(o)));
    bad=i;bad.HTotal=0;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);CHECK(!again.Valid);
    bad=i;bad.PitchPixels=1919;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.DchubRefClockKhz=0;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.DentistVcoKhz=0;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.VTotal=1090;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.Output=2;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    CHECK(Bc250Dml201Plan(&i,workspace,size-1,&again)==BC250_DML201_INVALID);
    CHECK(Bc250Dml201Plan(&i,(char*)workspace+1,size,&again)==BC250_DML201_INVALID);
    CHECK(Bc250Dml201Plan(NULL,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.PixelClockKhz=1200000;CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)!=0);CHECK(!again.Valid);
    bad=i;bad.HActive=1280;bad.HTotal=1650;bad.HFrontPorch=110;bad.HSyncWidth=40;
    bad.VActive=720;bad.VTotal=750;bad.VFrontPorch=5;bad.VSyncWidth=5;bad.PixelClockKhz=74250;bad.PitchPixels=1280;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==0);
    CHECK(again.Dlg.refcyc_per_htotal!=o.Dlg.refcyc_per_htotal);CHECK(again.Dlg.dlg_vblank_end==25);
    bad=i;bad.Output=BC250_DML201_DP;bad.CursorWidth=256;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==0);
    bad=i;bad.DchubRefClockKhz=50000;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==0);
    CHECK(again.Dlg.refcyc_per_htotal==(unsigned int)(50000.0/148500.0*2200.0*256.0));
    CHECK(again.Dlg.refcyc_per_htotal!=o.Dlg.refcyc_per_htotal);
    bad=i;bad.Output=BC250_DML201_DP;bad.DpLanes=3;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.CursorWidth=257;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.HFrontPorch=0xffffffffu;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.VSyncWidth=0xffffffffu;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==BC250_DML201_INVALID);
    bad=i;bad.HActive=640;bad.HTotal=800;bad.HFrontPorch=16;bad.HSyncWidth=96;
    bad.VActive=480;bad.VTotal=525;bad.VFrontPorch=10;bad.VSyncWidth=2;
    bad.PixelClockKhz=25175;bad.PitchPixels=640;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==0);CHECK(again.Destination.hactive==640);
    bad=i;bad.HActive=2560;bad.HTotal=2720;bad.HFrontPorch=48;bad.HSyncWidth=32;
    bad.VActive=1440;bad.VTotal=1481;bad.VFrontPorch=3;bad.VSyncWidth=5;
    bad.PixelClockKhz=241500;bad.PitchPixels=2560;
    CHECK(Bc250Dml201Plan(&bad,workspace,size,&again)==0);CHECK(again.Destination.hactive==2560);
    /* Firmware-timing reuse would fail these measured register relationships. */
    CHECK(again.Dlg.dlg_vblank_end==38);CHECK(again.Dlg.refcyc_per_htotal!=o.Dlg.refcyc_per_htotal);
    {
        /* BD-223: the scaled plane. The lab mode is the Lenovo's 1920x1200 CVT-RB
         * timing (154 MHz, 2080x1235); the chain is the game's 1920x1080. */
        BC250_DML201_INPUT lab=i,s;BC250_DML201_PLAN native,scaledPlan,ident;
        lab.HActive=1920;lab.HTotal=2080;lab.HFrontPorch=48;lab.HSyncWidth=32;
        lab.VActive=1200;lab.VTotal=1235;lab.VFrontPorch=3;lab.VSyncWidth=6;
        lab.PixelClockKhz=154000;lab.PitchPixels=1920;lab.Output=BC250_DML201_DP;
        CHECK(Bc250Dml201Plan(&lab,workspace,size,&native)==0);CHECK(native.Valid==1);
        /* The identity plane is the legacy pipe: the same plan, field for field. */
        s=lab;s.ViewportWidth=1920;s.ViewportHeight=1200;s.RecoutWidth=1920;s.RecoutHeight=1200;s.HTaps=s.VTaps=1;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&ident)==0);CHECK(!memcmp(&ident,&native,sizeof(native)));
        /* Stretched: 1080 lines up to 1200, 4 vertical taps, 1:1 across. */
        s.ViewportHeight=1080;s.VTaps=4;
        status=Bc250Dml201Plan(&s,workspace,size,&scaledPlan);
        printf("scaled 1920x1080->1920x1200 status=%d valid=%u dppclk=%u/%u dispclk=%u/%u vstartup=%u/%u\n",
            status,scaledPlan.Valid,scaledPlan.DppClockKhz,native.DppClockKhz,scaledPlan.DispClockKhz,
            native.DispClockKhz,scaledPlan.VStartup,native.VStartup);
        CHECK(status==0);CHECK(scaledPlan.Valid==1);CHECK(scaledPlan.Assertions==0);
        CHECK(scaledPlan.Destination.recout_height==1200);CHECK(scaledPlan.Destination.vactive==1200);
        /* Fewer source lines: the request schedule differs from the native pipe's. */
        CHECK(memcmp(&scaledPlan.Dlg,&native.Dlg,sizeof(native.Dlg))!=0);
        /* Aspect ratio kept: the recout is 1920x1080 inside 1920x1200, every direction 1:1. */
        s=lab;s.ViewportWidth=1920;s.ViewportHeight=1080;s.RecoutWidth=1920;s.RecoutHeight=1080;s.HTaps=s.VTaps=1;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==0);CHECK(again.Destination.recout_height==1080);
        /* Both directions up: 1280x720 into 1920x1080. */
        s=lab;s.ViewportWidth=1280;s.ViewportHeight=720;s.RecoutWidth=1920;s.RecoutHeight=1080;s.HTaps=s.VTaps=4;
        s.PitchPixels=1280;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==0);CHECK(again.Valid==1);
        /* Refusals before the AMD code runs. */
        s=lab;s.ViewportWidth=1920;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);CHECK(!again.Valid);
        s=lab;s.ViewportWidth=1920;s.ViewportHeight=1080;s.RecoutWidth=1921;s.RecoutHeight=1200;s.HTaps=4;s.VTaps=4;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);
        s.RecoutWidth=1920;s.RecoutHeight=1201;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);
        s.RecoutHeight=1200;s.VTaps=1;
        CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);
        s.VTaps=9;CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);
        s.VTaps=0;CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);
        s.VTaps=4;s.HTaps=1;CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==0);
        /* The pitch bounds the viewport, not the active width. */
        s=lab;s.ViewportWidth=1280;s.ViewportHeight=720;s.RecoutWidth=1920;s.RecoutHeight=1080;s.HTaps=s.VTaps=4;
        s.PitchPixels=1216;CHECK(Bc250Dml201Plan(&s,workspace,size,&again)==BC250_DML201_INVALID);
    }
    free(workspace);printf("dml201: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
