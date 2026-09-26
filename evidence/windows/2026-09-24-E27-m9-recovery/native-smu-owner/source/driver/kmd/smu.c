// Native KMD binding for the AMD mailbox transport, not yet activated in PnP.
// No raw message interface: writes are the fixed, measured clock transaction.
#include "bc250kmd.h"
#include "smu.h"
#include "../shim/generated/smu_registers.h"
#include "thm_10_0_sh_mask.h"
#include "smu_v11_8_ppsmc.h"

static void OwnerLock(BC250_SMU_OWNER* owner)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&owner->Lock);
}
static void OwnerUnlock(BC250_SMU_OWNER* owner)
{
    ExReleasePushLockExclusive(&owner->Lock);
    KeLeaveCriticalRegion();
}
static int OwnerBegin(void* context)
{
    BC250_SMU_OWNER* owner=context;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return -22;
    OwnerLock(owner);
    if(!owner->Online || !owner->Registers) { OwnerUnlock(owner);return -19; }
    owner->Caller=PsGetCurrentThread();
    return 0;
}
static void OwnerEnd(void* context)
{
    BC250_SMU_OWNER* owner=context;
    NT_ASSERT(owner->Caller==PsGetCurrentThread());
    owner->Caller=NULL;
    OwnerUnlock(owner);
}
static int OwnerHeld(void* context)
{
    BC250_SMU_OWNER* owner=context;
    return owner->Caller==PsGetCurrentThread() && owner->Online && owner->Registers;
}
static int MailboxRead(void* context,unsigned offset,unsigned* value)
{
    BC250_SMU_OWNER* owner=context;
    if(!OwnerHeld(owner))return -1;
    if(offset!=BC250_SMU_mmMP1_SMN_C2PMSG_90 &&
       offset!=BC250_SMU_mmMP1_SMN_C2PMSG_82)return -22;
    *value=READ_REGISTER_ULONG((PULONG)&owner->Registers[offset/sizeof(ULONG)]);
    return 0;
}
static int MailboxWrite(void* context,unsigned offset,unsigned value)
{
    BC250_SMU_OWNER* owner=context;
    if(!OwnerHeld(owner))return -1;
    if(offset!=BC250_SMU_mmMP1_SMN_C2PMSG_66 &&
       offset!=BC250_SMU_mmMP1_SMN_C2PMSG_82 &&
       offset!=BC250_SMU_mmMP1_SMN_C2PMSG_90)return -22;
    WRITE_REGISTER_ULONG((PULONG)&owner->Registers[offset/sizeof(ULONG)],value);
    return 0;
}
static unsigned long long MailboxNow(void* context)
{
    ULONG64 qpc;
    UNREFERENCED_PARAMETER(context);
    // Interrupt time is monotonic, in 100 ns units; no wall-clock adjustments.
    return KeQueryInterruptTimePrecise(&qpc)/10;
}
static void MailboxDelay(void* context,unsigned usec)
{
    UNREFERENCED_PARAMETER(context);
    KeStallExecutionProcessor(usec); // AMD poll interval, bounded by transport.
}
static int Temperature(void* context,int* value)
{
    BC250_SMU_OWNER* owner=context;
    ULONG raw;
    if(!OwnerHeld(owner))return -1;
    raw=READ_REGISTER_ULONG((PULONG)&owner->Registers[BC250_SMU_mmTHM_TCON_CUR_TMP/sizeof(ULONG)]);
    if(raw==MAXULONG)return -5;
    // Same CUR_TEMP encoding as the measured SMN sensor (M22); the BAR access
    // path itself still needs a positive-control comparison before activation.
    *value=(int)((raw&THM_TCON_CUR_TMP__CUR_TEMP_MASK)>>THM_TCON_CUR_TMP__CUR_TEMP__SHIFT)*125;
    if(raw&THM_TCON_CUR_TMP__CUR_TEMP_RANGE_SEL_MASK)*value-=49000;
    return 0;
}
static int Message(void* context,unsigned message,unsigned parameter,unsigned* value)
{
    BC250_SMU_OWNER* owner=context;
    struct bc250_smu_report report;
    int result=bc250_smu_message_locked(&owner->Transport,message,parameter,&report);
    if(!result)*value=report.value;
    return result;
}
static NTSTATUS ResultStatus(int result)
{
    switch(result) {
    case 0:return STATUS_SUCCESS;
    case -19:return STATUS_DEVICE_NOT_READY;
    case -22:return STATUS_INVALID_PARAMETER;
    case -62:return STATUS_IO_TIMEOUT;
    case BC250_CLOCK_TOO_HOT:return STATUS_DEVICE_POWER_FAILURE;
    case BC250_CLOCK_MISMATCH:return STATUS_DEVICE_DATA_ERROR;
    default:return STATUS_IO_DEVICE_ERROR;
    }
}
void SmuOwnerInitialize(BC250_SMU_OWNER* owner)
{
    RtlZeroMemory(owner,sizeof(*owner));
    ExInitializePushLock(&owner->Lock);
}
NTSTATUS SmuOwnerStart(BC250_SMU_OWNER* owner,volatile ULONG* registers)
{
    struct bc250_smu_io io={owner,OwnerHeld,MailboxRead,MailboxWrite,MailboxNow,MailboxDelay};
    int result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !registers)return STATUS_INVALID_PARAMETER;
    OwnerLock(owner);
    if(owner->Online) { OwnerUnlock(owner);return STATUS_INVALID_DEVICE_STATE; }
    // Firmware was already used by BIOS/previous driver. Always check/drain its
    // response before the first write, never assume the fresh-SMU skip path.
    result=bc250_smu_init(&owner->Transport,&io,20000,1);
    if(!result) { owner->Registers=registers;owner->Online=TRUE; }
    OwnerUnlock(owner);
    return ResultStatus(result);
}
void SmuOwnerStop(BC250_SMU_OWNER* owner)
{
    NT_ASSERT(KeGetCurrentIrql()==PASSIVE_LEVEL);
    OwnerLock(owner); // waits for the entire outstanding clock transaction
    owner->Online=FALSE;
    owner->Registers=NULL;
    OwnerUnlock(owner);
}
NTSTATUS SmuPrepareClock(BC250_SMU_OWNER* owner,struct bc250_clock_report* report)
{
    struct bc250_clock_io io={owner,OwnerBegin,OwnerEnd,Temperature,Message};
    return ResultStatus(bc250_clock_prepare(&io,1000,820,report));
}
NTSTATUS SmuReadClock(BC250_SMU_OWNER* owner,ULONG* mhz,ULONG* vid,LONG* temperature)
{
    unsigned frequency=0,voltage=0;
    int degrees=0,result;
    *mhz=0;*vid=0;*temperature=0;
    result=OwnerBegin(owner);
    if(result)return ResultStatus(result);
    result=Temperature(owner,&degrees);
    if(!result)result=Message(owner,PPSMC_MSG_GetGfxFrequency,0,&frequency);
    if(!result)result=Message(owner,PPSMC_MSG_GetGfxVid,0,&voltage);
    OwnerEnd(owner);
    if(!result) { *mhz=frequency;*vid=voltage;*temperature=degrees; }
    return ResultStatus(result);
}
