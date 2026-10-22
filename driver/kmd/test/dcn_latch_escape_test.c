// The exact dispatch helper, with no WDK or hardware operations.
#include <stdio.h>
#include <string.h>
#include "dcn_latch_escape.h"
typedef unsigned long ULONG;
typedef long NTSTATUS;
typedef unsigned char BOOLEAN;
typedef struct _BC250_DEVICE { int FullWddm; long RetainedPowerPhase; } BC250_DEVICE;
typedef union { struct { unsigned long HardwareAccess:1; }; unsigned long Value; } D3DDDI_ESCAPEFLAGS;
#define STATUS_SUCCESS 0L
#define STATUS_INVALID_PARAMETER ((long)0xc000000d)
#define STATUS_ACCESS_DENIED ((long)0xc0000022)
#define STATUS_INVALID_DEVICE_STATE ((long)0xc0000184)
#define STATUS_DEVICE_NOT_READY ((long)0xc00000a3)
#define STATUS_RETRY ((long)0xc000022d)
#define NT_SUCCESS(s) ((s)>=0)
#define PASSIVE_LEVEL 0
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
static long InterlockedCompareExchange(long* p,long value,long compare)
{ long old=*p; if (old==compare) *p=value; return old; }
static int irql, allowed, calls, gates, admits, failed, checks;
static long coreStatus;
static unsigned long coreArm;
static int KeGetCurrentIrql(void) { return irql; }
static int LabDiagnosticsAllowed(void) { ++gates; return allowed; }
enum { LabEscapeLatchProbe };
static void LabEscapeAdmitted(int family,const char* marker)
{ if (family!=LabEscapeLatchProbe || strcmp(marker,"lab:latch-probe")) ++failed; ++admits; }
long DcnLatchProbe(BC250_DEVICE* d,unsigned long arm,BC250_LATCH_PROBE_RESULT* r)
{ (void)d; ++calls; coreArm=arm; r->Arm=arm; r->Status=coreStatus; r->Samples=7; return coreStatus; }
/* ACTUAL_HANDLER */
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
static BC250_ESCAPE_LATCH_PROBE request(void)
{
    BC250_ESCAPE_LATCH_PROBE d;
    memset(&d,0,sizeof(d));
    d.Magic=BC250_ESCAPE_MAGIC; d.Command=BC250_ESCAPE_LATCH_PROBE_COMMAND;
    d.AbiVersion=BC250_LATCH_PROBE_ABI; d.ExpectedVersion=BC250_KMD_VERSION;
    // Output cannot be used to inject an address or fabricate a measurement.
    memset(&d.Result,0xcc,sizeof(d.Result));
    calls=gates=admits=irql=0; allowed=1; coreStatus=0; coreArm=99;
    return d;
}
static void refused(BC250_ESCAPE_LATCH_PROBE* d,long status)
{
    CHECK(!calls && !admits);
    CHECK(d->NtStatus==(ULONG)status);
    CHECK(d->Status!=BC250_ESCAPE_STATUS_DONE);
    CHECK(d->Result.Samples==0 && d->Result.WritesAttempted==0);
    CHECK(d->Result.AddressA==0 && d->Result.AddressB==0);
    CHECK(d->Result.Before.ValidMask==0);
    CHECK(d->Result.RestoreStatus==STATUS_INVALID_DEVICE_STATE);
    CHECK(d->Magic==BC250_ESCAPE_MAGIC && d->Command==BC250_ESCAPE_LATCH_PROBE_COMMAND);
    CHECK(d->Version==BC250_KMD_VERSION && d->AbiVersion==BC250_LATCH_PROBE_ABI);
}
int main(void)
{
    BC250_DEVICE device={1,0};
    BC250_ESCAPE_LATCH_PROBE d;
    unsigned long i;
    for (i=0;i<256;++i) {
        if (i==1) continue;
        d=request(); LatchProbeEscape(&device,&d,1,i); refused(&d,STATUS_INVALID_PARAMETER); CHECK(!gates);
    }
    for (i=0;i<32;++i) {
        if (i==0) continue;
        d=request(); LatchProbeEscape(&device,&d,1,1ul<<i); refused(&d,STATUS_INVALID_PARAMETER);
    }
    for (i=0;i<8;++i) {
        d=request(); d.Reserved[i]=1; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_INVALID_PARAMETER);
        CHECK(!gates && !d.Reserved[i]);
    }
    d=request(); LatchProbeEscape(&device,&d,0,1); refused(&d,STATUS_ACCESS_DENIED);
    CHECK(!gates && d.Status==BC250_ESCAPE_STATUS_NOT_ADMIN);
    d=request(); irql=2; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_INVALID_DEVICE_STATE); CHECK(!gates);
    d=request(); allowed=0; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_ACCESS_DENIED); CHECK(gates==1);
    d=request(); d.AbiVersion=0; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_INVALID_PARAMETER);
    d=request(); d.ExpectedVersion^=1; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_INVALID_PARAMETER);
    d=request(); d.Arm=Bc250LatchArmCount; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_INVALID_PARAMETER);
    d=request(); d.Arm=0xffffffff; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_INVALID_PARAMETER);
    d=request(); device.FullWddm=0; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_DEVICE_NOT_READY);
    device.FullWddm=1;
    d=request(); device.RetainedPowerPhase=1; LatchProbeEscape(&device,&d,1,1); refused(&d,STATUS_DEVICE_NOT_READY);
    device.RetainedPowerPhase=0;
    for (i=0;i<Bc250LatchArmCount;++i) {
        d=request(); d.Arm=i; LatchProbeEscape(&device,&d,1,1);
        CHECK(calls==1 && admits==1 && gates==1 && coreArm==i);
        CHECK(d.Status==BC250_ESCAPE_STATUS_DONE && d.NtStatus==0 && d.Result.Samples==7);
        d=request(); d.Arm=i; coreStatus=STATUS_RETRY; LatchProbeEscape(&device,&d,1,1);
        CHECK(calls==1 && admits==1 && coreArm==i);
        CHECK(d.Status==BC250_ESCAPE_STATUS_REFUSED && d.NtStatus==(ULONG)STATUS_RETRY);
        CHECK(d.Result.Status==STATUS_RETRY && d.Result.Samples==7);
    }
    printf("Latch escape: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
