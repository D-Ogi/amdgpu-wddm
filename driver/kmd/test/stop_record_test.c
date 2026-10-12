// Actual guard.c recorder with the same registry fault model as StartRecord.
#define main start_record_base_main
#include "start_record_test.c"
#undef main
#define StageStopEnter 70u
#define StageStopDone 79u
#define BC250_START_HEALTH_CONFIRMED 8u
typedef struct { ULONG Flags; unsigned long long Generation, Epoch; } BC250_ESCAPE_START_HEALTH;
static struct { ULONG BootId; } sharedUserData={1234};
#define SharedUserData (&sharedUserData)
static BC250_ESCAPE_START_HEALTH stopHealth;
static int healthClosed;
static void StartHealthFault(BC250_DEVICE* d) { (void)d; healthClosed=1; }
static void StartHealthStop(BC250_DEVICE* d, BC250_ESCAPE_START_HEALTH* s)
{ (void)d; *s=stopHealth; healthClosed=1; }
#include "stop_actual.inc"

int main(void)
{
    BC250_DEVICE d={0};
    ULONG v;
    unsigned old;
    int fault;
    // Existing start regression, then independent stop scenarios.
    if (start_record_base_main()) return 1;
    Reset();
    // Only a device that reached Started can be the case this record is for. Every scenario below that expects a
    // record therefore sets it, exactly as pnp.c does before dxgkrnl can stop the adapter.
    d.Started=TRUE;
    stopHealth.Flags=7;stopHealth.Generation=123456789012ull;stopHealth.Epoch=2;
    ringTotal=200;keepLog=0;GuardStage(StageStartDone);
    GuardRecordStopBegin(&d);
    CHECK(healthClosed && g_StopRecordPending);
    CHECK(Str(L"Result") && !wcscmp(Str(L"Result"),L"stopping"));
    CHECK(Dword(L"StartStage",&v) && v==39);
    CHECK(Dword(L"HealthFlags",&v) && v==7);
    CHECK(Dword(L"BootId",&v) && v==1234);
    CHECK(Dword(L"LogLines",&v) && v==64);
    CHECK(Str(L"Generation") && !wcscmp(Str(L"Generation"),L"123456789012"));
    CHECK(Find(2,L"Log")>=0);
    ringTotal=2000;GuardStage(StageStopDone);GuardRecordStopDone();
    CHECK(Str(L"Result") && !wcscmp(Str(L"Result"),L"stopped"));
    CHECK(Dword(L"Stage",&v) && v==79);
    old=writeOps;GuardRecordStopDone();CHECK(writeOps==old);
    // Normal stop of a confirmed generation neither writes nor clears evidence.
    stopHealth.Flags=15;GuardRecordStopBegin(&d);GuardRecordStopDone();CHECK(writeOps==old);
    stopHealth.Flags=7;stopHealth.Generation++;GuardRecordStopBegin(&d);CHECK(writeOps>old);
    // A stop that unwinds a start which never reached a started device writes nothing, and keeps the record of
    // the stopped unconfirmed start above standing. It still closes health, as every stop entry must.
    old=writeOps;healthClosed=0;d.Started=FALSE;stopHealth.Generation++;
    GuardRecordStopBegin(&d);GuardRecordStopDone();
    CHECK(writeOps==old && healthClosed && !g_StopRecordPending);
    CHECK(Str(L"Result") && !wcscmp(Str(L"Result"),L"stopping"));
    d.Started=TRUE;
    old=writeOps;g_Irql=DISPATCH_LEVEL;GuardRecordStopBegin(&d);GuardRecordStopDone();CHECK(writeOps==old);
    // Failure of every individual field must not publish a complete reason.
    for(fault=1;fault<=17;fault++) {
        Reset();stopHealth.Flags=7;stopHealth.Generation=9;ringTotal=100;
        failAt=fault;writeStatus=STATUS_REGISTRY_IO_FAILED;
        GuardRecordStopBegin(&d);GuardRecordStopDone();
        CHECK(!Str(L"Result") || wcscmp(Str(L"Result"),L"stopped"));
        CHECK(!g_StopRecordPending);
    }
    Reset();stopHealth.Flags=7;stopHealth.Generation=9;poolRefused=1;
    GuardRecordStopBegin(&d);CHECK(!g_StopRecordPending && Find(2,L"Log")<0);
    Reset();stopHealth.Flags=7;stopHealth.Generation=9;flushStatus=STATUS_REGISTRY_IO_FAILED;
    GuardRecordStopBegin(&d);CHECK(!g_StopRecordPending);
    printf("stop record actual-source: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
