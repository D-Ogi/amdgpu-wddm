#include <stdio.h>
typedef int NTSTATUS;typedef void* HANDLE;typedef struct {int ignored;} DXGKARG_BUILDPAGINGBUFFER;
typedef struct {int PagingBuildLock;} BC250_WDDM;
static BC250_WDDM state;static int region,depth,calls,bad;
static BC250_WDDM* WddmOf(HANDLE h){return h?&state:0;}
static void KeEnterCriticalRegion(void){region++;}
static void KeLeaveCriticalRegion(void){region--;}
static void ExAcquirePushLockExclusive(int*p){if(!region || depth)bad++;depth++;*p=1;}
static void ExReleasePushLockExclusive(int*p){if(!region || depth!=1 || !*p)bad++;depth--;*p=0;}
static NTSTATUS WddmBuildPagingBufferImpl(HANDLE h,DXGKARG_BUILDPAGINGBUFFER*b){(void)b;calls++;if(h && (!region || depth!=1))bad++;return 123;}
static NTSTATUS Bc250WddmBuildPagingBuffer(HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER* Build)
{
    BC250_WDDM* wddm=WddmOf(hAdapter);
    NTSTATUS status;
    if (!wddm) return WddmBuildPagingBufferImpl(hAdapter,Build);
    KeEnterCriticalRegion();ExAcquirePushLockExclusive(&wddm->PagingBuildLock);
    status=WddmBuildPagingBufferImpl(hAdapter,Build);
    ExReleasePushLockExclusive(&wddm->PagingBuildLock);KeLeaveCriticalRegion();
    return status;
}

int main(void){DXGKARG_BUILDPAGINGBUFFER b={0};if(Bc250WddmBuildPagingBuffer(&state,&b)!=123)bad++;if(Bc250WddmBuildPagingBuffer(0,&b)!=123)bad++;if(region||depth||calls!=2)bad++;printf("wrapper checks: %s\n",bad?"FAIL":"PASS");return bad?1:0;}
