/* CPU-only checks of the production lab gate and effective-token predicate. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef unsigned char BOOLEAN;
typedef unsigned long ULONG;
typedef long LONG, NTSTATUS;
typedef void* PVOID;
typedef ULONG* PULONG;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define NTSYSAPI
#define NTAPI
#define RtlZeroMemory(p,n) memset(p,0,n)
static unsigned Checks,Failures,Queries,Logs;
static ULONG Setting,Options,Returned;
static NTSTATUS QueryStatus;
static int Irql;
#define CHECK(x) do { ++Checks; if(!(x)) { ++Failures; printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
static LONG InterlockedCompareExchange(volatile LONG* p,LONG value,LONG compare)
{ LONG old=*p; if(old==compare)*p=value; return old; }
static LONG InterlockedExchange(volatile LONG* p,LONG value) { LONG old=*p;*p=value;return old; }
static LONG InterlockedOr(volatile LONG* p,LONG value) { LONG old=*p;*p|=value;return old; }
static int KeGetCurrentIrql(void) { return Irql; }
static ULONG GuardReadSetting(const wchar_t* name,ULONG fallback)
{ CHECK(!wcscmp(name,L"LabDiagnostics") && fallback==0);return Setting; }
static void GuardLog(const char* format,...) { (void)format;++Logs; }

#include "lab_gate_actual.c"

NTSTATUS NTAPI ZwQuerySystemInformation(ULONG kind,PVOID data,ULONG bytes,PULONG returned)
{
    BC250_SYSTEM_CODEINTEGRITY_INFORMATION* info=data;
    ++Queries;
    CHECK(kind==103 && bytes==sizeof(*info) && info->Length==sizeof(*info) && Irql==0);
    info->CodeIntegrityOptions=Options;*returned=Returned;return QueryStatus;
}

typedef struct { int PrimaryToken,ClientToken; } SECURITY_SUBJECT_CONTEXT;
static int Primary,Client,Locked,Captured,Released;
static void SeCaptureSubjectContext(SECURITY_SUBJECT_CONTEXT* p)
{ CHECK(!Captured);Captured=1;p->PrimaryToken=Primary;p->ClientToken=Client; }
static void SeLockSubjectContext(SECURITY_SUBJECT_CONTEXT* p) { (void)p;CHECK(Captured && !Locked);Locked=1; }
static int SeQuerySubjectContextToken(SECURITY_SUBJECT_CONTEXT* p)
{ CHECK(Locked);return p->ClientToken>=0 ? p->ClientToken : p->PrimaryToken; }
static BOOLEAN SeTokenIsAdmin(int token) { CHECK(Locked);return token==1; }
static void SeUnlockSubjectContext(SECURITY_SUBJECT_CONTEXT* p) { (void)p;CHECK(Locked);Locked=0; }
static void SeReleaseSubjectContext(SECURITY_SUBJECT_CONTEXT* p)
{ (void)p;CHECK(Captured && !Locked);Captured=0;++Released; }

/* ACTUAL_ADMIN */

static void Reset(void)
{
    Setting=1;Options=2;Returned=sizeof(BC250_SYSTEM_CODEINTEGRITY_INFORMATION);
    QueryStatus=0;Irql=0;Queries=Logs=0;g_TestSignState=0;g_LabEscapesSeen=0;
    Primary=1;Client=-1;Locked=Captured=Released=0;
}
int main(void)
{
    unsigned i;
    Reset();Setting=0;CHECK(!LabDiagnosticsAllowed() && Queries==0);
    Setting=2;CHECK(!LabDiagnosticsAllowed() && Queries==0);
    Reset();Options=0;CHECK(!LabDiagnosticsAllowed() && Queries==1);
    Options=2;CHECK(!LabDiagnosticsAllowed() && Queries==1); /* immutable boot policy is cached */
    Reset();CHECK(LabDiagnosticsAllowed() && Queries==1);
    Setting=0;CHECK(!LabDiagnosticsAllowed() && Queries==1); /* operator opt-in is not cached */
    Setting=1;CHECK(LabDiagnosticsAllowed() && Queries==1);
    Reset();Irql=2;CHECK(!LabDiagnosticsAllowed() && Queries==0 && !g_TestSignState);
    Irql=0;CHECK(LabDiagnosticsAllowed() && Queries==1);
    Reset();QueryStatus=(NTSTATUS)0xc0000001L;CHECK(!LabDiagnosticsAllowed() && Queries==1 && !g_TestSignState);
    QueryStatus=0;CHECK(LabDiagnosticsAllowed() && Queries==2);
    Reset();Returned=sizeof(BC250_SYSTEM_CODEINTEGRITY_INFORMATION)-1;
    CHECK(!LabDiagnosticsAllowed() && Queries==1 && !g_TestSignState);
    Returned++;CHECK(LabDiagnosticsAllowed() && Queries==2);
    Reset();for(i=0;i<(unsigned)LabEscapeCount;++i) {
        LabEscapeAdmitted((BC250_LAB_ESCAPE)i,"test");
        LabEscapeAdmitted((BC250_LAB_ESCAPE)i,"test");
        CHECK(Logs==i+1);
    }
    LabEscapeAdmitted(LabEscapeCount,"invalid");LabEscapeAdmitted((BC250_LAB_ESCAPE)-1,"invalid");
    CHECK(Logs==(unsigned)LabEscapeCount && LabEscapeConnectorReplug<LabEscapeCount);
    Reset();CHECK(WaveCallerIsAdmin() && Released==1 && !Locked && !Captured);
    Reset();Primary=0;CHECK(!WaveCallerIsAdmin() && Released==1);
    Reset();Primary=1;Client=0;CHECK(!WaveCallerIsAdmin() && Released==1); /* privileged broker, ordinary client */
    Reset();Primary=0;Client=1;CHECK(WaveCallerIsAdmin() && Released==1);
    printf("lab gate: %u checks, %u failures\n",Checks,Failures);
    return Failures ? 1 : 0;
}
