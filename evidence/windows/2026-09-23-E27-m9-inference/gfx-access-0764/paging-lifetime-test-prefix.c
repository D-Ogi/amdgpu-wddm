#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#define BC250_GFX_PAGING_BUFFER_BYTES 0x10000
#define AMDGPU_FENCE_FLAG_INT 1
#define BC250_GFX_TAG 0
#define GfxAccessClose(d) ((void)(d))
#define STATUS_SUCCESS 0
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER ((LONG)0xc01e0001)
typedef LONG NTSTATUS;
typedef unsigned u32;
typedef enum { BC250PagingSupported,BC250PagingNotReady,BC250PagingNoRoot } BC250_WDDM_PAGING_UNSUPPORTED;
enum { PagingStreamAddress=1,PagingStreamInvalid=2,PagingStreamMore=3 };
struct funcs {unsigned align_mask;};
struct ring {unsigned max_dw;struct funcs*funcs;};
typedef struct {BOOLEAN PagingReady;void*PagingDevicePtr;struct ring*PagingRing;} BC250_GFX;
typedef struct {SRWLOCK GfxPagingLock;int GartLock;BC250_GFX*Gfx;} BC250_DEVICE;
typedef struct {BC250_DEVICE*Device;BC250_GFX*Gfx;ULONGLONG Root;BOOLEAN Fill;ULONG Pattern;BC250_WDDM_PAGING_UNSUPPORTED Unsupported;} BC250_PAGING_STREAM;
static HANDLE buildersEntered,buildersRelease,stopEntered;
static volatile LONG readers,freed,violations;
static __declspec(thread) int critical;
#define KeEnterCriticalRegion() (++critical)
#define KeLeaveCriticalRegion() (--critical)
#define ExAcquirePushLockShared AcquireSRWLockShared
#define ExReleasePushLockShared ReleaseSRWLockShared
#define ExAcquirePushLockExclusive AcquireSRWLockExclusive
#define ExReleasePushLockExclusive ReleaseSRWLockExclusive
#define ExAcquireFastMutex(p) ((void)(p))
#define ExReleaseFastMutex(p) ((void)(p))
#define ExFreePoolWithTag(p,t) ((void)(p),(void)(t),InterlockedExchange(&freed,1))
static unsigned bc250_sdma_fence_size(struct ring*r,unsigned f){(void)r;(void)f;return 6;}
static unsigned PagingStreamCapacity(ULONG a,ULONG b,ULONG c,unsigned d,unsigned e,unsigned f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 64;}
#define PagingResolve 0
#define PagingEmit 0
static int PagingStreamBuild(BC250_PAGING_STREAM*s,int r,int e,BOOLEAN fill,ULONGLONG src,ULONGLONG dst,unsigned bytes,unsigned start,u32*p,unsigned budget,unsigned*w,unsigned*n){
 (void)r;(void)e;(void)fill;(void)src;(void)dst;(void)start;(void)p;(void)budget;
 if(critical!=1 || freed || !s->Gfx->PagingReady)InterlockedIncrement(&violations);
 if(InterlockedIncrement(&readers)==2)SetEvent(buildersEntered);
 if(WaitForSingleObject(buildersRelease,5000)!=WAIT_OBJECT_0)InterlockedIncrement(&violations);
 if(freed || !s->Gfx->PagingReady)InterlockedIncrement(&violations);
 *w=7;*n=bytes;return 0;
}
