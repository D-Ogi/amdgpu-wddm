// Executes the actual settingGate, budget and initial PnP admission functions with a
// registry model that distinguishes cached writes from durable flushes.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
typedef unsigned long ULONG;
typedef long NTSTATUS;
typedef unsigned char BOOLEAN;
typedef void* HANDLE;
typedef void* PVOID;
typedef ULONG* PULONG;
typedef const wchar_t* PCWSTR;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0L
#define STATUS_ACCESS_DENIED (-1L)
#define STATUS_OBJECT_NAME_NOT_FOUND (-2L)
#define STATUS_OBJECT_TYPE_MISMATCH (-3L)
#define STATUS_REGISTRY_IO_FAILED (-4L)
#define STATUS_DEVICE_CONFIGURATION_ERROR (-5L)
#define STATUS_DEVICE_HARDWARE_ERROR (-6L)
#define NT_SUCCESS(s) ((s)>=0)
#define PASSIVE_LEVEL 0
#define BC250_MAX_UNCONFIRMED_STARTS 2u
#define StageStartEnter 30u
#define StageStartGuardPassed 31u
#define StageRefusedByGuard 90u
typedef struct {BOOLEAN InheritedSignalValid,GpuStopUnconfirmed;} BC250_DEVICE;
typedef void* PDXGK_START_INFO;
typedef void* PDXGKRNL_INTERFACE;
static ULONG settingGate,cached,durable;
static NTSTATUS openStatus,readStatus,writeStatus,flushStatus;
static unsigned opens,writes,flushes,closes,admissions,checks,failures,logs,episodes;
static char lastLog[180];
static BOOLEAN g_FullWddm;
static int KeGetCurrentIrql(void){return PASSIVE_LEVEL;}
static NTSTATUS OpenParameters(HANDLE* key){opens++;*key=(HANDLE)1;return openStatus;}
static NTSTATUS ReadDword(HANDLE key,PCWSTR name,ULONG* value)
{(void)key;*value=0;if(!NT_SUCCESS(readStatus))return readStatus;*value=!wcscmp(name,L"EnableFullWddm")?settingGate:cached;return STATUS_SUCCESS;}
static NTSTATUS WriteDword(HANDLE key,PCWSTR name,ULONG value)
{(void)key;writes++;if(!NT_SUCCESS(writeStatus))return writeStatus;if(!wcscmp(name,L"EnableFullWddm"))settingGate=value;else cached=value;return STATUS_SUCCESS;}
static NTSTATUS ZwFlushKey(HANDLE key){(void)key;flushes++;if(NT_SUCCESS(flushStatus))durable=cached;return flushStatus;}
static void ZwClose(HANDLE key){(void)key;closes++;}
static void GuardLog(const char* format,...){logs++;strcpy_s(lastLog,sizeof(lastLog),format);}
static void GuardStage(ULONG stage){if(stage==StageStartGuardPassed)admissions++;}
// One kept log file per device start: the start names its episode before anything else can log.
static void GuardLogKeepEpisode(PCWSTR label){if(!wcscmp(label,L"start"))episodes++;}
#include "guard_actual.inc"
#include "gate_actual.inc"
// The start bookkeeping after the guard (start_health.c, cumode.c) is not what this test is about.
static void StartHealthBegin(BC250_DEVICE* d,BOOLEAN full){(void)d;(void)full;}
static void CuModeBegin(BC250_DEVICE* d){(void)d;}
#include "pnp_actual.inc"
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static void Reset(void)
{settingGate=cached=durable=0;openStatus=readStatus=writeStatus=flushStatus=0;opens=writes=flushes=closes=admissions=logs=episodes=0;g_FullWddm=FALSE;lastLog[0]=0;}
static NTSTATUS Start(void){BC250_DEVICE d={0};ULONG src=99,children=99;NTSTATUS s=Bc250StartDevice(&d,NULL,NULL,&src,&children);CHECK(src==0&&children==0);CHECK(episodes==1);return s;}
int main(void)
{
    unsigned before;
    Reset();settingGate=2;
    CHECK(WddmGateOpen() && settingGate==2 && writes==0 && flushes==0);
    before=opens;CHECK(WddmFullTableSelected()&&opens==before&&writes==0);
    CHECK(Start()==STATUS_SUCCESS&&admissions==1&&cached==1&&durable==1&&flushes==1);
    Reset();settingGate=2;CHECK(WddmGateOpen());readStatus=STATUS_OBJECT_NAME_NOT_FOUND;
    CHECK(Start()==STATUS_SUCCESS&&durable==1&&admissions==1); // absent initial count
    Reset();settingGate=2;CHECK(WddmGateOpen());openStatus=STATUS_ACCESS_DENIED;
    CHECK(Start()==STATUS_ACCESS_DENIED&&admissions==0&&writes==0&&flushes==0);
    Reset();settingGate=2;CHECK(WddmGateOpen());readStatus=STATUS_OBJECT_TYPE_MISMATCH;
    CHECK(Start()==STATUS_OBJECT_TYPE_MISMATCH&&admissions==0&&writes==0&&closes==2);
    Reset();settingGate=2;CHECK(WddmGateOpen());readStatus=STATUS_ACCESS_DENIED;
    CHECK(Start()==STATUS_ACCESS_DENIED&&admissions==0&&writes==0);
    Reset();settingGate=2;CHECK(WddmGateOpen());writeStatus=STATUS_ACCESS_DENIED;
    CHECK(Start()==STATUS_ACCESS_DENIED&&admissions==0&&flushes==0&&durable==0);
    Reset();settingGate=2;CHECK(WddmGateOpen());flushStatus=STATUS_REGISTRY_IO_FAILED;
    CHECK(Start()==STATUS_REGISTRY_IO_FAILED&&admissions==0&&cached==1&&durable==0);
    CHECK(strstr(lastLog,"flush")!=NULL);
    Reset();settingGate=2;CHECK(WddmGateOpen());cached=durable=2;
    CHECK(Start()==STATUS_DEVICE_CONFIGURATION_ERROR&&admissions==0&&writes==0);
    Reset();settingGate=1;writeStatus=STATUS_ACCESS_DENIED;
    CHECK(!WddmGateOpen()&&settingGate==1&&flushes==0);
    Reset();settingGate=1;flushStatus=STATUS_REGISTRY_IO_FAILED;
    CHECK(!WddmGateOpen()&&settingGate==0&&flushes==1); // no admission from cached closure
    Reset();settingGate=1;CHECK(WddmGateOpen()&&settingGate==0&&flushes==1);
    before=opens;CHECK(WddmFullTableSelected()&&opens==before); // no second consumption
    Reset();openStatus=STATUS_ACCESS_DENIED;CHECK(Start()==STATUS_SUCCESS&&admissions==1);
    Reset();readStatus=STATUS_OBJECT_TYPE_MISMATCH;CHECK(Start()==STATUS_SUCCESS&&admissions==1);
    Reset();writeStatus=STATUS_ACCESS_DENIED;CHECK(Start()==STATUS_SUCCESS&&admissions==1);
    Reset();flushStatus=STATUS_REGISTRY_IO_FAILED;CHECK(Start()==STATUS_SUCCESS&&admissions==1);
    Reset();cached=2;CHECK(Start()==STATUS_DEVICE_CONFIGURATION_ERROR&&admissions==0);
    printf("durable start guard: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
