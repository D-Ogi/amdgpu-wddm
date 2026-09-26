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
NTSTATUS VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    NTSTATUS status;
    KeEnterCriticalRegion();
    (void)0;
    status = VidMmUpdatePageTableLocked(Update);
    (void)0;
    KeLeaveCriticalRegion();
    return status;
}
void VidMmStop(void)
{
    KeEnterCriticalRegion();
    (void)0;
    g_VidMm.Ready = FALSE;
    g_VidMm.Write = FALSE;
    if (g_VidMm.SegmentMapping != NULL) {
        MmUnmapIoSpace(g_VidMm.SegmentMapping,(SIZE_T)g_VidMm.SegmentLength);
        g_VidMm.SegmentMapping = NULL;
    }
    (void)0;
    KeLeaveCriticalRegion();
}
BOOLEAN VidMmTranslate(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    BOOLEAN result;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { *Physical=0; *System=FALSE; return FALSE; }
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result = VidMmTranslateLocked(RootPhysical,Va,Physical,System);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return result;
}
static DWORD WINAPI WriteThread(void*arg){
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={(ULONG)(ULONG_PTR)arg};
 if(u.id==2)SetEvent(secondAttempted);
 if(VidMmUpdatePageTable(&u)!=0 || critical!=0)InterlockedIncrement(&violations);
 return 0;
}
static DWORD WINAPI StopThread(void*arg){(void)arg;SetEvent(stopAttempted);VidMmStop();if(critical!=0)InterlockedIncrement(&violations);return 0;}
static void wait_for(HANDLE h){if(WaitForSingleObject(h,5000)!=WAIT_OBJECT_0)InterlockedIncrement(&violations);}
static DWORD WINAPI ReaderThread(void*arg){
 ULONGLONG physical;BOOLEAN system;
 if(!VidMmTranslate((ULONGLONG)(ULONG_PTR)arg,0,&physical,&system) || physical!=4096 || system || critical!=0)InterlockedIncrement(&violations);
 return 0;
}
static void reader_lifetime_test(void){
 HANDLE a,b,c;ULONGLONG physical=123;BOOLEAN system=TRUE;
 readerEntered[0]=CreateEventW(NULL,TRUE,FALSE,NULL);readerEntered[1]=CreateEventW(NULL,TRUE,FALSE,NULL);
 readerRelease=CreateEventW(NULL,TRUE,FALSE,NULL);
 if(!readerEntered[0]||!readerEntered[1]||!readerRelease){InterlockedIncrement(&violations);return;}
 g_VidMm.Ready=TRUE;g_VidMm.Write=TRUE;g_VidMm.SegmentMapping=(void*)1;g_VidMm.SegmentLength=4096;
 // Two shared readers must overlap. An exclusive writer must wait for both.
 ResetEvent(secondAttempted);ResetEvent(secondEntered);
 a=CreateThread(NULL,0,ReaderThread,(void*)1,0,NULL);wait_for(readerEntered[0]);
 b=CreateThread(NULL,0,ReaderThread,(void*)2,0,NULL);wait_for(readerEntered[1]);
 if(activeReaders!=2)InterlockedIncrement(&violations);
 c=CreateThread(NULL,0,WriteThread,(void*)2,0,NULL);wait_for(secondAttempted);
 if(WaitForSingleObject(secondEntered,150)!=WAIT_TIMEOUT)InterlockedIncrement(&violations);
 SetEvent(readerRelease);wait_for(a);wait_for(b);wait_for(c);CloseHandle(a);CloseHandle(b);CloseHandle(c);
 // Stop must not unmap a held reader. Check lifetime, not just the thread exit.
 ResetEvent(readerEntered[0]);ResetEvent(readerRelease);ResetEvent(stopAttempted);
 a=CreateThread(NULL,0,ReaderThread,(void*)1,0,NULL);wait_for(readerEntered[0]);
 b=CreateThread(NULL,0,StopThread,NULL,0,NULL);wait_for(stopAttempted);
 if(WaitForSingleObject(b,150)!=WAIT_TIMEOUT || !g_VidMm.Ready || g_VidMm.SegmentMapping!=(void*)1)InterlockedIncrement(&violations);
 SetEvent(readerRelease);wait_for(a);wait_for(b);CloseHandle(a);CloseHandle(b);
 if(VidMmTranslate(1,0,&physical,&system) || physical!=0 || system || critical!=0 || activeReaders!=0 || g_VidMm.SegmentMapping)InterlockedIncrement(&violations);
 // IRQL rejection must happen before acquiring a push lock or walking memory.
 currentIrql=2;physical=123;system=TRUE;
 if(VidMmTranslate(1,0,&physical,&system) || physical!=0 || system || critical!=0)InterlockedIncrement(&violations);
 currentIrql=0;
 CloseHandle(readerEntered[0]);CloseHandle(readerEntered[1]);CloseHandle(readerRelease);
}
int main(void){
 HANDLE a,b;DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={2};
 firstEntered=CreateEventW(NULL,TRUE,FALSE,NULL);firstRelease=CreateEventW(NULL,TRUE,FALSE,NULL);
 secondAttempted=CreateEventW(NULL,TRUE,FALSE,NULL);secondEntered=CreateEventW(NULL,TRUE,FALSE,NULL);stopAttempted=CreateEventW(NULL,TRUE,FALSE,NULL);
 if(!firstEntered||!firstRelease||!secondAttempted||!secondEntered||!stopAttempted)return 2;
 InitializeSRWLock(&g_VidMm.CpuUpdateLock);g_VidMm.Ready=TRUE;g_VidMm.Write=TRUE;
 a=CreateThread(NULL,0,WriteThread,(void*)1,0,NULL);wait_for(firstEntered);
 b=CreateThread(NULL,0,WriteThread,(void*)2,0,NULL);wait_for(secondAttempted);
 if(WaitForSingleObject(secondEntered,150)!=WAIT_TIMEOUT)InterlockedIncrement(&violations);
 SetEvent(firstRelease);wait_for(a);wait_for(b);CloseHandle(a);CloseHandle(b);
 ResetEvent(firstEntered);ResetEvent(firstRelease);
 g_VidMm.SegmentMapping=(void*)1;g_VidMm.SegmentLength=4096;
 a=CreateThread(NULL,0,WriteThread,(void*)1,0,NULL);wait_for(firstEntered);
 b=CreateThread(NULL,0,StopThread,NULL,0,NULL);wait_for(stopAttempted);
 if(WaitForSingleObject(b,150)!=WAIT_TIMEOUT)InterlockedIncrement(&violations);
 SetEvent(firstRelease);wait_for(a);wait_for(b);CloseHandle(a);CloseHandle(b);
 if(g_VidMm.Ready || g_VidMm.Write || g_VidMm.SegmentMapping || VidMmUpdatePageTable(&u)!=-1 || critical!=0)InterlockedIncrement(&violations);
 reader_lifetime_test();
 CloseHandle(firstEntered);CloseHandle(firstRelease);CloseHandle(secondAttempted);CloseHandle(secondEntered);CloseHandle(stopAttempted);
 printf("%s: serialized CPU snapshots, concurrent readers, writer/stop exclusion, post-stop/IRQL refusal (%ld violations)\n",violations?"FAIL":"PASS",violations);
 return violations?1:0;
}
