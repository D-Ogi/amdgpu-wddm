
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
