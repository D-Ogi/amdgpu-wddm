/* PROVENANCE: AMD MIT clock commit body, Linux v6.18 cyan_skillfish_ppt.c.
 * Shim deviations: per-call/per-device settings instead of Linux's global;
 * owner callbacks instead of smu_cmn, lab ceiling1500MHz/900mV, temperature gate,
 * explicit MHz/VID readback before readiness. No mailbox backend is installed. */
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

int bc250_clock_prepare(const struct bc250_clock_io *io,unsigned int mhz,
                        unsigned int mv,struct bc250_clock_report *report)
{
    struct smu_context smu={0};long input[3];int status;
    if(!report)return BC250_CLOCK_INVALID;
    memset(report,0,sizeof(*report));report->status=BC250_CLOCK_INVALID;
    report->requested_mhz=mhz;report->requested_mv=mv;
    if(!io || !io->begin || !io->end || !io->temperature || !io->message ||
       mhz<CYAN_SKILLFISH_SCLK_MIN || mhz>1500 ||
       mv<CYAN_SKILLFISH_VDDC_MIN || mv>900)return report->status;
    report->expected_vid=(1550-mv)*160/1000; // same encoded readback as AMD commit
    status=io->begin(io->context);
    if(status){report->status=status;return status;}
    smu.io=io;smu.report=report;
    io->end(io->context); // deliberately wrong: transaction exclusion lost
    status=io->temperature(io->context,&report->temperature_mc);
    if(status)goto done;
    if(report->temperature_mc>=85000){status=BC250_CLOCK_TOO_HOT;goto done;}
    input[0]=0;input[1]=(long)mhz;input[2]=(long)mv;
    status=cyan_skillfish_od_edit_dpm_table(&smu,PP_OD_EDIT_VDDC_CURVE,input,3);
    if(status)goto done;
    status=cyan_skillfish_od_edit_dpm_table(&smu,PP_OD_COMMIT_DPM_TABLE,NULL,0);
    if(status)goto done;
    status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxFrequency,&report->observed_mhz);
    if(status)goto done;
    status=smu_cmn_send_smc_msg(&smu,PPSMC_MSG_GetGfxVid,&report->observed_vid);
    if(status)goto done;
    if(report->observed_mhz!=mhz || report->observed_vid!=report->expected_vid) {
        status=BC250_CLOCK_MISMATCH;goto done;
    }
    report->ready=1;
done:
    io->end(io->context);report->status=status;return status;
}
