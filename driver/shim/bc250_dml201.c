// PROVENANCE: AMD DC (MIT), Linux 7d0a66e4, DML20 and DCN201 resource bounding boxes.
#include "bc250_dml201.h"
#include "dml201/display_mode_lib.h"
#include "dml201/dcn20/display_mode_vba_20.h"
#include "dml201/dcn20/display_rq_dlg_calc_20.h"

/* AMD numerical code is compiled intact. Conversion, signedness and unused
 * diagnostics describe its generated arithmetic, not our adapter boundary. */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100 4101 4127 4189 4244 4267 4305 4389 4018 4706)
#endif
#include "dml201/dcn_calc_math.c"
#include "dml201/display_mode_vba.c"
#include "dml201/display_rq_dlg_helpers.c"
#include "dml201/dcn20/display_mode_vba_20.c"
#include "dml201/dcn20/display_rq_dlg_calc_20.c"
#include "dml201/bbox.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef struct BC250_DML201_WORKSPACE {
    struct display_mode_lib Lib;
    struct dal_logger Logger;
    display_e2e_pipe_params_st Pipe;
    BC250_DML201_PLAN Plan;
} BC250_DML201_WORKSPACE;

size_t Bc250Dml201WorkspaceSize(void) { return sizeof(BC250_DML201_WORKSPACE); }

static int scaled(const BC250_DML201_INPUT *i)
{
    return i->ViewportWidth || i->ViewportHeight || i->RecoutWidth || i->RecoutHeight || i->HTaps || i->VTaps;
}

/* BD-223. The limits are AMD's DCN201 scaler limits as dcn20_validate_bandwidth
 * reaches them: 8 taps at most, 1 tap only for a 1:1 direction, a recout inside
 * the active area. The ratio bounds themselves are the planner's (dcn_plane.c);
 * DML answers only whether the pipe fits the clocks and the return bandwidth. */
static int validScale(const BC250_DML201_INPUT *i)
{
    if (!scaled(i)) return 1;
    if (!i->ViewportWidth || !i->ViewportHeight || !i->RecoutWidth || !i->RecoutHeight ||
        i->RecoutWidth > i->HActive || i->RecoutHeight > i->VActive ||
        i->ViewportWidth > 16384 || i->ViewportHeight > 16384 ||
        i->HTaps < 1 || i->HTaps > 8 || i->VTaps < 1 || i->VTaps > 8 ||
        (i->HTaps == 1 && i->ViewportWidth != i->RecoutWidth) ||
        (i->VTaps == 1 && i->ViewportHeight != i->RecoutHeight))
        return 0;
    return 1;
}

static int validInput(const BC250_DML201_INPUT *i)
{
    unsigned int width;
    if (!i || !validScale(i)) return 0;
    width = scaled(i) ? i->ViewportWidth : i->HActive;
    if (!i->HActive || !i->VActive || i->HTotal > 8192 || i->VTotal > 8192 ||
        i->HActive >= i->HTotal || i->VActive >= i->VTotal ||
        !i->HFrontPorch || !i->HSyncWidth || !i->VFrontPorch || !i->VSyncWidth ||
        i->HFrontPorch >= i->HTotal - i->HActive ||
        i->HSyncWidth > i->HTotal - i->HActive - i->HFrontPorch ||
        i->VFrontPorch >= i->VTotal - i->VActive ||
        i->VSyncWidth > i->VTotal - i->VActive - i->VFrontPorch ||
        i->VTotal - i->VActive < dcn201_ip.min_vblank_lines ||
        i->PitchPixels < width || i->PitchPixels > 16384 || (i->PitchPixels & 63) ||
        i->PixelClockKhz < 10000 || i->PixelClockKhz > 1200000 ||
        i->DchubRefClockKhz < 40000 || i->DchubRefClockKhz > 60000 ||
        i->DchubRefClockKhz >= i->PixelClockKhz * 4u ||
        i->DentistVcoKhz < 1000000 || i->DentistVcoKhz > 6000000 ||
        i->Output > BC250_DML201_DP ||
        (i->Output == BC250_DML201_DP && i->DpLanes != 1 && i->DpLanes != 2 && i->DpLanes != 4) ||
        i->CursorWidth > 256 || i->AudioSampleRateKhz > 192)
        return 0;
    return 1;
}

static void pipeInput(const BC250_DML201_INPUT *i, display_e2e_pipe_params_st *p)
{
    display_pipe_source_params_st *s = &p->pipe.src;
    display_pipe_dest_params_st *d = &p->pipe.dest;
    /* dcn20_populate_dml_pipes_from_context, DCN201 progressive RGB8 branch. */
    s->source_format = dm_444_32; s->source_scan = dm_horz;
    s->source_rotation = dm_rotation_0; s->sw_mode = dm_sw_linear;
    s->macro_tile_size = dm_64k_tile; s->dcc_rate = 1;
    s->viewport_width = s->viewport_width_max = s->surface_width_y = i->HActive;
    s->viewport_height = s->viewport_height_max = s->surface_height_y = i->VActive;
    s->data_pitch = i->PitchPixels;
    s->num_cursors = i->CursorWidth ? 1 : 0;
    s->cur0_src_width = i->CursorWidth; s->cur0_bpp = dm_cur_32bit;
    d->use_maximum_vstartup = 1; /* DCN_VERSION_2_01 */
    d->synchronized_vblank_all_planes = d->synchronize_timings = 1;
    d->hblank_start = i->HTotal - i->HFrontPorch;
    d->hblank_end = d->hblank_start - i->HActive;
    d->vblank_start = i->VTotal - i->VFrontPorch;
    d->vblank_end = d->vblank_start - i->VActive;
    d->htotal = i->HTotal; d->vtotal = d->vtotal_min = d->vtotal_max = i->VTotal;
    d->hactive = d->recout_width = d->full_recout_width = i->HActive;
    d->vactive = d->recout_height = d->full_recout_height = i->VActive;
    d->pixel_rate_mhz = i->PixelClockKhz / 1000.0;
    d->odm_combine = dm_odm_combine_mode_disabled;
    p->pipe.scale_ratio_depth.lb_depth = dm_lb_16;
    p->pipe.scale_ratio_depth.hscl_ratio = p->pipe.scale_ratio_depth.vscl_ratio = 1.0;
    p->pipe.scale_taps.htaps = p->pipe.scale_taps.vtaps = 1;
    if (scaled(i)) {
        /* dcn20_populate_dml_pipes_from_context: viewport and surface from
         * plane_res.scl_data.viewport, recout from scl_data.recout, ratios
         * from scl_data.ratios (source over destination), taps from
         * scl_data.taps. The surface is the viewport: the plane reads its
         * allocation from the origin (dcn_plane.c refuses a crop). For RGB the
         * chroma ratios equal the luma ones, and the chroma taps are the ones
         * dcn_scale.c programs (2, or 1 for a 1:1 direction); DML's 444 path
         * does not read them. */
        s->viewport_width = s->viewport_width_max = s->surface_width_y = i->ViewportWidth;
        s->viewport_height = s->viewport_height_max = s->surface_height_y = i->ViewportHeight;
        d->recout_width = d->full_recout_width = i->RecoutWidth;
        d->recout_height = d->full_recout_height = i->RecoutHeight;
        p->pipe.scale_ratio_depth.hscl_ratio = p->pipe.scale_ratio_depth.hscl_ratio_c =
            (double)i->ViewportWidth / (double)i->RecoutWidth;
        p->pipe.scale_ratio_depth.vscl_ratio = p->pipe.scale_ratio_depth.vscl_ratio_c =
            (double)i->ViewportHeight / (double)i->RecoutHeight;
        p->pipe.scale_ratio_depth.scl_enable =
            i->ViewportWidth != i->RecoutWidth || i->ViewportHeight != i->RecoutHeight;
        p->pipe.scale_taps.htaps = i->HTaps;
        p->pipe.scale_taps.vtaps = i->VTaps;
        p->pipe.scale_taps.htaps_c = i->HTaps == 1 ? 1 : 2;
        p->pipe.scale_taps.vtaps_c = i->VTaps == 1 ? 1 : 2;
    }
    p->clks_cfg.refclk_mhz = i->DchubRefClockKhz / 1000.0;
    p->dout.output_type = i->Output == BC250_DML201_DP ? dm_dp : dm_hdmi;
    p->dout.output_format = dm_444; p->dout.output_bpp = 24;
    p->dout.output_bpc = 8; p->dout.dp_lanes = (int)i->DpLanes;
    p->dout.dp_rate = dm_dp_rate_na; p->dout.dsc_input_bpc = 12;
    p->dout.max_audio_sample_rate = (int)i->AudioSampleRateKhz;
}

/* Floating conversion is checked before the C cast. NaN and infinities fail. */
static int asUint(double value, unsigned int *out)
{
    if (!(value >= 0.0 && value <= 4294967294.0)) return 0;
    *out = (unsigned int)value;
    return 1;
}
static int ceilUint(double value, unsigned int *out)
{
    unsigned int floorValue;
    if (!asUint(value, &floorValue)) return 0;
    *out = floorValue + (value > (double)floorValue ? 1u : 0u);
    return 1;
}

static int watermarks(struct display_mode_lib *lib, display_e2e_pipe_params_st *p,
                      BC250_DML201_WATERMARKS *w)
{
    return ceilUint(get_wm_urgent(lib,p,1)*1000.0,&w->UrgentNs) &&
        ceilUint(get_wm_stutter_enter_exit(lib,p,1)*1000.0,&w->StutterEnterExitNs) &&
        ceilUint(get_wm_stutter_exit(lib,p,1)*1000.0,&w->StutterExitNs) &&
        ceilUint(get_wm_dram_clock_change(lib,p,1)*1000.0,&w->DramClockChangeNs) &&
        ceilUint(get_wm_memory_trip(lib,p,1)*1000.0,&w->MemoryTripNs);
}

int Bc250Dml201Plan(const BC250_DML201_INPUT *input, void *workspace,
                   size_t bytes, BC250_DML201_PLAN *plan)
{
    BC250_DML201_WORKSPACE *w = (BC250_DML201_WORKSPACE *)workspace;
    struct display_mode_lib *lib;
    display_e2e_pipe_params_st *p;
    BC250_DML201_PLAN *o;
    unsigned int vlevel, comb, k, level;
    int result = BC250_DML201_UNSUPPORTED;
    if (!plan) return BC250_DML201_INVALID;
    memset(plan,0,sizeof(*plan));
    if (!validInput(input) || !workspace || bytes < sizeof(*w) || ((size_t)workspace & 7u))
        return BC250_DML201_INVALID;
    memset(w,0,sizeof(*w));
    lib=&w->Lib; p=&w->Pipe; o=&w->Plan;
    lib->logger=&w->Logger; lib->ip=dcn201_ip; lib->soc=dcn201_soc;
    lib->project=DML_PROJECT_DCN201;
    /* The integration has one physical pipe. Reject plans requiring a split. */
    lib->ip.max_num_dpp=1; lib->ip.max_num_otg=1;
    lib->soc.dispclk_dppclk_vco_speed_mhz=input->DentistVcoKhz/1000.0;
    lib->funcs.validate=dml20_ModeSupportAndSystemConfigurationFull;
    lib->funcs.recalculate=dml20_recalculate;
    lib->funcs.rq_dlg_get_dlg_reg=dml20_rq_dlg_get_dlg_reg;
    lib->funcs.rq_dlg_get_rq_reg=dml20_rq_dlg_get_rq_reg;
    pipeInput(input,p);
    vlevel=dml_get_voltage_level(lib,p,1); comb=lib->vba.maxMpcComb;
    if (vlevel >= lib->soc.num_states || comb > 1 || lib->vba.DPPPerPlane[0] != 1 ||
        lib->vba.ODMCombineEnabled[0] != dm_odm_combine_mode_disabled ||
        !lib->vba.ModeSupport[vlevel][comb]) goto done;
    o->VoltageLevel=vlevel; o->BboxDefaults=1;
    p->clks_cfg.dispclk_mhz=lib->vba.RequiredDISPCLK[vlevel][comb];
    p->clks_cfg.dppclk_mhz=lib->vba.RequiredDPPCLK[vlevel][comb][0];
    if (!(p->clks_cfg.dispclk_mhz > 0.0 && p->clks_cfg.dppclk_mhz > 0.0)) goto done;
    /* AMD computes B,C,D first and restores the selected A state last. */
    for(k=1;k<=4;++k) {
        unsigned int index=k==4?0:k;
        level=vlevel>index?vlevel:index;
        p->clks_cfg.voltage=level;
        p->clks_cfg.dcfclk_mhz=lib->soc.clock_limits[level].dcfclk_mhz;
        p->clks_cfg.socclk_mhz=lib->soc.clock_limits[level].socclk_mhz;
        if (!watermarks(lib,p,&o->Watermarks[index])) goto done;
    }
    if (!asUint(get_vstartup(lib,p,1,0),&o->VStartup) ||
        !asUint(get_vupdate_offset(lib,p,1,0),&o->VUpdateOffset) ||
        !asUint(get_vupdate_width(lib,p,1,0),&o->VUpdateWidth) ||
        !asUint(get_vready_offset(lib,p,1,0),&o->VReady)) goto done;
    p->pipe.dest.vstartup_start=o->VStartup;
    p->pipe.dest.vupdate_offset=o->VUpdateOffset;
    p->pipe.dest.vupdate_width=o->VUpdateWidth;
    p->pipe.dest.vready_offset=o->VReady;
    o->PStateSupported=lib->vba.DRAMClockChangeSupport[vlevel][comb]!=dm_dram_clock_change_unsupported;
    o->CStateEnabled=0; /* Explicit AMD DCN201 policy, dcn20_fpu.c:1230. */
    lib->funcs.rq_dlg_get_dlg_reg(lib,&o->Dlg,&o->Ttu,p,1,0,false,
        (bool)o->PStateSupported,false,false,true);
    lib->funcs.rq_dlg_get_rq_reg(lib,&o->Rq,&p->pipe);
    if (!ceilUint(p->clks_cfg.dispclk_mhz*1000,&o->DispClockKhz) ||
        !ceilUint(p->clks_cfg.dppclk_mhz*1000,&o->DppClockKhz) ||
        !ceilUint(lib->vba.DCFCLK*1000,&o->DcfClockKhz) ||
        !ceilUint(lib->vba.SOCCLK*1000,&o->SocClockKhz) ||
        !ceilUint(lib->vba.DCFCLKDeepSleep*1000,&o->DcfDeepSleepKhz) ||
        !ceilUint(lib->vba.FabricClock*1000,&o->FabricClockKhz) ||
        !ceilUint(lib->vba.DRAMSpeed*1000/16,&o->DramClockKhz)) goto done;
    if (!o->VStartup || o->VStartup >= input->VTotal-input->VActive ||
        !o->VUpdateWidth || !o->DispClockKhz || !o->DppClockKhz) goto done;
    o->Destination=p->pipe.dest;
    result=BC250_DML201_OK;
done:
    if (w->Logger.Assertions) result=BC250_DML201_ASSERTION;
    if (result==BC250_DML201_OK) { o->Valid=1; *plan=*o; }
    plan->Assertions=w->Logger.Assertions;
    plan->FirstAssertionLine=w->Logger.FirstLine;
    return result;
}
