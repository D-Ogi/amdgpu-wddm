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
static BC250_GFX* GfxAccessAcquire(_In_ const BC250_DEVICE* Device)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Device;
    BC250_GFX* gfx = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&device->GfxAccessLock, &irql);
    if (!device->GfxAccessClosed && device->Gfx != NULL)
    {
        gfx = (BC250_GFX*)device->Gfx;
        if (device->GfxAccessUsers++ == 0) KeClearEvent(&device->GfxAccessDrained);
    }
    KeReleaseSpinLock(&device->GfxAccessLock, irql);
    return gfx;
}
static void GfxAccessRelease(_In_ const BC250_DEVICE* Device)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Device;
    KIRQL irql;
    KeAcquireSpinLock(&device->GfxAccessLock, &irql);
    NT_ASSERT(device->GfxAccessUsers != 0);
    if (--device->GfxAccessUsers == 0) KeSetEvent(&device->GfxAccessDrained, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&device->GfxAccessLock, irql);
}
static void GfxAccessClose(_Inout_ BC250_DEVICE* Device)
{
    KIRQL irql;
    KeAcquireSpinLock(&Device->GfxAccessLock, &irql);
    Device->GfxAccessClosed = TRUE;
    KeReleaseSpinLock(&Device->GfxAccessLock, irql);
    SetEvent(closeStarted);
}
static void GfxAccessOpen(_Inout_ BC250_DEVICE* Device)
{
    KIRQL irql;
    KeAcquireSpinLock(&Device->GfxAccessLock, &irql);
    Device->GfxAccessClosed = FALSE;
    KeReleaseSpinLock(&Device->GfxAccessLock, irql);
}
static BOOLEAN GfxFenceArrivedAccess(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page the CP writes. The outer lifetime reference protects
    // the device and fence page against concurrent FINI or Stop. SubmitAdev is only ever non-NULL between a submission and the teardown that frees
    // the page, and pnp.c drains ih.c's DPCs before that teardown runs (see the field's comment).
    if (gfx == NULL || gfx->SubmitAdev == NULL || Seq == 0) return FALSE;
    if ((ULONG)bc250_gfx_fence_read(gfx->SubmitAdev, BC250_SUBMIT_FENCE_SLOT) != Seq) return FALSE;
    // The sequence numbers only count up, so a slot holding Seq means that submission and every earlier one is done.
    if (gfx->SubmitSeq == Seq) InterlockedExchange(&gfx->SubmitInFlight, 0);
    return TRUE;
}
BOOLEAN GfxFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxFenceArrivedAccess(Device, Seq);
    GfxAccessRelease(Device);
    return result;
}
static BOOLEAN GfxPagingFenceArrivedAccess(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page SDMA0 writes, under the outer CPU lifetime reference - the node-1 twin of
    // GfxFenceArrived. PagingDevicePtr is only ever non-NULL between GfxEscape's capture and the teardown that
    // frees the fence page, and pnp.c drains ih.c's DPCs before that teardown runs, exactly as SubmitAdev's own
    // comment states.
    if (gfx == NULL || gfx->PagingDevicePtr == NULL || Seq == 0) return FALSE;
    if ((ULONG)bc250_sdma_fence_read(gfx->PagingDevicePtr, BC250_PAGING_FENCE_SLOT) != Seq) return FALSE;
    if (gfx->PagingSubmitSeq == Seq) InterlockedExchange(&gfx->PagingSubmitInFlight, 0);
    return TRUE;
}
BOOLEAN GfxPagingFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxPagingFenceArrivedAccess(Device, Seq);
    GfxAccessRelease(Device);
    return result;
}

static DWORD WINAPI GfxReader(void*p){if(!GfxFenceArrived(p,7))InterlockedIncrement(&errors);return 0;}
static DWORD WINAPI PagingReader(void*p){if(!GfxPagingFenceArrived(p,7))InterlockedIncrement(&errors);return 0;}
static DWORD WINAPI Close(void*p){GfxAccessClose(p);InterlockedExchange(&freed,1);SetEvent(closeDone);return 0;}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 BC250_GFX gfx={&gfx,&gfx,7,7,1,1};BC250_DEVICE d={SRWLOCK_INIT,NULL,0,FALSE,&gfx};HANDLE a,b,c;
 readersEntered=CreateEvent(NULL,TRUE,FALSE,NULL);readersRelease=CreateEvent(NULL,TRUE,FALSE,NULL);
 closeStarted=CreateEvent(NULL,TRUE,FALSE,NULL);closeDone=CreateEvent(NULL,TRUE,FALSE,NULL);
 d.GfxAccessDrained=CreateEvent(NULL,TRUE,TRUE,NULL);CHECK(readersEntered&&readersRelease&&closeStarted&&closeDone&&d.GfxAccessDrained);
 a=CreateThread(NULL,0,GfxReader,&d,0,NULL);b=CreateThread(NULL,0,PagingReader,&d,0,NULL);CHECK(a&&b);
 CHECK(WaitForSingleObject(readersEntered,5000)==WAIT_OBJECT_0);
 c=CreateThread(NULL,0,Close,&d,0,NULL);CHECK(c);
 CHECK(WaitForSingleObject(closeStarted,5000)==WAIT_OBJECT_0);
 CHECK(!GfxFenceArrived(&d,7) && !GfxPagingFenceArrived(&d,7) && reads==2);
 CHECK(WaitForSingleObject(closeDone,300)==WAIT_TIMEOUT && !freed);
 SetEvent(readersRelease);
 CHECK(WaitForSingleObject(a,5000)==WAIT_OBJECT_0 && WaitForSingleObject(b,5000)==WAIT_OBJECT_0 && WaitForSingleObject(c,5000)==WAIT_OBJECT_0);
 CHECK(freed && !errors && !gfx.SubmitInFlight && !gfx.PagingSubmitInFlight && d.GfxAccessUsers==0);
 /* Repeated close is immediate; reopening resets the event on first admission. */
 GfxAccessClose(&d);freed=0;GfxAccessOpen(&d);
 CHECK(GfxAccessAcquire(&d)==&gfx && d.GfxAccessUsers==1 && WaitForSingleObject(d.GfxAccessDrained,0)==WAIT_TIMEOUT);
 GfxAccessRelease(&d);CHECK(WaitForSingleObject(d.GfxAccessDrained,0)==WAIT_OBJECT_0);
 CHECK(!GfxFenceArrived(&d,0) && d.GfxAccessUsers==0);
 GfxAccessClose(&d);d.Gfx=NULL;GfxAccessOpen(&d);CHECK(!GfxAccessAcquire(&d) && d.GfxAccessUsers==0);
 CloseHandle(a);CloseHandle(b);CloseHandle(c);CloseHandle(readersEntered);CloseHandle(readersRelease);CloseHandle(closeStarted);CloseHandle(closeDone);CloseHandle(d.GfxAccessDrained);
 puts("PASS: actual GFX/SDMA fence callbacks, close blocks new admission and waits active readers, release/event ordering, repeated close, reopen and NULL object");return 0;
}
