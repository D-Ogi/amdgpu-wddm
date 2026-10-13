#include <ntifs.h>
#include "state_handoff.h"
#include "../state/transfer.h"
#define HANDOFF_TAG 'hSMA'
static const ULONG SystemSid[]={0x00000101u,0x05000000u,18u};
static const ULONG AdminSid[]={0x00000201u,0x05000000u,32u,544u};
static NTSTATUS Protected(HANDLE file)
{
    union {ULONG alignment;UCHAR bytes[512];} storage;
    PSECURITY_DESCRIPTOR sd=(PSECURITY_DESCRIPTOR)storage.bytes;ULONG needed=0,i;
    PSID owner;PACL acl;BOOLEAN defaulted,present;NTSTATUS status;
    status=ZwQuerySecurityObject(file,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
                                sd,sizeof(storage.bytes),&needed);
    if(!NT_SUCCESS(status))return status;
    if(needed>sizeof(storage.bytes) || !RtlValidSecurityDescriptor(sd) ||
       !(((SECURITY_DESCRIPTOR*)sd)->Control&SE_DACL_PROTECTED))return STATUS_ACCESS_DENIED;
    status=RtlGetOwnerSecurityDescriptor(sd,&owner,&defaulted);
    if(!NT_SUCCESS(status) || !owner || !RtlEqualSid(owner,(PSID)SystemSid))return STATUS_ACCESS_DENIED;
    status=RtlGetDaclSecurityDescriptor(sd,&present,&acl,&defaulted);
    if(!NT_SUCCESS(status) || !present || !acl || acl->AceCount!=2)return STATUS_ACCESS_DENIED;
    for(i=0;i<2;i++) {
        ACCESS_ALLOWED_ACE *ace;PSID sid=(PSID)(i?AdminSid:SystemSid);
        ULONG rights=i?FILE_GENERIC_READ:FILE_ALL_ACCESS;
        status=RtlGetAce(acl,i,(PVOID*)&ace);
        if(!NT_SUCCESS(status) || ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags ||
           ace->Mask!=rights || ace->Header.AceSize!=FIELD_OFFSET(ACCESS_ALLOWED_ACE,SidStart)+RtlLengthSid(sid) ||
           !RtlEqualSid(&ace->SidStart,sid))return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Open(HANDLE parent,PCWSTR leaf,BOOLEAN directory,PHANDLE out)
{
    OBJECT_ATTRIBUTES oa;UNICODE_STRING name;IO_STATUS_BLOCK io;
    FILE_ATTRIBUTE_TAG_INFORMATION tag;NTSTATUS status;
    *out=NULL;RtlInitUnicodeString(&name,leaf);
    InitializeObjectAttributes(&oa,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE|
                               (parent?OBJ_DONT_REPARSE:0),parent,NULL);
    status=ZwCreateFile(out,FILE_READ_DATA|FILE_READ_ATTRIBUTES|READ_CONTROL|SYNCHRONIZE,&oa,&io,NULL,
                        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,FILE_OPEN,
                        FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_REPARSE_POINT|
                        (directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE),NULL,0);
    if(!NT_SUCCESS(status))return status;
    status=ZwQueryInformationFile(*out,&io,&tag,sizeof(tag),FileAttributeTagInformation);
    if(NT_SUCCESS(status) && ((tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
       !!(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=!!directory))status=STATUS_REPARSE_POINT_ENCOUNTERED;
    if(NT_SUCCESS(status) && parent)status=Protected(*out);
    if(!NT_SUCCESS(status)){ZwClose(*out);*out=NULL;}return status;
}
NTSTATUS DriverStateReadHandoff(PVOID *buffer,ULONG *bytes)
{
    HANDLE handles[4]={0};ULONG i;NTSTATUS status;IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION size={0};LARGE_INTEGER offset={0};PVOID data=NULL;
    static const PCWSTR names[]={L"\\DriverData",L"amdgpu-wddm",L"StateV2",L"pending.bin"};
    if(!buffer || !bytes)return STATUS_INVALID_PARAMETER;*buffer=NULL;*bytes=0;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !PsIsSystemThread(PsGetCurrentThread()))return STATUS_INVALID_DEVICE_STATE;
    // Retained ancestor handles deny replacement/rename throughout the read.
    for(i=0;i<4;i++) {
        status=Open(i?handles[i-1]:NULL,names[i],i<3,&handles[i]);
        if(!NT_SUCCESS(status))goto done;
    }
    status=ZwQueryInformationFile(handles[3],&io,&size,sizeof(size),FileStandardInformation);
    if(!NT_SUCCESS(status))goto done;
    if(size.EndOfFile.QuadPart<sizeof(AMDGPU_TRANSFER_HEADER) || size.EndOfFile.QuadPart>AMDGPU_TRANSFER_MAX_BYTES) {
        status=STATUS_INFO_LENGTH_MISMATCH;goto done;
    }
    data=ExAllocatePool2(POOL_FLAG_PAGED,(SIZE_T)size.EndOfFile.QuadPart,HANDOFF_TAG);
    if(!data){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    status=ZwReadFile(handles[3],NULL,NULL,NULL,&io,data,(ULONG)size.EndOfFile.QuadPart,&offset,NULL);
    if(NT_SUCCESS(status) && io.Information!=(ULONG)size.EndOfFile.QuadPart)status=STATUS_END_OF_FILE;
    if(NT_SUCCESS(status) && AmdgpuTransferCheck(data,(ULONG)size.EndOfFile.QuadPart,NULL)!=AMDGPU_STATE_OK)
        status=STATUS_DATA_ERROR;
done:
    for(i=4;i;i--)if(handles[i-1]) {
        NTSTATUS closed=ZwClose(handles[i-1]);if(NT_SUCCESS(status) && !NT_SUCCESS(closed))status=closed;
    }
    if(NT_SUCCESS(status)){*buffer=data;*bytes=(ULONG)size.EndOfFile.QuadPart;}
    else if(data)ExFreePoolWithTag(data,HANDOFF_TAG);
    return status;
}
void DriverStateFreeHandoff(PVOID buffer)
{if(buffer)ExFreePoolWithTag(buffer,HANDOFF_TAG);}
