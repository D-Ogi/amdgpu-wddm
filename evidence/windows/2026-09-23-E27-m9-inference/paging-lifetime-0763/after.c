#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#define BC250_GFX_PAGING_BUFFER_BYTES 0x10000
#define AMDGPU_FENCE_FLAG_INT 1
#define BC250_GFX_TAG 0
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
NTSTATUS GfxPagingBuild(_Inout_ BC250_DEVICE* Device, ULONGLONG RootPhysical, BOOLEAN Fill, ULONGLONG SrcVa,
                        ULONGLONG DstVa, ULONGLONG Bytes, ULONG FillPattern, _Inout_ PVOID DmaBuffer,
                        ULONG DmaBufferOffset, ULONG DmaBufferFree, ULONG StartByte,
                        _Out_ ULONG* DwordsWritten, _Out_ ULONG* NextByte,
                        _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    BC250_PAGING_STREAM stream;
    unsigned int budget, written = 0, next = StartByte;
    u32* payload;
    int result;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *NextByte = StartByte; *Unsupported = BC250PagingSupported;
    if (DmaBuffer == NULL || Bytes == 0 || Bytes > 0xFFFFFFFFu || StartByte > Bytes)
        return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL) { status = STATUS_INVALID_PARAMETER; goto Done; }
    if (!gfx->PagingReady || gfx->PagingDevicePtr == NULL) { *Unsupported = BC250PagingNotReady; goto Done; }
    if (RootPhysical == 0) { *Unsupported = BC250PagingNoRoot; goto Done; }
    if ((DmaBufferOffset & 3u) != 0 || DmaBufferOffset > BC250_GFX_PAGING_BUFFER_BYTES) {
        status = STATUS_INVALID_PARAMETER; goto Done;
    }
    // The 64KiB OS command buffer is larger than the live SDMA reservation. Bound
    // accumulated packets too, reserving fence and amdgpu_ring_alloc alignment.
    budget = PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
                                  gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
                                  bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    payload = (u32*)DmaBuffer; // this call's dxgkrnl-owned private payload
    stream.Device=Device; stream.Gfx=gfx; stream.Root=RootPhysical;
    stream.Fill=Fill; stream.Pattern=FillPattern; stream.Unsupported=BC250PagingSupported;
    result=PagingStreamBuild(&stream,PagingResolve,PagingEmit,Fill,SrcVa,DstVa,(unsigned)Bytes,
                            StartByte,payload,budget,&written,&next);
    if (result==PagingStreamAddress) { *Unsupported=stream.Unsupported; goto Done; }
    if (result==PagingStreamInvalid) { status=STATUS_INVALID_PARAMETER; goto Done; }
    // Publish only complete packets. On translation/emit failure no part of this
    // batch is exposed to dxgkrnl. Earlier submitted batches need fault handling.
    // Packet bytes already reside in this DMA buffer's private payload.
    *DwordsWritten=written; *NextByte=next;
    if (result==PagingStreamMore) status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

static DWORD WINAPI StopThread(void* arg) { BC250_DEVICE* Device=arg; BC250_GFX* gfx;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    Device->Gfx = NULL;
SetEvent(stopEntered);
    ExReleaseFastMutex(&Device->GartLock);
    if (gfx != NULL) ExFreePoolWithTag(gfx, BC250_GFX_TAG);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();return 0; }

static DWORD WINAPI BuildThread(void*arg){
 ULONG words,next;BC250_WDDM_PAGING_UNSUPPORTED unsupported;u32 buffer[64];
 NTSTATUS st=GfxPagingBuild(arg,0x1000,0,0x2000,0x3000,4096,0,buffer,0,sizeof(buffer),0,&words,&next,&unsupported);
 if(st || words!=7 || next!=4096 || critical)InterlockedIncrement(&violations);
 return 0;
}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 struct funcs funcs={7};struct ring ring={1024,&funcs};BC250_GFX gfx={1,&ring,&ring};
 BC250_DEVICE d={SRWLOCK_INIT,0,&gfx};HANDLE a,b,stop;
 ULONG words,next;BC250_WDDM_PAGING_UNSUPPORTED unsupported;u32 buffer[64];
 buildersEntered=CreateEvent(NULL,TRUE,FALSE,NULL);buildersRelease=CreateEvent(NULL,TRUE,FALSE,NULL);stopEntered=CreateEvent(NULL,TRUE,FALSE,NULL);
 CHECK(buildersEntered && buildersRelease && stopEntered);
 a=CreateThread(NULL,0,BuildThread,&d,0,NULL);b=CreateThread(NULL,0,BuildThread,&d,0,NULL);CHECK(a&&b);
 CHECK(WaitForSingleObject(buildersEntered,5000)==WAIT_OBJECT_0);
 stop=CreateThread(NULL,0,StopThread,&d,0,NULL);CHECK(stop);
 /* Beyond the removed200ms deadline, the live object must still be untouched. */
 CHECK(WaitForSingleObject(stopEntered,300)==WAIT_TIMEOUT && !freed && d.Gfx==&gfx);
 SetEvent(buildersRelease);
 CHECK(WaitForSingleObject(a,5000)==WAIT_OBJECT_0 && WaitForSingleObject(b,5000)==WAIT_OBJECT_0);
 CHECK(WaitForSingleObject(stop,5000)==WAIT_OBJECT_0 && freed && !d.Gfx && !violations);
 CHECK(GfxPagingBuild(&d,0x1000,0,0,0,4096,0,buffer,0,sizeof(buffer),0,&words,&next,&unsupported)==STATUS_INVALID_PARAMETER);
 CHECK(!critical && !words);
 /* Reinitialize after stop and exercise early-exit unlocks; exclusive acquisition must succeed. */
 d.Gfx=&gfx;gfx.PagingReady=FALSE;
 CHECK(GfxPagingBuild(&d,0x1000,0,0,0,4096,0,buffer,0,sizeof(buffer),0,&words,&next,&unsupported)==0 && unsupported==BC250PagingNotReady);
 CHECK(!critical && TryAcquireSRWLockExclusive(&d.GfxPagingLock));ReleaseSRWLockExclusive(&d.GfxPagingLock);
 gfx.PagingReady=TRUE;
 CHECK(GfxPagingBuild(&d,0,0,0,0,4096,0,buffer,0,sizeof(buffer),0,&words,&next,&unsupported)==0 && unsupported==BC250PagingNoRoot);
 CHECK(!critical && TryAcquireSRWLockExclusive(&d.GfxPagingLock));ReleaseSRWLockExclusive(&d.GfxPagingLock);
 CloseHandle(a);CloseHandle(b);CloseHandle(stop);CloseHandle(buildersEntered);CloseHandle(buildersRelease);CloseHandle(stopEntered);
 puts("PASS: actual builder concurrent readers, stop waits beyond200ms, no premature detach/free, post-stop entry and early-return unlocks");return 0;
}
