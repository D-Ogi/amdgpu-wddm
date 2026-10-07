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
// The CPU-domain message as every case below sends it: no hot exception, and the temperature validity bit
// dropped. The two cases that exercise those (0.7.211) call SmuCpuMessage directly.
static NTSTATUS CpuMsg(BC250_SMU_OWNER* o,ULONG queue,ULONG message,ULONG parameter,BOOLEAN write,
                       ULONG busyPermille,ULONG* value,LONG* temperatureMc,ULONG* firmwareStatus)
{
    return SmuCpuMessage(o,queue,message,parameter,write,FALSE,busyPermille,value,temperatureMc,
                         firmwareStatus,NULL);
}
static unsigned phase,pending,mhz,vid,command,argument,reply,ramp,ramp_left,ramp_from,core_mask=0x77u;
// The temperature the THM register reports, in its own units (536 = 67 C), and the firmware's queue 3 (0.7.210):
// its own phase, command, argument and response, because "check and drain the previous response before the first
// write" runs once per queue. cpu_value is what a getter answers; cpu_calls counts queue 3 messages alone.
static unsigned cur_tmp=536u;
static unsigned cpu_phase,cpu_pending,cpu_command,cpu_argument,cpu_reply=1u,cpu_value=1050u;
static unsigned cpu_sent;          // the argument the last queue 3 message carried (cpu_argument becomes the answer)
static LONG cpu_calls;
static LONG calls;
static unsigned smu_version=0x00580600u,version_refuse;
static HANDLE entered,release_write,stop_started,stop_done;
static volatile LONG hold_write;
// The metrics path (0.7.215): the page the firmware writes into, the address it was told, the order of the three
// messages, and the two ways the fixture misbehaves (a refusal of the transfer, a transfer that writes nothing).
static volatile ULONG metrics_page[BC250_SMU_METRICS_PAGE/sizeof(ULONG)];
static ULONGLONG metrics_page_mc=0xF4008CF000ull;   // E03: the address amdgpu named on unit A
static unsigned metrics_hi,metrics_lo,metrics_named,metrics_refuse,metrics_nowrite,metrics_log[8],metrics_logged;
static void MetricsFirmwareWrite(void)
{
    volatile UCHAR* p=(volatile UCHAR*)metrics_page;
    unsigned i;
    // The firmware writes the table where it was told; this fixture checks that it was told the page's address.
    CHECK(metrics_named==2 && (((ULONGLONG)metrics_hi<<32)|metrics_lo)==metrics_page_mc);
    if(metrics_nowrite)return;
    for(i=0;i<BC250_SMU_METRICS_BYTES;i++)p[i]=0;
    *(volatile USHORT*)(p+BC250_SMU_METRICS_OFF_GFXCLK)=1500;
    *(volatile USHORT*)(p+BC250_SMU_METRICS_OFF_GFX_TEMP)=6200;
    *(volatile ULONG*)(p+BC250_SMU_METRICS_OFF_VOLTAGE)=900;
    *(volatile ULONG*)(p+BC250_SMU_METRICS_OFF_VOLTAGE+4)=919;
    *(volatile ULONG*)(p+BC250_SMU_METRICS_OFF_POWER)=21000;
    *(volatile ULONG*)(p+BC250_SMU_METRICS_OFF_POWER+4)=48000;
    *(volatile ULONG*)(p+BC250_SMU_METRICS_OFF_SOCKET)=78000;
    *(volatile ULONG*)(p+BC250_SMU_METRICS_HALF+BC250_SMU_METRICS_OFF_SOCKET)=77400;
    *(volatile USHORT*)(p+BC250_SMU_METRICS_OFF_SOC_TEMP)=6000;
}
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
            case PPSMC_MSG_SetDriverTableDramAddrHigh:metrics_hi=argument;metrics_named=1;break;
            case PPSMC_MSG_SetDriverTableDramAddrLow:CHECK(metrics_named==1);metrics_lo=argument;metrics_named=2;break;
            case PPSMC_MSG_TransferTableSmu2Dram:CHECK(argument==6u);if(!metrics_refuse)MetricsFirmwareWrite();break;
            default:CHECK(0);
            }
            if(command>=PPSMC_MSG_SetDriverTableDramAddrHigh && command<=PPSMC_MSG_TransferTableSmu2Dram &&
               metrics_logged<ARRAYSIZE(metrics_log))metrics_log[metrics_logged++]=command;
            reply=((command==PPSMC_MSG_GetSmuVersion && version_refuse) ||
                   (command==PPSMC_MSG_TransferTableSmu2Dram && metrics_refuse))?0xFEu:1u;
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
    else if(offset==BC250_CPU_Q3_PARAM_TEST) { CHECK(cpu_phase==1);cpu_argument=cpu_sent=value;cpu_phase=2; }
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
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,0x98u,0,TRUE,0,NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,0,FALSE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,5000,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,1,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);   // a positive scale raises the voltage
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_GFX,BC250_CPU_MSG_SET_CORE_ENABLE_MASK,0x7Fu,TRUE,0,
                            NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);  // a harvest pattern
        CHECK(CpuMsg(&owner,2u,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);   // no third queue
        // The clock transaction's own messages are not CPU messages (smu_v11_8_ppsmc.h).
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_GFX,PPSMC_MSG_RequestGfxclk,1500,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_GFX,PPSMC_MSG_ForceGfxVid,116,TRUE,0,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(calls==before && cpu_calls==cpuBefore);
        // The GPU busy gate: at or above the share, nothing is sent at all.
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,
                            BC250_CPU_GPU_BUSY_PERMILLE,&value,NULL,NULL)==STATUS_DEVICE_BUSY);
        CHECK(cpu_calls==cpuBefore && !value);
        // The joint power arm's way in (0.7.216.7): the busy gate is lifted for queue 3's clock limit and its voltage
        // readback, each in its own direction, and for nothing else.
        CHECK(SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,TRUE));
        CHECK(SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,FALSE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,FALSE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,TRUE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,TRUE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CAP_C,TRUE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CORE_MHZ,FALSE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_GFX,BC250_CPU_MSG_SET_MAX_MHZ,TRUE));
        CHECK(!SmuCpuJointAdmitted(BC250_CPU_QUEUE_GFX,BC250_CPU_MSG_SET_CORE_ENABLE_MASK,TRUE));
        // A message outside the two is refused at any load, and the argument range of the clock limit stays: nothing
        // reaches either mailbox.
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,
                                 bc250_cpu_scale_argument(2),TRUE,TRUE,1000,NULL,NULL,NULL,NULL)
              ==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CAP_C,95,TRUE,FALSE,0,
                                 NULL,NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CAP_C,0,FALSE,TRUE,1000,
                                 &value,NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,BC250_CPU_MIN_MHZ-1u,TRUE,
                                 TRUE,1000,NULL,NULL,NULL,NULL)==STATUS_INVALID_PARAMETER);
        CHECK(calls==before && cpu_calls==cpuBefore && !value);
        // The two at a fully busy GPU: each one message on queue 3, queue 0 untouched.
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,3000,TRUE,TRUE,1000,
                                 NULL,&mc,NULL,NULL)==STATUS_SUCCESS);
        CHECK(cpu_command==BC250_CPU_MSG_SET_MAX_MHZ && cpu_sent==3000u && cpu_calls==cpuBefore+1 &&
              calls==before && mc==67000);
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,TRUE,1000,
                                 &value,NULL,NULL,NULL)==STATUS_SUCCESS);
        CHECK(value==cpu_value && cpu_calls==cpuBefore+2);
        // The ordinary path keeps its gate for the very same message.
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,3000,TRUE,BC250_CPU_GPU_BUSY_PERMILLE,
                     NULL,NULL,NULL)==STATUS_DEVICE_BUSY);
        CHECK(cpu_calls==cpuBefore+2);
        // The hot gate is not lifted: at 87 C a change the caller does not name a cooling step or a restore waits,
        // and one it names goes out.
        cur_tmp=696u;
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,3200,TRUE,FALSE,1000,
                                 NULL,&mc,NULL,NULL)==STATUS_DEVICE_POWER_FAILURE);
        CHECK(cpu_calls==cpuBefore+2 && mc==87000);
        CHECK(SmuCpuJointMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_MAX_MHZ,2800,TRUE,TRUE,1000,
                                 NULL,NULL,NULL,NULL)==STATUS_SUCCESS);
        CHECK(cpu_command==BC250_CPU_MSG_SET_MAX_MHZ && cpu_sent==2800u && cpu_calls==cpuBefore+3);
        cur_tmp=536u;
        before=calls;cpuBefore=cpu_calls;
        // A getter on queue 3: one message on queue 3's mailbox, the firmware's response word, the temperature.
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,&value,&mc,&fw)
              ==STATUS_SUCCESS);
        CHECK(value==cpu_value && mc==67000 && fw==1u && cpu_calls==cpuBefore+1 &&
              cpu_command==BC250_CPU_MSG_READ_CPU_MV && calls==before);   // queue 0 saw nothing
        // A setter on queue 3, and the core mask on queue 0: each argument reaches its own queue's register.
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CAP_C,95,TRUE,0,NULL,NULL,NULL)
              ==STATUS_SUCCESS);
        CHECK(cpu_command==BC250_CPU_MSG_SET_CAP_C && cpu_calls==cpuBefore+2 && calls==before);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_GFX,BC250_CPU_MSG_SET_CORE_ENABLE_MASK,BC250_CPU_MASK_FULL,
                            TRUE,0,NULL,NULL,NULL)==STATUS_SUCCESS);
        CHECK(command==BC250_CPU_MSG_SET_CORE_ENABLE_MASK && core_mask==BC250_CPU_MASK_FULL &&
              calls==before+1 && cpu_calls==cpuBefore+2);
        // The 87 C gate, read before every message and not once per sequence. Since 0.7.211 it has two
        // exceptions, both of them the CPU rail's reading of the rule the GPU clock path already carries: the
        // part must always be returnable to the settings it is known to run at.
        //   a getter    changes nothing, and the read stage is how the driver learns the part is hot at all
        //   AllowHot    the way back from a trial, and every step that lowers the dissipation
        // A trial left in the chip because the part was hot is the worse of the two states, and nothing else
        // would ever take it out: the firmware keeps no copy and the window has already passed.
        cur_tmp=696u;
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,&value,&mc,NULL)
              ==STATUS_SUCCESS);
        CHECK(cpu_calls==cpuBefore+3 && mc==87000 && value==cpu_value);
        // A setter without the flag is refused, and nothing reaches the mailbox.
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,
                     bc250_cpu_scale_argument(2),TRUE,0,NULL,&mc,NULL)==STATUS_DEVICE_POWER_FAILURE);
        CHECK(cpu_calls==cpuBefore+3 && mc==87000);
        {
            BOOLEAN valid=FALSE;
            // The same message with the flag goes out, and the temperature is still read and still reported,
            // so nothing downstream judges the part as cold.
            CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,
                                bc250_cpu_scale_argument(0),TRUE,TRUE,0,NULL,&mc,NULL,&valid)
                  ==STATUS_SUCCESS);
            CHECK(cpu_command==BC250_CPU_MSG_SET_CURVE_SCALE && cpu_calls==cpuBefore+4);
            CHECK(mc==87000 && valid);
            // The busy gate is not an exception: a restore waits for the GPU, which is seconds and not hours.
            valid=TRUE;
            CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_SET_CURVE_SCALE,0,TRUE,TRUE,
                                BC250_CPU_GPU_BUSY_PERMILLE,NULL,NULL,NULL,&valid)==STATUS_DEVICE_BUSY);
            CHECK(cpu_calls==cpuBefore+4 && !valid);
            // Nor is the allowlist: a message that is not on it stays off it however the way back is marked.
            CHECK(SmuCpuMessage(&owner,BC250_CPU_QUEUE_CPU,0x98u,0,TRUE,TRUE,0,NULL,NULL,NULL,&valid)
                  ==STATUS_INVALID_PARAMETER);
            CHECK(cpu_calls==cpuBefore+4);
        }
        cur_tmp=536u;
        SmuCpuEnd(&owner);
        SmuOwnerStop(&owner);
        // Stopped: the flag is gone and no CPU message is sent.
        CHECK(!owner.CpuOnline);
        CHECK(CpuMsg(&owner,BC250_CPU_QUEUE_CPU,BC250_CPU_MSG_READ_CPU_MV,0,FALSE,0,&value,NULL,NULL)
              ==STATUS_DEVICE_NOT_READY);
        CHECK(cpu_calls==cpuBefore+4 && calls==before+1);
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
    // ---- the SMU metrics table (0.7.215, docs/design/dpm.md "Power reading") -----------------------------------
    // The owner sends the three messages under its lock, names the page once per owner start in amdgpu's order,
    // poisons the page before every transfer, and lets nothing through its own list but the page's arguments.
    {
        UCHAR copy[BC250_SMU_METRICS_BYTES];
        struct bc250_smu_metrics m;
        LONG before;
        unsigned ignored=0;
        // Offline: nothing is sent, and the reader is told to try again later.
        before=calls;
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,sizeof(copy))==BC250_SMU_METRICS_OFFLINE);
        CHECK(calls==before && !metrics_logged);
        CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS && !owner.MetricsTableMc);
        // Malformed requests: refused before the lock, nothing sent.
        before=calls;
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,NULL,copy,sizeof(copy))==-22);
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,NULL,sizeof(copy))==-22);
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,0)==-22);
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,243)==-22);
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,BC250_SMU_METRICS_PAGE+4u)==-22);
        // An address the list does not admit (not page-aligned, or zero): the list refuses the first message.
        CHECK(SmuReadMetrics(&owner,metrics_page_mc+4u,metrics_page,copy,sizeof(copy))==-22);
        CHECK(SmuReadMetrics(&owner,0,metrics_page,copy,sizeof(copy))==-22);
        CHECK(calls==before && !metrics_logged && !owner.MetricsTableMc);
        // The first read: High, Low, then the transfer, and the table as the firmware wrote it.
        memset(copy,0,sizeof(copy));
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,sizeof(copy))==0);
        CHECK(calls==before+3 && metrics_logged==3 && metrics_log[0]==PPSMC_MSG_SetDriverTableDramAddrHigh &&
              metrics_log[1]==PPSMC_MSG_SetDriverTableDramAddrLow && metrics_log[2]==PPSMC_MSG_TransferTableSmu2Dram);
        CHECK(metrics_hi==0xF4u && metrics_lo==0x008CF000u && owner.MetricsTableMc==metrics_page_mc && !owner.Caller);
        CHECK(bc250_smu_metrics_parse(copy,sizeof(copy),&m)==BC250_SMU_METRICS_PARSE_OK);
        CHECK(m.socket_mw==78000 && m.socket_avg_mw==77400 && m.gfx_mw==48000 && m.soc_mw==21000);
        CHECK(m.gfx_mv==919 && m.soc_mv==900 && m.gfx_mhz==1500 && m.gfx_cc==6200 && m.soc_cc==6000);
        // Every later read of this owner start: the transfer alone.
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,sizeof(copy))==0);
        CHECK(calls==before+4 && metrics_logged==4 && metrics_log[3]==PPSMC_MSG_TransferTableSmu2Dram);
        // A transfer the firmware answers OK and writes nothing for: the poison comes back, the decode refuses it.
        metrics_nowrite=1;
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,sizeof(copy))==0);
        for(i=0;i<sizeof(copy) && copy[i]==BC250_SMU_METRICS_POISON;i++);
        CHECK(i==sizeof(copy) && bc250_smu_metrics_parse(copy,sizeof(copy),&m)==BC250_SMU_METRICS_PARSE_UNWRITTEN);
        metrics_nowrite=0;
        // A refusal of the transfer: a non-zero result that is not OFFLINE, which the reader latches.
        metrics_refuse=1;before=calls;
        { int r=SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,sizeof(copy));
          CHECK(r!=0 && r!=BC250_SMU_METRICS_OFFLINE && calls==before+1 && !owner.Caller); }
        metrics_refuse=0;reply=1;
        // The clock list still admits none of the three, and the metrics list none of the clock messages.
        before=calls;
        owner.Caller=PsGetCurrentThread();
        CHECK(Message(&owner,PPSMC_MSG_SetDriverTableDramAddrHigh,0xF4u,&ignored)==-22);
        CHECK(Message(&owner,PPSMC_MSG_SetDriverTableDramAddrLow,0x008CF000u,&ignored)==-22);
        CHECK(Message(&owner,PPSMC_MSG_TransferTableSmu2Dram,6,&ignored)==-22);
        owner.Caller=NULL;
        CHECK(!bc250_smu_metrics_message_allowed(PPSMC_MSG_RequestGfxclk,1500,metrics_page_mc));
        CHECK(!bc250_smu_metrics_message_allowed(PPSMC_MSG_ForceGfxVid,116,metrics_page_mc));
        CHECK(!bc250_smu_metrics_message_allowed(PPSMC_MSG_TransferTableDram2Smu,6,metrics_page_mc));
        CHECK(calls==before);
        // A new owner start names the page again before its first transfer.
        SmuOwnerStop(&owner);
        CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS && !owner.MetricsTableMc);
        before=calls;metrics_logged=0;
        CHECK(SmuReadMetrics(&owner,metrics_page_mc,metrics_page,copy,sizeof(copy))==0);
        CHECK(calls==before+3 && metrics_log[0]==PPSMC_MSG_SetDriverTableDramAddrHigh &&
              metrics_log[1]==PPSMC_MSG_SetDriverTableDramAddrLow && metrics_log[2]==PPSMC_MSG_TransferTableSmu2Dram);
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
