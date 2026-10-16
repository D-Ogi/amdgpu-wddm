// Executes state_kernel.c against bounded in-memory keys. No installed state,
// SCM, registry, hardware, files or worker threads are accessed by this test.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sddl.h>
#include "state.h"
#include "transfer.h"
#include "../kmd/board_memory_format.h"
typedef LONG NTSTATUS;
typedef void *PDRIVER_OBJECT;
typedef struct { LONG count; } EX_RUNDOWN_REF;
typedef struct { LONG owned; } KMUTEX;
typedef struct { USHORT Length,MaximumLength;PWSTR Buffer; } UNICODE_STRING;
typedef struct { UNICODE_STRING *Name;HANDLE Root; } OBJECT_ATTRIBUTES;
typedef struct { ULONG TitleIndex,Type,DataLength;UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION,*PKEY_VALUE_PARTIAL_INFORMATION;
typedef struct { ULONG Values,SubKeys; } KEY_FULL_INFORMATION;
typedef GUID UUID;
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0L
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
#define RtlCompareMemory(a,b,n) (memcmp(a,b,n)?0:(n))
#define RtlStringCchCopyA StringCchCopyA
#define RtlStringCchPrintfW StringCchPrintfW
// WDK 26100 wdm.h signature; execute the same native descriptor validator.
__declspec(dllimport) BOOLEAN NTAPI RtlValidRelativeSecurityDescriptor(PSECURITY_DESCRIPTOR,ULONG,SECURITY_INFORMATION);
#define RtlValidSid IsValidSid
#define RtlLengthSid GetLengthSid
#define RtlEqualSid EqualSid
#define RtlGetAce(a,i,p) (GetAce(a,i,p)?0:STATUS_ACCESS_DENIED)
#define RtlAddAce(a,r,i,p,n) (AddAce(a,r,i,p,n)?0:STATUS_ACCESS_DENIED)
#define RtlCreateAcl(a,n,r) (InitializeAcl(a,n,r)?0:STATUS_ACCESS_DENIED)
#define RtlAddAccessAllowedAceEx(a,r,f,m,s) (AddAccessAllowedAceEx(a,r,f,m,s)?0:STATUS_ACCESS_DENIED)
#define RtlCreateSecurityDescriptor(s,r) (InitializeSecurityDescriptor(s,r)?0:STATUS_ACCESS_DENIED)
#define RtlSetDaclSecurityDescriptor(s,p,a,d) (SetSecurityDescriptorDacl(s,p,a,d)?0:STATUS_ACCESS_DENIED)
#ifndef MAXULONG
#define MAXULONG 0xffffffffUL
#endif
static NTSTATUS RtlGetDaclSecurityDescriptor(PSECURITY_DESCRIPTOR s,BOOLEAN *p,PACL *a,BOOLEAN *d)
{BOOL present,def;if(!GetSecurityDescriptorDacl(s,&present,a,&def))return STATUS_ACCESS_DENIED;*p=(BOOLEAN)present;*d=(BOOLEAN)def;return 0;}
static BYTE shared_security[4096];static DWORD shared_security_bytes;
static unsigned checks,failures,commits,allocations,seedFlushFail,seedFlushes,seedCalls,threadCalls;
static PCWSTR readDenied;
static NTSTATUS seedResult;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static struct { ULONG BootId; } TestShared={124};
#define SharedUserData (&TestShared)
static UCHAR KeGetCurrentIrql(void){return 0;}
static PVOID PsGetCurrentThread(void){return (PVOID)1;}
static BOOLEAN PsIsSystemThread(PVOID t){(void)t;return TRUE;}
static void ExInitializeRundownProtection(EX_RUNDOWN_REF *r){r->count=0;}
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *r){r->count++;return TRUE;}
static void ExReleaseRundownProtection(EX_RUNDOWN_REF *r){CHECK(r->count>0);r->count--;}
static void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r){CHECK(!r->count);}
static void KeInitializeMutex(KMUTEX *m,LONG level){(void)level;m->owned=0;}
static NTSTATUS KeWaitForSingleObject(KMUTEX *m,int reason,int mode,BOOLEAN alert,void *timeout)
{(void)reason;(void)mode;(void)alert;(void)timeout;CHECK(!m->owned);m->owned=1;return 0;}
static void KeReleaseMutex(KMUTEX *m,BOOLEAN wait){(void)wait;CHECK(m->owned);m->owned=0;}
static void *ExAllocatePool2(ULONG flags,SIZE_T bytes,ULONG tag)
{void *p;(void)flags;(void)tag;p=calloc(1,bytes);if(p)allocations++;return p;}
static void ExFreePoolWithTag(void *p,ULONG tag){(void)tag;if(p){CHECK(allocations>0);allocations--;free(p);}}
static void RtlInitUnicodeString(UNICODE_STRING *s,PCWSTR in)
{s->Buffer=(PWSTR)in;s->Length=(USHORT)(2*wcslen(in));s->MaximumLength=s->Length+2;}
#define InitializeObjectAttributes(a,n,f,r,s) do{(void)(f);(void)(s);(a)->Name=n;(a)->Root=r;}while(0)
typedef struct {WCHAR path[256];unsigned handles;} TEST_KEY;
typedef struct {unsigned key,type,bytes;WCHAR name[128];BYTE data[1024];} TEST_VALUE;
static TEST_KEY TestKeys[64];static TEST_VALUE TestValues[768];static unsigned TestKeyCount,TestValueCount;
static BYTE Handoff[16384];static ULONG HandoffBytes;
static unsigned TestKey(PCWSTR path,BOOL create)
{
    unsigned i;for(i=1;i<TestKeyCount;i++)if(!wcscmp(TestKeys[i].path,path))return i;
    if(!create)return 0;i=TestKeyCount++;CHECK(i<64);wcscpy_s(TestKeys[i].path,256,path);return i;
}
static unsigned TestValue(unsigned key,PCWSTR name,BOOL create)
{
    unsigned i;for(i=1;i<TestValueCount;i++)if(TestValues[i].key==key && !wcscmp(TestValues[i].name,name))return i;
    if(!create)return 0;i=TestValueCount++;CHECK(i<768);TestValues[i].key=key;wcscpy_s(TestValues[i].name,128,name);return i;
}
static void TestPut(unsigned key,PCWSTR name,ULONG type,const void *data,ULONG bytes)
{unsigned v=TestValue(key,name,TRUE);CHECK(bytes<=1024);TestValues[v].type=type;TestValues[v].bytes=bytes;memcpy(TestValues[v].data,data,bytes);}
static void TestNumber(unsigned key,PCWSTR name,ULONG number){TestPut(key,name,REG_DWORD,&number,4);}
static void TestString(unsigned key,PCWSTR name,PCWSTR value){TestPut(key,name,REG_SZ,value,(ULONG)(2*(wcslen(value)+1)));}
static NTSTATUS TestOpen(HANDLE *out,OBJECT_ATTRIBUTES *oa,BOOL create)
{
    WCHAR path[256];unsigned id,parent=(unsigned)(uintptr_t)oa->Root;
    if(!oa->Name->Buffer[0])id=parent;
    else {StringCchPrintfW(path,256,L"%s\\%s",TestKeys[parent].path,oa->Name->Buffer);id=TestKey(path,create);}
    if(!id)return STATUS_OBJECT_NAME_NOT_FOUND;TestKeys[id].handles++;*out=(HANDLE)(uintptr_t)id;return 0;
}
static NTSTATUS ZwCreateKey(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa,ULONG title,void *cls,ULONG options,void *disp)
{(void)access;(void)title;(void)cls;(void)options;(void)disp;return TestOpen(out,oa,TRUE);}
static NTSTATUS ZwOpenKey(PHANDLE out,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa)
{(void)access;return TestOpen(out,oa,FALSE);}
static NTSTATUS ZwClose(HANDLE key){unsigned id=(unsigned)(uintptr_t)key;if(id==0xdead)return 0;CHECK(TestKeys[id].handles>0);TestKeys[id].handles--;return 0;}
static NTSTATUS IoOpenDriverRegistryKey(PDRIVER_OBJECT d,ULONG kind,ACCESS_MASK access,ULONG flags,PHANDLE out)
{unsigned id=kind==DriverRegKeyParameters?1:2;(void)d;CHECK(flags==0);
 CHECK(access==(ACCESS_MASK)(id==1?KEY_READ:(KEY_READ|KEY_WRITE|WRITE_DAC)));
 TestKeys[id].handles++;*out=(HANDLE)(uintptr_t)id;return 0;}
static NTSTATUS ZwQuerySecurityObject(HANDLE key,SECURITY_INFORMATION information,
    PSECURITY_DESCRIPTOR descriptor,ULONG capacity,PULONG needed)
{
    CHECK((uintptr_t)key==2 && TestKeys[2].handles>0);CHECK(information==DACL_SECURITY_INFORMATION);
    *needed=shared_security_bytes;if(capacity<shared_security_bytes)return STATUS_BUFFER_TOO_SMALL;
    memcpy(descriptor,shared_security,shared_security_bytes);return 0;
}
static NTSTATUS ZwSetSecurityObject(HANDLE key,SECURITY_INFORMATION information,PSECURITY_DESCRIPTOR descriptor)
{
    SECURITY_DESCRIPTOR merged;PACL acl,before;PSID owner,group;BOOL present,defaulted;
    BYTE encoded[4096];DWORD size=sizeof(encoded);PVOID a,b;unsigned i;
    CHECK((uintptr_t)key==2 && TestKeys[2].handles>0);
    CHECK(information==(DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION));
    CHECK(GetSecurityDescriptorDacl(shared_security,&present,&before,&defaulted));
    CHECK(GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted));
    CHECK(present && acl && IsValidAcl(acl));CHECK(acl->AceCount==before->AceCount+1);
    for(i=0;i<before->AceCount;i++) {
        CHECK(GetAce(before,i,&a));CHECK(GetAce(acl,i,&b));CHECK(!memcmp(a,b,((ACE_HEADER*)a)->AceSize));
    }
    CHECK(GetAce(acl,before->AceCount,&b));CHECK(((ACE_HEADER*)b)->AceFlags==0);
    CHECK(((ACCESS_ALLOWED_ACE*)b)->Mask==KEY_READ);
    CHECK(InitializeSecurityDescriptor(&merged,SECURITY_DESCRIPTOR_REVISION));
    CHECK(SetSecurityDescriptorDacl(&merged,present,acl,defaulted));
    CHECK(GetSecurityDescriptorOwner(shared_security,&owner,&defaulted));CHECK(SetSecurityDescriptorOwner(&merged,owner,defaulted));
    CHECK(GetSecurityDescriptorGroup(shared_security,&group,&defaulted));CHECK(SetSecurityDescriptorGroup(&merged,group,defaulted));
    merged.Control|=SE_DACL_PROTECTED;
    CHECK(MakeSelfRelativeSD(&merged,encoded,&size));memcpy(shared_security,encoded,size);shared_security_bytes=size;
    return 0;
}
static NTSTATUS ZwQueryValueKey(HANDLE key,UNICODE_STRING *name,ULONG kind,PVOID data,ULONG bytes,PULONG needed)
{
    unsigned v=TestValue((unsigned)(uintptr_t)key,name->Buffer,FALSE);PKEY_VALUE_PARTIAL_INFORMATION info=data;(void)kind;
    if(readDenied && !wcscmp(readDenied,name->Buffer))return STATUS_ACCESS_DENIED;
    if(!v)return STATUS_OBJECT_NAME_NOT_FOUND;*needed=(ULONG)FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+TestValues[v].bytes;
    if(*needed>bytes)return STATUS_BUFFER_TOO_SMALL;info->Type=TestValues[v].type;info->DataLength=TestValues[v].bytes;
    memcpy(info->Data,TestValues[v].data,info->DataLength);return 0;
}
static NTSTATUS ZwSetValueKey(HANDLE key,UNICODE_STRING *name,ULONG title,ULONG type,PVOID data,ULONG bytes)
{(void)title;TestPut((unsigned)(uintptr_t)key,name->Buffer,type,data,bytes);return 0;}
static NTSTATUS ZwDeleteValueKey(HANDLE key,UNICODE_STRING *name)
{unsigned v=TestValue((unsigned)(uintptr_t)key,name->Buffer,FALSE);if(!v)return STATUS_OBJECT_NAME_NOT_FOUND;TestValues[v].key=0;return 0;}
static NTSTATUS ZwFlushKey(HANDLE key)
{(void)key;if(seedFlushFail)return STATUS_ACCESS_DENIED;seedFlushes++;return 0;}
static NTSTATUS ZwQueryKey(HANDLE key,ULONG kind,PVOID data,ULONG bytes,PULONG needed)
{KEY_FULL_INFORMATION *info=data;(void)key;(void)kind;(void)bytes;*needed=sizeof(*info);info->Values=0;info->SubKeys=1;return 0;}
static NTSTATUS ExUuidCreate(UUID *id){memset(id,0,sizeof(*id));((BYTE*)id)[0]=77;return 0;}
static NTSTATUS DriverStateReadHandoff(PVOID *out,ULONG *bytes)
{*out=NULL;*bytes=0;if(!HandoffBytes)return STATUS_OBJECT_NAME_NOT_FOUND;*out=malloc(HandoffBytes);memcpy(*out,Handoff,HandoffBytes);*bytes=HandoffBytes;return 0;}
static void DriverStateFreeHandoff(PVOID p){free(p);}
static void TestHex(const BYTE id[16],WCHAR out[33])
{unsigned i;for(i=0;i<16;i++)swprintf_s(out+2*i,33-2*i,L"%02x",id[i]);}
static void TestPublish(const void *buffer,ULONG bytes)
{
    AMDGPU_TRANSFER_HEADER h;unsigned root,gen,data,metadata,i;WCHAR tx[33],path[256],wide[128];
    CHECK(!AmdgpuTransferCheck(buffer,bytes,&h));TestHex(h.transaction,tx);
    root=TestKey(L"S\\StateV2",TRUE);TestString(root,L"Active",tx);TestNumber(root,L"Frozen",0);
    StringCchPrintfW(path,256,L"S\\StateV2\\Generations\\%s",tx);gen=TestKey(path,TRUE);
    TestNumber(gen,L"Ready",AmdgpuStateSchema());for(i=0;i<65;i++)wide[i]=(BYTE)AmdgpuStateCatalogId()[i];TestString(gen,L"CatalogId",wide);
    StringCchCatW(path,256,L"\\Migration");metadata=TestKey(path,TRUE);TestPut(metadata,L"Transfer",REG_BINARY,&h,sizeof(h));
    StringCchPrintfW(path,256,L"S\\StateV2\\Generations\\%s\\Data",tx);data=TestKey(path,TRUE);
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *s=AmdgpuStateAt(i);AMDGPU_STATE_VALUE v;const void *payload;unsigned j,key=data;ULONG off=0;
        if(s->kind==AMDGPU_STATE_VOLATILE)continue;
        if(s->path[0]){for(j=0;s->path[j];j++)wide[j]=(BYTE)s->path[j];wide[j]=0;StringCchPrintfW(path,256,L"%s\\%s",TestKeys[data].path,wide);key=TestKey(path,TRUE);}
        (void)AmdgpuTransferFind(buffer,bytes,i,&v,&payload);
        if(v.status==AMDGPU_STATE_ABSENT && h.origin==AMDGPU_TRANSFER_LEGACY && s->legacy_off){v.status=0;v.type=REG_DWORD;v.bytes=4;payload=&off;}
        for(j=0;s->name[j];j++)wide[j]=(BYTE)s->name[j];wide[j]=0;
        if(!v.status)TestPut(key,wide,v.type,payload,v.bytes);
    }
}
static NTSTATUS DriverStateCommitTransfer(HANDLE shared,const void *buffer,ULONG bytes)
{(void)shared;commits++;TestPublish(buffer,bytes);return 0;}
static NTSTATUS DriverStateFinishAccepted(HANDLE shared,const AMDGPU_TRANSFER_HEADER *h){(void)shared;(void)h;return 0;}
void DriverStateClose(void);
/* ACTUAL_KERNEL */
typedef struct {unsigned unused;} BC250_DEVICE;
static NTSTATUS BoardMemoryStoreSeed(BC250_DEVICE *device,const BOARD_STORE_RECORD *legacy,BOOLEAN fresh)
{
    unsigned metadata=TestKey(L"S\\StateV2\\Generations\\01000000000000000000000000000000\\Migration",FALSE);
    CHECK(device!=NULL);CHECK(fresh && !legacy);CHECK(seedFlushes!=0);
    CHECK(TestValue(metadata,L"BoardSeedConsumed",FALSE)!=0);seedCalls++;return seedResult;
}
static NTSTATUS PsCreateSystemThread(PHANDLE thread,ACCESS_MASK access,OBJECT_ATTRIBUTES *oa,
                                    HANDLE process,PVOID client,void (*worker)(PVOID),PVOID context)
{(void)access;(void)oa;(void)process;(void)client;*thread=(HANDLE)(uintptr_t)0xdead;threadCalls++;worker(context);return 0;}
static NTSTATUS PsTerminateSystemThread(NTSTATUS status){return status;}
static NTSTATUS ZwWaitForSingleObject(HANDLE thread,BOOLEAN alert,PVOID timeout)
{CHECK(thread==(HANDLE)(uintptr_t)0xdead);CHECK(!alert && !timeout);return 0;}
/* ACTUAL_BOARD */
static unsigned int TestEmpty(void *c,unsigned int src,const AMDGPU_STATE_SPEC *s,AMDGPU_STATE_VALUE *v,void *p,unsigned int cap)
{(void)c;(void)src;(void)s;(void)v;(void)p;(void)cap;return AMDGPU_STATE_ABSENT;}
static void TestReset(void)
{
    unsigned i;PSECURITY_DESCRIPTOR sd=NULL;ULONG size=0;
    DriverStateClose();CHECK(!allocations);for(i=1;i<TestKeyCount;i++)CHECK(!TestKeys[i].handles);
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:SYG:SYD:P(A;OICI;KA;;;SY)(A;OICI;CCDCLCSWRPSDRC;;;BA)"
        L"(A;OICI;KA;;;S-1-5-80-2476284245-3118694172-782969475-3399061036-443127832)",
        SDDL_REVISION_1,&sd,&size));
    CHECK(size<=sizeof(shared_security));if(size>sizeof(shared_security))exit(2);
    memcpy(shared_security,sd,size);shared_security_bytes=size;LocalFree(sd);
    memset(TestKeys,0,sizeof(TestKeys));memset(TestValues,0,sizeof(TestValues));TestKeyCount=TestValueCount=1;
    CHECK(TestKey(L"P",TRUE)==1);CHECK(TestKey(L"S",TRUE)==2);(void)TestKey(L"P\\DefaultsV2",TRUE);
    TestNumber(3,L"Schema",AmdgpuStateSchema());{WCHAR wide[128];for(i=0;i<65;i++)wide[i]=(BYTE)AmdgpuStateCatalogId()[i];TestString(3,L"CatalogId",wide);}
    TestString(3,L"PackageProductVersion",L"0.7.216.102");
    for(i=0;i<AmdgpuStateCount();i++) {
        const AMDGPU_STATE_SPEC *s=AmdgpuStateAt(i);WCHAR name[128];unsigned j;
        if(!s->factory_present)continue;CHECK(!s->path[0]);for(j=0;s->name[j];j++)name[j]=(BYTE)s->name[j];name[j]=0;TestNumber(3,name,s->factory);
    }
    commits=HandoffBytes=seedFlushFail=seedFlushes=seedCalls=threadCalls=0;readDenied=NULL;seedResult=0;
}
static AMDGPU_TRANSFER_HEADER TestHandoff(unsigned origin)
{
    AMDGPU_TRANSFER_HEADER h={0};unsigned bytes=0;
    h.origin=origin;h.transaction[0]=1;h.device_hash[0]=42;h.source_boot[0]=3;
    h.board_kind=origin==AMDGPU_TRANSFER_FRESH?AMDGPU_BOARD_FRESH:AMDGPU_BOARD_UNPROVEN;
    if(origin==AMDGPU_TRANSFER_LEGACY){h.source_schema=1;strcpy_s(h.source_version,32,"0.7.216.101");}
    if(origin==AMDGPU_TRANSFER_CURRENT || origin==AMDGPU_TRANSFER_ARCHIVE){h.source_schema=2;h.source_generation[0]=2;strcpy_s(h.source_version,32,"0.7.216.101");}
    memcpy(h.catalog,AmdgpuStateCatalogId(),64);strcpy_s(h.destination_version,32,"0.7.216.102");
    CHECK(!AmdgpuTransferCapture(&h,TestEmpty,NULL,Handoff,sizeof(Handoff),&bytes));HandoffBytes=bytes;return h;
}
int main(void)
{
    BYTE good[32]={42},bad[32]={43},zero[32]={0};ULONG number=0;HANDLE key=NULL;BOOLEAN first=FALSE;unsigned root,data;AMDGPU_TRANSFER_HEADER h;BC250_DEVICE device={0};
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !DriverStateReady() && !commits);
    CHECK(DriverStateNumber(L"",L"EnableFullWddm",TRUE,&number)==0);
    CHECK(DriverStateOpenData(&key)==STATUS_INVALID_DEVICE_STATE && !key);
    CHECK(DriverStateActivate(bad)!=0 && !DriverStateReady() && !commits);
    CHECK(DriverStateActivate(good)==0 && DriverStateReady() && commits==1);
    CHECK(DriverStateConsumeBoardSeed(&first)==0 && first);
    CHECK(DriverStateConsumeBoardSeed(&first)==0 && !first);
    // Reads which fail after initialization must close their whole safety group,
    // without a factory fallback or treating a pending marker as absent.
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(DriverStateActivate(good)==0);
    CHECK(DriverStateGroupEnabled(AMDGPU_STATE_GROUP_CPU));readDenied=L"CpuPending";
    CHECK(DriverStateNumber(L"",L"CpuPending",FALSE,&number)==STATUS_ACCESS_DENIED && !number);
    readDenied=NULL;CHECK(!DriverStateGroupEnabled(AMDGPU_STATE_GROUP_CPU));
    CHECK(DriverStateNumber(L"",L"CpuTune",TRUE,&number)==STATUS_DEVICE_CONFIGURATION_ERROR);
    readDenied=L"CuModePending";CHECK(DriverStateNumber(L"",L"CuModePending",FALSE,&number)==STATUS_ACCESS_DENIED);
    readDenied=L"DpmPending";CHECK(DriverStateNumber(L"",L"DpmPending",FALSE,&number)==STATUS_ACCESS_DENIED);readDenied=NULL;
    CHECK(!DriverStateGroupEnabled(AMDGPU_STATE_GROUP_CU) && !DriverStateGroupEnabled(AMDGPU_STATE_GROUP_DPM));
    // Execute the real board worker. Failure after the durable consumption mark
    // must never authorize another seed, including after a driver reload.
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(DriverStateActivate(good)==0);
    seedResult=STATUS_ACCESS_DENIED;CHECK(DriverStateSeedBoard(&device)==STATUS_ACCESS_DENIED && seedCalls==1);
    seedResult=0;CHECK(DriverStateSeedBoard(&device)==0 && seedCalls==1 && threadCalls==2);
    DriverStateClose();CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(DriverStateActivate(good)==0);
    CHECK(DriverStateSeedBoard(&device)==0 && seedCalls==1);
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(DriverStateActivate(good)==0);
    seedFlushFail=1;CHECK(DriverStateSeedBoard(&device)!=0 && !seedCalls);seedFlushFail=0;
    CHECK(DriverStateSeedBoard(&device)==0 && !seedCalls);
    DriverStateClose();CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !DriverStateReady());
    CHECK(DriverStateActivate(good)==0 && commits==1);CHECK(DriverStateConsumeBoardSeed(&first)==0 && !first);
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);CHECK(DriverStateActivate(good)==0);
    seedFlushFail=1;CHECK(DriverStateConsumeBoardSeed(&first)!=0 && !first);seedFlushFail=0;
    CHECK(DriverStateConsumeBoardSeed(&first)==0 && !first);
    // A damaged safety record in an already-active generation disables its group.
    data=TestKey(L"S\\StateV2\\Generations\\01000000000000000000000000000000\\Data",FALSE);CHECK(data!=0);
    TestString(data,L"CpuPending",L"x");TestString(data,L"CuModePending",L"x");TestString(data,L"DpmPending",L"x");
    DriverStateClose();HandoffBytes=0;CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0);
    CHECK(!DriverStateGroupEnabled(AMDGPU_STATE_GROUP_CPU) && !DriverStateGroupEnabled(AMDGPU_STATE_GROUP_CU) && !DriverStateGroupEnabled(AMDGPU_STATE_GROUP_DPM));
    CHECK(DriverStateNumber(L"",L"CpuTune",TRUE,&number)!=0 && !number);
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);TestString(3,L"EnableFullWddm",L"x");
    CHECK(DriverStateInit((PDRIVER_OBJECT)1)!=0 && !commits && !DriverStateReady());
    // CURRENT import requires the same frozen source again at actual activation.
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_FRESH);h.transaction[0]=2;
    strcpy_s(h.destination_version,32,"0.7.216.101");
    {unsigned size=0;CHECK(!AmdgpuTransferCapture(&h,TestEmpty,NULL,Handoff,sizeof(Handoff),&size));HandoffBytes=size;}
    TestPublish(Handoff,HandoffBytes);root=TestKey(L"S\\StateV2",FALSE);
    TestNumber(root,L"Frozen",1);TestString(root,L"FrozenBy",L"01000000000000000000000000000000");
    h=TestHandoff(AMDGPU_TRANSFER_CURRENT);CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !commits);
    TestNumber(root,L"Frozen",0);CHECK(DriverStateActivate(good)!=0 && !commits);
    TestNumber(root,L"Frozen",1);CHECK(DriverStateActivate(good)==0 && commits==1);
    CHECK(DriverStateConsumeBoardSeed(&first)==0 && !first);
    CHECK(DriverStateSeedBoard(&device)==0 && !seedCalls && !threadCalls);
    // Archive restore has no live source to merge and cannot seed board state.
    TestReset();h=TestHandoff(AMDGPU_TRANSFER_ARCHIVE);CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !commits);
    CHECK(DriverStateActivate(good)==0 && commits==1);CHECK(DriverStateConsumeBoardSeed(&first)==0 && !first);
    CHECK(DriverStateSeedBoard(&device)==0 && !seedCalls && !threadCalls);
    TestReset();CHECK(DriverStateInit((PDRIVER_OBJECT)1)==0 && !commits);CHECK(DriverStateActivate(zero)==0 && commits==1);
    TestReset();(void)h;printf("%u kernel bootstrap checks, %u failures\n",checks,failures);return failures?1:0;
}
