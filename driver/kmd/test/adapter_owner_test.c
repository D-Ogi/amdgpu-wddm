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
#define STATUS_DEVICE_DATA_ERROR ((LONG)-4)
#define NT_SUCCESS(status) ((LONG)(status)>=0)
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
static LONG connectorStopResult;
typedef struct {
 void* PhysicalDeviceObject;
 int Rotation,Smu,Hwmon,GartLock,GfxPagingLock,GfxAccessLock,GfxAccessDrained,GfxRetireEvent,GfxAccessClosed,PspSessionLock;
 BOOLEAN StopDone,GpuStopUnconfirmed,Started,InheritedSignalValid,SystemDisplayReady,PostDisplayStopAttempted;
 LONG PostDisplayStopStatus;
 volatile LONG StartVisibilityPending;
 int ModeActive,SourceVisible,CommitSeen,PresentSeen,FullWddm;
 int ConnectorInitialized,ConnectorDrained,ConnectorBufferFreed,ConnectorDestroyed;
} BC250_DEVICE;
#define CHECK(x) do { InterlockedIncrement(&checks); if(!(x)){InterlockedIncrement(&failures);printf("FAIL CHECK %u\n",(unsigned)__LINE__);fflush(stdout);ExitProcess(1);} } while(0)
static void touch(void) { InterlockedIncrement(&touches); }
/* Device-identity validation is exercised by driver-state, not the owner mock. */
static NTSTATUS DriverStateValidateDevice(PDEVICE_OBJECT device) { (void)device; return STATUS_SUCCESS; }
static void KeInitializeMutex(int* mutex,int level) { CHECK(!*mutex && !level);*mutex=1;touch(); }
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
#define HdmiLinkStop(...) touch()
#define HdmiLinkRelease(...) (touch(),STATUS_SUCCESS)
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
static void MmioStop(BC250_DEVICE* device) {
 CHECK(device->ConnectorDrained && device->ConnectorBufferFreed); touch();
}
static void ConnectorInitialize(BC250_DEVICE* device) {
 CHECK(!device->ConnectorInitialized && device->PhysicalDeviceObject && device->StopDone);
 device->ConnectorInitialized=1; touch();
}
static NTSTATUS ConnectorStop(BC250_DEVICE* device) {
 CHECK(device->ConnectorInitialized && !device->ConnectorDestroyed);
 CHECK(!device->StartVisibilityPending);
 device->ConnectorDrained=1; touch(); return connectorStopResult;
}
static void ConnectorBufferRelease(BC250_DEVICE* device) {
 CHECK(device->ConnectorDrained && !device->ConnectorDestroyed);
 device->ConnectorBufferFreed=1; touch();
}
static void ConnectorDestroy(BC250_DEVICE* device) {
 CHECK(device->ConnectorInitialized && device->StopDone && !device->ConnectorDestroyed);
 CHECK(AdapterOwnerIs(device->PhysicalDeviceObject));
 device->ConnectorDestroyed=1; touch();
}
#define GuardReleaseStart(...) touch()
#define GuardRecordStopBegin(...) touch()
#define GuardRecordStopDone(...) touch()
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
 unknown.PhysicalDeviceObject=&pdoB;unknown.StartVisibilityPending=1;
 CHECK(Bc250StopDevice(&unknown)==0); CHECK(touches==before); CHECK(Bc250RemoveDevice(&unknown)==0); CHECK(touches==before);
 CHECK(unknown.StartVisibilityPending==1);
 // Simulate a failed start after partial initialization: real Stop and Remove unwind it.
 ((BC250_DEVICE*)a)->StopDone=FALSE; ((BC250_DEVICE*)a)->Started=FALSE;
 ((BC250_DEVICE*)a)->StartVisibilityPending=1;
 CHECK(Bc250StopDevice(a)==0); CHECK(AdapterOwnerIs(&pdoA)); before=touches;
 CHECK(Bc250AddDevice(&pdoB,&b)==STATUS_DEVICE_BUSY); CHECK(touches==before);
 // Same context restarts; retained power does not release ownership.
 ((BC250_DEVICE*)a)->StopDone=FALSE; ((BC250_DEVICE*)a)->Started=TRUE;
 ((BC250_DEVICE*)a)->StartVisibilityPending=1;
 CHECK(AdapterOwnerIs(&pdoA)); CHECK(Bc250StopDevice(a)==0); CHECK(Bc250RemoveDevice(a)==0);
 CHECK(!AdapterOwnerIs(&pdoA));
 // Add followed directly by Remove must not attempt hardware teardown.
 before=stopCalls; CHECK(Bc250AddDevice(&pdoB,&b)==0);CHECK(Bc250RemoveDevice(b)==0);CHECK(stopCalls==before);
 // Disarm failure is retained in the handoff status; ordinary Stop still drains.
 CHECK(Bc250AddDevice(&pdoB,&b)==0);
 ((BC250_DEVICE*)b)->StopDone=FALSE;connectorStopResult=STATUS_DEVICE_DATA_ERROR;
 CHECK(Bc250StopDevice(b)==STATUS_SUCCESS);
 CHECK(((BC250_DEVICE*)b)->PostDisplayStopStatus==STATUS_DEVICE_DATA_ERROR);
 CHECK(((BC250_DEVICE*)b)->StopDone && ((BC250_DEVICE*)b)->ConnectorBufferFreed);
 CHECK(AdapterOwnerIs(&pdoB));connectorStopResult=STATUS_SUCCESS;
 CHECK(Bc250RemoveDevice(b)==0);
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
