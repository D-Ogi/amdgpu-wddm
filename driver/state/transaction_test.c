// The production kernel commit code with a fault-injected registry. Real Windows
// ACL routines validate private children; no installed registry is accessed.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "transfer.h"
typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034L)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001L)
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003eL)
#define STATUS_REVISION_MISMATCH ((NTSTATUS)0xc0000059L)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xc0000182L)
#define STATUS_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xc0000024L)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023L)
#define PASSIVE_LEVEL 0
#define OBJ_KERNEL_HANDLE 0x200
#define OBJ_CASE_INSENSITIVE 0x40
#define KeyValuePartialInformation 2
typedef struct { USHORT Length,MaximumLength;PWSTR Buffer; } UNICODE_STRING,*PUNICODE_STRING;
typedef struct { ULONG Attributes;UNICODE_STRING *Name;HANDLE Root;PSECURITY_DESCRIPTOR Security; } OBJECT_ATTRIBUTES;
typedef struct { ULONG TitleIndex,Type,DataLength;UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION,*PKEY_VALUE_PARTIAL_INFORMATION;
#define InitializeObjectAttributes(a,n,f,r,s) do{(a)->Attributes=(f);(a)->Name=(n);(a)->Root=(r);(a)->Security=(s);}while(0)
static void RtlInitUnicodeString(UNICODE_STRING *out,PCWSTR in) {out->Buffer=(PWSTR)in;out->Length=(USHORT)(2*wcslen(in));out->MaximumLength=out->Length+2;}
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))
#define RtlCompareMemory(a,b,n) (memcmp((a),(b),(n))?0:(n))
#define RtlCreateSecurityDescriptor(s,r) (InitializeSecurityDescriptor(s,r)?0:STATUS_UNSUCCESSFUL)
#define RtlCreateAcl(a,n,r) (InitializeAcl(a,n,r)?0:STATUS_UNSUCCESSFUL)
#define RtlAddAccessAllowedAce(a,r,m,s) (AddAccessAllowedAce(a,r,m,s)?0:STATUS_UNSUCCESSFUL)
#define RtlAddAccessAllowedAceEx(a,r,f,m,s) (AddAccessAllowedAceEx(a,r,f,m,s)?0:STATUS_UNSUCCESSFUL)
#define RtlSetDaclSecurityDescriptor(s,p,a,d) (SetSecurityDescriptorDacl(s,p,a,d)?0:STATUS_UNSUCCESSFUL)
#define RtlSetOwnerSecurityDescriptor(s,o,d) (SetSecurityDescriptorOwner(s,o,d)?0:STATUS_UNSUCCESSFUL)
static unsigned int checks,failures,call,fail_at,irql,system_thread=1,thaw_before_activate,fail_active_flush;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static int Fault(void) {return ++call==fail_at;}
static UCHAR KeGetCurrentIrql(void) {return (UCHAR)irql;}
static PVOID PsGetCurrentThread(void) {return (PVOID)1;}
static BOOLEAN PsIsSystemThread(PVOID thread) {(void)thread;return (BOOLEAN)system_thread;}
typedef struct key {unsigned parent,handles,private_acl;WCHAR name[128];} KEY;
typedef struct val {unsigned key,type,bytes;WCHAR name[128];BYTE data[512];} VAL;
typedef struct hive {KEY keys[64];VAL values[64];unsigned keys_count,values_count;} HIVE;
static HIVE memory,durable;
static unsigned Id(HANDLE h) {return (unsigned)(uintptr_t)h;}
static unsigned FindKey(unsigned parent,PCWSTR name)
{unsigned i;if(!name[0])return parent;for(i=2;i<memory.keys_count;i++)if(memory.keys[i].parent==parent && !wcscmp(memory.keys[i].name,name))return i;return 0;}
static unsigned FindValue(unsigned key,PCWSTR name)
{unsigned i;for(i=1;i<memory.values_count;i++)if(memory.values[i].key==key && !wcscmp(memory.values[i].name,name))return i;return 0;}
static void Security(PSECURITY_DESCRIPTOR sd)
{
    PACL acl;PSID owner;BOOL present,def;ACCESS_ALLOWED_ACE *ace;BYTE system[SECURITY_MAX_SID_SIZE];DWORD bytes=sizeof(system);
    CHECK(GetSecurityDescriptorOwner(sd,&owner,&def));CHECK(CreateWellKnownSid(WinLocalSystemSid,NULL,system,&bytes));
    CHECK(EqualSid(owner,system));CHECK(GetSecurityDescriptorDacl(sd,&present,&acl,&def));
    CHECK(present && acl && (acl->AceCount==2 || acl->AceCount==3) && (((SECURITY_DESCRIPTOR*)sd)->Control&SE_DACL_PROTECTED));
    CHECK(GetAce(acl,1,(void**)&ace));CHECK(ace->Mask==(DWORD)(acl->AceCount==2?KEY_READ:KEY_ALL_ACCESS));
    CHECK(ace->Header.AceFlags==CONTAINER_INHERIT_ACE);
    if(acl->AceCount==3) {CHECK(GetAce(acl,2,(void**)&ace));CHECK(ace->Mask==KEY_READ);}
}
static NTSTATUS ZwCreateKey(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa,ULONG title,PUNICODE_STRING cls,ULONG options,PULONG disposition)
{
    unsigned parent=Id(oa->Root),id;
    (void)access;(void)title;(void)cls;(void)disposition;
    CHECK(parent && parent<memory.keys_count);CHECK(options==REG_OPTION_NON_VOLATILE);
    CHECK(oa->Attributes==(OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE));
    if(Fault())return STATUS_UNSUCCESSFUL;
    id=FindKey(parent,oa->Name->Buffer);
    if(!id){id=memory.keys_count++;CHECK(id<64);if(id>=64)return STATUS_UNSUCCESSFUL;
        memory.keys[id].parent=parent;wcscpy_s(memory.keys[id].name,128,oa->Name->Buffer);}
    if(oa->Security){Security(oa->Security);memory.keys[id].private_acl=1;}
    memory.keys[id].handles++;*out=(HANDLE)(uintptr_t)id;return 0;
}
static NTSTATUS ZwOpenKey(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa)
{
    unsigned id;(void)access;if(Fault())return STATUS_UNSUCCESSFUL;
    id=FindKey(Id(oa->Root),oa->Name->Buffer);if(!id)return STATUS_OBJECT_NAME_NOT_FOUND;
    memory.keys[id].handles++;*out=(HANDLE)(uintptr_t)id;return 0;
}
static NTSTATUS ZwSetSecurityObject(HANDLE key,SECURITY_INFORMATION info,PSECURITY_DESCRIPTOR sd)
{(void)info;if(Fault())return STATUS_UNSUCCESSFUL;Security(sd);memory.keys[Id(key)].private_acl=1;return 0;}
static NTSTATUS ZwSetValueKey(HANDLE key,UNICODE_STRING *name,ULONG title,ULONG type,PVOID data,ULONG bytes)
{
    unsigned i; (void)title;if(Fault())return STATUS_UNSUCCESSFUL;
    i=FindValue(Id(key),name->Buffer);
    if(!i){i=memory.values_count++;CHECK(i<64);if(i>=64)return STATUS_UNSUCCESSFUL;}
    CHECK(bytes<=512);if(bytes>512)return STATUS_UNSUCCESSFUL;
    memory.values[i].key=Id(key);memory.values[i].type=type;memory.values[i].bytes=bytes;
    wcscpy_s(memory.values[i].name,128,name->Buffer);memcpy(memory.values[i].data,data,bytes);return 0;
}
static NTSTATUS ZwQueryValueKey(HANDLE key,UNICODE_STRING *name,ULONG kind,PVOID data,ULONG capacity,PULONG needed)
{
    unsigned i;PKEY_VALUE_PARTIAL_INFORMATION info=data;(void)kind;if(Fault())return STATUS_UNSUCCESSFUL;
    i=FindValue(Id(key),name->Buffer);if(!i)return STATUS_OBJECT_NAME_NOT_FOUND;
    *needed=(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+memory.values[i].bytes;
    if(*needed>capacity)return STATUS_BUFFER_TOO_SMALL;
    info->Type=memory.values[i].type;info->DataLength=memory.values[i].bytes;
    memcpy(info->Data,memory.values[i].data,info->DataLength);return 0;
}
static NTSTATUS ZwDeleteValueKey(HANDLE key,UNICODE_STRING *name)
{unsigned i;if(Fault())return STATUS_UNSUCCESSFUL;i=FindValue(Id(key),name->Buffer);if(!i)return STATUS_OBJECT_NAME_NOT_FOUND;memory.values[i].key=0;return 0;}
static NTSTATUS ZwFlushKey(HANDLE key)
{
    unsigned root=FindKey(1,L"StateV2"),v;
    if(Fault())return STATUS_UNSUCCESSFUL;
    if(thaw_before_activate && FindValue(Id(key),L"Ready")) {
        v=FindValue(root,L"Frozen");CHECK(v!=0);*(DWORD*)memory.values[v].data=0;thaw_before_activate=0;
    }
    v=FindValue(root,L"Active");
    if(fail_active_flush && Id(key)==root && v && ((WCHAR*)memory.values[v].data)[1]==L'2') {
        fail_active_flush=0;return STATUS_UNSUCCESSFUL;
    }
    durable=memory;return 0;
}
static NTSTATUS ZwClose(HANDLE key) {CHECK(memory.keys[Id(key)].handles);memory.keys[Id(key)].handles--;return Fault()?STATUS_UNSUCCESSFUL:0;}
/* ACTUAL_TRANSACTION */
static unsigned int Empty(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *v,void *data,unsigned int capacity)
{(void)context;(void)source;(void)spec;(void)v;(void)data;(void)capacity;return AMDGPU_STATE_ABSENT;}
// The source of a PC whose old driver never had a confirmed start (BD-189): the guard count 2, the start record's
// copy of it, and one setting the tester chose.
static unsigned int Guarded(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,AMDGPU_STATE_VALUE *v,void *data,unsigned int capacity)
{
    DWORD value=!strcmp(spec->name,"CpuTune")?1:2;(void)context;(void)source;
    if(strcmp(spec->name,"UnconfirmedStarts") && !(strcmp(spec->name,"CpuTune")==0 && !spec->path[0]))return AMDGPU_STATE_ABSENT;
    if(capacity<4)return AMDGPU_STATE_BUFFER;
    memcpy(data,&value,4);v->type=REG_DWORD;v->bytes=4;return AMDGPU_STATE_OK;
}
static void Reset(void)
{ZeroMemory(&memory,sizeof(memory));memory.keys_count=2;memory.values_count=1;memory.keys[1].handles=1;durable=memory;call=fail_at=thaw_before_activate=fail_active_flush=0;}
static void Crash(void)
{unsigned i;memory=durable;for(i=2;i<memory.keys_count;i++)memory.keys[i].handles=0;call=fail_at=0;}
static void NoLeaks(void) {unsigned i;for(i=2;i<memory.keys_count;i++)CHECK(!memory.keys[i].handles);}
int main(void)
{
    AMDGPU_TRANSFER_HEADER h={0};unsigned char buffer[8192];unsigned int bytes=0,steps,i,root,gens,gen,data,value;
    h.origin=AMDGPU_TRANSFER_LEGACY;h.source_schema=1;h.transaction[0]=1;h.device_hash[0]=2;h.source_boot[0]=3;
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);strcpy_s(h.source_version,32,"0.7.216.101");strcpy_s(h.destination_version,32,"0.7.216.102");
    CHECK(AmdgpuTransferCapture(&h,Empty,NULL,buffer,sizeof(buffer),&bytes)==0);
    Reset();CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0);steps=call;NoLeaks();
    root=FindKey(1,L"StateV2");gens=FindKey(root,L"Generations");
    gen=FindKey(gens,L"01000000000000000000000000000000");data=FindKey(gen,L"Data");
    value=FindValue(data,L"CpuTune");CHECK(value && memory.values[value].type==REG_DWORD && *(DWORD*)memory.values[value].data==0);
    // A replay after activation must preserve a later user change.
    *(DWORD*)memory.values[value].data=1;durable=memory;call=0;
    CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0 && *(DWORD*)memory.values[value].data==1);NoLeaks();
    for(i=1;i<=steps;i++) {
        Reset();fail_at=i;CHECK(!NT_SUCCESS(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)));NoLeaks();
        Crash();CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0);NoLeaks();
    }
    // Finishing a retained receipt does not need its old source generation and
    // cannot replay over edits made after the original activation.
    CHECK(DriverStateFinishAccepted((HANDLE)1,(const AMDGPU_TRANSFER_HEADER*)buffer)==0);NoLeaks();
    // CURRENT imports recheck the freeze at activation, after all data flushes.
    Reset();CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0);
    root=FindKey(1,L"StateV2");
    {DWORD one=1;WCHAR owner[]=L"02000000000000000000000000000000";
     CHECK(Put((HANDLE)(uintptr_t)root,L"Frozen",REG_DWORD,&one,4)==0);
     CHECK(Put((HANDLE)(uintptr_t)root,L"FrozenBy",REG_SZ,owner,sizeof(owner))==0);}
    h.origin=AMDGPU_TRANSFER_CURRENT;h.source_schema=2;h.source_generation[0]=1;h.transaction[0]=2;
    CHECK(AmdgpuTransferCapture(&h,Empty,NULL,buffer,sizeof(buffer),&bytes)==0);
    thaw_before_activate=1;CHECK(!NT_SUCCESS(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)));NoLeaks();
    value=FindValue(root,L"Active");CHECK(((WCHAR*)memory.values[value].data)[1]==L'1');
    {DWORD one=1;CHECK(Put((HANDLE)(uintptr_t)root,L"Frozen",REG_DWORD,&one,4)==0);}
    // A failed pointer flush leaves complete Ready data; retry without a crash
    // completes durability and preserves subsequent edits instead of replaying.
    fail_active_flush=1;CHECK(!NT_SUCCESS(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)));NoLeaks();
    CHECK(((WCHAR*)memory.values[value].data)[1]==L'2');
    gens=FindKey(root,L"Generations");gen=FindKey(gens,L"02000000000000000000000000000000");data=FindKey(gen,L"Data");
    {DWORD choice=1;CHECK(Put((HANDLE)(uintptr_t)data,L"CpuTune",REG_DWORD,&choice,4)==0);}
    CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0);NoLeaks();
    value=FindValue(data,L"CpuTune");CHECK(value && *(DWORD*)memory.values[value].data==1);
    // A transfer gives the new driver no boot-loop guard count, from every origin, and keeps the settings and
    // the start record (lab 22631, 2026-10-20: the count 2 of the old driver refused the new one).
    for(i=AMDGPU_TRANSFER_LEGACY;i<=AMDGPU_TRANSFER_ARCHIVE;i++) {
        AMDGPU_TRANSFER_HEADER g=h;unsigned int record;WCHAR id[33];
        g.origin=i;g.board_kind=AMDGPU_BOARD_UNPROVEN;g.source_schema=i==AMDGPU_TRANSFER_LEGACY?1:AmdgpuStateSchema();
        memset(g.source_generation,0,16);if(i!=AMDGPU_TRANSFER_LEGACY)g.source_generation[0]=1;
        memset(g.transaction,0,16);g.transaction[0]=(unsigned char)(0x10+i);
        swprintf_s(id,33,L"%02x000000000000000000000000000000",0x10+i);
        Reset();
        if(i==AMDGPU_TRANSFER_CURRENT) {
            // The update in place: the old generation 01 is active, and this transaction froze it.
            AMDGPU_TRANSFER_HEADER old=h;DWORD one=1;
            old.origin=AMDGPU_TRANSFER_LEGACY;old.source_schema=1;memset(old.source_generation,0,16);
            memset(old.transaction,0,16);old.transaction[0]=1;
            CHECK(AmdgpuTransferCapture(&old,Empty,NULL,buffer,sizeof(buffer),&bytes)==0);
            CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0);
            root=FindKey(1,L"StateV2");
            CHECK(Put((HANDLE)(uintptr_t)root,L"Frozen",REG_DWORD,&one,4)==0);
            CHECK(Put((HANDLE)(uintptr_t)root,L"FrozenBy",REG_SZ,id,sizeof(id))==0);
        }
        CHECK(AmdgpuTransferCapture(&g,Guarded,NULL,buffer,sizeof(buffer),&bytes)==0);
        CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==0);NoLeaks();
        root=FindKey(1,L"StateV2");gens=FindKey(root,L"Generations");gen=FindKey(gens,id);
        data=FindKey(gen,L"Data");CHECK(data!=0);
        CHECK(!FindValue(data,L"UnconfirmedStarts"));
        value=FindValue(data,L"CpuTune");CHECK(value && *(DWORD*)memory.values[value].data==1);
        record=FindKey(data,L"StartRecord");
        value=record?FindValue(record,L"UnconfirmedStarts"):0;CHECK(value && *(DWORD*)memory.values[value].data==2);
    }
    Reset();irql=1;CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==STATUS_INVALID_DEVICE_STATE && !call);
    irql=0;system_thread=0;CHECK(DriverStateCommitTransfer((HANDLE)1,buffer,bytes)==STATUS_INVALID_DEVICE_STATE && !call);
    system_thread=1;
    printf("%u kernel transaction checks, %u failures (%u interruption points)\n",checks,failures,steps);
    return failures?1:0;
}
