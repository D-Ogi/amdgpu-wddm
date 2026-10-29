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
    display_e2e_pipe_params_st Pipe[2];
    BC250_DML201_PLAN Plan[2];
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

static int validInput(const BC250_DML201_INPUT *i, int single)
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
        (single && (i->PitchPixels < width || i->PitchPixels > 16384 || (i->PitchPixels & 63))) ||
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
                      unsigned int count, BC250_DML201_WATERMARKS *w)
{
    return ceilUint(get_wm_urgent(lib,p,count)*1000.0,&w->UrgentNs) &&
        ceilUint(get_wm_stutter_enter_exit(lib,p,count)*1000.0,&w->StutterEnterExitNs) &&
        ceilUint(get_wm_stutter_exit(lib,p,count)*1000.0,&w->StutterExitNs) &&
        ceilUint(get_wm_dram_clock_change(lib,p,count)*1000.0,&w->DramClockChangeNs) &&
        ceilUint(get_wm_memory_trip(lib,p,count)*1000.0,&w->MemoryTripNs);
}

static int validAxis(const BC250_SCALER_AXIS_PLAN *a, unsigned surface,
                     unsigned active, unsigned source, unsigned taps)
{
    return surface && surface<=16383 && source && source<=surface &&
        a->ViewportSize && a->ViewportBegin<surface &&
        a->ViewportSize<=surface-a->ViewportBegin && a->ViewportSize<=source &&
        a->RecoutSize && a->RecoutBegin<active && a->RecoutSize<=active-a->RecoutBegin &&
        a->Ratio19 && a->Ratio19<(8u<<19) && taps && taps<=8 &&
        (taps!=1 || a->Ratio19==(1u<<19));
}

static int groupPipeInput(const BC250_DML201_INPUT *timing,
                          const BC250_DML201_PLANE_INPUT *i,
                          display_e2e_pipe_params_st *p)
{
    const BC250_SCALER_PREPARED *s=i->Scaler;
    unsigned int nv12=i->Format==BC250_DML201_NV12;
    if (!s || i->Format>BC250_DML201_NV12 ||
        i->StorageBits!=(nv12?8u:32u) || i->PitchPixels<i->SurfaceWidth ||
        i->PitchPixels>16384 || (i->PitchPixels&31) ||
        !validAxis(&s->Geometry.LumaH,i->SurfaceWidth,timing->HActive,i->SourceWidth,s->LumaH.Taps) ||
        !validAxis(&s->Geometry.LumaV,i->SurfaceHeight,timing->VActive,i->SourceHeight,s->LumaV.Taps) ||
        s->Buffer.Config>3 || !s->Buffer.LumaPartitions)
        return 0;
    if (nv12) {
        /* DML's linear 256-byte blocks hold 256 Y samples or 128 UV pairs.
         * Keep allocation geometry separate from destination clipping. */
        if ((i->SurfaceWidth&1) || (i->SurfaceHeight&1) || (i->PitchPixels&255) ||
            i->ChromaPitchSamples<i->SurfaceWidth/2 || i->ChromaPitchSamples>16384 ||
            (i->ChromaPitchSamples&127) || !s->Buffer.ChromaPartitions ||
            !validAxis(&s->Geometry.ChromaH,i->SurfaceWidth/2,timing->HActive,
                i->SourceWidth/2,s->ChromaH.Taps) ||
            !validAxis(&s->Geometry.ChromaV,i->SurfaceHeight/2,timing->VActive,
                i->SourceHeight/2,s->ChromaV.Taps)) return 0;
        const BC250_SCALER_AXIS_PLAN *l[2]={&s->Geometry.LumaH,&s->Geometry.LumaV};
        const BC250_SCALER_AXIS_PLAN *c[2]={&s->Geometry.ChromaH,&s->Geometry.ChromaV};
        for (unsigned int axis=0;axis<2;++axis) {
            /* Exact 8:1 is reduced by one luma LSB by dpp201, while chroma
             * remains exactly 4:1. Otherwise floor(luma/2) is exact. */
            if ((c[axis]->Ratio19!=l[axis]->Ratio19/2 &&
                 !(l[axis]->Ratio19==(8u<<19)-1 && c[axis]->Ratio19==(4u<<19))) ||
                c[axis]->RecoutBegin!=l[axis]->RecoutBegin ||
                c[axis]->RecoutSize!=l[axis]->RecoutSize) return 0;
        }
    } else if (i->ChromaPitchSamples ||
        s->Geometry.ChromaH.Ratio19!=s->Geometry.LumaH.Ratio19 ||
        s->Geometry.ChromaV.Ratio19!=s->Geometry.LumaV.Ratio19) return 0;
    pipeInput(timing,p);
    p->pipe.src.surface_width_y=i->SurfaceWidth;
    p->pipe.src.surface_height_y=i->SurfaceHeight;
    p->pipe.src.viewport_width_max=i->SourceWidth;
    p->pipe.src.viewport_height_max=i->SourceHeight;
    p->pipe.src.viewport_width=s->Geometry.LumaH.ViewportSize;
    p->pipe.src.viewport_height=s->Geometry.LumaV.ViewportSize;
    p->pipe.src.viewport_x_y=s->Geometry.LumaH.ViewportBegin;
    p->pipe.src.viewport_y_y=s->Geometry.LumaV.ViewportBegin;
    p->pipe.src.data_pitch=i->PitchPixels;
    if (nv12) {
        /* dcn20_populate_dml_pipes_from_context: progressive 4:2:0 video. */
        p->pipe.src.source_format=dm_420_8;
        p->pipe.src.surface_width_c=i->SurfaceWidth/2;
        p->pipe.src.surface_height_c=i->SurfaceHeight/2;
        p->pipe.src.viewport_width_c=s->Geometry.ChromaH.ViewportSize;
        p->pipe.src.viewport_height_c=s->Geometry.ChromaV.ViewportSize;
        p->pipe.src.viewport_x_c=s->Geometry.ChromaH.ViewportBegin;
        p->pipe.src.viewport_y_c=s->Geometry.ChromaV.ViewportBegin;
        p->pipe.src.data_pitch_c=i->ChromaPitchSamples;
        p->pipe.src.num_cursors=0;
        p->pipe.src.cur0_src_width=0;
        p->pipe.scale_taps.htaps_c=s->ChromaH.Taps;
        p->pipe.scale_taps.vtaps_c=s->ChromaV.Taps;
    }
    p->pipe.dest.recout_width=p->pipe.dest.full_recout_width=s->Geometry.LumaH.RecoutSize;
    p->pipe.dest.recout_height=p->pipe.dest.full_recout_height=s->Geometry.LumaV.RecoutSize;
    /* Use the same quantized ratios as the hardware geometry/filter plan. */
    p->pipe.scale_ratio_depth.hscl_ratio=s->Geometry.LumaH.Ratio19/524288.0;
    p->pipe.scale_ratio_depth.vscl_ratio=s->Geometry.LumaV.Ratio19/524288.0;
    p->pipe.scale_ratio_depth.hscl_ratio_c=s->Geometry.ChromaH.Ratio19/524288.0;
    p->pipe.scale_ratio_depth.vscl_ratio_c=s->Geometry.ChromaV.Ratio19/524288.0;
    p->pipe.scale_ratio_depth.scl_enable=s->Geometry.LumaH.Ratio19!=(1u<<19) ||
        s->Geometry.LumaV.Ratio19!=(1u<<19) ||
        (nv12 && (s->Geometry.ChromaH.Ratio19!=(1u<<19) ||
                  s->Geometry.ChromaV.Ratio19!=(1u<<19)));
    p->pipe.scale_taps.htaps=s->LumaH.Taps;
    p->pipe.scale_taps.vtaps=s->LumaV.Taps;
    return 1;
}

static int planPipes(const BC250_DML201_INPUT *input,
                     const BC250_DML201_PLANE_INPUT *planes, unsigned int count,
                     void *workspace, size_t bytes, BC250_DML201_PLAN *plan)
{
    BC250_DML201_WORKSPACE *w = (BC250_DML201_WORKSPACE *)workspace;
    struct display_mode_lib *lib;
    display_e2e_pipe_params_st *p;
    BC250_DML201_PLAN *o;
    unsigned int vlevel, comb, k, level, n;
    int result = BC250_DML201_UNSUPPORTED;
    if (!plan) return BC250_DML201_INVALID;
    memset(plan,0,count*sizeof(*plan));
    if (!validInput(input,planes==NULL) || !workspace || bytes < sizeof(*w) || ((size_t)workspace & 7u))
        return BC250_DML201_INVALID;
    memset(w,0,sizeof(*w));
    lib=&w->Lib; p=w->Pipe; o=w->Plan;
    lib->logger=&w->Logger; lib->ip=dcn201_ip; lib->soc=dcn201_soc;
    lib->project=DML_PROJECT_DCN201;
    /* One physical DPP per supplied plane, one shared OTG. No hidden split. */
    lib->ip.max_num_dpp=count; lib->ip.max_num_otg=1;
    lib->soc.dispclk_dppclk_vco_speed_mhz=input->DentistVcoKhz/1000.0;
    lib->funcs.validate=dml20_ModeSupportAndSystemConfigurationFull;
    lib->funcs.recalculate=dml20_recalculate;
    lib->funcs.rq_dlg_get_dlg_reg=dml20_rq_dlg_get_dlg_reg;
    lib->funcs.rq_dlg_get_rq_reg=dml20_rq_dlg_get_rq_reg;
    for(n=0;n<count;++n) {
        if (planes) {
            if (!groupPipeInput(input,&planes[n],&p[n])) return BC250_DML201_INVALID;
            /* AMD budgets a cursor on each RGB pipe. Keep that conservative
             * accounting until the hardware cursor placement is established. */
        } else pipeInput(input,&p[n]);
    }
    vlevel=dml_get_voltage_level(lib,p,count); comb=lib->vba.maxMpcComb;
    if (vlevel >= lib->soc.num_states || comb > 1 ||
        lib->vba.NumberOfActivePlanes!=count ||
        !lib->vba.ModeSupport[vlevel][comb]) goto done;
    for(n=0;n<count;++n) {
        if (lib->vba.DPPPerPlane[n]!=1 || lib->vba.BlendingAndTiming[n]!=0 ||
            lib->vba.ODMCombineEnabled[n]!=dm_odm_combine_mode_disabled) goto done;
        o[n].VoltageLevel=vlevel; o[n].BboxDefaults=1;
        p[n].clks_cfg.dispclk_mhz=lib->vba.RequiredDISPCLK[vlevel][comb];
        p[n].clks_cfg.dppclk_mhz=lib->vba.RequiredDPPCLK[vlevel][comb][n];
        if (!(p[n].clks_cfg.dispclk_mhz>0.0 && p[n].clks_cfg.dppclk_mhz>0.0)) goto done;
    }
    /* AMD computes B,C,D first and restores the selected A state last. */
    for(k=1;k<=4;++k) {
        unsigned int index=k==4?0:k;
        unsigned int clockLevel;
        level=vlevel>index?vlevel:index;
        /* Match dcn20_calculate_wm: state D uses state-2 DCF/SOC clocks
         * when the selected level is below D. DCN201 has equal 2/3 values. */
        clockLevel=index==3 && vlevel<3?2:level;
        p->clks_cfg.voltage=level;
        p->clks_cfg.dcfclk_mhz=lib->soc.clock_limits[clockLevel].dcfclk_mhz;
        p->clks_cfg.socclk_mhz=lib->soc.clock_limits[clockLevel].socclk_mhz;
        if (!watermarks(lib,p,count,&o->Watermarks[index])) goto done;
    }
    for(n=0;n<count;++n) {
        if (!asUint(get_vstartup(lib,p,count,n),&o[n].VStartup) ||
            !asUint(get_vupdate_offset(lib,p,count,n),&o[n].VUpdateOffset) ||
            !asUint(get_vupdate_width(lib,p,count,n),&o[n].VUpdateWidth) ||
            !asUint(get_vready_offset(lib,p,count,n),&o[n].VReady)) goto done;
        p[n].pipe.dest.vstartup_start=o[n].VStartup;
        p[n].pipe.dest.vupdate_offset=o[n].VUpdateOffset;
        p[n].pipe.dest.vupdate_width=o[n].VUpdateWidth;
        p[n].pipe.dest.vready_offset=o[n].VReady;
    }
    for(n=0;n<count;++n) {
        o[n].PStateSupported=lib->vba.DRAMClockChangeSupport[vlevel][comb]!=dm_dram_clock_change_unsupported;
        o[n].CStateEnabled=0; /* Explicit AMD DCN201 policy, dcn20_fpu.c:1230. */
        lib->funcs.rq_dlg_get_dlg_reg(lib,&o[n].Dlg,&o[n].Ttu,p,count,n,false,
            (bool)o[n].PStateSupported,false,false,true);
        lib->funcs.rq_dlg_get_rq_reg(lib,&o[n].Rq,&p[n].pipe);
        if (!ceilUint(p[n].clks_cfg.dispclk_mhz*1000,&o[n].DispClockKhz) ||
            !ceilUint(p[n].clks_cfg.dppclk_mhz*1000,&o[n].DppClockKhz) ||
            !ceilUint(lib->vba.DCFCLK*1000,&o[n].DcfClockKhz) ||
            !ceilUint(lib->vba.SOCCLK*1000,&o[n].SocClockKhz) ||
            !ceilUint(lib->vba.DCFCLKDeepSleep*1000,&o[n].DcfDeepSleepKhz) ||
            !ceilUint(lib->vba.FabricClock*1000,&o[n].FabricClockKhz) ||
            !ceilUint(lib->vba.DRAMSpeed*1000/16,&o[n].DramClockKhz)) goto done;
        if (!o[n].VStartup || o[n].VStartup>=input->VTotal-input->VActive ||
            !o[n].VUpdateWidth || !o[n].DispClockKhz || !o[n].DppClockKhz) goto done;
        o[n].Destination=p[n].pipe.dest;
        if (n) memcpy(o[n].Watermarks,o[0].Watermarks,sizeof(o[n].Watermarks));
    }
    result=BC250_DML201_OK;
done:
    if (w->Logger.Assertions) result=BC250_DML201_ASSERTION;
    if (result==BC250_DML201_OK) {
        for(n=0;n<count;++n) o[n].Valid=1;
        memcpy(plan,o,count*sizeof(*plan));
    }
    plan->Assertions=w->Logger.Assertions;
    plan->FirstAssertionLine=w->Logger.FirstLine;
    return result;
}

int Bc250Dml201Plan(const BC250_DML201_INPUT *input, void *workspace,
                   size_t bytes, BC250_DML201_PLAN *plan)
{
    return planPipes(input,NULL,1,workspace,bytes,plan);
}

int Bc250Dml201PlanGroup(const BC250_DML201_INPUT *timing,
                        const BC250_DML201_PLANE_INPUT *planes, unsigned int count,
                        void *workspace, size_t bytes, BC250_DML201_GROUP_PLAN *plan)
{
    int status;
    if (!plan) return BC250_DML201_INVALID;
    memset(plan,0,sizeof(*plan));
    if (!planes || count<1 || count>2) return BC250_DML201_INVALID;
    status=planPipes(timing,planes,count,workspace,bytes,plan->Plane);
    plan->Assertions=plan->Plane[0].Assertions;
    plan->FirstAssertionLine=plan->Plane[0].FirstAssertionLine;
    if (status==BC250_DML201_OK) { plan->Count=count; plan->Valid=1; }
    return status;
}
