// Real state_kernel.c and state_transaction.c share one bounded registry model.
// Only ACL helpers run in Windows; no installed registry, SCM, GPU or threads.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "state.h"
#include "transfer.h"
typedef LONG NTSTATUS;
typedef void *PDRIVER_OBJECT;
typedef struct { LONG count; } EX_RUNDOWN_REF;
typedef struct { LONG owned; } KMUTEX;
typedef struct { USHORT Length,MaximumLength;PWSTR Buffer; } UNICODE_STRING,*PUNICODE_STRING;
typedef struct { UNICODE_STRING *Name;HANDLE Root;ULONG Attributes;PSECURITY_DESCRIPTOR Security; } OBJECT_ATTRIBUTES;
typedef struct { ULONG TitleIndex,Type,DataLength;UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION,*PKEY_VALUE_PARTIAL_INFORMATION;
typedef struct { ULONG Values,SubKeys; } KEY_FULL_INFORMATION;
typedef GUID UUID;
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034L)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xc000003aL)
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)0xc0000206L)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003eL)
#define STATUS_REVISION_MISMATCH ((NTSTATUS)0xc0000059L)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xc0000182L)
#define STATUS_OBJECT_TYPE_MISMATCH ((NTSTATUS)0xc0000024L)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022L)
#define STATUS_PRIVILEGE_NOT_HELD ((NTSTATUS)0xc0000061L)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009aL)
#define STATUS_REGISTRY_IO_FAILED ((NTSTATUS)0xc000014dL)
#define PASSIVE_LEVEL 0
#define POOL_FLAG_PAGED 1
#define OBJ_KERNEL_HANDLE 0x200
#define OBJ_CASE_INSENSITIVE 0x40
#define KeyValuePartialInformation 2
#define KeyFullInformation 3
#define DriverRegKeyParameters 0
#define DriverRegKeySharedPersistentState 2
#define Executive 0
#define KernelMode 0
#define RtlCompareMemory(a,b,n) (memcmp((a),(b),(n))?0:(n))
#define RtlStringCchCopyA StringCchCopyA
#define RtlStringCchPrintfW StringCchPrintfW
#define RtlCreateSecurityDescriptor(s,r) (InitializeSecurityDescriptor(s,r)?0:STATUS_UNSUCCESSFUL)
#define RtlCreateAcl(a,n,r) (InitializeAcl(a,n,r)?0:STATUS_UNSUCCESSFUL)
#define RtlAddAccessAllowedAceEx(a,r,f,m,s) (AddAccessAllowedAceEx(a,r,f,m,s)?0:STATUS_UNSUCCESSFUL)
#define RtlSetDaclSecurityDescriptor(s,p,a,d) (SetSecurityDescriptorDacl(s,p,a,d)?0:STATUS_UNSUCCESSFUL)
#define RtlSetOwnerSecurityDescriptor(s,o,d) (SetSecurityDescriptorOwner(s,o,d)?0:STATUS_UNSUCCESSFUL)
static unsigned checks,failures,allocations,handoff_allocations,flush_calls,failed_flushes,ready_writes,active_writes;
static unsigned create_calls,set_calls,delete_calls,security_calls,uuid_calls,scenarios;
static unsigned fail_flush_at,flush_keys[64];
static WCHAR failed_flush_path[256];
static const char *test_case;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %s line %u: %s\n",test_case,(unsigned)__LINE__,#x);}}while(0)
static struct { ULONG BootId; } TestShared={124};
#define SharedUserData (&TestShared)
static UCHAR KeGetCurrentIrql(void){return PASSIVE_LEVEL;}
static PVOID PsGetCurrentThread(void){return (PVOID)1;}
static BOOLEAN PsIsSystemThread(PVOID thread){(void)thread;return TRUE;}
static void ExInitializeRundownProtection(EX_RUNDOWN_REF *r){r->count=0;}
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *r){++r->count;return TRUE;}
static void ExReleaseRundownProtection(EX_RUNDOWN_REF *r){CHECK(r->count>0);--r->count;}
static void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r){CHECK(r->count==0);}
static void KeInitializeMutex(KMUTEX *m,LONG level){(void)level;m->owned=0;}
static NTSTATUS KeWaitForSingleObject(KMUTEX *m,int reason,int mode,BOOLEAN alert,void *timeout)
{(void)reason;(void)mode;(void)alert;(void)timeout;CHECK(!m->owned);m->owned=1;return 0;}
static void KeReleaseMutex(KMUTEX *m,BOOLEAN wait){(void)wait;CHECK(m->owned);m->owned=0;}
static void *ExAllocatePool2(ULONG flags,SIZE_T bytes,ULONG tag)
{void *p;(void)flags;(void)tag;p=calloc(1,bytes);if(p)++allocations;return p;}
static void ExFreePoolWithTag(void *p,ULONG tag)
{(void)tag;if(p){CHECK(allocations>0);--allocations;free(p);}}
static void RtlInitUnicodeString(UNICODE_STRING *s,PCWSTR in)
{s->Buffer=(PWSTR)in;s->Length=(USHORT)(sizeof(WCHAR)*wcslen(in));s->MaximumLength=(USHORT)(s->Length+sizeof(WCHAR));}
#define InitializeObjectAttributes(a,n,f,r,s) do{(a)->Name=n;(a)->Root=r;(a)->Attributes=f;(a)->Security=s;}while(0)

typedef struct {WCHAR path[256];unsigned handles;} TEST_KEY;
typedef struct {unsigned key,type,bytes;WCHAR name[128];BYTE data[1024];} TEST_VALUE;
static TEST_KEY keys[96];static TEST_VALUE values[768];static unsigned key_count,value_count;
static BYTE handoff[AMDGPU_TRANSFER_MAX_BYTES];static ULONG handoff_bytes;
static const WCHAR GenerationPath[]=L"S\\StateV2\\Generations\\01000000000000000000000000000000";
static const WCHAR MetadataPath[]=L"S\\StateV2\\Generations\\01000000000000000000000000000000\\Migration";
static const WCHAR DataPath[]=L"S\\StateV2\\Generations\\01000000000000000000000000000000\\Data";
static unsigned Key(PCWSTR path,BOOL create)
{
    unsigned i;for(i=1;i<key_count;++i)if(!wcscmp(keys[i].path,path))return i;
    if(!create)return 0;CHECK(key_count<96);if(key_count>=96)exit(2);
    i=key_count++;wcscpy_s(keys[i].path,256,path);return i;
}
static unsigned Value(unsigned key,PCWSTR name,BOOL create)
{
    unsigned i;for(i=1;i<value_count;++i)if(values[i].key==key && !wcscmp(values[i].name,name))return i;
    if(!create)return 0;CHECK(value_count<768);if(value_count>=768)exit(2);
    i=value_count++;values[i].key=key;wcscpy_s(values[i].name,128,name);return i;
}
static void SetValue(unsigned key,PCWSTR name,ULONG type,const void *data,ULONG bytes)
{
    unsigned i=Value(key,name,TRUE);CHECK(bytes<=sizeof(values[i].data));if(bytes>sizeof(values[i].data))exit(2);
    values[i].type=type;values[i].bytes=bytes;if(bytes)memcpy(values[i].data,data,bytes);
}
static void SetNumber(unsigned key,PCWSTR name,ULONG number){SetValue(key,name,REG_DWORD,&number,4);}
static void SetString(unsigned key,PCWSTR name,PCWSTR value)
{SetValue(key,name,REG_SZ,value,(ULONG)(sizeof(WCHAR)*(wcslen(value)+1)));}
static unsigned KeyId(HANDLE key)
{unsigned id=(unsigned)(uintptr_t)key;CHECK(id>0 && id<key_count);if(!id || id>=key_count)exit(2);return id;}
static NTSTATUS TestOpen(PHANDLE out,OBJECT_ATTRIBUTES *oa,BOOL create)
{
    WCHAR path[256];unsigned id,parent=KeyId(oa->Root);
    CHECK(keys[parent].handles>0);CHECK(oa->Attributes==(OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE));
    if(!oa->Name->Buffer[0])id=parent;
    else {CHECK(SUCCEEDED(StringCchPrintfW(path,256,L"%s\\%s",keys[parent].path,oa->Name->Buffer)));id=Key(path,create);}
    if(!id)return STATUS_OBJECT_NAME_NOT_FOUND;
    ++keys[id].handles;*out=(HANDLE)(uintptr_t)id;return 0;
}
static NTSTATUS ZwCreateKey(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa,ULONG title,
    PUNICODE_STRING cls,ULONG options,PULONG disposition)
{(void)access;(void)title;(void)cls;(void)disposition;++create_calls;CHECK(options==REG_OPTION_NON_VOLATILE || options==REG_OPTION_VOLATILE);return TestOpen(out,oa,TRUE);}
static NTSTATUS ZwOpenKey(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa)
{(void)access;return TestOpen(out,oa,FALSE);}
static NTSTATUS ZwClose(HANDLE key)
{unsigned id=KeyId(key);CHECK(keys[id].handles>0);if(keys[id].handles)--keys[id].handles;return 0;}
static NTSTATUS IoOpenDriverRegistryKey(PDRIVER_OBJECT driver,ULONG kind,ACCESS_MASK access,ULONG flags,PHANDLE out)
{
    unsigned id=kind==DriverRegKeyParameters?1:2;(void)driver;
    CHECK(kind==DriverRegKeyParameters || kind==DriverRegKeySharedPersistentState);CHECK(flags==0);
    CHECK(access==(ACCESS_MASK)(kind==DriverRegKeyParameters?KEY_READ:(KEY_READ|KEY_WRITE)));
    ++keys[id].handles;*out=(HANDLE)(uintptr_t)id;return 0;
}
static NTSTATUS ZwQueryValueKey(HANDLE key,UNICODE_STRING *name,ULONG kind,PVOID data,ULONG bytes,PULONG needed)
{
    unsigned id=KeyId(key),v=Value(id,name->Buffer,FALSE);PKEY_VALUE_PARTIAL_INFORMATION info=data;
    CHECK(keys[id].handles>0);CHECK(kind==KeyValuePartialInformation);
    if(!v)return STATUS_OBJECT_NAME_NOT_FOUND;
    *needed=(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+values[v].bytes;
    if(*needed>bytes)return STATUS_BUFFER_TOO_SMALL;
    info->Type=values[v].type;info->DataLength=values[v].bytes;memcpy(info->Data,values[v].data,info->DataLength);return 0;
}
static NTSTATUS ZwSetValueKey(HANDLE key,UNICODE_STRING *name,ULONG title,ULONG type,PVOID data,ULONG bytes)
{
    unsigned id=KeyId(key);(void)title;++set_calls;CHECK(keys[id].handles>0);
    if(!wcscmp(name->Buffer,L"Ready"))++ready_writes;
    if(!wcscmp(name->Buffer,L"Active"))++active_writes;
    SetValue(id,name->Buffer,type,data,bytes);return 0;
}
static NTSTATUS ZwDeleteValueKey(HANDLE key,UNICODE_STRING *name)
{unsigned id=KeyId(key),v=Value(id,name->Buffer,FALSE);++delete_calls;CHECK(keys[id].handles>0);if(!v)return STATUS_OBJECT_NAME_NOT_FOUND;values[v].key=0;return 0;}
static NTSTATUS ZwFlushKey(HANDLE key)
{
    unsigned id=KeyId(key);CHECK(keys[id].handles>0);++flush_calls;
    CHECK(flush_calls<=64);if(flush_calls<=64)flush_keys[flush_calls-1]=id;
    if(fail_flush_at && flush_calls==fail_flush_at) {
        ++failed_flushes;wcscpy_s(failed_flush_path,256,keys[id].path);return STATUS_REGISTRY_IO_FAILED;
    }
    return 0;
}
static NTSTATUS ZwSetSecurityObject(HANDLE key,SECURITY_INFORMATION information,PSECURITY_DESCRIPTOR descriptor)
{
    unsigned id=KeyId(key);PACL acl;PSID owner;BOOL present,defaulted;++security_calls;
    CHECK(keys[id].handles>0);
    CHECK(information==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION));
    CHECK(IsValidSecurityDescriptor(descriptor));CHECK(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted));
    CHECK(owner && IsValidSid(owner));CHECK(GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted));
    CHECK(present && acl && IsValidAcl(acl));CHECK(((SECURITY_DESCRIPTOR*)descriptor)->Control&SE_DACL_PROTECTED);return 0;
}
static NTSTATUS ZwQueryKey(HANDLE key,ULONG kind,PVOID data,ULONG bytes,PULONG needed)
{
    unsigned id=KeyId(key),i;size_t prefix=wcslen(keys[id].path);KEY_FULL_INFORMATION *info=data;
    CHECK(keys[id].handles>0);CHECK(kind==KeyFullInformation);*needed=sizeof(*info);
    if(bytes<sizeof(*info))return STATUS_BUFFER_TOO_SMALL;
    info->Values=info->SubKeys=0;
    for(i=1;i<value_count;++i)if(values[i].key==id)++info->Values;
    for(i=1;i<key_count;++i)if(!wcsncmp(keys[i].path,keys[id].path,prefix) && keys[i].path[prefix]==L'\\' &&
        !wcschr(keys[i].path+prefix+1,L'\\'))++info->SubKeys;
    return 0;
}
static NTSTATUS ExUuidCreate(UUID *id){memset(id,0,sizeof(*id));((BYTE*)id)[0]=(BYTE)(77 + ++uuid_calls);return 0;}
static NTSTATUS DriverStateReadHandoff(PVOID *out,ULONG *bytes)
{
    *out=NULL;*bytes=0;if(!handoff_bytes)return STATUS_OBJECT_NAME_NOT_FOUND;
    *out=malloc(handoff_bytes);if(!*out)return STATUS_INSUFFICIENT_RESOURCES;
    ++handoff_allocations;memcpy(*out,handoff,handoff_bytes);*bytes=handoff_bytes;return 0;
}
static void DriverStateFreeHandoff(PVOID p)
{if(p){CHECK(handoff_allocations>0);--handoff_allocations;free(p);}}
// Macro aliases preserve both complete production bodies in one translation
// unit; only the two file-local names that collide are separated.
#define Hex TransactionHex
#define Wide TransactionWide
/* ACTUAL_TRANSACTION */
#undef Hex
#undef Wide
void DriverStateClose(void);
/* ACTUAL_KERNEL */

static void NoLeaks(void)
{unsigned i;CHECK(!allocations && !handoff_allocations);for(i=1;i<key_count;++i)CHECK(keys[i].handles==0);}
static void Reset(const char *name)
{
    unsigned i,j;WCHAR wide[128];DriverStateClose();NoLeaks();test_case=name;++scenarios;
    memset(keys,0,sizeof(keys));memset(values,0,sizeof(values));key_count=value_count=1;
    CHECK(Key(L"P",TRUE)==1);CHECK(Key(L"S",TRUE)==2);CHECK(Key(L"P\\DefaultsV2",TRUE)==3);
    SetNumber(3,L"Schema",AmdgpuStateSchema());
    for(i=0;i<65;++i)wide[i]=(BYTE)AmdgpuStateCatalogId()[i];SetString(3,L"CatalogId",wide);
    SetString(3,L"PackageProductVersion",L"0.7.216.102");
    for(i=0;i<AmdgpuStateCount();++i) {
        const AMDGPU_STATE_SPEC *s=AmdgpuStateAt(i);
        if(!s->factory_present)continue;CHECK(!s->path[0]);
        for(j=0;s->name[j];++j)wide[j]=(BYTE)s->name[j];wide[j]=0;SetNumber(3,wide,s->factory);
    }
    SetNumber(1,L"CuMode",40);SetNumber(1,L"DpmMode",0);SetNumber(1,L"EnableFullWddm",1);
    handoff_bytes=flush_calls=failed_flushes=ready_writes=active_writes=0;
    create_calls=set_calls=delete_calls=security_calls=uuid_calls=0;
    fail_flush_at=0;memset(flush_keys,0,sizeof(flush_keys));failed_flush_path[0]=0;TestShared.BootId=124;
}
static unsigned int CaptureLegacy(void *context,unsigned int source,const AMDGPU_STATE_SPEC *spec,
    AMDGPU_STATE_VALUE *value,void *data,unsigned int capacity)
{
    ULONG number;(void)context;(void)source;
    if(spec->path[0])return AMDGPU_STATE_ABSENT;
    if(!strcmp(spec->name,"CuMode"))number=40;
    else if(!strcmp(spec->name,"DpmMode"))number=0;
    else if(!strcmp(spec->name,"EnableFullWddm"))number=1;
    else return AMDGPU_STATE_ABSENT;
    CHECK(capacity>=4);if(capacity<4)return AMDGPU_STATE_BUFFER;
    value->type=REG_DWORD;value->bytes=4;memcpy(data,&number,4);return AMDGPU_STATE_OK;
}
static void PrepareLegacy(void)
{
    AMDGPU_TRANSFER_HEADER h={0};unsigned bytes=0;
    h.origin=AMDGPU_TRANSFER_LEGACY;h.source_schema=1;h.transaction[0]=1;h.device_hash[0]=42;
    h.board_kind=AMDGPU_BOARD_UNPROVEN;AmdgpuStateBootToken(123,h.source_boot);
    strcpy_s(h.source_version,32,"0.7.216.100");strcpy_s(h.destination_version,32,"0.7.216.102");
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);
    CHECK(AmdgpuTransferCapture(&h,CaptureLegacy,NULL,handoff,sizeof(handoff),&bytes)==AMDGPU_STATE_OK);
    handoff_bytes=bytes;CHECK(AmdgpuTransferCheck(handoff,handoff_bytes,NULL)==AMDGPU_STATE_OK);
}
static void Unpublished(void)
{
    unsigned root=Key(L"S\\StateV2",FALSE),gen=Key(GenerationPath,FALSE);
    CHECK(!root || !Value(root,L"Active",FALSE));CHECK(!gen || !Value(gen,L"Ready",FALSE));
    CHECK(!ready_writes && !active_writes);CHECK(!DriverStateReady());
}
static void CheckNumber(PCWSTR name,ULONG expected)
{ULONG number=~0u;CHECK(DriverStateNumber(L"",name,TRUE,&number)==0);CHECK(number==expected);}
static void Accepted(void)
{
    unsigned root=Key(L"S\\StateV2",FALSE),gen=Key(GenerationPath,FALSE),v;
    CHECK(DriverStateReady());CHECK(root && gen && Key(DataPath,FALSE) && Key(MetadataPath,FALSE));
    v=Value(root,L"Active",FALSE);CHECK(v && !wcscmp((WCHAR*)values[v].data,L"01000000000000000000000000000000"));
    v=Value(gen,L"Ready",FALSE);CHECK(v && values[v].type==REG_DWORD && *(DWORD*)values[v].data==AmdgpuStateSchema());
    CHECK(Value(gen,L"CatalogId",FALSE)!=0);CHECK(Value(Key(MetadataPath,FALSE),L"Transfer",FALSE)!=0);
}
static NTSTATUS InitReadOnly(void)
{
    unsigned creates=create_calls,sets=set_calls,deletes=delete_calls,flushes=flush_calls,security=security_calls;
    NTSTATUS result=DriverStateInit((PDRIVER_OBJECT)1);
    CHECK(create_calls==creates && set_calls==sets && delete_calls==deletes && flush_calls==flushes && security_calls==security);
    return result;
}
static void Fresh(const char *name)
{
    unsigned i;Reset(name);
    // A fresh installation has only the validated DefaultsV2 child in Parameters.
    for(i=1;i<value_count;++i)if(values[i].key==1)values[i].key=0;
}
static void NoPublishedGeneration(void)
{
    unsigned root=Key(L"S\\StateV2",FALSE),i;
    CHECK(!DriverStateReady() && !ready_writes && !active_writes);
    CHECK(!root || !Value(root,L"Active",FALSE));
    for(i=1;i<key_count;++i)CHECK(!Value(i,L"Ready",FALSE));
}
static void FreshReceipt(AMDGPU_TRANSFER_HEADER *out)
{
    unsigned root=Key(L"S\\StateV2",FALSE),v;
    CHECK(root!=0);v=root?Value(root,L"FactoryTransfer",FALSE):0;
    CHECK(v && values[v].type==REG_BINARY && values[v].bytes==sizeof(*out));
    memset(out,0,sizeof(*out));if(v && values[v].bytes==sizeof(*out))memcpy(out,values[v].data,sizeof(*out));
}
static void ConflictingFactoryReceipt(AMDGPU_TRANSFER_HEADER *out)
{
    memset(out,0,sizeof(*out));out->origin=AMDGPU_TRANSFER_FRESH;out->board_kind=AMDGPU_BOARD_UNPROVEN;
    memcpy(out->transaction,AcceptedTransfer.transaction,16);out->transaction[0]^=1;
    memcpy(out->catalog,AmdgpuStateCatalogId(),64);AmdgpuStateBootToken(TestShared.BootId,out->source_boot);
    strcpy_s(out->destination_version,32,"0.7.216.102");
}
static void FlushWas(unsigned ordinal,PCWSTR path)
{
    CHECK(ordinal>0 && ordinal<=flush_calls && ordinal<=64);
    if(ordinal>0 && ordinal<=flush_calls && ordinal<=64)CHECK(!wcscmp(keys[flush_keys[ordinal-1]].path,path));
}
int main(void)
{
    BYTE good[32]={42},wrong[32]={43},unbound[32]={0},factory_id[16];
    ULONG choice=24;unsigned i,root,gen,metadata,stored,sets_before,bytes=0;
    AMDGPU_TRANSFER_HEADER raw,conflict;
    test_case="initial";key_count=value_count=1;
    Reset("legacy successful activation");PrepareLegacy();
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(Candidate!=NULL && !flush_calls);Unpublished();
    CHECK(DriverStateActivate(wrong)==STATUS_DEVICE_CONFIGURATION_ERROR);Unpublished();
    CHECK(DriverStateActivate(good)==0);Accepted();CheckNumber(L"CuMode",40);CheckNumber(L"DpmMode",0);
    CHECK(DriverStateSet(L"",L"CuMode",REG_DWORD,&choice,sizeof(choice))==0);CheckNumber(L"CuMode",24);
    DriverStateClose();NoLeaks();
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !DriverStateReady() && Candidate==NULL);
    CHECK(DriverStateActivate(good)==0);Accepted();CheckNumber(L"CuMode",24);CHECK(active_writes==1);
    DriverStateClose();NoLeaks();

    Reset("first flush failure and legacy retry");PrepareLegacy();
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);fail_flush_at=1;
    CHECK(DriverStateActivate(good)==STATUS_REGISTRY_IO_FAILED);
    CHECK(flush_calls==1 && failed_flushes==1 && !wcscmp(failed_flush_path,GenerationPath));
    CHECK(Candidate!=NULL);Unpublished();
    gen=Key(GenerationPath,FALSE);metadata=Key(MetadataPath,FALSE);CHECK(gen && metadata);
    CHECK(!Key(DataPath,FALSE));CHECK(!Value(gen,L"CatalogId",FALSE));CHECK(!Value(metadata,L"Transfer",FALSE));
    for(i=1;i<key_count;++i)if(i!=1 && i!=2 && i!=3)CHECK(keys[i].handles==0);
    DriverStateClose();NoLeaks();
    // This retains the failed attempt's in-memory registry keys, as in the
    // observed failed AddDevice. It does not claim a failed flush was durable.
    fail_flush_at=0;
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && Candidate!=NULL);Unpublished();
    CHECK(DriverStateActivate(good)==0);Accepted();CheckNumber(L"CuMode",40);CheckNumber(L"DpmMode",0);
    CHECK(failed_flushes==1 && active_writes==1 && ready_writes==1);
    DriverStateClose();NoLeaks();

    Reset("preexisting Migration-only generation");PrepareLegacy();
    (void)Key(L"S\\StateV2",TRUE);(void)Key(L"S\\StateV2\\Generations",TRUE);
    (void)Key(GenerationPath,TRUE);(void)Key(MetadataPath,TRUE);
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);Unpublished();
    CHECK(DriverStateActivate(good)==0);Accepted();CheckNumber(L"CuMode",40);
    DriverStateClose();NoLeaks();

    Reset("malformed Active is never legacy absence");PrepareLegacy();
    root=Key(L"S\\StateV2",TRUE);SetString(root,L"Active",L"z1000000000000000000000000000000");
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==STATUS_DATA_ERROR);
    CHECK(!DriverStateReady() && Candidate==NULL && !flush_calls && !ready_writes && !active_writes);
    CHECK(DriverStateActivate(good)==STATUS_INVALID_DEVICE_STATE);NoLeaks();

    Fresh("fresh init is read-only and activation retains choices");
    CHECK(InitReadOnly()==0 && Candidate!=NULL && !DriverStateReady());
    CHECK(uuid_calls==1 && !Key(L"S\\StateV2",FALSE));NoPublishedGeneration();
    memcpy(factory_id,AcceptedTransfer.transaction,16);
    CHECK(DriverStateActivate(unbound)==0 && DriverStateReady());FreshReceipt(&raw);
    CHECK(!memcmp(raw.transaction,factory_id,16));
    CHECK(DriverStateSet(L"",L"CuMode",REG_DWORD,&choice,sizeof(choice))==0);
    DriverStateClose();NoLeaks();
    CHECK(InitReadOnly()==0 && Candidate==NULL);CHECK(uuid_calls==1);
    CHECK(DriverStateActivate(unbound)==0);CheckNumber(L"CuMode",24);CHECK(active_writes==1);
    DriverStateClose();NoLeaks();

    Fresh("fresh existing empty root remains read-only until activation");
    root=Key(L"S\\StateV2",TRUE);
    CHECK(InitReadOnly()==0 && Candidate!=NULL && uuid_calls==1);NoPublishedGeneration();
    CHECK(!Value(root,L"FactoryTransfer",FALSE));
    CHECK(DriverStateActivate(unbound)==0 && DriverStateReady());FreshReceipt(&raw);
    DriverStateClose();NoLeaks();

    Fresh("fresh receipt flush failure and reload retry");
    CHECK(InitReadOnly()==0 && Candidate!=NULL);memcpy(factory_id,AcceptedTransfer.transaction,16);
    fail_flush_at=1;
    CHECK(DriverStateActivate(unbound)==STATUS_REGISTRY_IO_FAILED);
    CHECK(flush_calls==1 && failed_flushes==1 && !wcscmp(failed_flush_path,L"S\\StateV2"));
    CHECK(Candidate!=NULL && !Key(L"S\\StateV2\\Generations",FALSE));NoPublishedGeneration();
    FreshReceipt(&raw);CHECK(!memcmp(raw.transaction,factory_id,16));
    DriverStateClose();NoLeaks();fail_flush_at=0;++TestShared.BootId;
    CHECK(InitReadOnly()==0 && Candidate!=NULL);CHECK(uuid_calls==1);
    CHECK(!memcmp(AcceptedTransfer.transaction,factory_id,16));NoPublishedGeneration();
    CHECK(DriverStateActivate(unbound)==0 && DriverStateReady());
    CHECK(flush_calls>1 && failed_flushes==1 && active_writes==1);FlushWas(2,L"S\\StateV2");FreshReceipt(&conflict);
    CHECK(!memcmp(&raw,&conflict,sizeof(raw)));DriverStateClose();NoLeaks();

    Fresh("fresh same-session retry repeats receipt flush");
    CHECK(InitReadOnly()==0);fail_flush_at=1;
    CHECK(DriverStateActivate(unbound)==STATUS_REGISTRY_IO_FAILED && Candidate!=NULL);NoPublishedGeneration();
    FreshReceipt(&raw);fail_flush_at=0;
    CHECK(DriverStateActivate(unbound)==0 && DriverStateReady());FlushWas(2,L"S\\StateV2");
    FreshReceipt(&conflict);CHECK(!memcmp(&raw,&conflict,sizeof(raw)) && uuid_calls==1);
    DriverStateClose();NoLeaks();

    Fresh("fresh generation flush failure reloads retained receipt");
    CHECK(InitReadOnly()==0);memcpy(factory_id,AcceptedTransfer.transaction,16);fail_flush_at=2;
    CHECK(DriverStateActivate(unbound)==STATUS_REGISTRY_IO_FAILED && Candidate!=NULL);NoPublishedGeneration();
    CHECK(flush_calls==2 && failed_flushes==1);FlushWas(1,L"S\\StateV2");
    CHECK(wcsstr(failed_flush_path,L"S\\StateV2\\Generations\\")==failed_flush_path);
    FreshReceipt(&raw);CHECK(!memcmp(raw.transaction,factory_id,16));
    DriverStateClose();NoLeaks();fail_flush_at=0;
    CHECK(InitReadOnly()==0 && Candidate!=NULL && uuid_calls==1);
    CHECK(!memcmp(AcceptedTransfer.transaction,factory_id,16));
    CHECK(DriverStateActivate(unbound)==0 && DriverStateReady());FlushWas(3,L"S\\StateV2");
    FreshReceipt(&conflict);CHECK(!memcmp(&raw,&conflict,sizeof(raw)) && active_writes==1);
    DriverStateClose();NoLeaks();

    Fresh("conflicting factory receipt cannot be overwritten");
    CHECK(InitReadOnly()==0);ConflictingFactoryReceipt(&conflict);
    root=Key(L"S\\StateV2",TRUE);SetValue(root,L"FactoryTransfer",REG_BINARY,&conflict,sizeof(conflict));
    CHECK(DriverStateActivate(unbound)==STATUS_REVISION_MISMATCH);CHECK(Candidate!=NULL);
    CHECK(!set_calls && !flush_calls);NoPublishedGeneration();FreshReceipt(&raw);
    CHECK(!memcmp(&raw,&conflict,sizeof(raw)));DriverStateClose();NoLeaks();

    Fresh("malformed factory receipt cannot be overwritten");
    CHECK(InitReadOnly()==0);root=Key(L"S\\StateV2",TRUE);SetNumber(root,L"FactoryTransfer",1);
    CHECK(DriverStateActivate(unbound)==STATUS_DATA_ERROR);CHECK(Candidate!=NULL);
    CHECK(!set_calls && !flush_calls);NoPublishedGeneration();
    stored=Value(root,L"FactoryTransfer",FALSE);CHECK(stored && values[stored].type==REG_DWORD);
    DriverStateClose();NoLeaks();

    Fresh("short factory receipt cannot be overwritten");
    CHECK(InitReadOnly()==0);ConflictingFactoryReceipt(&conflict);
    root=Key(L"S\\StateV2",TRUE);SetValue(root,L"FactoryTransfer",REG_BINARY,&conflict,sizeof(conflict)-1);
    CHECK(DriverStateActivate(unbound)==STATUS_DATA_ERROR);CHECK(Candidate!=NULL);
    CHECK(!set_calls && !flush_calls);NoPublishedGeneration();
    stored=Value(root,L"FactoryTransfer",FALSE);CHECK(stored && values[stored].bytes==sizeof(conflict)-1);
    DriverStateClose();NoLeaks();

    Fresh("conflicting Active cannot be replaced by factory activation");
    CHECK(InitReadOnly()==0);root=Key(L"S\\StateV2",TRUE);
    SetString(root,L"Active",L"02000000000000000000000000000000");
    CHECK(DriverStateActivate(unbound)==STATUS_REVISION_MISMATCH);
    CHECK(Candidate!=NULL && !DriverStateReady());CHECK(!set_calls && !flush_calls && !ready_writes && !active_writes);
    stored=Value(root,L"Active",FALSE);CHECK(stored && !wcscmp((WCHAR*)values[stored].data,L"02000000000000000000000000000000"));
    CHECK(!Value(root,L"FactoryTransfer",FALSE));DriverStateClose();NoLeaks();

    Fresh("protected fresh handoff does not publish factory receipt");
    memset(&raw,0,sizeof(raw));raw.origin=AMDGPU_TRANSFER_FRESH;raw.board_kind=AMDGPU_BOARD_FRESH;
    raw.transaction[0]=1;raw.device_hash[0]=42;AmdgpuStateBootToken(123,raw.source_boot);
    memcpy(raw.catalog,AmdgpuStateCatalogId(),64);strcpy_s(raw.destination_version,32,"0.7.216.102");
    CHECK(AmdgpuTransferCapture(&raw,Empty,NULL,handoff,sizeof(handoff),&bytes)==0);handoff_bytes=bytes;
    CHECK(InitReadOnly()==0 && Candidate!=NULL && !uuid_calls);sets_before=set_calls;
    CHECK(DriverStateActivate(good)==0);Accepted();CHECK(set_calls>sets_before);
    root=Key(L"S\\StateV2",FALSE);CHECK(!Value(root,L"FactoryTransfer",FALSE));DriverStateClose();NoLeaks();
    printf("state lifecycle: %u scenarios, %u checks, %u failures; actual kernel+transaction, no registry/GPU\n",scenarios,checks,failures);
    return failures?1:0;
}
