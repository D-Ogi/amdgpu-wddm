/* Production start admission and debt assignment, no MMIO or driver access. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long ULONG;
typedef long LONG;
typedef uint64_t ULONGLONG;
typedef unsigned short USHORT;
typedef unsigned char BOOLEAN;
typedef uintptr_t KSPIN_LOCK;
#define TRUE 1
#define FALSE 0
#include "dpaudio.h"
#include "bc250kmd_escape.h"
typedef struct TEST_RUN {unsigned Wrote,StreamWrote;} TEST_RUN;
typedef struct _BC250_DEVICE {BC250_DPAUDIO DpAudio;} BC250_DEVICE;
typedef unsigned KIRQL;
static unsigned lockDepth,lockCalls,lockFailures;
static void KeAcquireSpinLock(KSPIN_LOCK* lock,KIRQL* old)
{(void)lock;if(lockDepth)++lockFailures;*old=0;lockDepth=1;++lockCalls;}
static void KeReleaseSpinLock(KSPIN_LOCK* lock,KIRQL old)
{(void)lock;if(lockDepth!=1 || old!=0)++lockFailures;lockDepth=0;}
#include "dpaudio_debt_actual.inc"
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("line%u: %s\n",__LINE__,#x);}}while(0)
int main(void)
{
    BC250_DPAUDIO a,b;TEST_RUN run;unsigned flags,resume;
    BC250_DEVICE device;
    for(flags=0;flags<4;++flags) {
        memset(&device,0,sizeof(device));device.DpAudio.Written=(BOOLEAN)(flags&1);
        device.DpAudio.StreamWritten=(BOOLEAN)((flags>>1)&1);b=device.DpAudio;
        CHECK(DpAudioQuiesced(&device)==(flags==0));
        CHECK(!memcmp(&b,&device.DpAudio,sizeof(b)));
        CHECK(lockCalls==flags+1&&!lockDepth&&!lockFailures);
    }
    for(flags=1;flags<4;++flags)for(resume=0;resume<2;++resume) {
        memset(&a,0x5a,sizeof(a));a.Written=(BOOLEAN)(flags&1);a.StreamWritten=(BOOLEAN)((flags>>1)&1);
        b=a;CHECK(!BeginStartLocked(&a,(BOOLEAN)resume,0,0,0,0));
        CHECK(!memcmp(&a,&b,sizeof(a))); // original endpoint, protocol and diagnostics unchanged
    }
    memset(&a,0,sizeof(a));a.Endpoint=1;a.Stream=1;a.TmdsMode=1;a.PortId=0x12345678;
    a.Notes=99;a.LastStatus=-1;a.StreamState=9;a.MismatchOffset=123;
    CHECK(BeginStartLocked(&a,FALSE,1,1,1,1));CHECK(a.Starts==1&&!a.Resumes);
    CHECK(!a.Written&&!a.StreamWritten&&a.Endpoint==1&&a.Stream==1&&a.TmdsMode==1);
    CHECK(a.PortId==0x12345678&&a.Notes==0&&a.LastStatus==0&&a.MismatchOffset==0);
    CHECK(a.StreamState==BC250_DPAUDIO_STREAM_OFF&&a.StreamStep==BC250_DPAUDIO_STEP_NONE);
    CHECK(BeginStartLocked(&a,TRUE,0,0,0,0));CHECK(a.Starts==1&&a.Resumes==1);
    for(flags=0;flags<4;++flags) {
        memset(&a,0,sizeof(a));run.Wrote=flags&1;run.StreamWrote=(flags>>1)&1;
        RecordDebtActual(&a,&run,TRUE);
        CHECK(a.Written==(BOOLEAN)run.Wrote&&a.StreamWritten==(BOOLEAN)run.StreamWrote&&a.TmdsMode);
        b=a;CHECK(BeginStartLocked(&a,FALSE,1,1,1,1)==(flags==0));
        if(flags)CHECK(!memcmp(&a,&b,sizeof(a)));
    }
    printf("audio debt: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
