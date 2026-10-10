#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adapter_owner.h"
typedef void* PDEVICE_OBJECT;
#define NTSTATUS LONG
#define STATUS_SUCCESS ((LONG)0)
#define STATUS_DEVICE_BUSY ((LONG)-1)
#define STATUS_INSUFFICIENT_RESOURCES ((LONG)-2)
#define STATUS_DEVICE_NOT_READY ((LONG)-3)
#define POOL_FLAG_NON_PAGED 0
#define BC250_TAG 0
#define D3DKMDT_VPPR_IDENTITY 0
#define BC250_FAN_REASON_STOP 0
#define NotificationEvent 0
#define StageAddDevice 0
#define StageStopEnter 1
#define StageStopDone 2
static LONG touches, allocations, frees, failAllocation, retained, checks, failures, stopCalls;
static HANDLE releaseGate, enteredGate;
typedef struct {
 void* PhysicalDeviceObject;
 int Rotation,Smu,Hwmon,GartLock,GfxPagingLock,GfxAccessLock,GfxAccessDrained,GfxRetireEvent,GfxAccessClosed;
 BOOLEAN StopDone,GpuStopUnconfirmed,Started,InheritedSignalValid,SystemDisplayReady,PostDisplayStopAttempted;
 LONG PostDisplayStopStatus;
 int ModeActive,SourceVisible,CommitSeen,PresentSeen,FullWddm;
} BC250_DEVICE;
#define CHECK(x) do { InterlockedIncrement(&checks); if(!(x)){InterlockedIncrement(&failures);printf("FAIL CHECK %u\n",(unsigned)__LINE__);fflush(stdout);ExitProcess(1);} } while(0)
static void touch(void) { InterlockedIncrement(&touches); }
static void* allocate(size_t bytes) { InterlockedIncrement(&allocations); return failAllocation ? NULL : calloc(1,bytes); }
static void release(void* value) { InterlockedIncrement(&frees); free(value); }
#define ExAllocatePool2(flags,bytes,tag) allocate(bytes)
#define ExFreePoolWithTag(value,tag) release(value)
#define GuardStage(...) touch()
#define BoardMemoryInitialize(...) touch()
#define BoardMemoryIdentityClear(...) touch()
#define StartHealthInitialize(...) touch()
#define CuModeInitialize(...) touch()
#define DpmInitialize(...) touch()
#define SmuMetricsInitialize(...) touch()
#define CpuInitialize(...) touch()
#define InteropInitialize(...) touch()
#define SmuOwnerInitialize(...) touch()
#define HwmonInitialize(...) touch()
#define FanInitialize(...) touch()
#define DpAudioInitialize(...) touch()
#define ModesetInitialize(...) touch()
#define ExInitializeFastMutex(...) touch()
#define ExInitializePushLock(...) touch()
#define KeInitializeSpinLock(...) touch()
#define KeInitializeEvent(...) touch()
#define BoardMemoryStop(...) touch()
#define BoardProviderUnbind(...) touch()
#define HangDetectorStop(...) touch()
#define CpuStop(...) touch()
#define DpmStop(...) touch()
#define SmuMetricsStop(...) touch()
#define FanStop(...) touch()
#define StartHealthRemove(...) touch()
#define InteropRemove(...) touch()
#define DisplayUnmapFramebuffer(...) touch()
#define IhRemove(...) touch()
#define GuardLog(...) touch()
#define StartHealthClose(...) touch()
#define GuardLogKeepEpisode(...) touch()
#define HwmonStop(...) touch()
#define DpAudioStop(...) touch()
#define SmuOwnerStop(...) touch()
#define WddmStop(...) touch()
#define InteropStop(...) touch()
#define DcnStop(...) (touch(),STATUS_SUCCESS)
#define IhStop(...) touch()
#define GfxPrepareStop(...) touch()
#define PspStop(...) touch()
#define GartPrepareStop(...) touch()
static void gfx_stop(BC250_DEVICE* device) {
 touch(); ++stopCalls; if(retained) device->GpuStopUnconfirmed=1;
 if(enteredGate) { SetEvent(enteredGate); CHECK(WaitForSingleObject(releaseGate,5000)==WAIT_OBJECT_0); }
}
#define GfxStop(device) gfx_stop(device)
#define GartStop(...) touch()
#define GfxTraceRlcState(...) touch()
#define VramStop(...) touch()
#define MmioStop(...) touch()
#define GuardReleaseStart(...) touch()
#define GuardLogKeep(...) touch()
NTSTATUS Bc250StopDevice(const PVOID context);
/* ACTUAL_OWNER */
/* ACTUAL_PNP */
static DWORD WINAPI remove_thread(void* context) { return (DWORD)Bc250RemoveDevice(context); }
typedef struct { void* pdo; void* context; LONG status; HANDLE start; } RACER;
static DWORD WINAPI add_thread(void* context) {
 RACER* r=context; CHECK(WaitForSingleObject(r->start,5000)==WAIT_OBJECT_0);
 r->status=Bc250AddDevice(r->pdo,&r->context); return 0;
}
int main(void) {
 int pdoA,pdoB; void* a=NULL; void* b=(void*)1; BC250_DEVICE unknown={0}; LONG before; unsigned i;
 HANDLE threads[2], start; RACER racers[2];
 failAllocation=1; CHECK(Bc250AddDevice(&pdoA,&a)==STATUS_INSUFFICIENT_RESOURCES); CHECK(!a && !touches);
 failAllocation=0; CHECK(Bc250AddDevice(&pdoA,&a)==0); before=touches;
 CHECK(Bc250AddDevice(&pdoB,&b)==STATUS_DEVICE_BUSY); CHECK(!b && touches==before);
 unknown.PhysicalDeviceObject=&pdoB;
 CHECK(Bc250StopDevice(&unknown)==0); CHECK(touches==before); CHECK(Bc250RemoveDevice(&unknown)==0); CHECK(touches==before);
 // Simulate a failed start after partial initialization: real Stop and Remove unwind it.
 ((BC250_DEVICE*)a)->StopDone=FALSE; ((BC250_DEVICE*)a)->Started=FALSE;
 CHECK(Bc250StopDevice(a)==0); CHECK(AdapterOwnerIs(&pdoA)); before=touches;
 CHECK(Bc250AddDevice(&pdoB,&b)==STATUS_DEVICE_BUSY); CHECK(touches==before);
 // Same context restarts; retained power does not release ownership.
 ((BC250_DEVICE*)a)->StopDone=FALSE; ((BC250_DEVICE*)a)->Started=TRUE;
 CHECK(AdapterOwnerIs(&pdoA)); CHECK(Bc250StopDevice(a)==0); CHECK(Bc250RemoveDevice(a)==0);
 CHECK(!AdapterOwnerIs(&pdoA));
 // Add followed directly by Remove must not attempt hardware teardown.
 before=stopCalls; CHECK(Bc250AddDevice(&pdoB,&b)==0);CHECK(Bc250RemoveDevice(b)==0);CHECK(stopCalls==before);
 // Concurrent AddDevice: exactly one can initialize global state.
 start=CreateEventW(NULL,TRUE,FALSE,NULL); CHECK(start!=NULL);
 memset(racers,0,sizeof(racers)); racers[0].pdo=&pdoA;racers[1].pdo=&pdoB;
 for(i=0;i<2;++i){racers[i].start=start;threads[i]=CreateThread(NULL,0,add_thread,&racers[i],0,NULL);CHECK(threads[i]!=NULL);}
 SetEvent(start); CHECK(WaitForMultipleObjects(2,threads,TRUE,5000)==WAIT_OBJECT_0);
 CHECK((racers[0].status==0)+(racers[1].status==0)==1);
 for(i=0;i<2;++i){CloseHandle(threads[i]);if(racers[i].status==0) a=racers[i].context;}
 CloseHandle(start);
 // Pause actual Remove inside actual Stop: no handoff before teardown completes.
 ((BC250_DEVICE*)a)->StopDone=FALSE;
 enteredGate=CreateEventW(NULL,TRUE,FALSE,NULL);releaseGate=CreateEventW(NULL,TRUE,FALSE,NULL);
 threads[0]=CreateThread(NULL,0,remove_thread,a,0,NULL);CHECK(threads[0]!=NULL);
 CHECK(WaitForSingleObject(enteredGate,5000)==WAIT_OBJECT_0);before=touches;
 CHECK(Bc250AddDevice(&pdoA,&b)==STATUS_DEVICE_BUSY); CHECK(touches==before);
 SetEvent(releaseGate);CHECK(WaitForSingleObject(threads[0],5000)==WAIT_OBJECT_0);
 CloseHandle(threads[0]);CloseHandle(enteredGate);CloseHandle(releaseGate);enteredGate=NULL;releaseGate=NULL;
 // Unconfirmed hardware retirement poisons the image, even after context free.
 CHECK(Bc250AddDevice(&pdoA,&a)==0);((BC250_DEVICE*)a)->StopDone=FALSE;retained=1;
 CHECK(Bc250RemoveDevice(a)==0);before=touches;
 CHECK(Bc250AddDevice(&pdoB,&b)==STATUS_DEVICE_BUSY);CHECK(!b && touches==before);
 CHECK(Bc250AddDevice(&pdoA,&b)==STATUS_DEVICE_BUSY);CHECK(!b && touches==before);
 printf("%ld checks, %ld failures\n",checks,failures);return failures?1:0;
}
