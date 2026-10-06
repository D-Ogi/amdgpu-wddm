// Native KMD binding for the AMD mailbox transport; PnP requires explicit legacy-writer handover.
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
// The six mailbox offsets of the two queues, by name, and nothing else. Queue 3's three (0.7.210) come from the
// firmware's own queue descriptor table through bc250_cpu_queue3(); all three are already in the escape read
// allowlist (regs.generated.h), so the lab could read them before any message was ever sent.
static int MailboxOffsetAllowed(unsigned offset,int write)
{
    struct bc250_cpu_queue cpu;
    bc250_cpu_queue3(&cpu);
    if(offset==BC250_SMU_mmMP1_SMN_C2PMSG_90 || offset==BC250_SMU_mmMP1_SMN_C2PMSG_82 ||
       offset==cpu.resp_reg || offset==cpu.param_reg)return 1;
    // The two command registers are write-only to this driver: nothing reads them back.
    return write && (offset==BC250_SMU_mmMP1_SMN_C2PMSG_66 || offset==cpu.msg_reg);
}
static int MailboxRead(void* context,unsigned offset,unsigned* value)
{
    BC250_SMU_OWNER* owner=context;
    if(!OwnerHeld(owner))return -1;
    if(!MailboxOffsetAllowed(offset,0))return -22;
    *value=READ_REGISTER_ULONG((PULONG)&owner->Registers[offset/sizeof(ULONG)]);
    return 0;
}
static int MailboxWrite(void* context,unsigned offset,unsigned value)
{
    BC250_SMU_OWNER* owner=context;
    if(!OwnerHeld(owner))return -1;
    if(!MailboxOffsetAllowed(offset,1))return -22;
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
static void ClockDelay(void* context,unsigned usec)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(context);
    // The clock transaction's settle pause (bc250_clock.c): a sleep, not a stall. The owner is PASSIVE only
    // (OwnerBegin) and its lock a push lock, so the thread may wait; the wait may run to the timer resolution.
    interval.QuadPart=-10ll*(LONGLONG)usec;
    KeDelayExecutionThread(KernelMode,FALSE,&interval);
}
static int Temperature(void* context,int* value)
{
    BC250_SMU_OWNER* owner=context;
    ULONG raw;
    if(!OwnerHeld(owner))return -1;
    raw=READ_REGISTER_ULONG((PULONG)&owner->Registers[BC250_SMU_mmTHM_TCON_CUR_TMP/sizeof(ULONG)]);
    if(raw==MAXULONG)return -5;
    // M438/E30 validates this BAR path against SMN-backed k10temp before amdgpu.
    // M439 validates Windows READ_REG BAR/SMN comparison in full WDDM.
    *value=(int)((raw&THM_TCON_CUR_TMP__CUR_TEMP_MASK)>>THM_TCON_CUR_TMP__CUR_TEMP__SHIFT)*125;
    if(raw&THM_TCON_CUR_TMP__CUR_TEMP_RANGE_SEL_MASK)*value-=49000;
    return 0;
}
static int Message(void* context,unsigned message,unsigned parameter,unsigned* value)
{
    BC250_SMU_OWNER* owner=context;
    struct bc250_smu_report report;
    int result;
    // The allowlist of docs/hardware.md, enforced here and not only reviewed: nothing else reaches the mailbox.
    // This is the GFX clock domain, so it consults the clock list alone: a clock transaction can never send a CPU
    // message, and CpuDomainMessage below can never send a clock message (0.7.210, ADR 0020).
    if(!bc250_clock_message_allowed(message))return -22;
    result=bc250_smu_message_locked(&owner->Transport,message,parameter,&report);
    if(!result)*value=report.value;
    return result;
}

// ---- the CPU domain (0.7.210) ---------------------------------------------------------------------------------
static NTSTATUS ResultStatus(int result);
// One message of the CPU domain, with the owner lock already held by the caller. Both lists are consulted: the
// message against the queue and the direction, then its argument against its own form.
static int CpuDomainMessage(BC250_SMU_OWNER* owner,unsigned queue,unsigned message,unsigned parameter,int write,
                           unsigned* value,unsigned* response)
{
    struct bc250_smu_report report;
    int result;
    if(!bc250_cpu_message_allowed(queue,message,write))return -22;
    if(!bc250_cpu_argument_allowed(queue,message,parameter))return -22;
    if(queue==BC250_CPU_QUEUE_CPU && !owner->CpuOnline)return -19;
    result=bc250_smu_message_locked(queue==BC250_CPU_QUEUE_CPU?&owner->CpuTransport:&owner->Transport,
                                    message,parameter,&report);
    if(response)*response=report.response;
    if(!result && value)*value=report.value;
    return result;
}

BOOLEAN SmuCpuBegin(BC250_SMU_OWNER* owner)
{
    return InterlockedCompareExchange(&owner->CpuBusy,1,0)==0;
}

void SmuCpuEnd(BC250_SMU_OWNER* owner)
{
    InterlockedExchange(&owner->CpuBusy,0);
}

NTSTATUS SmuCpuMessage(BC250_SMU_OWNER* owner,ULONG queue,ULONG message,ULONG parameter,BOOLEAN write,
                       BOOLEAN allowHot,ULONG busyPermille,ULONG* value,LONG* temperatureMc,
                       ULONG* firmwareStatus,BOOLEAN* temperatureValid)
{
    int degrees=0,result;
    unsigned read=0,response=0;
    if(value)*value=0;
    if(temperatureMc)*temperatureMc=0;
    if(firmwareStatus)*firmwareStatus=0;
    if(temperatureValid)*temperatureValid=FALSE;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    // "No mailbox traffic during sustained compute" (docs/design/rejected-options.md). The caller passes the
    // governor's own share, so a combined CPU and GPU test applies its values first and loads second.
    if(busyPermille>=BC250_CPU_GPU_BUSY_PERMILLE)return STATUS_DEVICE_BUSY;
    result=OwnerBegin(owner);
    if(result)return ResultStatus(result);
    // Before every message, not once per sequence: a sequence is several hundred milliseconds long and the part
    // can cross 87 C inside one.
    result=Temperature(owner,&degrees);
    if(!result) { if(temperatureMc)*temperatureMc=degrees;if(temperatureValid)*temperatureValid=TRUE; }
    // A temperature this owner cannot read refuses every ordinary message, and never the way back: a restore
    // that waits for a reading that does not come leaves an untested operating point in the chip (0.7.211).
    else if(allowHot) { degrees=0;result=0; }
    if(!result) {
        // The hot gate. A getter changes nothing and AllowHot names the steps that lower the dissipation and
        // the way back; everything else waits for the part to cool, exactly as before 0.7.211.
        if(degrees>=BC250_CLOCK_HOT_MC && write && !allowHot)result=BC250_CLOCK_TOO_HOT;
        else result=CpuDomainMessage(owner,queue,message,parameter,write?1:0,&read,&response);
    }
    OwnerEnd(owner);
    if(firmwareStatus)*firmwareStatus=response;
    if(!result && value)*value=read;
    return ResultStatus(result);
}
static NTSTATUS ResultStatus(int result)
{
    switch(result) {
    case 0:return STATUS_SUCCESS;
    case -19:return STATUS_DEVICE_NOT_READY;
    case -22:return STATUS_INVALID_PARAMETER;
    case -62:return STATUS_IO_TIMEOUT;
    case BC250_CLOCK_TOO_HOT:return STATUS_DEVICE_POWER_FAILURE;
    case BC250_CLOCK_MISMATCH:
    case BC250_CLOCK_STATE_INVALID:return STATUS_DEVICE_DATA_ERROR;
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
    if(!result) {
        unsigned version=0;
        owner->Registers=registers;owner->Online=TRUE;
        owner->Caller=PsGetCurrentThread();
        // The BIOS owns this image. Query once per owner start, through the
        // same transport/lock as clock control (smu_cmn_get_smc_version).
        result=Message(owner,PPSMC_MSG_GetSmuVersion,0,&version);
        owner->Caller=NULL;
        if(!result)InterlockedExchange64(&owner->FirmwareSnapshot,
            (LONG64)((1ull<<32)|(ULONGLONG)version));
        else {owner->Online=FALSE;owner->Registers=NULL;}
        if(!result) {
            // The second transport instance, over the same mapping and the same owner lock (0.7.210). It sends
            // nothing here: a queue that has never been spoken to on this part is proven by driver/kmd/cpu.c's
            // read stage, not by an init. already_active=1 for the same reason as queue 0: the BIOS owns the
            // image, so the previous response is checked and drained before the first write.
            struct bc250_cpu_queue q;
            bc250_cpu_queue3(&q);
            owner->CpuOnline=bc250_smu_init_queue(&owner->CpuTransport,&io,20000,1,
                                                  q.msg_reg,q.param_reg,q.resp_reg)==0;
            InterlockedExchange(&owner->CpuBusy,0);
        }
    }
    OwnerUnlock(owner);
    return ResultStatus(result);
}
void SmuOwnerStop(BC250_SMU_OWNER* owner)
{
    NT_ASSERT(KeGetCurrentIrql()==PASSIVE_LEVEL);
    OwnerLock(owner); // waits for the entire outstanding clock transaction
    owner->Online=FALSE;
    owner->CpuOnline=FALSE;
    owner->Registers=NULL;
    OwnerUnlock(owner);
}
NTSTATUS SmuReadFirmwareVersion(BC250_SMU_OWNER* owner,ULONG* version)
{
    ULONGLONG snapshot;
    *version=0;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    // QueryAdapterInfo may overlap SetPowerState (MS Level Three exception).
    // Metadata is independent of mailbox availability. Atomic publication avoids
    // both a torn valid/version pair and waiting behind a power/clock transaction.
    snapshot=(ULONGLONG)InterlockedCompareExchange64(&owner->FirmwareSnapshot,0,0);
    if(!(snapshot&(1ull<<32)))return STATUS_DEVICE_NOT_READY;
    *version=(ULONG)snapshot;
    return STATUS_SUCCESS;
}
NTSTATUS SmuPrepareClock(BC250_SMU_OWNER* owner,struct bc250_clock_report* report)
{
    struct bc250_clock_io io={owner,OwnerBegin,OwnerEnd,Temperature,Message,ClockDelay};
    return ResultStatus(bc250_clock_prepare(&io,BC250_CLOCK_FLOOR_MHZ,BC250_CLOCK_FLOOR_MV,report));
}
NTSTATUS SmuSetPoint(BC250_SMU_OWNER* owner,ULONG mhz,ULONG mv,struct bc250_clock_report* report)
{
    struct bc250_clock_io io={owner,OwnerBegin,OwnerEnd,Temperature,Message,ClockDelay};
    return ResultStatus(bc250_clock_prepare(&io,mhz,mv,report));
}
NTSTATUS SmuReadTemperature(BC250_SMU_OWNER* owner,LONG* temperature)
{
    int degrees=0,result;
    *temperature=0;
    result=OwnerBegin(owner);
    if(result)return ResultStatus(result);
    result=Temperature(owner,&degrees); // one BAR read, no mailbox message
    OwnerEnd(owner);
    if(!result)*temperature=degrees;
    return ResultStatus(result);
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

void SmuClockRequest(BC250_SMU_OWNER* owner, BC250_ESCAPE_CLOCK* data,
    BOOLEAN administrator, BOOLEAN hardwareAccess, BOOLEAN noAdapterSynchronization)
{
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    struct bc250_clock_report report;
    struct bc250_clock_io io={owner,OwnerBegin,OwnerEnd,Temperature,Message,ClockDelay};
    data->Version=BC250_KMD_VERSION;
    data->Status=BC250_ESCAPE_STATUS_REFUSED;
    data->ObservedMHz=data->ObservedVid=0;
    data->TemperatureMc=0;
    data->InitialMHz=data->InitialVid=data->ExpectedVid=data->VoltageStaged=data->Ready=0;
    if(!administrator) { data->Status=BC250_ESCAPE_STATUS_NOT_ADMIN;status=STATUS_ACCESS_DENIED;goto Done; }
    if(data->Magic!=BC250_ESCAPE_MAGIC || data->Command!=BC250_ESCAPE_RUN_CLOCK ||
       data->AbiVersion!=BC250_CLOCK_ABI || data->Reserved[0] || data->Reserved[1] || data->Reserved[2])goto Done;
    if(data->Op==BC250_CLOCK_OP_READ) {
        if(hardwareAccess || !noAdapterSynchronization || data->RequestedMHz || data->RequestedMv)goto Done;
        status=SmuReadClock(owner,&data->ObservedMHz,&data->ObservedVid,&data->TemperatureMc);
        data->Ready=NT_SUCCESS(status)?1u:0u;
    } else if(data->Op==BC250_CLOCK_OP_SET) {
        if(!hardwareAccess || noAdapterSynchronization)goto Done;
        // The DPM governor owns the operating point while it runs (dpm.c); two writers would fight.
        if(InterlockedCompareExchange(&owner->GovernorActive,0,0)) { status=STATUS_DEVICE_BUSY;goto Done; }
        status=ResultStatus(bc250_clock_prepare(&io,data->RequestedMHz,data->RequestedMv,&report));
        data->ObservedMHz=report.observed_mhz;data->ObservedVid=report.observed_vid;
        data->TemperatureMc=report.temperature_mc;
        data->InitialMHz=report.initial_mhz;data->InitialVid=report.initial_vid;
        data->ExpectedVid=report.expected_vid;data->VoltageStaged=report.voltage_staged;
        data->Ready=report.ready;
    }
Done:
    data->NtStatus=(ULONG)status;
    if(NT_SUCCESS(status))data->Status=BC250_ESCAPE_STATUS_DONE;
}
