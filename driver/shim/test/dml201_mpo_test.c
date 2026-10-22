/* Host-only DML experiment. Include the adapter to use its pinned numerical
 * sources and bbox without exposing an unchecked multi-pipe production API.
 * Synthetic pipe input follows dcn20_populate_dml_pipes_from_context. No MMIO.
 */
#include "../bc250_dml201.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

typedef struct PROBE {
    struct display_mode_lib lib;
    struct dal_logger log;
    display_e2e_pipe_params_st pipes[2];
} PROBE;

static BC250_DML201_INPUT timing(void)
{
    BC250_DML201_INPUT i = {0};
    i.HActive=1920; i.HTotal=2080; i.HFrontPorch=48; i.HSyncWidth=32;
    i.VActive=1200; i.VTotal=1235; i.VFrontPorch=3; i.VSyncWidth=6;
    i.PixelClockKhz=154000; i.PitchPixels=1920;
    i.Output=BC250_DML201_DP; i.DpLanes=4;
    i.DchubRefClockKhz=48000; i.DentistVcoKhz=3000000;
    return i;
}

static void init(PROBE *w, unsigned dpps, unsigned otgs, unsigned secondOtg,
                 unsigned sourceHeight)
{
    BC250_DML201_INPUT i=timing();
    memset(w,0,sizeof(*w));
    w->lib.logger=&w->log;
    w->lib.ip=dcn201_ip; w->lib.soc=dcn201_soc;
    w->lib.ip.max_num_dpp=dpps; w->lib.ip.max_num_otg=otgs;
    w->lib.project=DML_PROJECT_DCN201;
    w->lib.soc.dispclk_dppclk_vco_speed_mhz=i.DentistVcoKhz/1000.0;
    w->lib.funcs.validate=dml20_ModeSupportAndSystemConfigurationFull;
    w->lib.funcs.recalculate=dml20_recalculate;
    w->lib.funcs.rq_dlg_get_dlg_reg=dml20_rq_dlg_get_dlg_reg;
    w->lib.funcs.rq_dlg_get_rq_reg=dml20_rq_dlg_get_rq_reg;
    pipeInput(&i,&w->pipes[0]);
    pipeInput(&i,&w->pipes[1]);
    w->pipes[1].pipe.dest.otg_inst=(unsigned char)secondOtg;
    w->pipes[1].pipe.src.viewport_height=sourceHeight;
    w->pipes[1].pipe.src.viewport_height_max=sourceHeight;
    w->pipes[1].pipe.src.surface_height_y=sourceHeight;
    /* The hardware geometry ratio is 31.32, then converted to double by AMD. */
    w->pipes[1].pipe.scale_ratio_depth.vscl_ratio=
        (double)(((unsigned long long)sourceHeight<<32)/i.VActive)/4294967296.0;
    w->pipes[1].pipe.scale_ratio_depth.scl_enable=sourceHeight!=i.VActive;
    w->pipes[1].pipe.scale_taps.vtaps=sourceHeight==i.VActive?1:4;
}

static unsigned voltage(PROBE *w, unsigned count)
{
    return dml_get_voltage_level(&w->lib,w->pipes,count);
}

static double admitted(PROBE *w, unsigned count, unsigned secondOtg,
                     unsigned sourceHeight, const char *name)
{
    struct display_mode_lib *lib=&w->lib;
    unsigned level=voltage(w,count), comb=lib->vba.maxMpcComb;
    CHECK(level<lib->soc.num_states);
    CHECK(comb<=1);
    CHECK(w->log.Assertions==0);
    if(level>=lib->soc.num_states || comb>1) return 0.0;
    CHECK(lib->vba.ModeSupport[level][comb]);
    CHECK(lib->vba.NumberOfActivePlanes==count);
    if(count==2) {
        CHECK(lib->vba.BlendingAndTiming[1]==(secondOtg?1:0));
        CHECK(lib->vba.VRatio[1]==w->pipes[1].pipe.scale_ratio_depth.vscl_ratio);
        CHECK(lib->vba.ScalerEnabled[1]==(sourceHeight!=1200));
    }
    for(unsigned p=0;p<count;++p) {
        CHECK(lib->vba.DPPPerPlane[p]==1);
        CHECK(lib->vba.ODMCombineEnabled[p]==dm_odm_combine_mode_disabled);
        w->pipes[p].clks_cfg.dispclk_mhz=lib->vba.RequiredDISPCLK[level][comb];
        w->pipes[p].clks_cfg.dppclk_mhz=lib->vba.RequiredDPPCLK[level][comb][p];
    }
    /* Shared watermarks must include both consumers. Recompute at B/C/D then A. */
    double urgent=0.0;
    for(unsigned k=1;k<=4;++k) {
        unsigned index=k==4?0:k, v=level>index?level:index;
        /* DCN20's state-D branch takes DCF/SOC from state 2. The pinned DCN201
         * table has equal values in states 2 and 3; retain the actual mapping. */
        unsigned clockLevel=index==3 && level<3?2:v;
        w->pipes[0].clks_cfg.voltage=v;
        w->pipes[0].clks_cfg.dcfclk_mhz=lib->soc.clock_limits[clockLevel].dcfclk_mhz;
        w->pipes[0].clks_cfg.socclk_mhz=lib->soc.clock_limits[clockLevel].socclk_mhz;
        urgent=get_wm_urgent(lib,w->pipes,count);
        CHECK(urgent>0.0 && urgent<1000000.0);
    }
    for(unsigned p=0;p<count;++p) {
        display_pipe_dest_params_st *d=&w->pipes[p].pipe.dest;
        d->vstartup_start=(unsigned)get_vstartup(lib,w->pipes,count,p);
        d->vupdate_offset=(unsigned)get_vupdate_offset(lib,w->pipes,count,p);
        d->vupdate_width=(unsigned)get_vupdate_width(lib,w->pipes,count,p);
        d->vready_offset=(unsigned)get_vready_offset(lib,w->pipes,count,p);
        CHECK(d->vstartup_start>0 && d->vstartup_start<35);
        CHECK(d->vupdate_width>0);
    }
    for(unsigned p=0;p<count;++p) {
        display_dlg_regs_st dlg={0};
        display_ttu_regs_st ttu={0};
        display_rq_regs_st rq={0};
        bool pstate=lib->vba.DRAMClockChangeSupport[level][comb]!=dm_dram_clock_change_unsupported;
        lib->funcs.rq_dlg_get_dlg_reg(lib,&dlg,&ttu,w->pipes,count,p,false,pstate,false,false,true);
        lib->funcs.rq_dlg_get_rq_reg(lib,&rq,&w->pipes[p].pipe);
        CHECK(dlg.dlg_vblank_end==32);
        CHECK(dlg.refcyc_per_htotal==(unsigned)(48000.0/154000.0*2080.0*256.0));
        CHECK(dlg.refcyc_per_line_delivery_l>0);
        printf("%s pipe=%u source_height=%u level=%u blend=%u dppclk=%.9g urgent_us=%.9g vstartup=%u refcyc_htotal=%u line_delivery=%u\n",
            name,p,p?sourceHeight:1200,level,lib->vba.BlendingAndTiming[p],
            w->pipes[p].clks_cfg.dppclk_mhz,urgent,
            w->pipes[p].pipe.dest.vstartup_start,dlg.refcyc_per_htotal,dlg.refcyc_per_line_delivery_l);
    }
    CHECK(w->log.Assertions==0);
    return urgent;
}

int main(void)
{
    PROBE *w=(PROBE *)malloc(sizeof(*w));
    CHECK(w!=NULL); if(!w) return 1;
    init(w,1,1,0,1200); double one=admitted(w,1,0,1200,"one_native");
    init(w,2,1,0,1200); double two=admitted(w,2,0,1200,"two_native");
    CHECK(two>one); /* A one-pipe watermark is not reusable for this fixture. */
    init(w,2,1,0,1080); admitted(w,2,0,1080,"overlay_upscale");
    init(w,2,1,0,2400); admitted(w,2,0,2400,"overlay_downscale");
    init(w,1,1,0,1200); CHECK(voltage(w,2)>=w->lib.soc.num_states);
    CHECK(w->log.Assertions==0);
    init(w,2,1,1,1200); CHECK(voltage(w,2)>=w->lib.soc.num_states);
    CHECK(w->log.Assertions==0);
    init(w,2,2,1,1200); admitted(w,2,1,1200,"two_outputs_control");
    free(w);
    printf("dml201_mpo: %u checks, %u failures; synthetic inputs, no hardware admission\n",checks,failures);
    return failures?1:0;
}
