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
