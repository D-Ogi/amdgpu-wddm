// Roots are obtained once in DriverEntry's SYSTEM context. Later PASSIVE callers
// use relative opens under rundown protection; no registry-path reconstruction.
#include <ntifs.h>
#include <ntstrsafe.h>
#include "state_kernel.h"
#include "state_handoff.h"
#include "state_transaction.h"
#include "../state/transfer.h"
#define STATE_TAG 'tSMA'
static HANDLE Defaults,Root,Generation,Data,Volatile,Parameters,Shared;
static EX_RUNDOWN_REF Readers;
static KMUTEX StateLock;
static BOOLEAN Initialized,HasActive;
static volatile LONG Prepared,Ready;
static volatile LONG InvalidGroups;
static AMDGPU_TRANSFER_HEADER AcceptedTransfer;
static PVOID Candidate;
static ULONG CandidateBytes,EntryFullWddm;
static char ProductVersion[32];

static NTSTATUS Open(HANDLE parent,PCWSTR child,ACCESS_MASK access,BOOLEAN create,BOOLEAN isvolatile,PHANDLE out)
{
    UNICODE_STRING name; OBJECT_ATTRIBUTES oa;
    *out=NULL; RtlInitUnicodeString(&name,child);
    InitializeObjectAttributes(&oa,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,parent,NULL);
    return create?ZwCreateKey(out,access,&oa,0,NULL,isvolatile?REG_OPTION_VOLATILE:REG_OPTION_NON_VOLATILE,NULL):
                  ZwOpenKey(out,access,&oa);
}
static NTSTATUS Read(HANDLE key,PCWSTR name,ULONG *type,PVOID data,ULONG capacity,ULONG *bytes)
{
    UNICODE_STRING value; PKEY_VALUE_PARTIAL_INFORMATION info; NTSTATUS status; ULONG needed=0;
    ULONG size;
    *bytes=0; *type=0;
    if(capacity>262144 || !data) return STATUS_INVALID_PARAMETER;
    size=FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+capacity;
    info=ExAllocatePool2(POOL_FLAG_PAGED,size,STATE_TAG);
    if(!info) return STATUS_INSUFFICIENT_RESOURCES;
    RtlInitUnicodeString(&value,name);
    status=ZwQueryValueKey(key,&value,KeyValuePartialInformation,info,size,&needed);
    if(NT_SUCCESS(status)) {
        if(needed<(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data) || info->DataLength>capacity ||
           info->DataLength>needed-(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)) status=STATUS_DATA_ERROR;
        else { *type=info->Type; *bytes=info->DataLength; RtlCopyMemory(data,info->Data,*bytes); }
    }
    ExFreePoolWithTag(info,STATE_TAG);
    return status;
}
static NTSTATUS Number(HANDLE key,PCWSTR name,ULONG *value)
{
    ULONG bytes,type; NTSTATUS status=Read(key,name,&type,value,4,&bytes);
    return NT_SUCCESS(status) && (type!=REG_DWORD || bytes!=4)?STATUS_OBJECT_TYPE_MISMATCH:status;
}
static NTSTATUS String(HANDLE key,PCWSTR name,PWCHAR value,ULONG chars)
{
    ULONG bytes,type,i; NTSTATUS status=Read(key,name,&type,value,chars*sizeof(WCHAR),&bytes);
    if(!NT_SUCCESS(status)) return status;
    if(type!=REG_SZ || bytes<2 || bytes%2 || value[bytes/2-1]) return STATUS_OBJECT_TYPE_MISMATCH;
    for(i=0;i+1<bytes/2;i++) if(!value[i]) return STATUS_OBJECT_TYPE_MISMATCH;
    return STATUS_SUCCESS;
}
static BOOLEAN Narrow(PCWSTR source,char *out,ULONG capacity)
{
    ULONG i;
    if(!source) return FALSE;
    for(i=0;i<capacity;i++) { if(source[i]>127) return FALSE; out[i]=(char)source[i]; if(!source[i]) return TRUE; }
    return FALSE;
}
static BOOLEAN Wide(const char *source,PWCHAR out,ULONG capacity)
{
    ULONG i;
    for(i=0;i<capacity;i++) { out[i]=(UCHAR)source[i]; if(!source[i]) return TRUE; }
    return FALSE;
}
static ULONG Status(NTSTATUS status)
{
    if(NT_SUCCESS(status)) return AMDGPU_STATE_OK;
    if(status==STATUS_OBJECT_NAME_NOT_FOUND || status==STATUS_OBJECT_PATH_NOT_FOUND) return AMDGPU_STATE_ABSENT;
    if(status==STATUS_ACCESS_DENIED || status==STATUS_PRIVILEGE_NOT_HELD) return AMDGPU_STATE_ACCESS;
    if(status==STATUS_BUFFER_TOO_SMALL || status==STATUS_BUFFER_OVERFLOW) return AMDGPU_STATE_BUFFER;
    return AMDGPU_STATE_IO;
}
static NTSTATUS Native(ULONG status,ULONG error)
{
    if(status==AMDGPU_STATE_OK) return STATUS_SUCCESS;
    if(status==AMDGPU_STATE_ABSENT) return STATUS_OBJECT_NAME_NOT_FOUND;
    if(status==AMDGPU_STATE_ACCESS) return STATUS_ACCESS_DENIED;
    if(status==AMDGPU_STATE_BUFFER) return STATUS_BUFFER_TOO_SMALL;
    if(status==AMDGPU_STATE_IO && !NT_SUCCESS((NTSTATUS)error)) return (NTSTATUS)error;
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
static unsigned int Fetch(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    HANDLE root,key=NULL; WCHAR child[128],name[128]; NTSTATUS status;
    ULONG type=0,bytes=0;
    UNREFERENCED_PARAMETER(context);
    if(source==AMDGPU_STATE_OVERRIDE && Candidate && spec->kind!=AMDGPU_STATE_VOLATILE) {
        const void *payload=NULL;ULONG index,result;
        for(index=0;index<AmdgpuStateCount();index++)if(AmdgpuStateAt(index)==spec)break;
        result=AmdgpuTransferFind(Candidate,CandidateBytes,index,value,&payload);
        if(result==AMDGPU_STATE_ABSENT && AcceptedTransfer.origin==AMDGPU_TRANSFER_LEGACY && spec->legacy_off) {
            if(capacity<4)return AMDGPU_STATE_BUFFER;
            RtlZeroMemory(data,4);value->type=REG_DWORD;value->bytes=4;return AMDGPU_STATE_OK;
        }
        if(result!=AMDGPU_STATE_OK)return result;
        if(value->bytes>capacity)return AMDGPU_STATE_BUFFER;
        RtlCopyMemory(data,payload,value->bytes);return AMDGPU_STATE_OK;
    }
    if(!Wide(spec->path,child,RTL_NUMBER_OF(child)) || !Wide(spec->name,name,RTL_NUMBER_OF(name)))
        return AMDGPU_STATE_INVALID_ARGUMENT;
    root=source==AMDGPU_STATE_FACTORY?Defaults:(spec->kind==AMDGPU_STATE_VOLATILE?Volatile:Data);
    if(!root) return AMDGPU_STATE_INCOMPLETE;
    status=Open(root,child,KEY_QUERY_VALUE,FALSE,FALSE,&key);
    if(NT_SUCCESS(status)) {
        status=Read(key,name,&type,data,capacity,&bytes);
        ZwClose(key);
    }
    value->os_error=(ULONG)status; value->type=type; value->bytes=bytes;
    return Status(status);
}
static NTSTATUS QueryUnlocked(const AMDGPU_STATE_SPEC *spec,ULONG mode,AMDGPU_STATE_VALUE *value,PVOID data,ULONG capacity)
{
    ULONG result=AmdgpuStateQuery(spec,mode,Fetch,NULL,value,data,capacity);
    return Native(result,value->os_error);
}
BOOLEAN DriverStateReady(void) { return InterlockedCompareExchange(&Ready,0,0)!=0; }
BOOLEAN DriverStateGroupEnabled(ULONG group)
{ return group<=AMDGPU_STATE_GROUP_INTEROP && !(InterlockedCompareExchange(&InvalidGroups,0,0)&(1u<<group)); }

static unsigned int Empty(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(source);UNREFERENCED_PARAMETER(spec);
    UNREFERENCED_PARAMETER(value);UNREFERENCED_PARAMETER(data);UNREFERENCED_PARAMETER(capacity);
    return AMDGPU_STATE_ABSENT;
}
static unsigned int FinalRaw(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    HANDLE key=NULL;WCHAR path[128],name[128];ULONG type=0,bytes=0;NTSTATUS status;
    UNREFERENCED_PARAMETER(source);
    if(!Wide(spec->path,path,128) || !Wide(spec->name,name,128))return AMDGPU_STATE_SCHEMA;
    status=Open((HANDLE)context,path,KEY_READ,FALSE,FALSE,&key);
    if(NT_SUCCESS(status)) {
        status=Read(key,name,&type,data,capacity>262144?262144:capacity,&bytes);ZwClose(key);
    }
    value->type=type;value->bytes=bytes;value->os_error=status;return Status(status);
}
static void Hex(const unsigned char id[16],WCHAR out[33])
{
    static const WCHAR digits[]=L"0123456789abcdef";ULONG i;
    for(i=0;i<16;i++){out[2*i]=digits[id[i]>>4];out[2*i+1]=digits[id[i]&15];}out[32]=0;
}
static void CloseActive(void)
{
    if(Data)ZwClose(Data);Data=NULL;
    if(Generation)ZwClose(Generation);Generation=NULL;
    if(Root)ZwClose(Root);Root=NULL;
}
static NTSTATUS LoadActive(BOOLEAN allowPriorVersion)
{
    WCHAR transaction[33],expected[65],catalog[65],path[80],receiptId[33];unsigned char boot[16];
    ULONG schema=0,i,type=0,count=0;HANDLE metadata=NULL;NTSTATUS status;
    CloseActive();HasActive=FALSE;
    status=Open(Shared,L"StateV2",KEY_READ|KEY_WRITE,FALSE,FALSE,&Root);
    if(!NT_SUCCESS(status))return status;
    status=String(Root,L"Active",transaction,33);if(!NT_SUCCESS(status))return status;
    HasActive=TRUE;
    for(i=0;i<32;i++)if(!((transaction[i]>=L'0' && transaction[i]<=L'9') ||
                         (transaction[i]>=L'a' && transaction[i]<=L'f')))return STATUS_DATA_ERROR;
    status=RtlStringCchPrintfW(path,80,L"Generations\\%ws",transaction);if(!NT_SUCCESS(status))return status;
    status=Open(Root,path,KEY_READ|KEY_WRITE,FALSE,FALSE,&Generation);if(!NT_SUCCESS(status))return status;
    status=Number(Generation,L"Ready",&schema);
    if(!NT_SUCCESS(status) || schema!=AmdgpuStateSchema())return STATUS_DEVICE_CONFIGURATION_ERROR;
    status=String(Generation,L"CatalogId",catalog,65);if(!NT_SUCCESS(status))return status;
    Wide(AmdgpuStateCatalogId(),expected,65);if(wcscmp(catalog,expected))return STATUS_REVISION_MISMATCH;
    status=Open(Generation,L"Data",KEY_READ|KEY_WRITE,FALSE,FALSE,&Data);if(!NT_SUCCESS(status))return status;
    status=Open(Generation,L"Migration",KEY_READ,FALSE,FALSE,&metadata);
    if(NT_SUCCESS(status)) {
        status=Read(metadata,L"Transfer",&type,&AcceptedTransfer,sizeof(AcceptedTransfer),&count);
        ZwClose(metadata);
        if(NT_SUCCESS(status) && (type!=REG_BINARY || count!=sizeof(AcceptedTransfer)))status=STATUS_DATA_ERROR;
    }
    if(!NT_SUCCESS(status))return status;
    Hex(AcceptedTransfer.transaction,receiptId);
    if(wcscmp(transaction,receiptId) || AcceptedTransfer.magic!=AMDGPU_TRANSFER_MAGIC ||
       AcceptedTransfer.version!=AMDGPU_TRANSFER_VERSION)return STATUS_REVISION_MISMATCH;
    AmdgpuStateBootToken(SharedUserData->BootId,boot);
    status=Native(AmdgpuTransferBind(&AcceptedTransfer,AcceptedTransfer.device_hash,
                                    AcceptedTransfer.transaction,boot,AcceptedTransfer.destination_version),0);
    if(NT_SUCCESS(status) && !allowPriorVersion && strcmp(AcceptedTransfer.destination_version,ProductVersion))
        status=STATUS_REVISION_MISMATCH;
    return status;
}
// An absent source is permitted only for a new/legacy/archive import. CURRENT
// must retain the same source pointer and its installer-owned freeze throughout.
static NTSTATUS Source(HANDLE *out)
{
    HANDLE root=NULL;WCHAR active[33],expected[33],owner[33],path[120];ULONG frozen=0;NTSTATUS status;
    *out=NULL;
    status=Open(Shared,L"StateV2",KEY_READ,FALSE,FALSE,&root);
    if(status==STATUS_OBJECT_NAME_NOT_FOUND && AcceptedTransfer.origin!=AMDGPU_TRANSFER_CURRENT)status=STATUS_SUCCESS;
    else if(NT_SUCCESS(status)) {
        status=String(root,L"Active",active,33);
        if(AcceptedTransfer.origin==AMDGPU_TRANSFER_CURRENT) {
            Hex(AcceptedTransfer.source_generation,expected);
            if(NT_SUCCESS(status) && wcscmp(active,expected))status=STATUS_REVISION_MISMATCH;
            if(NT_SUCCESS(status))status=Number(root,L"Frozen",&frozen);
            if(NT_SUCCESS(status) && frozen!=1)status=STATUS_DEVICE_CONFIGURATION_ERROR;
            if(NT_SUCCESS(status))status=String(root,L"FrozenBy",owner,33);
            Hex(AcceptedTransfer.transaction,expected);
            if(NT_SUCCESS(status) && wcscmp(owner,expected))status=STATUS_REVISION_MISMATCH;
        } else if(status==STATUS_OBJECT_NAME_NOT_FOUND)status=STATUS_SUCCESS;
        else if(NT_SUCCESS(status))status=STATUS_REVISION_MISMATCH;
    }
    if(root)ZwClose(root);
    if(!NT_SUCCESS(status))return status;
    if(AcceptedTransfer.origin==AMDGPU_TRANSFER_LEGACY)return Open(Parameters,L"",KEY_READ,FALSE,FALSE,out);
    if(AcceptedTransfer.origin!=AMDGPU_TRANSFER_CURRENT)return STATUS_SUCCESS;
    Hex(AcceptedTransfer.source_generation,expected);
    status=RtlStringCchPrintfW(path,120,L"StateV2\\Generations\\%ws\\Data",expected);
    return NT_SUCCESS(status)?Open(Shared,path,KEY_READ,FALSE,FALSE,out):status;
}
static NTSTATUS RefreshCandidate(void)
{
    HANDLE source=NULL;PVOID merged=NULL;unsigned int bytes=0,conflict=~0u;NTSTATUS status=Source(&source);
    if(!NT_SUCCESS(status) || !source)return status;
    merged=ExAllocatePool2(POOL_FLAG_PAGED,AMDGPU_TRANSFER_MAX_BYTES,STATE_TAG);
    if(!merged)status=STATUS_INSUFFICIENT_RESOURCES;
    else status=Native(AmdgpuTransferMergeFinal(Candidate,CandidateBytes,FinalRaw,source,merged,
                                               AMDGPU_TRANSFER_MAX_BYTES,&bytes,&conflict),0);
    ZwClose(source);
    if(NT_SUCCESS(status)) {
        ExFreePoolWithTag(Candidate,STATE_TAG);Candidate=merged;CandidateBytes=bytes;merged=NULL;
        RtlCopyMemory(&AcceptedTransfer,Candidate,sizeof(AcceptedTransfer));
    }
    if(merged)ExFreePoolWithTag(merged,STATE_TAG);
    return status;
}
static NTSTATUS Factory(HANDLE parameters,HANDLE shared,PCSTR version)
{
    KEY_FULL_INFORMATION info;ULONG needed=0,type=0,bytes=0;HANDLE root=NULL;
    NTSTATUS status;AMDGPU_TRANSFER_HEADER h;PVOID data=NULL;unsigned int used=0;
    UNICODE_STRING name;UUID id;
    RtlZeroMemory(&h,sizeof(h));
    status=Open(shared,L"StateV2",KEY_READ|KEY_WRITE,TRUE,FALSE,&root);if(!NT_SUCCESS(status))return status;
    // An accepted state is kept. DriverStateInit checks its complete generation
    // and catalog below; a damaged or obsolete generation is never factory reset.
    status=Read(root,L"Active",&type,&id,sizeof(id),&bytes);
    if(status!=STATUS_OBJECT_NAME_NOT_FOUND) {
        ZwClose(root);return NT_SUCCESS(status) || status==STATUS_BUFFER_TOO_SMALL || status==STATUS_BUFFER_OVERFLOW?
                            STATUS_SUCCESS:status;
    }
    status=ZwQueryKey(parameters,KeyFullInformation,&info,sizeof(info),&needed);
    if(!NT_SUCCESS(status))goto done;
    if(info.Values || info.SubKeys!=1) {status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
    // The exact child was opened/validated already. Any other retained legacy
    // state requires a protected installer snapshot, never guessed defaults.
    status=Read(root,L"FactoryTransfer",&type,&h,sizeof(h),&bytes);
    if(status==STATUS_OBJECT_NAME_NOT_FOUND) {
        status=ExUuidCreate(&id);if(!NT_SUCCESS(status))goto done;
        h.origin=AMDGPU_TRANSFER_FRESH;h.board_kind=AMDGPU_BOARD_UNPROVEN;
        RtlCopyMemory(h.transaction,&id,16);RtlCopyMemory(h.catalog,AmdgpuStateCatalogId(),64);
        AmdgpuStateBootToken(SharedUserData->BootId,h.source_boot);
        status=RtlStringCchCopyA(h.destination_version,32,version);if(!NT_SUCCESS(status))goto done;
        RtlInitUnicodeString(&name,L"FactoryTransfer");
        status=ZwSetValueKey(root,&name,0,REG_BINARY,&h,sizeof(h));
        if(NT_SUCCESS(status))status=ZwFlushKey(root);
    } else if(NT_SUCCESS(status) && (type!=REG_BINARY || bytes!=sizeof(h) ||
               h.origin!=AMDGPU_TRANSFER_FRESH || h.board_kind!=AMDGPU_BOARD_UNPROVEN ||
               memcmp(h.destination_version,version,strlen(version)+1)))status=STATUS_DATA_ERROR;
    if(!NT_SUCCESS(status))goto done;
    data=ExAllocatePool2(POOL_FLAG_PAGED,AMDGPU_TRANSFER_MAX_BYTES,STATE_TAG);
    if(!data){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    status=Native(AmdgpuTransferCapture(&h,Empty,NULL,data,AMDGPU_TRANSFER_MAX_BYTES,&used),0);
    if(NT_SUCCESS(status)) {
        Candidate=data;CandidateBytes=used;data=NULL;
        RtlCopyMemory(&AcceptedTransfer,Candidate,sizeof(AcceptedTransfer));
    }
done:
    if(data)ExFreePoolWithTag(data,STATE_TAG);
    ZwClose(root);return status;
}
static NTSTATUS Bootstrap(HANDLE parameters,HANDLE shared)
{
    PVOID captured=NULL;ULONG bytes=0;
    AMDGPU_TRANSFER_HEADER h;WCHAR package[32];unsigned char boot[16];NTSTATUS status;
    status=String(Defaults,L"PackageProductVersion",package,32);if(!NT_SUCCESS(status))return status;
    if(!Narrow(package,ProductVersion,32))return STATUS_DATA_ERROR;
    status=DriverStateReadHandoff(&captured,&bytes);
    if(status==STATUS_OBJECT_NAME_NOT_FOUND || status==STATUS_OBJECT_PATH_NOT_FOUND) {
        status=Factory(parameters,shared,ProductVersion);
        return NT_SUCCESS(status) && !Candidate?LoadActive(FALSE):status;
    }
    if(!NT_SUCCESS(status))return status;
    status=Native(AmdgpuTransferCheck(captured,bytes,&h),0);if(!NT_SUCCESS(status))goto done;
    if(strcmp(h.destination_version,ProductVersion)){status=STATUS_REVISION_MISMATCH;goto done;}
    AmdgpuStateBootToken(SharedUserData->BootId,boot);
    if(h.origin==AMDGPU_TRANSFER_LEGACY && RtlCompareMemory(boot,h.source_boot,16)==16) {
        status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;
    }
    // Once activated, a leftover handoff is merely an idempotence receipt. Read
    // current choices without re-importing its early snapshot or needing source.
    status=LoadActive(h.origin==AMDGPU_TRANSFER_CURRENT);
    if(NT_SUCCESS(status) && RtlCompareMemory(h.transaction,AcceptedTransfer.transaction,16)==16) {
        if(RtlCompareMemory(h.device_hash,AcceptedTransfer.device_hash,32)!=32)status=STATUS_REVISION_MISMATCH;
        goto done;
    }
    // A CURRENT source may belong to the previous product version; Source checks
    // its pointer/freeze. Other read errors never become a fresh initialization.
    if(!NT_SUCCESS(status) && (HasActive || (status!=STATUS_OBJECT_NAME_NOT_FOUND && status!=STATUS_OBJECT_PATH_NOT_FOUND)))goto done;
    CloseActive();AcceptedTransfer=h;
    Candidate=ExAllocatePool2(POOL_FLAG_PAGED,bytes,STATE_TAG);
    if(!Candidate){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    RtlCopyMemory(Candidate,captured,bytes);CandidateBytes=bytes;
    status=RefreshCandidate();
done:
    DriverStateFreeHandoff(captured);return status;
}

static NTSTATUS ValidateInputs(void)
{
    ULONG i,capacity=4;PVOID data;NTSTATUS status=STATUS_SUCCESS;
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);
        if(spec->kind!=AMDGPU_STATE_VOLATILE && (spec->kind==AMDGPU_STATE_SETTING || spec->group!=AMDGPU_STATE_GROUP_NONE)) {
            if(spec->max_bytes>262144)return STATUS_INVALID_BUFFER_SIZE;
            if(spec->max_bytes>capacity)capacity=spec->max_bytes;
        }
    }
    data=ExAllocatePool2(POOL_FLAG_PAGED,capacity,STATE_TAG);if(!data)return STATUS_INSUFFICIENT_RESOURCES;
    InvalidGroups=0;
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *spec=AmdgpuStateAt(i);AMDGPU_STATE_VALUE value;
        if(spec->kind==AMDGPU_STATE_VOLATILE || (spec->kind!=AMDGPU_STATE_SETTING && spec->group==AMDGPU_STATE_GROUP_NONE))continue;
        status=QueryUnlocked(spec,spec->kind==AMDGPU_STATE_SETTING?AMDGPU_STATE_QUERY_EFFECTIVE:AMDGPU_STATE_READ_RECORD,
                             &value,data,capacity);
        if(status==STATUS_OBJECT_NAME_NOT_FOUND)continue;
        if(!NT_SUCCESS(status)) {
            if(spec->group==AMDGPU_STATE_GROUP_NONE)break;
            InvalidGroups|=1u<<spec->group;
        }
        status=STATUS_SUCCESS;
    }
    ExFreePoolWithTag(data,STATE_TAG);return status==STATUS_OBJECT_NAME_NOT_FOUND?STATUS_SUCCESS:status;
}

NTSTATUS DriverStateInit(PDRIVER_OBJECT driver)
{
    WCHAR catalog[65],expected[65];ULONG schema=0;AMDGPU_STATE_VALUE value;NTSTATUS status;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !PsIsSystemThread(PsGetCurrentThread())) return STATUS_INVALID_DEVICE_STATE;
    if(Initialized) return STATUS_INVALID_DEVICE_STATE;
    ExInitializeRundownProtection(&Readers);KeInitializeMutex(&StateLock,0);Initialized=TRUE;
    InvalidGroups=0;
    status=IoOpenDriverRegistryKey(driver,DriverRegKeyParameters,KEY_READ,0,&Parameters);
    if(!NT_SUCCESS(status)) goto done;
    status=Open(Parameters,L"DefaultsV2",KEY_READ,FALSE,FALSE,&Defaults);
    if(!NT_SUCCESS(status)) goto done;
    status=Number(Defaults,L"Schema",&schema);
    if(!NT_SUCCESS(status) || schema!=AmdgpuStateSchema()) { status=STATUS_REVISION_MISMATCH; goto done; }
    Wide(AmdgpuStateCatalogId(),expected,65);
    status=String(Defaults,L"CatalogId",catalog,65);
    if(!NT_SUCCESS(status)) goto done;
    if(wcscmp(catalog,expected)) { status=STATUS_REVISION_MISMATCH; goto done; }
    status=IoOpenDriverRegistryKey(driver,DriverRegKeySharedPersistentState,KEY_READ|KEY_WRITE,0,&Shared);
    if(!NT_SUCCESS(status)) goto done;
    status=Bootstrap(Parameters,Shared);
    if(!NT_SUCCESS(status)) goto done;
    status=ValidateInputs();if(!NT_SUCCESS(status))goto done;
    status=QueryUnlocked(AmdgpuStateFind("","EnableFullWddm"),AMDGPU_STATE_QUERY_EFFECTIVE,&value,&EntryFullWddm,4);
    if(!NT_SUCCESS(status))goto done;
    // DriverEntry may read its early gate, but only AddDevice can publish/mutate.
    InterlockedExchange(&Prepared,1);status=STATUS_SUCCESS;
done:
    if(!NT_SUCCESS(status)) DriverStateClose();
    return status;
}
void DriverStateClose(void)
{
    if(!Initialized) return;
    InterlockedExchange(&Ready,0);
    InterlockedExchange(&Prepared,0);
    ExWaitForRundownProtectionRelease(&Readers);
    if(Volatile) ZwClose(Volatile); Volatile=NULL;
    if(Data) ZwClose(Data); Data=NULL;
    if(Generation) ZwClose(Generation); Generation=NULL;
    if(Root) ZwClose(Root); Root=NULL;
    if(Defaults) ZwClose(Defaults); Defaults=NULL;
    if(Parameters)ZwClose(Parameters);Parameters=NULL;
    if(Shared)ZwClose(Shared);Shared=NULL;
    if(Candidate)ExFreePoolWithTag(Candidate,STATE_TAG);Candidate=NULL;CandidateBytes=0;
    Initialized=FALSE;
    RtlZeroMemory(&AcceptedTransfer,sizeof(AcceptedTransfer));
}
static BOOLEAN Acquire(void)
{
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !InterlockedCompareExchange(&Prepared,0,0) ||
       !ExAcquireRundownProtection(&Readers))return FALSE;
    (void)KeWaitForSingleObject(&StateLock,Executive,KernelMode,FALSE,NULL);
    return TRUE;
}
static void Release(void)
{ KeReleaseMutex(&StateLock,FALSE);ExReleaseRundownProtection(&Readers); }
NTSTATUS DriverStateReceipt(AMDGPU_TRANSFER_HEADER *receipt)
{
    if(!receipt)return STATUS_INVALID_PARAMETER;
    if(!Acquire())return STATUS_INVALID_DEVICE_STATE;
    *receipt=AcceptedTransfer;Release();return STATUS_SUCCESS;
}
NTSTATUS DriverStateActivate(const unsigned char deviceHash[32])
{
    unsigned char boot[16];ULONG full=0;AMDGPU_STATE_VALUE value;NTSTATUS status;
    BOOLEAN published=FALSE;
    if(!deviceHash || !PsIsSystemThread(PsGetCurrentThread()))return STATUS_INVALID_DEVICE_STATE;
    if(!Acquire())return STATUS_INVALID_DEVICE_STATE;
    AmdgpuStateBootToken(SharedUserData->BootId,boot);
    status=Native(AmdgpuTransferBind(&AcceptedTransfer,deviceHash,AcceptedTransfer.transaction,boot,ProductVersion),0);
    if(!NT_SUCCESS(status) || DriverStateReady())goto done;
    if(Candidate) {
        HANDLE root=NULL;WCHAR active[33],expected[33];
        status=Open(Shared,L"StateV2",KEY_READ,FALSE,FALSE,&root);
        if(NT_SUCCESS(status)) {
            status=String(root,L"Active",active,33);ZwClose(root);
            Hex(AcceptedTransfer.transaction,expected);
            published=NT_SUCCESS(status) && !wcscmp(active,expected);
        }
        if(!published){status=RefreshCandidate();if(!NT_SUCCESS(status))goto done;}
    }
    status=ValidateInputs();if(!NT_SUCCESS(status))goto done;
    status=QueryUnlocked(AmdgpuStateFind("","EnableFullWddm"),AMDGPU_STATE_QUERY_EFFECTIVE,&value,&full,4);
    if(!NT_SUCCESS(status))goto done;
    if(full!=EntryFullWddm){status=STATUS_REVISION_MISMATCH;goto done;}
    if(Candidate) {
        status=DriverStateCommitTransfer(Shared,Candidate,CandidateBytes);if(!NT_SUCCESS(status))goto done;
        // Keep the candidate through any commit failure, including a lost Active
        // flush. Commit's idempotent path will never replay current user choices.
        ExFreePoolWithTag(Candidate,STATE_TAG);Candidate=NULL;CandidateBytes=0;
        status=LoadActive(FALSE);if(!NT_SUCCESS(status))goto done;
        status=ValidateInputs();if(!NT_SUCCESS(status))goto done;
        status=QueryUnlocked(AmdgpuStateFind("","EnableFullWddm"),AMDGPU_STATE_QUERY_EFFECTIVE,&value,&full,4);
        if(!NT_SUCCESS(status))goto done;
        if(full!=EntryFullWddm){status=STATUS_REVISION_MISMATCH;goto done;}
    } else {
        status=DriverStateFinishAccepted(Shared,&AcceptedTransfer);if(!NT_SUCCESS(status))goto done;
    }
    status=Open(Root,L"Volatile",KEY_READ|KEY_WRITE,TRUE,TRUE,&Volatile);
    if(NT_SUCCESS(status))InterlockedExchange(&Ready,1);
done:
    Release();return status;
}
NTSTATUS DriverStateConsumeBoardSeed(BOOLEAN *first)
{
    HANDLE metadata=NULL;ULONG consumed=0;UNICODE_STRING name;NTSTATUS status;
    if(!first)return STATUS_INVALID_PARAMETER;*first=FALSE;
    if(!PsIsSystemThread(PsGetCurrentThread()) || !Acquire())return STATUS_INVALID_DEVICE_STATE;
    if(!DriverStateReady()){status=STATUS_INVALID_DEVICE_STATE;goto done;}
    // Current/archive receipts inherit an existing board store, never authority
    // to recreate a baseline which disappeared after a previous hardware write.
    if(AcceptedTransfer.origin!=AMDGPU_TRANSFER_FRESH && AcceptedTransfer.origin!=AMDGPU_TRANSFER_LEGACY) {
        status=STATUS_SUCCESS;goto done;
    }
    status=Open(Generation,L"Migration",KEY_READ|KEY_WRITE,FALSE,FALSE,&metadata);
    if(!NT_SUCCESS(status))goto done;
    status=Number(metadata,L"BoardSeedConsumed",&consumed);
    if(NT_SUCCESS(status)) {
        if(consumed!=1)status=STATUS_DATA_ERROR;
    } else if(status==STATUS_OBJECT_NAME_NOT_FOUND) {
        consumed=1;RtlInitUnicodeString(&name,L"BoardSeedConsumed");
        status=ZwSetValueKey(metadata,&name,0,REG_DWORD,&consumed,4);
        if(NT_SUCCESS(status))status=ZwFlushKey(metadata);
        if(NT_SUCCESS(status))*first=TRUE;
    }
done:
    if(metadata)ZwClose(metadata);Release();return status;
}
NTSTATUS DriverStateOpenData(PHANDLE key)
{
    NTSTATUS status; *key=NULL;
    if(!Acquire()) return STATUS_INVALID_DEVICE_STATE;
    status=DriverStateReady()?Open(Data,L"",KEY_READ|KEY_WRITE,FALSE,FALSE,key):STATUS_INVALID_DEVICE_STATE;
    Release();return status;
}
NTSTATUS DriverStateOpenVolatile(PCWSTR child,BOOLEAN create,PHANDLE key)
{
    NTSTATUS status; *key=NULL;
    if(!Acquire()) return STATUS_INVALID_DEVICE_STATE;
    status=DriverStateReady()?Open(Volatile,child,create?KEY_READ|KEY_WRITE:KEY_READ,create,TRUE,key):STATUS_INVALID_DEVICE_STATE;
    Release();return status;
}
NTSTATUS DriverStateQuery(PCWSTR child,PCWSTR name,ULONG mode,AMDGPU_STATE_VALUE *value,PVOID data,ULONG capacity)
{
    char path[128],item[128]; const AMDGPU_STATE_SPEC *spec; NTSTATUS status;
    if(!value || !Narrow(child,path,128) || !Narrow(name,item,128)) return STATUS_INVALID_PARAMETER;
    if(!Acquire()) return STATUS_INVALID_DEVICE_STATE;
    spec=AmdgpuStateFind(path,item);
    status=QueryUnlocked(spec,mode,value,data,capacity);
    Release();return status;
}
NTSTATUS DriverStateNumber(PCWSTR child,PCWSTR name,BOOLEAN effective,ULONG *out)
{
    char path[128],item[128];const AMDGPU_STATE_SPEC *spec;AMDGPU_STATE_VALUE value;ULONG mode;NTSTATUS status;
    if(!out || !Narrow(child,path,128) || !Narrow(name,item,128))return STATUS_INVALID_PARAMETER;
    *out=0;spec=AmdgpuStateFind(path,item);
    if(!spec || spec->type!=AMDGPU_STATE_DWORD)return STATUS_INVALID_PARAMETER;
    if(!Acquire())return STATUS_INVALID_DEVICE_STATE;
    if(spec->kind==AMDGPU_STATE_SETTING && !DriverStateGroupEnabled(spec->group))status=STATUS_DEVICE_CONFIGURATION_ERROR;
    else {
        mode=spec->kind==AMDGPU_STATE_SETTING?
             (effective?AMDGPU_STATE_QUERY_EFFECTIVE:AMDGPU_STATE_QUERY_OVERRIDE):AMDGPU_STATE_READ_RECORD;
        status=QueryUnlocked(spec,mode,&value,out,4);
        if(!NT_SUCCESS(status) && status!=STATUS_OBJECT_NAME_NOT_FOUND && spec->group!=AMDGPU_STATE_GROUP_NONE)
            InterlockedOr(&InvalidGroups,1u<<spec->group);
    }
    if(!NT_SUCCESS(status))*out=0;
    Release();return status;
}
static NTSTATUS Change(PCWSTR child,PCWSTR name,ULONG type,const void *data,ULONG bytes,BOOLEAN clear)
{
    char path[128],item[128]; const AMDGPU_STATE_SPEC *spec; HANDLE key=NULL; UNICODE_STRING value;
    NTSTATUS status; ULONG valid;
    if(!Narrow(child,path,128) || !Narrow(name,item,128)) return STATUS_INVALID_PARAMETER;
    spec=AmdgpuStateFind(path,item);
    if(!spec) return STATUS_INVALID_PARAMETER;
    if(!clear) {
        valid=AmdgpuStateMayWrite(spec,1,1,type,data,bytes);
        if(valid!=AMDGPU_STATE_OK) return Native(valid,0);
    }
    if(!Acquire()) return STATUS_INVALID_DEVICE_STATE;
    if(!DriverStateReady()){Release();return STATUS_INVALID_DEVICE_STATE;}
    status=Open(spec->kind==AMDGPU_STATE_VOLATILE?Volatile:Data,child,KEY_READ|KEY_WRITE,
                TRUE,spec->kind==AMDGPU_STATE_VOLATILE,&key);
    if(NT_SUCCESS(status)) {
        RtlInitUnicodeString(&value,name);
        status=clear?ZwDeleteValueKey(key,&value):ZwSetValueKey(key,&value,0,type,(PVOID)data,bytes);
        if(clear && status==STATUS_OBJECT_NAME_NOT_FOUND) status=STATUS_SUCCESS;
        if(NT_SUCCESS(status) && spec->kind!=AMDGPU_STATE_VOLATILE) status=ZwFlushKey(key);
        ZwClose(key);
    }
    Release();return status;
}
NTSTATUS DriverStateSet(PCWSTR child,PCWSTR name,ULONG type,const void *data,ULONG bytes)
{ return Change(child,name,type,data,bytes,FALSE); }
NTSTATUS DriverStateClear(PCWSTR child,PCWSTR name)
{ return Change(child,name,0,NULL,0,TRUE); }
