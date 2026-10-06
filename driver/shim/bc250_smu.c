/* PROVENANCE: torvalds/linux v6.18 AMD MIT SMU command/response bodies.
 * Port: callbacks replace MMIO/udelay; monotonic bounded polling; hardware
 * bypass/debug halt are disabled. Owner assertion and IO failure propagation
 * are mandatory. Registers hold BAR byte offsets rather than Linux dwords. */
#include <stddef.h>
#include <string.h>
#include "bc250_smu.h"
#include "generated/smu_registers.h"
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
#undef EINVAL
#undef EOPNOTSUPP
#undef EIO
#undef EBUSY
#undef ETIME
#undef EREMOTEIO
#define EINVAL 22
#define EOPNOTSUPP 95
#define EIO 5
#define EBUSY 16
#define ETIME 62
#define EREMOTEIO 121
enum {SMU_FW_INIT,SMU_FW_RUNTIME,SMU_FW_HANG};
struct amdgpu_device {int no_hw_access;struct {unsigned int smu_debug_mask;} pm;};
struct smu_context {
    struct amdgpu_device *adev;
    struct bc250_smu *owner;
    struct bc250_smu_report *report;
    unsigned int msg_reg,param_reg,resp_reg;
    int smc_fw_state;
};
static unsigned int read_reg(struct smu_context *smu,unsigned int reg)
{
    unsigned int value=0;int status;
    if(smu->report->io_status)return ~0u;
    status=smu->owner->io.read(smu->owner->io.context,reg,&value);
    smu->report->reads++;
    if(status){smu->report->io_status=status;return ~0u;}
    return value;
}
static void write_reg(struct smu_context *smu,unsigned int reg,unsigned int value)
{
    int status;
    if(smu->report->io_status)return;
    status=smu->owner->io.write(smu->owner->io.context,reg,value);
    smu->report->writes++;
    if(status)smu->report->io_status=status;
    else if(reg==smu->msg_reg)smu->report->message_written=1;
}
static u32 __smu_cmn_poll_stat(struct smu_context *smu)
{
    const struct bc250_smu_io *io=&smu->owner->io;
    unsigned long long start=io->now_us(io->context);
    unsigned int attempt,value=0;
    for(attempt=0;attempt<smu->owner->timeout_us;attempt++) {
        value=read_reg(smu,smu->resp_reg);
        smu->report->polls++;
        if(value || smu->report->io_status)break;
        if(io->now_us(io->context)-start>=smu->owner->timeout_us)break;
        io->delay_us(io->context,1);smu->report->delays++;
    }
    /* Do not record a synthetic IO-error sentinel as a firmware response. */
    if(!smu->report->io_status) {
        if(smu->report->message_written)smu->report->response=value;
        else smu->report->prior_response=value;
    }
    return value;
}
#define RREG32(reg) ((void)adev,read_reg(smu,(reg)))
#define WREG32(reg,value) ((void)adev,write_reg(smu,(reg),(value)))
#define dev_err(...) ((void)0)
#define unlikely(x) (x)
#define SMU_DEBUG_HALT_ON_ERROR 1u
#define amdgpu_device_halt(adev) ((void)(adev))
#define WARN_ON(value) ((void)(value))
#pragma warning(push)
#pragma warning(disable:4100) /* imported reg2errno's unused smu parameter */
#include "smu_mailbox.inc"
#pragma warning(pop)

int bc250_smu_init_queue(struct bc250_smu *smu,const struct bc250_smu_io *io,
                         unsigned int timeout_us,int already_active,
                         unsigned int msg_reg,unsigned int param_reg,unsigned int resp_reg)
{
    if(!smu || !io || !io->owned || !io->read || !io->write || !io->now_us ||
       !io->delay_us || !timeout_us || timeout_us>2000000u)return BC250_SMU_INVALID;
    if(msg_reg==param_reg || msg_reg==resp_reg || param_reg==resp_reg)return BC250_SMU_INVALID;
    memset(smu,0,sizeof(*smu));smu->io=*io;smu->timeout_us=timeout_us;
    smu->firmware_state=already_active?SMU_FW_RUNTIME:SMU_FW_INIT;
    smu->msg_reg=msg_reg;smu->param_reg=param_reg;smu->resp_reg=resp_reg;
    return 0;
}
int bc250_smu_init(struct bc250_smu *smu,const struct bc250_smu_io *io,
                   unsigned int timeout_us,int already_active)
{
    return bc250_smu_init_queue(smu,io,timeout_us,already_active,
                                BC250_SMU_mmMP1_SMN_C2PMSG_66,BC250_SMU_mmMP1_SMN_C2PMSG_82,
                                BC250_SMU_mmMP1_SMN_C2PMSG_90);
}
int bc250_smu_message_locked(struct bc250_smu *owner,unsigned int message,
                             unsigned int parameter,struct bc250_smu_report *report)
{
    struct amdgpu_device adev={0};struct smu_context smu={0};int status;
    if(!report)return BC250_SMU_INVALID;
    memset(report,0,sizeof(*report));report->message=message;report->parameter=parameter;
    report->status=BC250_SMU_INVALID;
    if(!owner || !owner->io.owned || !owner->timeout_us || message>0xffffu)return report->status;
    if(!owner->msg_reg && !owner->param_reg && !owner->resp_reg)return report->status; // not initialized
    if(!owner->io.owned(owner->io.context)){report->status=BC250_SMU_NOT_OWNER;return report->status;}
    smu.adev=&adev;smu.owner=owner;smu.report=report;smu.smc_fw_state=owner->firmware_state;
    // The queue this instance drives (bc250_smu_init: the firmware's queue 0; bc250_smu_init_queue: any other).
    smu.msg_reg=owner->msg_reg;
    smu.param_reg=owner->param_reg;
    smu.resp_reg=owner->resp_reg;
    status=smu_cmn_send_msg_without_waiting(&smu,(uint16_t)message,parameter);
    if(!status && !report->io_status)status=smu_cmn_wait_for_response(&smu);
    if(!status && !report->io_status)smu_cmn_read_arg(&smu,&report->value);
    if(report->io_status)status=report->io_status;
    owner->firmware_state=smu.smc_fw_state;
    report->status=status;
    return status;
}
