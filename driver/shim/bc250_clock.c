/* PROVENANCE: AMD MIT clock commit body, Linux v6.18 cyan_skillfish_ppt.c.
 * Shim deviations: per-call/per-device settings instead of Linux's global;
 * owner callbacks instead of smu_cmn, the operating-point table of bc250_clock.h
 * (1000-2000 MHz, 820-1000 mV, docs/design/dpm.md), a temperature gate on raises,
 * explicit MHz/VID readback (re-read while a raise ramps) and voltage-up staging before AMD commit. */
#include <stddef.h>
#include <string.h>
#include "bc250_clock.h"
#include "smu_v11_8_ppsmc.h"

typedef unsigned int uint32_t;
// Keep imported Linux errno values identical in host and WDK builds.
#undef EINVAL
#undef EOPNOTSUPP
#define EINVAL 22
#define EOPNOTSUPP 95
#define dev_err(...) ((void)0)
/* Local enum only selects branches in the imported function; not a wire ABI. */
enum PP_OD_DPM_TABLE_COMMAND {
    PP_OD_EDIT_VDDC_CURVE, PP_OD_RESTORE_DEFAULT_TABLE, PP_OD_COMMIT_DPM_TABLE
};
#define SMU_MSG_RequestGfxclk PPSMC_MSG_RequestGfxclk
#define SMU_MSG_ForceGfxVid PPSMC_MSG_ForceGfxVid
#define SMU_MSG_UnforceGfxVid PPSMC_MSG_UnforceGfxVid
struct smu_context {
    const struct bc250_clock_io *io;
    struct bc250_clock_report *report;
    struct {uint32_t sclk,vddc;} settings;
    uint32_t default_sclk;
};
static int smu_cmn_send_smc_msg_with_param(struct smu_context *smu,
                                          unsigned int msg,unsigned int param,
                                          unsigned int *out)
{
    unsigned int ignored=0;int result;
    smu->report->messages_attempted++;
    result=smu->io->message(smu->io->context,msg,param,out?out:&ignored);
    if(!result)smu->report->messages_completed++;
    return result;
}
static int smu_cmn_send_smc_msg(struct smu_context *smu,unsigned int msg,unsigned int *out)
{return smu_cmn_send_smc_msg_with_param(smu,msg,0,out);}
#define cyan_skillfish_user_settings (smu->settings)
#define cyan_skillfish_sclk_default (smu->default_sclk)
#include "cyan_skillfish_clock.inc"
#undef cyan_skillfish_user_settings
#undef cyan_skillfish_sclk_default

// The table of bc250_clock.h: mV = the anchors' line at that clock, rounded up; vid = its encoding.
// driver/shim/test/dpm_test.c recomputes every row from the three anchors.
const struct bc250_clock_point bc250_clock_points[BC250_CLOCK_LEVELS]={
    {1000,820,116},{1100,840,113},{1200,860,110},{1300,880,107},{1400,899,104},{1500,919,100},
    {1600,935,98},{1700,952,95},{1800,968,93},{1900,984,90},{2000,1000,88}
};
unsigned int bc250_clock_min_mv(unsigned int mhz)
{
    if(mhz<BC250_CLOCK_FLOOR_MHZ || mhz>BC250_CLOCK_CEILING_MHZ || (mhz-BC250_CLOCK_FLOOR_MHZ)%BC250_CLOCK_STEP_MHZ)
        return 0;
    return bc250_clock_points[(mhz-BC250_CLOCK_FLOOR_MHZ)/BC250_CLOCK_STEP_MHZ].mv;
}
int bc250_clock_point_allowed(unsigned int mhz,unsigned int mv)
{
    unsigned int least=bc250_clock_min_mv(mhz);
    return least && mv>=least && mv<=BC250_CLOCK_CEILING_MV;
}
int bc250_clock_message_allowed(unsigned int message)
{
    // GetSmuVersion once per owner start; the rest is this file's transaction and its readback.
    // UnforceGfxVid is deliberately absent: the firmware's own voltage for a requested clock is
    // not measured on this part (docs/design/dpm.md), so every clock comes with a forced VID.
    switch(message) {
    case PPSMC_MSG_GetSmuVersion:
    case PPSMC_MSG_RequestGfxclk:
    case PPSMC_MSG_ForceGfxVid:
    case PPSMC_MSG_GetGfxFrequency:
    case PPSMC_MSG_GetGfxVid:
        return 1;
    default:
        return 0;
    }
}

// A readback between the clock the transaction found and the one it requested, the request itself excluded.
static int on_the_way(unsigned int observed,unsigned int from,unsigned int to)
{
    return from<to ? observed>=from && observed<to : observed<=from && observed>to;
}

int bc250_clock_prepare(const struct bc250_clock_io *io,unsigned int mhz,
                        unsigned int mv,struct bc250_clock_report *report)
{
    struct smu_context smu={0};long input[3];int status;
    if(!report)return BC250_CLOCK_INVALID;
    memset(report,0,sizeof(*report));report->status=BC250_CLOCK_INVALID;
    report->requested_mhz=mhz;report->requested_mv=mv;
    // The table is narrower than AMD's range (1000-2000 MHz, 700-1129 mV), never wider.
    if(!io || !io->begin || !io->end || !io->temperature || !io->message ||
       mhz<CYAN_SKILLFISH_SCLK_MIN || mhz>CYAN_SKILLFISH_SCLK_MAX ||
       mv<CYAN_SKILLFISH_VDDC_MIN || mv>CYAN_SKILLFISH_VDDC_MAX ||
       !bc250_clock_point_allowed(mhz,mv))return report->status;
    report->expected_vid=bc250_clock_vid(mv); // same encoded readback as AMD commit
    status=io->begin(io->context);
    if(status){report->status=status;return status;}
    smu.io=io;smu.report=report;
    status=io->temperature(io->context,&report->temperature_mc);
    if(status)goto done;
    input[0]=0;input[1]=(long)mhz;input[2]=(long)mv;
    status=cyan_skillfish_od_edit_dpm_table(&smu,PP_OD_EDIT_VDDC_CURVE,input,3);
    if(status)goto done;
    status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxFrequency,&report->initial_mhz);
    if(status)goto done;
    status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxVid,&report->initial_vid);
    if(status)goto done;
    if(!report->initial_mhz || report->initial_vid>255) {
        status=BC250_CLOCK_STATE_INVALID;goto done;
    }
    // Hot: only a transition that raises neither clock nor voltage (lower VID = higher voltage).
    // Before any request, so a refused raise leaves the operating point as it was.
    if(report->temperature_mc>=BC250_CLOCK_HOT_MC &&
       (mhz>report->initial_mhz || report->expected_vid<report->initial_vid)) {
        status=BC250_CLOCK_TOO_HOT;goto done;
    }
    // WDDM adaptation around the unchanged AMD frequency-first commit: lower
    // VID means higher voltage. If voltage must rise, establish and read it
    // back before any frequency request. Otherwise retain the existing higher
    // voltage until the imported commit has changed frequency. Never roll a
    // failed transition back by raising frequency or lowering staged voltage.
    if(report->expected_vid<report->initial_vid) {
        status=smu_cmn_send_smc_msg_with_param(&smu,PPSMC_MSG_ForceGfxVid,report->expected_vid,NULL);
        if(status)goto done;
        status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxVid,&report->staged_vid);
        if(status)goto done;
        if(report->staged_vid!=report->expected_vid) {status=BC250_CLOCK_MISMATCH;goto done;}
        report->voltage_staged=1;
    }
    status=cyan_skillfish_od_edit_dpm_table(&smu,PP_OD_COMMIT_DPM_TABLE,NULL,0);
    if(status)goto done;
    status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxFrequency,&report->observed_mhz);
    if(status)goto done;
    // A raise ramps (BC250_CLOCK_SETTLE_READS); the voltage for the request is already in place, so every
    // clock on the way is covered. Wait for the exact request, bounded; what does not arrive is MISMATCH below.
    while(report->observed_mhz!=mhz && report->settle_reads<BC250_CLOCK_SETTLE_READS &&
          on_the_way(report->observed_mhz,report->initial_mhz,mhz)) {
        if(io->delay)io->delay(io->context,BC250_CLOCK_SETTLE_US);
        report->settle_reads++;
        status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxFrequency,&report->observed_mhz);
        if(status)goto done;
    }
    status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxVid,&report->observed_vid);
    if(status)goto done;
    if(report->observed_mhz!=mhz || report->observed_vid!=report->expected_vid) {
        status=BC250_CLOCK_MISMATCH;goto done;
    }
    report->ready=1;
done:
    io->end(io->context);report->status=status;return status;
}
