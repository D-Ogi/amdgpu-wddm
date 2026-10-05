#include <windows.h>
#include <stdio.h>
#include <assert.h>
#define IO_NO_INCREMENT 0
#define Executive 0
#define KernelMode 0
#define BC250_SUBMIT_FENCE_SLOT 1
#define BC250_PAGING_FENCE_SLOT 4
#define NT_ASSERT assert
typedef int KIRQL;
typedef struct {void*SubmitAdev;void*PagingDevicePtr;ULONG SubmitSeq,PagingSubmitSeq;volatile LONG SubmitInFlight,PagingSubmitInFlight;} BC250_GFX;
typedef struct {SRWLOCK GfxAccessLock;HANDLE GfxAccessDrained;ULONG GfxAccessUsers;BOOLEAN GfxAccessClosed;BC250_GFX*Gfx;} BC250_DEVICE;
static HANDLE readersEntered,readersRelease,closeStarted,closeDone;
static volatile LONG reads,freed,errors;
static void KeAcquireSpinLock(SRWLOCK*l,KIRQL*i){AcquireSRWLockExclusive(l);*i=0;}
static void KeReleaseSpinLock(SRWLOCK*l,KIRQL i){(void)i;ReleaseSRWLockExclusive(l);}
static void KeClearEvent(HANDLE*e){ResetEvent(*e);}
static void KeSetEvent(HANDLE*e,int inc,BOOLEAN wait){(void)inc;(void)wait;SetEvent(*e);}
static DWORD KeWaitForSingleObject(HANDLE*e,int reason,int mode,BOOLEAN alert,void*timeout){
 (void)reason;(void)mode;(void)alert;(void)timeout;SetEvent(closeStarted);
 return WaitForSingleObject(*e,5000);
}
static ULONG ReadFence(void*p,unsigned slot){
 (void)p;(void)slot;
 if(InterlockedIncrement(&reads)==2)SetEvent(readersEntered);
 if(WaitForSingleObject(readersRelease,5000)!=WAIT_OBJECT_0 || freed)InterlockedIncrement(&errors);
 return 7;
}
#define bc250_gfx_fence_read ReadFence
#define bc250_sdma_fence_read ReadFence
