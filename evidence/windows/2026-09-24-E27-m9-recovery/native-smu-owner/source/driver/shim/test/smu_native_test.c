// Compile the actual native owner, replacing only Windows kernel primitives.
#define WIN32_NO_STATUS
#include "smu_native_mock.h"
#include "smu.h"
#include "smu-native.inc"
static volatile ULONG registers[0x80000/sizeof(ULONG)];
static BC250_SMU_OWNER owner;
static unsigned phase,pending,mhz,vid,command,argument,reply;
static LONG calls;
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
            default:CHECK(0);
            }
            reply=1;
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
        struct bc250_clock_report r;
        ULONG f,v;LONG t;
        CHECK(SmuPrepareClock(&owner,&r)==STATUS_SUCCESS && r.ready);
        CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_SUCCESS);
        CHECK(f==1000 && v==116 && t==67000);
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
    ULONG f,v;LONG t;unsigned i;
    SmuOwnerInitialize(&owner);reply=1;
    CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_DEVICE_NOT_READY);
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_INVALID_DEVICE_STATE);
    for(i=0;i<4;i++) { threads[i]=CreateThread(NULL,0,Worker,NULL,0,NULL);CHECK(threads[i]!=NULL); }
    CHECK(WaitForMultipleObjects(4,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(i=0;i<4;i++)CloseHandle(threads[i]);
    CHECK(calls==1200 && !owner.Caller);
    entered=CreateEvent(NULL,TRUE,FALSE,NULL);release_write=CreateEvent(NULL,TRUE,FALSE,NULL);
    stop_started=CreateEvent(NULL,TRUE,FALSE,NULL);stop_done=CreateEvent(NULL,TRUE,FALSE,NULL);
    hold_write=1;writer=CreateThread(NULL,0,Once,NULL,0,NULL);
    CHECK(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    stopper=CreateThread(NULL,0,Stop,NULL,0,NULL);
    CHECK(WaitForSingleObject(stop_started,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(stop_done,100)==WAIT_TIMEOUT); // close must join the whole transaction
    SetEvent(release_write);
    CHECK(WaitForSingleObject(writer,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(stopper,5000)==WAIT_OBJECT_0);
    CHECK(!owner.Online && !owner.Registers && !owner.Caller && calls==1204);
    CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_DEVICE_NOT_READY);
    CHECK(!f && !v && !t && calls==1204);
    CHECK(SmuOwnerStart(&owner,registers)==STATUS_SUCCESS);
    CHECK(SmuReadClock(&owner,&f,&v,&t)==STATUS_SUCCESS && f==1000 && v==116);
    SmuOwnerStop(&owner);
    CloseHandle(writer);CloseHandle(stopper);CloseHandle(entered);CloseHandle(release_write);
    CloseHandle(stop_started);CloseHandle(stop_done);
    printf("SMU native owner: %ld checks, %ld failures\n",native_checks,native_failures);
    return native_failures?1:0;
}
