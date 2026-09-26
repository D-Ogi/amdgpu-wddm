// Compile the actual native owner, replacing only Windows kernel primitives.
#define WIN32_NO_STATUS
#include "smu_native_mock.h"
#include "smu.h"
#include "smu-native.inc"
static volatile ULONG registers[0x80000/sizeof(ULONG)];
static BC250_SMU_OWNER owner;
static unsigned phase,pending,mhz,vid,command,argument,reply;
static LONG calls;
static unsigned smu_version=0x00580600u,version_refuse;
static HANDLE entered,release_write,stop_started,stop_done;
static volatile LONG hold_write;
ULONG NativeRead(PULONG address) {
    unsigned offset=(unsigned)((ULONG_PTR)address-(ULONG_PTR)registers);
    CHECK(OwnerHeld(&owner));
    if(offset==BC250_SMU_mmTHM_TCON_CUR_TMP) return (536u<<THM_TCON_CUR_TMP__CUR_TEMP__SHIFT); // 67 C
    if(offset==BC250_SMU_mmMP1_SMN_C2PMSG_90) {
        if(pending && !--pending) {
            switch(command) {
            case PPSMC_MSG_RequestGfxclk:mhz=argument;break;
            case PPSMC_MSG_ForceGfxVid:vid=argument;break;
            case PPSMC_MSG_GetGfxFrequency:argument=mhz;break;
            case PPSMC_MSG_GetGfxVid:argument=vid;break;
            case PPSMC_MSG_GetSmuVersion:argument=smu_version;break;
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
    CHECK(OwnerHeld(&owner) && !pending);
    if(offset==BC250_SMU_mmMP1_SMN_C2PMSG_90) { CHECK(!phase && !value);reply=0;phase=1; }
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
        r.Op=BC250_CLOCK_OP_SET;r.RequestedMHz=1500;r.RequestedMv=900;
        SmuClockRequest(&owner,&r,TRUE,TRUE,FALSE);
        CHECK(r.Status==BC250_ESCAPE_STATUS_DONE && r.Ready && r.VoltageStaged);
        CHECK(r.InitialMHz==1000 && r.InitialVid==116 && r.ObservedMHz==1500 && r.ObservedVid==104 && r.ExpectedVid==104);
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
