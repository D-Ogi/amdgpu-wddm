// Actual store implementation, mocked registry only. No registry or hardware I/O.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "bc250_uma.h"
#include "board_memory_service.h"
typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034L)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xc000003aL)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001L)
#define PASSIVE_LEVEL 0
#define OBJ_KERNEL_HANDLE 0x200
#define OBJ_CASE_INSENSITIVE 0x40
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;
#define RTL_CONSTANT_STRING(s) {sizeof(s)-sizeof(WCHAR),sizeof(s),(PWSTR)s}
typedef struct { ULONG Attributes; UNICODE_STRING* Name; PSECURITY_DESCRIPTOR Security; } OBJECT_ATTRIBUTES;
#define InitializeObjectAttributes(a,n,f,r,s) do { (a)->Attributes=(f); (a)->Name=(n); (a)->Security=(s); } while(0)
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;
#define KeyValuePartialInformation 2
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))
#define RtlCompareMemory MockCompareMemory
static SIZE_T MockCompareMemory(const void* a,const void* b,SIZE_T n) { return memcmp(a,b,n)?0:n; }
#define RtlEqualSid EqualSid
#define RtlLengthSid GetLengthSid
#define RtlValidSecurityDescriptor IsValidSecurityDescriptor
#define RtlCreateSecurityDescriptor(s,r) (InitializeSecurityDescriptor(s,r)?0:STATUS_UNSUCCESSFUL)
#define RtlCreateAcl(a,n,r) (InitializeAcl(a,n,r)?0:STATUS_UNSUCCESSFUL)
#define RtlAddAccessAllowedAce(a,r,m,s) (AddAccessAllowedAce(a,r,m,s)?0:STATUS_UNSUCCESSFUL)
#define RtlSetDaclSecurityDescriptor(s,p,a,d) (SetSecurityDescriptorDacl(s,p,a,d)?0:STATUS_UNSUCCESSFUL)
#define RtlSetOwnerSecurityDescriptor(s,o,d) (SetSecurityDescriptorOwner(s,o,d)?0:STATUS_UNSUCCESSFUL)
static NTSTATUS RtlGetOwnerSecurityDescriptor(PSECURITY_DESCRIPTOR s,PSID* o,BOOLEAN* d)
{ BOOL value; if(!GetSecurityDescriptorOwner(s,o,&value))return STATUS_UNSUCCESSFUL;*d=(BOOLEAN)value;return 0; }
static NTSTATUS RtlGetDaclSecurityDescriptor(PSECURITY_DESCRIPTOR s,BOOLEAN* p,PACL* a,BOOLEAN* d)
{ BOOL present,def; if(!GetSecurityDescriptorDacl(s,&present,a,&def))return STATUS_UNSUCCESSFUL;*p=(BOOLEAN)present;*d=(BOOLEAN)def;return 0; }
#define RtlGetAce(a,i,v) (GetAce(a,i,v)?0:STATUS_UNSUCCESSFUL)
typedef struct { UCHAR BoardMemoryMachineId[16]; ULONG BoardMemoryMapping,BoardMemoryBiosId; } BC250_DEVICE;
static unsigned checks, failures, stage, fail_stage, creates, sets, flushes, closes, exists, has_value;
static unsigned irql, corrupt_readback;
static UCHAR data[256], security_data[512];
static ULONG data_bytes, data_type, security_bytes;
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL CHECK line %u: %s\n",__LINE__,#x);} } while(0)
static int failed(void){return ++stage==fail_stage;}
static UCHAR KeGetCurrentIrql(void){return (UCHAR)irql;}
static NTSTATUS ZwOpenKey(HANDLE* key,ULONG access,OBJECT_ATTRIBUTES* a)
{
    CHECK(access==(KEY_QUERY_VALUE|READ_CONTROL));
    CHECK(a->Attributes==(OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE));
    if(failed())return STATUS_UNSUCCESSFUL;
    if(!exists)return STATUS_OBJECT_NAME_NOT_FOUND;
    *key=(HANDLE)1;return 0;
}
static NTSTATUS ZwCreateKey(HANDLE* key,ULONG access,OBJECT_ATTRIBUTES* a,ULONG title,void* cls,ULONG options,ULONG* disposition)
{
    UNREFERENCED_PARAMETER(access);UNREFERENCED_PARAMETER(title);UNREFERENCED_PARAMETER(cls);
    CHECK(a->Security!=NULL && options==REG_OPTION_NON_VOLATILE);
    CHECK(wcscmp(a->Name->Buffer,L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\amdgpu-wddm-board-memory")==0);
    ++creates;if(failed())return STATUS_UNSUCCESSFUL;
    *disposition=exists?REG_OPENED_EXISTING_KEY:REG_CREATED_NEW_KEY;exists=1;*key=(HANDLE)1;return 0;
}
static NTSTATUS ZwSetSecurityObject(HANDLE key,ULONG info,PSECURITY_DESCRIPTOR s)
{
    DWORD length=sizeof(security_data);UNREFERENCED_PARAMETER(key);
    CHECK(info==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION));
    if(failed())return STATUS_UNSUCCESSFUL;
    if(!MakeSelfRelativeSD(s,(PSECURITY_DESCRIPTOR)security_data,&length))return STATUS_UNSUCCESSFUL;
    security_bytes=length;return 0;
}
static NTSTATUS ZwQuerySecurityObject(HANDLE key,ULONG info,PSECURITY_DESCRIPTOR s,ULONG bytes,ULONG* needed)
{
    UNREFERENCED_PARAMETER(key);CHECK(info==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION));
    if(failed() || security_bytes>bytes)return STATUS_UNSUCCESSFUL;
    memcpy(s,security_data,security_bytes);*needed=security_bytes;return 0;
}
static NTSTATUS ZwQueryValueKey(HANDLE key,UNICODE_STRING* name,int cls,void* out,ULONG bytes,ULONG* needed)
{
    PKEY_VALUE_PARTIAL_INFORMATION v=out;UNREFERENCED_PARAMETER(key);
    CHECK(wcscmp(name->Buffer,L"Backup")==0 && cls==KeyValuePartialInformation);
    if(failed())return STATUS_UNSUCCESSFUL;
    if(!has_value)return STATUS_OBJECT_NAME_NOT_FOUND;
    *needed=FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+data_bytes;
    if(*needed>bytes)return STATUS_UNSUCCESSFUL;
    v->Type=data_type;v->DataLength=data_bytes;memcpy(v->Data,data,data_bytes);
    if(corrupt_readback && flushes)v->Data[0]^=1;
    return 0;
}
static NTSTATUS ZwSetValueKey(HANDLE key,UNICODE_STRING* name,ULONG title,ULONG type,void* value,ULONG bytes)
{
    UNREFERENCED_PARAMETER(key);UNREFERENCED_PARAMETER(name);UNREFERENCED_PARAMETER(title);
    ++sets;if(failed() || bytes>sizeof(data))return STATUS_UNSUCCESSFUL;
    memcpy(data,value,bytes);data_bytes=bytes;data_type=type;has_value=1;return 0;
}
static NTSTATUS ZwFlushKey(HANDLE key){UNREFERENCED_PARAMETER(key);++flushes;return failed()?STATUS_UNSUCCESSFUL:0;}
static NTSTATUS ZwClose(HANDLE key){UNREFERENCED_PARAMETER(key);++closes;return failed()?STATUS_UNSUCCESSFUL:0;}
/* ACTUAL_STORE */
static void init(BC250_DEVICE* device,unsigned char* backup)
{
    unsigned i,sum=0;
    memset(device,0,sizeof(*device));device->BoardMemoryMapping=1;device->BoardMemoryBiosId=3;device->BoardMemoryMachineId[0]=42;
    memset(backup,0,28);memcpy(backup,"CMSB",4);backup[27]=0x20;
    for(i=6;i<28;++i)sum+=backup[i];backup[4]=(UCHAR)sum;backup[5]=(UCHAR)(sum>>8);
    stage=fail_stage=creates=sets=flushes=closes=exists=has_value=irql=corrupt_readback=0;
    data_bytes=security_bytes=0;
}
int main(void)
{
    BC250_DEVICE device;
    struct board_memory_store store;
    unsigned char backup[28],out[28],saved[256];
    unsigned pending,i,max_stage;
    init(&device,backup);BoardMemoryStoreIo(&device,&store);
    CHECK(store.load(store.context,out,&pending)==0 && creates==0 && sets==0);
    CHECK(store.save(store.context,backup,1)==1 && flushes==1);
    CHECK(store.load(store.context,out,&pending)==1 && pending==1 && !memcmp(backup,out,28));
    CHECK(store.save(store.context,backup,0)==1);
    CHECK(store.load(store.context,out,&pending)==1 && pending==0);
    memcpy(saved,data,data_bytes);
    backup[27]=0x30;backup[4]=0x30;
    CHECK(store.save(store.context,backup,0)==0 && !memcmp(saved,data,data_bytes));
    backup[27]=0x20;backup[4]=0x20;
    device.BoardMemoryMachineId[0]^=1;
    CHECK(store.load(store.context,out,&pending)==-1);
    CHECK(store.save(store.context,backup,0)==0 && !memcmp(saved,data,data_bytes));
    device.BoardMemoryMachineId[0]^=1;
    data[0]^=1;CHECK(store.load(store.context,out,&pending)==-1);
    CHECK(store.save(store.context,backup,0)==0);data[0]^=1;
    ((SECURITY_DESCRIPTOR*)security_data)->Control &= ~SE_DACL_PROTECTED;
    CHECK(store.load(store.context,out,&pending)==-1 && creates==5);
    init(&device,backup);CHECK(store.save(store.context,backup,1)==1);
    device.BoardMemoryBiosId ^= 1;CHECK(store.load(store.context,out,&pending)==-1);device.BoardMemoryBiosId ^= 1;
    device.BoardMemoryMapping = 2;CHECK(store.load(store.context,out,&pending)==-1);device.BoardMemoryMapping = 1;
    data_type=REG_SZ;CHECK(store.load(store.context,out,&pending)==-1);data_type=REG_BINARY;
    --data_bytes;CHECK(store.load(store.context,out,&pending)==-1);++data_bytes;
    data[40]^=1;CHECK(store.load(store.context,out,&pending)==-1);data[40]^=1;
    {
        BOARD_STORE_RECORD* record=(BOARD_STORE_RECORD*)data;
        record->Pending=2;record->Checksum=StoreChecksum(record);
        CHECK(store.load(store.context,out,&pending)==-1);
        record->Pending=1;record->Reserved[0]=1;record->Checksum=StoreChecksum(record);
        CHECK(store.load(store.context,out,&pending)==-1);
    }
    init(&device,backup);CHECK(store.save(store.context,backup,1)==1);
    {
        PACL acl;BOOL present,def;ACCESS_ALLOWED_ACE* ace;
        CHECK(GetSecurityDescriptorDacl(security_data,&present,&acl,&def));
        CHECK(GetAce(acl,1,(void**)&ace));ace->Mask=KEY_ALL_ACCESS;
        CHECK(store.load(store.context,out,&pending)==-1);
    }
    init(&device,backup);CHECK(store.save(store.context,backup,1)==1);max_stage=stage;
    for(i=1;i<=max_stage;++i){init(&device,backup);fail_stage=i;CHECK(store.save(store.context,backup,1)==0);}
    init(&device,backup);corrupt_readback=1;CHECK(store.save(store.context,backup,1)==0);
    init(&device,backup);device.BoardMemoryMapping=0;
    CHECK(store.load(store.context,out,&pending)==-1 && stage==0);
    CHECK(store.save(store.context,backup,1)==0 && stage==0);
    init(&device,backup);irql=2;CHECK(store.save(store.context,backup,1)==0 && stage==0);
    printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
