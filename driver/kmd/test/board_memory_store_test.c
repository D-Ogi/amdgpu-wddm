// Production persistence code against a fault-injected filesystem. The Windows
// security-descriptor API is real; all file bytes, handles and flushes are fake.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bc250_uma.h"
#include "board_memory_service.h"
#include "board_memory_format.h"

typedef LONG NTSTATUS;
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS 0
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034L)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xc0000035L)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001L)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022L)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003eL)
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)0xc0000206L)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xc0000004L)
#define STATUS_END_OF_FILE ((NTSTATUS)0xc0000011L)
#define STATUS_DISK_FULL ((NTSTATUS)0xc000007fL)
#define STATUS_REPARSE_POINT_ENCOUNTERED ((NTSTATUS)0xc000050bL)
#define PASSIVE_LEVEL 0
#define OBJ_KERNEL_HANDLE 0x200
#define OBJ_CASE_INSENSITIVE 0x40
#define OBJ_DONT_REPARSE 0x1000
#define FILE_DIRECTORY_FILE 1
#define FILE_WRITE_THROUGH 2
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40
#define FILE_OPEN_REPARSE_POINT 0x200000
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FileStandardInformation 5
#define FileAttributeTagInformation 35
typedef struct { ULONG_PTR Information; } IO_STATUS_BLOCK;
typedef struct { LARGE_INTEGER EndOfFile; } FILE_STANDARD_INFORMATION;
typedef struct { ULONG FileAttributes; } FILE_ATTRIBUTE_TAG_INFORMATION;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;
static void RtlInitUnicodeString(UNICODE_STRING* s, PCWSTR p) { s->Buffer=(PWSTR)p;s->Length=(USHORT)(wcslen(p)*2);s->MaximumLength=s->Length+2; }
typedef struct { ULONG Attributes; UNICODE_STRING* Name; PSECURITY_DESCRIPTOR Security; HANDLE Root; } OBJECT_ATTRIBUTES;
#define InitializeObjectAttributes(a,n,f,r,s) do { (a)->Attributes=(f); (a)->Name=(n); (a)->Security=(s); (a)->Root=(r); } while(0)
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
static unsigned checks, failures, stage, fail_stage, writes, flushes, irql, short_read, short_write, corrupt_readback, system_thread=1;
struct object { unsigned exists, attributes, bytes, durable_bytes, handles, opens, writes; UCHAR data[88], durable[88], security[512]; DWORD security_bytes; };
static struct object objects[6]; // 1 DriverData, 2 company, 3 board, 4 original, 5 journal.
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL CHECK line %u: %s\n",__LINE__,#x);} } while(0)
static int failed(void){return ++stage==fail_stage;}
static UCHAR KeGetCurrentIrql(void){return (UCHAR)irql;}
static PVOID PsGetCurrentThread(void){return (PVOID)1;}
static BOOLEAN PsIsSystemThread(PVOID t){CHECK(t==(PVOID)1);return (BOOLEAN)system_thread;}
static struct object* object(HANDLE h) { uintptr_t i=(uintptr_t)h;CHECK(i>0 && i<6 && objects[i].handles);return &objects[i]; }
static NTSTATUS ZwCreateFile(HANDLE* handle,ULONG access,OBJECT_ATTRIBUTES* a,IO_STATUS_BLOCK* io,
    void* size,ULONG attr,ULONG share,ULONG how,ULONG flags,void* ea,ULONG eaSize)
{
    uintptr_t parent=(uintptr_t)a->Root; unsigned index=0; struct object* o;
    UNREFERENCED_PARAMETER(size);UNREFERENCED_PARAMETER(ea);UNREFERENCED_PARAMETER(eaSize);
    if(parent==0 && !wcscmp(a->Name->Buffer,L"\\DriverData"))index=1;
    if(parent==1 && !wcscmp(a->Name->Buffer,L"amdgpu-wddm"))index=2;
    if(parent==2 && !wcscmp(a->Name->Buffer,L"BoardMemory"))index=3;
    if(parent==3 && !wcscmp(a->Name->Buffer,L"original.dat"))index=4;
    if(parent==3 && !wcscmp(a->Name->Buffer,L"journal.dat"))index=5;
    CHECK(index>0);if(!index)return STATUS_ACCESS_DENIED;
    CHECK(a->Attributes==(ULONG)(OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE|(parent?OBJ_DONT_REPARSE:0)));
    CHECK((access & (FILE_READ_ATTRIBUTES|READ_CONTROL|SYNCHRONIZE))==(FILE_READ_ATTRIBUTES|READ_CONTROL|SYNCHRONIZE));
    CHECK(share==FILE_SHARE_READ && (flags & (FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_REPARSE_POINT))==(FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_REPARSE_POINT));
    CHECK((flags & (index<=3?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE))!=0);
    if(parent)CHECK(objects[parent].handles>0);
    if(failed())return STATUS_ACCESS_DENIED;
    o=&objects[index];
    if(how==FILE_CREATE && o->exists)return STATUS_OBJECT_NAME_COLLISION;
    if(!o->exists){
        if(how==FILE_OPEN)return STATUS_OBJECT_NAME_NOT_FOUND;
        CHECK(a->Security!=NULL);o->security_bytes=sizeof(o->security);
        CHECK(MakeSelfRelativeSD(a->Security,(PSECURITY_DESCRIPTOR)o->security,&o->security_bytes));
        o->exists=1;o->attributes=attr;io->Information=2;
    }else io->Information=1;
    o->handles++;o->opens++;*handle=(HANDLE)(uintptr_t)index;return STATUS_SUCCESS;
}
static NTSTATUS ZwQuerySecurityObject(HANDLE h,ULONG info,PSECURITY_DESCRIPTOR s,ULONG bytes,ULONG* needed)
{
    struct object* o=object(h);CHECK(info==(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION));
    if(failed() || o->security_bytes>bytes)return STATUS_ACCESS_DENIED;
    memcpy(s,o->security,o->security_bytes);*needed=o->security_bytes;return STATUS_SUCCESS;
}
static NTSTATUS ZwQueryInformationFile(HANDLE h,IO_STATUS_BLOCK* io,void* data,ULONG bytes,int kind)
{
    struct object* o=object(h);UNREFERENCED_PARAMETER(io);
    if(failed())return STATUS_UNSUCCESSFUL;
    if(kind==FileAttributeTagInformation){CHECK(bytes==sizeof(FILE_ATTRIBUTE_TAG_INFORMATION));((FILE_ATTRIBUTE_TAG_INFORMATION*)data)->FileAttributes=o->attributes;}
    else{CHECK(kind==FileStandardInformation && bytes==sizeof(FILE_STANDARD_INFORMATION));((FILE_STANDARD_INFORMATION*)data)->EndOfFile.QuadPart=o->bytes;}
    return STATUS_SUCCESS;
}
static NTSTATUS ZwReadFile(HANDLE h,void* event,void* apc,void* ctx,IO_STATUS_BLOCK* io,void* data,ULONG bytes,LARGE_INTEGER* offset,void* key)
{
    struct object* o=object(h);UNREFERENCED_PARAMETER(event);UNREFERENCED_PARAMETER(apc);UNREFERENCED_PARAMETER(ctx);UNREFERENCED_PARAMETER(key);
    CHECK(offset && !offset->QuadPart && bytes<=o->bytes);if(failed())return STATUS_UNSUCCESSFUL;
    io->Information=short_read?bytes-1:bytes;memcpy(data,o->data,(SIZE_T)io->Information);
    if(corrupt_readback && writes)((UCHAR*)data)[0]^=1;
    return STATUS_SUCCESS;
}
static NTSTATUS ZwWriteFile(HANDLE h,void* event,void* apc,void* ctx,IO_STATUS_BLOCK* io,void* data,ULONG bytes,LARGE_INTEGER* offset,void* key)
{
    struct object* o=object(h);UNREFERENCED_PARAMETER(event);UNREFERENCED_PARAMETER(apc);UNREFERENCED_PARAMETER(ctx);UNREFERENCED_PARAMETER(key);
    CHECK(offset && !offset->QuadPart && bytes<=sizeof(o->data));writes++;o->writes++;
    if(failed())return STATUS_DISK_FULL;
    io->Information=short_write?bytes-1:bytes;memcpy(o->data,data,(SIZE_T)io->Information);o->bytes=(unsigned)io->Information;
    return STATUS_SUCCESS;
}
static NTSTATUS ZwFlushBuffersFile(HANDLE h,IO_STATUS_BLOCK* io)
{
    struct object* o=object(h);UNREFERENCED_PARAMETER(io);flushes++;
    if(failed())return STATUS_UNSUCCESSFUL;
    memcpy(o->durable,o->data,o->bytes);o->durable_bytes=o->bytes;return STATUS_SUCCESS;
}
static NTSTATUS ZwClose(HANDLE h){struct object* o=object(h);o->handles--;return failed()?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;}
/* ACTUAL_STORE */
static void no_handles(void){unsigned i;for(i=1;i<6;i++)CHECK(objects[i].handles==0);}
static void init(BC250_DEVICE* device,unsigned char* backup)
{
    unsigned i,sum=0;no_handles();
    memset(device,0,sizeof(*device));device->BoardMemoryMapping=3;device->BoardMemoryBiosId=3;device->BoardMemoryMachineId[0]=42;
    memset(backup,0,28);memcpy(backup,"CMSB",4);backup[27]=0x20;
    for(i=6;i<28;++i)sum+=backup[i];backup[4]=(UCHAR)sum;backup[5]=(UCHAR)(sum>>8);
    stage=fail_stage=writes=flushes=irql=short_read=short_write=corrupt_readback=0;system_thread=1;
    memset(objects,0,sizeof(objects));objects[1].exists=1;objects[1].attributes=FILE_ATTRIBUTE_DIRECTORY;
}
static void seed(BC250_DEVICE* device){CHECK(BoardMemoryStoreSeed(device,NULL,TRUE)==0);no_handles();}
static void crash(void){unsigned i;no_handles();for(i=4;i<6;i++){memcpy(objects[i].data,objects[i].durable,objects[i].durable_bytes);objects[i].bytes=objects[i].durable_bytes;}}
int main(void)
{
    BC250_DEVICE device;struct board_memory_store store;BOARD_STORE_RECORD original;
    unsigned char backup[28],out[28];unsigned pending,i,max_stage;
    init(&device,backup);BoardMemoryStoreIo(&device,&store);
    CHECK(store.load(store.context,out,&pending)==-1 && writes==0); // Missing provenance is not fresh.
    CHECK(BoardMemoryStoreSeed(&device,NULL,FALSE)==STATUS_DATA_ERROR && writes==0);
    system_thread=0;CHECK(BoardMemoryStoreSeed(&device,NULL,TRUE)==STATUS_INVALID_DEVICE_STATE && writes==0);system_thread=1;
    seed(&device);CHECK(store.load(store.context,out,&pending)==0);
    CHECK(store.save(store.context,backup,0)==1);CHECK(objects[4].writes==1);
    memcpy(&original,objects[4].data,sizeof(original));
    CHECK(store.save(store.context,backup,1)==1);crash();
    CHECK(store.load(store.context,out,&pending)==1 && pending==1 && !memcmp(backup,out,28));
    CHECK(store.save(store.context,backup,0)==1);crash();
    CHECK(store.load(store.context,out,&pending)==1 && pending==0 && objects[4].writes==1);
    CHECK(!memcmp(&original,objects[4].data,sizeof(original)));
    backup[27]=0x30;backup[4]=0x30;CHECK(store.save(store.context,backup,0)==0);backup[27]=0x20;backup[4]=0x20;
    device.BoardMemoryMachineId[0]^=1;CHECK(store.load(store.context,out,&pending)==-1);CHECK(store.save(store.context,backup,0)==0);device.BoardMemoryMachineId[0]^=1;
    objects[5].data[0]^=1;CHECK(store.load(store.context,out,&pending)==-1);CHECK(store.save(store.context,backup,0)==0);
    CHECK(BoardMemoryStoreSeed(&device,NULL,TRUE)!=0); // Cannot re-seed a corrupt latest journal.
    CHECK(!memcmp(&original,objects[4].data,sizeof(original)));objects[5].data[0]^=1;
    objects[4].exists=0;CHECK(store.load(store.context,out,&pending)==-1);CHECK(BoardMemoryStoreSeed(&device,NULL,TRUE)!=0);objects[4].exists=1;
    for(i=1;i<6;i++){
        objects[i].attributes|=FILE_ATTRIBUTE_REPARSE_POINT;
        CHECK(store.load(store.context,out,&pending)==-1);CHECK(store.save(store.context,backup,1)==0);
        objects[i].attributes&=~FILE_ATTRIBUTE_REPARSE_POINT;
    }
    for(i=2;i<6;i++){
        PACL acl;BOOL present,def;ACCESS_ALLOWED_ACE* ace;DWORD saved;
        CHECK(GetSecurityDescriptorDacl(objects[i].security,&present,&acl,&def));CHECK(GetAce(acl,1,(void**)&ace));
        saved=ace->Mask;ace->Mask=FILE_ALL_ACCESS;CHECK(store.load(store.context,out,&pending)==-1);
        CHECK(store.save(store.context,backup,0)==0);CHECK(BoardMemoryStoreSeed(&device,NULL,TRUE)!=0);CHECK(ace->Mask==FILE_ALL_ACCESS);ace->Mask=saved;
    }
    short_read=1;CHECK(store.load(store.context,out,&pending)==-1);short_read=0;
    ((BOARD_STORE_JOURNAL*)objects[5].data)->Sequence=UINT64_MAX;
    ((BOARD_STORE_JOURNAL*)objects[5].data)->Checksum=JournalChecksum((BOARD_STORE_JOURNAL*)objects[5].data);
    CHECK(store.save(store.context,backup,1)==0);
    init(&device,backup);original.Pending=1;original.Checksum=StoreChecksum(&original);
    CHECK(BoardMemoryStoreSeed(&device,&original,FALSE)==0);crash();
    CHECK(!memcmp(&original,objects[4].data,sizeof(original)));
    CHECK(store.load(store.context,out,&pending)==1 && pending==1);
    CHECK(BoardMemoryStoreSeed(&device,&original,FALSE)==0);
    CHECK(store.save(store.context,backup,0)==1 && objects[4].writes==1);
    original.Backup[27]=0x30;original.Backup[4]=0x30;original.Checksum=StoreChecksum(&original);
    CHECK(BoardMemoryStoreSeed(&device,&original,FALSE)!=0);
    init(&device,backup);seed(&device);stage=0;CHECK(store.save(store.context,backup,1)==1);max_stage=stage;no_handles();
    for(i=1;i<=max_stage;i++){
        init(&device,backup);seed(&device);stage=0;fail_stage=i;
        CHECK(store.save(store.context,backup,1)==0);no_handles();fail_stage=0;crash();
        // Failure before an original can preserve the seeded empty state.
        // An incomplete pair is refused. A committed pending stays set.
        { int found=store.load(store.context,out,&pending);CHECK(found!=1 || pending==1); }
    }
    // First import, interrupted anywhere: no invalid half-import enables writes.
    init(&device,backup);original.Pending=1;original.Backup[27]=0x20;original.Backup[4]=0x20;
    original.Checksum=StoreChecksum(&original);
    CHECK(BoardMemoryStoreSeed(&device,&original,FALSE)==0);max_stage=stage;no_handles();
    for(i=1;i<=max_stage;i++){
        init(&device,backup);fail_stage=i;
        CHECK(BoardMemoryStoreSeed(&device,&original,FALSE)!=0);no_handles();fail_stage=0;crash();
        { int found=store.load(store.context,out,&pending);CHECK(found==-1 || (found==1 && pending==1)); }
    }
    // Journal updates from an existing original never rewrite the original, even
    // on a short write or a failed flush. A failed pending save cannot authorize I/O.
    init(&device,backup);seed(&device);CHECK(store.save(store.context,backup,0)==1);
    stage=0;CHECK(store.save(store.context,backup,1)==1);max_stage=stage;no_handles();
    for(i=1;i<=max_stage;i++){
        init(&device,backup);seed(&device);CHECK(store.save(store.context,backup,0)==1);
        memcpy(&original,objects[4].data,sizeof(original));stage=0;fail_stage=i;
        CHECK(store.save(store.context,backup,1)==0);no_handles();fail_stage=0;crash();
        CHECK(objects[4].writes==1 && !memcmp(&original,objects[4].data,sizeof(original)));
    }
    init(&device,backup);seed(&device);short_write=1;CHECK(store.save(store.context,backup,1)==0);short_write=0;
    CHECK(store.load(store.context,out,&pending)==-1);
    init(&device,backup);seed(&device);corrupt_readback=1;CHECK(store.save(store.context,backup,1)==0);corrupt_readback=0;
    init(&device,backup);seed(&device);irql=2;CHECK(store.load(store.context,out,&pending)==-1 && writes==1);CHECK(store.save(store.context,backup,1)==0 && writes==1);no_handles();
    printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
