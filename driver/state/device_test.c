// Actual state_device.c and the StartDevice admission prefix, CPU only.
// Kernel calls are bounded models; identity.c uses Windows' SHA-256 provider.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <stdio.h>
#include <string.h>
#include "identity.h"
#include "transfer.h"

typedef LONG NTSTATUS;
typedef void *PDEVICE_OBJECT;
typedef struct { ULONG Attributes; } OBJECT_ATTRIBUTES;
typedef struct { ULONG unused; } DXGK_START_INFO,*PDXGK_START_INFO;
typedef struct { ULONG unused; } DXGKRNL_INTERFACE,*PDXGKRNL_INTERFACE;
typedef struct { PDEVICE_OBJECT PhysicalDeviceObject; BOOLEAN InheritedSignalValid,GpuStopUnconfirmed; } BC250_DEVICE;
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001L)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022L)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011L)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xc0000182L)
#define STATUS_DEVICE_HARDWARE_ERROR ((NTSTATUS)0xc0000185L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009aL)
#define STATUS_REGISTRY_IO_FAILED ((NTSTATUS)0xc000014dL)
#define PASSIVE_LEVEL 0
#define OBJ_KERNEL_HANDLE 0x200u
#define RtlCompareMemory(a,b,n) (memcmp((a),(b),(n))?0:(n))
#define InitializeObjectAttributes(a,n,f,r,s) do{(void)(n);(void)(r);(void)(s);(a)->Attributes=(f);}while(0)
enum { StageStartEnter=1,StageStartFailed,StageRefusedByGuard,StageStartGuardPassed };

static unsigned checks,failures,receipt_calls,property_calls,thread_calls,wait_calls,close_calls;
static unsigned activation_calls,termination_calls,guard_calls,budget_calls,boundary_calls,order,activation_order,budget_order;
static const char *test_case;
static UCHAR irql;
static BOOL owned,system_worker,thread_live;
static NTSTATUS receipt_status,property_status,thread_status,wait_status,activation_status,budget_status;
static ULONG property_bytes;
static DEVPROPTYPE property_type;
static WCHAR property_text[513];
static BYTE activated_hash[32];
static AMDGPU_TRANSFER_HEADER fixture_receipt;
static void (*pending_worker)(PVOID);
static PVOID pending_context;
static const WCHAR FixtureId[]=L"PCI\\VEN_1002&DEV_13FE\\CPU_TEST_ONLY";
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %s line %u: %s\n",test_case,(unsigned)__LINE__,#x);}}while(0)

static UCHAR KeGetCurrentIrql(void){return irql;}
static NTSTATUS DriverStateReceipt(AMDGPU_TRANSFER_HEADER *out)
{++receipt_calls;if(NT_SUCCESS(receipt_status))*out=fixture_receipt;return receipt_status;}
static NTSTATUS IoGetDevicePropertyData(PDEVICE_OBJECT device,const DEVPROPKEY *key,LCID locale,ULONG flags,
    ULONG capacity,PVOID out,PULONG needed,DEVPROPTYPE *type)
{
    ++property_calls;CHECK(device==(PDEVICE_OBJECT)1);CHECK(IsEqualGUID(&key->fmtid,&DEVPKEY_Device_InstanceId.fmtid));
    CHECK(key->pid==DEVPKEY_Device_InstanceId.pid && locale==LOCALE_NEUTRAL && flags==0);
    CHECK(capacity==sizeof(property_text));
    if(NT_SUCCESS(property_status)){
        memcpy(out,property_text,capacity);*needed=property_bytes;*type=property_type;
    }
    return property_status;
}
static NTSTATUS DriverStateActivate(const BYTE hash[32])
{
    ++activation_calls;activation_order=++order;CHECK(system_worker);
    memcpy(activated_hash,hash,32);return activation_status;
}
static NTSTATUS PsCreateSystemThread(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *attributes,
    HANDLE process,PVOID client,void (*start)(PVOID),PVOID context)
{
    ++thread_calls;CHECK(access==SYNCHRONIZE && attributes->Attributes==OBJ_KERNEL_HANDLE);
    CHECK(!process && !client && start && context && !thread_live);
    if(!NT_SUCCESS(thread_status))return thread_status;
    pending_worker=start;pending_context=context;thread_live=TRUE;*out=(HANDLE)2;return STATUS_SUCCESS;
}
static void PsTerminateSystemThread(NTSTATUS status)
{CHECK(system_worker && status==STATUS_SUCCESS);++termination_calls;}
static NTSTATUS ZwWaitForSingleObject(HANDLE thread,BOOLEAN alertable,PLARGE_INTEGER timeout)
{
    ++wait_calls;CHECK(thread==(HANDLE)2 && thread_live && !alertable && !timeout);
    // This model always completes the worker before returning, even when the
    // wait status is injected. It tests status propagation and handle cleanup,
    // not the stack lifetime of a worker still running after a failed OS wait.
    CHECK(pending_worker!=NULL);system_worker=TRUE;pending_worker(pending_context);system_worker=FALSE;
    pending_worker=NULL;pending_context=NULL;return wait_status;
}
static NTSTATUS ZwClose(HANDLE thread)
{++close_calls;CHECK(thread==(HANDLE)2 && thread_live && !pending_worker);thread_live=FALSE;return STATUS_SUCCESS;}
NTSTATUS DriverStateActivateDevice(PDEVICE_OBJECT device);
/* ACTUAL_DEVICE */

static BOOLEAN AdapterOwnerIs(PDEVICE_OBJECT device){CHECK(device==(PDEVICE_OBJECT)1);return (BOOLEAN)owned;}
static void BoardMemoryIdentityClear(BC250_DEVICE *device){CHECK(device!=NULL);++guard_calls;}
static void GuardStage(ULONG stage){CHECK(stage>=StageStartEnter && stage<=StageStartGuardPassed);++guard_calls;}
static void GuardLogKeepEpisode(PCWSTR episode){CHECK(!wcscmp(episode,L"start"));++guard_calls;}
static void GuardRecordStartAttempt(void){++guard_calls;}
static void GuardLog(const char *message,...){CHECK(message!=NULL);++guard_calls;}
static void GuardStartStep(PCWSTR step){CHECK(step!=NULL);++guard_calls;}
static void GuardRecordStartFailed(NTSTATUS status){CHECK(!NT_SUCCESS(status));++guard_calls;}
static void GuardRecordStartRefused(NTSTATUS status){CHECK(!NT_SUCCESS(status));++guard_calls;}
static BOOLEAN WddmFullTableSelected(void){return TRUE;}
static NTSTATUS GuardCheckAndCountStart(BOOLEAN full)
{CHECK(full);++budget_calls;budget_order=++order;return budget_status;}
static NTSTATUS HardwareBoundary(void){++boundary_calls;return STATUS_SUCCESS;}
/* ACTUAL_START_PREFIX */

static void Reset(const char *name)
{
    CHECK(!thread_live && !pending_worker && !pending_context && !system_worker);test_case=name;
    receipt_calls=property_calls=thread_calls=wait_calls=close_calls=activation_calls=termination_calls=0;
    guard_calls=budget_calls=boundary_calls=order=activation_order=budget_order=0;
    irql=0;owned=TRUE;receipt_status=property_status=thread_status=wait_status=activation_status=budget_status=0;
    memset(property_text,0,sizeof(property_text));wcscpy_s(property_text,513,FixtureId);
    property_bytes=sizeof(FixtureId);property_type=DEVPROP_TYPE_STRING;
    memset(&fixture_receipt,0,sizeof(fixture_receipt));fixture_receipt.origin=AMDGPU_TRANSFER_LEGACY;
    CHECK(AmdgpuStateDeviceIdentity(FixtureId,(unsigned)(sizeof(FixtureId)/sizeof(WCHAR)-1),fixture_receipt.device_hash)==AMDGPU_STATE_OK);
    memset(activated_hash,0xa5,sizeof(activated_hash));
}
static void NoActivation(void)
{CHECK(!thread_calls && !wait_calls && !close_calls && !activation_calls && !termination_calls && !thread_live);}
static void RejectedByBoth(NTSTATUS expected)
{
    CHECK(DriverStateValidateDevice((PDEVICE_OBJECT)1)==expected);NoActivation();
    CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==expected);NoActivation();
}
static NTSTATUS Start(BC250_DEVICE *device)
{
    ULONG sources=99,children=99;DXGK_START_INFO info={0};DXGKRNL_INTERFACE callbacks={0};NTSTATUS status;
    status=Bc250StartDevice(device,&info,&callbacks,&sources,&children);
    CHECK(sources==0 && children==0);return status;
}
int main(void)
{
    unsigned i;BC250_DEVICE device;BYTE zero[32]={0};
    Reset("valid validation stays read-only");
    CHECK(DriverStateValidateDevice((PDEVICE_OBJECT)1)==0);CHECK(receipt_calls==1 && property_calls==1);NoActivation();
    CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==0);
    CHECK(receipt_calls==2 && property_calls==2 && thread_calls==1 && wait_calls==1 && close_calls==1);
    CHECK(activation_calls==1 && termination_calls==1 && !memcmp(activated_hash,fixture_receipt.device_hash,32));
    Reset("identity changes after validation");CHECK(DriverStateValidateDevice((PDEVICE_OBJECT)1)==0);
    property_text[4]=L'X';CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==STATUS_DEVICE_CONFIGURATION_ERROR);NoActivation();
    Reset("fixture_receipt failure");receipt_status=STATUS_ACCESS_DENIED;RejectedByBoth(receipt_status);CHECK(!property_calls);
    Reset("property failure");property_status=STATUS_ACCESS_DENIED;RejectedByBoth(property_status);
    Reset("wrong identity");fixture_receipt.device_hash[0]^=1;RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);
    Reset("wrong property type");property_type=DEVPROP_TYPE_UINT32;RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);
    for(i=0;i<3;++i){Reset("invalid property byte count");property_bytes=i==0?2:i==1?5:1028;RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);}
    Reset("unterminated property");property_text[property_bytes/2-1]=L'X';RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);
    Reset("embedded nul identity");property_text[5]=0;RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);
    Reset("non-ASCII identity");property_text[5]=0x100;RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);
    Reset("invalid IRQL");irql=1;RejectedByBoth(STATUS_INVALID_DEVICE_STATE);CHECK(!receipt_calls);
    Reset("null PDO");CHECK(DriverStateValidateDevice(NULL)==STATUS_INVALID_DEVICE_STATE);
    CHECK(DriverStateActivateDevice(NULL)==STATUS_INVALID_DEVICE_STATE);CHECK(!receipt_calls);NoActivation();
    Reset("zero hash refuses legacy");memset(fixture_receipt.device_hash,0,32);RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);CHECK(!property_calls);
    Reset("zero hash refuses proven board");memset(fixture_receipt.device_hash,0,32);fixture_receipt.origin=AMDGPU_TRANSFER_FRESH;
    fixture_receipt.board_kind=AMDGPU_BOARD_FRESH;RejectedByBoth(STATUS_DEVICE_CONFIGURATION_ERROR);
    Reset("unproven fresh identity");memset(fixture_receipt.device_hash,0,32);fixture_receipt.origin=AMDGPU_TRANSFER_FRESH;
    CHECK(DriverStateValidateDevice((PDEVICE_OBJECT)1)==0);NoActivation();
    CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==0);CHECK(!property_calls && !memcmp(activated_hash,zero,32));
    Reset("activation failure propagated");activation_status=STATUS_REGISTRY_IO_FAILED;
    CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==activation_status);
    CHECK(thread_calls==1 && wait_calls==1 && close_calls==1 && activation_calls==1 && termination_calls==1);
    Reset("thread creation failure");thread_status=STATUS_INSUFFICIENT_RESOURCES;
    CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==thread_status);CHECK(thread_calls==1 && !wait_calls && !close_calls && !activation_calls);
    Reset("wait failure after completed worker propagated");wait_status=STATUS_UNSUCCESSFUL;
    CHECK(DriverStateActivateDevice((PDEVICE_OBJECT)1)==wait_status);CHECK(wait_calls==1 && close_calls==1 && !thread_live);

    memset(&device,0,sizeof(device));device.PhysicalDeviceObject=(PDEVICE_OBJECT)1;
    Reset("foreign owner before activation");owned=FALSE;CHECK(Start(&device)==STATUS_DEVICE_BUSY);
    CHECK(!receipt_calls && !guard_calls && !budget_calls && !boundary_calls);NoActivation();
    Reset("null context before activation");CHECK(Start(NULL)==STATUS_DEVICE_BUSY);
    CHECK(!receipt_calls && !guard_calls && !budget_calls && !boundary_calls);NoActivation();
    Reset("Start activation before guard");activation_status=STATUS_REGISTRY_IO_FAILED;
    CHECK(Start(&device)==STATUS_REGISTRY_IO_FAILED);CHECK(activation_calls==1 && !guard_calls && !budget_calls && !boundary_calls);
    Reset("Start revalidates PDO");property_text[4]=L'X';CHECK(Start(&device)==STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!guard_calls && !budget_calls && !boundary_calls);NoActivation();
    Reset("Start success ordering");CHECK(Start(&device)==0);
    CHECK(activation_calls==1 && budget_calls==1 && boundary_calls==1 && activation_order<budget_order);
    Reset("start budget refusal");budget_status=STATUS_ACCESS_DENIED;CHECK(Start(&device)==budget_status);
    CHECK(activation_calls==1 && budget_calls==1 && !boundary_calls && activation_order<budget_order);
    Reset("prior hardware stop unconfirmed");device.GpuStopUnconfirmed=TRUE;CHECK(Start(&device)==STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(activation_calls==1 && !budget_calls && !boundary_calls);device.GpuStopUnconfirmed=FALSE;
    CHECK(!thread_live && !system_worker && !pending_worker && !pending_context);
    printf("%u device lifecycle checks, %u failures\n",checks,failures);return failures?1:0;
}
