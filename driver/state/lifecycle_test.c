// Real state_kernel.c and state_transaction.c share one bounded registry model.
// Only ACL helpers run in Windows; no installed registry, SCM, GPU or threads.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sddl.h>
#include <authz.h>
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
// Same native validator as the KMD; declaration from WDK 26100 wdm.h.
__declspec(dllimport) BOOLEAN NTAPI RtlValidRelativeSecurityDescriptor(PSECURITY_DESCRIPTOR,ULONG,SECURITY_INFORMATION);
#define RtlValidSid IsValidSid
#define RtlLengthSid GetLengthSid
#define RtlEqualSid EqualSid
#define RtlGetAce(a,i,p) (GetAce(a,i,p)?0:STATUS_UNSUCCESSFUL)
#define RtlGetDaclSecurityDescriptor(s,p,a,d) TestGetDacl(s,p,a,d)
#define RtlAddAce(a,r,i,p,n) (AddAce(a,r,i,p,n)?0:STATUS_UNSUCCESSFUL)
#ifndef MAXULONG
#define MAXULONG 0xffffffffUL
#endif
static NTSTATUS TestGetDacl(PSECURITY_DESCRIPTOR s,BOOLEAN *p,PACL *a,BOOLEAN *d)
{BOOL present,def;if(!GetSecurityDescriptorDacl(s,&present,a,&def))return STATUS_UNSUCCESSFUL;*p=(BOOLEAN)present;*d=(BOOLEAN)def;return 0;}
static BYTE shared_security[4096];static DWORD shared_security_bytes;
static unsigned shared_security_writes;static NTSTATUS shared_query_failure,shared_set_failure;
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
    CHECK(access==(ACCESS_MASK)(kind==DriverRegKeyParameters?KEY_READ:(KEY_READ|KEY_WRITE|WRITE_DAC)));
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
static BOOL PublicAccess(PSECURITY_DESCRIPTOR sd,DWORD access)
{
    AUTHZ_RESOURCE_MANAGER_HANDLE rm=NULL;AUTHZ_CLIENT_CONTEXT_HANDLE base=NULL,ctx=NULL;
    PSID user=NULL,users=NULL;SID_AND_ATTRIBUTES group;LUID id={0};BOOL allowed=FALSE;
    AUTHZ_ACCESS_REQUEST request={0};AUTHZ_ACCESS_REPLY reply={0};DWORD granted=0,error=0;
    CHECK(ConvertStringSidToSidW(L"S-1-5-90-0-1",&user));
    CHECK(ConvertStringSidToSidW(L"S-1-5-32-545",&users));
    CHECK(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT,NULL,NULL,NULL,L"state ACL CPU test",&rm));
    CHECK(AuthzInitializeContextFromSid(AUTHZ_SKIP_TOKEN_GROUPS,user,rm,NULL,id,NULL,&base));
    group.Sid=users;group.Attributes=SE_GROUP_ENABLED|SE_GROUP_ENABLED_BY_DEFAULT|SE_GROUP_MANDATORY;
    CHECK(AuthzAddSidsToContext(base,&group,1,NULL,0,&ctx));
    request.DesiredAccess=access;reply.ResultListLength=1;reply.GrantedAccessMask=&granted;reply.Error=&error;
    CHECK(AuthzAccessCheck(0,ctx,&request,NULL,sd,NULL,0,&reply,NULL));
    allowed=error==ERROR_SUCCESS && granted==access;
    if(ctx)AuthzFreeContext(ctx);if(base)AuthzFreeContext(base);if(rm)AuthzFreeResourceManager(rm);
    LocalFree(users);LocalFree(user);return allowed;
}
static NTSTATUS ZwSetSecurityObject(HANDLE key,SECURITY_INFORMATION information,PSECURITY_DESCRIPTOR descriptor)
{
    unsigned id=KeyId(key);PACL acl;PSID owner;BOOL present,defaulted;
    if(id==2) {
        SECURITY_DESCRIPTOR merged;PSID group;BYTE encoded[4096];DWORD size=sizeof(encoded);WORD control;DWORD revision;
        CHECK(information==(DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION) ||
              information==(DACL_SECURITY_INFORMATION|UNPROTECTED_DACL_SECURITY_INFORMATION));
        if(shared_set_failure)return shared_set_failure;
        CHECK(InitializeSecurityDescriptor(&merged,SECURITY_DESCRIPTOR_REVISION));
        CHECK(GetSecurityDescriptorOwner(shared_security,&owner,&defaulted));
        CHECK(SetSecurityDescriptorOwner(&merged,owner,defaulted));
        CHECK(GetSecurityDescriptorGroup(shared_security,&group,&defaulted));
        CHECK(SetSecurityDescriptorGroup(&merged,group,defaulted));
        CHECK(GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted));
        CHECK(SetSecurityDescriptorDacl(&merged,present,acl,defaulted));
        CHECK(GetSecurityDescriptorControl(shared_security,&control,&revision));
        CHECK(!!(information&PROTECTED_DACL_SECURITY_INFORMATION)==!!(control&SE_DACL_PROTECTED));
        merged.Control|=control&SE_DACL_PROTECTED;
        CHECK(MakeSelfRelativeSD(&merged,encoded,&size));memcpy(shared_security,encoded,size);shared_security_bytes=size;
        ++shared_security_writes;return 0;
    }
    ++security_calls;
    CHECK(keys[id].handles>0);
    CHECK(information==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION));
    CHECK(IsValidSecurityDescriptor(descriptor));CHECK(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted));
    CHECK(owner && IsValidSid(owner));CHECK(GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted));
    CHECK(present && acl && IsValidAcl(acl));CHECK(((SECURITY_DESCRIPTOR*)descriptor)->Control&SE_DACL_PROTECTED);
    if(wcsstr(keys[id].path,L"\\Migration") || wcsstr(keys[id].path,L"\\Waves")) {
        CHECK(!PublicAccess(descriptor,KEY_READ));CHECK(!PublicAccess(descriptor,KEY_WRITE));
    }
    return 0;
}
static NTSTATUS ZwQuerySecurityObject(HANDLE key,SECURITY_INFORMATION information,
    PSECURITY_DESCRIPTOR descriptor,ULONG capacity,PULONG needed)
{
    CHECK(KeyId(key)==2 && keys[2].handles>0);CHECK(information==DACL_SECURITY_INFORMATION);
    if(shared_query_failure)return shared_query_failure;
    *needed=shared_security_bytes;if(capacity<shared_security_bytes)return STATUS_BUFFER_TOO_SMALL;
    memcpy(descriptor,shared_security,shared_security_bytes);return 0;
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
static void SharedSecurity(PCWSTR sddl)
{
    PSECURITY_DESCRIPTOR sd=NULL;ULONG bytes=0;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,&bytes));
    CHECK(bytes<=sizeof(shared_security));if(bytes>sizeof(shared_security))exit(2);
    memcpy(shared_security,sd,bytes);shared_security_bytes=bytes;LocalFree(sd);
}
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
    shared_security_writes=0;shared_query_failure=shared_set_failure=0;
    SharedSecurity(L"O:SYG:SYD:P(A;OICI;KA;;;SY)(A;OICI;CCDCLCSWRPSDRC;;;BA)"
        L"(A;OICI;KA;;;S-1-5-80-2476284245-3118694172-782969475-3399061036-443127832)");
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
static void RootAclCases(void)
{
    BYTE old[4096];DWORD old_bytes;PACL before,after;BOOL present,defaulted;
    unsigned i;PSID owner_before,owner_after;PVOID a,b;unsigned writes;
    Reset("shared root grants read only and preserves every original ACE");PrepareLegacy();
    memcpy(old,shared_security,shared_security_bytes);old_bytes=shared_security_bytes;
    CHECK(!PublicAccess(shared_security,KEY_READ));CHECK(!PublicAccess(shared_security,KEY_WRITE));
    CHECK(InitReadOnly()==0);CHECK(shared_security_writes==1 && !DriverStateReady());
    CHECK(PublicAccess(shared_security,KEY_READ));CHECK(!PublicAccess(shared_security,KEY_WRITE));
    CHECK(!PublicAccess(shared_security,WRITE_DAC|WRITE_OWNER));CHECK(!PublicAccess(shared_security,DELETE));
    CHECK(GetSecurityDescriptorDacl(old,&present,&before,&defaulted));
    CHECK(GetSecurityDescriptorDacl(shared_security,&present,&after,&defaulted));
    CHECK(after->AceCount==before->AceCount+1);
    for(i=0;i<before->AceCount;i++) {
        CHECK(GetAce(before,i,&a));CHECK(GetAce(after,i,&b));
        CHECK(!memcmp(a,b,((ACE_HEADER*)a)->AceSize));
    }
    CHECK(GetAce(after,before->AceCount,&b));CHECK(((ACE_HEADER*)b)->AceFlags==0);
    CHECK(((ACCESS_ALLOWED_ACE*)b)->Mask==KEY_READ);
    CHECK(GetSecurityDescriptorOwner(old,&owner_before,&defaulted));
    CHECK(GetSecurityDescriptorOwner(shared_security,&owner_after,&defaulted));CHECK(EqualSid(owner_before,owner_after));
    DriverStateClose();NoLeaks();CHECK(InitReadOnly()==0 && shared_security_writes==1);
    CHECK(old_bytes<shared_security_bytes);DriverStateClose();NoLeaks();

    Reset("shared root explicit write deny retained");PrepareLegacy();
    SharedSecurity(L"O:SYG:SYD:P(D;;0x00000006;;;BU)(A;;KA;;;SY)");
    CHECK(InitReadOnly()==0);CHECK(PublicAccess(shared_security,KEY_READ));CHECK(!PublicAccess(shared_security,KEY_WRITE));
    DriverStateClose();NoLeaks();
    Reset("shared root explicit read deny is not bypassed");PrepareLegacy();
    SharedSecurity(L"O:SYG:SYD:P(D;;KR;;;BU)(A;;KA;;;SY)");
    CHECK(InitReadOnly()==0);CHECK(!PublicAccess(shared_security,KEY_READ));DriverStateClose();NoLeaks();

    Reset("shared root explicit allow precedes inherited ACE, flags unchanged");PrepareLegacy();
    SharedSecurity(L"O:SYG:SYD:(A;;KA;;;SY)(A;ID;KR;;;BA)");
    CHECK(InitReadOnly()==0);CHECK(GetSecurityDescriptorDacl(shared_security,&present,&after,&defaulted));
    CHECK(after->AceCount==3);CHECK(GetAce(after,1,&b));CHECK(((ACE_HEADER*)b)->AceFlags==0);
    CHECK(GetAce(after,2,&b));CHECK(((ACE_HEADER*)b)->AceFlags==INHERITED_ACE);
    CHECK(!(((SECURITY_DESCRIPTOR*)shared_security)->Control&SE_DACL_PROTECTED));DriverStateClose();NoLeaks();

    Reset("shared root preexisting read ACL is not rewritten");PrepareLegacy();
    SharedSecurity(L"O:SYG:SYD:P(A;;KA;;;SY)(A;;KR;;;BU)");
    CHECK(InitReadOnly()==0 && shared_security_writes==0);DriverStateClose();NoLeaks();
    Reset("shared root inherited-only allow does not grant root access");PrepareLegacy();
    SharedSecurity(L"O:SYG:SYD:P(A;;KA;;;SY)(A;CIIO;KR;;;BU)");
    CHECK(!PublicAccess(shared_security,KEY_READ));CHECK(InitReadOnly()==0 && shared_security_writes==1);
    CHECK(PublicAccess(shared_security,KEY_READ));DriverStateClose();NoLeaks();

    Reset("shared root query failure prevents Ready");PrepareLegacy();shared_query_failure=STATUS_ACCESS_DENIED;
    CHECK(InitReadOnly()==STATUS_ACCESS_DENIED);CHECK(!shared_security_writes && !DriverStateReady());NoLeaks();
    Reset("shared root DACL outside returned descriptor is refused");PrepareLegacy();
    ((SECURITY_DESCRIPTOR_RELATIVE*)shared_security)->Dacl=shared_security_bytes+4;
    CHECK(InitReadOnly()==STATUS_DATA_ERROR);CHECK(!shared_security_writes && !DriverStateReady());NoLeaks();
    Reset("shared root security write failure prevents Ready");PrepareLegacy();shared_set_failure=STATUS_ACCESS_DENIED;
    CHECK(InitReadOnly()==STATUS_ACCESS_DENIED);CHECK(!shared_security_writes && !DriverStateReady());NoLeaks();
    Reset("shared root absent DACL is refused");PrepareLegacy();SharedSecurity(L"O:SYG:SY");
    writes=shared_security_writes;CHECK(InitReadOnly()==STATUS_ACCESS_DENIED);
    CHECK(shared_security_writes==writes && !DriverStateReady());NoLeaks();
}
int main(void)
{
    BYTE good[32]={42},wrong[32]={43},unbound[32]={0},factory_id[16];
    ULONG choice=24;unsigned i,root,gen,metadata,stored,sets_before,bytes=0;
    AMDGPU_TRANSFER_HEADER raw,conflict;
    test_case="initial";key_count=value_count=1;
    RootAclCases();
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

    // An update in place: the installer's worker froze the active generation and captured it into the handoff;
    // the first start of the new package merges the capture with that generation's data and publishes a new
    // generation under its own catalog. The retained generation may belong to an earlier catalog of the same
    // schema (a release that added settings); without a handoff such a generation is refused.
    for(i=0;i<5;i++) {
        static const WCHAR same[]=L"02000000000000000000000000000000";
        static const WCHAR earlier[]=L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        static const char *const names[]={"update in place with the same catalog","update in place across a catalog change",
            "a generation of another catalog without a handoff is refused","a malformed catalog identity is never a source",
            "a receipt of another catalog than the generation is refused"};
        AMDGPU_TRANSFER_HEADER old;WCHAR wide[65];unsigned j,gen2,data2,meta2;
        Reset(names[i]);
        root=Key(L"S\\StateV2",TRUE);SetString(root,L"Active",same);
        SetNumber(root,L"Frozen",i==2?0:1);SetString(root,L"FrozenBy",L"01000000000000000000000000000000");
        gen2=Key(L"S\\StateV2\\Generations\\02000000000000000000000000000000",TRUE);
        data2=Key(L"S\\StateV2\\Generations\\02000000000000000000000000000000\\Data",TRUE);
        meta2=Key(L"S\\StateV2\\Generations\\02000000000000000000000000000000\\Migration",TRUE);
        SetNumber(gen2,L"Ready",AmdgpuStateSchema());
        for(j=0;j<65;++j)wide[j]=(BYTE)AmdgpuStateCatalogId()[j];
        SetString(gen2,L"CatalogId",i==0?wide:i==3?L"wrong":earlier);
        SetNumber(data2,L"CuMode",40);SetNumber(data2,L"DpmMode",0);SetNumber(data2,L"EnableFullWddm",1);
        memset(&old,0,sizeof(old));old.magic=AMDGPU_TRANSFER_MAGIC;old.version=AMDGPU_TRANSFER_VERSION;
        old.origin=AMDGPU_TRANSFER_LEGACY;old.source_schema=1;old.board_kind=AMDGPU_BOARD_UNPROVEN;
        old.transaction[0]=2;old.device_hash[0]=42;AmdgpuStateBootToken(100,old.source_boot);
        strcpy_s(old.source_version,32,"0.7.216.100");strcpy_s(old.destination_version,32,"0.7.216.101");
        memcpy(old.catalog,AmdgpuStateCatalogId(),64);
        if(i==1 || i==2)memcpy(old.catalog,"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",64);
        SetValue(meta2,L"Transfer",REG_BINARY,&old,sizeof(old));
        if(i!=2) {
            memset(&raw,0,sizeof(raw));raw.origin=AMDGPU_TRANSFER_CURRENT;raw.source_schema=AmdgpuStateSchema();
            raw.transaction[0]=1;raw.source_generation[0]=2;raw.device_hash[0]=42;
            AmdgpuStateBootToken(123,raw.source_boot);memcpy(raw.catalog,AmdgpuStateCatalogId(),64);
            strcpy_s(raw.source_version,32,"0.7.216.101");strcpy_s(raw.destination_version,32,"0.7.216.102");
            CHECK(AmdgpuTransferCapture(&raw,CaptureLegacy,NULL,handoff,sizeof(handoff),&bytes)==0);handoff_bytes=bytes;
        }
        if(i>=2) {
            CHECK(DriverStateInit((PDRIVER_OBJECT)1)==STATUS_REVISION_MISMATCH);
            CHECK(!DriverStateReady() && Candidate==NULL && !ready_writes && !active_writes);NoLeaks();continue;
        }
        CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(Candidate!=NULL && !DriverStateReady());
        CHECK(!ready_writes && !active_writes);
        CHECK(DriverStateActivate(good)==0);Accepted();CheckNumber(L"CuMode",40);CheckNumber(L"DpmMode",0);
        gen=Key(GenerationPath,FALSE);stored=Value(gen,L"CatalogId",FALSE);
        CHECK(stored && !wcscmp((WCHAR*)values[stored].data,wide));
        stored=Value(gen2,L"CatalogId",FALSE);CHECK(stored && !wcscmp((WCHAR*)values[stored].data,i?earlier:wide));
        stored=Value(root,L"Frozen",FALSE);CHECK(stored && *(DWORD*)values[stored].data==0);
        DriverStateClose();NoLeaks();
        // The next start finds the new generation: the handoff is only its receipt.
        CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !DriverStateReady() && Candidate==NULL);
        CHECK(DriverStateActivate(good)==0);Accepted();CheckNumber(L"CuMode",40);
        DriverStateClose();NoLeaks();
    }
    printf("state lifecycle: %u scenarios, %u checks, %u failures; actual kernel+transaction, no registry/GPU\n",scenarios,checks,failures);
    return failures?1:0;
}
