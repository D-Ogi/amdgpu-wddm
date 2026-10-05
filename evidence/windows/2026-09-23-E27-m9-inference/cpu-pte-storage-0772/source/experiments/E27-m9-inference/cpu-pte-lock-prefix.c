#include <windows.h>
#include <stdio.h>
typedef LONG NTSTATUS;
typedef struct {ULONG id;} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
static struct {SRWLOCK CpuUpdateLock;BOOLEAN Ready,Write;ULONG Snapshot;} g_VidMm;
static HANDLE firstEntered,firstRelease,secondAttempted,secondEntered,stopAttempted;
static volatile LONG violations;
static __declspec(thread) int critical;
#define KeEnterCriticalRegion() (++critical)
#define KeLeaveCriticalRegion() (--critical)
#define ExAcquirePushLockExclusive AcquireSRWLockExclusive
#define ExReleasePushLockExclusive ReleaseSRWLockExclusive
static NTSTATUS VidMmUpdatePageTableLocked(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE*u){
 if(critical!=1)InterlockedIncrement(&violations);
 if(!g_VidMm.Ready || !g_VidMm.Write)return -1;
 g_VidMm.Snapshot=u->id;
 if(u->id==1){SetEvent(firstEntered);if(WaitForSingleObject(firstRelease,5000)!=WAIT_OBJECT_0)InterlockedIncrement(&violations);}
 else SetEvent(secondEntered);
 if(g_VidMm.Snapshot!=u->id || !g_VidMm.Ready)InterlockedIncrement(&violations);
 return 0;
}
