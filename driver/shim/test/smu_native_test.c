// Compile the actual native owner, replacing only Windows kernel primitives.
#define WIN32_NO_STATUS
#include "smu_native_mock.h"
#include "smu.h"
#include "smu-native.inc"
static volatile ULONG registers[0x80000/sizeof(ULONG)];
// Queue 3's three offsets come from the policy layer, like every other address in this tree.
#define BC250_CPU_Q3_MSG_TEST   (BC250_SMU_mmMP1_SMN_C2PMSG_66+6u*4u)
#define BC250_CPU_Q3_RESP_TEST  (BC250_SMU_mmMP1_SMN_C2PMSG_90+6u*4u)
#define BC250_CPU_Q3_PARAM_TEST (BC250_SMU_mmMP1_SMN_C2PMSG_90+8u*4u)
static BC250_SMU_OWNER owner;
static unsigned phase,pending,mhz,vid,command,argument,reply,ramp,ramp_left,ramp_from,core_mask=0x77u;
// The temperature the THM register reports, in its own units (536 = 67 C), and the firmware's queue 3 (0.7.210):
// its own phase, command, argument and response, because "check and drain the previous response before the first
// write" runs once per queue. cpu_value is what a getter answers; cpu_calls counts queue 3 messages alone.
static unsigned cur_tmp=536u;
static unsigned cpu_phase,cpu_pending,cpu_command,cpu_argument,cpu_reply=1u,cpu_value=1050u;
static LONG cpu_calls;
static LONG calls;
static unsigned smu_version=0x00580600u,version_refuse;
static HANDLE entered,release_write,stop_started,stop_done;
static volatile LONG hold_write;
ULONG NativeRead(PULONG address) {
    unsigned offset=(unsigned)((ULONG_PTR)address-(ULONG_PTR)registers);
    CHECK(OwnerHeld(&owner));
    if(offset==BC250_SMU_mmTHM_TCON_CUR_TMP) return (cur_tmp<<THM_TCON_CUR_TMP__CUR_TEMP__SHIFT); // 536 = 67 C
    if(offset==BC250_CPU_Q3_RESP_TEST) {        // queue 3's response register, draining like queue 0's
        if(cpu_pending && !--cpu_pending) { cpu_argument=cpu_value;cpu_reply=1u; }
        return cpu_reply;
    }
    if(offset==BC250_CPU_Q3_PARAM_TEST) { CHECK(!cpu_pending && cpu_reply==1);return cpu_argument; }
    if(offset==BC250_SMU_mmMP1_SMN_C2PMSG_90) {
        if(pending && !--pending) {
            switch(command) {
            case PPSMC_MSG_RequestGfxclk:ramp_left=ramp;ramp=0;ramp_from=mhz;mhz=argument;break;
            case PPSMC_MSG_ForceGfxVid:vid=argument;break;
            case PPSMC_MSG_GetGfxFrequency:argument=ramp_left?(ramp_left--,ramp_from+1):mhz;break; // "ramp" reads on the way
            case PPSMC_MSG_GetGfxVid:argument=vid;break;
            case PPSMC_MSG_GetSmuVersion:argument=smu_version;break;
            case BC250_CPU_MSG_SET_CORE_ENABLE_MASK:core_mask=argument;break; // the one CPU message on queue 0
            default:CHECK(0);
            }
            reply=(command==PPSMC_MSG_GetSmuVersion && version_refuse)?0xFEu:1u;
        }
        return reply;
    }
    CHECK(offset==BC250_SMU_mmMP1_SMN_C2PMSG_82 && !pending && reply==1);
    return argument;
}
void NativeWrite(PULONG address,ULONG value) {
    unsigned offset=(unsigned)((ULONG_PTR)address-(ULONG_PTR)registers);
    CHECK(OwnerHeld(&owner) && (!pending || offset==BC250_CPU_Q3_MSG_TEST ||
                               offset==BC250_CPU_Q3_RESP_TEST || offset==BC250_CPU_Q3_PARAM_TEST));
    if(offset==BC250_CPU_Q3_RESP_TEST) { CHECK(!cpu_phase && !value);cpu_reply=0;cpu_phase=1; }
    else if(offset==BC250_CPU_Q3_PARAM_TEST) { CHECK(cpu_phase==1);cpu_argument=value;cpu_phase=2; }
    else if(offset==BC250_CPU_Q3_MSG_TEST) {
        CHECK(cpu_phase==2);cpu_command=value;cpu_pending=3;cpu_phase=0;InterlockedIncrement(&cpu_calls);
    }
    else if(offset==BC250_SMU_mmMP1_SMN_C2PMSG_90) { CHECK(!phase && !value);reply=0;phase=1; }
    else if(offset==BC250_SMU_mmMP1_SMN_C2PMSG_82) { CHECK(phase==1);argument=value;phase=2; }
    else {
        CHECK(offset==BC250_SMU_mmMP1_SMN_C2PMSG_66 && phase==2);
        command=value;pending=3;phase=0;InterlockedIncrement(&calls);
        if(InterlockedExchange(&hold_write,0)) {
            SetEvent(entered);CHECK(WaitForSingleObject(release_write,5000)==WAIT_OBJECT_0);
        }
    }
}
static DWORD WINAPI Worker(void* ignored) {
    unsigned i;UNREFERENCED_PARAMETER(ignored);
    for(i=0;i<50;i++) {
        BC250_ESCAPE_CLOCK r={0};
        r.Magic=BC250_ESCAPE_MAGIC;r.Command=BC250_ESCAPE_RUN_CLOCK;r.AbiVersion=BC250_CLOCK_ABI;
        r.Op=BC250_CLOCK_OP_SET;r.RequestedMHz=1000;r.RequestedMv=820;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready && r.NtStatus==STATUS_SUCCESS);
        r.Op=BC250_CLOCK_OP_READ;r.RequestedMHz=r.RequestedMv=0;
        SmuClockRequest(&owner,&r,TRUE,FALSE,TRUE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready);
        CHECK(r.ObservedMHz==1000 && r.ObservedVid==116 && r.TemperatureMc==67000);
    }
    return 0;
}
static DWORD WINAPI Once(void* ignored) {
    struct bc250_clock_report r;UNREFERENCED_PARAMETER(ignored);
    CHECK(SmuPrepareClock(&owner,&r)==STATUS_SUCCESS && r.ready);return 0;
}
static DWORD WINAPI Stop(void* ignored) {
    UNREFERENCED_PARAMETER(ignored);SetEvent(stop_started);
    SmuOwnerStop(&owner);SetEvent(stop_done);return 0;
}
int main(void) {
    HANDLE threads[4],writer,stopper;
    ULONG f,v,version;LONG t;unsigned i;
    SmuOwnerInitialize(&owner);reply=1;mhz=1500;vid=104;
    CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_DEVICE_NOT_READY);
    CHECK(SmuReadTemperature(&owner,&t)==STATUS_DEVICE_NOT_READY && !t);
    CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_DEVICE_NOT_READY && !version);
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_INVALID_DEVICE_STATE);
    CHECK(calls==1);
    CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0x00580600u && calls==1);
    for(i=0;i<4;i++) { threads[i]=CreateThread(NULL,0,Worker,NULL,0,NULL);CHECK(threads[i]!=NULL); }
    CHECK(WaitForMultipleObjects(4,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(i=0;i<4;i++)CloseHandle(threads[i]);
    CHECK(calls==1601 && !owner.Caller);
    entered=CreateEvent(NULL,TRUE,FALSE,NULL);release_write=CreateEvent(NULL,TRUE,FALSE,NULL);
    stop_started=CreateEvent(NULL,TRUE,FALSE,NULL);stop_done=CreateEvent(NULL,TRUE,FALSE,NULL);
    hold_write=1;writer=CreateThread(NULL,0,Once,NULL,0,NULL);
    CHECK(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    stopper=CreateThread(NULL,0,Stop,NULL,0,NULL);
    CHECK(WaitForSingleObject(stop_started,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(stop_done,100)==WAIT_TIMEOUT); // close must join the whole transaction
    // The mailbox writer is deliberately held and Stop is waiting behind it.
    // Cached capability queries must finish without acquiring that owner lock.
    { LONG before=calls;
      CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0x00580600u);
      CHECK(calls==before); }
    SetEvent(release_write);
    CHECK(WaitForSingleObject(writer,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(stopper,5000)==WAIT_OBJECT_0);
    CHECK(!owner.Online && !owner.Registers && !owner.Caller && calls==1607);
    CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_DEVICE_NOT_READY);
    CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0x00580600u);
    CHECK(!f && !v && !t && calls==1607);
    smu_version=0x00580701u;
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
    CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0x00580701u);
    CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_SUCCESS && f==1000 && v==116);
    SmuOwnerStop(&owner);
    CloseHandle(writer);CloseHandle(stopper);CloseHandle(entered);CloseHandle(release_write);
    CloseHandle(stop_started);CloseHandle(stop_done);
    {
        BC250_ESCAPE_CLOCK r={0};LONG before=calls;
        C_ASSERT(sizeof(r)==80 && sizeof(r)>=sizeof(BC250_ESCAPE));
        r.Magic=BC250_ESCAPE_MAGIC;r.Command=BC250_ESCAPE_RUN_CLOCK;r.AbiVersion=BC250_CLOCK_ABI;
        SmuClockRequest(&owner,&r,TRUE,FALSE,TRUE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_REFUSED && r.NtStatus==(ULONG)STATUS_DEVICE_NOT_READY && !r.Ready && calls==before);
        CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
        r.Op=BC250_CLOCK_OP_SET;r.RequestedMHz=1500;r.RequestedMv=bc250_clock_floor_mv(1500)-1u;
        before=calls;
        // Under the band (0.7.210: the table's line less BC250_CURVE_UNDERVOLT_MV): refused, nothing sent.
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.NtStatus==(ULONG)STATUS_INVALID_PARAMETER && !r.Ready && calls==before);
        r.RequestedMv=919;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready && r.VoltageStaged);
        CHECK(r.InitialMHz==1000 && r.InitialVid==116 && r.ObservedMHz==1500 && r.ObservedVid==100 && r.ExpectedVid==100);
        r.RequestedMHz=2000;r.RequestedMv=1000;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE); // the DPM ceiling
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready && r.ObservedMHz==2000 && r.ObservedVid==88);
        // While the governor runs, the escape does not write.
        before=calls;owner.GovernorActive=1;r.RequestedMHz=1000;r.RequestedMv=820;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_REFUSED && r.NtStatus==(ULONG)STATUS_DEVICE_BUSY && !r.Ready && calls==before);
        r.Op=BC250_CLOCK_OP_READ;r.RequestedMHz=r.RequestedMv=0; // reading is still fine
        SmuClockRequest(&owner,&r,TRUE,FALSE,TRUE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.ObservedMHz==2000 && r.ObservedVid==88);
        owner.GovernorActive=0;r.Op=BC250_CLOCK_OP_SET;
        // The governor's own path, and its temperature read without a message.
        { struct bc250_clock_report p;LONG mc=0;unsigned ignored=0;
          CHECK(SmuSetPoint(&owner,1700,952,&p)==STATUS_SUCCESS && p.ready && p.observed_mhz==1700 && p.observed_vid==95);
          CHECK(!p.settle_reads && !native_settle_sleeps);
          // A raise that ramps: the owner's transaction sleeps before each re-read and ends at the request.
          ramp=3;
          CHECK(SmuSetPoint(&owner,1900,984,&p)==STATUS_SUCCESS && p.ready && p.observed_mhz==1900 && p.observed_vid==90);
          CHECK(p.settle_reads==3 && native_settle_sleeps==3 && p.voltage_staged && !ramp_left);
          CHECK(SmuSetPoint(&owner,1700,952,&p)==STATUS_SUCCESS && p.ready && !p.settle_reads);
          // One millivolt under the band at that clock: refused in the owner, before the transport.
          CHECK(SmuSetPoint(&owner,1700,bc250_clock_floor_mv(1700)-1u,&p)==STATUS_INVALID_PARAMETER && !p.ready);
          // Inside the band it goes through, which is how a curve's undervolt reaches the hardware.
          CHECK(SmuSetPoint(&owner,1700,bc250_clock_floor_mv(1700),&p)==STATUS_SUCCESS && p.ready &&
                p.observed_vid==bc250_clock_vid(bc250_clock_floor_mv(1700)));
          CHECK(SmuSetPoint(&owner,1700,952,&p)==STATUS_SUCCESS && p.ready);
          before=calls;
          CHECK(SmuReadTemperature(&owner,&mc)==STATUS_SUCCESS && mc==67000 && calls==before);
          // The allowlist is enforced in the owner, before the transport: nothing reaches the mailbox.
          owner.Caller=PsGetCurrentThread();
          CHECK(Message(&owner,PPSMC_MSG_UnforceGfxVid,0,&ignored)==-22);
          CHECK(Message(&owner,PPSMC_MSG_TransferTableSmu2Dram,6,&ignored)==-22);
          CHECK(Message(&owner,PPSMC_MSG_ForceGfxFreq,2000,&ignored)==-22);
          owner.Caller=NULL;
          CHECK(calls==before); }
        r.RequestedMHz=1000;r.RequestedMv=820;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready && !r.VoltageStaged && r.ObservedVid==116);
        before=calls;
        SmuClockRequest(&owner,&r,FALSE,TRUE,FALSE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_NOT_ADMIN && r.NtStatus==(ULONG)STATUS_ACCESS_DENIED && !r.Ready && !r.ObservedMHz && calls==before);
        SmuClockRequest(&owner,&r,TRUE,FALSE,TRUE);
        CHECK(r.NtStatus==(ULONG)STATUS_INVALID_PARAMETER && !r.Ready && calls==before); // mutation needs Level Two
        r.RequestedMHz=2000;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.NtStatus==(ULONG)STATUS_INVALID_PARAMETER && !r.Ready && calls==before);
        r.Op=BC250_CLOCK_OP_READ;r.RequestedMHz=r.RequestedMv=0;
        SmuClockRequest(&owner,&r,TRUE,FALSE,TRUE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready && r.ObservedMHz==1000 && r.ObservedVid==116);
        SmuOwnerStop(&owner);
    }
    // ---- the CPU domain, queue 3 (0.7.210, docs/design/tuner.md) ---------------------------------------
    // What is checked here is what keeps an inferred message from reaching the chip: the allowlist and the
    // argument ranges of the queue the caller names, the one-sequence flag, the GPU busy gate, the 87 C gate,
    // and that the two queues never borrow each other's mailbox.
    {
        ULONG value=0,fw=0;LONG mc=0,before,cpuBefore;
        struct bc250_cpu_queue q;
        bc250_cpu_queue3(&q);
        CHECK(q.msg_reg==BC250_CPU_Q3_MSG_TEST && q.resp_reg==BC250_CPU_Q3_RESP_TEST &&
              q.param_reg==BC250_CPU_Q3_PARAM_TEST);
        CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
        CHECK(owner.CpuOnline);                 // the instance exists; it never means queue 3 has answered
        CHECK(SmuCpuBegin(&owner) && !SmuCpuBegin(&owner));   // one CPU sequence at a time
        SmuCpuEnd(&owner);
        CHECK(SmuCpuBegin(&owner));
        before=calls;cpuBefore=cpu_calls;
        // Not on the allowlist of that queue, in that direction, or outside the argument's range: refused in
        // the owner, before the transport, and nothing reaches either mailbox.
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,0x98u,0,TRUE,0,NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,0,FALSE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,5000,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,1,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);   // a positive scale raises the voltage
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_GFX,BC250_CPU_MSG_SET_CORE_ENABLE_MASK,0x7Fu,TRUE,0,
                            NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);  // a harvest pattern
        CHECK(SmuCpuMessage(&owner,2u,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);   // no third queue
        // The clock transaction's own messages are not CPU messages (smu_v11_8_ppsmc.h).
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_GFX,PPSMC_MSG_RequestGfxclk,1500,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_GFX,PPSMC_MSG_ForceGfxVid,116,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(calls==before && cpu_calls==cpuBefore);
        // The GPU busy gate: at or above the share, nothing is sent at all.
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,
                            BC250_CPU_GPU_BUSY_PERMILLE,&value,NULL,NULL)==STATUS_DEVICE_BUSY);
        CHECK(cpu_calls==cpuBefore && !value);
        // A getter on queue 3: one message on queue 3's mailbox, the firmware's response word, the temperature.
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,&value,&mc,&fw)
              ==STATUS_SUCCESS);
        CHECK(value==cpu_value && mc==67000 && fw==1u && cpu_calls==cpuBefore+1 &&
              cpu_command==BC250_CPU_MSG_READ_CPU_MV && calls==before);   // queue 0 saw nothing
        // A setter on queue 3, and the core mask on queue 0: each argument reaches its own queue's register.
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CAP_C,95,TRUE,0,NULL,NULL,NULL)
              ==STATUS_SUCCESS);
        CHECK(cpu_command==BC250_CPU_MSG_SET_CAP_C && cpu_calls==cpuBefore+2 && calls==before);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_GFX,BC250_CPU_MSG_SET_CORE_ENABLE_MASK,BC250_CPU_MASK_FULL,
                            TRUE,0,NULL,NULL,NULL)==STATUS_SUCCESS);
        CHECK(command==BC250_CPU_MSG_SET_CORE_ENABLE_MASK && core_mask==BC250_CPU_MASK_FULL &&
              calls==before+1 && cpu_calls==cpuBefore+2);
        // The 87 C gate, read before every message and not once per sequence.
        cur_tmp=696u;
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,&value,&mc,NULL)
              ==STATUS_DEVICE_POWER_FAILURE);
        CHECK(cpu_calls==cpuBefore+2 && mc==87000 && !value);
        cur_tmp=536u;
        SmuCpuEnd(&owner);
        SmuOwnerStop(&owner);
        // Stopped: the flag is gone and no CPU message is sent.
        CHECK(!owner.CpuOnline);
        CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,&value,NULL,NULL)
              ==STATUS_DEVICE_NOT_READY);
        CHECK(cpu_calls==cpuBefore+2 && calls==before+1);
    }
    {
        LONG before=calls;
        version_refuse=1;
        CHECK(SmuOwnerStart(&owner,registers)!=STATUS_SUCCESS && !owner.Online && !owner.Registers);
        CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0x00580701u && calls==before+1);
        version_refuse=0;reply=1;
        CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
        CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0x00580701u && calls==before+2);
        SmuOwnerStop(&owner);
    }
    // Zero is a valid firmware response, distinct from never initialized.
    smu_version=0;reply=1;
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
    SmuOwnerStop(&owner);
    CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_SUCCESS && version==0);
    // A genuinely new owner has no inherited snapshot, even after a failed start.
    SmuOwnerInitialize(&owner);version_refuse=1;reply=1;
    CHECK(SmuOwnerStart(&owner,registers)!=STATUS_SUCCESS);
    CHECK(SmuReadFirmwareVersion(&owner,&version)==STATUS_DEVICE_NOT_READY && !version);
    printf("SMU native owner: %ld checks, %ld failures\n",native_checks,native_failures);
    return native_failures?1:0;
}
