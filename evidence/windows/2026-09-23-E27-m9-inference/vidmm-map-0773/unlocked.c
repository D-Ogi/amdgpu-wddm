#include <windows.h>
#include <stdio.h>
typedef LONG NTSTATUS;
typedef struct {ULONG id;} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
static struct {SRWLOCK CpuUpdateLock;BOOLEAN Ready,Write;ULONG Snapshot;void*SegmentMapping;SIZE_T SegmentLength;} g_VidMm;
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
static void MmUnmapIoSpace(void*p,SIZE_T bytes){if(p!=(void*)1 || bytes!=4096 || g_VidMm.Ready || g_VidMm.Write)InterlockedIncrement(&violations);}
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
static DWORD WINAPI WriteThread(void*arg){
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={(ULONG)(ULONG_PTR)arg};
 if(u.id==2)SetEvent(secondAttempted);
 if(VidMmUpdatePageTable(&u)!=0 || critical!=0)InterlockedIncrement(&violations);
 return 0;
}
static DWORD WINAPI StopThread(void*arg){(void)arg;SetEvent(stopAttempted);VidMmStop();if(critical!=0)InterlockedIncrement(&violations);return 0;}
static void wait_for(HANDLE h){if(WaitForSingleObject(h,5000)!=WAIT_OBJECT_0)InterlockedIncrement(&violations);}
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
 CloseHandle(firstEntered);CloseHandle(firstRelease);CloseHandle(secondAttempted);CloseHandle(secondEntered);CloseHandle(stopAttempted);
 printf("%s: serialized CPU snapshots, stop joins update, post-stop refusal (%ld violations)\n",violations?"FAIL":"PASS",violations);
 return violations?1:0;
}
