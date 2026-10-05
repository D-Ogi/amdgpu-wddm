
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
