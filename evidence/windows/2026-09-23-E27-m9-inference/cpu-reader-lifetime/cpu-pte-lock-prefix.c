#include <windows.h>
#include <stdio.h>
typedef LONG NTSTATUS;
typedef struct {ULONG id;} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
static struct {SRWLOCK CpuUpdateLock;BOOLEAN Ready,Write;ULONG Snapshot;void*SegmentMapping;SIZE_T SegmentLength;} g_VidMm;
static HANDLE firstEntered,firstRelease,secondAttempted,secondEntered,stopAttempted;
static volatile LONG violations, activeReaders;
static HANDLE readerEntered[2], readerRelease;
static __declspec(thread) int currentIrql;
#define PASSIVE_LEVEL 0
#define KeGetCurrentIrql() currentIrql
#define ExAcquirePushLockShared AcquireSRWLockShared
#define ExReleasePushLockShared ReleaseSRWLockShared
static __declspec(thread) int critical;
#define KeEnterCriticalRegion() (++critical)
#define KeLeaveCriticalRegion() (--critical)
#define ExAcquirePushLockExclusive AcquireSRWLockExclusive
#define ExReleasePushLockExclusive ReleaseSRWLockExclusive
static NTSTATUS VidMmUpdatePageTableLocked(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE*u){
 if(critical!=1 || activeReaders!=0)InterlockedIncrement(&violations);
 if(!g_VidMm.Ready || !g_VidMm.Write)return -1;
 g_VidMm.Snapshot=u->id;
 if(u->id==1){SetEvent(firstEntered);if(WaitForSingleObject(firstRelease,5000)!=WAIT_OBJECT_0)InterlockedIncrement(&violations);}
 else SetEvent(secondEntered);
 if(g_VidMm.Snapshot!=u->id || !g_VidMm.Ready)InterlockedIncrement(&violations);
 return 0;
}
static void MmUnmapIoSpace(void*p,SIZE_T bytes){if(activeReaders!=0)InterlockedIncrement(&violations);if(p!=(void*)1 || bytes!=4096 || g_VidMm.Ready || g_VidMm.Write)InterlockedIncrement(&violations);}

// The real public wrapper is extracted; the walk body models a held mapping.
// Real PTE decoding and bounds are tested separately by run_paging -KmdRouting.
static BOOLEAN VidMmTranslateLocked(ULONGLONG root,ULONGLONG va,ULONGLONG*physical,BOOLEAN*system){
 void* mapping=g_VidMm.SegmentMapping;
 (void)va;*physical=0;*system=FALSE;
 if(critical!=1)InterlockedIncrement(&violations);
 if(!g_VidMm.Ready || !g_VidMm.Write || !mapping)return FALSE;
 InterlockedIncrement(&activeReaders);
 SetEvent(readerEntered[root==1?0:1]);
 if(WaitForSingleObject(readerRelease,5000)!=WAIT_OBJECT_0)InterlockedIncrement(&violations);
 if(mapping!=g_VidMm.SegmentMapping || !g_VidMm.Ready)InterlockedIncrement(&violations);
 InterlockedDecrement(&activeReaders);
 *physical=4096;return TRUE;
}
