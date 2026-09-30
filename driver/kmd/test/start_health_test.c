// Actual kernel policy with deterministic registry/lifecycle interleavings.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
typedef long LONG,NTSTATUS,LONG32;
typedef long long LONG64,LONGLONG;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef int BOOLEAN,KIRQL,KMUTEX,KSPIN_LOCK,EX_RUNDOWN_REF;
typedef void* HANDLE;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { ULONG Value; struct {ULONG HardwareAccess:1,Unused:2,NoAdapterSynchronization:1;} ; } D3DDDI_ESCAPEFLAGS;
#define TRUE 1
#define FALSE 0
#define Executive 0
#define KernelMode 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_RETRY ((NTSTATUS)0xC000022D)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3)
#define STATUS_DELETE_PENDING ((NTSTATUS)0xC0000056)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define NT_SUCCESS(s) ((s)>=0)
#define GuardLog(...) ((void)0)
#include "start_health_state.inc"
typedef struct {BC250_START_HEALTH_STATE StartHealth;} BC250_DEVICE;
static unsigned checks,failures,spins,mutexes,writes,flushes,closes;
static ULONGLONG clock100ns=10000;
static LONGLONG qpc=100;
static NTSTATUS openStatus,writeStatus,flushStatus;
static int faultDuringFlush;
static BC250_DEVICE* live;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static void KeInitializeMutex(KMUTEX*p,int n){(void)n;*p=0;}
static void KeInitializeSpinLock(KSPIN_LOCK*p){*p=0;}
static void ExInitializeRundownProtection(EX_RUNDOWN_REF*p){*p=0;}
static int KeWaitForSingleObject(KMUTEX*p,int a,int b,int c,void*d){(void)a;(void)b;(void)c;(void)d;CHECK(!*p);*p=1;mutexes++;return 0;}
static void KeReleaseMutex(KMUTEX*p,int a){(void)a;CHECK(*p==1);*p=0;mutexes--;}
static void KeAcquireSpinLock(KSPIN_LOCK*p,KIRQL*i){CHECK(!*p);*p=1;spins++;*i=0;}
static void KeReleaseSpinLock(KSPIN_LOCK*p,KIRQL i){(void)i;CHECK(*p==1);*p=0;spins--;}
static int ExAcquireRundownProtection(EX_RUNDOWN_REF*p){if(*p<0)return 0;(*p)++;return 1;}
static void ExReleaseRundownProtection(EX_RUNDOWN_REF*p){CHECK(*p>0);(*p)--;}
static void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF*p){CHECK(*p==0);*p=-1;}
static ULONGLONG KeQueryInterruptTime(void){return clock100ns;}
static LARGE_INTEGER KeQueryPerformanceCounter(void*p){LARGE_INTEGER r={0};(void)p;r.QuadPart=qpc;return r;}
static LONGLONG InterlockedCompareExchange64(volatile LONGLONG*p,LONGLONG v,LONGLONG c){LONGLONG old=*p;if(old==c)*p=v;return old;}
void StartHealthFault(BC250_DEVICE*);
static NTSTATUS OpenParameters(HANDLE*k){*k=(void*)1;return openStatus;}
static NTSTATUS WriteDword(HANDLE k,const unsigned short*n,ULONG v){(void)k;(void)n;CHECK(v==0 && spins==0 && mutexes==1);writes++;return writeStatus;}
static NTSTATUS ZwFlushKey(HANDLE k){(void)k;CHECK(spins==0 && mutexes==1);flushes++;if(faultDuringFlush)StartHealthFault(live);return flushStatus;}
static void ZwClose(HANDLE k){(void)k;closes++;}
#define RtlZeroMemory(p,n) memset((p),0,(n))
// cumode.c: a durable start-health confirmation also confirms a pending 40 CU request, under Lifecycle,
// with no spin lock held (it does registry I/O).
static unsigned cuConfirms;
static NTSTATUS CuModeConfirm(BC250_DEVICE*d,const char*why){(void)d;(void)why;CHECK(spins==0 && mutexes==1);cuConfirms++;return STATUS_SUCCESS;}
#include "confirm_actual.inc"
#include "start_health_actual.inc"
static BC250_ESCAPE_START_HEALTH query(BC250_DEVICE*d,int confirm)
{
    BC250_ESCAPE_START_HEALTH r={0};
    r.Magic=BC250_ESCAPE_MAGIC;r.Command=BC250_ESCAPE_RUN_START_HEALTH;r.AbiVersion=1;r.Op=(ULONG)confirm;
    r.ExpectedGeneration=d->StartHealth.Generation;r.ExpectedEpoch=d->StartHealth.Epoch;
    StartHealthRequest(d,&r,TRUE,confirm?1:8);return r;
}
static void ready(BC250_DEVICE*d)
{
    StartHealthBegin(d,TRUE);StartHealthReady(d,TRUE);
    StartHealthEnter(d);StartHealthDisplayLocked(d,TRUE,TRUE);StartHealthLeave(d);
}
static void advance(unsigned ms){clock100ns+=(ULONGLONG)ms*10000;}
static void healthy(BC250_DEVICE*d)
{
    unsigned i;ready(d);
    for(i=1;i<=12;i++){advance(5000);StartHealthCompleted(d,2*i);}
}
int main(void)
{
    BC250_DEVICE d={0};BC250_ESCAPE_START_HEALTH r;ULONGLONG generation,epoch,count;unsigned old;
    live=&d;StartHealthInitialize(&d);CHECK(sizeof(r)==96);
    r=query(&d,0);CHECK(!(r.Flags&BC250_START_HEALTH_READY));
    healthy(&d);r=query(&d,0);CHECK(r.Flags==7 && r.Completed==12 && r.ReadyAgeMs==60000 && r.LastCompletionAgeMs==0);
    r=query(&d,1);CHECK(r.Status==0 && r.Flags==15 && writes==1 && flushes==1 && cuConfirms==1);
    CHECK(StartHealthIsReady(&d,&generation) && generation==d.StartHealth.Generation);
    // Unchanged visibility and one repeated primary cannot manufacture progress.
    epoch=d.StartHealth.Epoch;count=d.StartHealth.Completed;
    StartHealthEnter(&d);StartHealthDisplayLocked(&d,TRUE,TRUE);StartHealthLeave(&d);
    StartHealthCompleted(&d,24);CHECK(d.StartHealth.Epoch==epoch && d.StartHealth.Completed==count);
    advance(15001);r=query(&d,1);CHECK(r.NtStatus==(ULONG)STATUS_DEVICE_NOT_READY && writes==1 && cuConfirms==1);
    CHECK(!StartHealthIsReady(&d,&generation)); // stale completions: not the milestone either
    ready(&d);advance(60000);r=query(&d,1);CHECK(r.NtStatus==(ULONG)STATUS_DEVICE_NOT_READY); // no completed primary
    ready(&d);advance(59999);StartHealthCompleted(&d,2);r=query(&d,1);CHECK(r.NtStatus==(ULONG)STATUS_DEVICE_NOT_READY);
    advance(1);r=query(&d,1);CHECK(r.Status==0);
    generation=d.StartHealth.Generation;StartHealthBegin(&d,TRUE);CHECK(d.StartHealth.Generation>generation); // same mocked QPC
    healthy(&d);r=query(&d,0);r.Op=1;r.ExpectedGeneration--;old=writes;StartHealthRequest(&d,&r,TRUE,1);CHECK(r.NtStatus==(ULONG)STATUS_RETRY && writes==old);
    healthy(&d);StartHealthClose(&d);r=query(&d,1);CHECK(!(r.Flags&2) && r.Status==2); // stop/power closes before teardown
    StartHealthReady(&d,TRUE);r=query(&d,0);CHECK(!(r.Flags&2)); // D0 cannot reopen
    healthy(&d);epoch=d.StartHealth.Epoch;StartHealthEnter(&d);StartHealthDisplayLocked(&d,FALSE,TRUE);StartHealthLeave(&d);CHECK(d.StartHealth.Epoch>epoch);r=query(&d,1);CHECK(r.Status==2);
    healthy(&d);StartHealthEnter(&d);StartHealthDisplayLocked(&d,TRUE,FALSE);StartHealthVisibilityLocked(&d,TRUE);StartHealthLeave(&d);
    r=query(&d,1);CHECK(!(r.Flags&4) && r.Status==2); // visibility cannot undo path power-off
    healthy(&d);openStatus=STATUS_ACCESS_DENIED;old=writes;r=query(&d,1);CHECK(r.NtStatus==(ULONG)openStatus && writes==old && !(r.Flags&8));openStatus=0;
    healthy(&d);writeStatus=STATUS_ACCESS_DENIED;old=flushes;r=query(&d,1);CHECK(r.NtStatus==(ULONG)writeStatus && flushes==old && !(r.Flags&8));writeStatus=0;
    old=cuConfirms;healthy(&d);flushStatus=STATUS_UNSUCCESSFUL;r=query(&d,1);CHECK(r.NtStatus==(ULONG)flushStatus && !(r.Flags&8) && cuConfirms==old);flushStatus=0;
    healthy(&d);epoch=d.StartHealth.Epoch;faultDuringFlush=1;old=writes;r=query(&d,1);faultDuringFlush=0;
    CHECK(r.NtStatus==(ULONG)STATUS_RETRY && r.Epoch!=epoch && !(r.Flags&(2|8)) && writes==old+1);
    CHECK(d.StartHealth.ConfirmedEpoch==epoch); // old interval only, no compensating counter rewrite
    healthy(&d);r=query(&d,0);r.Op=1;old=writes;StartHealthRequest(&d,&r,FALSE,1);CHECK(r.Status==3 && writes==old);
    r=query(&d,0);r.Op=1;StartHealthRequest(&d,&r,TRUE,9);CHECK(r.NtStatus==(ULONG)STATUS_INVALID_PARAMETER && writes==old);
    r=query(&d,0);r.AbiVersion=2;StartHealthRequest(&d,&r,TRUE,8);CHECK(r.Status==2);
    healthy(&d);r=query(&d,1);CHECK(r.Flags==15);
    generation=d.StartHealth.Generation;epoch=d.StartHealth.Epoch;old=writes;
    StartHealthClose(&d);StartHealthResumeReady(&d,TRUE);r=query(&d,0);
    CHECK(r.Generation==generation && r.Epoch>epoch && r.Flags==3 && !r.Completed && writes==old);
    advance(60001);StartHealthCompleted(&d,2);r=query(&d,1);
    CHECK(r.Status==2 && !r.Completed && writes==old); // old progress/hidden time cannot confirm resume
    StartHealthEnter(&d);StartHealthVisibilityLocked(&d,TRUE);StartHealthLeave(&d);
    advance(60000);StartHealthCompleted(&d,2);r=query(&d,1);
    CHECK(r.Status==0 && r.Flags==15 && r.Generation==generation && writes==old+1);
    StartHealthRemove(&d);r=query(&d,0);CHECK(r.NtStatus==(ULONG)STATUS_DELETE_PENDING);
    CHECK(spins==0 && mutexes==0 && closes>0);
    printf("start health actual-source: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
